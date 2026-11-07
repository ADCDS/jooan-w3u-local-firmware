# What was planned and done differently

The plan behind the admin UI and the OEM comparison promised some things that were not built,
or were built another way. Each is here with its reason.

| Planned | What happened | Why |
| --- | --- | --- |
| Travel limits in status, for the map's preset marks | Not added | Presets and the position already reach the page normalized to -100..100, so the map needs no limits |
| White-balance gains to tell infrared from daylight when returning to day | Not used | Under a street light the gains stay apart with the infrared on, so the test is fooled. A reversal instead lengthens the hold ([Platform](../media/docs/platform.md)) |
| `IMP_ISP_Tuning_GetEVAttr` as a cross-check of the exposure | Not used | Its ABI on this kernel is unverified, and a wrong call is worse than no check |
| Automatic levels calibrated at dusk and at dawn | Dawn only | They come from one camera's overnight log. Dusk has not been recorded, so the levels stay adjustable and provisional |
| Hue control | Dropped | The driver accepts the call and changes nothing |
| Night and light settings re-applied after homing | Applied once the backend is ready | Nothing about them depends on the head's position |
| Lease readout on the Network page | Gateway and address only | BusyBox `udhcpc` gives the lease to the vendor's `default.script`, which cannot be changed and does not keep it |
| Media `bind=0.0.0.0` restricted to the camera's interfaces | Not restricted in the daemon | The camera has one Wi-Fi interface, and route pruning leaves it no route to anything but its own network and the allowed ones. There is no kernel firewall |
| Scheduled restart by uptime, by wall clock when network time is synced | Built, with "synced" meaning confirmed by network time or by a person | A clock set by hand is as good as one from the network until the next boot |
| A permanent JPEG channel per encoder group | One per request | It takes no reserved memory between stills, and the requests are rare |
| An OSD pool of at least 128 KiB | Left at 64 KiB | OpenIMP only records this value, and the pictures come from elsewhere |
| Fixed lens at 1280×720, or a real 640×360 substream | 1024×576 fixed lens; the substream URL shares the main feed | The larger sizes do not fit the reserved memory ([Hardware](../media/docs/hardware.md)) |
| Strobe with the white light or the infrared | White light only | An infrared flash deters nobody |
| Privacy cover by OSD | By the ISP's picture settings | The OSD path does not reach the encoder channels, and the settings give every pixel black, measured |
| Media installer on Windows through Paramiko | The `ssh` program with `SSH_ASKPASS` | The camera offers only `chacha20-poly1305@openssh.com`, and Paramiko 5.1.0 fails with "no acceptable ciphers" |
| `install-camera.py --first` | The bundle says it is a first one; going ahead takes `--yes-untested` | A flag could be passed with the wrong bundle, and the extra consent says what is at stake |
| HTTPS interop with browsers, go2rtc and zeep in the integration tests | Python's TLS with refusals tested; a Go client and browsers checked by hand; go2rtc and Frigate on camera1 | go2rtc and zeep are not on the hosted runner, and a Go client or a driven browser would be new dependencies of the gate; this keeps the gate the same in both places |
| RTSPS | Left out, by the owner's decision | TLS on every video frame on a busy CPU, for a stream that stays on a trusted network |
| Visual comparison with the design in the repository | The tool is, in `media/tools/ui-compare/`; the design is not | The design files and its canvas runtime stay private, so the tool takes their pictures as an argument |

## Known limits

- A fixed address whose address step fails can come up on a DHCP address, while `network/ip` still
  says `static`, until a restart, unless a native boot is still waiting and a later attempt gets there
  first. A camera that can be reached is better here than a strict one.
- The reset button also works in the handoff once the vendor's program has stopped, not only in a
  native boot. It is useful there, and its reboot counts for nothing against that boot.
- The system clock, with no battery and with network time off, lags by the time without power after
  each power cut. Schedules then run on it after 2 minutes. A privacy schedule fails closed, and with
  network time configured waits 30, in case the router is back from the same cut and has no time yet.
  The page says so.
