#!/bin/bash
# Build hdmi-display.fpk for Fygo OS.
#
#   packaging/build-fpk.sh
#
# Optional:
#   BUNDLE_LIBS=0 packaging/build-fpk.sh
#       do not bundle FFmpeg/libdrm (default is 1: a portable package).
#       The app, FreeRDP and their non-base dependencies are always bundled.
#
# Requirements: fygopack, make, the app build dependencies
# (libdrm-dev, freerdp3-dev, winpr3-dev, libssl-dev, libx11-dev,
#  libxtst-dev) and the minimal FFmpeg in $FFMPEG_PREFIX.
# packaging/build-ffmpeg-min.sh builds it automatically when missing.
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO=$(dirname "$SCRIPT_DIR")
NAME=hdmi-display
BUILD=$REPO/build
PKG=$SCRIPT_DIR/fpk/$NAME
BUNDLE_LIBS=${BUNDLE_LIBS:-1}
FFMPEG_PREFIX=${FFMPEG_PREFIX:-/opt/ffmpeg-min}

log() { printf '==> %s\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

FIND_PACKER=$(command -v fygopack || command -v fnpack || true)
[ -n "$FIND_PACKER" ] || die \
    "fygopack/fnpack not found: https://developer.fygonas.com/docs/cli/fygopack/"
PACKER=$FIND_PACKER
command -v make >/dev/null || die "make not found"
[ -f "$REPO/Makefile" ] || die "run this script from the project's packaging/ directory"
[ -d "$PKG" ] || die "package tree missing: $PKG"

# The portable package uses a minimal FFmpeg (only the used codecs/demuxers/
# protocols). Build it once if the prefix is missing.
if [ "$BUNDLE_LIBS" = "1" ] && [ ! -f "$FFMPEG_PREFIX/lib/libavdevice.so.59" ]; then
    log "minimal FFmpeg not found in $FFMPEG_PREFIX; building it"
    FFMPEG_PREFIX="$FFMPEG_PREFIX" "$SCRIPT_DIR/build-ffmpeg-min.sh"
fi

export PKG_CONFIG_PATH="$FFMPEG_PREFIX/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export LD_LIBRARY_PATH="$FFMPEG_PREFIX/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

log "building hdmi_display (FFmpeg: $FFMPEG_PREFIX)"
(cd "$REPO" && make -j"$(nproc 2>/dev/null || echo 4)" FFMPEG_PREFIX="$FFMPEG_PREFIX")
[ -x "$REPO/hdmi_display" ] || die "build failed: $REPO/hdmi_display not found"

dst=$BUILD/fpk/$NAME
rm -rf "$dst"
mkdir -p "$BUILD/fpk"
cp -a "$PKG" "$dst"
install -Dm755 "$REPO/hdmi_display" "$dst/app/bin/hdmi_display"

# The admin panel is a static web app (fnOS gateway / direct :8080).
if [ -d "$REPO/web" ]; then
    mkdir -p "$dst/app/web"
    cp -a "$REPO/web/." "$dst/app/web/"
else
    die "web/ directory missing (admin panel)"
fi

# Windows RDP helper, served by the web panel.
RDP_DIR=$REPO/packaging/rdp
if [ -f "$RDP_DIR/enable-rdp.ps1" ]; then
    install -Dm644 "$RDP_DIR/enable-rdp.ps1" "$dst/app/bin/enable-rdp.ps1"
else
    log "warning: $RDP_DIR/enable-rdp.ps1 missing; RDP setup download will be unavailable"
fi

# HDMI audio: alsa_out calls amixer to enable the codec's IEC958 switch. Bundle
# it so a NAS without alsa-utils still gets sound; the unit puts app/bin first
# in PATH.
if [ -x /usr/bin/amixer ]; then
    install -Dm755 /usr/bin/amixer "$dst/app/bin/amixer"
fi

# Microsoft RDP source: link libfreerdp directly, so bundle the client,
# core and WinPR runtime libraries. No xfreerdp3 binary is needed.
mkdir -p "$dst/app/lib"
for pat in 'libfreerdp-client3.so.3*' 'libfreerdp3.so.3*' 'libwinpr3.so.3*'; do
    for dir in /usr/lib/x86_64-linux-gnu /lib/x86_64-linux-gnu; do
        for f in $dir/$pat; do
            [ -e "$f" ] && cp -a "$f" "$dst/app/lib/"
        done
    done
done
log "bundled FreeRDP client/core/WinPR runtime libraries"

if [ "$BUNDLE_LIBS" = "1" ]; then
    [ -f "$FFMPEG_PREFIX/lib/libavdevice.so.59" ] || \
        die "minimal FFmpeg missing in $FFMPEG_PREFIX"
    mkdir -p "$dst/app/lib"
    for pat in \
        'libavdevice.so.59*' 'libavformat.so.59*' 'libavcodec.so.59*' \
        'libavutil.so.57*' 'libswscale.so.6*' 'libswresample.so.4*'; do
        for f in "$FFMPEG_PREFIX"/lib/$pat; do
            [ -e "$f" ] && cp -a "$f" "$dst/app/lib/"
        done
    done
    for pat in 'libdrm.so.2*'; do
        for dir in /usr/lib/x86_64-linux-gnu /lib/x86_64-linux-gnu; do
            for f in $dir/$pat; do
                [ -e "$f" ] && cp -a "$f" "$dst/app/lib/"
            done
        done
    done
    log "bundled minimal FFmpeg + DRM libraries"
fi

# Bundle the complete non-base dependency closure so the fpk also starts on a
# NAS that does not have the FreeRDP/FFmpeg development packages installed.
# Base glibc, the dynamic loader, the host OpenSSL 3 and host ALSA are left
# alone; everything else is shipped in app/lib.
bundle_closure() {
    local queue=("$@")
    local seen=""
    while [ ${#queue[@]} -gt 0 ]; do
        local cur="${queue[0]}"
        queue=("${queue[@]:1}")
        [ -e "$cur" ] || continue
        case " $seen " in *" $cur "*) continue ;; esac
        seen="$seen $cur"
        while IFS= read -r line; do
            case "$line" in
                *"=> not found"*)
                    log "warning: unresolved dependency: $line"
                    continue
                    ;;
                *"=> /"*) ;;
                *) continue ;;
            esac
            # ldd indents dependency lines with a tab; strip it or the copied
            # file gets a tab-prefixed name and the loader cannot find it.
            line="${line#"${line%%[![:space:]]*}"}"
            local name="${line%% *}"
            local path
            path=$(printf '%s\n' "$line" | sed -n 's/.*=> \([^ ]*\).*/\1/p')
            [ -n "$path" ] || continue
            case "$name" in
                libc.so.6|libm.so.6|libpthread.so.0|libdl.so.2|librt.so.1|libresolv.so.2|libcrypt.so.1|ld-linux*|linux-vdso.so.1)
                    continue
                    ;;
            esac
            local base
            base=$(basename "$name")
            if [ ! -e "$dst/app/lib/$base" ]; then
                cp -L "$path" "$dst/app/lib/$base" 2>/dev/null || true
            fi
            queue+=("$dst/app/lib/$base")
        done < <(LD_LIBRARY_PATH="$dst/app/lib" ldd "$cur" 2>/dev/null)
    done
}

roots=("$dst/app/bin/hdmi_display")
for f in "$dst"/app/lib/*.so*; do
    [ -e "$f" ] && roots+=("$f")
done
bundle_closure "${roots[@]}"
log "bundled $(find "$dst/app/lib" -maxdepth 1 \( -type f -o -type l \) | wc -l) dependency libraries"

# OpenSSL 3 keeps MD4/RC4 (needed by NLA) in the legacy provider. Bundle it
# and point OPENSSL_MODULES at it from the systemd unit.
if [ -f /usr/lib/x86_64-linux-gnu/ossl-modules/legacy.so ]; then
    install -Dm644 /usr/lib/x86_64-linux-gnu/ossl-modules/legacy.so \
        "$dst/app/lib/ossl-modules/legacy.so"
fi

find "$dst" -type d -exec chmod 755 {} +
find "$dst" -type f -exec chmod 644 {} +
chmod 755 "$dst"/cmd/* "$dst"/app/bin/*

log "packing $NAME $(sed -n 's/^version="\(.*\)"/\1/p' "$dst/manifest")"
(cd "$BUILD" && "$PACKER" build --directory "$dst")
[ -f "$BUILD/$NAME.fpk" ] || die "fygopack did not produce $BUILD/$NAME.fpk"
ls -l "$BUILD/$NAME.fpk"
