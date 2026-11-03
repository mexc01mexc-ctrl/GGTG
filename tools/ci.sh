#!/bin/bash
# GGTG release builds happen only on GitHub Actions.
#   tools/ci.sh run [android|macos|windows|all]   start builds on current remote main
#   tools/ci.sh status                            latest runs
#   tools/ci.sh get [android|macos|windows|all]   download latest successful artifacts into APP/
# Old files in APP/ move to APP/previous/. macOS symbols go to APP/symbols/<UUID>/GGTG.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
APP="$ROOT/APP"
cmd="${1:-status}"; which="${2:-all}"
wf() { case "$1" in android) echo android.yml;; macos) echo macos.yml;; windows) echo windows.yml;; esac; }
targets() { if [ "$which" = all ]; then echo android macos windows; else echo "$which"; fi; }

archive() { # move existing file to APP/previous
  mkdir -p "$APP/previous"
  [ -e "$1" ] && mv -f "$1" "$APP/previous/" || true
}

case "$cmd" in
run)
  for t in $(targets); do gh workflow run "$(wf $t)" --ref main && echo "started $t"; done ;;
status)
  for t in android macos windows; do
    echo "== $t"; gh run list --workflow "$(wf $t)" --limit 3
  done ;;
get)
  mkdir -p "$APP"
  for t in $(targets); do
    id="$(gh run list --workflow "$(wf $t)" --status success --limit 1 --json databaseId -q '.[0].databaseId')"
    [ -z "$id" ] && { echo "$t: no successful run"; continue; }
    tmp="$(mktemp -d)"
    case "$t" in
      android)
        gh run download "$id" -n GGTG-Android -D "$tmp"
        apk="$(find "$tmp" -name '*.apk' | head -1)"
        archive "$APP/GGTG-Android.apk"; mv "$apk" "$APP/GGTG-Android.apk" ;;
      macos)
        gh run download "$id" -n GGTG-macOS-DMG -D "$tmp"
        archive "$APP/GGTG.dmg"; mv "$tmp/GGTG.dmg" "$APP/GGTG.dmg"
        if gh run download "$id" -n GGTG-macOS-symbols -D "$tmp/sym" 2>/dev/null; then
          for f in "$tmp"/sym/*.gz; do
            u="$(basename "$f" .gz)"; mkdir -p "$APP/symbols/$u"
            gunzip -c "$f" > "$APP/symbols/$u/GGTG"
          done
        fi ;;
      windows)
        gh run download "$id" -n GGTG-Windows-x64 -D "$tmp"
        archive "$APP/GGTG-Windows-x64.zip"
        (cd "$tmp" && zip -qr "$APP/GGTG-Windows-x64.zip" .) ;;
    esac
    rm -rf "$tmp"; echo "$t: run $id -> APP/"
  done ;;
*) echo "usage: $0 run|status|get [android|macos|windows|all]"; exit 1 ;;
esac
