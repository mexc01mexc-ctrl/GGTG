# Сквидвард: редизайн — план реализации

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Заменить «зеркало чата» с @skvidvarde_bot на аккуратный экран: поле поиска → досье → сбор списка по кнопке → сохранение в «Базу»; из 🦑 в «Базе» — всё автоматически.

**Architecture:** Три единицы: `ggtg_squidward_parse` (чистые функции разбора ответов бота, только Qt Core, проверяются отдельным тестом), `ggtg_squidward_session` (работа с ботом: пробив, листание страниц, фильтр каналов, события через rpl), `ggtg_squidward.cpp` (только экран, подписан на события сессии).

**Tech Stack:** C++20, Qt 6.11 (Core), tdesktop (lib_ui, rpl, MTP), CMake + Xcode на macOS.

**Spec:** `docs/superpowers/specs/2026-09-29-squidward-redesign-design.md`

**Все пути ниже — относительно `nicegram-desktop/`.** Папка не является git-репозиторием: шагов «commit» нет, каждая задача заканчивается проверкой (тест или сборка).

## Global Constraints

- Бот: `@skvidvarde_bot`; платные кнопки (содержат `💎`) никогда не нажимаются автоматически и не показываются.
- Таймауты: ответ на пробив — 30 с; первая страница списка — 40 с, до 2 повторных нажатий; следующая страница — 20 с, 1 повтор, затем остановка с сохранением собранного.
- Недавние пробивы: максимум 8, файл `tdata/ggtg_squidward_recent.json` (массив строк).
- Имена баз: `Data::GroupBaseTitle(имя из досье, @username из запроса)`; для закрытой ссылки — только имя; если имени нет — запрос.
- В самом разделе сбор только по кнопке; при открытии через 🦑 (`lookupGroup` не пуст) — автосбор участников и автосохранение.
- Закрытые группы (`t.me/c/…`, `t.me/+…`, `t.me/joinchat/…`) сохраняются в базу групп со ссылкой (`ScrapedGroupEntry::link`).
- `ggtg_squidward_parse.{h,cpp}` зависят только от Qt Core (литералы `u"…"_s`, не `_q`), чтобы тест собирался отдельно.
- Сборка: только Debug. После добавления исходников: `env -u QT cmake -S . -B out` (не экспортировать `QT`).

## Review Focus

1. Бот пишет «похожими» буквами и иногда пропускает строки → досье показывается частично, без «-1», пустых подписей и «null». Тест: Task 3 (неполное досье + `FormatUserDetails`).
2. Ссылка на ID аккаунта в шапке списка групп (`tg://user?id=…`) и прочие не-t.me ссылки не должны становиться «группами». Тест: Task 2.
3. Бот дважды редактирует одну и ту же страницу или одна группа встречается на двух страницах → без дублей. Тест: Task 2 (`DedupeGroupRows`), плюс проверка номера страницы в Task 5.
4. Новый пробив, пока идёт дообработка прошлого списка → старый результат не должен появиться под новым досье. Защита счётчиком поколений в Task 5, ручная проверка в Task 7.
5. Раздел закрыли посреди сбора → без падения и без поздних всплывающих сообщений. `MTP::Sender` + `base::has_weak_ptr` в Task 5, ручная проверка в Task 7.

---

### Task 1: Основа разбора и отдельный тест

**Files:**
- Create: `Telegram/SourceFiles/window/ggtg_squidward_parse.h`
- Create: `Telegram/SourceFiles/window/ggtg_squidward_parse.cpp`
- Create: `Telegram/SourceFiles/window/ggtg_squidward_parse_test.cpp`
- Create: `Telegram/build/ggtg/test_squidward_parse.sh`

**Interfaces:**
- Produces: весь заголовок `Window::Squidward` (типы `Entity`, `Message`, `UserCard`, `GroupCard`, `PageInfo`, `GroupRow`, `UserRow`, `NameChange`, `TopMember`) и функции `Fold`, `ButtonLastWord`, `ParsePageInfo`, `UsernameFromLink`, `PrivateLink`, `UsernameFromQuery`, `IsPlaceholder`, `CleanText`, `FormatCount`, `PushRecent`. Остальные объявленные функции реализуются в Task 2–4.

- [ ] **Step 1: Создать заголовок**

`Telegram/SourceFiles/window/ggtg_squidward_parse.h`:

```cpp
/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and exceptions, see https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtCore/QString>
#include <QtCore/QStringList>

#include <optional>
#include <vector>

// Reading @skvidvarde_bot's replies into plain data. Depends on Qt Core only
// (see Telegram/build/ggtg/test_squidward_parse.sh, which builds its test
// without the rest of the app).
namespace Window::Squidward {

struct Entity {
	enum class Type {
		Bold,
		Url,
		CustomUrl,
		Mention,
		MentionName,
		Other,
	};
	Type type = Type::Other;
	int offset = 0;
	int length = 0;
	QString data;
};

// A bot message's text and its formatting/link entities.
struct Message {
	QString text;
	std::vector<Entity> entities;
};

struct NameChange {
	QString date; // "2026-09-17"
	QString name;
};

// A person's card. Numbers are -1 and strings empty when the bot didn't say.
struct UserCard {
	QString name;
	QString id;
	QString activeSince; // "13.06.2018"
	QString activeUntil; // "сегодня 12:06"
	int messages = -1;
	int chats = -1;
	QString diversity; // "67,15%"
	QString replies; // "96,16%"
	QString media; // "85,85%"
	int roundVideos = -1;
	int voices = -1;
	QString favoriteChat;
	QString channel;
	int searched = -1;
	QStringList usernames; // without '@'
	std::vector<NameChange> names;
};

struct TopMember {
	QString name;
	int messages = 0;
};

// A group's card. Same "not said" convention as UserCard.
struct GroupCard {
	QString name;
	QString id;
	int topPlace = -1;
	int topOf = -1;
	int peopleDay = -1;
	int messagesDay = -1;
	int mediaDay = -1;
	QString mediaDayPercent; // "98,99%"
	int roundVideosDay = -1;
	int voicesDay = -1;
	QString diversity;
	QString description;
	std::vector<TopMember> topDay;
};

struct PageInfo {
	int page = 0;
	int pages = 0;
	int total = -1; // "Всего N", -1 when absent
};

// One line of a user's groups list: a public group (username), or a closed
// one (link to t.me/c/..., t.me/+..., t.me/joinchat/...).
struct GroupRow {
	QString username;
	QString title;
	QString link;
};

// One line of a group's members list: a username straight from a t.me link,
// or a text-mention's raw data to be resolved by the caller.
struct UserRow {
	QString username;
	QString mentionData;
};

// Lower-cases and folds the bot's look-alike letters (Latin/Greek/small caps
// in Russian words, "İD", "ɪᴅ"...) one character to one character, so
// positions in the result match positions in `text`.
[[nodiscard]] QString Fold(const QString &text);
// A keyboard button's last word, folded ("🎖 Топ, люди" -> "люди").
[[nodiscard]] QString ButtonLastWord(const QString &text);
[[nodiscard]] std::optional<PageInfo> ParsePageInfo(const QString &text);
// "t.me/name" -> "name"; empty for anything else.
[[nodiscard]] QString UsernameFromLink(const QString &link);
// The link itself if it's a closed group's link; empty otherwise.
[[nodiscard]] QString PrivateLink(const QString &link);
// The public username a lookup query names ("@name", "name", t.me link);
// empty for ids, closed links and anything invalid.
[[nodiscard]] QString UsernameFromQuery(const QString &query);
// The bot's "⏳ Вычисляю..." stand-in before the real answer.
[[nodiscard]] bool IsPlaceholder(const QString &text);
// Trims lines, keeps at most one empty line in a row, trims the result.
[[nodiscard]] QString CleanText(const QString &text);
// 30707 -> "30 707".
[[nodiscard]] QString FormatCount(int value);
// `query` first, no case-insensitive repeats, at most `limit` entries.
[[nodiscard]] QStringList PushRecent(
	QStringList list,
	const QString &query,
	int limit);

[[nodiscard]] std::vector<GroupRow> ExtractGroupRows(const Message &message);
[[nodiscard]] std::vector<UserRow> ExtractUserRows(const Message &message);
// Keeps the first of each username / link (case-insensitive).
[[nodiscard]] std::vector<GroupRow> DedupeGroupRows(
	std::vector<GroupRow> rows);
// Strips '@', drops empty names and bots ("...bot"), keeps the first of each
// (case-insensitive).
[[nodiscard]] QStringList DedupeUsernames(const QStringList &usernames);

[[nodiscard]] std::optional<UserCard> ParseUserCard(const Message &message);
[[nodiscard]] std::optional<GroupCard> ParseGroupCard(const Message &message);
// Human-readable lines for a card; fields the bot didn't give are left out.
[[nodiscard]] QStringList FormatUserDetails(const UserCard &card);
[[nodiscard]] QStringList FormatGroupDetails(const GroupCard &card);

} // namespace Window::Squidward
```

- [ ] **Step 2: Создать тест и скрипт сборки теста**

`Telegram/SourceFiles/window/ggtg_squidward_parse_test.cpp`:

```cpp
/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and exceptions, see https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
// Standalone checks for ggtg_squidward_parse.cpp - not part of the app build.
// Run: Telegram/build/ggtg/test_squidward_parse.sh
#include "window/ggtg_squidward_parse.h"

#include <QtCore/QLocale>

#include <cstdio>

using namespace Window::Squidward;
using namespace Qt::StringLiterals;

namespace {

int Failures = 0;

void Check(bool ok, const char *what, int line) {
	if (!ok) {
		++Failures;
		std::printf("FAIL line %d: %s\n", line, what);
	}
}

void CheckString(
		const QString &actual,
		const QString &expected,
		const char *what,
		int line) {
	if (actual != expected) {
		++Failures;
		std::printf(
			"FAIL line %d: %s\n  actual:   \"%s\"\n  expected: \"%s\"\n",
			line,
			what,
			actual.toUtf8().constData(),
			expected.toUtf8().constData());
	}
}

#define CHECK(x) Check((x), #x, __LINE__)
#define CHECK_EQ(a, b) Check((a) == (b), #a " == " #b, __LINE__)
#define CHECK_STR(a, b) CheckString((a), (b), #a, __LINE__)

[[nodiscard]] Entity Ent(
		Entity::Type type,
		const QString &text,
		const QString &shown,
		const QString &data = QString()) {
	return { type, int(text.indexOf(shown)), int(shown.size()), data };
}

void TestFoundation() {
	CHECK_STR(Fold(u"İD"_s), u"id"_s);
	CHECK_STR(Fold(u"ɪᴅ"_s), u"id"_s);
	CHECK_STR(Fold(u"Кαнαл"_s), u"канал"_s);
	CHECK_EQ(Fold(u"userŋames 🍀"_s).size(), u"userŋames 🍀"_s.size());
	CHECK_STR(ButtonLastWord(u"👁 Группы"_s), Fold(u"группы"_s));
	CHECK_STR(ButtonLastWord(u"🎖 Тoп, люди"_s), Fold(u"люди"_s));

	const auto page = ParsePageInfo(u"Всего 1262, страница 1 из 85"_s);
	CHECK(page.has_value());
	CHECK_EQ(page->page, 1);
	CHECK_EQ(page->pages, 85);
	CHECK_EQ(page->total, 1262);
	CHECK(!ParsePageInfo(u"Это FAMBUN"_s).has_value());

	CHECK_STR(UsernameFromLink(u"https://t.me/fambun_chat"_s), u"fambun_chat"_s);
	CHECK(UsernameFromLink(u"https://t.me/c/2361171296/896316"_s).isEmpty());
	CHECK_STR(
		PrivateLink(u"https://t.me/c/2361171296/896316"_s),
		u"https://t.me/c/2361171296/896316"_s);
	CHECK_STR(PrivateLink(u"t.me/+AbCd_ef"_s), u"t.me/+AbCd_ef"_s);
	CHECK(PrivateLink(u"https://t.me/fambun_chat"_s).isEmpty());

	CHECK_STR(UsernameFromQuery(u"@Qiuvvinn"_s), u"Qiuvvinn"_s);
	CHECK_STR(UsernameFromQuery(u" https://t.me/fambun_chat "_s), u"fambun_chat"_s);
	CHECK(UsernameFromQuery(u"https://t.me/c/2361171296/896316"_s).isEmpty());
	CHECK(UsernameFromQuery(u"149474479"_s).isEmpty());

	CHECK(IsPlaceholder(u"⏳ Вычисляю..."_s));
	CHECK(!IsPlaceholder(u"Это FAMBUN"_s));
	CHECK_STR(CleanText(u"  a  \n\n\n\nb\n"_s), u"a\n\nb"_s);
	CHECK_STR(FormatCount(30707), QLocale(QLocale::Russian).toString(30707));

	CHECK_EQ(
		PushRecent({ u"@a"_s, u"@b"_s }, u"@B"_s, 8),
		(QStringList{ u"@B"_s, u"@a"_s }));
	auto many = QStringList();
	for (auto i = 0; i != 10; ++i) {
		many.push_back(QString::number(i));
	}
	CHECK_EQ(PushRecent(many, u"x"_s, 8).size(), 8);
}

} // namespace

int main() {
	TestFoundation();
	if (Failures) {
		std::printf("%d check(s) failed\n", Failures);
		return 1;
	}
	std::printf("All checks passed\n");
	return 0;
}
```

`Telegram/build/ggtg/test_squidward_parse.sh`:

```bash
#!/bin/bash
# Builds and runs the Сквидвард reply-parser checks against the checkout's
# own static Qt Core - no app build needed.
set -euo pipefail
root="$(cd "$(dirname "$0")/../../.." && pwd)"
qt="$(ls -d "$root/../Libraries/local/Qt-"* | head -1)"
src="$root/Telegram/SourceFiles"
out="${TMPDIR:-/tmp}/ggtg_squidward_parse_test"
clang++ -std=c++20 -O0 -g -Wno-deprecated-declarations \
	-I"$src" -I"$qt/include" -I"$qt/include/QtCore" \
	"$src/window/ggtg_squidward_parse.cpp" \
	"$src/window/ggtg_squidward_parse_test.cpp" \
	"$qt/lib/libQt6Core.a" "$qt/lib/libQt6BundledPcre2.a" \
	-lz -framework IOKit -framework CoreFoundation -framework Foundation \
	-framework DiskArbitration -framework AppKit \
	-framework ApplicationServices -framework CoreServices \
	-framework Security -framework UniformTypeIdentifiers \
	-o "$out"
"$out"
```

Run: `chmod +x Telegram/build/ggtg/test_squidward_parse.sh`

- [ ] **Step 3: Убедиться, что тест не собирается**

Run: `Telegram/build/ggtg/test_squidward_parse.sh`
Expected: FAIL — нет `ggtg_squidward_parse.cpp` (или «undefined symbols»).

- [ ] **Step 4: Реализовать основу**

`Telegram/SourceFiles/window/ggtg_squidward_parse.cpp` (функции Task 2–4 добавляются ниже в том же файле, внутри `namespace Window::Squidward`):

```cpp
/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and exceptions, see https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "window/ggtg_squidward_parse.h"

#include <QtCore/QHash>
#include <QtCore/QLocale>
#include <QtCore/QRegularExpression>

namespace Window::Squidward {
namespace {

using namespace Qt::StringLiterals;

[[nodiscard]] QChar FoldChar(QChar ch) {
	// Look-alikes the bot mixes into Russian words, and the odd letters of
	// its "ID" labels; one character to one character, always.
	static const auto table = QHash<char16_t, char16_t>{
		{ u'a', u'а' }, { u'c', u'с' }, { u'e', u'е' }, { u'o', u'о' },
		{ u'p', u'р' }, { u'x', u'х' }, { u'y', u'у' }, { u'r', u'г' },
		{ u'k', u'к' },
		{ 0x03BF, u'о' }, { 0x03B3, u'у' }, { 0x03C1, u'р' },
		{ 0x03B1, u'а' }, { 0x03C0, u'п' }, { 0x03B7, u'н' },
		{ 0x03BD, u'н' }, { 0x1D0F, u'о' }, { 0x2113, u'л' },
		{ 0x0251, u'а' }, { 0x04A1, u'к' }, { 0x0142, u'л' },
		{ 0x01EB, u'о' },
		{ 0x026A, u'i' }, { 0x0131, u'i' }, { 0x1D05, u'd' },
		{ 0x014B, u'n' }, { 0x0273, u'n' }, { 0x0192, u'f' },
	};
	const auto lower = ch.toLower().unicode();
	return QChar(table.value(lower, lower));
}

} // namespace

QString Fold(const QString &text) {
	auto result = text;
	for (auto i = 0; i != result.size(); ++i) {
		result[i] = FoldChar(text[i]);
	}
	return result;
}

QString ButtonLastWord(const QString &text) {
	const auto parts = text.simplified().split(u' ', Qt::SkipEmptyParts);
	return parts.isEmpty() ? QString() : Fold(parts.back());
}

std::optional<PageInfo> ParsePageInfo(const QString &text) {
	const auto folded = Fold(text);
	static const auto pages = QRegularExpression(
		QRegularExpression::escape(Fold(u"страниц"_s))
			+ uR"(\D*(\d+)\D+(\d+))"_s);
	static const auto total = QRegularExpression(
		QRegularExpression::escape(Fold(u"всего"_s)) + uR"(\D*?(\d+))"_s);
	const auto match = pages.match(folded);
	if (!match.hasMatch()) {
		return std::nullopt;
	}
	auto result = PageInfo{
		.page = match.captured(1).toInt(),
		.pages = match.captured(2).toInt(),
	};
	if (const auto all = total.match(folded); all.hasMatch()) {
		result.total = all.captured(1).toInt();
	}
	return result;
}

QString UsernameFromLink(const QString &link) {
	static const auto expression = QRegularExpression(
		uR"(^(?:https?://)?t(?:elegram)?\.me/([A-Za-z0-9_]{4,32})/?$)"_s);
	const auto match = expression.match(link.trimmed());
	return match.hasMatch() ? match.captured(1) : QString();
}

QString PrivateLink(const QString &link) {
	static const auto expression = QRegularExpression(
		uR"(^(?:https?://)?t(?:elegram)?\.me/(?:c/\d+(?:/\d+)?|\+[A-Za-z0-9_-]+|joinchat/[A-Za-z0-9_-]+)/?$)"_s);
	const auto trimmed = link.trimmed();
	return expression.match(trimmed).hasMatch() ? trimmed : QString();
}

QString UsernameFromQuery(const QString &query) {
	const auto trimmed = query.trimmed();
	if (!PrivateLink(trimmed).isEmpty()) {
		return QString();
	}
	if (const auto fromLink = UsernameFromLink(trimmed); !fromLink.isEmpty()) {
		return fromLink;
	}
	static const auto valid = QRegularExpression(
		uR"(^@?([A-Za-z][A-Za-z0-9_]{3,31})$)"_s);
	const auto match = valid.match(trimmed);
	return match.hasMatch() ? match.captured(1) : QString();
}

bool IsPlaceholder(const QString &text) {
	return Fold(text).contains(Fold(u"вычисля"_s));
}

QString CleanText(const QString &text) {
	auto lines = QStringList();
	for (auto line : text.split(u'\n')) {
		while (!line.isEmpty() && line.back().isSpace()) {
			line.chop(1);
		}
		if (line.trimmed().isEmpty()
			&& (lines.isEmpty() || lines.back().isEmpty())) {
			continue;
		}
		lines.push_back(line.trimmed().isEmpty() ? QString() : line);
	}
	return lines.join(u'\n').trimmed();
}

QString FormatCount(int value) {
	return QLocale(QLocale::Russian).toString(value);
}

QStringList PushRecent(QStringList list, const QString &query, int limit) {
	const auto trimmed = query.trimmed();
	list.removeIf([&](const QString &entry) {
		return entry.compare(trimmed, Qt::CaseInsensitive) == 0;
	});
	list.prepend(trimmed);
	return list.mid(0, limit);
}

} // namespace Window::Squidward
```

- [ ] **Step 5: Запустить тест**

Run: `Telegram/build/ggtg/test_squidward_parse.sh`
Expected: `All checks passed`

---

### Task 2: Строки списков групп и участников

**Files:**
- Modify: `Telegram/SourceFiles/window/ggtg_squidward_parse.cpp` (добавить функции перед `} // namespace Window::Squidward`)
- Test: `Telegram/SourceFiles/window/ggtg_squidward_parse_test.cpp`

**Interfaces:**
- Consumes: `UsernameFromLink`, `PrivateLink` (Task 1).
- Produces: `ExtractGroupRows`, `ExtractUserRows`, `DedupeGroupRows`, `DedupeUsernames` (сигнатуры — в заголовке Task 1).

- [ ] **Step 1: Написать тест**

В `ggtg_squidward_parse_test.cpp` перед `} // namespace` добавить:

```cpp
[[nodiscard]] Message GroupsPage() {
	auto m = Message();
	m.text = u"Чаты аккаунта 149474479.\n"
		u"👮-админ, 🔒-приватный, ✖️-вышел\n"
		u"Посл. сообщ - чат (всего сообщений)\n\n"
		u"12:06 FAMBUN 🐇 (1503)\n"
		u"11:57 Teo Iŋfłuencers™.. (81)\n"
		u"09:29 pemuluŋ jalanan (922)\n\n"
		u"Всего 1262, страница 1 из 85"_s;
	m.entities = {
		Ent(Entity::Type::CustomUrl, m.text, u"149474479"_s, u"tg://user?id=149474479"_s),
		Ent(Entity::Type::CustomUrl, m.text, u"FAMBUN 🐇"_s, u"https://t.me/fambun_chat"_s),
		Ent(Entity::Type::CustomUrl, m.text, u"Teo Iŋfłuencers™.."_s, u"https://t.me/c/2361171296/896316"_s),
		Ent(Entity::Type::CustomUrl, m.text, u"pemuluŋ jalanan"_s, u"https://t.me/FAMBUN_chat"_s),
		Ent(Entity::Type::Bold, m.text, u"1262"_s),
	};
	return m;
}

void TestRows() {
	const auto rows = ExtractGroupRows(GroupsPage());
	CHECK_EQ(int(rows.size()), 3); // the account-id link is not a group
	CHECK_STR(rows[0].username, u"fambun_chat"_s);
	CHECK_STR(rows[0].title, u"FAMBUN 🐇"_s);
	CHECK(rows[1].username.isEmpty());
	CHECK_STR(rows[1].link, u"https://t.me/c/2361171296/896316"_s);
	CHECK_STR(rows[1].title, u"Teo Iŋfłuencers™.."_s);

	CHECK_EQ(int(DedupeGroupRows(rows).size()), 2);
	auto twice = rows;
	twice.insert(twice.end(), rows.begin(), rows.end());
	CHECK_EQ(int(DedupeGroupRows(twice).size()), 2);

	auto members = Message();
	members.text = u"Участники чата\n"
		u"01. ✓ sheAy fourtis - 449\n"
		u"02. ✓ RayRed - 243\n"
		u"03. ✓ HelperBot - 3\n\n"
		u"Всего 8067, страница 1 из 404"_s;
	members.entities = {
		Ent(Entity::Type::MentionName, members.text, u"sheAy fourtis"_s, u"12345.678"_s),
		Ent(Entity::Type::CustomUrl, members.text, u"RayRed"_s, u"https://t.me/RayRed"_s),
		Ent(Entity::Type::CustomUrl, members.text, u"HelperBot"_s, u"https://t.me/HelperBot"_s),
	};
	const auto users = ExtractUserRows(members);
	CHECK_EQ(int(users.size()), 3);
	CHECK_STR(users[0].mentionData, u"12345.678"_s);
	CHECK_STR(users[1].username, u"RayRed"_s);

	CHECK_EQ(
		DedupeUsernames({ u"@RayRed"_s, u"rayred"_s, u"HelperBot"_s, u""_s, u"@Mogu"_s }),
		(QStringList{ u"RayRed"_s, u"Mogu"_s }));
}
```

В `main()` после `TestFoundation();` добавить `TestRows();`.

- [ ] **Step 2: Убедиться, что тест падает**

Run: `Telegram/build/ggtg/test_squidward_parse.sh`
Expected: FAIL — undefined symbols `ExtractGroupRows`, `DedupeGroupRows`, `ExtractUserRows`, `DedupeUsernames`.

- [ ] **Step 3: Реализовать**

В `ggtg_squidward_parse.cpp` перед `} // namespace Window::Squidward`:

```cpp
std::vector<GroupRow> ExtractGroupRows(const Message &message) {
	auto result = std::vector<GroupRow>();
	const auto addLink = [&](const QString &shown, const QString &link) {
		const auto title = (shown == link) ? QString() : shown.trimmed();
		if (const auto username = UsernameFromLink(link); !username.isEmpty()) {
			result.push_back({ .username = username, .title = title });
		} else if (const auto closed = PrivateLink(link); !closed.isEmpty()) {
			result.push_back({ .title = title, .link = closed });
		}
		// Anything else (tg://user?id=... in the header, etc.) isn't a group.
	};
	for (const auto &entity : message.entities) {
		const auto shown = message.text.mid(entity.offset, entity.length);
		switch (entity.type) {
		case Entity::Type::Mention:
			result.push_back({ .username = shown.mid(1) });
			break;
		case Entity::Type::Url:
			addLink(shown, shown);
			break;
		case Entity::Type::CustomUrl:
			addLink(shown, entity.data);
			break;
		default:
			break;
		}
	}
	return result;
}

std::vector<UserRow> ExtractUserRows(const Message &message) {
	auto result = std::vector<UserRow>();
	for (const auto &entity : message.entities) {
		const auto shown = message.text.mid(entity.offset, entity.length);
		if (entity.type == Entity::Type::MentionName) {
			result.push_back({ .mentionData = entity.data });
		} else if (entity.type == Entity::Type::Url
			|| entity.type == Entity::Type::CustomUrl) {
			const auto username = UsernameFromLink(
				entity.type == Entity::Type::Url ? shown : entity.data);
			if (!username.isEmpty()) {
				result.push_back({ .username = username });
			}
		}
	}
	return result;
}

std::vector<GroupRow> DedupeGroupRows(std::vector<GroupRow> rows) {
	auto seen = QSet<QString>();
	auto result = std::vector<GroupRow>();
	for (auto &row : rows) {
		const auto key = !row.username.isEmpty()
			? (u"@"_s + row.username.toLower())
			: row.link.toLower();
		if (key.isEmpty() || seen.contains(key)) {
			continue;
		}
		seen.insert(key);
		result.push_back(std::move(row));
	}
	return result;
}

QStringList DedupeUsernames(const QStringList &usernames) {
	auto seen = QSet<QString>();
	auto result = QStringList();
	for (const auto &raw : usernames) {
		auto name = raw.trimmed();
		if (name.startsWith(u'@')) {
			name.remove(0, 1);
		}
		const auto key = name.toLower();
		if (name.isEmpty()
			|| key.endsWith(u"bot"_s)
			|| seen.contains(key)) {
			continue;
		}
		seen.insert(key);
		result.push_back(name);
	}
	return result;
}
```

Добавить в начало файла `#include <QtCore/QSet>`. `using namespace Qt::StringLiterals;` уже внутри анонимного пространства; для функций вне него добавить `using namespace Qt::StringLiterals;` сразу после `namespace Window::Squidward {` (строкой выше `namespace {`), а строку внутри анонимного пространства удалить.

- [ ] **Step 4: Запустить тест**

Run: `Telegram/build/ggtg/test_squidward_parse.sh`
Expected: `All checks passed`

---

### Task 3: Досье человека

**Files:**
- Modify: `Telegram/SourceFiles/window/ggtg_squidward_parse.cpp`
- Test: `Telegram/SourceFiles/window/ggtg_squidward_parse_test.cpp`

**Interfaces:**
- Consumes: `Fold`, `FormatCount` (Task 1).
- Produces: `ParseUserCard`, `FormatUserDetails`; внутренний помощник `Reader` (используется и в Task 4).

- [ ] **Step 1: Написать тест**

Перед `} // namespace` в тесте:

```cpp
[[nodiscard]] Message UserCardMessage() {
	auto m = Message();
	m.text = u"Это Qiuvviɳɳ\n"
		u"Разнообразие сообщ. 67,15%\n"
		u"С 13.06.2018 по сегодня 12:06\n"
		u"30707 сообщений в 446 чатах\n"
		u"96,16% реплаи 85,85% медиа\n"
		u"Кружки: 0, голос: 0\n"
		u"любимый чат: pemuluŋ jałanaŋ\n"
		u"Искали: 2\n"
		u"Кαнал: Proofs\n\n"
		u"ɪᴅ: 149474479\n"
		u"userŋames: (3 oƒ 10)\n"
		u"| @Qiuvvinn | @RandyPlenger | @WangiWon\n"
		u"Имена: (3 из 10)\n"
		u"├2026-09-17 ➔ 🍀 Qiuvviɳɳ 🍀\n"
		u"├2026-09-17 ➔ 😎 Qiuvviɳɳ 😎\n"
		u"├2026-09-14 ➔ Qiuvviɳɳ@stakecruncher *FREPP"_s;
	return m;
}

void TestUserCard() {
	const auto card = ParseUserCard(UserCardMessage());
	CHECK(card.has_value());
	if (!card) {
		return;
	}
	CHECK_STR(card->name, u"Qiuvviɳɳ"_s);
	CHECK_STR(card->id, u"149474479"_s);
	CHECK_STR(card->diversity, u"67,15%"_s);
	CHECK_STR(card->activeSince, u"13.06.2018"_s);
	CHECK_STR(card->activeUntil, u"сегодня 12:06"_s);
	CHECK_EQ(card->messages, 30707);
	CHECK_EQ(card->chats, 446);
	CHECK_STR(card->replies, u"96,16%"_s);
	CHECK_STR(card->media, u"85,85%"_s);
	CHECK_EQ(card->roundVideos, 0);
	CHECK_EQ(card->voices, 0);
	CHECK_STR(card->favoriteChat, u"pemuluŋ jałanaŋ"_s);
	CHECK_EQ(card->searched, 2);
	CHECK_STR(card->channel, u"Proofs"_s);
	CHECK_EQ(
		card->usernames,
		(QStringList{ u"Qiuvvinn"_s, u"RandyPlenger"_s, u"WangiWon"_s }));
	CHECK_EQ(int(card->names.size()), 3);
	if (card->names.size() == 3) {
		CHECK_STR(card->names[0].date, u"2026-09-17"_s);
		CHECK_STR(card->names[0].name, u"🍀 Qiuvviɳɳ 🍀"_s);
		CHECK_STR(card->names[2].name, u"Qiuvviɳɳ@stakecruncher *FREPP"_s);
	}

	const auto lines = FormatUserDetails(*card);
	CHECK_EQ(lines.size(), 7);
	if (lines.size() == 7) {
		CHECK_STR(lines[0], u"Активен с 13.06.2018 по сегодня 12:06"_s);
		CHECK_STR(lines[1], FormatCount(30707) + u" сообщ. в "_s + FormatCount(446) + u" чатах"_s);
		CHECK_STR(lines[3], u"Реплаи 96,16% · медиа 85,85% · кружки 0 · голос 0"_s);
		CHECK_STR(lines[5], u"Канал: Proofs · искали: 2"_s);
		CHECK_STR(lines[6], u"Юзернеймы: @Qiuvvinn @RandyPlenger @WangiWon"_s);
	}

	// Review focus 1: a card with most lines missing.
	auto partial = Message();
	partial.text = u"Это Someone\nɪᴅ: 42\n30 сообщений в 3 чатах"_s;
	const auto small = ParseUserCard(partial);
	CHECK(small.has_value());
	if (small) {
		CHECK_EQ(
			FormatUserDetails(*small),
			(QStringList{ FormatCount(30) + u" сообщ. в "_s + FormatCount(3) + u" чатах"_s }));
	}
	auto notACard = Message();
	notACard.text = u"Ничего не найдено"_s;
	CHECK(!ParseUserCard(notACard).has_value());
}
```

В `main()` добавить `TestUserCard();`.

- [ ] **Step 2: Убедиться, что тест падает**

Run: `Telegram/build/ggtg/test_squidward_parse.sh`
Expected: FAIL — undefined symbols `ParseUserCard`, `FormatUserDetails`.

- [ ] **Step 3: Реализовать**

В анонимное пространство `ggtg_squidward_parse.cpp` (после `FoldChar`) добавить помощник:

```cpp
// A folded, escaped label for use inside a pattern.
[[nodiscard]] QString L(const QString &label);

// Line-by-line matching on the folded text; captures are cut from the
// original line at the same positions (Fold keeps them aligned).
class Reader final {
public:
	explicit Reader(const QString &text) {
		for (const auto &line : text.split(u'\n')) {
			_lines.push_back({ line, Fold(line) });
		}
	}

	[[nodiscard]] int size() const {
		return int(_lines.size());
	}
	[[nodiscard]] const QString &original(int index) const {
		return _lines[index].original;
	}

	// Captures of the first line at or after `from` matching `pattern`.
	[[nodiscard]] std::optional<QStringList> find(
			const QString &pattern,
			int *index = nullptr,
			int from = 0) const {
		const auto re = QRegularExpression(pattern);
		for (auto i = from; i < size(); ++i) {
			const auto match = re.match(_lines[i].folded);
			if (!match.hasMatch()) {
				continue;
			}
			if (index) {
				*index = i;
			}
			auto result = QStringList();
			for (auto n = 1; n <= match.lastCapturedIndex(); ++n) {
				result.push_back(_lines[i].original.mid(
					match.capturedStart(n),
					match.capturedLength(n)).trimmed());
			}
			return result;
		}
		return std::nullopt;
	}

	[[nodiscard]] bool has(const QString &pattern) const {
		return find(pattern).has_value();
	}

private:
	struct Line {
		QString original;
		QString folded;
	};
	std::vector<Line> _lines;

};

[[nodiscard]] int ToInt(const QString &text) {
	auto digits = QString();
	for (const auto ch : text) {
		if (ch.isDigit()) {
			digits.push_back(ch);
		}
	}
	return digits.isEmpty() ? -1 : digits.toInt();
}

// Shared by both cards.
constexpr auto kNamePattern = u"^\\s*это\\s+(.+)$";
constexpr auto kIdPattern = u"^\\s*id\\s*:\\s*(\\d+)";
```

И после конца анонимного пространства, до `QString Fold(...)`, определение `L` (оно использует `Fold`, объявленный в заголовке):

```cpp
namespace {

QString L(const QString &label) {
	return QRegularExpression::escape(Fold(label));
}

[[nodiscard]] QString Capitalized(QString text) {
	if (!text.isEmpty()) {
		text[0] = text[0].toUpper();
	}
	return text;
}

[[nodiscard]] QString DiversityPattern() {
	return L(u"разнообразие сообщ"_s) + uR"(\S*\s*([\d.,]+\s*%))"_s;
}

} // namespace
```

(`kNamePattern` и `kIdPattern` уже в свёрнутом виде: «это» и «id» не меняются при `Fold`.)

Затем перед `} // namespace Window::Squidward`:

```cpp
std::optional<UserCard> ParseUserCard(const Message &message) {
	const auto reader = Reader(message.text);
	const auto isGroup = reader.has(L(u"в топе"_s));
	const auto activity = reader.find(uR"((\d+)\s+)"_s
		+ L(u"сообщени"_s) + uR"(\S*\s+)"_s
		+ L(u"в"_s) + uR"(\s+(\d+)\s+)"_s + L(u"чат"_s));
	const auto style = reader.find(uR"(([\d.,]+\s*%)\s*)"_s
		+ L(u"реплаи"_s) + uR"(\s*([\d.,]+\s*%)\s*)"_s + L(u"медиа"_s));
	if (isGroup || (!activity && !style)) {
		return std::nullopt;
	}
	auto card = UserCard();
	if (const auto name = reader.find(QString::fromUtf16(kNamePattern))) {
		card.name = name->value(0);
	}
	if (const auto id = reader.find(QString::fromUtf16(kIdPattern))) {
		card.id = id->value(0);
	}
	if (activity) {
		card.messages = ToInt(activity->value(0));
		card.chats = ToInt(activity->value(1));
	}
	if (style) {
		card.replies = style->value(0);
		card.media = style->value(1);
	}
	if (const auto diversity = reader.find(DiversityPattern())) {
		card.diversity = diversity->value(0);
	}
	if (const auto period = reader.find(uR"(^\s*)"_s + L(u"с"_s)
			+ uR"(\s+(\d{2}\.\d{2}\.\d{4})\s+)"_s + L(u"по"_s)
			+ uR"(\s+(.+)$)"_s)) {
		card.activeSince = period->value(0);
		card.activeUntil = period->value(1);
	}
	if (const auto rv = reader.find(L(u"кружки"_s)
			+ uR"(\s*:\s*(\d+)\s*,\s*)"_s + L(u"голос"_s)
			+ uR"(\S*\s*:\s*(\d+))"_s)) {
		card.roundVideos = ToInt(rv->value(0));
		card.voices = ToInt(rv->value(1));
	}
	if (const auto favorite = reader.find(
			L(u"любимый чат"_s) + uR"(\s*:\s*(.+)$)"_s)) {
		card.favoriteChat = favorite->value(0);
	}
	if (const auto searched = reader.find(
			L(u"искали"_s) + uR"(\s*:\s*(\d+))"_s)) {
		card.searched = ToInt(searched->value(0));
	}
	if (const auto channel = reader.find(
			L(u"канал"_s) + uR"(\s*:\s*(.+)$)"_s)) {
		card.channel = channel->value(0);
	}

	auto namesAt = -1;
	const auto names = reader.find(uR"(^\s*)"_s + L(u"имена"_s), &namesAt);
	auto usernamesAt = -1;
	if (reader.find(L(u"usernames"_s), &usernamesAt)) {
		static const auto mention = QRegularExpression(
			uR"(@([A-Za-z0-9_]{4,32}))"_s);
		const auto last = std::min(
			usernamesAt + 1,
			(namesAt > usernamesAt) ? (namesAt - 1) : (reader.size() - 1));
		for (auto i = usernamesAt; i <= last; ++i) {
			auto it = mention.globalMatch(reader.original(i));
			while (it.hasNext()) {
				card.usernames.push_back(it.next().captured(1));
			}
		}
	}
	if (names) {
		static const auto change = QRegularExpression(
			uR"(^[\s├└│|]*(\d{4}-\d{2}-\d{2})\s*[➔→➜⮕>\-–—]*\x{FE0F}?\s*(.+?)\s*$)"_s);
		for (auto i = namesAt + 1; i < reader.size(); ++i) {
			const auto line = reader.original(i);
			const auto match = change.match(line);
			if (!match.hasMatch()) {
				if (line.trimmed().isEmpty()) {
					continue;
				}
				break;
			}
			card.names.push_back({ match.captured(1), match.captured(2) });
		}
	}
	return card;
}

QStringList FormatUserDetails(const UserCard &card) {
	auto result = QStringList();
	if (!card.activeSince.isEmpty()) {
		result.push_back(u"Активен с "_s + card.activeSince
			+ (card.activeUntil.isEmpty()
				? QString()
				: (u" по "_s + card.activeUntil)));
	}
	if (card.messages >= 0 && card.chats >= 0) {
		result.push_back(FormatCount(card.messages) + u" сообщ. в "_s
			+ FormatCount(card.chats) + u" чатах"_s);
	}
	if (!card.favoriteChat.isEmpty()) {
		result.push_back(u"Любимый чат: "_s + card.favoriteChat);
	}
	auto style = QStringList();
	if (!card.replies.isEmpty()) {
		style.push_back(u"реплаи "_s + card.replies);
	}
	if (!card.media.isEmpty()) {
		style.push_back(u"медиа "_s + card.media);
	}
	if (card.roundVideos >= 0) {
		style.push_back(u"кружки "_s + QString::number(card.roundVideos));
	}
	if (card.voices >= 0) {
		style.push_back(u"голос "_s + QString::number(card.voices));
	}
	if (!style.isEmpty()) {
		result.push_back(Capitalized(style.join(u" · "_s)));
	}
	if (!card.diversity.isEmpty()) {
		result.push_back(u"Разнообразие сообщений "_s + card.diversity);
	}
	auto extra = QStringList();
	if (!card.channel.isEmpty()) {
		extra.push_back(u"канал: "_s + card.channel);
	}
	if (card.searched >= 0) {
		extra.push_back(u"искали: "_s + QString::number(card.searched));
	}
	if (!extra.isEmpty()) {
		result.push_back(Capitalized(extra.join(u" · "_s)));
	}
	if (!card.usernames.isEmpty()) {
		auto handles = QStringList();
		for (const auto &name : card.usernames) {
			handles.push_back(u"@"_s + name);
		}
		result.push_back(u"Юзернеймы: "_s + handles.join(u' '));
	}
	return result;
}
```

- [ ] **Step 4: Запустить тест**

Run: `Telegram/build/ggtg/test_squidward_parse.sh`
Expected: `All checks passed`. Если не совпала строка — вывод покажет actual/expected; править шаблон в `ParseUserCard`, не тест.

---

### Task 4: Досье группы

**Files:**
- Modify: `Telegram/SourceFiles/window/ggtg_squidward_parse.cpp`
- Test: `Telegram/SourceFiles/window/ggtg_squidward_parse_test.cpp`

**Interfaces:**
- Consumes: `Reader`, `L`, `ToInt`, `Capitalized`, `DiversityPattern`, `kNamePattern`, `kIdPattern` (Task 3), `FormatCount` (Task 1).
- Produces: `ParseGroupCard`, `FormatGroupDetails`.

- [ ] **Step 1: Написать тест**

```cpp
[[nodiscard]] Message GroupCardMessage(bool withDescription) {
	auto m = Message();
	m.text = u"Это ƑAMBuN 🐇🐰👯\n"
		u"В топе # 17851 из 756778\n"
		u"Разнообразие сообщ. 9,80%\n"
		u"349 человek за сутки\n"
		u"написали 579 сообщений\n"
		u"из них 572 (98,99%) медиа\n"
		u"0 кружков и 0 голосовых\n"
		u"İD: 2303994538\n"
		u"Описание:\n"
		u"--- --- --- --- --- --- ---\n"_s
		+ (withDescription
			? u"Лучший чат о розыгрышах\nЗаходите!\n"_s
			: u"\n\n"_s)
		+ u"--- --- --- --- --- --- ---\n"
		u"Топ за сутки:\n"
		u"1. Ay🐰 - 25\n"
		u"2. ołaaaa - 18\n"
		u"3. S R - 15"_s;
	return m;
}

void TestGroupCard() {
	const auto card = ParseGroupCard(GroupCardMessage(true));
	CHECK(card.has_value());
	if (!card) {
		return;
	}
	CHECK_STR(card->name, u"ƑAMBuN 🐇🐰👯"_s);
	CHECK_STR(card->id, u"2303994538"_s);
	CHECK_EQ(card->topPlace, 17851);
	CHECK_EQ(card->topOf, 756778);
	CHECK_STR(card->diversity, u"9,80%"_s);
	CHECK_EQ(card->peopleDay, 349);
	CHECK_EQ(card->messagesDay, 579);
	CHECK_EQ(card->mediaDay, 572);
	CHECK_STR(card->mediaDayPercent, u"98,99%"_s);
	CHECK_EQ(card->roundVideosDay, 0);
	CHECK_EQ(card->voicesDay, 0);
	CHECK_STR(card->description, u"Лучший чат о розыгрышах\nЗаходите!"_s);
	CHECK_EQ(int(card->topDay.size()), 3);
	if (card->topDay.size() == 3) {
		CHECK_STR(card->topDay[0].name, u"Ay🐰"_s);
		CHECK_EQ(card->topDay[0].messages, 25);
		CHECK_STR(card->topDay[2].name, u"S R"_s);
	}
	const auto lines = FormatGroupDetails(*card);
	CHECK_EQ(lines.size(), 4);
	if (lines.size() == 4) {
		CHECK_STR(lines[0], u"В топе #"_s + FormatCount(17851) + u" из "_s + FormatCount(756778));
		CHECK_STR(lines[1], u"За сутки: "_s + FormatCount(349) + u" человек · "_s
			+ FormatCount(579) + u" сообщ. · медиа "_s + FormatCount(572) + u" (98,99%)"_s);
		CHECK_STR(lines[2], u"Кружки 0 · голосовые 0"_s);
	}

	const auto empty = ParseGroupCard(GroupCardMessage(false));
	CHECK(empty.has_value() && empty->description.isEmpty());

	// A card is one kind or the other, never both.
	CHECK(!ParseUserCard(GroupCardMessage(true)).has_value());
	CHECK(!ParseGroupCard(UserCardMessage()).has_value());
}
```

В `main()` добавить `TestGroupCard();`.

- [ ] **Step 2: Убедиться, что тест падает**

Run: `Telegram/build/ggtg/test_squidward_parse.sh`
Expected: FAIL — undefined symbols `ParseGroupCard`, `FormatGroupDetails`.

- [ ] **Step 3: Реализовать**

```cpp
std::optional<GroupCard> ParseGroupCard(const Message &message) {
	const auto reader = Reader(message.text);
	const auto top = reader.find(L(u"в топе"_s)
		+ uR"(\s*#\s*(\d+)\s*)"_s + L(u"из"_s) + uR"(\s*(\d+))"_s);
	if (!top && !reader.has(L(u"за сутки"_s))) {
		return std::nullopt;
	}
	auto card = GroupCard();
	if (top) {
		card.topPlace = ToInt(top->value(0));
		card.topOf = ToInt(top->value(1));
	}
	if (const auto name = reader.find(QString::fromUtf16(kNamePattern))) {
		card.name = name->value(0);
	}
	if (const auto id = reader.find(QString::fromUtf16(kIdPattern))) {
		card.id = id->value(0);
	}
	if (const auto diversity = reader.find(DiversityPattern())) {
		card.diversity = diversity->value(0);
	}
	if (const auto people = reader.find(uR"((\d+)\s*)"_s
			+ L(u"челов"_s) + uR"(\S*\s*)"_s + L(u"за сутки"_s))) {
		card.peopleDay = ToInt(people->value(0));
	}
	if (const auto written = reader.find(L(u"написали"_s)
			+ uR"(\s*(\d+)\s*)"_s + L(u"сообщ"_s))) {
		card.messagesDay = ToInt(written->value(0));
	}
	if (const auto media = reader.find(L(u"из них"_s)
			+ uR"(\s*(\d+)\s*\(([\d.,]+\s*%)\)\s*)"_s + L(u"медиа"_s))) {
		card.mediaDay = ToInt(media->value(0));
		card.mediaDayPercent = media->value(1);
	}
	if (const auto rv = reader.find(uR"((\d+)\s*)"_s + L(u"кружк"_s)
			+ uR"(\S*\s*)"_s + L(u"и"_s) + uR"(\s*(\d+)\s*)"_s
			+ L(u"голос"_s))) {
		card.roundVideosDay = ToInt(rv->value(0));
		card.voicesDay = ToInt(rv->value(1));
	}

	auto topAt = -1;
	const auto topDay = reader.find(
		uR"(^\s*)"_s + L(u"топ за сутки"_s),
		&topAt);
	auto descriptionAt = -1;
	if (const auto description = reader.find(
			uR"(^\s*)"_s + L(u"описание"_s) + uR"(\s*:(.*)$)"_s,
			&descriptionAt)) {
		static const auto filler = QRegularExpression(uR"(^[\s\-–—]*$)"_s);
		auto lines = QStringList();
		if (!description->value(0).isEmpty()) {
			lines.push_back(description->value(0));
		}
		const auto end = (topAt > descriptionAt) ? topAt : reader.size();
		for (auto i = descriptionAt + 1; i < end; ++i) {
			const auto line = reader.original(i).trimmed();
			if (!filler.match(line).hasMatch()) {
				lines.push_back(line);
			}
		}
		card.description = lines.join(u'\n').trimmed();
	}
	if (topDay) {
		static const auto row = QRegularExpression(
			uR"(^\s*(\d+)\.\s*(.+?)\s*[-–—]\s*(\d+)\s*$)"_s);
		for (auto i = topAt + 1; i < reader.size(); ++i) {
			const auto line = reader.original(i);
			const auto match = row.match(line);
			if (!match.hasMatch()) {
				if (line.trimmed().isEmpty()) {
					continue;
				}
				break;
			}
			card.topDay.push_back({
				match.captured(2),
				match.captured(3).toInt(),
			});
		}
	}
	return card;
}

QStringList FormatGroupDetails(const GroupCard &card) {
	auto result = QStringList();
	if (card.topPlace >= 0) {
		result.push_back(u"В топе #"_s + FormatCount(card.topPlace)
			+ (card.topOf >= 0
				? (u" из "_s + FormatCount(card.topOf))
				: QString()));
	}
	auto day = QStringList();
	if (card.peopleDay >= 0) {
		day.push_back(FormatCount(card.peopleDay) + u" человек"_s);
	}
	if (card.messagesDay >= 0) {
		day.push_back(FormatCount(card.messagesDay) + u" сообщ."_s);
	}
	if (card.mediaDay >= 0) {
		day.push_back(u"медиа "_s + FormatCount(card.mediaDay)
			+ (card.mediaDayPercent.isEmpty()
				? QString()
				: (u" ("_s + card.mediaDayPercent + u')')));
	}
	if (!day.isEmpty()) {
		result.push_back(u"За сутки: "_s + day.join(u" · "_s));
	}
	auto voice = QStringList();
	if (card.roundVideosDay >= 0) {
		voice.push_back(u"кружки "_s + QString::number(card.roundVideosDay));
	}
	if (card.voicesDay >= 0) {
		voice.push_back(u"голосовые "_s + QString::number(card.voicesDay));
	}
	if (!voice.isEmpty()) {
		result.push_back(Capitalized(voice.join(u" · "_s)));
	}
	if (!card.diversity.isEmpty()) {
		result.push_back(u"Разнообразие сообщений "_s + card.diversity);
	}
	return result;
}
```

- [ ] **Step 4: Запустить тест**

Run: `Telegram/build/ggtg/test_squidward_parse.sh`
Expected: `All checks passed`

---

### Task 5: Работа с ботом (Session)

**Files:**
- Create: `Telegram/SourceFiles/window/ggtg_squidward_session.h`
- Create: `Telegram/SourceFiles/window/ggtg_squidward_session.cpp`
- Modify: `Telegram/CMakeLists.txt` (после строки `window/ggtg_squidward.h`)

**Interfaces:**
- Consumes: весь `ggtg_squidward_parse.h` (Task 1–4); `Data::ScrapedGroupEntry`, `Data::GroupBaseTitle` (`data/scraped_members_db.h`).
- Produces (для Task 6):

```cpp
namespace Window::Squidward {
enum class BotStatus { Connecting, Ready, Failed };
enum class CardKind { User, Group, Unknown };
struct Card { CardKind kind; std::optional<UserCard> user; std::optional<GroupCard> group;
	QString rawText; QString query; QString baseTitle; };
struct Progress { int page; int pages; int total; int found; bool checking; };
struct GroupsResult { std::vector<Data::ScrapedGroupEntry> groups; int channelsRemoved; int closed; bool stopped; };
struct UsersResult { QStringList usernames; bool stopped; };
class Session : public base::has_weak_ptr {
	explicit Session(not_null<Window::SessionController*> controller);
	rpl::producer<BotStatus> status() const; rpl::producer<Card> cards() const;
	rpl::producer<Progress> progress() const; rpl::producer<GroupsResult> groupsCollected() const;
	rpl::producer<UsersResult> usersCollected() const; rpl::producer<QString> errors() const;
	void lookup(const QString &query); void collect(); void stop();
	bool ready() const; bool collecting() const;
};
}
```

- [ ] **Step 1: Заголовок**

`Telegram/SourceFiles/window/ggtg_squidward_session.h`:

```cpp
/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and exceptions, see https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/timer.h"
#include "base/weak_ptr.h"
#include "data/data_msg_id.h"
#include "data/scraped_members_db.h"
#include "mtproto/sender.h"
#include "window/ggtg_squidward_parse.h"

class HistoryItem;
class UserData;

namespace Window {
class SessionController;
} // namespace Window

namespace Window::Squidward {

enum class BotStatus {
	Connecting,
	Ready,
	Failed,
};

enum class CardKind {
	User,
	Group,
	Unknown, // "not found", a limit notice, or a reply we can't read
};

struct Card {
	CardKind kind = CardKind::Unknown;
	std::optional<UserCard> user;
	std::optional<GroupCard> group;
	QString rawText; // the bot's reply, cleaned (see CleanText)
	QString query;
	QString baseTitle; // name for a base saved from this card
};

struct Progress {
	int page = 0;
	int pages = 0; // 0 until the bot's page counter is seen
	int total = -1;
	int found = 0;
	bool checking = false; // pages done, groups/users being checked
};

struct GroupsResult {
	std::vector<Data::ScrapedGroupEntry> groups;
	int channelsRemoved = 0;
	int closed = 0;
	bool stopped = false;
};

struct UsersResult {
	QStringList usernames; // without '@'
	bool stopped = false;
};

// Talks to @skvidvarde_bot from the window's account: a lookup ends in a
// Card, and collect() pages through the card's list (a person's groups, or a
// group's members) until the end, a stop() or a timeout. Paid buttons are
// never pressed.
class Session final : public base::has_weak_ptr {
public:
	explicit Session(not_null<Window::SessionController*> controller);

	[[nodiscard]] rpl::producer<BotStatus> status() const;
	[[nodiscard]] rpl::producer<Card> cards() const;
	[[nodiscard]] rpl::producer<Progress> progress() const;
	[[nodiscard]] rpl::producer<GroupsResult> groupsCollected() const;
	[[nodiscard]] rpl::producer<UsersResult> usersCollected() const;
	[[nodiscard]] rpl::producer<QString> errors() const;

	void lookup(const QString &query);
	// Collects the list of the last card; results come as groupsCollected()
	// for a person's card and usersCollected() for a group's.
	void collect();
	// Ends collecting now; what was collected so far still comes out.
	void stop();

	[[nodiscard]] bool ready() const;
	[[nodiscard]] bool collecting() const;

private:
	struct Collect {
		CardKind kind = CardKind::Unknown;
		FullMsgId listItem;
		bool waitingFirst = true;
		int lastPage = 0;
		int retries = 0;
		int safety = 2000;
		FullMsgId buttonItem;
		std::pair<int, int> button;
		std::vector<GroupRow> groups;
		std::vector<UserRow> users;
		Progress progress;
	};
	struct GroupCheck {
		std::vector<Data::ScrapedGroupEntry> source;
		std::vector<Data::ScrapedGroupEntry> kept;
		size_t next = 0;
		int channels = 0;
		int closed = 0;
		bool stopped = false;
		int generation = 0;
	};

	void resolveBot();
	void handle(not_null<HistoryItem*> item, bool edited);
	void handleCard(not_null<HistoryItem*> item, bool refresh);
	void handlePage(not_null<HistoryItem*> item, bool edited);
	void click(FullMsgId itemId, std::pair<int, int> button);
	void lookupTimeout();
	void pageTimeout();
	void finish(bool stopped);
	void finishGroups(std::vector<GroupRow> rows, bool stopped);
	void checkNextGroup();
	void settleGroup(
		Data::ScrapedGroupEntry entry,
		PeerData *peer,
		crl::time delay);
	void finishUsers(std::vector<UserRow> rows, bool stopped);
	void resolveUsers(QVector<MTPInputUser> pending, Fn<void()> done);
	[[nodiscard]] QString baseTitleFor(const QString &name) const;

	const not_null<Window::SessionController*> _controller;
	MTP::Sender _api;
	UserData *_bot = nullptr;
	rpl::variable<BotStatus> _status = BotStatus::Connecting;
	rpl::event_stream<Card> _cards;
	rpl::event_stream<Progress> _progress;
	rpl::event_stream<GroupsResult> _groups;
	rpl::event_stream<UsersResult> _users;
	rpl::event_stream<QString> _errors;

	QString _query;
	bool _awaitingCard = false;
	std::optional<Card> _card;
	FullMsgId _cardItem;
	std::optional<Collect> _collect;
	std::optional<GroupCheck> _check;
	bool _finishing = false;
	// Bumped by every lookup: results of an older one are dropped.
	int _generation = 0;
	base::Timer _lookupTimer;
	base::Timer _pageTimer;
	rpl::lifetime _lifetime;

};

} // namespace Window::Squidward
```

- [ ] **Step 2: Реализация**

`Telegram/SourceFiles/window/ggtg_squidward_session.cpp` — логика перенесена из старых `SquidwardResolveBot`, `SquidwardStartCollection`, `SquidwardCollectNextPage`, `SquidwardFilterAndShowGroups`, `SquidwardResolveAndShowUsers`:

```cpp
/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and exceptions, see https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "window/ggtg_squidward_session.h"

#include "api/api_bot.h"
#include "api/api_common.h"
#include "apiwrap.h"
#include "base/call_delayed.h"
#include "base/flat_set.h"
#include "core/click_handler_types.h"
#include "core/ggtg_log.h"
#include "data/data_changes.h"
#include "data/data_channel.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_item_components.h"
#include "history/history_item_reply_markup.h"
#include "main/main_session.h"
#include "ui/text/text_entity.h"
#include "window/window_session_controller.h"

namespace Window::Squidward {
namespace {

constexpr auto kBotUsername = "skvidvarde_bot";
constexpr auto kLookupTimeout = crl::time(30000);
constexpr auto kFirstPageTimeout = crl::time(40000);
constexpr auto kPageTimeout = crl::time(20000);

[[nodiscard]] Message ToMessage(not_null<HistoryItem*> item) {
	const auto text = item->originalText();
	auto result = Message{ .text = text.text };
	for (const auto &entity : text.entities) {
		auto type = Entity::Type::Other;
		switch (entity.type()) {
		case EntityType::Bold: type = Entity::Type::Bold; break;
		case EntityType::Url: type = Entity::Type::Url; break;
		case EntityType::CustomUrl: type = Entity::Type::CustomUrl; break;
		case EntityType::Mention: type = Entity::Type::Mention; break;
		case EntityType::MentionName:
			type = Entity::Type::MentionName;
			break;
		default: break;
		}
		result.entities.push_back({
			type,
			entity.offset(),
			entity.length(),
			entity.data(),
		});
	}
	return result;
}

[[nodiscard]] std::optional<std::pair<int, int>> FindButton(
		not_null<HistoryItem*> item,
		Fn<bool(const QString&)> match) {
	const auto markup = item->Get<HistoryMessageReplyMarkup>();
	if (!markup) {
		return std::nullopt;
	}
	const auto &rows = markup->data.rows;
	for (auto row = 0; row != int(rows.size()); ++row) {
		for (auto column = 0; column != int(rows[row].size()); ++column) {
			if (match(rows[row][column].text)) {
				return std::make_pair(row, column);
			}
		}
	}
	return std::nullopt;
}

[[nodiscard]] bool IsGroupsButton(const QString &text) {
	return ButtonLastWord(text) == Fold(u"группы"_q);
}

[[nodiscard]] bool IsPeopleButton(const QString &text) {
	return ButtonLastWord(text) == Fold(u"люди"_q)
		&& !text.contains(u"💎"_q);
}

[[nodiscard]] bool IsNextPageButton(const QString &text) {
	return text.simplified().startsWith(u"➡"_q) && !text.contains(u"💎"_q);
}

} // namespace

Session::Session(not_null<Window::SessionController*> controller)
: _controller(controller)
, _api(&controller->session().mtp())
, _lookupTimer([=] { lookupTimeout(); })
, _pageTimer([=] { pageTimeout(); }) {
	const auto session = &controller->session();
	session->data().newItemAdded(
	) | rpl::on_next([=](not_null<HistoryItem*> item) {
		handle(item, false);
	}, _lifetime);
	session->changes().messageUpdates(
		Data::MessageUpdate::Flag::Edited
	) | rpl::on_next([=](const Data::MessageUpdate &update) {
		handle(update.item, true);
	}, _lifetime);
	resolveBot();
}

rpl::producer<BotStatus> Session::status() const {
	return _status.value();
}

rpl::producer<Card> Session::cards() const {
	return _cards.events();
}

rpl::producer<Progress> Session::progress() const {
	return _progress.events();
}

rpl::producer<GroupsResult> Session::groupsCollected() const {
	return _groups.events();
}

rpl::producer<UsersResult> Session::usersCollected() const {
	return _users.events();
}

rpl::producer<QString> Session::errors() const {
	return _errors.events();
}

bool Session::ready() const {
	return _bot != nullptr;
}

bool Session::collecting() const {
	return _collect.has_value() || _finishing;
}

void Session::resolveBot() {
	const auto username = QString::fromLatin1(kBotUsername);
	const auto session = &_controller->session();
	if (const auto peer = session->data().peerByUsername(username)) {
		if (const auto user = peer->asUser()) {
			_bot = user;
			_status = BotStatus::Ready;
			return;
		}
	}
	_api.request(MTPcontacts_ResolveUsername(
		MTP_flags(MTPcontacts_ResolveUsername::Flags(0)),
		MTP_string(username),
		MTP_string(QString())
	)).done([=](const MTPcontacts_ResolvedPeer &result) {
		result.match([&](const MTPDcontacts_resolvedPeer &data) {
			session->data().processUsers(data.vusers());
			session->data().processChats(data.vchats());
			const auto peerId = peerFromMTP(data.vpeer());
			_bot = peerId ? session->data().peer(peerId)->asUser() : nullptr;
		});
		_status = _bot ? BotStatus::Ready : BotStatus::Failed;
	}).fail([=](const MTP::Error &error) {
		Core::GgtgLog::Info(
			u"Сквидвард"_q,
			u"Бот не найден: "_q + error.type());
		_status = BotStatus::Failed;
	}).send();
}

void Session::lookup(const QString &query) {
	const auto text = query.trimmed();
	if (text.isEmpty()) {
		return;
	}
	if (!_bot) {
		_errors.fire(u"Бот ещё не подключён."_q);
		return;
	}
	if (_collect) {
		stop();
	}
	++_generation;
	_finishing = false;
	_check.reset();
	_query = text;
	_card.reset();
	_cardItem = FullMsgId();
	_awaitingCard = true;
	Core::GgtgLog::Info(u"Сквидвард"_q, u"Пробив: "_q + text);
	auto message = Api::MessageToSend(Api::SendAction(
		_controller->session().data().history(_bot)));
	message.textWithTags = { text, TextWithTags::Tags() };
	_controller->session().api().sendMessage(std::move(message));
	_lookupTimer.callOnce(kLookupTimeout);
}

void Session::lookupTimeout() {
	if (!_awaitingCard) {
		return;
	}
	_awaitingCard = false;
	_errors.fire(u"Бот не отвечает. Попробуйте ещё раз."_q);
}

void Session::handle(not_null<HistoryItem*> item, bool edited) {
	if (!_bot || item->history()->peer != _bot || item->out()) {
		return;
	}
	const auto text = item->originalText().text;
	if (IsPlaceholder(text)) {
		return;
	}
	Core::GgtgLog::Info(u"Сквидвард"_q, u"Бот: "_q + text);
	if (_collect) {
		handlePage(item, edited);
	} else if (_awaitingCard) {
		handleCard(item, false);
	} else if (edited && item->fullId() == _cardItem && !_finishing) {
		// The bot sometimes fills the card in with a later edit.
		handleCard(item, true);
	}
}

void Session::handleCard(not_null<HistoryItem*> item, bool refresh) {
	const auto message = ToMessage(item);
	auto card = Card{
		.rawText = CleanText(message.text),
		.query = _query,
	};
	if (auto user = ParseUserCard(message)) {
		card.kind = CardKind::User;
		card.baseTitle = baseTitleFor(user->name);
		card.user = std::move(user);
	} else if (auto group = ParseGroupCard(message)) {
		card.kind = CardKind::Group;
		card.baseTitle = baseTitleFor(group->name);
		card.group = std::move(group);
	} else if (refresh) {
		return; // an edit into something we can't read keeps the old card
	}
	_awaitingCard = false;
	_lookupTimer.cancel();
	_card = card;
	_cardItem = item->fullId();
	_cards.fire(std::move(card));
}

QString Session::baseTitleFor(const QString &name) const {
	const auto username = UsernameFromQuery(_query);
	if (name.isEmpty()) {
		return username.isEmpty() ? _query : (u"@"_q + username);
	}
	return Data::GroupBaseTitle(name, username);
}

void Session::collect() {
	if (collecting() || !_card || _card->kind == CardKind::Unknown) {
		return;
	}
	const auto item = _controller->session().data().message(_cardItem);
	if (!item) {
		_errors.fire(u"Ответ бота больше не найден, пробейте заново."_q);
		return;
	}
	const auto isUser = (_card->kind == CardKind::User);
	const auto button = FindButton(
		item,
		isUser ? &IsGroupsButton : &IsPeopleButton);
	if (!button) {
		_errors.fire(isUser
			? u"В ответе бота нет кнопки «Группы»."_q
			: u"В ответе бота нет кнопки «Топ, люди»."_q);
		return;
	}
	_collect.emplace();
	_collect->kind = _card->kind;
	_collect->buttonItem = _cardItem;
	_collect->button = *button;
	_progress.fire_copy(_collect->progress);
	click(_cardItem, *button);
	_pageTimer.callOnce(kFirstPageTimeout);
}

void Session::click(FullMsgId itemId, std::pair<int, int> button) {
	Api::ActivateBotCommand(ClickHandlerContext{
		.itemId = itemId,
		.sessionWindow = base::make_weak(_controller),
	}, button.first, button.second);
}

void Session::handlePage(not_null<HistoryItem*> item, bool edited) {
	auto &c = *_collect;
	if (!c.waitingFirst && (!edited || item->fullId() != c.listItem)) {
		return;
	}
	const auto message = ToMessage(item);
	const auto info = ParsePageInfo(message.text);
	if (!c.waitingFirst && info && info->page == c.lastPage) {
		return; // the same page edited again
	}
	c.waitingFirst = false;
	c.listItem = item->fullId();
	c.retries = 0;
	_pageTimer.cancel();
	if (info) {
		c.lastPage = info->page;
	}
	if (c.kind == CardKind::User) {
		const auto rows = ExtractGroupRows(message);
		c.groups.insert(c.groups.end(), rows.begin(), rows.end());
		c.progress.found = int(DedupeGroupRows(c.groups).size());
	} else {
		const auto rows = ExtractUserRows(message);
		c.users.insert(c.users.end(), rows.begin(), rows.end());
		c.progress.found = int(c.users.size());
	}
	c.progress.page = info ? info->page : (c.progress.page + 1);
	c.progress.pages = info ? info->pages : 0;
	if (info && info->total >= 0) {
		c.progress.total = info->total;
	}
	_progress.fire_copy(c.progress);

	const auto next = FindButton(item, &IsNextPageButton);
	if (!next || (info && info->page >= info->pages) || --c.safety <= 0) {
		finish(false);
		return;
	}
	c.buttonItem = c.listItem;
	c.button = *next;
	click(c.buttonItem, c.button);
	_pageTimer.callOnce(kPageTimeout);
}

void Session::pageTimeout() {
	if (!_collect) {
		return;
	}
	auto &c = *_collect;
	if (c.retries++ < (c.waitingFirst ? 2 : 1)) {
		click(c.buttonItem, c.button);
		_pageTimer.callOnce(c.waitingFirst ? kFirstPageTimeout : kPageTimeout);
		return;
	}
	_errors.fire(c.waitingFirst
		? u"Бот не прислал список. Попробуйте ещё раз."_q
		: u"Страница не пришла — показано то, что успели собрать."_q);
	finish(true);
}

void Session::stop() {
	if (_collect) {
		finish(true);
	}
}

void Session::finish(bool stopped) {
	_pageTimer.cancel();
	auto c = std::move(*_collect);
	_collect.reset();
	_finishing = true;
	auto progress = c.progress;
	progress.checking = true;
	_progress.fire_copy(progress);
	if (c.kind == CardKind::User) {
		finishGroups(std::move(c.groups), stopped);
	} else {
		finishUsers(std::move(c.users), stopped);
	}
}

void Session::finishGroups(std::vector<GroupRow> rows, bool stopped) {
	auto check = GroupCheck{ .stopped = stopped, .generation = _generation };
	for (const auto &row : DedupeGroupRows(std::move(rows))) {
		if (!row.link.isEmpty()) {
			++check.closed;
		}
		check.source.push_back({
			.username = row.username,
			.title = row.title,
			.link = row.link,
		});
	}
	_check = std::move(check);
	checkNextGroup();
}

// Channels are of no use as recipients: every group with a username is
// looked up (cache first, then contacts.resolveUsername with a pause) and
// broadcast channels are dropped. A group that can't be checked now is kept.
void Session::checkNextGroup() {
	if (!_check || _check->generation != _generation) {
		return;
	}
	auto &check = *_check;
	while (check.next < check.source.size()
		&& check.source[check.next].username.isEmpty()) {
		check.kept.push_back(check.source[check.next++]);
	}
	if (check.next >= check.source.size()) {
		auto result = GroupsResult{
			.groups = std::move(check.kept),
			.channelsRemoved = check.channels,
			.closed = check.closed,
			.stopped = check.stopped,
		};
		_check.reset();
		_finishing = false;
		_groups.fire(std::move(result));
		return;
	}
	auto entry = check.source[check.next++];
	const auto session = &_controller->session();
	if (const auto peer = session->data().peerByUsername(entry.username)) {
		settleGroup(std::move(entry), peer, 0);
		return;
	}
	_api.request(MTPcontacts_ResolveUsername(
		MTP_flags(MTPcontacts_ResolveUsername::Flags(0)),
		MTP_string(entry.username),
		MTP_string(QString())
	)).done([=](const MTPcontacts_ResolvedPeer &result) {
		auto peer = (PeerData*)nullptr;
		result.match([&](const MTPDcontacts_resolvedPeer &data) {
			session->data().processUsers(data.vusers());
			session->data().processChats(data.vchats());
			const auto peerId = peerFromMTP(data.vpeer());
			peer = peerId ? session->data().peer(peerId).get() : nullptr;
		});
		settleGroup(entry, peer, 400);
	}).fail([=] {
		settleGroup(entry, nullptr, 800);
	}).send();
}

void Session::settleGroup(
		Data::ScrapedGroupEntry entry,
		PeerData *peer,
		crl::time delay) {
	if (!_check) {
		return;
	}
	const auto channel = peer ? peer->asChannel() : nullptr;
	if (channel && !channel->isMegagroup()) {
		++_check->channels;
	} else {
		if (channel) {
			if (entry.title.isEmpty()) {
				entry.title = channel->name();
			}
			entry.membersCount = std::max(
				entry.membersCount,
				channel->membersCount());
		}
		_check->kept.push_back(std::move(entry));
	}
	auto progress = Progress{
		.found = int(_check->kept.size()),
		.checking = true,
	};
	_progress.fire_copy(progress);
	base::call_delayed(delay, this, [=] { checkNextGroup(); });
}

void Session::finishUsers(std::vector<UserRow> rows, bool stopped) {
	const auto session = &_controller->session();
	const auto generation = _generation;
	auto names = QStringList();
	auto ids = std::vector<uint64>();
	auto toResolve = QVector<MTPInputUser>();
	auto seen = base::flat_set<uint64>();
	for (const auto &row : rows) {
		if (!row.username.isEmpty()) {
			names.push_back(row.username);
			continue;
		}
		const auto fields = TextUtilities::MentionNameDataToFields(
			row.mentionData);
		if (!fields.userId || !seen.emplace(fields.userId).second) {
			continue;
		}
		ids.push_back(fields.userId);
		if (!session->data().userLoaded(UserId(fields.userId))) {
			toResolve.push_back(MTP_inputUser(
				MTP_long(fields.userId),
				MTP_long(fields.accessHash)));
		}
	}
	resolveUsers(std::move(toResolve), [=] {
		if (generation != _generation) {
			return;
		}
		auto all = names;
		for (const auto id : ids) {
			if (const auto user = session->data().userLoaded(UserId(id))) {
				all.push_back(user->username());
			}
		}
		_finishing = false;
		_users.fire({ DedupeUsernames(all), stopped });
	});
}

void Session::resolveUsers(QVector<MTPInputUser> pending, Fn<void()> done) {
	if (pending.isEmpty()) {
		done();
		return;
	}
	const auto count = std::min<int>(100, pending.size());
	auto chunk = pending.mid(0, count);
	const auto rest = pending.mid(count);
	_api.request(MTPusers_GetUsers(
		MTP_vector<MTPInputUser>(std::move(chunk))
	)).done([=](const MTPVector<MTPUser> &result) {
		_controller->session().data().processUsers(result);
		resolveUsers(rest, done);
	}).fail([=] {
		resolveUsers(rest, done);
	}).send();
}

} // namespace Window::Squidward
```

- [ ] **Step 3: Добавить файлы в сборку**

`Telegram/CMakeLists.txt`, сразу после строки `    window/ggtg_squidward.h`:

```
    window/ggtg_squidward_parse.cpp
    window/ggtg_squidward_parse.h
    window/ggtg_squidward_session.cpp
    window/ggtg_squidward_session.h
```

`ggtg_squidward_parse_test.cpp` в CMake **не** добавлять (у него свой `main`).

- [ ] **Step 4: Пересобрать проект**

Run (из `nicegram-desktop/`, долго — запускать в фоне):
```bash
env -u QT cmake -S . -B out && cmake --build out --config Debug --target Telegram 2>&1 | grep -E "error:|BUILD (SUCCEEDED|FAILED)" | head -40
```
Expected: `** BUILD SUCCEEDED **` (старый `ggtg_squidward.cpp` пока не трогаем, новый код ещё никем не вызывается). Если есть ошибки — только в новых файлах; исправить (обычно не хватает `#include`) и повторить.

- [ ] **Step 5: Тест разбора всё ещё зелёный**

Run: `Telegram/build/ggtg/test_squidward_parse.sh`
Expected: `All checks passed`

---

### Task 6: Новый экран Сквидварда

**Files:**
- Modify (полная замена): `Telegram/SourceFiles/window/ggtg_squidward.cpp`
- Modify: `Telegram/SourceFiles/window/ggtg_widgets.h` (добавить `ProgressBar`)
- Modify: `Telegram/SourceFiles/settings/bulk_accounts_actions.h`, `.cpp` (добавить `ShowSavedBase`)
- Modify: `Telegram/SourceFiles/window/window_main_menu.cpp` (описание раздела)

**Interfaces:**
- Consumes: `Squidward::Session` и типы из Task 5; парсерные `FormatUserDetails`, `FormatGroupDetails`, `FormatCount`, `PushRecent`, `UsernameFromQuery` (Task 1–4).
- Produces: `Window::MakeSquidwardBox(controller, lookupGroup)` — сигнатура в `ggtg_squidward.h` не меняется; `Settings::ShowSavedBase(std::shared_ptr<Ui::Show>, uint64)`; `Ggtg::ProgressBar`.

- [ ] **Step 1: ProgressBar**

В `ggtg_widgets.h` перед `} // namespace Ggtg`:

```cpp
// A thin rounded progress line.
class ProgressBar final : public Ui::RpWidget {
public:
	explicit ProgressBar(not_null<QWidget*> parent)
	: RpWidget(parent.get()) {
		resize(width(), style::ConvertScale(6));
	}

	void setValue(double value) {
		_value = std::clamp(value, 0., 1.);
		update();
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		auto p = QPainter(this);
		auto hq = PainterHighQualityEnabler(p);
		const auto radius = height() / 2.;
		p.setPen(Qt::NoPen);
		p.setBrush(st::windowBgOver);
		p.drawRoundedRect(rect(), radius, radius);
		if (_value > 0.) {
			p.setBrush(st::windowActiveTextFg);
			p.drawRoundedRect(
				QRectF(0, 0, width() * _value, height()),
				radius,
				radius);
		}
	}

private:
	double _value = 0.;

};
```

- [ ] **Step 2: ShowSavedBase**

`settings/bulk_accounts_actions.h`, после объявления `MakeScrapedGroupsDbBox`:

```cpp
// Opens a saved base's editor (users or groups) over `show` - the same one
// a click on it in «База» opens.
void ShowSavedBase(std::shared_ptr<Ui::Show> show, uint64 id);
```

`settings/bulk_accounts_actions.cpp`, в самом конце перед `} // namespace Settings` (там уже видны `ShowEditBaseBox` и `ShowEditGroupsBaseBox`, они вне анонимных пространств):

```cpp
void ShowSavedBase(std::shared_ptr<Ui::Show> show, uint64 id) {
	for (const auto &meta : Data::LoadScrapedGroupsIndex()) {
		if (meta.id != id) {
			continue;
		}
		if (meta.kind == Data::BaseKind::Users) {
			ShowEditBaseBox(
				show,
				BasesActions(),
				id,
				meta.title,
				Data::LoadScrapedGroupUsernames(id),
				false);
		} else {
			ShowEditGroupsBaseBox(
				show,
				BasesActions(),
				id,
				meta.title,
				Data::LoadScrapedGroupEntries(id),
				false);
		}
		return;
	}
	show->showToast(u"База не найдена"_q);
}
```

- [ ] **Step 3: Заменить `ggtg_squidward.cpp` целиком**

```cpp
/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and exceptions, see https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "window/ggtg_squidward.h"

#include "data/scraped_members_db.h"
#include "settings.h"
#include "settings/bulk_accounts_actions.h"
#include "settings/settings_common.h"
#include "ui/boxes/confirm_box.h"
#include "ui/layers/generic_box.h"
#include "ui/text/text_utilities.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "window/ggtg_squidward_parse.h"
#include "window/ggtg_squidward_session.h"
#include "window/ggtg_widgets.h"
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"
#include "styles/style_widgets.h"

#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QPointer>
#include <QtCore/QSaveFile>
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>

namespace Window {
namespace {

using namespace Squidward;

constexpr auto kRecentLimit = 8;
constexpr auto kPreviewCount = 8;

[[nodiscard]] QString RecentPath() {
	return cWorkingDir() + u"tdata/ggtg_squidward_recent.json"_q;
}

[[nodiscard]] QStringList LoadRecent() {
	auto file = QFile(RecentPath());
	if (!file.open(QIODevice::ReadOnly)) {
		return {};
	}
	auto result = QStringList();
	for (const auto &value : QJsonDocument::fromJson(file.readAll()).array()) {
		const auto text = value.toString().trimmed();
		if (!text.isEmpty()) {
			result.push_back(text);
		}
	}
	return result.mid(0, kRecentLimit);
}

void SaveRecent(const QStringList &list) {
	auto file = QSaveFile(RecentPath());
	if (file.open(QIODevice::WriteOnly)) {
		file.write(QJsonDocument(QJsonArray::fromStringList(list)).toJson(
			QJsonDocument::Compact));
		file.commit();
	}
}

not_null<Ui::FlatLabel*> AddLabel(
		not_null<Ui::VerticalLayout*> to,
		const QString &text) {
	const auto label = to->add(
		object_ptr<Ui::FlatLabel>(to, text, st::boxLabel),
		st::boxRowPadding);
	label->setSelectable(true);
	return label;
}

// One or two buttons sharing a row.
void AddButtons(
		not_null<Ui::VerticalLayout*> to,
		std::vector<std::tuple<QString, const style::RoundButton*, Fn<void()>>> list) {
	auto wrapOwned = object_ptr<Ui::RpWidget>(to);
	const auto wrap = wrapOwned.data();
	auto buttons = std::vector<not_null<Ui::RoundButton*>>();
	for (auto &[text, st, callback] : list) {
		const auto button = Ui::CreateChild<Ui::RoundButton>(
			wrap,
			rpl::single(text),
			*st);
		button->setTextTransform(Ui::RoundButtonTextTransform::NoTransform);
		button->setClickedCallback(std::move(callback));
		buttons.push_back(button);
	}
	wrap->resize(wrap->width(), buttons.front()->height());
	wrap->widthValue(
	) | rpl::on_next([=](int width) {
		const auto skip = st::boxRowPadding.left() / 2;
		const auto count = int(buttons.size());
		const auto each = (width - skip * (count - 1)) / count;
		for (auto i = 0; i != count; ++i) {
			buttons[i]->setFullWidth(each);
			buttons[i]->moveToLeft(i * (each + skip), 0, width);
		}
	}, wrap->lifetime());
	to->add(std::move(wrapOwned), st::boxRowPadding);
}

// "▸ Title" that opens `body` below it.
void AddExpander(
		not_null<Ui::VerticalLayout*> to,
		const QString &title,
		const QString &body) {
	const auto shown = to->lifetime().make_state<rpl::variable<bool>>(false);
	const auto button = to->add(object_ptr<Ui::SettingsButton>(
		to,
		shown->value() | rpl::map([=](bool open) {
			return (open ? u"▾ "_q : u"▸ "_q) + title;
		})));
	const auto wrap = to->add(
		object_ptr<Ui::SlideWrap<Ui::FlatLabel>>(
			to,
			object_ptr<Ui::FlatLabel>(to, body, st::boxLabel),
			st::boxRowPadding));
	wrap->entity()->setSelectable(true);
	wrap->toggleOn(shown->value());
	wrap->finishAnimating();
	button->setClickedCallback([=] {
		*shown = !shown->current();
	});
}

[[nodiscard]] QString GroupName(const Data::ScrapedGroupEntry &entry) {
	if (!entry.link.isEmpty()) {
		return u"🔒 "_q + (entry.title.isEmpty() ? entry.link : entry.title);
	}
	return entry.title.isEmpty() ? (u"@"_q + entry.username) : entry.title;
}

[[nodiscard]] QString GroupLine(const Data::ScrapedGroupEntry &entry) {
	return !entry.username.isEmpty()
		? (u"@"_q + entry.username)
		: !entry.link.isEmpty()
		? entry.link
		: entry.title;
}

class Screen final {
public:
	Screen(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller,
		const QString &autoQuery);

private:
	void setupHeader();
	void setupSearch();
	void setupEvents();
	void submit();
	void start(const QString &query);
	void renderRecent();
	void renderIdle();
	void renderSearching(const QString &text);
	void renderLookupError(const QString &text);
	void renderCard(const Card &card);
	void renderUser(const Card &card);
	void renderGroup(const Card &card);
	void addHeader(
		const QString &title,
		const QString &subtitle,
		const QString &id);
	void renderCollectIdle();
	void renderProgress(const Progress &progress);
	void renderGroups(const GroupsResult &result);
	void renderUsers(const UsersResult &result);
	void renderSaved(
		uint64 id,
		const QString &title,
		int saved,
		int collected);
	void saveGroups();
	void saveUsers();

	const not_null<Ui::GenericBox*> _box;
	const std::unique_ptr<Session> _session;
	// 🦑 from «База»: looked up, collected and saved without any clicks.
	const QString _autoQuery;
	bool _autoCollect = false;
	Ui::FlatLabel *_status = nullptr;
	Ui::InputField *_field = nullptr;
	Ui::SlideWrap<Ui::FlatLabel> *_recent = nullptr;
	Ui::VerticalLayout *_card = nullptr;
	Ui::VerticalLayout *_collect = nullptr;
	QStringList _recentList;
	QString _lastQuery;
	QString _pending; // waiting for the bot to connect
	std::optional<Card> _current;
	std::optional<GroupsResult> _groups;
	std::optional<UsersResult> _users;

};

Screen::Screen(
	not_null<Ui::GenericBox*> box,
	not_null<Window::SessionController*> controller,
	const QString &autoQuery)
: _box(box)
, _session(std::make_unique<Session>(controller))
, _autoQuery(autoQuery.trimmed())
, _recentList(LoadRecent()) {
	setupHeader();
	setupSearch();
	Ui::AddDivider(_box->verticalLayout());
	_card = _box->addRow(
		object_ptr<Ui::VerticalLayout>(_box),
		style::margins());
	_collect = _box->addRow(
		object_ptr<Ui::VerticalLayout>(_box),
		style::margins());
	setupEvents();
	renderRecent();
	renderIdle();
	if (!_autoQuery.isEmpty()) {
		_autoCollect = true;
		_field->setText(_autoQuery);
		start(_autoQuery);
	}
}

void Screen::setupHeader() {
	_box->setTitle(rpl::single(u"Сквидвард"_q));
	_status = _box->addRow(object_ptr<Ui::FlatLabel>(
		_box,
		u"🟡 Подключаемся к боту…"_q,
		st::boxLabel));
}

void Screen::setupSearch() {
	auto wrapOwned = object_ptr<Ui::RpWidget>(_box);
	const auto wrap = wrapOwned.data();
	_field = Ui::CreateChild<Ui::InputField>(
		wrap,
		st::defaultInputField,
		rpl::single(u"@username, ID или ссылка"_q));
	const auto button = Ui::CreateChild<Ui::RoundButton>(
		wrap,
		rpl::single(u"Пробить"_q),
		st::defaultActiveButton);
	button->setTextTransform(Ui::RoundButtonTextTransform::NoTransform);
	wrap->widthValue(
	) | rpl::on_next([=](int width) {
		const auto skip = st::boxRowPadding.left() / 2;
		_field->resizeToWidth(width - button->width() - skip);
		const auto height = std::max(_field->height(), button->height());
		_field->moveToLeft(0, height - _field->height(), width);
		button->moveToRight(0, height - button->height(), width);
		wrap->resize(width, height);
	}, wrap->lifetime());
	_box->addRow(std::move(wrapOwned));
	button->setClickedCallback([=] { submit(); });
	_field->submits(
	) | rpl::on_next([=](Qt::KeyboardModifiers) {
		submit();
	}, _field->lifetime());
	_box->setFocusCallback([=] { _field->setFocusFast(); });

	_recent = _box->addRow(
		object_ptr<Ui::SlideWrap<Ui::FlatLabel>>(
			_box,
			object_ptr<Ui::FlatLabel>(_box, QString(), st::boxLabel)),
		st::boxRowPadding);
	_recent->entity()->setClickHandlerFilter([=](
			const ClickHandlerPtr &handler,
			Qt::MouseButton) {
		const auto url = handler ? handler->url() : QString();
		const auto prefix = u"squid:"_q;
		if (url.startsWith(prefix)) {
			const auto index = url.mid(prefix.size()).toInt();
			if (index >= 0 && index < _recentList.size()) {
				_field->setText(_recentList[index]);
				submit();
			}
		}
		return false;
	});
}

void Screen::setupEvents() {
	const auto lifetime = &_box->lifetime();
	_session->status(
	) | rpl::on_next([=](BotStatus status) {
		_status->setText(status == BotStatus::Ready
			? u"🟢 Бот @skvidvarde_bot подключён"_q
			: status == BotStatus::Failed
			? u"🔴 Бот недоступен — проверьте сеть и откройте раздел заново"_q
			: u"🟡 Подключаемся к боту…"_q);
		if (status == BotStatus::Ready && !_pending.isEmpty()) {
			start(base::take(_pending));
		}
	}, *lifetime);
	_session->cards(
	) | rpl::on_next([=](const Card &card) {
		renderCard(card);
		if (_autoCollect && card.kind == CardKind::Group) {
			_session->collect();
		}
	}, *lifetime);
	_session->progress(
	) | rpl::on_next([=](const Progress &progress) {
		renderProgress(progress);
	}, *lifetime);
	_session->groupsCollected(
	) | rpl::on_next([=](const GroupsResult &result) {
		_groups = result;
		renderGroups(result);
	}, *lifetime);
	_session->usersCollected(
	) | rpl::on_next([=](const UsersResult &result) {
		_users = result;
		if (std::exchange(_autoCollect, false)
			&& !result.usernames.isEmpty()) {
			saveUsers();
		} else {
			renderUsers(result);
		}
	}, *lifetime);
	_session->errors(
	) | rpl::on_next([=](const QString &text) {
		if (!_current) {
			renderLookupError(text);
		} else {
			_box->uiShow()->showToast(text);
		}
	}, *lifetime);
}

void Screen::submit() {
	const auto query = _field->getLastText().trimmed();
	if (query.isEmpty()) {
		return;
	}
	if (!_session->collecting()) {
		start(query);
		return;
	}
	const auto weak = QPointer<Ui::GenericBox>(_box.get());
	_box->uiShow()->showBox(Ui::MakeConfirmBox({
		.text = u"Идёт сбор списка. Остановить его и пробить новое?"_q,
		.confirmed = [=](Fn<void()> &&close) {
			close();
			if (weak) {
				_session->stop();
				start(query);
			}
		},
		.confirmText = u"Остановить"_q,
	}));
}

void Screen::start(const QString &query) {
	_lastQuery = query;
	_recentList = PushRecent(_recentList, query, kRecentLimit);
	SaveRecent(_recentList);
	renderRecent();
	_current.reset();
	_groups.reset();
	_users.reset();
	_collect->clear();
	if (!_session->ready()) {
		_pending = query;
		renderSearching(u"Подключаемся к боту…"_q);
		return;
	}
	renderSearching(u"Пробиваем "_q + query + u"…"_q);
	_session->lookup(query);
}

void Screen::renderRecent() {
	auto text = TextWithEntities{ u"Недавние: "_q };
	for (auto i = 0; i != int(_recentList.size()); ++i) {
		if (i) {
			text.append(u"  ·  "_q);
		}
		text.append(Ui::Text::Link(
			_recentList[i],
			u"squid:"_q + QString::number(i)));
	}
	_recent->entity()->setMarkedText(text);
	_recent->toggle(!_recentList.isEmpty(), anim::type::instant);
}

void Screen::renderIdle() {
	_card->clear();
	_card->add(object_ptr<Ggtg::EmptyState>(
		_card,
		u"Введите @username, ID или ссылку на группу — "
			"здесь появится досье."_q));
}

void Screen::renderSearching(const QString &text) {
	_card->clear();
	Ui::AddSkip(_card);
	AddLabel(_card, text);
}

void Screen::renderLookupError(const QString &text) {
	_card->clear();
	Ui::AddSkip(_card);
	AddLabel(_card, text);
	const auto query = _lastQuery;
	AddButtons(_card, {
		{ u"Повторить"_q, &st::defaultLightButton, [=] { start(query); } },
	});
}

void Screen::addHeader(
		const QString &title,
		const QString &subtitle,
		const QString &id) {
	const auto row = _card->add(
		object_ptr<Ggtg::InfoRow>(
			_card,
			rpl::single(Ggtg::InfoRowData{
				.title = title,
				.subtitle = subtitle,
				.chip = id.isEmpty() ? QString() : (u"ID "_q + id),
			})),
		st::boxRowPadding);
	if (!id.isEmpty()) {
		row->setClickedCallback([=] {
			QGuiApplication::clipboard()->setText(id);
			_box->uiShow()->showToast(u"ID скопирован"_q);
		});
	}
}

void Screen::renderCard(const Card &card) {
	_current = card;
	_card->clear();
	_collect->clear();
	Ui::AddSkip(_card);
	if (card.kind == CardKind::User) {
		renderUser(card);
	} else if (card.kind == CardKind::Group) {
		renderGroup(card);
	} else {
		AddLabel(_card, u"Бот ответил:"_q);
		AddLabel(_card, card.rawText);
		return;
	}
	AddExpander(_card, u"Исходный ответ бота"_q, card.rawText);
	renderCollectIdle();
}

void Screen::renderUser(const Card &card) {
	const auto &user = *card.user;
	const auto handle = user.usernames.isEmpty()
		? card.query
		: (u"@"_q + user.usernames.front());
	addHeader(
		u"👤 "_q + (user.name.isEmpty() ? handle : user.name),
		handle,
		user.id);
	const auto details = FormatUserDetails(user);
	if (!details.isEmpty()) {
		AddLabel(_card, details.join(u'\n'));
	}
	if (!user.names.empty()) {
		auto lines = QStringList();
		for (const auto &change : user.names) {
			lines.push_back(change.date + u" — "_q + change.name);
		}
		AddExpander(
			_card,
			u"Имена: "_q + QString::number(user.names.size()) + u" изм."_q,
			lines.join(u'\n'));
	}
}

void Screen::renderGroup(const Card &card) {
	const auto &group = *card.group;
	const auto username = UsernameFromQuery(card.query);
	addHeader(
		u"👥 "_q + (group.name.isEmpty() ? card.query : group.name),
		username.isEmpty() ? card.query : (u"@"_q + username),
		group.id);
	const auto details = FormatGroupDetails(group);
	if (!details.isEmpty()) {
		AddLabel(_card, details.join(u'\n'));
	}
	if (!group.description.isEmpty()) {
		AddExpander(_card, u"Описание"_q, group.description);
	}
	if (!group.topDay.empty()) {
		auto lines = QStringList();
		for (auto i = 0; i != int(group.topDay.size()); ++i) {
			lines.push_back(QString::number(i + 1) + u". "_q
				+ group.topDay[i].name + u" — "_q
				+ FormatCount(group.topDay[i].messages));
		}
		AddExpander(_card, u"Топ за сутки"_q, lines.join(u'\n'));
	}
}

void Screen::renderCollectIdle() {
	_collect->clear();
	if (!_current || _current->kind == CardKind::Unknown) {
		return;
	}
	const auto isUser = (_current->kind == CardKind::User);
	Ui::AddSkip(_collect);
	AddButtons(_collect, {
		{
			isUser ? u"👁 Собрать группы"_q : u"👥 Собрать участников"_q,
			&st::defaultActiveButton,
			[=] { _session->collect(); },
		},
	});
}

void Screen::renderProgress(const Progress &progress) {
	_collect->clear();
	const auto isUser = _current && (_current->kind == CardKind::User);
	Ui::AddSkip(_collect);
	const auto bar = _collect->add(
		object_ptr<Ggtg::ProgressBar>(_collect),
		st::boxRowPadding);
	bar->setValue(progress.checking
		? 1.
		: (progress.pages > 0)
		? (double(progress.page) / progress.pages)
		: 0.);
	const auto found = FormatCount(progress.found);
	AddLabel(_collect, progress.checking
		? (isUser
			? u"Отделяем группы от каналов… оставлено "_q + found
			: u"Уточняем участников… найдено "_q + found)
		: (progress.pages > 0)
		? (u"Страница "_q + FormatCount(progress.page) + u" из "_q
			+ FormatCount(progress.pages) + u" · найдено "_q + found)
		: u"Запрашиваем список у бота…"_q);
	if (!progress.checking) {
		AddButtons(_collect, {
			{ u"Стоп"_q, &st::defaultLightButton, [=] { _session->stop(); } },
		});
	}
}

void Screen::renderGroups(const GroupsResult &result) {
	_collect->clear();
	Ui::AddSkip(_collect);
	if (result.groups.empty()) {
		AddLabel(_collect, result.stopped
			? u"Сбор остановлен, групп не собрано."_q
			: u"Групп не найдено."_q);
		AddButtons(_collect, {
			{ u"Собрать заново"_q, &st::defaultLightButton, [=] {
				_session->collect();
			} },
		});
		return;
	}
	auto summary = u"✓ Групп: "_q + FormatCount(result.groups.size());
	if (result.channelsRemoved) {
		summary += u" · каналов убрано: "_q
			+ FormatCount(result.channelsRemoved);
	}
	if (result.closed) {
		summary += u" · 🔒 закрытых: "_q + FormatCount(result.closed);
	}
	if (result.stopped) {
		summary += u" · сбор остановлен"_q;
	}
	AddLabel(_collect, summary);
	auto names = QStringList();
	auto lines = QStringList();
	for (const auto &entry : result.groups) {
		if (names.size() < kPreviewCount) {
			names.push_back(GroupName(entry));
		}
		lines.push_back(GroupLine(entry));
	}
	AddLabel(
		_collect,
		names.join(u" · "_q)
			+ (int(result.groups.size()) > kPreviewCount ? u" …"_q : QString()));
	const auto joined = lines.join(u'\n');
	AddButtons(_collect, {
		{ u"Сохранить в базу"_q, &st::defaultActiveButton, [=] {
			saveGroups();
		} },
		{ u"Копировать"_q, &st::defaultLightButton, [=] {
			QGuiApplication::clipboard()->setText(joined);
			_box->uiShow()->showToast(u"Скопировано"_q);
		} },
	});
}

void Screen::renderUsers(const UsersResult &result) {
	_collect->clear();
	Ui::AddSkip(_collect);
	if (result.usernames.isEmpty()) {
		AddLabel(_collect, result.stopped
			? u"Сбор остановлен, юзернеймов не собрано."_q
			: u"Участников с юзернеймом не найдено."_q);
		AddButtons(_collect, {
			{ u"Собрать заново"_q, &st::defaultLightButton, [=] {
				_session->collect();
			} },
		});
		return;
	}
	AddLabel(
		_collect,
		u"✓ Юзернеймов: "_q + FormatCount(result.usernames.size())
			+ (result.stopped ? u" · сбор остановлен"_q : QString()));
	auto handles = QStringList();
	for (const auto &name : result.usernames) {
		handles.push_back(u"@"_q + name);
	}
	AddLabel(
		_collect,
		handles.mid(0, kPreviewCount).join(u" · "_q)
			+ (handles.size() > kPreviewCount ? u" …"_q : QString()));
	const auto joined = handles.join(u'\n');
	AddButtons(_collect, {
		{ u"Сохранить в базу"_q, &st::defaultActiveButton, [=] {
			saveUsers();
		} },
		{ u"Копировать"_q, &st::defaultLightButton, [=] {
			QGuiApplication::clipboard()->setText(joined);
			_box->uiShow()->showToast(u"Скопировано"_q);
		} },
	});
}

void Screen::saveGroups() {
	if (!_groups || !_current) {
		return;
	}
	const auto id = Data::GenerateManualBaseId();
	Data::SaveScrapedGroupEntries(id, _current->baseTitle, _groups->groups);
	renderSaved(
		id,
		_current->baseTitle,
		int(Data::LoadScrapedGroupEntries(id).size()),
		int(_groups->groups.size()));
}

void Screen::saveUsers() {
	if (!_users || !_current) {
		return;
	}
	const auto id = Data::GenerateManualBaseId();
	Data::SaveScrapedGroupUsernames(
		id,
		_current->baseTitle,
		_users->usernames);
	renderSaved(
		id,
		_current->baseTitle,
		int(Data::LoadScrapedGroupUsernames(id).size()),
		int(_users->usernames.size()));
}

void Screen::renderSaved(
		uint64 id,
		const QString &title,
		int saved,
		int collected) {
	_collect->clear();
	Ui::AddSkip(_collect);
	auto text = u"✓ Сохранено в «"_q + title + u"»: "_q + FormatCount(saved);
	if (saved < collected) {
		text += u" (ещё "_q + FormatCount(collected - saved)
			+ u" уже есть в других базах)"_q;
	}
	AddLabel(_collect, text);
	const auto show = _box->uiShow();
	AddButtons(_collect, {
		{ u"Открыть базу"_q, &st::defaultLightButton, [=] {
			Settings::ShowSavedBase(show, id);
		} },
	});
	show->showToast(text);
}

} // namespace

object_ptr<Ui::BoxContent> MakeSquidwardBox(
		not_null<SessionController*> controller,
		const QString &lookupGroup) {
	return Box([=](not_null<Ui::GenericBox*> box) {
		box->setWidth(st::boxWideWidth);
		box->lifetime().make_state<Screen>(box, controller, lookupGroup);
		box->addButton(rpl::single(u"Закрыть"_q), [=] { box->closeBox(); });
	});
}

} // namespace Window
```

Замечание для исполнителя: в `Screen` поля `_status`, `_field`, `_recent`, `_card`, `_collect` — дочерние виджеты `_box`; `Screen` живёт в `box->lifetime()` и уничтожается вместе с окном, `Session` отменяет запросы через `MTP::Sender`.

- [ ] **Step 4: Описание раздела**

`window/window_main_menu.cpp`, секция `.key = u"squidward"_q`, заменить `.description = …` на:

```cpp
		.description = u"Пробивает человека или группу через бота @skvidvarde_bot и показывает аккуратное досье. По кнопке собирает группы человека или участников группы и сохраняет их в «Базу»."_q,
```

- [ ] **Step 5: Сборка**

Run (из `nicegram-desktop/`, в фоне):
```bash
cmake --build out --config Debug --target Telegram 2>&1 | grep -E "error:|BUILD (SUCCEEDED|FAILED)" | head -40
```
Expected: `** BUILD SUCCEEDED **`. Типичные поправки: недостающий `#include`, другое имя стиля — сверить с `ui/widgets/widgets.style`/`boxes.style` и заменить на существующее, не меняя поведения.

- [ ] **Step 6: Тест разбора**

Run: `Telegram/build/ggtg/test_squidward_parse.sh`
Expected: `All checks passed`

---

### Task 7: Проверка вживую

**Files:** — (только запуск)

- [ ] **Step 1: Запустить приложение**

Run: `open out/Debug/NiceGram.app`
Открыть меню → GGTG → «Сквидвард».

- [ ] **Step 2: Пройти сценарии, отмечая результат**

1. Статус сверху становится «🟢 Бот … подключён».
2. `@Qiuvvinn` + Enter → «Пробиваем…», затем досье 👤 с ID, активностью, стилем, каналом, юзернеймами; «Имена: 3 изм.» раскрывается; «Исходный ответ бота» раскрывается. Клик по строке с ID → «ID скопирован».
3. «👁 Собрать группы» → полоска и «Страница N из 85»; через несколько страниц «Стоп» → итог «сбор остановлен», превью, «Сохранить в базу» → «✓ Сохранено в «…»», «Открыть базу» открывает её; в базе закрытые группы с 🔒.
4. `https://t.me/fambun_chat` → досье 👥 (топ, за сутки, топ-10 свёрнут) → «Собрать участников» → итог с юзернеймами → «Копировать» кладёт список в буфер.
5. Ссылка `https://t.me/c/…` из «Недавних»/базы → пробив идёт по ссылке, база называется именем группы.
6. Во время сбора ввести новый запрос → вопрос «Остановить?»; после «Остановить» старый результат не появляется под новым досье (Review Focus 4).
7. Закрыть окно GGTG посреди сбора → нет падения, нет поздних всплывающих сообщений (Review Focus 5).
8. «Недавние» после перезапуска приложения на месте (не больше 8).
9. «База» → группа → 🦑: раздел открывается поверх, пробив и сбор идут сами, по окончании — тост «✓ Сохранено…» и новая база в «Базе».
10. Отключить сеть и пробить → через 30 с «Бот не отвечает» + «Повторить».

- [ ] **Step 3: Отчитаться**

Записать, какие пункты прошли и какие нет (с текстом ответа бота из «Исходный ответ бота» для непрочитанных карточек — по нему уточняется разбор в Task 3/4 с новым тестовым образцом).
