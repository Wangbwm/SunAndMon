# SunAndMon

SunAndMon 把 Moonlight Qt 控制端与 Foundation Sunshine 被控引擎放进同一个 Windows x64 便携包。解压后运行 `SunAndMon.exe`，从同一个窗口连接其他设备或开启本机被控。Android 版 Moonlight VPlus 可以使用同一个中心连接电脑。

## 启动中心

中心是单进程 Go 服务。生成 32 字节随机密钥并编码成小写十六进制，将它保存在部署环境的 `SUNANDMON_CENTER_KEY` 中。同一个密钥的持有者可查看该中心的设备并申请配对，只应发给允许控制这些设备的人。

```sh
cd center
export SUNANDMON_CENTER_KEY="$(openssl rand -hex 32)"
go run . -listen 127.0.0.1:8787 -state state/devices.json
```

跨设备访问时，可在服务前配置 HTTPS 反向代理并将请求转发到 `127.0.0.1:8787`，也可用 `-listen :8787 -tls-cert fullchain.pem -tls-key privkey.pem` 让中心直接提供 HTTPS。客户端使用系统信任的 HTTPS 证书。本机调试可用 `http://127.0.0.1:8787`。

也可以直接运行 `dist` 中对应平台的中心服务二进制文件。Windows x64 使用 `sunandmon-center.exe`，Linux x64 使用 `sunandmon-center-linux-amd64`。

### 节点管理

在中心服务的部署环境添加独立的管理员密钥 `SUNANDMON_CENTER_ADMIN_KEY`，可用 `openssl rand -hex 32` 生成。重启中心后，打开中心网址下的 `/admin/`，或在 Windows 客户端打开「中心服务器 → 管理节点」。使用反向代理时，同一中心的 `/v1/` 和 `/admin/` 都需要转发到中心服务。

管理页列出所有登记记录，包括普通设备列表隐藏的重复记录。可修改节点名称、连接 IP 和 HTTP/HTTPS 端口。字段留空时跟随主机上报。删除节点会从中心列表移除该记录，并拒绝原设备 ID 再次登记，直到管理员选择「允许重新登记」。删除前已完成的设备配对可在对应主机上撤销。

中心将 IP、端口、公钥证书和用于签发配对票据的被控端密钥存入状态文件。该文件应由服务账号独占。每个被控端单独拥有登记密钥。Windows 一体包用当前用户的 Windows 数据保护机制加密保存中心登录密钥。退出中心会清除保存的密钥，运行中的被控引擎会重启并继续提供局域网连接。需要撤销之前已完成的配对时，可在该电脑的「主机设置」内禁用对应控制端。

## 构建并使用客户端

首次克隆后，在项目根目录运行 `git submodule update --init --recursive` 初始化上游依赖。

1. 按各仓库自带的构建流程准备修改后的 [Moonlight Qt](https://github.com/qiin2333/moonlight-qt) 与 [Foundation Sunshine](https://github.com/AlkaidLab/foundation-sunshine) 的 Windows x64 构建依赖。控制端便携构建调用 qmake 时追加 `CONFIG+=sunandmon`，部署生成的 `SunAndMon.exe`、剪贴板助手和运行库。
2. 在工作区运行 `python scripts/package-windows.py Moonlight-VPlus-Portable-x64.zip Sunshine-Portable-x64.zip SunAndMon-Windows-x64.zip`。解压到当前用户可写目录，保留内部 `host` 目录。
3. 两台电脑打开 `SunAndMon.exe` 后都会启动本机被控。从左下角「中心服务器」填写相同的 HTTPS URL 和密钥。登录成功后，被控端会自动登记。在设备页选中上线的远端电脑并点「连接桌面」即可由中心授权，无需输入 PIN。同一页可打开应用列表、显示器与虚拟屏、设备操作及串流设置。需要登录 Windows 后自动运行时，在「本机被控」打开「登录 Windows 时自动启动」。
4. Android 端按 [Moonlight VPlus](https://github.com/qiin2333/moonlight-vplus) 的构建流程生成 APK。从「我的设备」打开「中心服务器」登录，再进入在线电脑的详情页。详情页提供桌面连接、应用列表、副屏串流和原设备操作。「局域网设备」可进入原来的设备页。

局域网设备发现、手动添加 PC 和常规 PIN 配对可从「局域网」页继续使用。在「本机被控」开启主机后，从「主机设置」进入 Sunshine 原有控制面板，可配置虚拟显示器、应用、音视频编码和串流端口。控制端会在主机按新配置重启后更新入口地址和中心登记。便携主机的配置保存在解压目录的 `host/config`。

Windows 客户端的「设置 → 软件 → 关闭按钮行为」可选择确认后退出或缩小到托盘。从托盘选择退出时也需要确认。

串流期间可从「控制中心 → 视频和显示」开启或关闭隐私屏。被控端需要新版 SunAndMon、可用的 ZakoVDD 虚拟显示驱动和虚拟音频设备（Steam Streaming Speakers 或 VB-CABLE）。开启时串流会重连，主机切到虚拟显示器，仅通过虚拟音频输出提供串流声音。正常退出串流时恢复原显示布局和应用静音前的输出状态。

中心处理设备发现和配对。音视频和输入走设备间直连。设备需要通过同一局域网、VPN 或已配置的公网端口互相访问。需要优先使用指定网卡地址或外部地址时，可在开启被控前填写「发布 IP」。

中心签发的配对票据期限为一分钟。被控端验票时容忍前后各 30 分钟的时钟偏差，同一张票据在整个接受窗口内只能使用一次。

Windows 客户端可以通过 IP 直连 HTTPS 中心。在「中心服务器」填写证书对应的 HTTPS 网址，并在「直连 IP」填写服务器的数字 IP 地址。客户端直接连接该 IP，并使用网址里的域名校验服务器证书。选项留空时会按网址正常解析。
客户端内的「管理节点」沿用同一连接设置。

## Linux 虚拟显示器

`scripts/linux/virtual-display/` 提供合盖场景下的 1080p 120 Hz 虚拟屏配置。它使用 Intel `i915` 的空闲 `HDMI-A-1`，供桌面和 Sunshine 的 KMS 捕获使用。仅在目标机器具备这个输出且 HDMI 未接显示器时部署。

```sh
cd scripts/linux/virtual-display
sudo install -D -m 0644 virtual-1080p.edid /usr/local/share/sunandmon/virtual-1080p.edid
sudo install -D -m 0755 sunandmon-virtual-display /usr/local/libexec/sunandmon-virtual-display
sudo install -D -m 0644 sunandmon-virtual-display.service /etc/systemd/system/sunandmon-virtual-display.service
sudo systemctl daemon-reload
sudo systemctl enable --now sunandmon-virtual-display.service
```

## 目录

- `foundation-sunshine/` 提供受控引擎、被控进程生命周期和基于 TLS 客户端证书的配对。
- `moonlight-qt/` 提供桌面端设备界面、中心登录、本机被控与串流启动。
- `moonlight-vplus/` 提供 Android 中心设备入口和串流启动。
- `center/` 保存设备注册表、在线状态和短时配对授权。

## 上游基线

- `foundation-sunshine/` 基于 `7f7630f61c5b6db09ca9a6931586bcffa03ce263`。
- `moonlight-qt/` 基于 `490923396a0bc53350dfadf88d5cf39bc9a39362`。
- `moonlight-vplus/` 基于 `484532dc38775b5d2763a4609e622a50b3dc986a`。
