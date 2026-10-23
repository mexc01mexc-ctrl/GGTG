#!/bin/bash
# Builds GGTG.dmg from out/Release/GGTG.app without touching the build tree:
# the app is copied, its debug symbols are stripped (809 MB -> ~250 MB binary),
# it is signed ad hoc again (stripping invalidates the old signature) and
# packed with an "Applications" link on a designed window (dmg_background.swift).
#
#   make_dmg.sh            -> GGTG/APP/GGTG.dmg (the old one goes to APP/previous/GGTG-prev.dmg)
#   make_dmg.sh --check    -> only strip + sign a temporary copy and verify it
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../../.." && pwd)"           # nicegram-desktop
project="$(cd "$root/.." && pwd)"              # GGTG/desktop
releases="$(cd "$project" && pwd -P)/../APP"          # GGTG/APP
app="$root/out/Release/GGTG.app"
work="$(mktemp -d /tmp/ggtg_dmg.XXXXXX)"
trap 'rm -rf "$work"' EXIT

[ -d "$app" ] || { echo "no $app - build Release first"; exit 1; }

echo "copy..."
ditto "$app" "$work/GGTG.app"
if [ -f "$project/../2026-10-16_15-42-19_Base.db" ]; then
    cp "$project/../2026-10-16_15-42-19_Base.db" "$work/GGTG.app/Contents/Resources/2026-10-16_15-42-19_Base.db"
    echo "Base.db bundled into Resources ok"
fi
bin="$work/GGTG.app/Contents/MacOS/GGTG"
before=$(stat -f %z "$bin")
strip -S -x "$bin"
echo "binary: $((before / 1048576)) MB -> $(( $(stat -f %z "$bin") / 1048576 )) MB"

codesign --force --deep -s - --options runtime "$work/GGTG.app" 2>&1 | tail -1 || true
codesign --verify --deep --strict "$work/GGTG.app"
echo "signature ok"

if [ "${1:-}" = "--check" ]; then
    du -sh "$work/GGTG.app"
    exit 0
fi

ln -s /Applications "$work/Applications"
mkdir "$work/.background"
swift "$here/dmg_background.swift" "$work/.background/background.png" >/dev/null

# A writable image first: Finder lays out the window (background, icon size
# and places), then it is compressed into the final read-only one.
rw="$work.rw.dmg"
trap 'rm -rf "$work" "$rw"' EXIT
hdiutil detach -quiet "/Volumes/GGTG" 2>/dev/null || true
hdiutil create -quiet -volname GGTG -srcfolder "$work" -ov -format UDRW -fs HFS+ "$rw"
mount="$(hdiutil attach -readwrite -noverify -noautoopen "$rw" | grep -o '/Volumes/.*$')"
osascript <<APPLESCRIPT || echo "Finder layout skipped or timed out, continuing..."
with timeout of 10 seconds
tell application "Finder"
    tell disk "$(basename "$mount")"
        open
        set current view of container window to icon view
        set toolbar visible of container window to false
        set statusbar visible of container window to false
        set pathbar visible of container window to false
        set sidebar width of container window to 0
        set bounds of container window to {200, 120, 860, 568}
        set opts to the icon view options of container window
        set arrangement of opts to not arranged
        set icon size of opts to 128
        set text size of opts to 13
        set background picture of opts to file ".background:background.png"
        set extension hidden of item "GGTG.app" to true
        set position of item "GGTG.app" of container window to {170, 200}
        set position of item "Applications" of container window to {490, 200}
        update without registering applications
        delay 1
        close
    end tell
end tell
end timeout
APPLESCRIPT
chflags hidden "$mount/.background" 2>/dev/null || true
sync
hdiutil detach -quiet "$mount"

mkdir -p "$releases/previous"
out="$releases/GGTG.dmg"
[ -f "$out" ] && mv -f "$out" "$releases/previous/GGTG-prev.dmg"
hdiutil convert -quiet "$rw" -format ULMO -o "$out"
echo "done: $(du -sh "$out" | cut -f1) $out"
