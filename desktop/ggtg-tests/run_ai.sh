#!/bin/bash
# Builds and runs the checks of the AI logic (core/ggtg_ai*.h) against the real
# headers, with only QtCore linked. Run from anywhere.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
Q="$ROOT/Libraries/local/Qt-6.11.1"
SRC="$ROOT/nicegram-desktop/Telegram/SourceFiles"
T="$ROOT/nicegram-desktop/Telegram"
OUT="${TMPDIR:-/tmp}/ggtg-tests"
mkdir -p "$OUT"
rm -rf "${TMPDIR:-/tmp}/ggtg-ai-test-work"
clang++ -std=c++20 -DCRL_USE_DISPATCH -Wno-deprecated-declarations -Wno-deprecated-this-capture \
	-I"$ROOT/ggtg-tests/stub" \
	-I"$Q/include" -I"$Q/include/QtCore" -I"$Q/include/QtGui" \
	-I"$Q/include/QtWidgets" -I"$Q/include/QtNetwork" -I"$SRC" \
	-I"$T/lib_base" -I"$T/lib_crl" -I"$T/lib_rpl" -I"$T/lib_ui" \
	-I"$T/ThirdParty/expected/include" -I"$T/ThirdParty/GSL/include" \
	-I"$T/ThirdParty/range-v3/include" \
	"$ROOT/ggtg-tests/ai_logic_check.cpp" \
	"$Q/lib/libQt6Core.a" "$Q/lib/libQt6BundledPcre2.a" -lz \
	-framework IOKit -framework CoreFoundation -framework Foundation \
	-framework DiskArbitration -framework AppKit \
	-framework ApplicationServices -framework CoreServices \
	-framework Security -framework UniformTypeIdentifiers \
	-o "$OUT/ai_logic_check"
"$OUT/ai_logic_check"
