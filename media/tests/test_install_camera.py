#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Preflight decisions of scripts/install-camera.py; no device access."""
import importlib.util
import sys

spec = importlib.util.spec_from_file_location("install_camera", sys.argv[1])
ic = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ic)

GOOD = {"appfs_size": "3211264", "appfs_name": "appfs", "appfs_md5": "a" * 32, "opt_free": "120",
        "trial": "no", "native": "yes", "native_failed": "", "wifi_json": "yes", "qualified": "1"}
LOCAL = {"release_sequence": 58}
APPFS = {"expected_current_appfs_md5": "a" * 32}


def problems(facts=None, platform=None, local=None, appfs=None, config=None):
    return ic.preflight_problems(GOOD | (facts or {}), platform or {"release_sequence": 57},
                                 local, appfs, config or {})


assert problems(local=LOCAL, appfs=APPFS) == []
# A latched native failure is reported, never silently installed over.
found = problems({"native_failed": "boot=x stage=failed: network "})
assert len(found) == 1 and "native-failed is latched (boot=x stage=failed: network)" in found[0], found
# Native without a committed Wi-Fi network would fail after 300 s: refuse.
assert any("committed Wi-Fi" in p for p in problems({"wifi_json": "no"}))
# ...including when the bundle is what turns native on,
assert any("committed Wi-Fi" in p for p in problems(
    {"native": "no", "wifi_json": "no"}, config={"files": {ic.NATIVE: ""}}))
# ...but not when the bundle turns native off, or native is off and stays off.
assert problems({"wifi_json": "no"}, config={"files": {ic.NATIVE: None}}) == []
assert problems({"native": "no", "wifi_json": "no"}) == []
# Existing gates still hold.
assert any("sequence 58" in p for p in problems(platform={"release_sequence": 58}, local=LOCAL))
assert any("bundle expects" in p for p in problems({"appfs_md5": "b" * 32}, appfs=APPFS))
assert any("did not qualify" in p for p in problems({"qualified": "0"}))
assert any("64 KiB" in p for p in problems({"opt_free": "10"}))
# Whether ending on the fallback is a failure depends on what the camera should do.
assert ic.native_expected({"native": "yes"}, {}) and not ic.native_expected({"native": "no"}, {})
assert ic.native_expected({"native": "no"}, {"files": {ic.NATIVE: ""}})
assert not ic.native_expected({"native": "yes"}, {"files": {ic.NATIVE: None}})
# A release with another libimp must bring the sums the boot checks it against.
SAME = APPFS | {"release": "11111111-904d60a6"}
NEW = APPFS | {"release": "22222222-8b374e7a"}
FACTS = {"libimp_sha256": "904d60a6" + "0" * 56}
assert problems(FACTS, appfs=SAME) == []
assert any("changes libimp" in p for p in problems(FACTS, appfs=NEW))
assert any("changes libimp" in p for p in problems(FACTS, appfs=NEW | {"imp_md5": "c" * 32}))
assert any("changes libimp" in p for p in problems(
    FACTS, appfs=NEW | {"imp_md5": "c" * 32, "imp_sha256": "904d60a6" + "0" * 56}))
assert problems(FACTS, appfs=NEW | {"imp_md5": "c" * 32, "imp_sha256": "8b374e7a" + "0" * 56}) == []
# A first install: only a camera with the platform, its own stock partition and Wi-Fi.
FIRST = {"appfs_size": "3211264", "appfs_name": "appfs", "appfs_md5": "a" * 32, "opt_free": "200", "native": "no",
         "enabled": "no", "wifi_json": "yes", "oem": "1", "daemon": "0", "sensors": "cv2005 cv2005s1"}
READY = {"state": "ready", "release_sequence": ic.FIRST_PLATFORM_SEQUENCE}
def first(**facts):
    return ic.first_problems(FIRST | facts, READY, APPFS)
assert first() == []
assert any("another copy" in p for p in first(appfs_md5="b" * 32))
assert any("already has a media layer" in p for p in first(native="yes"))
assert any("already has a media layer" in p for p in first(enabled="yes"))
assert any("already has a media layer" in p for p in first(daemon="1"))
assert any("stock application" in p for p in first(oem="0"))
assert any("stock application" in p for p in first(oem="2"))
assert any("committed Wi-Fi" in p for p in first(wifi_json="no"))
assert any("96 KiB" in p for p in first(opt_free="90"))
assert any("not a JA-A12" in p for p in first(appfs_size="4096"))
assert any("install the platform" in p for p in ic.first_problems(FIRST, READY | {"state": "setup"}, APPFS))
# A camera with other sensors would pass everything else and end up with no video at all.
assert any("sensors" in p for p in first(sensors="sc1a4t sc1a4t"))
assert any("sensors" in p for p in first(sensors=""))
# v0.2.21 reports "ready" too, and its boot hook knows nothing of a media layer.
assert any("sequence 55" in p for p in ic.first_problems(FIRST, {"state": "ready", "release_sequence": 55}, APPFS))
assert ic.stream_password_problem("a good one") == ""
for bad in ("", " x", "x ", "a\nb", "a\tb", "x" * 129):
    assert ic.stream_password_problem(bad), bad
# The whole of a first install, against a stand-in for the camera.
import hashlib
import json
import os
import pathlib
import tempfile


class FakeCamera:
    def __init__(self, facts, state="ready"):
        self.facts, self.state, self.calls = facts, state, []

    def ssh(self, script, data=None, timeout=120, check=True):
        self.calls.append((script, data))
        if "appfs_size" in script:
            return "".join(f"{k}={v}\n" for k, v in self.facts.items())
        if script.startswith("md5sum /dev/mtdblock4"):
            return self.after_reboot + "\n"
        return ""

    def boot_id(self):
        return "b" * 36

    def platform(self, method, path, *_):
        return 200, {}, json.dumps(READY | {"state": self.state}).encode()

    def written(self, path):
        for script, data in self.calls:
            if f"cat > {path}.tmp " in script:
                return data
        raise AssertionError(f"{path} was not written")


class Args:
    yes_untested = True


with tempfile.TemporaryDirectory() as temporary:
    bundle = pathlib.Path(temporary)
    files = {"appfs.img": b"image", "first-appfs.sh": b"flash", "native-boot.sh": b"boot", "boot-handoff.sh": b"hand"}
    for name, data in files.items():
        (bundle / name).write_bytes(data)
    digest = lambda data: hashlib.md5(data).hexdigest()
    spec = {"jooanipc_open": APPFS | {"first": True, "release": "11111111-22222222", "image": "appfs.img", "image_md5": digest(b"image"),
                                      "installer": "first-appfs.sh", "installer_md5": digest(b"flash"), "app_md5": "1" * 32,
                                      "imp_md5": "2" * 32, "imp_sha256": "3" * 64, "atomic_md5": "4" * 32}}
    real_wait_reboot = ic.wait_reboot
    ic.wait_reboot = lambda cam, before, aborted, after_boot=None, **_: (after_boot(), "native")[1]
    os.environ["STREAM_PASSWORD"] = "stream-secret"
    camera = FakeCamera(FIRST)
    camera.after_reboot = spec["jooanipc_open"]["image_md5"]
    assert ic.first_install(Args, camera, bundle, spec) == 0
    root = "/opt/jooanipc-open"
    ini = camera.written(f"{root}/camera.ini.next").decode()
    release = "/mnt/mtd/jooanipc-open/releases/11111111-22222222"
    assert f"libimp={release}/libimp.so\n" in ini and "password=stream-secret\n" in ini and "audio=1\n" in ini
    assert "watchdog=1\n" in ini and "motor_home=1\n" in ini and "RELEASE-ID" not in ini
    env = camera.written(f"{root}/deployment.env.next").decode()
    assert f"JIPC_RELEASE={release}\n" in env and f"JIPC_LIBIMP_SHA256={'3' * 64}\n" in env
    assert camera.written(f"{root}/native-boot.sh") == b"boot" and camera.written(f"{root}/boot-handoff.sh.next") == b"hand"
    assert b"PRIVATE KEY" in camera.written(f"{root}/key.pem") and b"CERTIFICATE" in camera.written(f"{root}/cert.pem")
    # The flash runs last, and detached, because its connection goes with the reboot.
    scripts = [script for script, _ in camera.calls]
    flash = next(i for i, script in enumerate(scripts) if "sh /tmp/first-appfs.sh" in script)
    assert spec["jooanipc_open"]["image_md5"] in scripts[flash] and "a" * 32 in scripts[flash] and scripts[flash].rstrip().endswith("&")
    assert not any("mtd" in script and "flashcp" in script for script in scripts)
    assert all(i < flash for i, script in enumerate(scripts) if "cat >" in script)

    def refused(camera, needle, args=Args, sent=spec):
        try:
            ic.first_install(args, camera, bundle, sent)
        except SystemExit as stop:
            assert needle in str(stop), stop
            assert not any("first-appfs.sh" in script and "sh /tmp" in script for script, _ in camera.calls)
            return
        raise AssertionError("it went ahead")
    # Nothing goes to a camera that is wrong for it, or without the two extra consents.
    refused(FakeCamera(FIRST | {"appfs_md5": "b" * 32}), "another copy")
    class NoConsent:
        yes_untested = False
    camera = FakeCamera(FIRST)
    refused(camera, "--yes-untested", NoConsent)
    assert not any("cat >" in script for script, _ in camera.calls)
    del os.environ["STREAM_PASSWORD"]
    refused(FakeCamera(FIRST), "STREAM_PASSWORD")
    os.environ["STREAM_PASSWORD"] = "two\nlines"
    refused(FakeCamera(FIRST), "control character")
    os.environ["STREAM_PASSWORD"] = "stream-secret"
    (bundle / "appfs.img").write_bytes(b"other")
    refused(FakeCamera(FIRST), "checksum mismatch")
    (bundle / "appfs.img").write_bytes(b"image")
    # A camera that backed out is reported at once, and tidied, not waited on for ten minutes.
    camera = FakeCamera(FIRST)
    camera.after_reboot = "a" * 32
    try:
        ic.first_install(Args, camera, bundle, spec)
        raise AssertionError("it claimed success")
    except SystemExit as stop:
        assert "not installed" in str(stop) and "stock" in str(stop), stop
    assert any(script.startswith("rm -f") and "camera.ini.next" in script for script, _ in camera.calls)
# A connection that fails while the flash script runs is not a restart. Nothing happens on
# the old boot ID but waiting; only a new one ends the wait.
class Polled:
    def __init__(self, answers):
        self.answers, self.calls = list(answers), []

    def boot_id(self):
        answer = self.answers.pop(0)
        if isinstance(answer, Exception):
            raise answer
        return answer

    def ssh(self, script, **_):
        self.calls.append(script)
        return "native boot qualified\n"

    def up(self):
        return True


ic.time.sleep = lambda seconds: None
ic.wait_reboot = real_wait_reboot
seen = []
camera = Polled(["old", RuntimeError("dropped"), OSError("no route"), "old", "new"])
assert ic.wait_reboot(camera, "old", aborted=lambda: "", after_boot=lambda: seen.append("after")) == "native"
assert seen == ["after"] and not camera.answers
# ...and is not asked about the partition, or tidied, until the boot ID has changed.
camera = Polled(["old", RuntimeError("dropped"), "old", "new"])
order = []
def noting():
    order.append(len(camera.answers))
    return ""
ic.wait_reboot(camera, "old", aborted=noting, after_boot=lambda: order.append("flashed"))
assert order[-1] == "flashed" and camera.answers == []
# What the script says decides what the owner is told.
assert "nothing was changed" in ic.stopped_message("9.1 ABORT: original appfs changed")
message = ic.stopped_message("9.1 ABORT: rollback failed 3 times; camera kept up")
assert "nothing was changed" not in message and "DO NOT restart" in message
assert "nothing was changed" not in ic.stopped_message("9.1 ABORT: config space low after install; feeder retained")
try:
    ic.wait_reboot(Polled(["old"]), "old", aborted=lambda: "9.1 ABORT: rollback failed 3 times")
    raise AssertionError("it kept waiting")
except ic.InstallerStopped as stop:
    assert "DO NOT restart" in str(stop)
# Every message past a script's point of no return is known for what it is: a new one must not read
# as "nothing was changed".
import re
for name in ("first-appfs.sh", "upgrade-appfs.sh"):
    text = (pathlib.Path(sys.argv[1]).parent / name).read_text()
    after = text[text.index("# Commit point"):]
    for said in re.findall(r"""\bdie\s+["']([^"']*)""", after):
        assert ic.after_commit(said), (name, said)
    before = text[:text.index("# Commit point")]
    for said in re.findall(r"""\bdie\s+["']([^"']*)""", before):
        assert not ic.after_commit(said), (name, said)
# The password for ssh is only ever in the environment, and the helper prints it.
with tempfile.TemporaryDirectory() as temporary:
    helper = ic.askpass_helper(temporary)
    import subprocess
    out = subprocess.run([str(helper)], env={"CAMERA_ASKPASS": "pass word"}, capture_output=True, text=True, check=True).stdout
    assert out == "pass word\n" and "pass word" not in helper.read_text()
camera = ic.Camera("example", "secret", "")
# A platform update that stopped says so at once, and says not to restart.
class Answers:
    def __init__(self, *said):
        self.said = list(said)

    def platform(self, *args, **kwargs):
        said = self.said.pop(0)
        if isinstance(said, Exception):
            raise said
        return said


assert ic.platform_stopped(Answers((200, {}, b'{"state":"running"}')), None) == ""
assert ic.platform_stopped(Answers(OSError("no route")), None) == ""
assert ic.platform_stopped(Answers((500, {}, b"")), None) == ""
assert ic.platform_stopped(Answers((200, {}, b"not json")), None) == ""
assert "stopped (mount)" in ic.platform_stopped(Answers((200, {}, b'{"state":"failed","reason":"mount"}')), None)
assert "not restarted" in ic.platform_stopped(Answers((200, {}, b'{"state":"complete","restart":"needed"}')), None)
assert "DO NOT restart" in ic.stopped_message(ic.platform_stopped(Answers((200, {}, b'{"state":"failed"}')), None))
# An update brings the boot script its release goes with, and a bundle without one is refused.
with tempfile.TemporaryDirectory() as temporary:
    bundle = pathlib.Path(temporary)
    image, flash, boot = b"i" * 3211264, b"flash", b"boot"
    for name, data in (("appfs.img", image), ("upgrade-appfs.sh", flash), ("native-boot.sh", boot)):
        (bundle / name).write_bytes(data)
    digest = lambda data: hashlib.md5(data).hexdigest()
    update = {"image": "appfs.img", "image_md5": digest(image), "installer": "upgrade-appfs.sh", "installer_md5": digest(flash),
              "native_boot": "native-boot.sh", "native_boot_md5": digest(boot)}
    ic.check_appfs_artifacts(bundle, update)
    for bad in ({k: v for k, v in update.items() if k != "native_boot"}, update | {"native_boot_md5": "0" * 32}):
        try:
            ic.check_appfs_artifacts(bundle, bad)
        except SystemExit as stopped:
            assert "native-boot" in str(stopped), stopped
        else:
            raise AssertionError(f"accepted {bad}")
camera.prefix = []
environment = camera.ssh_environment()
assert environment["CAMERA_ASKPASS"] == "secret" and environment["SSH_ASKPASS_REQUIRE"] == "force"
assert pathlib.Path(environment["SSH_ASKPASS"]).exists()
print("install-camera preflight: PASS")
