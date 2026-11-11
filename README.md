# HDMI Display

Linux 多屏异显：每个 HDMI/DP 接口一条独立 DRM/KMS 管线，可显示视频、图片、
VNC/虚拟机控制台、Windows RDP、网页、画中画、仪表盘或测试图。无桌面环境也能运行，
通过内置网页后台统一管理。

目标平台：fnOS / Debian 12、x86_64、DRM/KMS。

## 从源码编译

`build-kit/` 提供可移植的编译环境；换电脑后可直接编译出 fpk：

```bash
# Docker（Windows 用 build-kit\build-in-docker.ps1）
bash build-kit/build-in-docker.sh

# 或在 Debian 12 / fnOS 上原生编译
sudo bash build-kit/setup-debian.sh
bash packaging/build-fpk.sh
```

Docker 镜像包含 Debian 12、FreeRDP 3、最小 FFmpeg 5.1.8 和 fnpack；
详细说明、离线编译和依赖版本见 `build-kit/README.md`。

## 架构

```
media/image/vnc/rdp/web/pip
        │
  每屏独立解码/采集线程
        │
  XRGB 画布（draw.c 绘制 UI/HUD/画中画）
        │
  DRM/KMS atomic: connector → CRTC → plane
```

- 每块屏一个线程、一个解码器/采集器、一个画布，互不影响。
- 视频用最小化 FFmpeg（VAAPI 硬解，软件解码回退）。
- RDP 用原生 libfreerdp，直接解码到 DRM 帧缓冲；音频走对应 HDMI PCM。
- 网页源用 Xvfb + Chromium，x11grab 采集；键鼠用 XTEST 注入。
- 看门狗：长时间无出帧自动重开源；进程由 systemd 拉起。

## 源码

| 文件 | 作用 |
|---|---|
| `main.c` | 多屏调度、UI/HUD、画中画、网页状态回调 |
| `drm_display.c` | DRM/KMS atomic 显示后端 |
| `video_dec.c` | FFmpeg 拉流/解码（VAAPI） |
| `rdp_source.c` | 原生 FreeRDP 客户端、键鼠/光标 |
| `rdp_audio.c` | 私有 rdpsnd 后端 → HDMI PCM |
| `vnc_source.c` | VNC/RFB 客户端（虚拟机控制台） |
| `web_source.c` | 网页源（Xvfb + Chromium + x11grab + XTEST） |
| `input_router.c` | 物理键鼠 → 屏幕（evdev） |
| `web_server.c` | HTTP、MJPEG 预览、静态控制台、fnOS 网关 |
| `draw.c` / `font8x16.c` / `font_cjk.c` | 软件绘图、ASCII + Noto Sans CJK SC GB2312 点阵字库 |
| `alsa_out.c` | ALSA 输出、HDMI PCM、IEC958 |
| `config.c` | 配置读写 |
| `os_thread.c` / `platform.c` | 线程封装、系统信息/单实例 |

## 依赖与编译

应用本身：

```bash
sudo apt install build-essential pkg-config libdrm-dev \
    freerdp3-dev winpr3-dev libssl-dev libx11-dev libxtst-dev
```

最小化 FFmpeg（只包含实际用到的解码器/解复用器/协议、x11grab、MJPEG 预览编码器）：

```bash
sudo apt install nasm libva-dev libdrm-dev libx11-dev libxext-dev \
    libxfixes-dev libxv-dev libxcb1-dev libxcb-shm0-dev \
    libxcb-shape0-dev libxcb-xfixes0-dev libssl-dev zlib1g-dev
packaging/build-ffmpeg-min.sh          # -> /opt/ffmpeg-min
```

编译：

```bash
make FFMPEG_PREFIX=/opt/ffmpeg-min     # 推荐（最小 FFmpeg）
# 或 make                             # 使用系统 FFmpeg 5.x
```

运行时可把库目录加入 `LD_LIBRARY_PATH`：

```bash
LD_LIBRARY_PATH=/opt/ffmpeg-min/lib sudo -E ./hdmi_display --list
```

## 内容源

| 类型 | 参数 | 说明 |
|---|---|---|
| media | `rtsp://...`、`http(s)://...`、`/path/a.mp4` | 视频/流/文件 |
| image | `/path/a.jpg` 或 URL | 静态图片 |
| vnc | `host:5900` 或 `/var/run/vms/x.vnc.sock` | VNC 控制台 |
| rdp | `用户:密码@主机[:端口]` | 微软 RDP，默认 `+admin` 控制台 |
| web | `https://example.com` | 网页/看板（Xvfb + Chromium） |
| pip | `media:URL\|vnc:HOST:PORT\|x,y,w,h` | 画中画 |
| ui | 空 | 仪表盘 |
| bars | 空 | 测试图 |

- 每个输出启动时显示测试图，之后在面板里选源。
- RDP 地址后加 `?normal` 可改用独立会话；默认连控制台会话。
- 画中画主层支持 RDP/VNC/网页/视频/图片；叠加层支持 VNC/网页/视频/图片。
- 画中画包含 VNC/网页/RDP 层时，输入设备可以绑定到它，鼠标坐标映射到小窗。
- 网页源需要主机安装 `xvfb` 和 `chromium`（或 `google-chrome`）；安装/配置向导
  可以选择一键通过 apt 安装（fnOS 应用中心不提供这两个包）。

## fnOS 生命周期

严格按 fnOS 的按钮语义：

- **安装完成 = 打开**：应用立即运行，App Center 显示“打开”。
- **用户点停止 = 启用**：执行 `stop + disable`，应用停止，后台不再运行，
  也不会开机自启；App Center 显示“启用”。
- **点启用 = 打开**：执行 `enable + start`，应用运行；App Center 显示“打开”。
- **更新 / 改配置保持原状态**：原来显示“打开”就继续运行（重启服务），
  原来显示“启用”就保持停止；只有用户点“停止”才会切换到“启用”。
- **设置持久化**：安装/配置向导保存端口和 appliance-compositor 停止策略，
  App Center 升级不会把它们重置为默认值；配置向导的“保持当前设置”用于只改
  端口而不动合成器策略。
- **卸载**：删除 systemd unit、drop-in、socket、runtime 目录、Chromium 临时
  profile；卸载向导默认同时删除配置、应用数据和 app 目录（可选择保留配置/媒体）。

## 网页后台

- 直接入口：`http://<NAS>:8080/`
- fnOS 网关：`app/ui/config` 使用 iframe + Unix socket `app.sock` +
  前缀 `/app/hdmi-display`，仅管理员可见；8080 被占用不影响网关。
- 每块屏一张卡片：状态、分辨率、预览、源类型/参数、应用/停用。
- 远程连接只有一个“远程连接”选项，后台根据地址自动判断协议：
  `用户:密码@主机[:端口]`（含 `@`）按 RDP；`主机:端口`、本地 socket
  `/var/run/vms/xxx.vnc.sock` 或普通路径按 VNC。
- 自动扫描本机 `/var/run/vms/*.vnc.sock`，一个或多个虚拟机都会出现在
  “本机 VNC 虚拟机”下拉框中，直接选择即可；也可以手动填写远程 VNC 地址。
- VNC 键盘会跟踪 Shift/Caps Lock 并发送对应的大小写和 Shift 符号，避免远端
  只收到小写字母。
- 切换源类型时自动填入该屏幕、该方式上一次成功使用的地址；未使用过的类型只显示
  输入提示，不会被旧内容或自动刷新覆盖。
- 输入设备区在输出卡片下方，使用说明位于页面底部；只显示可绑定键鼠的屏幕：
  整屏 RDP/VNC/网页，或包含这些层的画中画。
- 绑定保存到 `hdmi_display.conf`，重启恢复。

### API

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/api/state` | JSON（outputs、inputs） |
| GET | `/api/apply?out=NAME&type=T&arg=..&w=..&h=..` | 启用/切换源 |
| GET | `/api/disable?out=NAME` | 停用 |
| GET | `/api/input/bind?id=DEV&out=NAME` | 绑定键鼠（空 out = 解绑） |
| GET | `/api/quit` | 退出 |
| GET | `/preview/NAME.mjpg` | MJPEG 预览 |
| GET | `/snapshot/NAME.jpg` | 单帧 JPEG |

## 屏幕 UI

- 视频/流、图片、画中画正常出画 4 秒后，顶部 `LIVE VIEW / REC` 和底部状态条
  自动隐藏；重新应用源时再显示 4 秒。
- HUD 与等待页支持中文（内嵌 Noto Sans CJK SC 16x16 GB2312 点阵字库，SIL OFL 1.1）；生僻字/繁体/emoji 显示为 `?`。
- RDP/网页/VNC 镜像不叠加视频 HUD。

## 键鼠绑定

- 读取 `/dev/input/by-id/*` 或 `/dev/input/event*`，需要 root 或 `input` 组。
- 整屏 RDP/VNC/网页：事件注入对应会话。
- 画中画：鼠标按小窗坐标映射；点小窗外/黑边不误触；键盘跟随最后进入的层。
- 绑定写入配置，重启恢复。

## 性能与网页依赖

- RDP 客户端固定使用 LAN + AVC444 + FastPath + 软件 GDI，保留 Windows 的主题、
  动画、壁纸和字体平滑，观感与物理机一致。
- 该 Windows 服务端实测不接受 AVC444v2 和额外缓存组合（开启后会立即断线），
  因此没有启用；RDP 的实际上限主要取决于 Windows 主机/虚拟机的 H.264 编码
  CPU/GPU，而不是客户端参数。
- 建议：给 Windows VM 足够的 vCPU/GPU、保持 1080p、确认
  `DWMFRAMEINTERVAL=15`；这些比客户端调参收益更大。
- 网页源需要 `xvfb` + `chromium`；安装/配置向导提供一键 apt 安装（约 300 MB）。
  fnOS 应用中心不提供这两个包。

## 打包 fpk

```bash
# 安装 fygopack（下载地址见 packaging/README.md）
packaging/build-fpk.sh
# -> build/hdmi-display.fpk
```

- 默认 `BUNDLE_LIBS=1`：把最小 FFmpeg、FreeRDP、OpenSSL、ALSA、libdrm、
  X11/XCB 及其依赖闭包打进 `app/lib`，不依赖目标机安装 FFmpeg/FreeRDP。
- 最小 FFmpeg 缺失时打包脚本会自动调用 `packaging/build-ffmpeg-min.sh`。
- 目标机只需：x86_64 Linux（glibc 基础库）、DRM/KMS；网页源额外需要
  `xvfb` + `chromium`。
- fnOS 上通过 App Center 手动安装 fpk；详细说明见 `packaging/README.md`。

## 配置

`hdmi_display.conf`：

```
web_port=8080
audio=auto
output card1-HDMI-A-3 rdp 1920x1080 Administrator:password@10.0.0.5:3389
output card1-HDMI-A-4 web 1920x1080 https://example.com
output card0-HDMI-A-1 pip 1920x1080 media:/movies/a.mp4|vnc:127.0.0.1:5900
input /dev/input/by-id/usb-...-event-kbd card1-HDMI-A-3
```

- RDP 密码以明文保存，仅建议可信内网使用。
- 面板本身无登录；需要时用 `--web-token TOK` 或防火墙/VPN。
- DRM master 只能被一个进程占用；与 fnOS 的 appliance-compositor 冲突时，
  使用安装向导里的“停止 appliance-compositor”选项。

## 安全

- 应用以 root 运行（DRM master、/dev/input、ALSA）。
- Chromium 以 `--no-sandbox` 运行在 Xvfb 中，只加载可信网页。
- 面板是管理员工具，不要直接暴露到公网。
- 详见 [SECURITY.md](SECURITY.md)。

## 许可证

本项目使用 **GPL-3.0-or-later**，见 [LICENSE](LICENSE)。

- 中文字库：Noto Sans CJK SC，SIL OFL 1.1，见 `packaging/fonts/OFL-Noto.txt`
- 第三方组件与许可证：见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)
- FFmpeg 使用 LGPL-2.1-or-later 的最小构建，库文件在 `app/lib`，可替换
