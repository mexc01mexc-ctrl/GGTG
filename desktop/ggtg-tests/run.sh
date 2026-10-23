#!/bin/bash
# Builds and runs the QtCore-only tests of GGTG's pure logic without the app.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
Q="$(ls -d "$ROOT"/Libraries/local/Qt-6.11.* 2>/dev/null | sort -V | tail -n 1)"
SRC="$ROOT/nicegram-desktop/Telegram/SourceFiles"
OUT="${TMPDIR:-/tmp}/ggtg-tests"
mkdir -p "$OUT"
for test in "$ROOT"/ggtg-tests/*_test.cpp; do
	name="$(basename "$test" .cpp)"
	clang++ -std=c++20 -DGGTG_MODEL_STANDALONE \
		-I"$Q/include" -I"$Q/include/QtCore" -I"$SRC" \
		"$test" "$Q/lib/libQt6Core.a" "$Q/lib/libQt6BundledPcre2.a" -lz \
		-framework IOKit -framework CoreFoundation -framework Foundation \
		-framework DiskArbitration -framework AppKit \
		-framework ApplicationServices -framework CoreServices \
		-framework Security -framework UniformTypeIdentifiers \
		-o "$OUT/$name"
	"$OUT/$name"
done
