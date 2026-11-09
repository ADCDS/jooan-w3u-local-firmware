<!-- Translated from docs/INSTALL.md. Authoritative version: ../../INSTALL.md -->

# 安装

本指南面向拥有 JOOAN W3-U 摄像机、能够按照说明操作并复制命令，但并不精通 Linux 的用户。自行构建发布版本的维护者请参阅[构建与发布](../../../docs/BUILDING.md)。

<a id="read-this-first"></a>
## 请先阅读

截至 2026-11-09，[GitHub Releases](https://github.com/ADCDS/jooan-w3u-local-firmware/releases) 上有以下版本：

- **v0.2.21**（2026-10-16）是本指南各个编号步骤所讲解的版本。它通过 Wi-Fi 安装，无需额外硬件。安装后，您将获得本地 Web 控制台、SSH、两个镜头的 RTSP 视频，以及供 Frigate 等 NVR 使用的 ONVIF 云台控制和预置位，并且摄像机将无法再访问 JOOAN 云端。视频仍由厂商的摄像机程序提供，该程序会继续在受限环境中运行。
- **v0.2.40**（2026-11-09）是最新版本。它新增了位于 [`media/`](../../../media/) 的开源媒体守护进程，本仓库其余部分描述的正是这一版本。现已有一台摄像机按照本说明从原厂状态升级到 v0.2.40 并安装了其媒体层，先后共完成两次。选择该版本之前，请先阅读[媒体层](#the-media-layer)一节，包括其中哪些内容已经测试、哪些尚未测试。
- **每个版本都只能安装在厂商固件与已验证设备一致的摄像机上**。在写入任何内容之前，安装程序会将摄像机上的厂商文件与已验证设备的进行比对：v0.2.21 比对整个应用分区，v0.2.40 则比对其所依赖的十二个文件。如有差异，安装程序会拒绝安装，并让摄像机保持原样。此外，媒体层对应用分区还有更多要求：它所保留的每个文件都必须存在，并且摄像机必须配备 CV2005 传感器。目前还没有人知道当下销售的摄像机是否搭载相同的厂商固件。
- 目前还没有任何版本属于“受支持”版本。迄今为止的每个版本都是工程预发布版，且仅在一台摄像机上测试过。

## 通过无线（OTA）还是借助硬件？

| 操作 | 方式 | 额外硬件 |
|---|---|---|
| 首次安装 | 通过 Wi-Fi，借助厂商固件内置的 Web 更新程序 | 无 |
| 后续更新和卸载 | 通过 Wi-Fi，在 Web 控制台的“固件”（Firmware）部分进行 | 无 |
| 备份摄像机闪存 | 安装完成后通过 SSH 进行（[第 7 步](#7-back-up-the-camera-over-ssh)） | 无 |
| 芯片级备份或恢复 | 拆开摄像机，使用 SPI 闪存编程器和 SOIC-8 测试夹 | 安装时并非必需。但对于已无法启动的摄像机，这是唯一的恢复方法 |
| 媒体层（v0.2.40） | 通过 Wi-Fi 和 SSH。它会重写摄像机的应用分区 | 安装时无需额外硬件。但如果在写入过程中断电，就只能依靠编程器和备份来恢复 |

**安装 v0.2.21 不需要 microSD 卡、USB 线、串口适配器或闪存编程器，也不需要拆开摄像机**。本说明不会用到摄像机上的以太网尾线。

任何情况下都不需要 microSD 卡。如果插入了 microSD 卡，固件会将崩溃日志写入卡中。在后备启动模式下，媒体层也可以从卡中运行其程序文件，但这属于维护者的范畴，并不是一种安装途径（参见[媒体层](#the-media-layer)）。

## 所需条件

- **摄像机**：必须是标签或厂商 App 中显示型号为 `JA-A12` 的 JOOAN W3-U。其他以“W3-U”名义销售的摄像机采用的是不同的芯片：请阅读[兼容性](../../../docs/COMPATIBILITY.md)。安装程序会检查型号和厂商文件，并拒绝不符合的摄像机。但它无法看到电路板，因此拆开摄像机是唯一完整的检查方法（“兼容性”一文说明了需要查看哪些内容）。
- **JOOAN 手机 App**：厂商固件要借助该 App 才能接入您的 Wi-Fi。本项目没有其他办法让原厂摄像机联网。
- **一台 Wi-Fi 路由器**，需满足以下条件：
  - 摄像机能够加入其 WPA2 网络。曾有一台采用 Qualcomm ath11k 无线芯片的 OpenWrt 接入点无法加入，而基于 Realtek 的接入点可以正常使用（参见[兼容性](../../../docs/COMPATIBILITY.md#wi-fi-access-point-interoperability)）；
  - 您能够禁止单台设备访问互联网。路由器通常将这项功能称为家长控制、访问控制或防火墙规则；
  - 您的电脑与摄像机处于同一网络。
- **一台电脑**，运行 Windows 10 或更高版本、macOS 或 Linux，并具备：
  - Python 3.9 或更高版本；
  - Python 软件包 `cryptography`；
  - SSH 客户端。Windows 10 及更高版本、macOS 和 Linux 均已内置。
- **大约 30 分钟**的时间，以及为摄像机供电的稳定电源（5 V，1.5 A）。安装过程中切勿拔掉摄像机的电源。

## 分步操作

### 1. 使用 JOOAN App 设置摄像机

按照 App 的常规设置流程，将摄像机连接到它今后将一直使用的 Wi-Fi 网络。以后再更改摄像机的网络会比较麻烦（参见[第 8 步](#8-use-the-camera)）。App 通过 JOOAN 云端与摄像机配对，因此请在摄像机仍能访问互联网时完成这一步；之后的第 3 步会切断其互联网访问。（厂商固件还提供设置热点和以太网 DHCP 两种途径，或许可以不用 App；但这两种方式都没有按本指南测试过。）

如果 App 提示更新摄像机固件，请拒绝。较新的厂商固件会改变安装程序所检查的文件，导致安装程序拒绝该摄像机。

**成功标志**：App 显示实时视频。

### 2. 查找摄像机的 IP 地址

在路由器的已连接设备列表中查找。厂商固件会以 `JAIPC` 这一名称向路由器申请地址。摄像机的 MAC 地址可在 App 的设备信息中查到。建议在路由器的 DHCP 设置中为摄像机保留该地址，以免地址发生变化。

**成功标志**：从电脑执行 `ping CAMERA_IP` 时摄像机有响应。在本步及下文中，请将 `CAMERA_IP` 替换为您查到的地址，例如 `192.168.1.50`。

### 3. 禁止摄像机访问互联网

在路由器上禁止摄像机访问互联网，并且只禁止摄像机。您的电脑应继续留在同一网络中。

每次通电后的几秒钟内，在本固件接管之前，厂商的 Web 服务器和更新程序会先运行。这项屏蔽可让这段时间窗口与互联网隔绝。从现在起，JOOAN App 将无法再访问这台摄像机。这是预期中的情况。

**成功标志**：App 显示摄像机离线，而 `ping CAMERA_IP` 仍有响应。

### 4. 准备电脑

将以下内容下载到同一个新建的文件夹中：

1. **v0.2.21 源代码**，下载地址为 <https://github.com/ADCDS/jooan-w3u-local-firmware/archive/refs/tags/v0.2.21.zip>。将其解压，会得到一个名为 `jooan-w3u-local-firmware-0.2.21` 的文件夹。
2. [v0.2.21 发布页面](https://github.com/ADCDS/jooan-w3u-local-firmware/releases/tag/v0.2.21)上的以下六个文件。请将它们一起放到源代码文件夹旁边一个名为 `release` 的文件夹中：`JOOAN_FW_PKG`、`JOOAN_FW_PKG.sha256`、`JOOAN_UNINSTALL`、`JOOAN_UNINSTALL.sha256`、`manifest.json` 和 `manifest.json.sig`。

源代码必须是 v0.2.21，而不是当前的 `main` 分支。当前版本的上传工具会拒绝 v0.2.21 软件包，并报错 `signed release target/ABI contract does not match this repository`。

安装 `cryptography`。

Linux 或 macOS：

```sh
python3 -m pip install --user cryptography
```

在 Debian 或 Ubuntu 上，也可以使用 `sudo apt install python3-cryptography`。

Windows（命令提示符）：

```bat
py -m pip install cryptography
```

**成功标志**：以下命令会输出一个版本号：

```sh
python3 -c "import cryptography; print(cryptography.__version__)"
```

在 Windows 上，此处以及第 5 步中请输入 `py` 代替 `python3`。

### OTA 分发：摄像机实际上如何获取软件包

请在执行第 5 步之前阅读本节。原厂更新程序采用的是从云端**拉取**的方式，而不是上传。`tools/upload_ota.py` 会触发更新，但它通过 POST 发送的内容会被忽略：随后摄像机会通过 TLS 从一个固定的云端 URL `https://use1api.jooaniot.com/fw/JOOAN_FW_PKG` 下载固件。因此，除了运行该工具之外，安装还需要满足两个条件：

1. **对摄像机而言，`use1api.jooaniot.com` 必须解析到负责提供软件包的那台电脑**。这是大多数人容易忽略的部分。摄像机**会忽略路由器通过 DHCP 下发的 DNS 服务器**，转而查询硬编码的公共解析服务器（223.6.6.6 和 8.8.8.8）。更改“您的 DNS”不起任何作用。重定向必须在网络路径上完成。以下两种方法可行：

   - **由路由器为摄像机改写该域名的解析结果**。在 OpenWrt 上，只需一行 dnsmasq 配置 `address=/use1api.jooaniot.com/<PC-IP>`，再加上一条防火墙规则，**将摄像机自身发出的 53 端口流量重定向到路由器的解析服务**（即把摄像机的 UDP/TCP 53 端口流量 DNAT 到路由器），否则摄像机会绕过路由器的 DNS。这两项设置都应仅作用于摄像机的 MAC 地址。
   - **在摄像机所在子网中部署一台 DNS 重定向设备**，对这一个域名返回电脑的地址，其余查询一律转发；同样需要将摄像机的 53 端口流量指向这台设备。

   您无需伪造云端的其他部分。只需处理这一个主机名，而且仅用于固件下载。

2. **电脑必须在 TCP 443 端口上提供软件包**。`tools/serve_ota.py` 正是做这件事，且仅此而已：它使用自签名证书（摄像机不会校验该证书），对 `/fw/JOOAN_FW_PKG` 的请求返回您指定的文件，对其他所有路径一律返回 404。它不会冒充云端 API，不会向上游转发任何内容，也不会接触设备的凭据。

如果您的摄像机已经在运行 jooan-local（即后续更新，而非首次安装），则以上内容均不适用：更新会通过需要身份验证的 `/api/v1/update` API 进行，而不是从云端拉取。

### 5. 安装

首先提供软件包。在 `jooan-w3u-local-firmware-0.2.21` 文件夹中打开一个终端，运行以下命令（绑定 443 端口需要相应权限；在 Linux 上请使用 `sudo`）：

```sh
sudo python3 tools/serve_ota.py --pkg ../release/JOOAN_FW_PKG
```

它会输出 `[ready] https://0.0.0.0:443/fw/JOOAN_FW_PKG`，并为 `use1api.jooaniot.com` 生成一个临时证书。请让它保持运行。按照上一节的说明设置 DNS 重定向，使摄像机能够访问这台服务器。

然后，在同一文件夹中打开第二个终端，触发摄像机更新：

```sh
python3 tools/upload_ota.py --host CAMERA_IP --pkg ../release/JOOAN_FW_PKG
```

该工具会检查软件包的签名，触发摄像机的厂商更新程序（它会从您在上面启动的服务器下载软件包），然后等待摄像机以新固件重启。摄像机拉取软件包时，提供软件包的终端会输出 `[served] /fw/JOOAN_FW_PKG -> ... bytes`。整个过程预计需要一到五分钟。请务必保持摄像机通电。

**成功标志**：最后一行为：

```text
[+] jooan-local is active
```

如果失败：

| 消息 | 含义及处理方法 |
|---|---|
| `error: signed release target/ABI contract does not match this repository` | 您运行的是另一个版本的源代码。请使用第 4 步中的 v0.2.21 zip 包 |
| `... requires the Python 'cryptography' package` | 按照第 4 步安装该软件包 |
| `[-] unexpected OEM response HTTP ...` | 摄像机的厂商更新程序拒绝了触发请求。请就此停止 |
| 提供软件包的终端始终没有输出 `[served] ...` | 摄像机无法访问您的服务器。原因几乎总是 DNS 重定向：请确认**从摄像机一侧**看，`use1api.jooaniot.com` 确实解析到了您的电脑，并且摄像机的 53 端口流量已被重定向（它会忽略 DHCP 下发的 DNS）。另外请检查电脑上的 443 端口是否开放 |
| `[-] package upload completed but jooan-local did not become healthy` | 摄像机已接收软件包，但本固件没有启动。最可能的原因是安装程序发现厂商文件不一致，在写入任何内容之前就拒绝了该摄像机。摄像机仍保留其厂商固件。请就此停止，并参阅[出现问题时](#if-something-goes-wrong) |
| 完全没有响应 | 检查 `CAMERA_IP` 是否正确，以及电脑和摄像机是否位于同一网络 |

### 6. 登录

在浏览器中打开 `http://CAMERA_IP/` 并登录：

```text
username: admin
password: admin
```

在“系统”（System）部分修改之前，密码一直是 `admin`。SSH 和 RTSP 也使用同一密码，修改它会同时更改这三处的密码。

**成功标志**：`http://CAMERA_IP/api/v1/setup/status` 显示 `"release_version":"0.2.21"`。

<a id="7-back-up-the-camera-over-ssh"></a>
### 7. 通过 SSH 备份摄像机

推荐执行。这一步无需额外硬件，即可把每个闪存分区的副本保存到您的电脑上。如果以后的某次更新出了问题，只有这份副本能让这台摄像机恢复原状：其中包含的厂商文件在任何地方都没有公开。要恢复一台已无法启动的摄像机，还需要闪存编程器（参见[恢复](./RECOVERY.md#external-spi-recovery)）。

摄像机共有七个分区，即 `mtd0` 到 `mtd6`。以下命令会列出这些分区：

```sh
ssh admin@CAMERA_IP cat /proc/mtd
```

然后逐一保存每个分区。依次运行每条命令，并在提示时输入密码。在 Windows 上，请使用命令提示符，而不是 PowerShell。Windows PowerShell 的 `>` 会损坏二进制文件。

```sh
ssh admin@CAMERA_IP "cat /dev/mtdblock0" > camera-mtd0.bin
ssh admin@CAMERA_IP "cat /dev/mtdblock1" > camera-mtd1.bin
ssh admin@CAMERA_IP "cat /dev/mtdblock2" > camera-mtd2.bin
ssh admin@CAMERA_IP "cat /dev/mtdblock3" > camera-mtd3.bin
ssh admin@CAMERA_IP "cat /dev/mtdblock4" > camera-mtd4.bin
ssh admin@CAMERA_IP "cat /dev/mtdblock5" > camera-mtd5.bin
ssh admin@CAMERA_IP "cat /dev/mtdblock6" > camera-mtd6.bin
```

**成功标志**：七个文件的大小合计为 8388608 字节（8 MiB），并且对于 `camera-mtd4.bin`（应用分区），摄像机上的 MD5 校验值：

```sh
ssh admin@CAMERA_IP md5sum /dev/mtdblock4
```

与电脑上的一致：`md5sum camera-mtd4.bin`（Linux）、`md5 camera-mtd4.bin`（macOS）或 `certutil -hashfile camera-mtd4.bin MD5`（Windows）。

这些文件包含这台摄像机的身份信息、Wi-Fi 密码和密钥。请妥善保管，切勿与他人分享，也切勿将其写入另一台摄像机。

<a id="8-use-the-camera"></a>
### 8. 使用摄像机

使用 v0.2.21 时：

- **视频**：RTSP，端口 554，用户名 `admin`，密码为管理员密码。`rtsp://CAMERA_IP:554/live/ch0` 对应云台上的镜头，`rtsp://CAMERA_IP:554/live/ch1` 对应固定镜头。
- **云台控制和预置位**：ONVIF，地址为 `CAMERA_IP`，端口 80，用户名和密码同上。
- **SSH**：`ssh admin@CAMERA_IP`，密码同上。
- **Wi-Fi**：摄像机会一直使用 App 设置的网络。如需切换网络，请在 Web 控制台的“网络 → Wi-Fi”（Network → Wi-Fi）中输入新网络并暂存，待摄像机重新连接后再提交。未提交的更改会被回滚。
- **时钟**：摄像机没有电池供电的时钟。断电之后，请在“系统 → 摄像机时间”（System → Camera time）中点击“Sync time & zone to this device”（与此设备同步时间和时区）。

v0.2.21 的文档介绍了这些功能。请阅读该标签对应的版本：[README](https://github.com/ADCDS/jooan-w3u-local-firmware/blob/v0.2.21/README.md) 和 [API](https://github.com/ADCDS/jooan-w3u-local-firmware/blob/v0.2.21/docs/API.md)。`main` 分支上的文档描述的是 0.2.40。

<a id="updating-later"></a>
## 后续更新

通过 Web 控制台安装较新的已发布版本。在“固件”（Firmware）部分选择该版本的 `JOOAN_FW_PKG`，等待显示“signature verified”（签名已验证）后再应用。摄像机会重启，并离线约 90 秒。只有经本项目签名、且比当前已安装版本更新的版本才会被接受。

**除非您打算紧接着安装媒体层，否则不要将运行 v0.2.21 的摄像机更新到 0.2.22 或更高版本**。从 0.2.22 起，平台层本身不再提供视频，因此在安装媒体层之前，摄像机将没有视频。

0.2.21 之后的版本只响应通过私有 IP 地址或摄像机自身名称访问的请求。除非您在“网络 → 本地名称”（Network → Local name）中修改过，否则该名称为 `jooan-w3u`。名称可以单独使用，也可以以 `.local`、`.lan`、`.home.arpa` 或 `.internal` 结尾，例如 `jooan-w3u.local` 或 `jooan-w3u.lan`。使用其他任何名称都会收到 `403 host_rejected`（参见[网络安全](../../../docs/NETWORK-SECURITY.md#host-and-origin-checks)）。

## 卸载

在“固件”（Firmware）部分，应用您所安装版本附带的 `JOOAN_UNINSTALL` 文件。重启后，摄像机将再次以不受限制的方式运行其厂商固件：厂商的 Web 服务器会恢复运行，而且除非路由器仍在屏蔽，否则摄像机将可以再次访问云端。摄像机会保留厂商自身的 Wi-Fi 设置。平台层的设置（包括在平台层中提交的 Wi-Fi 网络）将被删除。详情请参阅[恢复](./RECOVERY.md#booting-camera-rollback)。

**如果已安装媒体层，卸载并不会移除它**。它在 `/opt/jooanipc-open` 中的文件会保留下来，其中包括码流密码和 TLS 私钥，因此请先通过 SSH 删除这些文件：`rm -rf /opt/jooanipc-open`。应用分区也仍是精简后的版本，不包含厂商的语音提示和其他传感器的驱动程序。**切勿尝试在运行中的摄像机上写回原分区**：从该分区运行的程序会随着分区被擦除而崩溃，而一旦在写入完成之前发生复位，就只能借助闪存编程器才能恢复。要恢复原始分区，需要闪存编程器以及您保存的 `my-appfs.img`。

<a id="if-something-goes-wrong"></a>
## 出现问题时

- **完成第 5 步后，摄像机启动的仍是厂商固件**。如果安装程序拒绝了该摄像机，它不会写入任何内容，摄像机保持原样。只要解除路由器上的屏蔽，您就可以重新使用 JOOAN App。
- **更新后 Web 控制台或 SSH 不再响应**。将摄像机断电重启一次，然后等待三分钟。如果仍然没有响应，请按照[恢复](./RECOVERY.md)中的说明操作。
- **摄像机完全无法启动**。只有使用闪存编程器对您的备份进行芯片级还原，才能让它恢复（[恢复](./RECOVERY.md#external-spi-recovery)）。

寻求帮助时，切勿公开发布闪存镜像、密码或密钥（参见[安全策略](../../../SECURITY.md#reporting-a-vulnerability)）。

<a id="the-media-layer"></a>
## 媒体层

从 0.2.22 起，视频、ONVIF 和云台控制改由开源媒体守护进程 [jooanipc-open](../../../media/) 提供，而不再由厂商程序提供。在原生启动（自 0.2.23 起为默认模式）中，厂商程序根本不会启动。本仓库其余部分描述的正是这种配置。

**从原厂状态到开源软件栈的整条流程，现已在一台真实摄像机上完整运行过两次**。一台测试摄像机先被恢复为真正的原厂状态，然后在不使用厂商 App 的情况下完成 Wi-Fi 配网，通过 OTA 更新到平台层，再刷入基于其自身分区构建的媒体镜像。它通过 RTSP 提供了视频，并在开源软件栈上经受住了冷重启。下表列出了第一次运行验证的内容，以及仍然薄弱的环节：

| 步骤 | 验证内容 |
|---|---|
| 恢复为原厂状态 | 测试摄像机被刷回原厂分区（刷写期间持续喂看门狗，并在 RAM 中保留回滚副本），随后以真正的、未配网的原厂状态启动 |
| 免 App 配网 | 在附近的一台电脑上，通过摄像机的 SoftAP 发送 `GET /goform/setupWifiInfo`，使其接入了 Wi-Fi；摄像机报告成功并加入了网络 |
| 通过 OTA 安装平台层 | 将云端重定向到本地服务器后，摄像机拉取并应用了已签名的平台软件包，并成功启动 |
| 镜像 | 基于真实的原厂分区构建；其中的文件与一个已知良好的媒体镜像逐字节相同 |
| 配置 | 安装程序写入的内容通过了发布版守护进程自身的检查 |
| 安装程序 | 其决策、执行顺序和预检均已分别在模拟环境和真实摄像机上验证；在平台层和 Wi-Fi 就绪之前，它都正确地拒绝了安装 |
| 刷写脚本 `first-appfs.sh` | 已实际运行：它刷入了媒体镜像，保留了 RAM 中的回滚副本，开源守护进程完成了原生启动的合格认定，并接管了看门狗 |
| 重启后保持 | 完全断电重启后，摄像机重新运行开源媒体镜像并提供 RTSP 视频，没有回滚到原厂固件 |
| 从原厂状态安装 0.2.39 | 已在本次运行中通过同一更新程序完成 |

这次运行中发现的两处小问题已在 v0.2.40 中修复，随后又使用该版本从原厂状态出发，将整条流程完整运行了第二次：

- 现在可以通过平台层提交**开放**（无密码）Wi-Fi 网络（以 `key_mgmt NONE` 方式加入）；WPA2 网络的使用方式与以前相同。
- 刚安装的平台层可能会让 384 KiB 的 `/opt` 在首次安装媒体层时空间略显不足。现在，当空间不足、且存在可用于恢复的原厂副本时，安装程序会自动删除可重新生成的原厂文件 `/opt/conf/config.org` 以腾出空间（原厂启动流程会从 `/mnt/mtd/etc` 重新创建该文件）。

如果刷写后的回读校验失败，脚本会利用 RAM 中的副本恢复该分区。**如果在写入过程中断电，只有闪存编程器加上该分区的备份才能救回摄像机**（参见[恢复](./RECOVERY.md#appfs-reflash-recovery)）。刷写过程现已在一台真实摄像机上、在两次完整流程中成功运行过数次，但仅限这一台设备；请务必先对每个分区进行完整备份（安装程序会执行这一操作），并在刷写所需的几分钟内确保摄像机供电稳定。

您需要一台运行 Linux 或 WSL 的电脑，并装有 `squashfs-tools`、Python 3、OpenSSH 和 OpenSSL，此外还需要发布页面上的 v0.2.40 文件。

1. **安装平台层**。使用 v0.2.40 的源代码和文件执行第 4 步和第 5 步；或者从 v0.2.21 按照[后续更新](#updating-later)中的方法进行更新。从现在起直到最后一步完成，摄像机都没有视频。Web 控制台仍位于 `http://CAMERA_IP/`。
2. **备份摄像机**（[第 7 步](#7-back-up-the-camera-over-ssh)）。
3. **提交摄像机的 Wi-Fi 网络**：在 Web 控制台的“网络 → Wi-Fi”（Network → Wi-Fi）中提交，即使它就是摄像机当前连接的网络也要提交。原生启动只使用在此处提交的网络，而不会使用 JOOAN App 设置的网络。
4. **解压** `jooanipc-open-*.tar.gz` 和 `libimp-*.tar.gz`，使解压出的两个文件夹并列存放，然后在 `jooanipc-open-*` 文件夹中打开一个终端。
5. **复制摄像机的应用分区，并基于它构建您的镜像**。两者都包含厂商文件，因此请妥善保管，切勿公开：

   ```sh
   ssh admin@CAMERA_IP 'cat /dev/mtdblock4' > my-appfs.img
   sh media/tools/build-appfs.sh --first my-appfs.img bin/jooanipc-open ../libimp-*/libimp.so bin/libatomic.so.1 my-bundle
   ```

   如果分区缺少所需的文件，或者其中的厂商程序与平台层锁定的版本不同，该脚本会拒绝处理。

6. **检查摄像机**。这一步不会做任何更改：

   ```sh
   CAMERA_PASSWORD=admin python3 media/scripts/install-camera.py my-bundle --camera CAMERA_IP
   ```

7. **安装**，同时为摄像机的视频码流设置密码：

   ```sh
   CAMERA_PASSWORD=admin STREAM_PASSWORD=your-choice python3 media/scripts/install-camera.py my-bundle --camera CAMERA_IP --yes-untested
   ```

   请务必保持摄像机通电。**成功标志**：最后一行显示该版本正在运行。管理界面现在位于 `https://CAMERA_IP/`，使用的是摄像机自己的证书，因此浏览器会警告一次。请使用管理员密码登录。`http://CAMERA_IP/` 现在只会重定向到该地址。

启动过程会自我检查：如果某次原生启动记录了故障，或原生启动连续三次被中途打断，下一次启动就会回退到厂商程序。在原生启动首次通过合格认定之前，如果启动 5 分钟后仍没有网关，也会同样回退（参见[恢复](./RECOVERY.md#a-media-change-that-stops-native-boot)）。

**在 Windows 上**，请在 WSL 中构建发布包，然后在命令提示符中用 `py` 运行安装程序，并用 `set` 设置每个密码。安装程序使用 Windows 自带的 OpenSSH，并要求能在 PATH 中找到 `openssl`（Git for Windows 已包含它）。OpenSSH 8.4 或更高版本（Windows 11）会从 `CAMERA_PASSWORD` 读取密码；Windows 10 自带的版本则会在每次连接时询问密码，因此请先在 Web 控制台中添加 SSH 密钥。这一流程同样尚未在 Windows 上运行过。

**后续的媒体层更新**以摄像机当前的分区为基础，使用不带 `--first` 的 `build-appfs.sh`，并配合您为其编写的 `bundle.json`（参见[摄像机发布包](../../../media/docs/hardware.md#camera-release-bundles)）。此时安装程序还需要 `MEDIA_PASSWORD`（即码流密码）来验证结果。管理界面中“系统 → 媒体”（System → Media）下的表单只接受使用本项目发布密钥签名的软件包。每次媒体层更新，无论是通过安装程序还是通过该表单进行，都会像首次安装一样重写同一个分区，因此上文关于断电的警告同样适用，主板自身发生的复位也不例外。由于镜像中含有厂商文件，本项目不发布任何镜像，因此该表单仅供密钥持有者，或使用自己的密钥构建平台层的人使用。

**SD 卡**：在媒体层的后备启动模式下，可以从 microSD 卡而非应用分区运行守护进程。只有厂商程序会挂载 SD 卡，因此这种方式仅在该后备模式下有效。它并不能让您绕过上述任何步骤。

发布包的格式请参阅[摄像机发布包](../../../media/docs/hardware.md#camera-release-bundles)。
