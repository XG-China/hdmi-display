# HDMI Display 代码导读

本文按“从启动到出图”的顺序介绍全部源码、脚本和打包结构。目标是让你能快速回答三个问题：

1. 一块屏从配置到点亮，经过哪些文件？
2. 想加一种信号源、一个 API 或一种显示后端，应该改哪里？
3. fpk 里的二进制、依赖、systemd unit 和生命周期脚本是怎么来的？

> 项目主页和使用说明见 [README.md](../README.md)。
> 功能截图见 [marketing/INTRO.md](../marketing/INTRO.md)。

---

## 1. 总架构

```text
CLI / hdmi_display.conf / Web API
                │
                ▼
        main.c  app_t / screen_t
                │
        ┌───────┴────────┐
        │                │
  每屏 worker 线程     web_server.c
        │                │
        ▼                └─ /api/state /api/apply /preview /snapshot
  source 对象
  ├─ video_dec.c      FFmpeg 视频/图片/本机画面/网页采集
  ├─ vnc_source.c     内置 RFB/VNC 客户端
  ├─ rdp_source.c     libfreerdp RDP 客户端
  └─ web_source.c     Xvfb + Chromium + x11grab
        │
        ▼
  draw.c / font*.c    XRGB8888 画布：HUD、测试图、仪表盘、画中画
        │
        ▼
  drm_display.c       connector → CRTC → primary plane
        │
        ▼
  HDMI / DP 输出
```

几个固定原则：

- **一个输出一个线程**：`screen_t` 持有自己的 `disp_output`、源对象、画布和统计，互不阻塞。
- **只保留最新帧**：视频/RDP/VNC 都使用“最新帧”模型，旧帧直接丢弃，降低延迟。
- **按名字管理输出**：输出名是 `cardN-CONNECTOR`，例如 `card0-HDMI-A-1`，热插拔后仍然稳定。
- **没有桌面依赖**：显示后端直接 DRM/KMS；网页源才需要 Xvfb/Chromium。
- **Web 是唯一管理入口**：CLI 只用于调试、自动分配和直接指定源。

---

## 2. 源码文件总表

### 2.1 核心调度

| 文件 | 行数 | 作用 | 关键函数/结构 |
|---|---:|---|---|
| `main.c` | ~2440 | 程序入口、多屏调度、源生命周期、HUD、画中画、输入路由、看门狗、Web 回调 | `app_t`、`screen_t`、`apply_output()`、`screen_thread()`、`route_input()`、`watchdog_check()` |
| `display.h` | ~70 | 显示后端抽象接口 | `disp_ctx`、`disp_output`、`disp_init()`、`disp_open()`、`disp_present()` |
| `os_thread.c/.h` | ~30 | pthread 线程/互斥量薄封装 | `os_thread_create()`、`os_mutex_*()` |
| `platform.c` | ~90 | POSIX 时钟、睡眠、系统信息、单实例锁 | `disp_now()`、`disp_get_sysinfo()`、`app_singleton()` |

### 2.2 显示与绘制

| 文件 | 行数 | 作用 | 关键函数/结构 |
|---|---:|---|---|
| `drm_display.c` | ~790 | Linux DRM/KMS atomic 后端，多卡、connector/CRTC/plane、dumb buffer、shadow buffer | `disp_init()`、`disp_enumerate()`、`disp_open()`、`disp_present()` |
| `draw.c/.h` | ~290 | 软件绘制：矩形、线、文字、进度条、波形 | `cv_fill()`、`cv_text()`、`cv_bar()`、`cv_wave()` |
| `font8x16.c/.h` | ~100 | 8×16 ASCII 点阵字库 | `font8x16[]` |
| `font_cjk.c/.h` | ~15500 | Noto Sans CJK SC 16×16 GB2312 点阵表（7445 字，238,240 字节） | `font_cjk16[]` |

### 2.3 内容源

| 文件 | 行数 | 作用 | 关键函数/结构 |
|---|---:|---|---|
| `video_dec.c/.h` | ~780 | FFmpeg 解封装/解码/缩放/音频；视频、图片、本机画面、网页采集都走这里 | `video_source_open()`、`video_source_draw()`、`run_session()` |
| `vnc_source.c/.h` | ~710 | 最小 RFB/VNC 客户端：3.3/3.8、None 认证、Raw/CopyRect、DesktopSize | `vnc_source_open()`、`handshake()`、`update_loop()` |
| `rdp_source.c/.h` | ~1070 | 原生 libfreerdp RDP：NLA、AVC444、GFX、光标、键鼠、音频 | `rdp_source_open()`、`rdp_thread()`、`rdp_end_paint()` |
| `web_source.c/.h` | ~410 | Xvfb + Chromium kiosk，XTEST 键鼠注入，临时 profile 清理 | `web_proc_start()`、`web_proc_input()`、`web_proc_stop()` |

### 2.4 输入与音频

| 文件 | 行数 | 作用 | 关键函数/结构 |
|---|---:|---|---|
| `input_router.c/.h` | ~615 | 扫描 `/dev/input/by-id` 和 `/dev/input/event*`，分类、绑定、归一化键鼠事件 | `input_init()`、`input_list()`、`input_bind()`、`linux_reader()` |
| `alsa_out.c/.h` | ~330 | `dlopen("libasound.so.2")` 播放 PCM；按 DRM 卡/连接器/ELD 找 HDMI PCM；开 IEC958 | `alsa_open()`、`alsa_detect_for_drm_conn()`、`alsa_write()` |
| `rdp_audio.c/.h` | ~190 | FreeRDP `rdpsnd` 私有后端，把 RDP 音频写到对应 HDMI PCM | `rdp_audio_install_provider()`、`rdp_audio_make_arg()` |

### 2.5 配置与 Web

| 文件 | 行数 | 作用 | 关键函数/结构 |
|---|---:|---|---|
| `config.c/.h` | ~175 | 行式配置文件读写、原子保存、输入绑定持久化 | `config_load()`、`config_save()`、`config_set()` |
| `web_server.c/.h` | ~970 | 内置 HTTP 服务：静态面板、JSON API、MJPEG 预览、单帧 JPEG、fnOS gateway socket | `web_start()`、`handle_client()`、`serve_mjpeg()` |
| `web/index.html` | ~66 | 中文管理面板结构 | 输出卡片、输入设备、RDP 帮助 |
| `web/app.js` | ~507 | 无框架前端：状态轮询、信号源表单、历史地址、VNC 下拉、输入绑定、预览 | `refresh()`、`render()`、`applyOutput()`、`bindInput()` |
| `web/style.css` | ~227 | 面板样式、深色主题、响应式布局 | — |

### 2.6 构建、打包、生命周期

| 文件 | 作用 |
|---|---|
| `Makefile` | 编译 `hdmi_display`，链接 libdrm/FFmpeg/FreeRDP/OpenSSL/X11/XTST |
| `packaging/build-fpk.sh` | 构建 fpk：编译、拷贝 web、打包依赖闭包、OpenSSL legacy、amixer、RDP 脚本 |
| `packaging/build-ffmpeg-min.sh` | 构建最小 FFmpeg 5.1.8 到 `/opt/ffmpeg-min` |
| `packaging/README.md` | fpk 结构、依赖、安装/卸载/DRM master 说明 |
| `packaging/fpk/hdmi-display/manifest` | App Center 元数据：版本、平台、入口、root 安装 |
| `packaging/fpk/hdmi-display/cmd/common` | systemd unit 生成、启停、状态保存、二进制/依赖检查 |
| `packaging/fpk/hdmi-display/cmd/main` | App Center `start/stop/status` |
| `cmd/install_*`、`upgrade_*`、`config_*`、`uninstall_*` | 安装、升级、改配置、卸载生命周期 |
| `packaging/fpk/hdmi-display/wizard/*` | 中文安装/配置/卸载向导 |
| `packaging/fpk/hdmi-display/config/privilege` | `run-as: root` |
| `packaging/fpk/hdmi-display/config/resource` | 共享目录 `hdmi-display/media` |
| `packaging/fpk/hdmi-display/i18n/*` | App Center 中英文名称/描述 |
| `packaging/rdp/enable-rdp.ps1` | Windows 端开启 RDP、放行防火墙、60 fps、英文键盘 |
| `build-kit/Dockerfile` | Debian 12 + FreeRDP 3 + 最小 FFmpeg + fnpack |
| `build-kit/build-in-docker.sh/.ps1` | Linux/macOS 与 Windows Docker 一键编译 |
| `build-kit/setup-debian.sh` | Debian 12/fnOS 原生依赖安装 |
| `build-kit/make-source-archive.sh` | 生成不含构建产物的源码包 |

### 2.7 文档与许可证

| 文件 | 作用 |
|---|---|
| `README.md` | 项目主页：使用、功能、截图、代码摘要 |
| `marketing/INTRO.md` | 图文介绍：功能、截图、代码架构摘要 |
| `marketing/PROMO.md` | 各平台宣传文案 |
| `SECURITY.md` | 安全模型和部署建议 |
| `CONTRIBUTING.md` | 贡献说明 |
| `THIRD_PARTY_NOTICES.md` | 第三方组件许可证 |
| `LICENSE` | GPL-3.0-or-later |

---

## 3. 启动流程

`main()` 的顺序大致是：

1. 解析命令行：`--list`、`--device`、`--config`、`--web-port`、`--web-bind`、`--web-token`、`--web-dir`、`--web-socket`、`--web-prefix`、`--fps`、`--hwaccel`、`--audio`、`--demo`、`-v`。
2. `app_singleton("hdmi_display")`：用 `flock` 防止重复实例。
3. 初始化配置 `config_init()` / `config_load()`。
4. 扫描 `/dev/dri/card*`，每个卡调用 `disp_init()`；再 `disp_enumerate()` 枚举所有 connector。
5. 为每个 connector 建立 `screen_t`，名字为 `cardN-CONNECTOR`。
6. 加载配置里的 `output ...` 项；如果没有，则 `auto_assign()` 给最多 3 块已连接屏打开测试图。
7. `input_init(route_input, &g_app)` 扫描键盘/鼠标，恢复配置里的 `input ...` 绑定。
8. `web_start()` 启动 HTTP 面板和可选的 Unix socket gateway。
9. 主循环：
   - `poll_outputs()`：热插拔检测、断线关闭、恢复重开；
   - `process_commands()`：执行 Web 队列里的 apply/disable；
   - `watchdog_check()`：无出帧/RDP 断线时重开源；
   - 配置有变化时 `config_save()`；
   - `disp_sleep_ms(20)`。
10. 收到 SIGINT/SIGTERM 后停 Web、停输入、关闭所有输出、释放 DRM。

### 3.1 输出和源的状态

`screen_t` 里最重要的字段：

- `name` / `conn` / `cardno`：稳定输出名、connector 名、GPU 卡号。
- `active` / `connected`：是否正在出图、线是否插着。
- `src_type` / `src_arg`：当前源类型和参数。
- `dirty`：源变化，worker 线程下一次循环会重建源。
- `user_off`：用户从面板点了“停止”，热插拔不能自动恢复。
- `hotplug_off`：因为拔线被关闭，插回后可以自动恢复。
- `vs` / `vnc` / `rdp` / `web`：主层源对象，归 worker 线程所有。
- `vs2` / `vnc2` / `web2`：画中画叠加层。
- `preview`：供 Web 预览的 RGB24 最新帧。
- `last_present`：看门狗判断是否还在出图。

### 3.2 线程模型

| 线程 | 数量 | 职责 |
|---|---|---|
| 主线程 | 1 | 参数、热插拔、命令队列、看门狗、配置保存 |
| `screen_thread` | 每块活动输出 1 个 | 同步源、取最新帧、绘制、`disp_present()` |
| 视频解码线程 | 每个视频/图片/本机画面/网页源 1 个 | FFmpeg 解封装/解码，发布最新帧 |
| VNC 线程 | 每个 VNC 源 1 个 | RFB 握手、更新循环、输入写 socket |
| RDP 线程 | 每个 RDP 源 1 个 | FreeRDP 连接/事件循环 |
| 音频线程 | 每个媒体音频源 1 个 | ALSA 播放和时钟 |
| Web 客户端线程 | 每个 HTTP 连接 1 个 | 静态文件/API/预览 |
| Web accept 线程 | 1 个 TCP + 1 个 Unix socket | 接受连接 |

锁的层次：

- `g_input_lock`：保护源对象生命周期和输入转发，避免输入线程访问已释放的 RDP/VNC/Web 对象。
- `screen_t.lock`：保护 `active`、`src_*`、统计、`dirty` 等短字段。
- `screen_t.preview_lock`：保护预览帧。
- `app_t.lock`：保护输出槽、配置和命令队列。
- 各源自己的 `lock`：保护最新帧、串口写、光标状态等。

---

## 4. 每屏 worker：`screen_thread()`

核心循环：

1. `screen_sync()`：如果 `dirty`，关掉旧源、按新类型打开新源。
2. 读取源对象的 `serial`；`serial` 不变表示没有新帧。
3. 有新帧：
   - 视频/图片/网页：`video_source_draw()`
   - VNC：`vnc_source_draw()`
   - RDP：`rdp_source_draw()`
   - 画中画：再画叠加层 `draw_pip()`
   - 视频类源在启动 4 秒内叠加 HUD
4. 没有新帧：
   - 静态图片每 0.25 秒重绘一次，保证预览是活的；
   - 视频/VNC/网页/RDP 空闲时每 1 秒重绘一次，让 HUD 能按时消失，也让看门狗看到“还在出图”。
5. `disp_present()` 阻塞提交，成功后更新 `last_present`。
6. 每秒更新输出 FPS；每 0.5 秒发布统计；每 1 秒更新 Web 预览。

`serial` 的语义是“源内容版本号”：

- 视频每解出一帧 `serial++`；
- VNC 每次 framebuffer 更新 `serial++`；
- RDP 每次 EndPaint `serial++`，静态桌面时每秒也 bump 一次；
- 图片只解一帧，之后 `serial` 不变，由 worker 定时重绘。

---

## 5. 内容源实现

### 5.1 视频 / 图片 / 本机画面：`video_dec.c`

`video_source_open()` 接收 `video_src_opts`：

- `url`：RTSP/HTTP/HLS/本地文件/`:99.0`（x11grab）`/dev/fb0`（fbdev）；
- `screen_w` / `screen_h`：输出尺寸；
- `loop`：本地文件循环；
- `still`：图片只解一帧；
- `hw`：尝试 VAAPI；
- `fmt`：可选的 FFmpeg 输入格式；
- `audio_dev`：媒体音频的 ALSA 设备。

内部流程：

1. `avformat_open_input()` + `avformat_find_stream_info()`；
2. 找最佳视频流，创建解码器；
3. 如果 `hw`，先尝试 `AV_HWDEVICE_TYPE_VAAPI`；失败或 codec 不支持时自动回退软件解码；
4. 找最佳音频流，用 `swresample` 转成 S16/48k/立体声，交给 `alsa_out`；
5. 解码线程只保留最新 `AVFrame`（`publish_frame()`），旧帧由下一次解码覆盖；
6. 按 PTS 做简单节流，避免 RTSP/HTTP 突发帧导致画面跳跃；
7. 显示线程 `video_source_draw()` 用 `sws_getCachedContext()` 直接缩放到 XRGB8888 画布。

图片源就是 `still=1` 的视频源：解出第一帧后停止，worker 以 4 Hz 重绘。

“本机画面”是 `screen` 源：

- 有 X11 时用 `x11grab` 抓 `:0` 或指定 display；
- 没有 X11 时用 `fbdev` 抓 `/dev/fb0`，即 fnOS 的控制台/登录画面。

### 5.2 RDP：`rdp_source.c`

不是启动 `xfreerdp` 进程，而是直接链接 `libfreerdp`：

```text
Windows RDP server
   ⇄ libfreerdp（NLA / GFX / AVC444）
   → FreeRDP GDI primary buffer
   → rdp_source->fb（BGRA）
   → XRGB 画布
```

关键点：

- 参数格式：`[domain\\]user:password@host[:port][?normal]`；
- 默认加 `+admin` 连接控制台会话；`?normal` 使用独立会话；
- `/sec:nla`、`/cert:ignore`、`/gfx:AVC444`、`/network:lan`、`/audio-mode:2` 或私有 `rdpsnd`；
- `FreeRDP_SoftwareGdi = TRUE`，`FastPathInput/Output = TRUE`；
- `rdp_end_paint()` 把 GDI primary buffer 拷贝到自己的 `fb`，`serial++`；
- 服务端光标形状通过 `rdp_ptr_*` 回调缓存，`rdp_source_draw()` 在缩放后的位置叠加光标；
- `rdp_source_keycode()` 把 Linux evdev keycode 转成 RDP scancode；扩展键有显式映射；
- `rdp_source_pointer()` 使用 0..65535 绝对坐标；
- 静态桌面每秒 bump 一次 `serial`，防止看门狗误判；
- RDP 线程断开后，主线程看门狗会重新 `dirty`，自动重连。

### 5.3 VNC：`vnc_source.c`

内置最小 RFB 客户端，不依赖 libvncserver：

- 支持 RFB 3.3 / 3.8；
- 安全类型只支持 `None`；
- 编码支持 `Raw`、`CopyRect`、`DesktopSize`；
- 支持 TCP `host:port` 和 Unix socket `/var/run/vms/*.vnc.sock`；
- `handshake()` 完成协议版本、认证、ServerInit；
- `update_loop()` 请求增量 framebuffer，处理矩形更新；
- 输入用 `vnc_source_pointer()` / `vnc_source_key()` 写 RFB 消息；
- 显示时按比例 letterbox 到输出画布，`main.c` 的 `vnc_map_point()` 使用同样的几何做坐标反算。

### 5.4 网页：`web_source.c`

```text
Xvfb（虚拟 X，无桌面）
   → Chromium --kiosk --app=URL
   → FFmpeg x11grab
   → video_dec.c
   → DRM/KMS
```

- `Xvfb -displayfd 3` 让 Xvfb 自己选空闲 display；
- Chromium 用独立 `--user-data-dir=/tmp/hdmi-web-N`，停止时 `nftw()` 删除；
- `--no-sandbox` 是因为应用通常以 root 运行，只应加载可信网页；
- `web_proc_input()` 用 XTEST 注入键鼠；
- 键盘会处理 Shift/Caps，避免网页输入大小写错乱；
- 网页源本身不直接画图，它只是启动 Xvfb/Chromium，然后让 `video_dec.c` 以 `x11grab` 采集。

### 5.5 画中画：`main.c` + `draw.c`

格式：

```text
主源|小源|x,y,w,h
```

例如：

```text
media:/movies/a.mp4|vnc:/var/run/vms/demo.vnc.sock|1280,720,480,270
```

- 主层支持视频/图片/RDP/VNC/网页；
- 叠加层支持 VNC/网页/视频/图片；
- `parse_pip()` 拆成主层/叠加层和矩形；
- `draw_pip()` 先画主层，再画叠加层；
- `route_input()` 根据指针是否进入小窗决定事件发给哪一层；
- `input_layer` 记录键盘最后进入的层，键盘跟随指针；
- 小窗坐标会按输出尺寸做裁剪和居中映射。

### 5.6 仪表盘和测试图：`draw.c` + `main.c`

- `bars`：`draw_testpattern()`，彩条 + 输出名 + 分辨率 + 时间；
- `ui`：`draw_dashboard()`，本机 CPU/内存/主机名/运行时间等；
- 视频 HUD：`draw_video_hud()`，显示源地址、codec、分辨率、FPS、进度条，约 4 秒后消失；
- 等待画面：`draw_waiting()`，源还没出帧时显示。

---

## 6. DRM/KMS 后端：`drm_display.c`

### 6.1 多卡和输出

- `disp_init()` 打开一个 `/dev/dri/cardN`，尝试 `drmSetMaster()`；
- 每张卡最多 8 个输出，全局最多 16 个槽（`DISP_MAX_OUTPUTS`）；
- `disp_enumerate()` 枚举所有 connector，包括未连接的；
- 输出名由 connector 类型和序号组成，例如 `HDMI-A-1`、`DP-1`；
- `main.c` 再拼上卡号，形成 `card0-HDMI-A-1`，避免多卡重名。

### 6.2 选 connector / CRTC / plane

`disp_open()`：

1. 解析输出名（支持 `HDMI-A-1`、`card0-HDMI-A-1`、索引 `0`）；
2. 选模式：请求 `WxH` 优先精确匹配，否则用首选模式/1080p/第一个模式；
3. 找空闲 encoder/CRTC；
4. 找该 CRTC 可用的 primary plane，并避开已被其他输出占用的 plane；
5. `drmModeAtomicCommit(DRM_MODE_ATOMIC_ALLOW_MODESET)` 做 modeset。

### 6.3 双缓冲 + shadow buffer

每个输出有两块 dumb buffer：

- `fb[2]`：DRM framebuffer；
- `map[2]`：mmap 地址；
- `back`：当前后台缓冲索引；
- `shadow`：普通 RAM 里的写回画布。

为什么要 shadow：

- dumb buffer 通常是 write-combined 内存，直接逐像素画很慢；
- 所有绘制先写 `shadow`，`disp_present()` 一次性 `memcpy` 到后台 dumb buffer，再 atomic commit；
- 避免撕裂，不需要 page-flip 事件队列。

### 6.4 `disp_present()`

1. 把 `shadow` 拷到 `map[back]`；
2. atomic 请求把 plane 的 `FB_ID` 切到 `fb[back]`；
3. 阻塞 commit；
4. 交换 `back`。

### 6.5 热插拔和 DRM master

- `poll_outputs()` 每 ~20 ms 重新枚举 connector；
- 拔线：关闭该输出，标记 `hotplug_off`；
- 插回：如果不是用户主动停止，自动重新 apply 上一次源；
- 如果启动时不是 DRM master，`disp_open()` 会再尝试 `drmSetMaster()`，成功则继续，失败提示停止 X/Wayland/compositor；
- 失败重试有 5 秒退避，避免刷日志和反复占用 GPU。

---

## 7. 输入路由：`input_router.c`

### 7.1 扫描

- 优先扫描 `/dev/input/by-id/*-event-kbd` 和 `*-event-mouse`；
- 再扫描 `/dev/input/event*`，补上没有 by-id 的设备；
- 用 `EVIOCGBIT` 判断键盘/鼠标/触摸板/触摸屏/手柄/媒体键/系统键；
- 同一物理接口的重复节点会被 `physkey + role` 去重；
- 设备 `id` 是稳定路径，例如 `/dev/input/by-id/usb-...-event-kbd`。

### 7.2 事件归一化

`input_event` 统一表示：

```c
typedef struct {
    int type;          /* 0 = pointer, 1 = key */
    int mask;          /* 1 左键, 2 中键, 4 右键 */
    int x, y;          /* 0..65535 绝对坐标 */
    uint32_t keysym;   /* VNC 用 X11 keysym */
    uint16_t keycode;  /* RDP 用 Linux evdev KEY_* */
    int down;
    int caps;
    int wheel;
} input_event;
```

- 键盘：`linux_keysym()` 把 Linux keycode 映射成 X11 keysym，同时保留原始 keycode 给 RDP；
- Shift/Caps 在本地组合，VNC 端看到正确大小写；
- 鼠标：`REL_X/REL_Y` 累加并 clamp 到 0..65535；`REL_WHEEL` 传给 VNC/RDP；
- 按键 bitmask 直接对应 RFB 的 1/2/4。

### 7.3 绑定和转发

- `input_bind(id, output)` 把设备绑定到输出；
- `route_input()` 在 `main.c`：
  - 整屏 RDP → `rdp_source_keycode/pointer`；
  - 整屏 VNC → `vnc_source_key/pointer`，坐标做 letterbox 反算；
  - 整屏网页 → `web_proc_input()`；
  - 画中画 → 根据指针进入的层转发，键盘跟随最后进入的层。

---

## 8. 音频：`alsa_out.c` + `rdp_audio.c`

### 8.1 ALSA 动态加载

- 不在编译期硬依赖 ALSA 头文件；
- `dlopen("libasound.so.2")`，缺库时只是没有声音；
- `snd_pcm_set_params()` 一次配置采样率/格式/通道/延迟；
- `alsa_write()` 写 S16 interleaved 帧。

### 8.2 找 HDMI PCM

`alsa_detect_for_drm_conn()`：

1. 根据 `/dev/dri/cardN` 找 PCI slot；
2. 在 `/proc/asound/card*/` 里找同一 PCI slot 的声卡；
3. 找该声卡的 HDMI PCM 设备；
4. 读 ELD（`/proc/asound/card*/eld#*`）匹配 monitor 名；
5. 返回 `hw:C,D`；
6. 需要时用 `amixer` 打开 IEC958。

### 8.3 媒体音频

`video_dec.c` 在同一个 `AVFormatContext` 里解音频，`swresample` 转 S16/48k/立体声，交给 `audio_sink_thread` 播放。视频和音频共享时间戳，避免第二路 reader 造成不同步。

### 8.4 RDP 音频

`rdp_audio.c` 注册一个私有 FreeRDP `rdpsnd` 后端：

- FreeRDP 的 `/sound` 参数用逗号分隔，不能直接传 `hw:3,8`；
- 所以传 `dev:c3d8` 这种无逗号 token；
- 后端还原成 `hw:3,8`，用 `alsa_out` 播放。

---

## 9. Web 面板与 API：`web_server.c` + `web/`

### 9.1 服务形态

`web_start()` 可以同时监听：

- TCP：默认 `0.0.0.0:8080`；
- Unix socket：`app.sock`，给 fnOS gateway 用；
- 可选 `prefix`，例如 `/app/hdmi-display`，请求进来先剥前缀。

静态文件从 `--web-dir` 读取；`web_server.c` 启动时计算文件哈希作为 `?v=` 版本，避免浏览器缓存旧 JS/CSS。

### 9.2 路由

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/` | 管理面板 `index.html` |
| GET | `/style.css`、`/app.js` | 静态资源 |
| GET | `/api/state` | JSON：outputs、inputs、vnc_hosts |
| GET | `/api/apply?out=&type=&arg=&w=&h=` | 应用/切换源 |
| GET | `/api/disable?out=` | 停用输出 |
| GET | `/api/input/bind?id=&out=` | 绑定键鼠；`out=` 空表示解绑 |
| GET | `/api/quit` | 请求退出 |
| GET | `/preview/NAME.mjpg` | MJPEG 实时预览 |
| GET | `/snapshot/NAME.jpg` | 单帧 JPEG |
| GET | `/rdp/enable-rdp.ps1` | 下载 Windows RDP 60fps 脚本 |

可选 `--web-token TOK`：所有请求要带 `?token=TOK`。

### 9.3 `/api/state` 主要字段

每个 output 包含：

- `name`、`connected`、`active`、`width`、`height`、`refresh`；
- `source_type`、`source_arg`、`online`、`fps`、`error`；
- `src_w`、`src_h`、`pos`、`dur`；
- `input_ok`、`pip` 等信息。

`inputs` 包含设备 `id`、`name`、`group`、`kind`、`output`、`events`、`last_ms`。

`vnc_hosts` 是 `/run/vms` 和 `/var/run/vms` 下的 `*.vnc.sock`。

### 9.4 预览

- 预览最大 640×400（`PREVIEW_MAX_W/H`），按输出比例缩放；
- `copy_preview()` 从 `screen_t.preview` 拷贝 RGB24；
- `serve_mjpeg()` 用 FFmpeg MJPEG 编码器输出 `multipart/x-mixed-replace`；
- `serve_snapshot()` 编码单帧 JPEG。

### 9.5 前端 `web/app.js`

无 React/Vue，纯 DOM：

- `BASE` 同时兼容直连 `/` 和 fnOS gateway `/app/hdmi-display/`；
- `TOKEN` 从 URL 读取并附加到 API/预览；
- 每秒轮询 `/api/state`，只有状态签名变化才重绘；
- 每块屏一张卡片：状态、预览、类型、参数、分辨率、应用/停止；
- `HIST` 用 `localStorage` 保存“每块屏 + 每种源类型”的最后地址；
- `DIRTY`/`PENDING` 防止用户正在输入时被轮询覆盖；
- 输入设备区只显示可绑定的输出（RDP/VNC/网页或包含这些层的画中画）；
- VNC 下拉框来自 `vnc_hosts`，选择后自动填 socket 路径。

---

## 10. 配置：`config.c`

配置文件默认 `hdmi_display.conf`，fnOS 下通常是
`/usr/local/apps/@appconf/hdmi-display/hdmi_display.conf`。

格式：

```text
web_port=8080
audio=auto

output card0-HDMI-A-1 media 1920x1080 rtsp://10.0.0.5/live
output card0-HDMI-A-2 ui 0x0
output card0-HDMI-A-3 image 0x0 /data/wallpaper.jpg
output card0-HDMI-A-4 remote 1920x1080 Administrator:password@10.0.0.12:3389

input /dev/input/by-id/usb-...-event-kbd card0-HDMI-A-4
```

- `web_port`：Web 端口；
- `audio`：`auto` / `off` / `hw:C,D`；
- `output NAME TYPE WxH ARG`：输出源；
- `input DEVICE OUTPUT`：键鼠绑定。

`config_save()` 写 `.tmp`，`fsync` 后 `rename`，避免断电留下半个配置。

---

## 11. 打包与 fnOS 生命周期

### 11.1 fpk 目录

```text
packaging/fpk/hdmi-display/
├─ manifest
├─ cmd/                 start/stop/install/upgrade/config/uninstall
├─ config/privilege     run-as: root
├─ config/resource      hdmi-display/media 共享目录
├─ wizard/              中文安装/配置/卸载向导
├─ i18n/                App Center 名称/描述
├─ ui/                  App Center iframe 入口
└─ app/
   ├─ bin/hdmi_display
   ├─ bin/amixer
   ├─ bin/enable-rdp.ps1
   ├─ lib/              依赖闭包 + ossl-modules/legacy.so
   ├─ web/              管理面板
   └─ licenses/
```

### 11.2 systemd unit

`cmd/common` 的 `install_unit()` 生成：

- `User=root`；
- `Environment=LD_LIBRARY_PATH=$APPDEST/lib`；
- `Environment=OPENSSL_MODULES=$APPDEST/lib/ossl-modules`；
- `ExecStart=.../hdmi_display --config ... --hwaccel --web-port ... --web-socket ... --web-prefix /app/hdmi-display`；
- `Restart=always`、`RestartSec=3`；
- 如果向导选择停止 `appliance-compositor`，unit 会加 `Conflicts=`、`ExecStartPre=stop`、`ExecStopPost=start`。

### 11.3 生命周期

- `install_init`：解包前检查 `/dev/dri/card0`，停旧实例；
- `install_callback`：解包后检查二进制和 `ldd`，保存向导设置，安装网页依赖，生成 unit，启动应用；
- `config_init`：记录当前是否运行，停止服务；
- `config_callback`：保存新设置，重建 unit，恢复原运行状态；
- `upgrade_init`：记录运行状态，停服务；
- `upgrade_callback`：重建 unit，恢复运行状态；
- `uninstall_init`：停服务；
- `uninstall_callback`：删 unit、drop-in、socket、runtime、Chromium 临时 profile；按向导选择删除或保留配置/数据/应用目录。

### 11.4 依赖打包

`packaging/build-fpk.sh`：

- 编译 `hdmi_display`；
- 拷贝 `web/`、`enable-rdp.ps1`、`amixer`；
- 拷贝 FreeRDP client/core/WinPR；
- 拷贝最小 FFmpeg 和 libdrm；
- `bundle_closure()` 用 `ldd` 递归收集非 glibc 依赖；
- 拷贝 OpenSSL 3 `legacy.so`（NLA 需要 MD4/RC4）；
- `fygopack`/`fnpack` 生成 fpk。

### 11.5 `enable-rdp.ps1`

Windows 端脚本做四件事：

1. `fDenyTSConnections=0` 开启 RDP；
2. 添加 en-US 键盘并设为默认，避免中文输入法把 RDP 字母键变成拼音；
3. `DWMFRAMEINTERVAL=15` 把 RDP 帧率上限从 30 提到 60；
4. 放行 Remote Desktop 防火墙组，并打印连接地址示例。

---

## 12. 构建

### 12.1 Makefile

```bash
make
make FFMPEG_PREFIX=/opt/ffmpeg-min
make clean
```

依赖：

```bash
sudo apt install build-essential pkg-config libdrm-dev \
    freerdp3-dev winpr3-dev libssl-dev libx11-dev libxtst-dev
```

`SRCS` 包含：

```text
main.c draw.c font8x16.c font_cjk.c video_dec.c vnc_source.c
rdp_source.c rdp_audio.c web_source.c input_router.c web_server.c
os_thread.c platform.c config.c alsa_out.c drm_display.c
```

### 12.2 build-kit

- Docker：`build-kit/build-in-docker.sh` / `.ps1`；
- Debian 12/fnOS 原生：`build-kit/setup-debian.sh`；
- 最小 FFmpeg：`packaging/build-ffmpeg-min.sh`，固定 5.1.8；
- 源码包：`build-kit/make-source-archive.sh`。

详见 [build-kit/README.md](../build-kit/README.md)。

---

## 13. 二次开发入口

### 加一种内容源

1. 在 `main.c` 的 `classify()` 或 `open_layer()` 加类型分支；
2. 新建 `xxx_source.c/.h`，实现：
   - `open` / `close`
   - `draw`
   - `serial`
   - `status`
   - 输入函数（如果需要键鼠）
3. 在 `screen_t` 加对象指针；
4. 在 `screen_sync()` 打开，在 `screen_thread()` 画，在 `close_layer()` 关闭；
5. 在 `route_input()` 加输入转发；
6. 在 `web/app.js` 的类型下拉和参数提示里加类型；
7. 在 `Makefile` 的 `SRCS` 加源文件。

### 加一个 Web API

1. 在 `web_server.c` 的 `handle_client()` 加路由；
2. 在 `web_ops` 加回调；
3. 在 `main.c` 实现回调并更新 `app_state_json()`（如果需要暴露状态）；
4. 在 `web/app.js` 调用。

### 加一个显示后端

当前只有 `drm_display.c`。要加 Wayland/GBM/X11 后端：

1. 实现 `display.h` 的 `disp_*` 接口；
2. 在 `Makefile` 按平台选择 `DISPLAY_SRC`；
3. 保持 `disp_backbuffer()` / `disp_pitch()` / `disp_present()` 语义不变，上层不用改。

### 加字库

- ASCII：改 `font8x16.c`；
- 中文：重新生成 `font_cjk.c` 的 16×16 GB2312 表，保持 `font_cjk16[]` 布局；
- `draw.c` 的 `cjk_glyph()` 按 Unicode 查表。

---

## 14. 性能与稳定性设计

- **最新帧模型**：解码线程不排队，显示线程只拿最新帧，延迟低。
- **shadow buffer**：避免直接写 write-combined 显存。
- **静态帧降频**：图片 4 Hz，空闲视频/VNC/RDP 1 Hz，减少 CPU。
- **RDP 静态桌面 keep-alive**：每秒 bump serial，避免看门狗误判。
- **看门狗**：10 秒无 `disp_present` 或 RDP 断线时重开源；systemd `Restart=always` 兜底。
- **热插拔恢复**：拔线关闭、插回自动恢复；用户主动停止不会被热插拔覆盖。
- **单实例锁**：`flock` 防止两个进程抢 DRM master。
- **配置原子写**：临时文件 + fsync + rename。

测试数据见 [README.md](../README.md) 的“性能与稳定性”。

---

## 15. 已知边界

- 只支持 Linux DRM/KMS；没有 Wayland/GBM 后端。
- VNC 只支持无认证。
- RDP 剪贴板未开启。
- 输入设备在启动时扫描；新增设备需要重启服务。
- 网页源 Chromium 以 `--no-sandbox` 运行，只加载可信页面。
- 输出数量受 GPU connector/CRTC/带宽限制，不是软件固定值。
- RDP 密码明文保存在本机配置，仅建议可信内网。

---

## 16. 许可证

项目使用 **GPL-3.0-or-later**，见 [LICENSE](../LICENSE)。

第三方组件：

- FFmpeg 5.1.8：LGPL-2.1-or-later（最小构建，库可替换）
- FreeRDP / WinPR：Apache-2.0
- Noto Sans CJK SC：SIL OFL 1.1
- X11/XCB/libdrm/OpenSSL/ALSA：各自上游许可证

完整说明见 [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md)。
