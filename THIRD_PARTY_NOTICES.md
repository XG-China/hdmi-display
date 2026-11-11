# Third-party notices

HDMI Display is licensed under **GPL-3.0-or-later**. It contains or links
against the following third-party components:

## FFmpeg 5.1.8

- License: LGPL-2.1-or-later (this build does not enable GPL components)
- Project: https://ffmpeg.org/
- The shared libraries are bundled in `app/lib` and can be replaced by the user.
- Source: https://ffmpeg.org/releases/ffmpeg-5.1.8.tar.xz or the vendored
  archive in `build-kit/vendor/ffmpeg-5.1.8-git.tar.gz`.

## FreeRDP / WinPR 3.x

- License: Apache License 2.0
- Project: https://www.freerdp.com/
- See the upstream `LICENSE` and `NOTICE` files.

## Noto Sans CJK SC

- License: SIL Open Font License 1.1
- Project: https://github.com/notofonts/noto-cjk
- The 16x16 bitmap glyph table in `font_cjk.c` is generated from
  `NotoSansCJKsc-Regular.otf`.
- License text: `packaging/fonts/OFL-Noto.txt` and
  `packaging/fpk/hdmi-display/app/licenses/OFL-Noto.txt`.

## Other libraries

X11, XCB, libdrm, libva, OpenSSL, ALSA, libfuse, zlib, cJSON and their
dependencies keep their upstream licenses (MIT-style, Apache-2.0,
LGPL-2.1-or-later, zlib, etc.). Their license files are provided by the
corresponding Debian packages and upstream projects.

No trademark rights are granted. “Windows” and “RDP” are names of Microsoft
products/protocols and are used only to describe compatibility.
