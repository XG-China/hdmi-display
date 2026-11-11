#!/bin/bash
# Prepare a native Debian 12 / fnOS build environment (x86_64).
#
#   sudo bash build-kit/setup-debian.sh
#
# Installs the build dependencies, FreeRDP 3 from bookworm-backports, fnpack
# and the minimal FFmpeg in /opt/ffmpeg-min. Then build with:
#
#   bash packaging/build-fpk.sh
#
set -euo pipefail

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)

if [ "$(id -u)" -ne 0 ]; then
    echo "run this script with sudo: sudo bash $0" >&2
    exit 1
fi

if [ -r /etc/os-release ]; then
    . /etc/os-release
    echo "system: ${PRETTY_NAME:-unknown}"
fi

if ! grep -Rqs 'bookworm-backports' /etc/apt/sources.list /etc/apt/sources.list.d/ 2>/dev/null; then
    echo "==> enabling Debian bookworm-backports (FreeRDP 3)"
    echo 'deb http://deb.debian.org/debian bookworm-backports main' \
        > /etc/apt/sources.list.d/bookworm-backports.list
fi

echo "==> installing build dependencies"
apt-get update
DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    build-essential pkg-config nasm curl ca-certificates xz-utils file \
    libva-dev libdrm-dev libx11-dev libxext-dev libxfixes-dev libxv-dev \
    libxcb1-dev libxcb-shm0-dev libxcb-shape0-dev libxcb-xfixes0-dev \
    libxtst-dev libssl-dev zlib1g-dev \
    freerdp3-dev libwinpr3-dev

if ! command -v fnpack >/dev/null 2>&1; then
    echo "==> installing fnpack 1.2.3"
    curl -fsSL \
        https://static2.fnnas.com/fnpack/fnpack-1.2.3-linux-amd64 \
        -o /tmp/fnpack
    echo "54B97FA7B70968C4D05C79840F5DAEFF508957D0BB2062FDB0376D00D9615C93  /tmp/fnpack" \
        | sha256sum -c -
    install -m 755 /tmp/fnpack /usr/local/bin/fnpack
    rm -f /tmp/fnpack
fi

echo "==> building minimal FFmpeg 5.1.8 in /opt/ffmpeg-min"
FFMPEG_PREFIX=/opt/ffmpeg-min bash "$ROOT/packaging/build-ffmpeg-min.sh"

cat <<EOF

environment ready.

Next:
  cd "$ROOT"
  bash packaging/build-fpk.sh

Output:
  $ROOT/build/hdmi-display_<version>.fpk

If you only want to rebuild the app after changing sources:
  make -j\$(nproc) FFMPEG_PREFIX=/opt/ffmpeg-min
  bash packaging/build-fpk.sh

EOF
