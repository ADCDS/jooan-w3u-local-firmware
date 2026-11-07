# Third-party components and redistribution boundary

## Repository licenses

The repository holds two separately licensed layers:

- **The platform** (the repository root: everything outside `media/`) is
  licensed under GPL-2.0-only ([LICENSE](../LICENSE)). Every platform source
  file carries an `SPDX-License-Identifier: GPL-2.0-only` header.
- **The media daemon** (`media/`) is licensed under MIT
  ([media/LICENSE](../media/LICENSE)); its files carry
  `SPDX-License-Identifier: MIT`. Its own dependencies are listed in
  [media/docs/third-party.md](../media/docs/third-party.md).

Each file keeps its own license. The MIT `media/scripts/oem-boot-handoff.sh`
sources the platform's GPL `runtime/boot/common.sh` on the camera at run time;
that does not change either file's license, and MIT code may be distributed as
part of a GPL-2.0 work. Individual files may carry additional notices where
required. Build and packaging tools do not change the license of their inputs.

### Mbed TLS linking exception

`joan-daemon` and the platform's release verifiers (`jooan-sha256`,
`jooan-auth-verify`, `jooan-ironman-inspect`) are compiled against the Mbed TLS
2.25.0 headers (Apache-2.0) and dynamically link the Mbed TLS libraries that
are preinstalled in the camera's OEM rootfs. The project ships no Mbed TLS code
or binaries. Apache-2.0 and GPL-2.0-only are generally considered
incompatible, so the copyright holder of the platform code (ADCDS, its sole
author) grants this additional permission:

> As a special exception, the copyright holder of the platform code gives
> permission to link it with the Mbed TLS library (or a modified version of
> it) distributed under the Apache License 2.0, and to distribute the result.
> You must obey the GNU General Public License in all respects for all code
> other than Mbed TLS. If you modify this code, you may extend this exception
> to your version, but you are not obliged to.

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
supplied privately by that device's owner under whatever rights apply. A
media-layer bundle's appfs image is rebuilt from the owner's own camera and
contains OEM files; it is a private artifact, not a generic release.

## Published media binaries

Since v0.2.34 a release also carries these, none of them vendor code:

- `jooanipc-open`, under MIT, which includes MbedTLS under Apache-2.0;
- `libatomic.so.1`, from GCC's runtime, under GPLv3 or later with the GCC
  Runtime Library Exception;
- `libimp.so`, built from the
  [OpenIMP fork](https://github.com/ADCDS/openimp/tree/jooan/oem-t23-abi), with
  the source that rebuilds it in the same release, and OpenIMP's `NOTICE`.

**OpenIMP has no project-wide licence.** Per its `NOTICE`, licensing is per file.
x264-derived code built into this library is under GPL-2.0-or-later, its WebRTC
code is under BSD-3-Clause, and files without a header carry no explicit grant.
So this library's licensing is not fully settled, which only those files'
authors can change. The maintainer publishes it knowing that. The daemon loads
it at run time, and anyone can build it from that source instead.

## External open-source projects

The release build uses:

- Dropbear 2026.94, MIT-style Dropbear license, fetched from the upstream
  release site with SHA-256 `e098034a…e14c76d`. The release contains a reduced
  server with synchronized `admin` password authentication and optional
  Ed25519 keys; forwarding, PAM, X11, and SFTP are disabled.
- Mbed TLS 2.25.0 headers at commit `1c54b541…a67`; Apache-2.0, used under
  the [linking exception](#mbed-tls-linking-exception) above. The platform
  links to the ABI-compatible Mbed TLS libraries already present on the camera
  and does not redistribute those OEM-built library files.
- The Ingenic GCC 5.4 multilib/uClibc toolchain as a build-time-only SDK input.
  `tools/fetch-ingenic-toolchain.sh` records the public mirror and hashes; the
  toolchain is not committed or included in releases, and its SDK terms apply.

Host tools also use the Python standard library and normal system utilities.
The timestamp's font in `media/src/overlay.c` is font8x8 by Daniel Hepper, in the
public domain.

The media daemon's dependencies (cJSON, Mbed TLS 3.6.7 built into the
program, and the external OpenIMP `libimp.so` with its `libatomic.so.1`) are
covered in [media/docs/third-party.md](../media/docs/third-party.md).

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
