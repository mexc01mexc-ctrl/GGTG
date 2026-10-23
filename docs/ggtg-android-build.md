# Сборка Android-APK GGTG (чтобы он устанавливался)

Собирать только так:

```bash
/Users/yansheva/Desktop/GGTG/android/build_release.sh
```

Результат: `GGTG/APP/GGTG-Android.apk` (предыдущий уходит в `GGTG/APP/previous/`).

## Почему APK «не устанавливался»

Для сборки только под arm64 нужен ключ `-Pandroid.injected.build.abi=arm64-v8a` (готовые нативные библиотеки есть только для arm64). Этот ключ заставляет Gradle считать сборку «запуском из Android Studio» и ставить в APK метку `android:testOnly="true"`. Телефон такой APK не ставит: «Приложение не установлено», в adb `INSTALL_FAILED_TEST_ONLY`. Такая метка была у APK от 8 октября 12:05.

Исправление: добавить `-Pandroid.injected.testOnly=false`. Скрипт делает это сам и после сборки проверяет:

1. в манифесте нет `testOnly`;
2. APK не отладочный, пакет `org.telegram.messenger.web`;
3. подпись та же, что у уже установленных копий (`a08d7dc3…`, ключ `TMessagesProj/config/release.keystore`), иначе обновление поверх не встанет.

Если хоть одна проверка не прошла, скрипт пишет `FAIL` и не подменяет APK.

## Проверка установки без телефона

На Mac есть эмулятор Android 15:

```bash
S=/opt/homebrew/share/android-commandlinetools
$S/emulator/emulator -avd nicegram_test -no-window -no-audio &
$S/platform-tools/adb install -r /Users/yansheva/Desktop/GGTG/APP/GGTG-Android.apk
$S/platform-tools/adb shell am start -n org.telegram.messenger.web/org.telegram.messenger.DefaultIcon
```

Должно быть `Success`, и приложение открывается на стартовом экране с кнопкой «Импорт .ggtg». Закрыть эмулятор: `adb emu kill`.

## Что ещё важно

- Работа над macOS и Windows Android-код не меняет: если Kotlin-файлы не правились, APK получается тем же, что был, но уже без метки testOnly.
- Файл `APP/previous/GGTG-Android-testonly-broken.apk` это старый APK с меткой, его удалить можно.
- Установка поверх уже стоящего приложения проходит, если версия не ниже и подпись та же. Если на телефоне стоит копия с другой подписью (например, из чужой сборки), её сначала нужно удалить.
