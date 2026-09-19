# Third-party components and redistribution boundary

## Repository license

Original source and documentation in this repository are licensed under
GPL-2.0-only. Individual files may carry additional notices where required.
Build and packaging tools do not change the license of their inputs.

## Retained OEM components

The working JA-A12 retrofit depends on components already present on the
owner's camera, including:

- JOOAN bootloader and OEM Linux kernel;
- `jooanipc`;
- Ingenic/vendor ISP, VPU, audio, motor, and sensor modules/libraries;
- CV2005/CV2005S1 sensor IQ and calibration data;
- SeekWave SKW6316 driver and firmware;
- factory configuration and per-device identity material.

These artifacts are **not distributed by this project**. Their copyrights and
licenses remain with their respective owners. The absence of a visible notice
is not permission to copy or redistribute them.

Generic build/release artifacts must not contain OEM binaries, flash dumps,
factory secrets, Wi-Fi credentials, TLS private keys, SSH private keys, device
tokens, or calibration copied from a contributor's camera. Required retained
components are used in place on the recipient's own compatible device or are
supplied privately by that device's owner under whatever rights apply.

## External open-source projects

The release build uses:

- Dropbear 2026.94, MIT-style Dropbear license, fetched from the upstream
  release site with SHA-256 `e098034a…e14c76d`. The release contains a reduced
  key-only server and the repository contains its feature configuration.
- Mbed TLS 2.25.0 headers at commit `1c54b541…a67`; Apache-2.0. The daemon links
  to the ABI-compatible Mbed TLS libraries already present on the camera and
  does not redistribute those OEM-built library files.
- The Ingenic GCC 5.4 multilib/uClibc toolchain as a build-time-only SDK input.
  `tools/fetch-ingenic-toolchain.sh` records the public mirror and hashes; the
  toolchain is not committed or included in releases, and its SDK terms apply.

Host tools also use the Python standard library and normal system utilities.

Do not describe a component as open source merely because source was found on a
public mirror. Verify provenance and license terms first.

## Contributor checklist

Before adding a file or dependency:

1. identify its author, upstream URL, exact revision, and license;
2. confirm that the license is compatible with the intended distribution;
3. retain required notices and source-offer obligations;
4. remove test keys, credentials, customer data, and device identifiers;
5. ensure generated releases are scanned for known vendor binary names and
   high-entropy secret material;
6. document any build-time-only dependency separately from shipped code.

If redistribution rights are unclear, do not commit or release the artifact.
Document how an owner can use their own device-local copy instead.
