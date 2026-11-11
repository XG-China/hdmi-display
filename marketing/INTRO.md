# 一台 NAS，接几块屏，就是几台独立电脑

> HDMI Display 图文介绍 · 多屏独立输出 / RDP / VNC / 网页 / 视频 / 画中画

![HDMI Display 主视觉](poster-1.png)

## 一句话介绍

HDMI Display 把飞牛 fnOS / Debian NAS 变成一台多屏主机：

- 每个 HDMI/DP 输出接口独立显示不同内容
- 视频、图片、Windows RDP、VNC 虚拟机、网页、画中画各占一屏
- 每块屏可以配一套键盘鼠标
- 全部通过中文网页后台管理，不需要桌面环境

**不是镜像，不是窗口铺满，而是每个接口一条独立的 DRM/KMS 输出通道。**

## 功能一：每个视频输出口都是独立通道

![按接口扩展](poster-3.png)

- 机器有几个可用 HDMI/DP 接口，就能带几块屏
- 每块屏独立选择信号源，也可以手动让多块屏显示同一个源
- 支持多张显卡 / 多卡输出
- 实际数量取决于 GPU 接口、CRTC、带宽和分辨率/刷新率

## 功能二：一个网页后台管理所有屏幕

![Web 管理面板](panel-demo.png)

- 每块屏一张卡片：状态、分辨率、预览、信号源、应用/停用
- 支持实时 MJPEG 预览和单帧截图
- 切换源类型时自动填入该屏幕、该方式上一次成功使用的地址
- 输入设备区只显示可绑定键鼠的屏幕
- 支持 fnOS 网关和直接 HTTP 端口访问

## 功能三：远程桌面 RDP / VNC 自动识别

后台不再要求手动选 RDP 或 VNC，直接填地址：

- `用户:密码@主机[:端口]` → 自动按 **RDP**
- `主机:端口` → 自动按 **VNC**
- `/var/run/vms/xxx.vnc.sock` → 自动按 **VNC**

本机虚拟机自动扫描：

- 扫描 `/var/run/vms/*.vnc.sock`
- 一台或多台虚拟机出现在“本机 VNC 虚拟机”下拉框
- 选择后自动填入 socket 路径
- 也可以手动填写远程 VNC 地址

RDP 默认连接控制台会话（`+admin`）；地址后加 `?normal` 改用独立会话。

## 功能四：一屏一套键鼠，多套互不干扰

![多席位办公](poster-2.png)

- 每套键盘/鼠标可以绑定到不同输出
- 适合“一台 NAS + 多台 Windows 虚拟机”的小型轻办公
- 画中画按小窗坐标映射，点小窗外不会误触
- 键盘跟随指针最后进入的层
- 设备通过 `/dev/input/by-id/*` 或 `/dev/input/event*` 读取

## 功能五：视频、图片、本机画面、网页

- **视频/直播**：RTSP、HTTP、HLS、本地文件
- **图片**：本地路径或 URL
- **本机画面**：X11 桌面或 `/dev/fb0` 控制台
- **网页**：Xvfb + Chromium，适合看板、监控、网页系统
- **画中画**：主画面 + 小窗叠加
- **仪表盘 / 测试图**：本机绘制，不依赖外部源

网页源需要额外安装：

```bash
sudo apt install -y --no-install-recommends xvfb chromium
```

视频、图片、RDP、VNC、画中画、本机画面不需要额外依赖。

## 功能六：中文 HUD 和等待界面

![中文 HUD](hud-demo-early.jpg)

- 视频/图片/画中画启动后显示 HUD，约 4 秒自动隐藏
- 中文路径正常显示，使用 Noto Sans CJK SC 16×16 点阵
- 生僻字/繁体/emoji 显示为 `?`
- RDP/网页/VNC 镜像不叠加视频 HUD

## 功能七：fnOS 生命周期

- 安装完成 = **打开**，应用立即运行
- 用户点停止 = **启用**，执行 `stop + disable`，后台不运行
- 点启用 = **打开**，执行 `enable + start`
- 更新 / 改配置保持原状态：原来运行就重启，原来停止就保持停止
- 卸载删除 unit、drop-in、socket、runtime、Chromium 临时 profile；
  默认同时删除配置、应用数据和 app 目录

## 适合谁

- 有 NAS、有多块显示器，想统一管理多路输出
- 想用一台 NAS 跑多个 Windows 11 虚拟机做轻办公
- 家庭实验室：多台 VM、多屏、多套键鼠
- 数字标牌、监控、网页看板
- 需要 RDP/VNC 多屏控制台

## 不适合谁

- 视频剪辑、CAD、3D、游戏等高负载场景
- 对单点故障零容忍的生产办公
- 没有多视频输出接口的 NAS
- 需要 HDR/高刷/多路 4K 的极端场景（取决于硬件）

## 多虚拟机轻办公示例

```text
NAS
 ├─ Win11 VM 1 → HDMI 1 → 显示器 1 + 键鼠 1
 ├─ Win11 VM 2 → HDMI 2 → 显示器 2 + 键鼠 2
 ├─ Win11 VM 3 → HDMI 3 → 显示器 3 + 键鼠 3
 └─ 网页/监控 → HDMI 4
```

建议：

- Win11 虚拟机使用 UEFI + TPM（swtpm）
- 每台 VM 建议 8–12 GB 内存
- 3 台 VM 建议 32–48 GB 主机内存
- 虚拟机磁盘放 SSD/NVMe
- 正式办公请确认 Windows 授权

## 获取与安装

- GitHub：<https://github.com/XG-China/hdmi-display>
- Releases：<https://github.com/XG-China/hdmi-display/releases>

安装：

1. 下载 `hdmi-display_<version>.fpk`
2. 在 fnOS App Center 手动安装
3. 打开 `http://<NAS>:8080/`
4. 给每块屏选择信号源

## 安全提示

- 应用以 root 运行（DRM master、`/dev/input`、ALSA）
- RDP 密码明文保存在本机配置，仅建议可信内网
- VNC 目前只支持无认证
- 面板不要直接暴露公网，建议防火墙/VPN
- Chromium 以 `--no-sandbox` 运行，只加载可信网页

## 许可证

GPL-3.0-or-later。第三方组件见
[THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md)。
