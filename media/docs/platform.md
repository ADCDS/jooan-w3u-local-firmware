# W3-U platform backend

`src/platform.c` contains original MIT-licensed userspace code. The `imp`
backend uses the OEM kernel's already loaded `sample_motor.ko` and GPIO sysfs;
it does not invoke `jooanipc`, CGI, MQTT, ONVIF, a shell, module loading, or boot
configuration. `replay` simulates all of these controls in memory and writes
only its own preset file. Replay status explicitly identifies the backend.

The hardware profile is specifically the JOOAN W3-U / JA-A12 described by the
verified unit's factory configuration (`/etc/config.org`). It is not a generic
Ingenic GPIO map. Other board revisions require a reviewed profile change.

## Motor ABI and movement

The module's `motor_ioctl` symbol is at section `.text+0x10a0`. Its jump table
in `.rodata+0` maps the literal request numbers below; these are not `_IO*`
numbers synthesized for the host architecture.

| Request | Argument | Use |
| --- | --- | --- |
| 1 | none | Stop |
| 2 | four int32: maximum X, maximum Y, current X, current Y | Reset/calibration; boot homing only (`motor_home=1`) |
| 3 | two signed int32: delta X, delta Y | Move; can block when both axes are requested |
| 4 | four int32 returned: X, Y, moving, speed | Position/status |
| 5 | int32 pointer, accepted range 100–900 | Speed |
| 6 | none | Return/back operation; not used |
| 8 | two signed int32: delta X, delta Y | OEM nonblocking move; not used because its Y path corrupts swapped-axis bookkeeping |

Independent evidence comes from static analysis of the OEM application:
`0x48ce10` issues request 3, `0x48d268` issues request 8, `0x48d550` request 5,
`0x48d8b8` request 4, and the reset routine `0x48d9d0` request 2. Module
`motor_get_message` establishes the four returned int32 fields;
`motor_ops_move_not_stop` establishes request 8's behavior.

The verified OEM startup script `jzstart.sh:297–309` loads the driver with
maximum X=8600, maximum Y=2000, and the shared motor-coil switch GPIO17.
OEM homing on this installed profile reports calibrated maxima 4080/1230.
The replacement reads those limits from `/proc/jz/motor/motor_info` when it
opens the motor. Normalized status uses the calibrated limits. Imported
presets remain in raw driver steps and are checked against live limits before
recall; they are never scaled or flipped during migration.

The replacement submits one finite displacement on one axis through request
**3**. This path is asynchronous for a single axis and restores the driver's
axis bookkeeping. Request 8's Y path does not restore that bookkeeping. The
worker checks deadlines every 10 ms, outside the encoder SDK lock. No motor
GPIOs are manipulated directly.

Manual `ptz(x,y)` chooses the dominant axis, maps normalized velocity to the
OEM speed range 100–900, and moves toward its calibrated travel limit. A
1200 ms lease enforces Stop. Repeated identical commands renew the lease
without stopping and restarting the coils. Direction changes cancel the old
job. `ptz_stop` cancels all manual and preset motion.

`onvif_nudge=1` selects compatibility with the previously deployed local
Frigate controller: one steady 350 ms pulse at speed 540, followed by Stop.
The old helper sent velocity 0.6; the OEM handler at `0x0057a458` scales it
by 900, and the timer at `0x0048e2a0` starts the move once. Our nudge requests
only enough finite travel for its deadline plus 100 ms, then explicitly
stops. A concurrent identical nudge does not extend the pulse. SOAP replies
after stopping, matching the old helper. With this setting off, ONVIF retains
the 1200 ms continuous-move lease. Advertised timeouts match the selected mode.

Preset recall uses one exact remaining displacement per axis, with a global
45-second limit and position checks between axes. Stop normalizes the
module's axis-switch state after natural completion. Coordinates are the
driver's open-loop steps, not independently measured shaft angles.

The OEM stop ioctl requests hardware stop before waiting for completion, but
the driver's failure path may wait up to 15 seconds. The userspace deadman
requests stop within the stated deadline; no software-only implementation can
guarantee physical stopping or syscall completion during a kernel/driver fault.
Native pan, tilt, the manual deadman stop, and repeated mixed-axis preset
return passed on this target. This does not establish fault-time stop latency.

On a fresh kernel/module load the driver's coordinate origin may not match
the physical camera until calibration, and saved presets are meaningful only
in that calibrated coordinate system. With `motor_home=1` (local-only) the
daemon replays the OEM `motor_step_reset` once per boot, asynchronously after
startup: request 5 speed 450; request 2 `{4080, 1200, 2040, 200}` from the
factory `HorizontalMaxStep`, `VerticalMaxStep` and `VerticalResetStep`;
request 3 `{+2040, 0}` into the pan end stop; then request 2 `{0, 0, 0, 0}`,
the driver's own reset, which rams both axes and returns to `2040,200` (the
kernel logs `cx=4080,cy=1200,x move=-2040,y move=-1000`). Each stage is polled
with request 4 for up to 60 s. Limits are then reread and the head returns to
the last position, which the daemon saves in `state_dir/position` once the head
has rested at a new place for five seconds. PTZ and presets are refused while
homing; status reports `homing` and `homed`. `/tmp/jooanipc-open.homed` marks
the boot so restarts and reloads do not repeat it. A native boot requires
`motor_home=1`: there the OEM application never runs, so this is the only
homing. The guarded OEM handoff creates that marker after the OEM's own
homing, so the setting may stay on.

## Presets and status

`preset_set`, `preset_goto`, and `preset_remove` accept slots 1 through 16.
Setting a slot stops current movement and reads the driver position first.
Updates use atomic mode-0600 files under `state_dir/presets.v2.json`; the
state directory is mode 0700. The JSON catalog identifies version 2, backend,
coordinate space (`motor_steps` or replay `normalized`), and records with
`slot`, `name`, `x`, and `y`. Names contain 1–64 bytes of printable UTF-8 and
must be unique ignoring ASCII case. ONVIF tokens remain `preset1` through
`preset16`; names are escaped separately for XML and JSON.

The old `presets.v1` format loads only when v2 is absent. Invalid or empty v2
never restores deleted v1 records. Unnamed updates preserve existing labels;
new unnamed slots receive `Preset N`. Persistence failure rolls back the
in-memory change. OEM preset migration is an explicit deployment step:
copy enabled OEM slot N to slot N+1 with its original name and raw coordinates,
back up the previous application catalog privately, and restart the service.
The application never writes OEM configuration.

Status reports normalized pan/tilt -100 through 100, motion state and preset
slots. Sensor 0’s `flip0` bits also reverse the corresponding manual-control
and displayed-position axes, keeping movement aligned with the upright image.
Persisted presets retain raw driver coordinates. Runtime `flip`, `hflip` and
`vflip` controls persist the sensor bitmask and request an ordered service
restart so image orientation and PTZ signs change together. JSON also reports `position_known`, the backend and asynchronous motion
errors. A successful native move request means the move was accepted; the
status can subsequently report a device or timeout error. On replay the
positions, lights and night mode are explicitly simulated.

## Lights and IR-cut

Factory `HardwareParam/LightParam` and `SubLightParam` both identify:

| Function | GPIO | Electrical behavior |
| --- | --- | --- |
| Infrared illuminator | 59 | Active high |
| White light | 60 | Active high |
| Pilot/alarm LED | 58 | Active high for the main profile |
| IR-cut coil A/B | 49 / 50 | Model 1, 150 ms pulse, then both low |
| Speaker amplifier enable | 63 | Active high; media waits 200 ms before playback |

In the OEM application, `set_IRCUT_Mode` at `0x4861e4` establishes model-1
polarity: night pulses GPIO49 high with GPIO50 low; day pulses GPIO50 high
with GPIO49 low. Both sensor descriptions share the pair, so the replacement
pulses it once. `set_IRLED_Mode` at `0x486028` establishes active-level
handling. Manual `night=0/1` combines
the filter pulse and IR illuminator; the media backend also applies the ISP
day/night mode to both sensors. `ircut` is an alias for the same manual GPIO
operation. `white_light` and `pilot_led` accept 0 or 1.

`speaker_enable` accepts 0 or 1 and drives GPIO63. Its source is factory
`HardwareParam/AudioParam/SpeakGpio`, with the enable/settle sequence recovered
from `0x4808fc`. The OEM audio module leaves its own `spk_gpio` unset, so
userspace owns the amplifier control. The amplifier is active-low on this
board (factory `SpeakLevelEx=0`): `speaker_enable=1` drives GPIO63 low and `0`
drives it high. The media backend enables it before talkback and waits 200 ms.
Shutdown mutes it (GPIO63 high) if this application used it. Replay records
the simulated state without GPIO access. Media releases before
`4b20a364` had this polarity inverted.

The platform's `local.rc` also drives GPIO63 at every boot: it writes `1`
(mute) every 20 ms for roughly the first minute (in a native boot, where no
containment guard takes the pin over, for the whole window) and leaves it at
`1`. Holding the pin high is what silenced the OEM motion siren on hardware.
Do not race that early-boot hook with `speaker_enable`.

GPIO57 is not exposed as a blue LED: factory `IndicatorParam/type` is zero,
and `HardwareParam/OneTouchCallGpio` identifies GPIO57 as a button input.
The unused blue/yellow indicator description must not override that evidence.

Automatic day/night value 2 is explicitly rejected. Although the OEM config
contains EV thresholds 75000/12000, a complete reliable sensor exposure query
ABI has not been established. Returning success or guessing automatic light
changes would misrepresent the hardware behavior. Audio volume is implemented
by the SDK media backend, not by this motor/GPIO module.
