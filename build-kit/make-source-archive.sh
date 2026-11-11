#!/bin/bash
# Create a clean source + build-kit archive (no build outputs).
#
#   bash build-kit/make-source-archive.sh [output.tar.gz]
#
set -euo pipefail

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
VERSION=$(sed -n 's/^version="\(.*\)"/\1/p' "$ROOT/packaging/fpk/hdmi-display/manifest" | tr -d '\r')
[ -n "$VERSION" ] || { echo "cannot read version from manifest" >&2; exit 1; }

OUT=${1:-$ROOT/build/hdmi-display_${VERSION}_src.tar.gz}
NAME=hdmi-display-$VERSION
STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT

mkdir -p "$STAGE/$NAME"
tar -C "$ROOT" \
    --exclude=./build \
    --exclude=./.git \
    --exclude='*.o' \
    --exclude='*.d' \
    --exclude='*.fpk' \
    --exclude=./hdmi_display \
    -cf - . | tar -C "$STAGE/$NAME" -xf -

mkdir -p "$(dirname "$OUT")"
tar -C "$STAGE" -czf "$OUT" "$NAME"
sha256sum "$OUT" | tee "$OUT.sha256"

echo "source archive: $OUT"
