# HDMI Display 源码 / 编译环境

这个目录让源码换一台电脑也能直接编译出可安装的 `.fpk`。

## 内容

- 完整 C 源码、Makefile、`web/` 管理面板、`packaging/` 打包脚本。
- `build-kit/Dockerfile`：Debian 12 + FreeRDP 3 + 最小 FFmpeg 5.1.8 + fnpack。
- `build-kit/build-in-docker.sh`：Linux / macOS 一键容器编译。
- `build-kit/build-in-docker.ps1`：Windows Docker Desktop 一键编译。
- `build-kit/setup-debian.sh`：在 Debian 12 / fnOS 上直接安装编译依赖。
- `build-kit/make-source-archive.sh`：重新生成干净的源码包。

## 方式 A：Docker（推荐，最省事）

要求：安装 Docker Desktop / Docker Engine，x86_64，能联网拉取 Debian 镜像和 apt 包。

Windows PowerShell：

```powershell
powershell -ExecutionPolicy Bypass -File build-kit\build-in-docker.ps1
```

Linux / macOS：

```bash
bash build-kit/build-in-docker.sh
```

第一次会构建镜像（安装编译依赖、fnpack、编译最小 FFmpeg），需要几分钟。之后复用镜像，只编译应用。

国内网络如果 apt 很慢，可以指定镜像源：

```powershell
$env:DEBIAN_MIRROR="http://mirrors.ustc.edu.cn/debian"
$env:DEBIAN_SECURITY_MIRROR="http://mirrors.ustc.edu.cn/debian-security"
powershell -ExecutionPolicy Bypass -File build-kit\build-in-docker.ps1
```

```bash
DEBIAN_MIRROR=http://mirrors.ustc.edu.cn/debian \
DEBIAN_SECURITY_MIRROR=http://mirrors.ustc.edu.cn/debian-security \
bash build-kit/build-in-docker.sh
```

输出：

```text
build/hdmi-display_<version>.fpk
```

## 方式 B：Debian 12 / fnOS 原生编译

要求：x86_64，root/sudo，能联网安装 apt 包和下载 FFmpeg 源码。

```bash
sudo bash build-kit/setup-debian.sh
bash packaging/build-fpk.sh
```

脚本会：

1. 启用 `bookworm-backports`，安装 `freerdp3-dev` / `libwinpr3-dev`。
2. 安装 `libdrm-dev`、X11/XCB、VAAPI、OpenSSL、nasm 等编译依赖。
3. 安装固定版本的 `fnpack 1.2.3`。
4. 下载 FFmpeg 5.1.8 源码并校验 SHA256，编译最小版到 `/opt/ffmpeg-min`。
5. 提示后续编译命令。

只改源码后的快速重建：

```bash
make -j"$(nproc)" FFMPEG_PREFIX=/opt/ffmpeg-min
bash packaging/build-fpk.sh
```

## 依赖说明

- **FreeRDP 3**：来自 Debian 12 `bookworm-backports`，版本约 3.10.x；不要换成 Debian 12 默认的 FreeRDP 2。
- **FFmpeg**：固定 5.1.8，源码已放在 `build-kit/vendor/ffmpeg-5.1.8-git.tar.gz`，不再依赖 ffmpeg.org 下载；只启用应用实际使用的解码器、解复用器、协议、x11grab/fbdev、VAAPI 和 MJPEG 预览编码器。
- **fnpack 1.2.3**：用于生成 `.fpk`，下载地址和 SHA256 固定。
- 目标架构：`x86_64`，manifest 为 `platform="x86"`。

## 离线编译

如果目标电脑不能联网，可提前准备：

1. Debian 12 的 apt 包缓存（`apt-get install --download-only ...`）。
2. FFmpeg 源码已经随源码包提供，不需要联网下载。
3. fnpack：放到 `/usr/local/bin/fnpack` 并 `chmod 755`；也可以复制一台已编译机器的版本。
4. 或直接复制一台已编译机器的 `/opt/ffmpeg-min` 到相同路径。

Docker 方式也可以在有网机器上先 `docker build`，然后用 `docker save` / `docker load` 把镜像带到离线机器。

## 验证

编译后可检查：

```bash
tar xOf build/hdmi-display_<version>.fpk manifest | head
sha256sum build/hdmi-display_<version>.fpk
```

干净容器运行检查：

```bash
docker run --rm -v "$PWD/build/fpk/hdmi-display:/app:ro" debian:bookworm \
  bash -c 'LD_LIBRARY_PATH=/app/app/lib ldd /app/app/bin/hdmi_display | grep "not found" || echo ALL_FOUND'
```

## 重新生成源码包

```bash
bash build-kit/make-source-archive.sh
```

会生成：

```text
build/hdmi-display_<version>_src.tar.gz
build/hdmi-display_<version>_src.tar.gz.sha256
```

源码包不包含 `build/`、`.git/`、`.o`、`.d`、已编译二进制和 fpk。
