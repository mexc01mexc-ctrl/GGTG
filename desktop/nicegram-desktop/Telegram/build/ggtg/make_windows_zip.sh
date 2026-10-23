#!/bin/bash
# Packs the desktop sources for the Windows build (windows/BUILD_WINDOWS_RU.md):
# the nicegram-desktop tree without the macOS build folder, git data and
# generated graphs - the same layout the Windows machine unpacks over
# D:\TBuild\tdesktop.
#
#   make_windows_zip.sh -> GGTG/APP/GGTG-windows-src.zip
#                          (the old one goes to APP/previous/GGTG-windows-src-prev.zip)
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../../.." && pwd -P)"        # GGTG/desktop/nicegram-desktop
releases="$root/../../APP"                     # GGTG/APP
mkdir -p "$releases/previous"
out="$releases/GGTG-windows-src.zip"
tmp="$(mktemp -u /tmp/ggtg_win.XXXXXX).zip"

cd "$root/.."
zip -qr -X "$tmp" nicegram-desktop \
    -x "nicegram-desktop/out/*" \
    -x "*/.git/*" -x "*/.git" \
    -x "*graphify-out/*" \
    -x "*.DS_Store"
[ -f "$out" ] && mv -f "$out" "$releases/previous/GGTG-windows-src-prev.zip"
mv "$tmp" "$out"
echo "done: $(du -sh "$out" | cut -f1) $(unzip -l "$out" | tail -1 | awk '{print $2}') files $out"
