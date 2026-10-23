#!/bin/bash
set -e

# tools/create_transfer_archive.sh
# Packages GGTG into a compact, ready-to-transfer archive.
# Usage:
#   ./tools/create_transfer_archive.sh          # Full development bundle (~3.5-4 GB compressed)
#   ./tools/create_transfer_archive.sh --source # Pure source code only (~500 MB compressed)

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
PARENT_DIR="$(cd "$ROOT_DIR/.." && pwd)"
DIR_NAME="$(basename "$ROOT_DIR")"
OUT_DIR="$ROOT_DIR/APP"
mkdir -p "$OUT_DIR"

MODE="full"
if [ "$1" == "--source" ] || [ "$1" == "-s" ]; then
    MODE="source"
fi

# Run cleaning first
bash "$ROOT_DIR/tools/prepare_for_transfer.sh"

echo ""
if [ "$MODE" == "source" ]; then
    ARCHIVE_NAME="GGTG-source-only-$(date +%Y%m%d).tar.gz"
    ARCHIVE_PATH="$OUT_DIR/$ARCHIVE_NAME"
    echo "Создание архива исходного кода: $ARCHIVE_PATH ..."

    tar --exclude="*/.git" \
        --exclude="*/build" \
        --exclude="*/.cxx" \
        --exclude="*/.gradle" \
        --exclude="*/out/build" \
        --exclude="$DIR_NAME/desktop/Libraries" \
        --exclude="$DIR_NAME/backups" \
        --exclude="$DIR_NAME/APP/*.dmg" \
        --exclude="$DIR_NAME/APP/*.apk" \
        --exclude="$DIR_NAME/APP/*.tar.gz" \
        --exclude="*.DS_Store" \
        -czf "$ARCHIVE_PATH" \
        -C "$PARENT_DIR" "$DIR_NAME"

    echo "Архив успешно создан: $ARCHIVE_PATH"
    ls -lh "$ARCHIVE_PATH"
else
    ARCHIVE_NAME="GGTG-transfer-ready-$(date +%Y%m%d).tar.gz"
    ARCHIVE_PATH="$OUT_DIR/$ARCHIVE_NAME"
    echo "Создание готового к сборке архива (с библиотеками): $ARCHIVE_PATH ..."

    tar --exclude="*/.git" \
        --exclude="*/build" \
        --exclude="*/.cxx" \
        --exclude="*/.gradle" \
        --exclude="*/out/build" \
        --exclude="*/out/Debug*" \
        --exclude="$DIR_NAME/desktop/Libraries/qt_6.11.2" \
        --exclude="$DIR_NAME/backups" \
        --exclude="$DIR_NAME/APP/*.dmg" \
        --exclude="$DIR_NAME/APP/*.apk" \
        --exclude="$DIR_NAME/APP/*.tar.gz" \
        --exclude="*.DS_Store" \
        -czf "$ARCHIVE_PATH" \
        -C "$PARENT_DIR" "$DIR_NAME"

    echo "Архив успешно создан: $ARCHIVE_PATH"
    ls -lh "$ARCHIVE_PATH"
fi

echo ""
echo "Готово! На новом ПК распакуйте архив и запустите:"
echo "  ./tools/setup_new_pc.sh"

