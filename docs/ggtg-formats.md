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
- `handoff: true` (текущий Mac): это обычная Desktop-сессия (api_id 2040), созданная только для переноса. Android
  ставит её ключи **как есть** и дальше работает на ней (устройство берёт из `device` — то самое, с которым сессия
  родилась). Никаких «Android-сессий»: Telegram требует почту при каждом новом входе Android в существующий аккаунт,
  а `auth.exportLoginToken` с api_id Android отклоняется (`API_ID_PUBLISHED_FLOOD`). На Mac остаётся только заметка
  (`exported`), не запасная сессия: один ключ работает только в одном месте.
- Без `handoff` (старые файлы) ключи тоже ставятся напрямую.
- `device` — профиль устройства сессии (для `handoff: true` — Desktop-устройство; в старых файлах — профиль Android): `deviceModel` (бренд+модель без пробела, `XiaomiRedmi 9C`), `systemVersion`
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
  "files": { "ai_profile.json": { "mtime": "unix", "text": "…" }, "ai_knowledge/<id>.json": {…} },
  "account_statuses": { "<userId>": { "userId": "…", "phone": "…", "state": "clear|temporary|permanent", "until": "0", "checkedAt": "unix", "folder": "…", "trashed": false, "offline": false, "sentToday": 0 } },
  "session_notes": [ { "id": "…", "userId": 12345, "note": "…", "role": "…", "platform": "android", "current": true, "createdAt": 1790000000, "hash": "…", "device": {…} } ]
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
- `account_statuses`: вердикт SpamBot (`state`, `until`, `checkedAt`) обновляется у аккаунта, если удалённая проверка свежее локальной (`remote.checkedAt > local.checkedAt`). Для совпадающих активированных аккаунтов также передаются `folder` (если локально папка не назначена) и `trashed` (помещение в корзину).
- `session_notes`: примечания и роли сохранённых сессий объединяются по идентификатору и userId.
  `platform` (`desktop` / `android` / `ios`, необязательно) и `current` (сессия, на которой аккаунт работает сейчас,
  необязательно) хранятся отдельными полями, а не в заметке.

### Сессии: четыре вида (одинаково на Desktop и Android)

Строка `role` остаётся для совместимости, а в интерфейсе она сводится к виду:
- **оригинал** (`purchased`, `imported`): купленная сессия, не трогается;
- **запасная** (`backup`, `duplicate`, `anti_theft_backup`, `desktop`): наша копия, на неё можно переключить аккаунт;
- **передана** (`exported`, `android`): ушла на другое устройство;
- **бывшая рабочая** (`previous`): остаётся после переключения, у аккаунта хранится только последняя.
`stored` («сохранённая») без файлов ключей переключаемой не считается. Платформа берётся из поля `platform`,
для старых записей угадывается по роли, источнику и заметке. Название в списке: «вид · платформа», у рабочей
спереди «текущая».
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

## Данные о людях из базы (users_meta)

Имя, id и «был в сети» хранятся один раз на устройство (`users_meta.json` / `UsersMetaCache`), а не
на аккаунт. При показе списка базы человек ищется во всех аккаунтах (активный первым), а в фоне
запросы к Telegram идут по очереди с разных аккаунтов. `access_hash` у каждого аккаунта свой, поэтому
чат открывается только объектом того аккаунта, под которым человек разрешён заново.

## pinned_countries.json

Закреплённые страны в списке выбора страны для прокси. JSON-массив ISO2-кодов, новые закрепы первыми,
например `["US","DE"]`. Desktop: `tdata/ggtg_db/pinned_countries.json` (ПКМ по стране).
Android: `filesDir/ggtg_bulk_db/pinned_countries.json` (долгое нажатие).

## id_calibration.json

Точные точки для оценки даты регистрации по ID: JSON-массив `[{"id": 8912345678, "t": 1793000000}]`
(`t` — unix-секунды). Точку добавляет каждый аккаунт, зарегистрированный самой программой (ID и настоящее
время регистрации). Оценка (`core/ggtg_id_date.h`, `GgtgIdDate.kt`) строит монотонную кривую по известным
якорям и этим точкам, дальше последнего якоря идёт с недавней скоростью до «сейчас», а дата первого фото
профиля ограничивает ответ сверху. Desktop: `tdata/ggtg_db/id_calibration.json`,
Android: `filesDir/ggtg_bulk_db/id_calibration.json`.
Точки добавляются сами при регистрации и вручную: в профиле правый клик (Mac) / долгое нажатие (Android)
на «Примерная дата регистрации» → «Уточнить дату по Telegram». Запрос `messages.getPeerSettings`,
поле `registration_month` ("MM.YYYY", Telegram отдаёт его, когда незнакомый человек пишет первым);
точка ставится на середину месяца (`ParseRegistrationMonth` / `parseRegistrationMonth`).

## Список аккаунтов при старте (Desktop)

Аккаунты стартуют пачками по 5 (`Domain::startNextBatch`), но в боковом списке видны все сразу: для ещё не
запущенных строка берётся из `tdata/ggtg_account_cache.json` (`{индекс: {name, phone, folder, trashed}}`,
пишется с задержкой при смене списка, папки и корзины). Клик по такой строке запускает аккаунт вне очереди
(`Domain::startPendingNow`) и открывает его. Группа «Корзина» (ключ `⁣Корзина`, как на Android) стоит
последней и показывается и в списке Desktop, и в переключателе Android.

### Управляющие сообщения (автоматический перенос аккаунтов)

Файл `ggtg-accounts-ctl-<deviceId>.bin` в «Избранном» главного аккаунта (те же `Pack`/`Unpack`), внутри JSON.
Получатель подхватывает его сразу, как он пришёл (новое сообщение в этом чате), а при запуске и раз в 5 минут
проверяет пропущенные; обработанный файл удаляется.
- Mac → Android (аккаунт есть на Mac, на телефоне создаётся новая сессия по коду): телефон делает `auth.sendCode`
  и шлёт `{"type":"code_req","userId":"…","ts":"unix"}`; Mac, где аккаунт вошёл, берёт код из чата 777000 и отвечает
  `{"type":"code","userId":"…","code":"12345"}`; телефон входит сам. Не пришёл за 75 с — код вводится вручную.
- Android → Mac (аккаунт есть на телефоне, на Mac создаётся новая сессия по QR): Mac показывает QR-вход и на каждый
  токен шлёт `{"type":"qr","userId":"…","link":"tg://login?token=…"}`; телефон принимает его `auth.acceptLoginToken`
  аккаунтом с этим `userId`. Прокси, язык и время Mac берёт с аккаунта на телефоне (список синхронизации и номер).

### Control message `pack_req` (phone asks the Mac for an account)

`{"type":"pack_req","userId":"<id>"}` in a `ctl` file. The Mac makes a separate Desktop session of that
account (`Main::DuplicateSessionTracked`, a device picked for it) and puts a `pack` file with `"handoff": true` and
that `device` into the main account's "Saved messages". The phone installs its keys as they are
(`GgtgPackImporter`) and runs on that session; the Mac keeps only a note. If the Mac stays silent for 3 minutes
the phone reports it: there is no login-by-code way any more (Telegram asks a login e-mail on every new Android login).
