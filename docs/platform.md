# W3-U platform backend

`src/platform.c` contains original MIT-licensed userspace code. The `imp`
backend uses the OEM kernel's already loaded `sample_motor.ko` and GPIO sysfs;
it does not invoke `jooanipc`, CGI, MQTT, ONVIF, a shell, module loading, or boot
configuration. `replay` simulates all of these controls in memory and writes
only its own preset file. Replay status explicitly identifies the backend.

The hardware profile is specifically the JOOAN W3-U / JA-A12 represented by
`../jooan-w3u/dumps/p25q64ha-20250916-mtd/etc/config.org`. It is not a generic
Ingenic GPIO map. Other board revisions require a reviewed profile change.

## Motor ABI and movement

The module's `motor_ioctl` symbol is at section `.text+0x10a0`. Its jump table
in `.rodata+0` maps the literal request numbers below; these are not `_IO*`
numbers synthesized for the host architecture.

| Request | Argument | Use |
| --- | --- | --- |
| 1 | none | Stop |
| 2 | four int32: maximum X, maximum Y, current X, current Y | Reset/calibration; deliberately not issued automatically |
| 3 | two signed int32: delta X, delta Y | Move; can block when both axes are requested |
| 4 | four int32 returned: X, Y, moving, speed | Position/status |
| 5 | int32 pointer, accepted range 100–900 | Speed |
| 6 | none | Return/back operation; not used |
| 8 | two signed int32: delta X, delta Y | Nonblocking finite move |

Independent evidence is the historical export
`../jooan-w3u/ghidra/jooanipc-decompiled.c`: `0x48ce10` issues request 3
(line 43600), `0x48d268` issues request 8 (43760), `0x48d550` request 5
(43813), `0x48d8b8` request 4 (43958), and reset routine `0x48d9d0`
request 2 (44038). Module `motor_get_message` establishes the four returned
int32 fields; `motor_ops_move_not_stop` establishes request 8's behavior.

The verified OEM startup script `jzstart.sh:297–309` loads the driver with
maximum X=8600, maximum Y=2000, and the shared motor-coil switch GPIO17.
The replacement opens `/dev/motor` exclusively as enforced by the driver,
sets speed 450, reads real positions, and submits at most 90 steps on one axis
at a time. It alternates axes for diagonal requests because this board shares
the motor coils. It never directly manipulates motor GPIO17 or the coil pins.

Manual `ptz(x,y)` takes signed normalized velocities from -100 through 100.
Each request expires after 1200 ms; a worker checks at 25 ms intervals and
requests a stop. A finite chunk that remains active after one second also
requests a stop and records an error. `ptz_stop` cancels manual and preset
movement, and shutdown requests a stop before closing the device.

Preset travel consists of finite bounded chunks and has a separate 45-second
total limit. It is an explicit autonomous move; manual request renewal is not
required during preset travel. The service measures position between chunks
and does not estimate native position from elapsed time. Presets outside the
known board range are refused when loaded. Coordinates are the driver's
open-loop step coordinates, not independently measured shaft angles.

The OEM stop ioctl requests hardware stop before waiting for completion, but
the driver's failure path may wait up to 15 seconds. The userspace deadman
requests stop within the stated deadline; no software-only implementation can
guarantee physical stopping or syscall completion during a kernel/driver fault.
Normal native motion and stop latency still require target validation.

No implicit homing is performed. On a fresh kernel/module load the driver's
coordinate origin may not match the physical camera until calibration. Saved
presets are meaningful only in the same calibrated coordinate system; the
replacement does not silently perform the OEM's long end-stop reset sequence.

## Presets and status

`preset_set`, `preset_goto`, and `preset_remove` accept slots 1 through 16.
Setting a slot stops current movement and reads the driver position first.
Updates are atomic files, mode 0600, under configured `state_dir/presets.v1`;
the configured state directory is created with mode 0700. The file begins with
`JIPC_PRESETS_1 imp` or `JIPC_PRESETS_1 replay`, preventing simulated positions
from being reused as native step coordinates. These files belong to this
application; OEM configuration and persistent boot/flash settings are untouched.

Status reports normalized pan/tilt -100 through 100, motion state and preset
slots. JSON also reports `position_known`, the backend and asynchronous motion
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

The historical `set_IRCUT_Mode` at `0x4861e4` (lines 41211–41267)
establishes model-1 polarity: night pulses GPIO49 high with GPIO50 low; day
pulses GPIO50 high with GPIO49 low. Both sensor descriptions share the pair,
so the replacement pulses it once. `set_IRLED_Mode` at `0x486028`
(41145–41184) establishes active-level handling. Manual `night=0/1` combines
the filter pulse and IR illuminator; the media backend also applies the ISP
day/night mode to both sensors. `ircut` is an alias for the same manual GPIO
operation. `white_light` and `pilot_led` accept 0 or 1.

`speaker_enable` accepts 0 or 1 and drives GPIO63. Its source is factory
`HardwareParam/AudioParam/SpeakGpio`, with the enable/settle sequence recovered
from `0x4808fc`. The OEM audio module leaves its own `spk_gpio` unset, so
userspace owns the amplifier control. The media backend enables it before
talkback and waits 200 ms. Shutdown drives GPIO63 low if this application used
it. Replay records the simulated state without GPIO access. A retained OEM
boot hook may hold GPIO63 low for approximately 66 seconds; target deployment
must account for that existing hook without racing it.

GPIO57 is not exposed as a blue LED: factory `IndicatorParam/type` is zero,
and `HardwareParam/OneTouchCallGpio` identifies GPIO57 as a button input.
The unused blue/yellow indicator description must not override that evidence.

Automatic day/night value 2 is explicitly rejected. Although the OEM config
contains EV thresholds 75000/12000, a complete reliable sensor exposure query
ABI has not been established. Returning success or guessing automatic light
changes would misrepresent the hardware behavior. Audio volume is implemented
by the SDK media backend, not by this motor/GPIO module.
