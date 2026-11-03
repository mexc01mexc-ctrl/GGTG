#!/bin/bash
# Keeps an unstripped copy of the macOS binary of a release under APP/symbols/<UUID>/GGTG,
# so a crash report of that release can be symbolicated later (tools/symbolicate_crash.py).
# Usage: tools/save_symbols.sh [path to GGTG binary]
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${1:-$ROOT/desktop/nicegram-desktop/out/Release/GGTG.app/Contents/MacOS/GGTG}"
UUID="$(dwarfdump --uuid "$BIN" | head -1 | awk '{print $2}')"
if [ -z "$UUID" ]; then echo "no UUID in $BIN" >&2; exit 1; fi
DIR="$ROOT/APP/symbols/$UUID"
mkdir -p "$DIR"
cp "$BIN" "$DIR/GGTG"
echo "saved $UUID -> $DIR/GGTG"
