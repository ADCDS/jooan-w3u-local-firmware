# Comparing the admin UI with its design

Pictures of every page at both widths, checked for script errors, CSP reports and sideways
scroll, then side by side with the design. The design's files and its canvas runtime are the
owner's and stay out of this repository, so its pictures come in as a directory with the same
file names (`pantilt-phone.png`).

```sh
python3 media/tools/ui-mock-server.py --port 8094 &
node media/tools/ui-compare/shoot.mjs http://127.0.0.1:8094/ /tmp/build
python3 media/tools/ui-compare/compose.py DESIGN_PICTURES /tmp/build /tmp/compare
```

It needs Node 22, Chromium (`CHROMIUM` names another one) and, for `compose.py`, Pillow. `shoot.mjs`
also takes a camera's URL and admin password, and changes nothing on it.
