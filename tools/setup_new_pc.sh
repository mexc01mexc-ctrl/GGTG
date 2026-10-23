#!/bin/bash
set -e

# tools/setup_new_pc.sh
# Run this script on the new computer right after transferring the project.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

echo "=== GGTG: Настройка проекта на новом ПК ==="
echo "Корень проекта: $ROOT_DIR"

# 1. Update absolute paths in precompiled libraries and cmake
echo ""
echo "[1/4] Обновление путей в библиотеках и CMake..."
python3 "$ROOT_DIR/tools/relocate_paths.py" "/Users/macbook/Desktop/GGTG" "$ROOT_DIR"

# 2. Check and setup desktop symlinks if on macOS
if [[ "$OSTYPE" == "darwin"* ]]; then
    echo ""
    echo "[2/4] Проверка симлинков на Рабочем столе..."
    DESKTOP_DIR="$HOME/Desktop"
    if [ -d "$DESKTOP_DIR" ]; then
        if [ ! -L "$DESKTOP_DIR/ggtg-macos" ]; then
            ln -s "$ROOT_DIR/desktop" "$DESKTOP_DIR/ggtg-macos"
            chflags -h hidden "$DESKTOP_DIR/ggtg-macos" 2>/dev/null || true
            echo "  Создана скрытая ссылка ~/Desktop/ggtg-macos"
        fi
        if [ ! -L "$DESKTOP_DIR/ggtg-Android" ]; then
            ln -s "$ROOT_DIR/android" "$DESKTOP_DIR/ggtg-Android"
            chflags -h hidden "$DESKTOP_DIR/ggtg-Android" 2>/dev/null || true
            echo "  Создана скрытая ссылка ~/Desktop/ggtg-Android"
        fi
    fi

    # 3. Regenerate Xcode project if CMake exists
    echo ""
    echo "[3/4] Регенерация проекта Xcode через CMake..."
    if command -v cmake >/dev/null 2>&1; then
        if [ -d "$ROOT_DIR/desktop/nicegram-desktop/out" ]; then
            (cd "$ROOT_DIR/desktop/nicegram-desktop/out" && cmake .)
            echo "  Проект Xcode успешно обновлён под текущую систему."
        fi
    else
        echo "  Внимание: CMake не найден в PATH. Установите CMake (например: brew install cmake)."
    fi
else
    echo ""
    echo "[2/4] Пропуск macOS симлинков (не-macOS окружение)."
    echo "[3/4] Пропуск Xcode регенерации."
fi

# 4. Check Android JDK
echo ""
echo "[4/4] Проверка Android окружения..."
if [ -d "$ROOT_DIR/android/jdk" ]; then
    echo "  Встроенный JDK найден: $ROOT_DIR/android/jdk"
elif command -v java >/dev/null 2>&1; then
    echo "  Системная Java найдена: $(java -version 2>&1 | head -n 1)"
fi

echo ""
echo "=== Настройка завершена! ==="
echo "Для сборки macOS:   cd desktop/nicegram-desktop/out && xcodebuild -project Telegram.xcodeproj -target Telegram -configuration Release ARCHS=arm64 ONLY_ACTIVE_ARCH=YES"
echo "Для создания DMG:   ./desktop/nicegram-desktop/Telegram/build/ggtg/make_dmg.sh"
echo "Для сборки Android: ./android/build_release.sh"

