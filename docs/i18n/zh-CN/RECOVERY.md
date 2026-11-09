<!-- Translated from docs/RECOVERY.md. Authoritative version: ../../RECOVERY.md -->

# 恢复

## 恢复层级

需要准备两条相互独立的恢复途径：

- **软件包回滚/卸载**：适用于仍能启动、且本地管理功能或 OEM 更新通道仍可正常工作的摄像机。
- **外部 SPI 恢复**：适用于已无法启动或无法访问的摄像机。

媒体层的 appfs 重新刷写还带来了第三条手动恢复途径，仅适用于一种情况：重刷流程让摄像机在已损坏的分区上继续保持运行。参见 [appfs 重刷恢复](#appfs-reflash-recovery)。

仅保存在摄像机上的卸载包并不是一种独立的恢复手段。请将其另行保存在设备之外。对于无法启动或无法接受更新的摄像机，外部 SPI 还原仍然是最可靠的途径。

## 恢复资料

请离线保存以下资料，并标明它们属于哪一台实体摄像机：

- 两份逐字节相同的完整 8 MiB 闪存读取镜像及其 SHA-256 哈希值，可在平台层运行后通过 SSH 获取（[安装，第 7 步](./INSTALL.md#7-back-up-the-camera-over-ssh)），也可使用 SPI 编程器读取；
- PCB `JA-6621 V1.0`、闪存芯片方向、测试夹方向以及接线情况的照片；
- 编程器型号、电压设置、所选的闪存芯片型号，以及一条已确认可用的读取命令；
- 当时实际使用的安装包和卸载包，以及相应的清单和哈希值；
- 每次更新媒体层之前读取的 appfs 分区（`/dev/mtd4`）镜像；发布包中的 `backups/` 目录只保存配置文件；
- 该发布版本源代码的副本以及工具链元数据；
- 经过脱敏处理的记录，描述最近一次成功启动的情况及当时的网络状态。

切勿把一台摄像机的闪存转储提供给另一台设备用作恢复镜像。

<a id="booting-camera-rollback"></a>
## 可启动摄像机的回滚

1. 将摄像机和工作站移到隔离的恢复网络中。
2. 在路由器上阻断摄像机的一切互联网访问。
3. 核对卸载包的哈希值以及 `A12` 目标元数据。
4. 使用需要身份验证的本地更新途径，或文档中说明的离线传输方式。
5. 保持供电稳定，直到软件包报告完成且重启结束。
6. 冷启动摄像机，检查状态和监听端口，并确认已恢复到预期的状态。

卸载会保留出厂数据和 OEM 固件保存的 Wi-Fi 配置，但会删除平台层的状态，包括已提交的 Wi-Fi 网络（`wifi.json`）；此后 OEM 应用程序会使用它自己保存的网络，而这未必是摄像机之前所用的网络。如果摄像机无法完成身份验证、无法校验软件包，或者反复重启，请停止尝试通过网络更新，改用外部恢复。

替换运行时是一项维护事务：在替换唯一的持久运行时期间，已签名的控制器和 SSH 恢复包始终可用。在两分钟的健康试运行期间，监管器会应用已提交的 Wi-Fi 网络（如果没有任何值得等待的对象，原生启动会在 300 秒后放弃等待网关），并重启启动失败的候选运行时：`start.sh` 的时限为 15 秒，而 `entropy-ready.sh` 最多会等待 30 秒，并且原生启动初期收集熵的速度很慢。如果候选运行时仍然失败，或者试运行被中断，它仍会作为唯一的运行时保持选中状态（因为更新已经删除了之前的运行时），监管器也会不断重启它；请通过由控制器管理的 SSH 进行恢复，然后重试或卸载。（在这一问题修复之前，试运行失败会删除候选运行时，导致摄像机没有任何运行时；下一段描述的正是这种状态。）在更新引发的重启之前，旧运行时可能仍会从 tmpfs 中继续运行，但它并不是闪存上的回滚槽位。平台层更新失败时不会回退到 OEM 更新程序：只要控制器在运行，GoAhead 就会一直被终止。

没有运行时，就没有任何组件会去应用已提交的 Wi-Fi 网络。于是原生启动无法连接到网关，又发现没有任何组件在尝试连接，便会在 300 秒后宣告失败并锁存 `native-failed`；之后的启动会运行受防护的 OEM 应用程序，而它会加入自己保存的网络。在已验证的设备上，这个网络是厂商的台架测试网络 `JOOANTEST`（开放网络，以隐藏 SSID 的方式探测）。要在这种状态下访问摄像机：

1. 在摄像机能够接收到信号的接入点上，临时发布一个隐藏的开放网络 `JOOANTEST`，仅允许该摄像机的 MAC 地址接入，并将其置于隔离的子网中。摄像机会在几秒钟内加入。它可能不会获取 IPv4 地址，但它的 IPv6 链路本地地址可以响应 SSH（dropbear 监听在 `[::]:22` 上），例如可以在该接入点上通过 `ssh -L 2222:[fe80::…%<bridge>]:22` 进行访问。
2. 将运行时安装为稳定槽位：把发布暂存目录（release stage）中的 `runtime.tar.gz` 和 `runtime.md5` 复制到 `/opt/custom/jooan-local/slots/A/`，然后在已引入（source）`common.sh` 的 shell 中执行 `jl_lock; jl_write_selection A - 0; jl_unlock`。
3. 删除 `/opt/jooanipc-open/native-failed`，执行 `sync` 后重启。监管器会启动运行时（如果首次启动失败，还会再次重启它），运行时的钩子会应用已提交的网络，原生启动随之通过合格认定。
4. 移除这个临时网络。

## 更新中断

在空间不足的摄像机上，平台层更新会先删除闪存上唯一的运行时，然后才写入新的运行时。如果更新在这两步之间中断，摄像机仍可依靠 RAM 继续工作，但闪存上已经没有下次启动时可运行的内容：“系统”（System）部分会显示更新已中断，重启请求也会被拒绝，并返回 `no-runtime`。

**此时切勿重启摄像机，也切勿拔掉电源**。否则下次启动时将没有网络，您就只能按上文的步骤操作。通过 SSH 执行：

```sh
cat /opt/custom/jooan-local/state/selection
ls /opt/custom/jooan-local/slots/
```

如果选择记录指明了某个槽位（`A - 0`），该槽位确实存在，并且 `df -k /opt` 显示至少有 51 KiB 可用空间，那么一切都没有丢失。如果启动时可用空间不足 50 KiB，厂商的 `startapp` 会清空 `/opt/etc` 和 `/opt/jooanipc-open`，后果比没有运行时更严重，因此请先腾出空间。可以删除的内容：`/opt/custom/jooan-local` 和 `/opt/jooanipc-open` 下任何名为 `*.new`、`*.next` 或 `*.previous` 的文件或目录，以及 `state/net-loss.log`。切勿删除的内容：`config/`、`state/selection`、`controller.tar.gz`、`recovery/`、已选中的槽位，以及 `/opt/jooanipc-open` 中的 `camera.ini`、`deployment.env`、两个启动脚本和各个密钥。如果选择记录显示为 `- - 0`，请重新放回一个运行时。在此之前，请先停止仍在等待这台摄像机的任何安装程序：一旦发现摄像机重启，它就会继续执行。

1. 找到失败的那个软件包，它会一直保留在 RAM 中，直到重启为止：
   ```sh
   ls /run/jooan-local/firmware.*.sqfs
   mkdir /tmp/fw && mount -t squashfs -o ro,loop /run/jooan-local/firmware.<id>.sqfs /tmp/fw
   ```
   如果它已经不在了，发布版本的 `JOOAN_FW_PKG` 中也包含同样的两个文件。在电脑上执行 `dd if=JOOAN_FW_PKG bs=96 skip=1 of=p.sqfs && unsquashfs p.sqfs runtime.tar.gz runtime.md5`，然后在摄像机上执行 `mkdir -p /tmp/fw`，再用 `ssh admin@CAMERA 'cat > /tmp/fw/runtime.tar.gz' < squashfs-root/runtime.tar.gz` 这样的命令把这两个文件复制过去。
2. 将其复制到一个槽位中，由校验和决定是否采用它。首先确认 `df -k /opt` 显示的可用空间不少于 `runtime.tar.gz` 的大小再加 60 KiB；否则请先腾出空间，原因见上文。
   ```sh
   cd /opt/custom/jooan-local/slots
   rm -rf A.new
   mkdir A.new && cp /tmp/fw/runtime.tar.gz /tmp/fw/runtime.md5 A.new/
   [ "$(md5sum A.new/runtime.tar.gz | cut -c1-32)" = "$(cat A.new/runtime.md5)" ] && sync && mv A.new A && sync
   ```
3. 使用正在运行的控制器自带的函数选中该槽位：
   ```sh
   . /run/jooan-local/controller/boot/common.sh; jl_lock; jl_write_selection A - 0; jl_unlock
   ```

监管器只在启动时读取选择记录，因此这样做不会干扰任何正在运行的组件。只要 `df -k /opt` 显示有 51 KiB 或更多可用空间，现在就可以安全地重启摄像机，并重新尝试更新。在摄像机重启之前，管理页面会一直锁定“重启”（Restart）和“重置”（Reset），因此请在同一个 SSH 会话中使用 `jl_reboot_asked` 重启摄像机：它会先保存时钟，并告知下一次启动这次重启是主动请求的。2026-11-07，这一流程已在 camera1 上实际执行过，当时摄像机完全依靠 RAM 运行。

<a id="a-media-change-that-stops-native-boot"></a>
## 导致原生启动失败的媒体层变更

如果媒体守护进程在原生启动期间退出，它已启用的看门狗就会复位主板。如果某次启动记录下了这一故障，下一次启动就会锁存 `native-failed` 并改走交接（handoff）流程；如果某次启动只是因断电或卡死而被中途打断，则会先再获得两次重试机会。如果交接流程同样失败，就会锁存 `boot-trial`，并让受防护的 OEM 应用程序继续运行。如果重启是由平台层主动请求的，被它打断的那次启动不会因此计入中断次数；但在这次重启之前已经记录的故障仍然有效。

没有网关的原生启动会一直等待，既不复位，也不写入任何标记，前提是等待有望取得结果：存在已提交的网络、有运行时正在尝试加入该网络，并且这台摄像机此前曾有一次启动通过了合格认定。否则，它会如上所述在 300 秒后宣告失败。如果某个发布版本在启动后才失去网关，它仍会通过合格认定，除非这种情况连续出现在三次启动中。如果已提交的网络永久消失，而这台摄像机此前已通过合格认定，它就会一直等待下去：要访问它，请重新提供一个名称和密码都相同的网络。此时平台层和 SSH 仍会响应，但 HTTPS 界面不会。恢复方法：

1. 在 `/tmp/jooanipc-open.log` 中查看失败原因；如果主板此后已经复位，则查看转存到摄像机之外的日志副本。用 grep 搜索 `startup failed` 和 `health check`，并在内核消息中搜索 `Process `：如果出现 oops，说明原因可能不在发布版本本身。
2. 如果原因是某项已保存的设置，请将其撤销。例如，在 `/opt/jooanipc-open/state/controls.json` 或 `/opt/jooanipc-open/camera.ini` 中修改。
3. 执行 `cd /opt/jooanipc-open && rm -f native-failed native-trial native-retry boot-trial && sync` 并重启。启动会重新采用原生模式，安装程序也会重新接受这台摄像机。

2026-11-06，这种情况在测试摄像机上发生了两次，起因是一个在隐私模式开启时无法启动的发布版本。两次都按上述方法恢复，每次用时约 5 分钟。

同一天还发生了第三次，而这次的发布版本本身并没有问题：`IMP_ISP_Open failed (-1)`。原因是有一个监控脚本在主板启动期间通过 SSH 读取了 `/proc/jz/isp/isp-m0`。如果在 ISP 模块加载之后、守护进程打开 ISP 之前读取该文件，就会使驱动程序发生 oops（内核消息中会出现 `Process awk`），于是一秒钟后守护进程打开 ISP 失败。原生启动在等待 Wi-Fi 时，会处于这一状态 30 秒甚至更久。任何对该文件进行采样的程序，都必须先确认 `/api/v1/status` 中出现了 `"ready":true`。

已签名的卸载是一种破坏性移除，而不是恢复到先前的状态。它会移除 Web 界面、路由清理、进程防护、本项目的 SSH，以及压缩的控制器/运行时；下次启动时，摄像机将恢复 OEM/GoAhead 的行为，OEM 应用程序将不受任何限制地运行。媒体层位于 `/opt/jooanipc-open` 中的文件及其在 appfs 中的发布版本仍会保留在闪存上，但一旦 `local.rc` 被删除，就不会再有任何组件启动它们。卸载不会恢复旧的、无需身份验证的 telnet 钩子，也不会恢复已归档的先前版本 `/opt/etc/local.rc`。在卸载之前、期间和之后，都要始终保持路由器隔离。

<a id="appfs-reflash-recovery"></a>
## appfs 重刷恢复

`media/scripts/upgrade-appfs.sh` 会重新刷写 `/dev/mtd4`，也就是 appfs：其中包含 OEM 的 `startapp`、`jooanipc`、内核模块、传感器 IQ 参数以及媒体层发布版本。在停止媒体守护进程之前，该脚本会先获取平台层的维护锁（`/run/jooan-local/state.lock`），以暂停监管器对运行时的重启以及网络看门狗触发的重启，同时由 rootfs 中的 `watchdog` 喂狗程序接管已启用的硬件看门狗。管理界面中的“系统 → 媒体”（System → Media）运行的也是这个脚本。`media/scripts/first-appfs.sh` 用于摄像机的首个媒体层发布版本，它从原厂应用程序运行的状态出发，执行相同的步骤。它的回滚还会删除媒体层配置，使摄像机回到原厂状态，仅保留平台层。

- **在提交点之前**（即紧接在 `flashcp` 之前进行的配置切换），任何失败都会使摄像机重启，并回到未被改动的 appfs。
- **在提交点之后**，如果刷写未通过校验，就会利用 RAM 中原始 appfs 的副本进行回滚，最多尝试三次；随后恢复配置文件和 `native-boot.sh`，并重启摄像机。
- **如果回滚也失败**，安装程序不会重启。摄像机会保持运行：rootfs 喂狗程序持续运行，维护锁仍被持有，两个镜像都保存在 `/tmp` 中，即 `/tmp/jipc-appfs-rollback.bin`（原始 appfs）和 `/tmp/jipc-flash-candidate.squashfs`（新镜像）。SSH 仍可正常使用，因为平台层是从 `/run` 和 rootfs 运行的。

请通过 SSH 从这一状态中恢复，期间切勿重启：

```sh
flashcp -v /tmp/jipc-appfs-rollback.bin /dev/mtd4
md5sum /dev/mtd4      # must equal the original appfs MD5
cp /tmp/jipc-install/boot-handoff.sh.before /opt/jooanipc-open/boot-handoff.sh
cp /tmp/jipc-install/native-boot.sh.before /opt/jooanipc-open/native-boot.sh
cp /tmp/jipc-install/camera.ini.before /opt/jooanipc-open/camera.ini
cp /tmp/jipc-install/deployment.env.before /opt/jooanipc-open/deployment.env
chmod 700 /opt/jooanipc-open/boot-handoff.sh /opt/jooanipc-open/native-boot.sh
chmod 600 /opt/jooanipc-open/camera.ini /opt/jooanipc-open/deployment.env
sync
reboot
```

首次安装不会保留任何 `.before` 文件，因为当时还没有媒体层。在执行 `flashcp` 并完成校验之后，请运行以下命令来代替那三行 `cp` 命令：

```sh
cd /opt/jooanipc-open && rm -f boot-handoff.sh camera.ini deployment.env enabled native
```

原始 appfs 的 MD5 就是传给安装程序的第二个参数，即发布包中的 `expected_current_appfs_md5`；`install-camera.py` 在预检时也会把它打印出来。请反复执行 `flashcp`，直到 `md5sum` 的结果与之相符；在相符之前，切勿重启。`/tmp` 位于 RAM 中：任何一次重启都会导致这些镜像丢失。

如果在 appfs 已损坏的状态下断电，或者摄像机发生重启，`startapp` 就已不复存在：不会有任何组件引入（source）`local.rc`，因此平台层和 SSH 都不会启动。此时只能借助 SPI 闪存编程器（8 MiB SPI NOR）以及 appfs 分区或整个闪存的备份来恢复，方法见下文。

<a id="external-spi-recovery"></a>
## 外部 SPI 恢复

已验证的主板使用 8 MiB SPI NOR 闪存（在所观察的设备上为 Puya P25Q64HA）。请核实板上实际焊装的芯片，切勿凭假设行事。

1. 断开摄像机的电源和以太网连接。
2. 使用能够安全用于 3.3 V 芯片的编程器，并根据主板标识和闪存数据手册核实 1 号引脚及芯片方向。
3. 写入之前，先再次读取芯片，并保存这份故障状态下的镜像。
4. 确认备份的长度恰好为 8 MiB，且其 SHA-256 与安装前读取的某一份镜像一致。
5. 使用该编程器已验证可用的命令写入完整镜像。
6. 回读芯片内容，并与备份逐字节比对。
7. 在为摄像机接通正常电源之前，先断开编程器。
8. 只在隔离的恢复网络中启动摄像机，并检查 OEM 内核、两个传感器、无线模块以及网络行为是否正常。

此处有意不提供通用的编程器命令：不同编程器的测试夹接线、控制器电压和闪存支持情况各不相同。请遵循编程器和闪存厂商的文档。切勿对 3.3 V 的 SPI NOR 闪存施加 5 V 电压。

## 恢复之后

发生安全故障时设备上存在的任何凭据或私钥，都应视为可能已经泄露。请更换管理员密码，替换已登记的 SSH 密钥，并更换 Wi-Fi 凭据。在让摄像机恢复正常使用之前，请重新确认路由器隔离有效。

请记录需要恢复的原因，并保留脱敏后的日志。切勿将原始闪存镜像、密钥、令牌、Cookie 或 Wi-Fi 配置上传到 issue 中。

## 固定地址位于错误的网络

固定地址在重启后依然保留，而新路由器可能位于另一个子网。“恢复默认设置”（Reset to defaults）会把地址改回自动获取，同时保留 Wi-Fi 网络，因此按住复位按钮 5 秒钟即可恢复访问。如果够不到该按钮：可以在旧子网上临时架设一台路由器，并让它使用旧网关的地址，因为媒体守护进程只有在该地址有响应后才会启动。然后在管理界面的“网络”（Network）中进行修改。

## 只有满足以下条件，恢复才算完成

- 设备能够反复冷启动；
- 两路 CV2005 都能输出预期的视频；
- SKW6316 接口工作稳定；
- 预期的管理途径能够正常工作；
- 不存在意外的端口和出站流量，或者它们已被阻断；
- 新读取的完整闪存镜像已连同其哈希值一起归档。
