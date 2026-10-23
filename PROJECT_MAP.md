# GGTG: карта проекта

Документ для разработчика или ИИ, который открыл проект впервые. Здесь описано, что
это за продукт, где что лежит, как собирать и о чём нельзя забывать. Подробности по
отдельным файлам собраны в [`docs/FILE_INDEX.md`](docs/FILE_INDEX.md).

## 1. Что такое GGTG

GGTG — это Telegram-клиент для работы со множеством аккаунтов: рассылки по базам,
сбор аудитории, накрутка, ИИ-помощник для ответов. Есть три платформы:

| Платформа | Что это | Язык | Исходники |
|---|---|---|---|
| macOS | форк Telegram Desktop 7.3.1 (через Nicegram) | C++20, Qt 6 | `desktop/nicegram-desktop` |
| Windows | **тот же исходник**, что и macOS, собирается на Windows (Qt 5) | C++ | `desktop/nicegram-desktop` → архив в `APP/` |
| Android | форк Telegram Android 12.10.6 | Java (Telegram) + Kotlin (GGTG) | `android/Telegram-GGTG` |

Mac и Android повторяют друг друга по функциям и форматам данных (паритет). Новую
функцию, как правило, делают на обеих платформах; общие форматы описаны в
[`docs/ggtg-formats.md`](docs/ggtg-formats.md).

### Разделы приложения (окно GGTG на Mac, вкладка GGTG на Android)

| Раздел | Что делает | Mac | Android |
|---|---|---|---|
| Главная | приветствие, плитки с цифрами, что идёт сейчас, лента ответов | `window/ggtg_home_widgets.h` | `GgtgHome.kt` |
| Аккаунты → Список | аккаунты по папкам, статус, прокси, массовые действия | `settings/bulk_accounts_actions.cpp` | `GgtgAccountManagerActivity.kt`, `GgtgBulk*.kt` |
| Аккаунты → Добавить | импорт tdata, вход по номеру, приём с другого устройства | `settings/ggtg_add_accounts.h` | `GgtgAddAccountsActivity.kt` |
| Аккаунты → Прокси | именованные списки запасных прокси (пулы) | `mtproto/proxy_pool.*`, `boxes/connection_box.cpp` | `GgtgProxy*.kt` |
| Аккаунты → Экспорт | `.ggtg` для Android, tdata | `settings/ggtg_export.*` | `GgtgTdataExport*.kt` |
| Сквидвард | поиск через бота @skvidvarde_bot и сохранение в базу | `window/ggtg_squidward*.cpp` | `GgtgSquidward*.kt`, `SquidwardParse.kt` |
| База | списки людей и групп, импорт, поиск, чистка | `data/scraped_members_db.*` | `GgtgDb.kt`, `GgtgBase*.kt` |
| Списки чатов | свои списки переписок, «Написать всему списку» | `settings/ggtg_chat_lists_ui.h`, `settings/ggtg_list_send.h` | `ai/GgtgChatList*.kt` |
| Рассылка | личные сообщения по базам с нескольких аккаунтов | `settings/sms_campaign*.{cpp,h}` | `GgtgSms*.kt` |
| Накрутка | просмотры постов, вступление в каналы | `settings/views_campaign.*` | `GgtgViews*.kt` |
| Пуш | пока пустой раздел, функции добавятся позже | `window/window_main_menu.cpp` (секция `push`) | `GgtgPushActivity.kt` |
| Процессы | все долгие операции с живым журналом | `core/process_manager.h`, `settings/process_box.*` | `GgtgProcess*.kt` |
| ИИ | ключи бесплатных облачных моделей, профиль, материалы, обучение, лента ответов, агент | `core/ggtg_ai*.h`, `settings/ggtg_ai*.h` | `ai/GgtgAi*.kt` |
| Синхронизация | данные между Mac и телефоном через «Избранное» главного аккаунта | `core/ggtg_sync*`, `settings/ggtg_sync_box.h` | `GgtgSync*.kt` |

Названия «Рассылка» и «SMS» в коде означают одно и то же: это личные сообщения в
Telegram, а не SMS.

## 2. Структура папки

```
GGTG/
├── PROJECT_MAP.md        ← этот файл
├── README.md             ← коротко для человека
├── AGENTS.md, CLAUDE.md  ← указатели для ИИ-агентов на этот файл
├── desktop/              ← macOS + Windows (один исходник)
│   ├── nicegram-desktop/ ← исходники (git-репозиторий, форк tdesktop)
│   │   └── Telegram/
│   │       ├── SourceFiles/    ← весь код приложения (GGTG-файлы: см. FILE_INDEX)
│   │       ├── build/ggtg/     ← наши скрипты: make_dmg.sh, make_windows_zip.sh, фон DMG
│   │       └── lib_ui/ …       ← библиотеки Telegram (git-подмодули)
│   │   └── out/          ← сборка Xcode (генерируется, не трогать руками)
│   ├── Libraries/        ← собранные зависимости (Qt 6.11, ffmpeg, openssl…), 77 ГБ
│   ├── ThirdParty/       ← сторонние исходники для сборки
│   └── ggtg-tests/       ← тесты чистой логики без приложения (run.sh)
├── android/
│   ├── Telegram-GGTG/    ← исходники Android (Gradle)
│   │   └── TMessagesProj/src/main/java/
│   │       ├── app/ggtg/          ← весь код GGTG (Kotlin)
│   │       └── org/telegram/…     ← код Telegram (точечные правки помечены ng:/ggtg:)
│   ├── build_release.sh  ← сборка APK с проверками
│   ├── jdk/              ← JDK 17 для сборки
│   └── keys/             ← ключи подписи (не публиковать)
├── windows/
│   ├── BUILD_WINDOWS_RU.md ← пошаговая сборка exe на Windows-машине
│   └── patch/            ← патч совместимости
├── APP/                  ← все сборки: GGTG.dmg, GGTG-Android.apk,
│                           GGTG-windows-src.zip; прошлые версии в previous/
├── docs/                 ← форматы данных, ИИ, сборка Android, указатель файлов, планы
├── tools/                ← update_file_index.py, граф кода (graphify-out)
└── backups/              ← архив исходников и копии tdata на всякий случай
```

**Скрытые ссылки.** На Рабочем столе остались скрытые ссылки `ggtg-macos → GGTG/desktop`
и `ggtg-Android → GGTG/android`. Они нужны, потому что кэш сборки Xcode/CMake и
собранные `Libraries` содержат абсолютные пути `/Users/yansheva/Desktop/ggtg-macos/...`.
Не удаляйте эти ссылки, пока сборка не перенастроена с нуля под новый путь (это
полная пересборка на несколько часов). Скрыты флагом `chflags -h hidden`.

## 3. Сборка

| Что | Команда | Результат |
|---|---|---|
| Mac, инкрементально (обычно 1–3 мин) | `cd desktop/nicegram-desktop/out && xcodebuild -project Telegram.xcodeproj -target Telegram -configuration Release ARCHS=arm64 ONLY_ACTIVE_ARCH=YES` | `desktop/nicegram-desktop/out/Release/GGTG.app` |
| Mac, DMG | `desktop/nicegram-desktop/Telegram/build/ggtg/make_dmg.sh` | `APP/GGTG.dmg` (удаляет отладочные символы, подписывает, рисует окно) |
| Установить на этот Mac | закрыть GGTG, `ditto out/Release/GGTG.app /Applications/GGTG.app` | |
| Android APK (3–5 мин) | `android/build_release.sh` | `APP/GGTG-Android.apk` (проверяет testOnly, подпись, пакет) |
| Windows | `desktop/nicegram-desktop/Telegram/build/ggtg/make_windows_zip.sh`, дальше на Windows по `windows/BUILD_WINDOWS_RU.md` | `APP/GGTG-windows-src.zip` |
| Тесты логики | `desktop/ggtg-tests/run.sh` | |

Не запускайте Debug-сборку Mac: она пересобирает всё для двух архитектур.
Новые `.cpp`/`.h` в Mac-проекте нужно добавить в `Telegram/CMakeLists.txt`, а потом
перегенерировать `out/` (конфигурация описана в `desktop/nicegram-desktop/docs`).
Самые простые для добавления — header-only файлы (`settings/ggtg_*.h`): так сделана
большая часть GGTG.

## 4. Где хранятся данные

| | Mac | Android |
|---|---|---|
| Данные Telegram | `~/Library/Application Support/NiceGram Desktop/tdata` | данные приложения `app.ggtg.android.web` |
| Данные GGTG (базы, «уже написали», ИИ, журналы) | `tdata/ggtg_db/` (`core/ggtg_paths.h`) | `filesDir/ggtg_bulk_db/` (`GgtgDb.kt`) |
| Журнал GGTG | `tdata/ggtg_db/ggtg.log` (`core/ggtg_log.h`) | «Процессы» |
| Журнал Telegram | `~/Library/Application Support/NiceGram Desktop/log.txt` | logcat |

Форматы баз, `.ggtg`, синхронизации и прочего: [`docs/ggtg-formats.md`](docs/ggtg-formats.md).
ИИ и его ключи: [`docs/ggtg-ai-cloud.md`](docs/ggtg-ai-cloud.md).

## 5. Ключевые механизмы (что важно понимать)

- **Каждый аккаунт работает только через свой прокси.** Прокси хранится в аккаунте
  (Mac: `Main::Account::proxyOverride()`, Android: `UserConfig.ncgProxy*`). Без прокси
  аккаунт не подключается. Глобальный список прокси Telegram скрыт: экран «Прокси» и
  индикатор подключения показывают прокси аккаунта (`boxes/connection_box.cpp`,
  `ProxyListActivity.java`). Новые аккаунты берут прокси из именованных пулов.
- **Подмена устройства и языка.** Каждому аккаунту подбираются язык и часовой пояс по
  стране его прокси (Mac: `Settings::AutoDeviceGeo` в `bulk_accounts_actions.cpp`,
  Android: `GgtgDevice.kt`). Окно «сменить язык» от Telegram отключено.
- **Новые сессии.** Аккаунт копируется на другое устройство новой независимой сессией
  (`main/session_duplicate.*`, `GgtgTdataImport.kt`). На Android сначала пробуется вход
  по коду, как у настоящего Telegram Android: код читается временной сессией из
  сообщений 777000. Если не вышло, используется login token с `api_id` Telegram
  Desktop (2040): `api_id` 4 Telegram отклоняет с `API_ID_PUBLISHED_FLOOD`. Свой
  `api_id` можно задать в «Синхронизации».
- **Синхронизация.** Каждое устройство кладёт зашифрованный снимок данных в
  «Избранное» главного аккаунта (`ggtg-sync-<device>-<keyId>.bin`, AES-256-GCM).
  Телефон подключается по QR-коду с Mac. `keyId` — отпечаток ключа: устройства открывают
  только файлы со своим ключом. Перед рассылкой устройство сначала забирает, кому уже
  написали с другого устройства, чтобы одному человеку не написали дважды.
- **«Уже написали».** Это общий реестр отправленных (`sms_sent.json`). Его учитывают
  рассылки на всех аккаунтах и устройствах.
- **Процессы.** Каждая долгая операция регистрируется в `ProcessManager` / `GgtgProcess`
  и видна в «Процессах» с журналом.

## 6. Соглашения в коде

- Правки GGTG в чужом коде Telegram помечаются комментарием `// ng:` или `// ggtg:`.
  Новые функции лучше выносить в отдельные `ggtg_*`-файлы, а не размазывать по Telegram.
- В начале каждого GGTG-файла комментарий: что это и зачем. По нему строится
  `docs/FILE_INDEX.md` (`python3 tools/update_file_index.py`).
- Тексты интерфейса GGTG написаны по-русски прямо в коде (без lang-ключей Telegram).
- **Никаких эмодзи вместо иконок.** Только иконки Telegram: на Mac `st::menuIcon*`
  (`Ggtg::ActionRow`, `GlyphButton`), на Android `R.drawable.msg_*` через
  `GgtgUi.glyphView`/`iconRes`. Текстовые ✓/✗ допустимы в журналах и результатах.
- Mac-код должен собираться и на Windows с Qt 5: не используйте `Qt::StringLiterals`
  и другое, что есть только в Qt 6.
- Стиль C++ — как в Telegram Desktop (табы, `rpl`, `base::`, `crl::`); подробные правила
  в `desktop/nicegram-desktop/AGENTS.md` (раздел про задачи `ai-tdesktop` к GGTG не относится).
- Паритет: поменяли формат или поведение на одной платформе — проверьте вторую и
  обновите `docs/ggtg-formats.md`.

## 7. С чего начать типичную задачу

| Задача | Куда смотреть |
|---|---|
| Новый раздел окна GGTG (Mac) | `window/window_main_menu.cpp` (список разделов, `OpenGgtgSection`), виджеты в `window/ggtg_widgets.h` |
| Новый экран GGTG (Android) | `GgtgHubActivity.kt`, `GgtgHome.kt`, общий вид в `GgtgUi.kt` |
| Логика рассылки | Mac: `settings/sms_campaign.cpp` + модель `settings/sms_campaign_model.h`; Android: `GgtgSms.kt`, `GgtgSmsRun.kt` |
| Аккаунты, массовые действия, прокси аккаунта | `settings/bulk_accounts_actions.cpp` / `GgtgAccountManagerActivity.kt`, `GgtgBulk*.kt` |
| Перенос аккаунтов между устройствами | `settings/ggtg_export.cpp`, `main/session_duplicate.cpp` / `GgtgPackImporter.kt`, `GgtgTdataImport.kt` |
| ИИ | `core/ggtg_ai.h` (модели, ключи), `settings/ggtg_ai_*.h` (экраны) / `ai/GgtgAiCore.kt` |
| Синхронизация | `core/ggtg_sync*.{h,cpp}` / `GgtgSync.kt`, `GgtgSyncTransport.kt` |
| Найти файл по смыслу | `docs/FILE_INDEX.md`, граф кода в `tools/graphify-out` |

## 8. Подводные камни

- `APP/` — единственное место готовых сборок, скрипты пишут туда сами.
- Ключи подписи Android лежат в `android/keys` и `TMessagesProj/config/release.keystore`.
  Другая подпись не обновит уже установленные копии.
- `Libraries` и `out/` огромные и генерируются, не кладите их в git и в архивы.
- Версия Android в `android/Telegram-GGTG/gradle.properties` (`APP_VERSION_NAME`) должна
  совпадать с `kAndroidAppVersion` в `main/session_duplicate.cpp`.
- В `backups/` лежат архив исходников и копии tdata на всякий случай.
