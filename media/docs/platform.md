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
the driver's own reset, which rams both axes and returns to `2040,1199` (the
kernel logs `cx=4080,cy=1200,x move=-2040,y move=-1000`). Each stage is polled
with request 4 for up to 60 s. Limits are then reread and the head returns to
the last position, which the daemon saves in `state_dir/position` once the head
has rested at a new place for five seconds. PTZ and presets are refused while
homing; status reports `homing` and `homed`. `/tmp/jooanipc-open.homed` marks
the boot so restarts and reloads do not repeat it. A native boot requires
`motor_home=1`: there the OEM application never runs, so this is the only
homing. The guarded OEM handoff creates that marker after the OEM's own
homing, so the setting may stay on.

## Exact moves, recalibration and patrol

ONVIF `AbsoluteMove` and `RelativeMove` go through the same one-axis-at-a-time
worker as presets, at the requested speed (0 to 1 maps to 100–900 steps per
second; the default is the presets' 450). `GetStatus` reports the position to
four decimals and `MoveStatus`. On camera1 (`flip0=3`), +x and +y take the step
counts down, which turns the view right and up, as ONVIF expects.

`TranslationSpaceFov` is what Frigate's autotracker uses: a value of 1 brings the
frame's edge to its centre. Half the field of view is 405 pan steps and 220 tilt
steps, measured from the picture's shift at 1920x1080 by phase correlation, in
moves of 60 to 240 steps that ran to completion, at three speeds: 2.37 px per pan
step and 2.45 per tilt step. Every reversal first loses about 25 steps to gear
backlash, so a short move after changing direction falls that much short, and so
does a return to a preset approached from the other side.

The siren is a generator inside the speaker service, so one thread still owns the
audio output and the amplifier. It comes ahead of a talker: their audio is dropped, and
talk answers 409 until it ends. It ends by itself, each request at 300 s at most, and it ends if
the output fails; nothing limits how often something can ask for it again. Its time and its
stop are looked at between writes to the driver, so if a write ever blocks, the controls
engine cuts the amplifier 2 s after either. If the daemon crashes, the amplifier and the
light stay as they were until the hardware watchdog resets the camera, 180 s at most. It
needs `audio=1`. The microphone is out of it, as in any talk session, so it does not
reach the NVR's recording. `siren` in `/api/v1/controls` keeps `sound` (`alarm`,
`two_tone`, `pulse`, `chime`), `volume` (1 to 100), `seconds` and `strobe`. Volume is the
output's gain: 100% is its unity, where the sounds play just below clipping, and 50% is
15 dB less. The speaker's own volume comes back afterwards. On camera1 it was checked at
5% and 15% for state, timing, the amplifier and video, which it did not disturb, and the
owner found 100% right by ear; its level has not been measured.

`strobe` flashes the white light, 500 ms on and 500 ms off (measured on its pin), which
stays well below the flash rates that are a concern for photosensitivity. It stops after
its time and leaves the light as it was. Any manual change of the light ends it, and one
that came with the siren goes when the siren does, however that ends. `live.strobe_left_s`
reports it. Only the white light flashes: an infrared flash deters nobody.

`overlay` in `/api/v1/controls` burns a timestamp into both lenses' video and
stills: `enabled`, `position` (a corner), `format` (`ymd`, `dmy`, `mdy`) and `name`
(plain text, at most 16 characters). It is redrawn on each second in the camera's
time zone, and shows nothing until network time or a person has confirmed the clock since
the camera started (`live.clock_ok`): the time a boot restores lags by the time without
power, and would be burned into recordings. An OSD group sits between
the frame source and the encoder of each lens all the time, in `main_only`; the IPU
blends the picture into each frame. Measured on camera1 with both lenses drawing: the
daemon went from 36% to 38% of the CPU, with no frames lost. The pictures take about
210 KiB of reserved memory as measured, and by calculation up to about 280 KiB with a
16-character name, only once it has been turned on. The ISP's own OSD is not
used: on camera1 its calls succeed and nothing reaches the channels the encoders read.
Changing digits look like motion to an NVR, so mask that corner in its motion detection.

The two lenses see about as much as each other: the pan/tilt lens's picture spans
roughly nine tenths of the fixed lens's width and all of its height. So the fixed
lens is not a wider view, only another direction, and aiming from it reaches what it
shows.
`tools/aim-calibrate.py` (a host tool, once per camera, about 5 minutes) measures
which motor steps centre each point of that picture and stores six numbers per axis
in `state/aim.json`. A reset to defaults keeps the file, since it describes how the
lenses are mounted; a change of the fixed lens's orientation needs a new calibration, and a calibration saved since 0.2.39 records it and stops being used if it changes. Pan/tilt steps do not depend on the other lens's flip.
Without one the Aim card is hidden and `POST /api/v1/aim` answers 409. On camera1,
with 20 stops matched, points of the street land a median of 1.2% of the frame's
width and 1.1% of its height from the centre, at worst 3% and 3.9%. Something beside
the camera does not: the lenses are some way apart, and points on a nearby wall
landed 20-40% off. Nothing arbitrates with an NVR: while Frigate is auto-tracking, it
keeps steering, and a click competes with it.

For Frigate's autotracking, `onvif.autotracking` needs a zone in `required_zones`
and a preset name in `return_preset`, and Frigate polls `GetStatus` without pausing
while the head moves (56 requests a second measured). Replay protection copes with
that: its table of used tokens is a ring, and a token older than the newest one
overwritten is refused, so a forgotten token cannot be replayed and fresh ones
always pass.

`ptz_recalibrate` runs the boot's homing sequence on demand and returns to where
the head was; `homed_ago_s` in the status says when (`null`: at boot, by an
earlier start of the daemon). `SetHomePosition` saves a home position, kept in the
controls below; `GotoHomePosition` without one goes to the middle of the travel.

The patrol lives in the controls: `{"patrol":{"enabled":true,"pause_s":120,
"stops":[{"slot":1,"dwell_s":10},{"slot":2,"dwell_s":10}]}}`. Each stop waits for
the head to arrive, then stays for `dwell_s` (3 to 3600 s). Any other move, from
the admin UI, ONVIF or an autotracker, pauses it for `pause_s` and it carries on
at the same stop. A deleted preset is skipped. `live.patrol` reports `state` (`off`,
`moving`, `dwell`, `paused`, `privacy`, `homing`, `no_stops`), `slot` and `left_s`.
Its stops are not written as the last position, so a patrol costs no flash writes.

## Privacy mode

`{"privacy":{"on":true}}` in the controls, or a daily window with
`{"privacy":{"schedule":{"enabled":true,"from":"22:00","until":"06:00"}}}` in the
camera's time zone. With a schedule, `on` holds until the window's next edge.
`live.privacy` is what applies now.

A timestamp burned into the picture, when it is on, stays on the black picture.

The cover is the ISP in day mode: brightness, contrast and saturation at 0 on both lenses.
Measured on camera1 at midday and at night, every pixel of both streams then reads black. An IMP OSD cover would need a change to the video graph. The picture
settings are not touched in the file: a manual change during privacy mode is kept
for afterwards and does not reach the lens.

So that no frame can leak, video frames are dropped from before video starts until
the lenses are black and 2 s have passed for what was in the pipe, at every
turn-on and every start of the daemon. Frames that were already in the ring stay out too.
After that the output is checked: during privacy, a frame over 16 KB is not black (a black
keyframe is under 7 KB, a picture over 100 KB), so it is dropped, the hold comes back, and
`live.error` says so. Only H.264 parameter sets get through while
frames are held, because RTSP and the startup check need them and they show
nothing, and each stream then resumes at a keyframe. If the black-out fails, the
frames stay dropped and `live.error` says so: a failure pauses the video instead of
showing it. The hardware watchdog's stream check allows for this.

Leaving privacy should not cost video either. A black picture costs no bits, so rate
control would sit at its lowest quantiser and the first real frames would be huge: up to
640 KB and 1 to 4 frames a second for 15 to 25 s on camera1. So the encoders' floor goes
from 15 to 30 while the picture is black and back afterwards. In three runs by day that
kept 15 fps but for one dip to 7 fps for a second, largest frame 189 KB. On the way out, day or night goes straight back to what privacy
interrupted, since what the exposure reads at the privacy pose says nothing about the view,
and the usual rule corrects it within its wait if the light changed meanwhile. In real darkness
on camera1 (2026-11-07, auto mode, exposure 41,000 to 68,000 against a night level of 30,000),
privacy entered at night was back in night 0.5 s after it ended. Entered in day, with auto chosen
meanwhile, it came back in day and turned to night 66 s later: its 60 s wait and a reading. Across
a daemon restart during privacy every frame stayed black, and it was in night 11 s after the end.
In all three every frame while black was black on both lenses (largest 4.5 KB), the microphone
stayed silent and stills were refused. Video kept 15 fps except at each switch from day to night,
where both lenses drop to 2 to 4 frames a second for 1 to 3 seconds; the run that switched 66 s
after privacy shows that this is the switch itself.

The black-out only works in the ISP's day mode: in night mode it ignores the picture
settings that make it, as it does the Image page's sliders (brightness 40 changed nothing, measured
on camera1), and at night the whole street showed. So privacy runs in day
mode with the infrared off, whatever the chosen mode says, sent at every turn-on, and nothing can
flip it to night meanwhile. Frames stay held until day mode and the black-out are confirmed, plus 2 s. Afterwards the
mode privacy interrupted comes back (a start with privacy already on starts as usual). Measured on camera1 at night with every frame of
both lenses recorded across the switch: pictures, then only black (Y=16, U=V=128 at the
source), then pictures, through a day/night request and a daemon restart too.

The microphone is muted, and moves from anyone else are refused (`privacy mode is
on`), as is setting a home position; a stop or a preset's removal leaves the head going
to its pose. The head goes to `privacy.slot` if that preset exists, and otherwise looks
down at the end of its tilt travel that means down for this mount, on the same pan; it
comes back afterwards, to where it was going if it was moving, and to the saved view if
privacy was already on when the daemon started.

With a schedule, the clock may be hours behind after a power cut, so privacy stays on after
a start until network time or a person has confirmed the clock, or for 2 minutes if nothing
can (30 with network time configured: a router back from the same power cut may need that long
for its own time). A white light schedule waits as long, as a light at 02:00 helps nobody. A night
schedule waits the 2 minutes only, as a slightly wrong clock is much better than no night vision. The
page warns while the clock is unconfirmed. "On" by hand under a schedule lasts until the next edge, across a restart, as
`privacy.held_until`. Turning a schedule on during manual privacy works the same way, and
turning it off while that holds leaves privacy on, by hand. A new pose applies at the next turn-on. On
camera1's mounting no end stop hides the lens: down shows the wall and the ledge
below it, up the street.

## Presets and status

`preset_set`, `preset_goto`, `preset_remove` and `preset_rename` accept slots 1 through 16.
`preset_rename` takes a `name` and changes only the name (409 for a name already in use, an empty slot, or while the head is being steered): the saved position stays,
the motor does not move, and a name another slot already uses (ignoring ASCII case)
or an empty slot is refused.
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

## Image and encoder controls

With the IMP backend, `POST /api/v1/control` also accepts:

- `brightness`, `contrast`, `saturation`, `sharpness`, `denoise` (temporal and
  spatial noise reduction, 128 neutral): `value` 0–255, `value2` the sensor
  index (0 movable lens, 1 fixed lens). There is no hue: the camera's ISP
  driver rejects the V4L2 hue control;
- `anti_flicker`: `value` 0 (off), 1 (50 Hz) or 2 (60 Hz) on both sensors,
  visible as `Antiflicker` in `/proc/jz/isp/isp-m0`;
- `hflip`, `vflip` (`value` 0 or 1) and `flip` (bitmask 0–3), per sensor;
- `isp_night`: the ISP running mode, `value` 0 (day) or 1 (night) on both
  sensors only (`night` drives this and the IR-cut GPIO together);
- `idr`: request a key frame on stream channel `value`;
- `mic_mute`: `value` 1 sends silence on the microphone track (the track and
  its timestamps keep running), 0 restores it; needs `audio=1`.
  `GET /api/v1/status` reports `audio.muted`;
- `volume`: `value` -30–120 (60 is unity gain, about 0.5 dB per step) on
  `value2` 0 (microphone, default 80) or 1 (speaker, default 60); it needs
  `audio=1`. The speaker volume also applies to the next talk session.
- `gain`: the codec's analog gain, `value` 0–31, on `value2` 0 (microphone,
  default 22) or 1 (speaker, default 25), the stock firmware's
  `AudioParam` values. Only the thread that owns the device applies it: the
  microphone within about 100 ms, the speaker when it next opens.

## Remembered controls and automatic day/night

`state_dir/controls.json` (mode 0600, written 3 s after the last change)
keeps the night mode, infrared mode, the automatic levels and wait, the night
and white-light schedules, the white-light trigger time, the pilot LED, the
microphone mute, each lens's picture settings and anti-flicker. The daemon
applies them once the backend is ready, without waiting for homing. A saved file with one
bad value costs that section only: the rest, privacy included, still applies. `GET /api/v1/controls` returns them
with a `live` block (current day/night, the exposure level, the pending
switch, the trigger countdown, the last error); `POST /api/v1/controls` takes
any part of the same JSON, validates all of it, and changes nothing on an
error. The manual controls above (`night`, `white_light`, `pilot_led`,
`mic_mute`, picture, `anti_flicker`) are remembered too.

Night modes (`night` `value`, or `night.mode`): 0 `day`, 1 `night`, 2 `auto`,
3 `schedule` (night from `from` until `until`, local time by
`utc_offset_min`; the admin UI sends the browser's offset). In `auto` the
daemon reads lens 0's `ISP EV value` from the kernel's
`/proc/jz/isp/isp-m0` every 5 s (higher is darker), and only with the video running:
read before the daemon has opened the ISP, that file oopses the driver, so nothing
else should read it during a boot either. It switches to night
when the level stays at or above `night_ev` for `wait_s`, and back to day
when it stays at or below `day_ev` (lower, for hysteresis) for `wait_s`. A start begins
in day and waits 10 s for the exposure, because the first readings can be wrong either
way, then goes to night straight away if the level is at `night_ev`. If it stays in day,
no hold applies, so dusk can still switch it after `wait_s`.
At least 5 minutes pass between automatic switches, because a switch changes
the reading itself. With the IR-cut filter open the level reads lower for the
same light: about 3.6x at twilight, when the sky has plenty of infrared
(measured at dawn on camera1, under a street light; deep night reads about
41,000 in day mode and 37,000 with the infrared on). So `day_ev` must stay
under `night_ev` / 3.6, or the switch to day reads dark again and flaps. A switch that
to day reverses within four holds doubles the hold in night, up to an hour, until a switch lasts,
while the hold in day stays 5 minutes, because that is the near-black one: aimed at
something close, the camera's own infrared can read as daylight, and no level is right
for that. A trigger or a strobe does not count as daylight either: decisions wait for
them, and for `wait_s` after, and the white light going off clears any hold. Changing the
levels, the infrared setting or the mode starts the back-off over. The defaults are 30000 and 5000, with a 60 s wait, from
measurements at dawn; dusk has not been recorded. Schedules use the camera's time
zone (`GmtTz` in the vendor config, set by `PUT /api/v1/timezone`) when it has
one, otherwise `utc_offset_min`.

`ir_mode` (`night.ir`): 0 `night` turns the infrared LEDs on with night
mode; 1 `off` keeps them off (night mode still opens the IR-cut filter and
the image is black and white) for a scene with its own light.

White light: `white_light` sets the manual state. With its schedule enabled
the light follows the schedule, and a manual change holds until the next
schedule edge. `white_light_trigger` (`value` seconds, 0 for the configured
`trigger_s`) turns it on for that long and then back to whatever applies:
Home Assistant calls it on an NVR event.

## Lights and IR-cut

Factory `HardwareParam/LightParam` and `SubLightParam` both identify:

| Function | GPIO | Electrical behavior |
| --- | --- | --- |
| Infrared illuminator | 59 | Active high |
| White light | 60 | Active high |
| Pilot/alarm LED | 58 | Active high for the main profile |
| IR-cut coil A/B | 49 / 50 | Model 1, 150 ms pulse, then both low |
| Speaker amplifier enable | 63 | Active low (`0` = on); media waits 200 ms before playback |

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
drives it high. The speaker service in the media daemon is the only automatic
driver of the pin: it enables the amplifier at the start of each talk session
(never from a cached state), waits 200 ms, and mutes after 500 ms without
audio or when the talker disconnects. Shutdown mutes it (GPIO63 high) if this
application used it. Replay records the simulated state without GPIO access.
Media releases before `4b20a364` had this polarity inverted.

The platform's `local.rc` also drives GPIO63 at every boot: it writes `1`
(mute) every 20 ms for roughly the first minute (in a native boot, where no
containment guard takes the pin over, for the whole window) and leaves it at
`1`. Holding the pin high is what silenced the OEM motion siren on hardware.
When the hook is done it creates `/run/jooan-local/speaker-hook-released`;
until that file exists the media daemon refuses talk and `speaker_enable`, so
the two never race.

GPIO57 is not exposed as a blue LED: factory `IndicatorParam/type` is zero,
and `HardwareParam/OneTouchCallGpio` identifies GPIO57 as a button input.
The unused blue/yellow indicator description must not override that evidence.

The stock firmware's EV thresholds are 75000 (to night) and 12000 (to day).
Automatic mode reads the exposure from the kernel's ISP report rather than
the library's `IMP_ISP_Tuning_GetEVAttr`, whose ABI on this kernel is
unverified. Audio volume is implemented by the SDK media backend, not by this
motor/GPIO module.
