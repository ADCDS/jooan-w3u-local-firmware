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
print("install-camera preflight: PASS")
