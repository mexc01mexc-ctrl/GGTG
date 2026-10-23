#!/bin/bash
set -e

# tools/prepare_for_transfer.sh
# Cleans huge temporary build artifacts, compiler caches, and intermediate trees
# so the project can be transferred to another computer without weighing 60+ GB.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

echo "=== GGTG: Подготовка проекта к переносу ==="
echo "Корень проекта: $ROOT_DIR"
echo ""

# 1. Clean Android build artifacts
echo "[1/4] Очистка кэша сборки Android..."
rm -rf "$ROOT_DIR/android/Telegram-GGTG/TMessagesProj/build"
rm -rf "$ROOT_DIR/android/Telegram-GGTG/TMessagesProj/.cxx"
rm -rf "$ROOT_DIR/android/Telegram-GGTG/TMessagesProj_AppStandalone/build"
rm -rf "$ROOT_DIR/android/Telegram-GGTG/TMessagesProj_App/build"
rm -rf "$ROOT_DIR/android/Telegram-GGTG/TMessagesProj_AppHockeyApp/build"
rm -rf "$ROOT_DIR/android/Telegram-GGTG/TMessagesProj_AppHuawei/build"
rm -rf "$ROOT_DIR/android/Telegram-GGTG/TMessagesProj_AppTests/build"
rm -rf "$ROOT_DIR/android/Telegram-GGTG/TMessagesProj_Modules/media3"/*/build
rm -rf "$ROOT_DIR/android/Telegram-GGTG/.gradle"
rm -rf "$ROOT_DIR/android/Telegram-GGTG/build"
echo "  Очищено ~4.3 ГБ кэшей Android (будут пересозданы при gradle assemble)."

# 2. Clean Desktop temporary object files in out/build
echo ""
echo "[2/4] Очистка промежуточных объектных файлов Desktop (.o, .hxx, .dia)..."
if [ -d "$ROOT_DIR/desktop/nicegram-desktop/out/build" ]; then
    rm -rf "$ROOT_DIR/desktop/nicegram-desktop/out/build"
    echo "  Очищено ~9 ГБ объектных файлов out/build (регенерируются при сборке за 1–2 мин)."
fi

# 3. Clean temporary logs and temp files
echo ""
echo "[3/4] Очистка логов и временных файлов..."
rm -f "$ROOT_DIR/desktop/prepare.log"
rm -f "$ROOT_DIR/desktop/xcodebuild.log"
find "$ROOT_DIR" -name ".DS_Store" -delete 2>/dev/null || true
echo "  Удалены временные логи."

# 4. Check qt_6.11.2 source build tree (26 GB)
QT_BUILD_TREE="$ROOT_DIR/desktop/Libraries/qt_6.11.2"
if [ -d "$QT_BUILD_TREE" ]; then
    echo ""
    echo "[4/4] Обнаружено дерево сборки Qt 6.11.2 ($QT_BUILD_TREE): 26 ГБ."
    echo "  Примечание: Установленный Qt для линковки Telegram уже находится в"
    echo "  desktop/Libraries/local/Qt-6.11.2 (8.4 ГБ). Исходное дерево сборки qt_6.11.2"
    echo "  не используется CMake и может быть безопасно удалено для экономии 26 ГБ."
fi

echo ""
echo "=== Очистка завершена! ==="
echo "Текущий размер папки:"
du -sh "$ROOT_DIR" 2>/dev/null || true
echo ""
echo "Памятка по переносу:"
echo "1. Скопируйте папку GGTG на новый компьютер."
echo "2. На новом компьютере запустите: ./tools/setup_new_pc.sh"
echo "   (скрипт автоматически обновит пути к библиотекам и настроит проект)."

