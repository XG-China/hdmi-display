#!/bin/bash
# Build the fpk inside a reproducible Debian 12 container.
#
#   bash build-kit/build-in-docker.sh
#
# The first run builds the image (installs dependencies and minimal FFmpeg).
# Later runs reuse it. Override with IMAGE=name:tag if needed.
set -euo pipefail

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
VERSION=$(sed -n 's/^version="\(.*\)"/\1/p' "$ROOT/packaging/fpk/hdmi-display/manifest")
[ -n "$VERSION" ] || { echo "cannot read version from manifest" >&2; exit 1; }
IMAGE=${IMAGE:-hdmi-display-build:$VERSION}

echo "==> building/refreshing build image $IMAGE"
ARGS=()
[ -n "${DEBIAN_MIRROR:-}" ] && \
    ARGS+=(--build-arg "DEBIAN_MIRROR=$DEBIAN_MIRROR")
[ -n "${DEBIAN_SECURITY_MIRROR:-}" ] && \
    ARGS+=(--build-arg "DEBIAN_SECURITY_MIRROR=$DEBIAN_SECURITY_MIRROR")
docker build "${ARGS[@]}" -f "$ROOT/build-kit/Dockerfile" -t "$IMAGE" "$ROOT"

mkdir -p "$ROOT/build"
echo "==> building fpk in $IMAGE"
docker run --rm \
    -v "$ROOT:/src" -w /src \
    -e HOME=/tmp \
    --user "$(id -u):$(id -g)" \
    "$IMAGE" bash packaging/build-fpk.sh

echo
echo "done: $ROOT/build/hdmi-display_$VERSION.fpk"
