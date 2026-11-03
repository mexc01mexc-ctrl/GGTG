# Архив: раздел «ИИ»

Раздел «ИИ» (ключи бесплатных облачных моделей, помощник в чате, лента ответов, агент, обучение,
память и знания) убран из программы. Код сохранён здесь, чтобы его можно было вернуть.

## Что лежит

- `desktop/core/ggtg_ai*.h`, `desktop/settings/ggtg_ai*.h` — все файлы Desktop (до удаления).
  `ggtg_ai_translate.h` теперь живёт в программе как `settings/ggtg_translate.h`, а виджет вкладок
  из `ggtg_ai_cards.h` как `settings/ggtg_tabs.h`.
- `android/*.kt` — весь пакет `app.ggtg.ai` Android. «Списки чатов» (`GgtgChatLists*`,
  `GgtgChatListSend`) и страничные помощники (`GgtgAiUi` → `GgtgPageUi`) остались в программе.
- `desktop-removal.patch` — правки Desktop, которые отключили ИИ (обратный `patch -R` возвращает как было).
- `desktop/icons/bot.png` — значок робота раздела.

## Что осталось в программе вместо ИИ

- Desktop: `core/ggtg_lang.h` (языки и имена, настройки перевода), `core/ggtg_replies.h` (ответы людей и
  отметка «успех», типы ответов по словам), `settings/ggtg_translate.h` (бесплатный перевод),
  `settings/ggtg_tabs.h`. Панель собеседника (мозг в чате): `Ggtg::FakeDetector::ShowFakeDetailsBox`.
- Android: `GgtgPageUi`, `app.ggtg.chatlists.*`, панель собеседника `GgtgFakeDetector.showFakeDetailsDialog`.

## Как вернуть (Desktop)

1. `cd desktop/nicegram-desktop/Telegram/SourceFiles`
2. Скопировать `archive/ai/desktop/core/*` в `core/` и `archive/ai/desktop/settings/*` в `settings/`
   (кроме `ggtg_ai_translate.h`, он стал `ggtg_translate.h`).
3. Применить `patch -R -p1 < archive/ai/desktop-removal.patch` (вернёт раздел в меню, помощника в чате,
   кнопки в рассылке). Мозг у поля ввода (`HistoryWidget::showChatBrain`) править вручную: вернуть
   вызов `showAiAssistMenu()` по ПКМ, если нужен робот.
4. Вернуть `AiTabs` из `ggtg_ai_cards.h`, удалить `ggtg_tabs.h`, если появятся конфликты имён.

## Как вернуть (Android)

1. Скопировать `archive/ai/android/*.kt` в `app/ggtg/ai/` (кроме `GgtgChatLists*`, `GgtgChatListSend`, `GgtgAiUi`).
2. В `GgtgHubActivity` вернуть пункт «ИИ», в `GgtgHome` плитку и подсказку, в `GgtgManualSends`
   вызовы `GgtgAiInbox.startBackground()` и `GgtgAiInbox.onIncoming(...)`, в `GgtgFakeDetector` разбор ИИ.

Пользовательские данные ИИ (`tdata/ggtg_db/ai_*.json`, `ai_knowledge/`) программа не трогала.
