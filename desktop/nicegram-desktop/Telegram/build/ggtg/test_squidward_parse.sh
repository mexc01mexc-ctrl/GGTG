#!/bin/bash
# Builds and runs the Сквидвард reply-parser checks against the checkout's
# own static Qt Core - no app build needed.
set -euo pipefail
root="$(cd "$(dirname "$0")/../../.." && pwd)"
qt="$(ls -d "$root/../Libraries/local/Qt-"* | head -1)"
src="$root/Telegram/SourceFiles"
out="${TMPDIR:-/tmp}/ggtg_squidward_parse_test"
clang++ -std=c++20 -O0 -g -Wno-deprecated-declarations \
	-I"$src" -I"$qt/include" -I"$qt/include/QtCore" \
	"$src/window/ggtg_squidward_parse.cpp" \
	"$src/window/ggtg_squidward_parse_test.cpp" \
	"$qt/lib/libQt6Core.a" "$qt/lib/libQt6BundledPcre2.a" \
	-lz -framework IOKit -framework CoreFoundation -framework Foundation \
	-framework DiskArbitration -framework AppKit \
	-framework ApplicationServices -framework CoreServices \
	-framework Security -framework UniformTypeIdentifiers \
	-o "$out"
"$out"
