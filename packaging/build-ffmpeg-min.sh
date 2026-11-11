#!/bin/bash
# Build a minimal FFmpeg for HDMI Display.
#
# Only the components the app actually uses are enabled:
#   - decoders: h264/hevc/mpeg2/mpeg4/vc1/wmv3/vp8/vp9/av1/mjpeg/png/bmp/gif
#               and the common audio codecs
#   - demuxers: mov/matroska/mpegts/mpegps/avi/flv/rtsp/hls/image2/...
#   - protocols: file/http/https/tcp/udp/rtp/rtsp/rtmp
#   - x11grab (xcbgrab) and fbdev input devices
#   - the MJPEG *encoder* used by the panel preview / snapshot endpoint
#   - VAAPI hwaccels and openssl/zlib
#
# It does NOT enable encoders (except MJPEG), muxers, filters or any
# autodetected third-party codec library (x265, aom, vpx, rsvg, flite, ...).
#
#   packaging/build-ffmpeg-min.sh
#   FFMPEG_PREFIX=/opt/ffmpeg-min packaging/build-ffmpeg-min.sh
#   FORCE=1 packaging/build-ffmpeg-min.sh
#
# Requirements (Debian 12 / fnOS):
#   sudo apt install build-essential pkg-config nasm \
#        libva-dev libdrm-dev libx11-dev libxext-dev libxfixes-dev libxv-dev \
#        libxcb1-dev libxcb-shm0-dev libxcb-shape0-dev libxcb-xfixes0-dev \
#        libssl-dev zlib1g-dev
#
# The output is ABI-compatible with Debian 12's FFmpeg 5.1 (same sonames).
set -euo pipefail

VERSION=5.1.8
# Upstream GitHub archive for tag n5.1.8 (works when ffmpeg.org is blocked).
SHA256=${FFMPEG_SHA256:-8c1e31f2082dc091fbd3d8d1a3ec2d08edddc7f310f5c6c4c461a4bc3d19eb1f}
PREFIX=${FFMPEG_PREFIX:-/opt/ffmpeg-min}
TAR=${FFMPEG_TARBALL:-/tmp/ffmpeg-$VERSION-git.tar.gz}
SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO=$(dirname "$SCRIPT_DIR")
VENDOR="$REPO/build-kit/vendor/ffmpeg-$VERSION-git.tar.gz"

log() { printf '==> %s\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

if [ -f "$PREFIX/lib/libavdevice.so.59" ] && [ "${FORCE:-0}" != "1" ]; then
    log "minimal FFmpeg already installed in $PREFIX (set FORCE=1 to rebuild)"
    exit 0
fi

for t in curl tar make gcc pkg-config; do
    command -v "$t" >/dev/null || die "$t not found"
done

if [ ! -f "$TAR" ] && [ -f "$VENDOR" ]; then
    log "using vendored FFmpeg $VERSION source"
    cp "$VENDOR" "$TAR"
fi
if [ ! -f "$TAR" ]; then
    log "downloading FFmpeg $VERSION"
    curl -L --fail -o "$TAR" \
        "https://github.com/FFmpeg/FFmpeg/archive/refs/tags/n$VERSION.tar.gz"
fi
echo "$SHA256  $TAR" | sha256sum -c - >/dev/null || die "FFmpeg source checksum mismatch"

rm -rf "/tmp/ffmpeg-$VERSION" "/tmp/FFmpeg-n$VERSION"
log "extracting"
tar -C /tmp -xf "$TAR"
SRC="/tmp/$(tar -tf "$TAR" | sed -n '1s#/.*##p')"
[ -d "$SRC" ] || die "cannot find extracted FFmpeg source"
cd "$SRC"

log "configuring (prefix $PREFIX)"
./configure --prefix="$PREFIX" \
    --disable-autodetect --disable-programs --disable-doc --disable-debug \
    --enable-shared --disable-static --enable-pic \
    --disable-everything \
    --disable-avfilter \
    --enable-avdevice --enable-avformat --enable-avcodec --enable-avutil \
    --enable-swscale --enable-swresample \
    --enable-network --enable-xlib \
    --enable-protocol=file,http,https,tcp,udp,rtp,rtsp,rtmp,crypto,data \
    --enable-demuxer=mov,matroska,mpegts,mpegps,avi,flv,rtsp,sdp,image2,jpeg_pipe,png_pipe,bmp_pipe,gif,mp3,aac,flac,ogg,wav,hls,concat,rawvideo \
    --enable-parser=h264,hevc,mpeg4video,mpegvideo,vp8,vp9,av1,aac,ac3,dca,flac,opus,vorbis,mjpeg,png \
    --enable-decoder=h264,hevc,mpeg2video,mpeg4,msmpeg4v3,vc1,wmv3,vp8,vp9,av1,mjpeg,png,bmp,gif,rawvideo,aac,mp3,ac3,eac3,dca,truehd,flac,opus,vorbis,pcm_s16le,pcm_s24le,pcm_f32le,pcm_s16be,pcm_s24be,pcm_u8,pcm_alaw,pcm_mulaw \
    --enable-encoder=mjpeg \
    --enable-hwaccel=h264_vaapi,hevc_vaapi,mpeg2_vaapi,vp8_vaapi,vp9_vaapi \
    --enable-indev=xcbgrab,fbdev \
    --enable-bsf=h264_mp4toannexb,hevc_mp4toannexb,aac_adtstoasc \
    --enable-vaapi --enable-libdrm --enable-openssl --enable-zlib \
    --enable-libxcb --enable-libxcb-shm --enable-libxcb-xfixes --enable-libxcb-shape

log "building"
make -j"$(nproc 2>/dev/null || echo 4)"
log "installing"
make install

log "done: $PREFIX"
ls -l "$PREFIX"/lib/libavcodec.so.* "$PREFIX"/lib/libavformat.so.* \
      "$PREFIX"/lib/libavdevice.so.* "$PREFIX"/lib/libavutil.so.* \
      "$PREFIX"/lib/libswscale.so.* "$PREFIX"/lib/libswresample.so.* 2>/dev/null || true
