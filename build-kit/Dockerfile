# Reproducible x86_64 build environment for HDMI Display.
# Build from the repository root:
#   docker build -f build-kit/Dockerfile -t hdmi-display-build:1.0.28 .
#
FROM debian:bookworm

ENV DEBIAN_FRONTEND=noninteractive

# Optional Debian mirror override, useful on slow international links.
# Use http:// here: the base image has no ca-certificates before this step.
#   docker build --build-arg DEBIAN_MIRROR=http://mirrors.ustc.edu.cn/debian \
#                --build-arg DEBIAN_SECURITY_MIRROR=http://mirrors.ustc.edu.cn/debian-security ...
ARG DEBIAN_MIRROR=http://deb.debian.org/debian
ARG DEBIAN_SECURITY_MIRROR=http://deb.debian.org/debian-security

# FreeRDP 3.x is in bookworm-backports (the app links against libfreerdp3).
RUN rm -f /etc/apt/sources.list.d/debian.sources && \
    printf 'deb %s bookworm main\ndeb %s bookworm-updates main\ndeb %s bookworm-security main\ndeb %s bookworm-backports main\n' \
        "$DEBIAN_MIRROR" "$DEBIAN_MIRROR" "$DEBIAN_SECURITY_MIRROR" "$DEBIAN_MIRROR" \
        > /etc/apt/sources.list && \
    printf 'Acquire::Retries "3";\nAcquire::http::Timeout "30";\nAcquire::https::Timeout "30";\n' \
        > /etc/apt/apt.conf.d/80-retries && \
    apt-get update && \
    apt-get install -y --no-install-recommends \
        build-essential pkg-config nasm curl ca-certificates xz-utils file \
        libva-dev libdrm-dev libx11-dev libxext-dev libxfixes-dev libxv-dev \
        libxcb1-dev libxcb-shm0-dev libxcb-shape0-dev libxcb-xfixes0-dev \
        libxtst-dev libssl-dev zlib1g-dev \
        freerdp3-dev libwinpr3-dev \
    && rm -rf /var/lib/apt/lists/*

# fnpack builds the .fpk container. Pin it so builds are reproducible.
RUN curl -fsSL \
        https://static2.fnnas.com/fnpack/fnpack-1.2.3-linux-amd64 \
        -o /usr/local/bin/fnpack && \
    echo "54B97FA7B70968C4D05C79840F5DAEFF508957D0BB2062FDB0376D00D9615C93  /usr/local/bin/fnpack" \
        | sha256sum -c - && \
    chmod 755 /usr/local/bin/fnpack

# Build the minimal FFmpeg once into the image. The vendored upstream source
# archive avoids depending on ffmpeg.org at build time.
COPY build-kit/vendor/ffmpeg-5.1.8-git.tar.gz /tmp/ffmpeg-5.1.8-git.tar.gz
COPY packaging/build-ffmpeg-min.sh /tmp/build-ffmpeg-min.sh
RUN chmod 755 /tmp/build-ffmpeg-min.sh && \
    FFMPEG_TARBALL=/tmp/ffmpeg-5.1.8-git.tar.gz \
    FFMPEG_PREFIX=/opt/ffmpeg-min /tmp/build-ffmpeg-min.sh && \
    rm -rf /tmp/build-ffmpeg-min.sh /tmp/ffmpeg-5.1.8-git.tar.gz \
           /tmp/FFmpeg-n5.1.8

WORKDIR /src
CMD ["bash"]
