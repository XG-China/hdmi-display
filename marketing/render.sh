#!/bin/bash
# Re-render the PNG posters from the HTML/CSS sources.
# Requires chromium/chromium-browser (headless) and Chinese fonts.
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"

CHROME=${CHROME:-$(command -v chromium || command -v chromium-browser || command -v google-chrome || true)}
[ -n "$CHROME" ] || { echo "chromium not found (set CHROME=...)" >&2; exit 1; }

shot() {
    local page="$1" size="$2" out="$3"
    "$CHROME" --headless=new --no-sandbox --disable-gpu --disable-dev-shm-usage \
        --hide-scrollbars --force-device-scale-factor=1 \
        --window-size="$size" --virtual-time-budget=1500 \
        --screenshot="$out" "file://$(pwd)/$page" >/dev/null 2>&1
    echo "$out"
}

shot poster-1.html 1600,900 poster-1.png
shot poster-2.html 1080,1080 poster-2.png
shot poster-3.html 1600,900 poster-3.png
shot poster-4.html 1080,1920 poster-4.png
