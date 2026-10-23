# GGTG

Telegram-клиент для работы со множеством аккаунтов: рассылки по базам, сбор аудитории,
накрутка, ИИ-помощник. Платформы: macOS, Windows, Android.

| Папка | Что там |
|---|---|
| `desktop/` | macOS и Windows (один исходник, C++/Qt) |
| `android/` | Android (Java/Kotlin) |
| `windows/` | как собрать exe на Windows |
| `APP/` | все сборки: `GGTG.dmg`, `GGTG-Android.apk`, `GGTG-windows-src.zip`; прошлые версии в `APP/previous` |
| `docs/` | форматы данных, ИИ, сборка, указатель файлов |
| `tools/` | служебные скрипты |
| `backups/` | старые копии (можно удалить, если не нужны) |

**Разработчику или ИИ:** начните с [`PROJECT_MAP.md`](PROJECT_MAP.md): там устройство
проекта, сборка, соглашения и где что искать. Указатель всех GGTG-файлов:
[`docs/FILE_INDEX.md`](docs/FILE_INDEX.md).

Быстрые команды:

```bash
desktop/nicegram-desktop/Telegram/build/ggtg/make_dmg.sh          # APP/GGTG.dmg
android/build_release.sh                                          # APP/GGTG-Android.apk
desktop/nicegram-desktop/Telegram/build/ggtg/make_windows_zip.sh  # APP/GGTG-windows-src.zip
python3 tools/update_file_index.py                                # обновить docs/FILE_INDEX.md
```
