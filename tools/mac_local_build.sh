#!/bin/bash
# Local macOS DMG build: the same recipe as .github/workflows/macos.yml, but incremental,
# so after the first (full) build a change in a few files takes minutes, not an hour.
#
#   tools/mac_local_build.sh            build + DMG into APP/GGTG.dmg
#   tools/mac_local_build.sh deps       only fetch/unpack the prebuilt libraries (first run does it)
#   tools/mac_local_build.sh nodmg      build only
#
# Needs: full Xcode (xcode-select -s /Applications/Xcode.app), git. ccache is used when installed (brew install ccache).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/desktop/nicegram-desktop/out"
LIBS="$ROOT/desktop/Libraries"
MODE="${1:-all}"

if ! xcodebuild -version >/dev/null 2>&1; then
  echo "Нужен полный Xcode: установите его и выполните" >&2
  echo "  sudo xcode-select -s /Applications/Xcode.app/Contents/Developer && sudo xcodebuild -license accept" >&2
  exit 1
fi

# --- 1. Prebuilt libraries + generated Xcode project (once; the CI keeps them in the macos-deps-storage branch)
if [ ! -d "$LIBS" ] || [ ! -d "$OUT/Telegram.xcodeproj" ]; then
  echo "=== Зависимости: скачиваем один раз (несколько ГБ) ==="
  TMP="$(mktemp -d)"
  git -C "$ROOT" clone --depth 1 --branch macos-deps-storage "$(git -C "$ROOT" remote get-url origin)" "$TMP/deps"
  cat "$TMP"/deps/macos_deps.tar.gz.part_* | tar -xzf - -C "$ROOT"
  rm -rf "$TMP"
fi
[ "$MODE" = "deps" ] && { echo "Зависимости на месте."; exit 0; }

# --- 1b. Qt host tools (moc, rcc, uic...). The prebuilt deps carry only stubs for them: libexec/rcc is a 175-byte
# script that "generates" EMPTY .rcc files (the app then dies at start in RegisterBundledResources - fonts, icons
# and emoji are missing), and moc & co. are links into /tmp/qt-clean, which a reboot wipes. A real Qt host install
# (aqtinstall, as the CI does) lives in ~/.ggtg-qt-host; libexec/ and bin/ get small wrappers that run those.
QT="$LIBS/local/Qt-6.11.2"
QTHOST="${GGTG_QT_HOST:-$HOME/.ggtg-qt-host}/6.11.2/macos"
tools_ok() { [ -f "$QT/libexec/rcc" ] && [ "$(stat -f%z "$QT/libexec/rcc")" -lt 1000 ] && grep -q "$QTHOST" "$QT/libexec/rcc"; }
if ! tools_ok; then
  echo "=== Qt host tools: ставим настоящие moc/rcc/uic (в deps лежат заглушки) ==="
  if [ ! -x "$QTHOST/libexec/rcc" ]; then
    python3 -m pip install --user aqtinstall
    python3 -m aqt install-qt mac desktop 6.11.2 clang_64 --archives qtbase -O "${GGTG_QT_HOST:-$HOME/.ggtg-qt-host}"
  fi
  [ -x "$QTHOST/libexec/rcc" ] || { echo "Не удалось поставить Qt host tools ($QTHOST)" >&2; exit 1; }
  for sub in libexec bin; do
    mkdir -p "$QT/$sub"
    for f in "$QTHOST/$sub"/*; do
      [ -f "$f" ] && [ -x "$f" ] || continue
      name="$(basename "$f")"
      rm -rf "$QT/$sub/$name"
      printf '#!/bin/sh\nexec "%s" "$@"\n' "$f" > "$QT/$sub/$name"
      chmod +x "$QT/$sub/$name"
    done
  done
  # the resource packs made with the stub are empty: make them again
  rm -f "$OUT"/*.rcc
fi

# --- 2. The project files hold absolute paths of the machine they were generated on (see the CI):
# make them resolve here.
LINK=/Users/macbookair/Desktop/GGTG
if [ "$ROOT" != "$LINK" ] && [ "$(readlink "$LINK" 2>/dev/null)" != "$ROOT" ]; then
  sudo ln -sfn "$ROOT" "$LINK"
fi

# the debug libraries the project asks for are the release ones
find "$LIBS" -name "*.a" ! -name "*_debug.a" | while read -r lib; do
  dir=$(dirname "$lib"); base=$(basename "$lib" .a)
  [ -e "$dir/${base}_debug.a" ] || ln -sf "$(basename "$lib")" "$dir/${base}_debug.a"
done

# no CMake regeneration (it would need the whole toolchain the deps were built with)
printf 'all:\n\t@echo "Skipping CMake regeneration"\n' > "$OUT/CMakeScripts/ReRunCMake.make"

# --- 3. Build (incremental: Xcode keeps the objects in out/, ccache keeps the rest)
CC_ARGS=()
if command -v ccache >/dev/null 2>&1; then
  W=/tmp/ggtg_ccache_wrappers; mkdir -p "$W"
  printf '#!/bin/bash\nexec ccache /usr/bin/clang "$@"\n' > "$W/clang"
  printf '#!/bin/bash\nexec ccache /usr/bin/clang++ "$@"\n' > "$W/clang++"
  chmod +x "$W/clang" "$W/clang++"
  CC_ARGS=(CC="$W/clang" CXX="$W/clang++")
fi

cd "$OUT"
xcodebuild -project Telegram.xcodeproj -target Telegram -configuration Release \
  ARCHS=arm64 ONLY_ACTIVE_ARCH=YES CODE_SIGNING_ALLOWED=NO "${CC_ARGS[@]+"${CC_ARGS[@]}"}"

[ "$MODE" = "nodmg" ] && { echo "Собрано: $OUT/Release/GGTG.app"; exit 0; }

# --- 4. DMG
chmod +x "$ROOT/desktop/nicegram-desktop/Telegram/build/ggtg/make_dmg.sh"
"$ROOT/desktop/nicegram-desktop/Telegram/build/ggtg/make_dmg.sh"
ls -lh "$ROOT/APP/GGTG.dmg"
