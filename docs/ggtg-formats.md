# Форматы данных GGTG (общие для macOS и Android)

## `.ggtg` — экспорт аккаунтов для Android

Пишет: macOS, раздел «Экспорт» (`nicegram-desktop/Telegram/SourceFiles/settings/ggtg_export.cpp`).
Читает: Android, «Импорт из архива» (`TMessagesProj/src/main/java/app/nicegram/GgtgPackImporter.kt`).

Обычный zip без пароля с одним файлом `manifest.json`:

```json
{
  "format": 1,
  "kind": "ggtg-android-export",
  "createdAt": 1790000000,
  "accounts": [
    {
      "userId": "149474479",
      "mainDcId": 2,
      "keys": [ { "dcId": 2, "key": "<512 hex-символов = 256 байт>" } ],
      "phone": "79990000000",
      "firstName": "Mike", "lastName": "", "username": "mike",
      "pinned": false,
      "folder": "Work",
      "twoFactor": "пароль",
      "proxy": { "type": "socks5", "host": "1.2.3.4", "port": 1080, "user": "u", "password": "p" }
    }
  ]
}
```

- `userId` — строка (64-битный id не влезает в double JSON без потерь).
- `keys` — ключи **новой** сессии, созданной при экспорте (`Main::DuplicateSession`). Сессия Mac не передаётся.
- `handoff: true` (текущий Mac): это обычная Desktop-сессия (api_id 2040, устройство аккаунта), только для
  переноса. Android делает из неё **свою** сессию как настоящий клиент Android (QR-передача, как при импорте
  tdata: `TdataImportHelper.duplicateAccountViaHandoff`, устройство из `device`), затем закрывает сессию Mac
  (`auth.logOut`). Если передача не удалась (кроме отказа от ввода 2FA), ключи ставятся напрямую.
  Причина: `auth.exportLoginToken` с api_id 4 (Android) Telegram отклоняет с `API_ID_PUBLISHED_FLOOD` — с 14.10.2026
  даже от настоящего клиента Android. Поэтому Android запрашивает токен с api_id Telegram Desktop (2040), если не
  задан свой (`GgtgApiOverride`); новая сессия записывается как Telegram Desktop.
- Без `handoff` (старые файлы) Android ставит ключи напрямую, без повторного дублирования.
- `device` — профиль клиента Android: `deviceModel` (бренд+модель без пробела, `XiaomiRedmi 9C`), `systemVersion`
  (`SDK n`, строго от этой модели), `appVersion` (настоящая сборка Telegram `12.10.6 (71129)`: versionCode×10 + цифра ABI),
  `systemLangCode` (`ru-ru`, для индонезийского `in-id`), `cloudLangCode` (`ru`), `langPackName` (`android`), `tzOffset`
  (секунды от UTC). Выбор: бренд → SDK этого бренда → модель → сборка → язык, всё из каталога
  `settings/ggtg_android_profiles_data.h` / `GgtgAndroidProfilesData.kt` (генерирует `tools/gen_android_profiles.py` из
  `Base.db` эталонного генератора). Язык и часовой пояс аккаунта (по прокси) сохраняются, если язык есть в каталоге.
- Профиль назначается **автоматически и у каждого аккаунта свой**: новому аккаунту и аккаунту без профиля при запуске
  (Mac/Windows: `Domain::watchSession`; Android: `ConnectionsManager.init`). Windows получает ноутбук из каталога
  (`Ggtg::DesktopProfiles`, `Windows 11 x64`, образец: Desktop api_id 2040, `tdesktop`), Mac — MacBook/iMac/Mac mini из
  пресетов, Android — телефон из каталога. Язык и пояс подбираются по прокси.
- `folder` отсутствует, если выключена опция «Папки»; `""` значит «без папки».
- `twoFactor` и `proxy` отсутствуют, если их нет или опция выключена.
- `proxy.type`: `socks5`, `mtproto` (с `secret` вместо `user`/`password`) или `http`.
  Android не поддерживает HTTP — такому аккаунту выдаётся прокси из выбранного пула.
- Файл даёт полный доступ к аккаунтам: пароля на архиве нет по решению пользователя.
- Новое поле — не повод поднимать `format`; поднимать только при несовместимых изменениях.
  Android отказывается читать `format` больше, чем знает.

## Синхронизация между устройствами (`ggsync`)

Пишет и читает: macOS (`core/ggtg_sync*.cpp`, раздел «Синхронизация») и Android
(`app/ggtg/GgtgSync.kt`, `GgtgSyncTransport.kt`, «Главная» → «Синхронизация»).

Носитель — «Избранное» одного главного аккаунта Telegram. Каждое устройство кладёт туда **свой**
файл `ggtg-sync-<deviceId>-<keyId>.bin` (подпись `#ggtgsync`; `keyId` — первые 6 hex-символов SHA-256 от `"ggtgsync-key:" + фраза`, заглавными; старые копии своего файла и копии со старой фразой удаляет, файлы других устройств с другим `keyId` не открывает, а старше 7 дней удаляет; старые имена без `-<keyId>` пробует открыть) и забирает
свежие файлы остальных устройств. Слияние идемпотентно, устройства сходятся к одному состоянию.

Файл: `"GGSY" 0x01` + salt(16) + iv(12) + AES-256-GCM(шифртекст ‖ tag 16).
Ключ: PBKDF2-HMAC-SHA256(фраза в UTF-8, salt, 100000 итераций, 32 байта).
Внутри: формат Qt `qCompress` (4 байта длины big-endian + zlib) от JSON:

```json
{
  "format": 1, "device": "<hex>", "deviceName": "Mac|Android", "createdAt": "1790000000",
  "bases": [ { "id": "беззнаковое десятичное", "kind": "users|groups", "title": "…", "savedAt": "…",
               "items": ["username", …] | [ {"username","title","membersCount","link","checked"} ] } ],
  "sent":  { "username": { "status": "ok|spam_acc|not_found|restricted|peer_limit|account_fail|suspected_fake|long_ago",
                           "account": "…", "at": "…", "base": "id", "source": "…", "note": "…" } },
  "tomb":  { "base:<id>": "unix", "sent:<username>": "unix" },
  "files": { "ai_profile.json": { "mtime": "unix", "text": "…" }, "ai_knowledge/<id>.json": {…} }
}
```

Подключение и автоматика:
- Телефон подключается по QR-коду с Mac («Синхронизация» → «Подключить телефон по QR-коду»). В коде строка
  `ggtgsync:1:<userId главного аккаунта>:<фраза: base64url от UTF-8, без '='>`. Фразу Mac создаёт сам
  (24 случайных байта в base64url), если её ещё нет. Android (`GgtgSync.parsePairing`) ставит главный аккаунт и
  фразу, включает синхронизацию и сразу запускает раунд.
- Раунд идёт сам: при запуске, после изменений, каждые 5 минут, **перед каждой рассылкой** (рассылка ждёт до 30 с,
  потом стартует в любом случае: `SyncBeforeMailing` / `syncBeforeMailing`) и через 5 с после её конца.

Правила слияния:
- `sent`: «Отправлено» (ok) никогда не теряется; иначе побеждает запись с большим `at`.
  `account_fail` Android не знает и пропускает.
- `bases`: по `id` побеждает больший `savedAt`, база заменяется целиком; потом из баз убираются
  все, кому уже написали.
- `tomb` — что удалено намеренно и когда; запись или база удаляются, если метка не старше их
  `at`/`savedAt`. Метки живут 90 дней.
- `files` (ИИ: профиль, примеры, плейбук, обучение, настройки, материалы, списки чатов): побеждает
  файл с большим `mtime`, целиком.
- id баз во всех файлах — беззнаковые (ручные базы на Mac имеют старший бит; на Android это
  отрицательный Long, поэтому `GgtgDb.idToString/idFromString`).

### Перенос аккаунтов через синхронизацию

Те же носитель и шифрование (`Pack`/`Unpack`), отдельные файлы в «Избранном» главного аккаунта:
- `ggtg-accounts-pack-<deviceId>.bin` — для Android: внутри обычный `.ggtg` (см. выше). Кладёт Mac
  («Синхронизация» → «Отправить аккаунты на Android…»), забирает Android («Принять аккаунты с Mac»).
- `ggtg-accounts-keys-<deviceId>.bin` — для Mac: внутри JSON `{"format":1,"kind":"ggtg-desktop-import",
  "accounts":[{userId, mainDcId, keys:[{dcId,key}], phone, folder, pinned, twoFactor}]}`. Кладёт Android,
  забирает Mac (`Domain::addFromAuthorizations`, прокси спрашивается как при импорте tdata).
Каждому аккаунту выдаётся новая сессия (`DuplicateSession` / `duplicateSession`); после успешной
загрузки файл удаляется из «Избранного».
