# GGTG UI Style Guide & Design System

> **СТРОГОЕ ПРАВИЛО ДЛЯ ВСЕХ ИИ-АГЕНТОВ:**
> Все новые экраны, окна, диалоги и настройки в GGTG обязаны строго следовать этому руководству.
> Любой интерфейс GGTG должен выглядеть и ощущаться как **официальный нативный Telegram** (в macOS/Windows и Android). Никаких кустарных самодельных стилей, плоских неинтерактивных кнопок и разнобоя.

---

## 1. Главные правила (Zero Tolerance)

1. **Никаких эмодзи в роли иконок и маркеров:**
   - ❌ ЗАПРЕЩЕНО: `🟢`, `🟡`, `🔴`, `💻`, `🍏`, `📱`, `⚙️`, `🚀`, `🎲`, `🔥`, `🌱`, `⚡`, `⏳`, `✓` в заголовках, кнопках, списках, плашках.
   - ✅ РАЗРЕШЕНО: использовать исключительно **нативные векторные иконки Telegram** (`st::menuIcon*`, `st::windowMenu*` на Desktop; `R.drawable.msg_*` на Android) либо чистый типографический текст.

2. **Никаких сырых HTML-тегов в тексте:**
   - ❌ ЗАПРЕЩЕНО: писать `<b>текст</b>` или `<i>текст</i>` в `Ui::FlatLabel` или `TextView`. Они выводятся пользователю сырыми тегами.
   - ✅ РАЗРЕШЕНО: использовать стандартное разделение на ключ/значение, отдельные строки, стиль заголовка/подзаголовка (`Ui::AddSubsectionTitle`, `GgtgUi.title`, `GgtgUi.body`).

3. **Выбор аккаунтов — ТОЛЬКО через нативный аккордеон папок:**
   - ❌ ЗАПРЕЩЕНО: выводить аккаунты простым плоским списком чекбоксов.
   - ✅ РАЗРЕШЕНО: использовать готовый компонент с папками, раскрытием/складыванием и общим чекбоксом на папку (`AddAccountPicker` на Desktop; `GgtgAccountPicker` на Android).

4. **Выбор режима/пресета — ТОЛЬКО через радиокнопки или выделяемые карточки:**
   - ❌ ЗАПРЕЩЕНО: делать обычные кнопки `Ui::SettingsButton`, при клике на которые ничего визуально не меняется.
   - ✅ РАЗРЕШЕНО: использовать `Ui::RadiobuttonGroup` + `Ui::Radiobutton` (на Desktop) или карточки с выделением рамки активным цветом (на Android).

5. **Кнопки и контекстные меню ВСЕГДА видны полностью (Anti-Truncation Rule):**
   - ❌ ЗАПРЕЩЕНО: допускать обрезание текста на кнопках многоточием (`...`), перекрытие кнопок соседними элементами или сжатие кнопок в нечитаемые тонкие полоски (15-20px) из-за нехватки места.
   - ❌ ЗАПРЕЩЕНО: впихивать 3-4 действия в нижнюю панель (футер) модального диалога `GenericBox`.
   - ✅ РАЗРЕШЕНО: в футере диалога максимум **2 кнопки справа** ("Отмена" + "Сохранить") и максимум **1 кнопка слева** ("Удалить"). Если у сущности есть дополнительные действия ("Переключить", "Экспорт", "Копировать"), они обязаны располагаться **в теле самого диалога** в виде полноширинных строк (`Ui::SettingsButton`), либо вертикальным блоком.
   - ✅ РАЗРЕШЕНО: формулировать текст кнопок и пунктов меню кратко, емко и по делу: "Переименовать...", "В онлайн", "В спящий режим", "Назначить прокси...", "В корзину". Не вставлять длинные названия внутрь пунктов меню.
   - ✅ РАЗРЕШЕНО: все диалоги с кнопками футера обязаны использовать ширину не менее `st::accountsBoxWidth` (480px).

6. **Паритет macOS и Android:**
   - Любая функция в Desktop обязана иметь полный аналог в Android (и наоборот).

---

## 2. Desktop (C++ / Qt): Паттерны и Компоненты

### 2.1. Ширина стандартных диалоговых окон
Все окна настроек, действий и мастеров используют стандартную ширину Telegram:
```cpp
box->setWidth(st::accountsBoxWidth); // 480px - стандарт для форм с аккаунтами
```

### 2.2. Выбор режима / пресета (Радиокнопки)
Используйте `Ui::RadiobuttonGroup` с паддингом `st::ggtgChoiceRowPadding`:
```cpp
#include "styles/style_boxes.h"
#include "styles/style_window_main_menu.h"

// 1. Создаем группу с начальным значением
const auto presetGroup = std::make_shared<Ui::RadiobuttonGroup>(static_cast<int>(MyEnum::Default));

// 2. Добавляем радиокнопку и поясняющий текст под ней
box->addRow(
    object_ptr<Ui::Radiobutton>(
        box,
        presetGroup,
        static_cast<int>(MyEnum::Option1),
        u"Название опции"_q,
        st::defaultCheckbox),
    st::ggtgChoiceRowPadding);

content->add(
    object_ptr<Ui::FlatLabel>(
        content,
        rpl::single(u"Подробное человеческое описание того, что делает эта опция."_q),
        st::boxDividerLabel),
    style::margins(style::ConvertScale(48), 0, style::ConvertScale(24), style::ConvertScale(8)));

// 3. Обработка переключения
presetGroup->setChangedCallback([=](int val) {
    state->selectedOption = static_cast<MyEnum>(val);
    // обновляем зависимые поля
});
```

### 2.3. Выбор аккаунтов (Папки + Чекбоксы)
Для выбора аккаунтов наследуйте состояние от `AccountPickerState` и вызывайте `AddAccountPicker`:
```cpp
#include "settings/bulk_accounts_actions.h"

struct MyFormState : AccountPickerState {
    // ваши дополнительные параметры формы
    int threads = 2;
};

// Внутри создания Box:
const auto state = box->lifetime().make_state<MyFormState>();

// Предзаполнение выбранных аккаунтов:
for (const auto &acc : Core::App().domain().orderedAccounts()) {
    if (acc->sessionExists()) {
        state->selected.emplace(acc);
    }
}

// Рендеринг блока аккаунтов (папки, folding, счетчик "Выбрано: N"):
Ui::AddSubsectionTitle(content, rpl::single(u"2. Аккаунты"_q));
AddAccountPicker(box, state);
```

### 2.4. Разделители и заголовки секций
Используйте стандартные вертикальные интервалы Telegram:
```cpp
Ui::AddSkip(content, st::defaultVerticalListSkip);
Ui::AddDivider(content);
Ui::AddSkip(content, st::defaultVerticalListSkip);

Ui::AddSubsectionTitle(content, rpl::single(u"Заголовок секции"_q));
```

### 2.5. Поля ввода (Input Fields)
```cpp
const auto input = content->add(
    object_ptr<Ui::InputField>(
        content,
        st::defaultInputField,
        rpl::single(u"Плейсхолдер или подсказка"_q),
        state->initialValue),
    style::margins(style::ConvertScale(24), style::ConvertScale(4), style::ConvertScale(24), style::ConvertScale(8)));

input->changes() | rpl::on_next([=] {
    state->value = input->getLastText().trimmed();
}, input->lifetime());
```

### 2.6. Кнопки действий, нижние панели диалогов и контекстные меню (UX/UI Архитектура)

#### Анатомия модального диалога (`GenericBox`):
- **Тело диалога (`box->verticalLayout()`):**
  - Здесь отображаются данные сущности и все контекстные действия над ней (например, переключение сессии, экспорт данных, сброс параметров).
  - Контекстные действия оформляются строками `Ui::SettingsButton` на всю ширину диалога со стандартным шевроном `›`:
    ```cpp
    Ui::AddSkip(container, st::sessionSubtitleSkip);
    Ui::AddDivider(container);
    Ui::AddSkip(container, st::sessionSubtitleSkip);
    Ui::AddSubsectionTitle(container, rpl::single(u"Действия"_q));

    const auto btn = container->add(
        object_ptr<Ui::SettingsButton>(
            container,
            rpl::single(u"Переключить на эту сессию"_q),
            st::settingsButton));
    btn->setClickedCallback([=] { /* действие */ });
    ```
- **Нижняя панель (Футер / Action Bar):**
  - Предназначена **исключительно** для закрытия/подтверждения самого диалога.
  - Справа: 1 или максимум 2 кнопки (`box->addButton(...)`, например "Готово" или "Отмена" + "Сохранить").
  - Слева: максимум 1 деструктивная кнопка (`box->addLeftButton(...)`, например "Завершить сеанс" или "Удалить").
  - **Никогда** не помещайте 3-4 кнопки в один ряд футера — это приводит к перекрытию и превращению кнопок в нечитаемые огрызки шириной 15px.
- **Ширина диалогов (`box->setWidth(...)`):**
  - Для всех диалогов с действиями, аккаунтами или несколькими кнопками всегда задавайте:
    ```cpp
    box->setWidth(st::accountsBoxWidth); // 480px
    ```
    Стандартная ширина 364px (`st::boxWideWidth`) допустима только для коротких текстовых предупреждений с единственной кнопкой "Закрыть".

#### Контекстные меню (`Ui::PopupMenu`):
- **Стилизация и иконки:**
  - При наличии иконок используйте `st::folderContextMenu` (или `popupMenuWithIcons`) с `widthMin: 200px`, `widthMax: 400px` и паддингом `margins(46px, 8px, 18px, 8px)`.
  - Каждый пункт меню с иконкой использует официальную иконку Telegram (`&st::menuIcon*`):
    - Редактировать / переименовать: `&st::menuIconEdit`
    - В онлайн: `&st::menuIconShowInChat`
    - В спящий режим: `&st::menuIconMute`
    - Настройки / прокси: `&st::menuIconSettings`
    - Проверка SpamBot / статус: `&st::menuIconFactcheck`
    - Удалить из группы / убрать: `&st::menuIconRemove`
    - Деструктивное удаление / корзина: `&st::menuIconDeleteAttention`
    - Выбранный пункт / активный список: `&st::mediaPlayerMenuCheck`
- **Чекмарки и активное состояние:**
  - Если меню представляет собой выбор (например, добавление чата в список или выбор папки), у уже выбранных элементов **обязательно** отображается нативная галочка `st::mediaPlayerMenuCheck` (на Desktop) или `"✓ "` (на Android).
- **Правило лаконичности названий (UX Copywriting):**
  - Никаких громоздких канцелярских фраз! Название кнопки или пункта должно быть максимально коротким, емким и функциональным (1–3 слова):
    - ❌ Плохо: "Перевести все подключенные аккаунты в онлайн режим"
    - ✅ Хорошо: "Все в онлайн" или "В онлайн"
    - ❌ Плохо: "Перевести все аккаунты в Офлайн (Спящий режим)"
    - ✅ Хорошо: "Все в спящий режим" или "В спящий режим"
    - ❌ Плохо: "Переименовать выбранную папку аккаунтов..."
    - ✅ Хорошо: "Переименовать..."
    - ❌ Плохо: "Удалить выбранную папку вместе со всеми содержащимися аккаунтами..."
    - ✅ Хорошо: "Удалить с аккаунтами..."
    - ❌ Плохо: "Очистить содержимое корзины навсегда без возможности восстановления..."
    - ✅ Хорошо: "Очистить корзину навсегда..."
- **Родительский виджет контекстного меню:**
  - Контекстное меню **всегда** создается с родителем верхнего уровня окна (`_outer`, `window` или `box`), а **не** с временным микро-виджетом вроде бейджика (`container`), иначе координаты или клиппинг меню на macOS могут сломаться.

---

## 3. Android (Kotlin): Паттерны и Компоненты

Все Android-экраны GGTG используют компонент `GgtgUi` и нативные темы Telegram.

### 3.1. Структура экрана
```kotlin
val frame = FrameLayout(context)
frame.setBackgroundColor(Theme.getColor(Theme.key_windowBackgroundGray))
fragmentView = frame
val root = GgtgUi.page(context, frame) // скроллируемый контейнер с отступами
```

### 3.2. Карточки и секции
```kotlin
root.addView(GgtgUi.sectionLabel(context, "Название секции"))

val card = GgtgUi.card(context)
card.addView(GgtgUi.title(context, "Заголовок"))
card.addView(GgtgUi.body(context, "Описание"))
GgtgUi.addCard(root, card)
```

### 3.3. Выбор аккаунтов с группировкой по папкам
Используйте компонент `GgtgAccountPicker`:
```kotlin
root.addView(GgtgUi.sectionLabel(context, "1. Выбор аккаунтов"))
val accountsCard = GgtgUi.card(context)
val picker = GgtgAccountPicker(context)
picker.setBackgroundColor(android.graphics.Color.TRANSPARENT)

// Дополнительная строка под аккаунтом (например, траст или отправлено сегодня)
picker.todayProvider = { slot ->
    val score = GgtgWarmup.getStats(slot).trustScore
    "Траст: $score%"
}

accountsCard.addView(picker, LinearLayout.LayoutParams(LayoutHelper.MATCH_PARENT, LayoutHelper.WRAP_CONTENT))
GgtgUi.addCard(root, accountsCard)

// Получение выбранных слотов:
val selected = picker.selectedSlots() // List<Int>
```

### 3.4. Кнопки действий
```kotlin
// Обычная кнопка действия
GgtgUi.addButton(root, GgtgUi.button(context, "Начать действие") { startAction() })

// Опасное действие / Остановка (красный цвет)
GgtgUi.addButton(root, GgtgUi.button(context, "Остановить", destructive = true) { stopAction() })
```

### 3.5. Иконки
Никогда не ставьте смайлики в текст или бейджи. Используйте Telegram drawables:
```kotlin
val icon = R.drawable.msg_channel   // каналы
val icon = R.drawable.msg_contacts  // аккаунты
val icon = R.drawable.msg_settings  // настройки
val icon = R.drawable.msg_send      // рассылка
val icon = R.drawable.msg_stats     // статистика/прогрев
val icon = R.drawable.msg_language  // прокси/гео
val icon = R.drawable.msg_delete    // удаление
```

---

## 4. Чеклист перед сдачей задачи

- [ ] В интерфейсе нет ни одного эмодзи в качестве иконки, статуса или буллета.
- [ ] Все кнопки и пункты меню видны полностью: текст не обрезан `...` и не сжат в узкую полоску.
- [ ] В футере модальных диалогов не более 2 кнопок справа и 1 слева; ширина диалогов с действиями — 480px.
- [ ] Все радиокнопки и чекбоксы визуально меняют состояние при клике.
- [ ] Аккаунты сгруппированы по папкам через стандартный компонент аккордеона.
- [ ] Текст нигде не содержит сырых тегов `<b>...</b>`.
- [ ] Проект успешно собирается на macOS (`xcodebuild` + `make_dmg.sh` -> `APP/GGTG.dmg`).
- [ ] Проект успешно собирается на Android (`./android/build_release.sh` -> `APP/GGTG-Android.apk`).
- [ ] Обновлён индекс файлов: `python3 tools/update_file_index.py`.

