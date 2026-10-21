# Optional OEM audio-processing pause

Firmware 0.2.21 adds an opt-in, persistent System setting and the authenticated
`GET`/`PUT /api/v1/system/audio-pause` endpoint. Default: disabled. The setting
is `config/audio-pause` under `/opt/custom/jooan-local`; normal shutdown and
firmware replacement preserve it.

The existing exact-executable-hash guard interposes two verified boundaries:
its outer `usleep(10000)` (return `0x00494e2c`) and its audio queue's
`pthread_cond_timedwait` (return `0x006f4b40`, restricted to thread
`AE_AudioFrameCB`). The latter is essential: the OEM poller can retry an expired
absolute deadline indefinitely and never return to the outer sleep. That busy
poll, rather than AAC encoding, caused the observed CPU load on this unit.

Before pausing at the condition wait, the guard disables cancellation and
releases the queue mutex. It uses relative sleeps while leased, reacquires the
mutex and restores cancellation before returning to the real condition wait.
Each queue-wait pause is bounded to one second, preserving cancellation during
OEM shutdown/reconfiguration even while a healthy lease is being renewed.
No driver frame or publication lock is held while paused. Other callers,
threads, executables and forked children retain normal behavior. No ptrace,
thread IDs or instruction patches are persisted.

Pausing stops the OEM microphone callback and its raw/AAC publications. It does
not implement microphone listening, change the encoders, or unmute the speaker.
Do not equate this control with an audio mute or assume other OEM audio features
continue working.

## Camera-owned recovery

The daemon writes `audio-pause.lease` in `/run/jooan-local` once a second,
expiring eight monotonic seconds later. A missing, stale, malformed or excessively
future lease leaves/resumes OEM audio. The guard acknowledges an effective pause
in the RAM-only `audio-pause.active` file, so the API distinguishes requested
from effective state. No heartbeat writes flash.

Before leasing, the daemon waits for established RTSP media forwarding (at
least 32 KiB forwarded for a sensor). Every observed sensor must continue
forwarding within eight seconds. This observes transport progress, not image
content or decoder correctness. Losing a stream disables the setting and
removes the lease, avoiding repeated automatic pause/retry cycles. Stopping the
NVR intentionally also invokes this recovery; re-enable explicitly after video
returns. If the exact callback never acknowledges within 15 seconds, the setting
is disabled with `callback_unavailable`.

Before the first lease, a persistent `config/audio-pause.pending` marker arms
a 60-second trial. It is removed after a minute of healthy operation or on clean
shutdown/disable. If a daemon restart or power loss interrupts the trial, startup
disables the optimization with `previous_start_interrupted`. Configuration
storage errors never keep renewing the lease. The default audio path and SSH
remain available for recovery.

The System control reports `running`, `waiting_for_video`, `starting`, `paused`
or `recovered`. Recovery disables the saved setting until the administrator
explicitly retries it. Turning it off removes the lease immediately; the
callback resumes on its next 100 ms lease check.

## Qualification

Host tests cover exact-callsite isolation, cooperative pause, explicit resume,
lease expiry, authentication/CSRF, strict boolean input, persistence, stalled
media, and interrupted-trial recovery. On the physical camera verify both video
streams, API off/on behavior, lower CPU use, clean-reboot persistence, and
recovery from interrupted supervision before leaving the setting enabled.
