#!/bin/bash
# Checks the GGTG sources against Qt 5.15 (what the Windows build uses) on the Mac,
# so a Qt 6-only call is caught here and not on Windows. Needs: brew install qt@5,
# and a finished Mac build (it borrows the compile flags from out/).
#   ggtg-tests/check_windows_qt5.sh            - every file changed in the last 30 days
#   ggtg-tests/check_windows_qt5.sh a.cpp b.cpp - only these (paths from SourceFiles)
set -u
root="$(cd "$(dirname "$0")/.." && pwd)"
src="$root/nicegram-desktop/Telegram/SourceFiles"
qt5=/opt/homebrew/opt/qt@5
work="${TMPDIR:-/tmp}/ggtg_qt5check"
mkdir -p "$work"
# The flags of the app's own sources (the folder also has those of its libraries).
resp=$(grep -l -- "-std=gnu++20" "$root"/nicegram-desktop/out/build/Telegram.build/Release/Objects-normal/arm64/*-common-args.resp \
    | xargs grep -l "Telegram/SourceFiles" | xargs grep -l "DQT_WIDGETS_LIB" | head -1)
[ -d "$qt5" ] || { echo "Нужен Qt 5: brew install qt@5"; exit 1; }
[ -f "$resp" ] || { echo "Сначала соберите Mac-версию (нет $resp)"; exit 1; }
python3 - "$resp" "$qt5" "$work/args.resp" <<'PY'
import sys, shlex
r, q, out = sys.argv[1:]
a = shlex.split(open(r).read()); new = []; i = 0
while i < len(a):
    x = a[i]
    if x in ('-isystem', '-I', '-F', '-iframework') and i + 1 < len(a) and 'Qt-6' in a[i + 1]:
        i += 2; continue
    if 'Qt-6' in x:
        i += 1; continue
    new.append(x); i += 1
qt = [f'-F{q}/lib', '-isystem', f'{q}/include']
for m in ['QtCore', 'QtGui', 'QtWidgets', 'QtNetwork', 'QtSvg', 'QtDBus', 'QtOpenGL']:
    h = f'{q}/lib/{m}.framework/Headers'
    qt += ['-isystem', h, '-isystem', f'{h}/5.15.19', '-isystem', f'{h}/5.15.19/{m}']
qt += ['-isystem', f'{q}/mkspecs/macx-clang']
open(out, 'w').write(' '.join(shlex.quote(x) for x in new + qt))
PY
cd "$src"
if [ $# -gt 0 ]; then
    printf '%s\n' "$@" > "$work/files.txt"
else
    find . -name "*.cpp" -mtime -30 | grep -v "/mac/\|/linux/\|/win/\|_mac\.\|_linux\.\|_win\.\|_test\.cpp" > "$work/files.txt"
fi
sdk=$(xcrun --show-sdk-path)
check() {
    clang++ -x c++ -std=gnu++2b -fsyntax-only -ferror-limit=50 -target arm64-apple-macos11 \
        -isysroot "$sdk" @"$work/args.resp" -include "$src/stdafx.h" "$src/$1" 2>&1 | grep "error:"
}
export -f check; export work src sdk
echo "Проверяю $(wc -l < "$work/files.txt") файлов на Qt 5..."
xargs -P 8 -I{} bash -c 'check "$@"' _ {} < "$work/files.txt" \
    | sed "s#$src/##; s#^\./##" | sort -u \
    | grep -v "platform/mac/\|_mac\.h\|application.cpp:1883\|mtproto_tcp_socket.h\|mtproto_tls_socket.h\|mtproto_abstract_socket.cpp\|iv_editor_window.h:68\|image_source" \
    > "$work/errors.txt"
if [ -s "$work/errors.txt" ]; then
    echo "Ошибки, которые будут на Windows:"; cat "$work/errors.txt"; exit 1
fi
echo "OK: под Qt 5 ошибок нет."
