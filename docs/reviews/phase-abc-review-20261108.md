# Adversarial review — Phases A, B, C (2026-11-08)

Two subagent reviewers hung in this environment without returning; the review
below was conducted directly by the lead, with the same adversarial intent and
evidence standard. Tests: 67 pass across test_finalize_sequence,
test_eth0_collision, test_ironman, test_github_release, test_runtime_hooks,
test_clock_persistence. Shell syntax gate: PASS. Hosted CI gate: exit 0.

## VERDICT: GO (after one HIGH finding was fixed during review)

## Findings

### HIGH (fixed) — reconciler did not complete an interrupted *upgrade*
`jl_finalize_sequence` originally promoted the pending sequence only when
`release-sequence` was absent/empty (the first-install case). An OTA *upgrade*
interrupted mid-finalize leaves the OLD sequence N published, so the reconciler
would drop pending and never advance to N+1 — the exact unjournaled-but-working
state this phase exists to prevent, just one version up. Fixed: the reconciler
now promotes when `pending > current` (absent/empty/non-numeric = 0), which
completes both a first install and an interrupted upgrade and can never publish
a downgrade (the installer already validated `sequence > current` before writing
pending). Covered by test_completes_interrupted_upgrade and
test_drops_stale_lower_or_equal_pending.

### Considered and cleared

- **Stale pending wrongly promoted (CRITICAL if real).** The installer writes
  `release-sequence.pending` only in the final finalize window, *after* full
  activation is journaled (install-upgrade.sh:597, after `write_state activated`
  at ~583). Every earlier failure path `die`s before that point, so a failed
  install never leaves a pending marker. The reconciler additionally gates on
  `migration-state == legacy-retired` and `pending > current`. No path promotes
  a stale or downgrade sequence.
- **Crash safety under JFFS2.** pending is written atomically (`.new` + fsync +
  rename) before the journal flip; the published sequence likewise. A crash at
  any point leaves either the old consistent state or a pending marker the
  reconciler completes. Ordering: pending → flip → publish → rm pending.
- **Persistent-layout / size contract.** `state/` is a declared directory in
  persistent.contract; check_persistent_size allows any file under a declared
  directory, and the freshly built package carries no pending marker anyway.
  Hosted gate exit 0 confirms.
- **Leftover pending on a later unrelated boot.** The reconciler clears a
  pending that is `<= current` (stale) on the next boot, so it cannot linger to
  be misread later.

### PHASE B — eth0 flush

- **carrier==0 guard is correct for this product.** On the JA-A12 `eth0` is a
  USB-net gadget (stock rcS sets `ifconfig eth0 172.16.1.18` unconditionally;
  usbnet.sh = "wifi to usbnet"), never a wired port, so carrier is always 0 and
  flushing the stale address is always right. A hypothetical wired board is still
  protected: only `carrier==1` is skipped, and an empty/unreadable carrier only
  flushes when `ifconfig` actually shows an `inet addr:` to remove.
- **Availability.** `ip addr flush dev eth0` verified present on the live camera
  (BusyBox ip has the `address` object); `ifconfig eth0 0.0.0.0` is the fallback.
  Both idempotent; running every second causes no flapping because stock sets the
  address once at boot, not continuously.
- **Real-hardware proof.** Reproduced the duplicate 172.16.1.0/24 route on the
  camera and the exact function logic removed it, restoring a single wlan0 route.

### PHASE C — docs + serve_ota.py

- **serve_ota.py is minimal and safe.** Reads the package once into memory at
  startup; the handler compares `self.path` against a fixed constant and never
  opens a request-derived path — no traversal, no over-serving, no per-request
  file access. Self-signed cert for use1api.jooaniot.com only; no cloud
  impersonation, no upstream forwarding, no credential handling, no logging of
  device data.
- **Docs are accurate and not dangerously incomplete.** The DNS-redirect
  guidance correctly states the camera ignores DHCP DNS and uses hard-coded
  resolvers, so a port-53 interception (not a DNS-server setting) is required.
  No secrets: scan of INSTALL.md/API.md/serve_ota.py found only legitimate
  instructional references to passwords, no credentials/IPs from the private rig.
- **routes GET-array vs PUT-string** asymmetry documented matches the source
  (json_field extracts a scalar string; routes-list emits a JSON array).

## Must-fix before publish: none outstanding (the HIGH was fixed in this pass).
