# Third party components

Original application code is licensed under MIT. All protocol implementations
were written for this project; ABI declarations describe observed interfaces
and do not contain the vendor's implementation.

| Component | Revision | License | Distribution |
| --- | --- | --- | --- |
| cJSON | 1.7.19, `c859b25da02955fef659d658b8f324b5cde87be3` | MIT | Source and license under `vendor/cjson` |
| MbedTLS | 3.6.7, release SHA-256 `a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6` | Apache-2.0 OR GPL-2.0-or-later; use Apache-2.0 for this build | Official source fetched during configure; full license retained below |
| GNU libatomic | Matching MIPS compiler runtime | GPLv3 with GCC Runtime Library Exception; retain applicable notices when packaging | Required by the separate OpenIMP build, not vendored here |
| External T23/Ace libimp | Operator-selected compatible file | Its original terms | Not distributed; dynamically loaded only for `backend=imp` |
| OEM kernel/modules/IQ | Existing camera installation | Their respective terms | Not distributed |
| Ingenic GCC/uClibc toolchain | Operator-provided GCC 5.4 multilib | Compiler/runtime components' terms | Build-time dependency, not distributed |

The MbedTLS license text is in `../vendor/licenses/mbedtls-LICENSE`. Upstream
source URLs and vendored-file hashes are recorded in `dependencies.lock.json`.
The application's license grants no rights to redistribute vendor artifacts.

Protocol references: RTSP1.0 RFC2326, RTP/RTCP RFC3550, H.264 payload RFC6184,
Digest authentication RFC2617, ONVIF Device/Media/PTZ schemas and WS-Discovery.
