# Admin UI

One page served by the media daemon over HTTPS: `index.html` (markup, icon
sprite), `app.css`, `app.js`. Vanilla JS, no build step; needs a
current browser (CSS `light-dark()`, `inert`, `<dialog>`).

Mock API: `python3 media/tools/ui-mock-server.py`, then open the printed URL
(admin/admin). `--help` lists simulated failures; `/__mock/fail?on=1|0`
toggles "camera not answering".

Budget: the files are embedded in the daemon; keep this directory under 80 KB
(`cat media/ui/* | wc -c`).

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
