#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""A plain-Tk front end over the install engine (installer/install.py).

The engine already does the work: provision -> platform -> backup -> build ->
flash, each step idempotent and checkpointed. This wraps it in a single window
so an owner never touches a terminal: fill in a short form, press Start, watch a
live log, and answer one clear confirmation before the only irreversible step.

It is deliberately thin. The GUI subclasses the engine's Reporter to turn step /
detail / warn / confirm calls into log lines and a dialog, and runs the engine on
a worker thread so the window stays responsive. Everything the GUI can do, the
CLI can do; this adds no behaviour, only a face.

Tkinter ships with CPython on Windows and macOS, and is `python3-tk` on Linux.
Run: python3 installer/gui.py
"""
from __future__ import annotations

import pathlib
import queue
import sys
import threading
import types

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import install as engine  # noqa: E402

try:
    import tkinter as tk
    from tkinter import filedialog, messagebox, ttk
except Exception as exc:  # pragma: no cover - environment without Tk
    raise SystemExit(
        "this needs Tkinter (python3-tk on Linux; bundled on Windows/macOS): " + str(exc))

REPO = HERE.parent

STEP_TITLES = {
    "provision": "1. Camera on your Wi-Fi",
    "platform": "2. Install the open platform",
    "backup": "3. Back up the camera",
    "build": "4. Build your media image",
    "flash": "5. Install the open media",
}


class GuiReporter(engine.Reporter):
    """Turns engine callbacks into thread-safe UI events."""

    def __init__(self, ui_queue: "queue.Queue", confirm_box: "ConfirmBox"):
        self.q = ui_queue
        self.confirm_box = confirm_box

    def step(self, name, message):
        self.q.put(("step", name, message))

    def detail(self, message):
        self.q.put(("detail", message))

    def warn(self, message):
        self.q.put(("warn", message))

    def confirm(self, prompt):
        # Block the worker thread until the UI thread answers.
        return self.confirm_box.ask(prompt)


class ConfirmBox:
    """A cross-thread confirmation: the worker waits, the UI thread shows a dialog."""

    def __init__(self, root):
        self.root = root
        self._event = threading.Event()
        self._answer = False
        self._prompt = ""

    def ask(self, prompt: str) -> bool:
        self._prompt = prompt
        self._event.clear()
        self.root.after(0, self._show)
        self._event.wait()
        return self._answer

    def _show(self):
        self._answer = messagebox.askyesno(
            "Confirm the irreversible step",
            self._prompt
            + "\n\nThis writes the camera's application partition. If power is "
            "lost while it writes, recovery needs an SPI programmer and your "
            "backup. A full backup has been taken.\n\nProceed?",
            icon="warning", default="no")
        self._event.set()


class App:
    def __init__(self, root):
        self.root = root
        root.title("JOOAN JA-A12 — open firmware installer")
        root.minsize(720, 560)
        self.q: queue.Queue = queue.Queue()
        self.confirm_box = ConfirmBox(root)
        self.worker: threading.Thread | None = None
        self._build_form()
        self._build_log()
        self._poll_queue()

    # ---- layout ----------------------------------------------------------

    def _build_form(self):
        frm = ttk.Frame(self.root, padding=12)
        frm.pack(fill="x")
        self.vars = {}

        def row(label, key, default="", browse=None, show=None, width=44):
            r = ttk.Frame(frm)
            r.pack(fill="x", pady=3)
            ttk.Label(r, text=label, width=20, anchor="w").pack(side="left")
            var = tk.StringVar(value=default)
            self.vars[key] = var
            entry = ttk.Entry(r, textvariable=var, width=width, show=show)
            entry.pack(side="left", fill="x", expand=True)
            if browse:
                ttk.Button(r, text="Browse…",
                           command=lambda: self._browse(var, browse)).pack(side="left", padx=4)

        row("Camera IP", "camera", "")
        row("Camera password", "camera_password", "admin", show="•")
        row("Stream password", "stream_password", "", show="•")
        row("Platform package", "platform_pkg", str(REPO / "dist/JOOAN_FW_PKG"), browse="file")
        row("Media release dir", "media_release", "", browse="dir")
        row("SSH private key", "ssh_key", str(pathlib.Path.home() / ".ssh/id_ed25519"), browse="file")
        row("SSH public key", "ssh_pubkey", str(pathlib.Path.home() / ".ssh/id_ed25519.pub"), browse="file")
        row("Work directory", "work", str(pathlib.Path.home() / ".jooan-installer"), browse="dir")

        opts = ttk.Frame(frm)
        opts.pack(fill="x", pady=(6, 0))
        self.dry_run = tk.BooleanVar(value=True)
        ttk.Checkbutton(opts, text="Dry run (plan only, touch nothing)",
                        variable=self.dry_run).pack(side="left")

        btns = ttk.Frame(frm)
        btns.pack(fill="x", pady=(10, 0))
        self.start_btn = ttk.Button(btns, text="Start", command=self._start)
        self.start_btn.pack(side="left")
        ttk.Button(btns, text="Quit", command=self.root.destroy).pack(side="right")

    def _build_log(self):
        box = ttk.Frame(self.root, padding=(12, 0, 12, 12))
        box.pack(fill="both", expand=True)
        # Step progress line.
        self.progress = ttk.Label(box, text="Ready.", anchor="w",
                                  font=("TkDefaultFont", 10, "bold"))
        self.progress.pack(fill="x", pady=(0, 4))
        self.text = tk.Text(box, height=18, wrap="word", state="disabled",
                            background="#111", foreground="#ddd")
        self.text.pack(side="left", fill="both", expand=True)
        sb = ttk.Scrollbar(box, command=self.text.yview)
        sb.pack(side="right", fill="y")
        self.text.configure(yscrollcommand=sb.set)
        for tag, color in (("step", "#7ac"), ("warn", "#e97"), ("detail", "#9a9")):
            self.text.tag_configure(tag, foreground=color)

    # ---- actions ---------------------------------------------------------

    def _browse(self, var, kind):
        path = (filedialog.askdirectory() if kind == "dir"
                else filedialog.askopenfilename())
        if path:
            var.set(path)

    def _log(self, message, tag=None):
        self.text.configure(state="normal")
        self.text.insert("end", message + "\n", (tag,) if tag else ())
        self.text.see("end")
        self.text.configure(state="disabled")

    def _args(self):
        """Build an argparse-like namespace the engine expects."""
        v = {k: var.get().strip() for k, var in self.vars.items()}
        return types.SimpleNamespace(
            camera=v["camera"], work=v["work"] or str(pathlib.Path.home() / ".jooan-installer"),
            platform_pkg=v["platform_pkg"], media_release=v["media_release"],
            camera_password=v["camera_password"] or "admin",
            stream_password=v["stream_password"],
            ssh_key=v["ssh_key"], ssh_pubkey=v["ssh_pubkey"],
            provision_helper="", only=None, force=False,
            assume_yes=False, dry_run=bool(self.dry_run.get()))

    def _start(self):
        if self.worker and self.worker.is_alive():
            return
        if not self.vars["camera"].get().strip():
            messagebox.showerror("Missing", "Enter the camera's IP address.")
            return
        if not self.vars["media_release"].get().strip():
            messagebox.showerror("Missing", "Choose the media release directory.")
            return
        self.start_btn.configure(state="disabled")
        self._log("", None)
        self.worker = threading.Thread(target=self._run, daemon=True)
        self.worker.start()

    def _run(self):
        args = self._args()
        work = pathlib.Path(args.work)
        state_path = work / "run-state.json"
        state = engine.RunState.load(state_path)
        if args.camera:
            state.camera_ip = args.camera
        key = pathlib.Path(args.ssh_key) if args.ssh_key else None
        reporter = GuiReporter(self.q, self.confirm_box)
        ctx = engine.Context(args=args, state=state, state_path=state_path,
                             report=reporter, key=key)
        try:
            rc = engine.run(ctx)
            self.q.put(("done", rc))
        except Exception as exc:  # keep the UI alive on any engine fault
            self.q.put(("warn", f"unexpected error: {exc}"))
            self.q.put(("done", 1))

    def _poll_queue(self):
        try:
            while True:
                event = self.q.get_nowait()
                kind = event[0]
                if kind == "step":
                    name, message = event[1], event[2]
                    title = STEP_TITLES.get(name, name)
                    self.progress.configure(text=f"{title}: {message}")
                    self._log(f"[{name}] {message}", "step")
                elif kind == "detail":
                    self._log("    " + event[1], "detail")
                elif kind == "warn":
                    self._log("    ! " + event[1], "warn")
                elif kind == "done":
                    rc = event[1]
                    self.progress.configure(
                        text="Done." if rc == 0 else "Stopped — see the log.")
                    self._log("\n== finished ==" if rc == 0
                              else "\n== stopped; fix the issue and press Start to resume ==",
                              "step" if rc == 0 else "warn")
                    self.start_btn.configure(state="normal")
        except queue.Empty:
            pass
        self.root.after(120, self._poll_queue)


def main():
    root = tk.Tk()
    App(root)
    root.mainloop()


if __name__ == "__main__":
    main()
