# Admin UI

One page served by the media daemon over HTTPS: `index.html` (markup, icon
sprite), `app.css`, `app.js`. Vanilla JS, no build step; needs a
current browser (CSS `light-dark()`, `inert`, `<dialog>`).

Mock API: `python3 media/tools/ui-mock-server.py`, then open the printed URL
(admin/admin). `--help` lists simulated failures; `/__mock/fail?on=1|0`
toggles "camera not answering".

Budget: the files are embedded in the daemon, which must fit the camera's
appfs partition. `tools/check-size.py` (run by the full gate after `make mips`)
caps the UI at 150,000 bytes raw and the whole stripped daemon at 349,324 bytes
xz, which keeps at least 48 KiB of the partition free.

## What the UI expects

- Every API call sends `X-Requested-With: jipc-ui`; non-GET calls send
  `X-CSRF-Token` (the csrf from `/api/joan/v1/session`).
- `/api/joan/<rest>` relays to the platform's `/api/<rest>`, two at a time: the
  UI queues the rest and retries a 503 "busy" once.
- 401 means signed out. A failed or timed-out request means the camera is not
  answering: a banner retries `GET /api/joan/v1/session` every 10 s.
- Talk posts audio/PCMA in 2000–4000-byte chunks. SSH key `DELETE` sends
  `{"id":"all"}`. Updates: `POST /api/joan/v1/update` (raw package, XHR), then
  `PUT {"id"}`. Wi-Fi staging may return `deadline_seconds`; otherwise the
  90 s window counts from the request.
- Media controls are `POST /api/v1/control {"operation","value","value2"[,"name"]}`:
  pan/tilt `ptz` at ±30/60/100 (Slow/Normal/Fast), `preset_set` into any slot
  1–16, `preset_rename` (name only; 409 for a duplicate name or an empty slot),
  `flip` per lens (1 mirror, 2 vertical flip; "Upside down" is 3),
  `mic_mute` 1/0 (the RTSP audio track stays up and carries silence).
- Status `audio.muted` and `audio.level` (`{"peak_dbfs","rms_dbfs"}` or `null`)
  drive the microphone switch and the "Sound level now" meter (polled every
  2 s on Audio); without them, both stay hidden.
- The stream password: `POST /api/v1/config {"password"}` changes it;
  `GET /api/v1/config/password` is fetched only to show or copy it (the daemon
  answers only the UI's own signed-in fetch) and is forgotten on leaving the
  page. Copied URLs and snippets never contain it: Frigate gets a
  `{FRIGATE_<NAME>_PASSWORD}` placeholder, Home Assistant a `!secret`, curl asks.
- Health shows `watchdog` from `GET /api/v1/config`. SSH keys are checked as
  ed25519 and shown with their SHA256 fingerprint (as `ssh-keygen -l` prints it).
- Image & night reads `GET /api/v1/controls` (every 2 s there, 5 s on
  Overview) and changes it with `POST /api/v1/controls` and a partial object
  (`picture: [null, {...}]` leaves the pan/tilt lens alone); the answer is the
  full state, and the page reads it again a second later, once the engine has
  applied it. Night and white-light schedules also send `utc_offset_min` from
  this browser. `live` drives the darkness meter (`level` against `day_ev` and
  `night_ev`), the state line and the switch countdown (`wait_s - pending_s`),
  and the triggered light's countdown (`trigger_left_s`). The Overview white
  light switch keeps using the `white_light` control (with a schedule it holds
  until the next edge); "Turn on for N" and Home Assistant use
  `white_light_trigger` (value 0: `trigger_s`).
