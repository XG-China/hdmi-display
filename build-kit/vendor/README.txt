FFmpeg 5.1.8 source archive
===========================

File:    ffmpeg-5.1.8-git.tar.gz
Origin:  https://github.com/FFmpeg/FFmpeg/archive/refs/tags/n5.1.8.tar.gz
SHA256:  8c1e31f2082dc091fbd3d8d1a3ec2d08edddc7f310f5c6c4c461a4bc3d19eb1f

The official https://ffmpeg.org/releases/ URL is blocked on some networks.
packaging/build-ffmpeg-min.sh uses this vendored archive when present and
falls back to downloading the GitHub tag archive. The build only enables the
codecs/demuxers/protocols/devices/VAAPI/MJPEG preview encoder used by the app.
