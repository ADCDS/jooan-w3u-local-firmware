# End-user installer (CLI engine)

`install.py` chains the five steps that take a stock JOOAN JA-A12 to the open
platform and the open media layer, so an owner does not run them by hand:

| Step | What it does | Wraps |
|---|---|---|
| `provision` | confirm the camera is on your Wi-Fi | (Phase E: SoftAP/CoAP; today: vendor app once) |
| `platform` | deliver + trigger the signed platform OTA, enroll an SSH key | `tools/serve_ota.py`, `tools/upload_ota.py`, platform API |
| `backup` | copy every flash partition off the camera (mandatory) | `ssh cat /dev/mtdblock*` |
| `build` | build *your* media image from *your* appfs (no compiler) | `media/tools/build-appfs.sh --first` |
| `flash` | write the media image; native boot qualifies | `media/scripts/install-camera.py` |

Every step is idempotent and checkpointed to `WORK/run-state.json`, so a run that
stops can be resumed by re-running the same command. Nothing is destructive
before `flash`, which asks for explicit confirmation and refuses to run without a
completed backup.

## Why you build the image yourself

The media image contains vendor files from your camera's own application
partition, which cannot be redistributed. The build is a squashfs repack of your
own `mtd4` with the open binaries that ship prebuilt in the media release — no
compiler is involved. It needs `squashfs-tools`; on Windows, run in WSL.

## Use

```sh
# Plan the whole run without touching anything (recommended first):
python3 installer/install.py --dry-run --camera CAMERA_IP \
    --media-release path/to/jooanipc-open-release

# Real run (serve_ota.py must be running and the DNS redirect in place for the
# platform step -- see docs/INSTALL.md):
python3 installer/install.py --camera CAMERA_IP \
    --platform-pkg dist/JOOAN_FW_PKG \
    --media-release path/to/jooanipc-open-release \
    --ssh-key ~/.ssh/id_ed25519 --ssh-pubkey ~/.ssh/id_ed25519.pub \
    --stream-password 'your-stream-password'

# One step at a time, or resume after a stop:
python3 installer/install.py --only backup --camera CAMERA_IP
```

The GUI (added after the end-to-end round trip is proven on a real camera) is a
linear wizard over exactly these steps.

## Status

The CLI engine is tested in `tests/test_installer_orchestrator.py` (ordering,
resume, the flash-requires-backup gate, build-input assembly, dry-run safety).
The full stock -> provision -> OTA -> platform -> media round trip has now been
run once end to end on a real camera (the media build and flash via this repo's
scripts; the camera served RTSP and survived a cold reboot on the open stack).

## The GUI

`gui.py` is a single-window Tk front end over this same engine, so an owner never
needs a terminal: fill in the camera address, the platform package and the media
release, press **Start**, watch a colour-coded live log, and answer one clear
warning dialog before the only irreversible step. It starts in **Dry run** so the
first press plans the whole install and touches nothing.

```sh
python3 installer/gui.py
```

Tkinter ships with CPython on Windows and macOS; on Linux install `python3-tk`.
The GUI adds no behaviour of its own -- it drives exactly the five steps above,
subclassing the engine's `Reporter` and running it on a worker thread -- so the
CLI and the GUI always do the same thing. The `squashfs-tools` requirement for
the build step is unchanged (on Windows, build in WSL).
