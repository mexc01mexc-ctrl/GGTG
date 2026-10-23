# Рассылка: понятные исходы и окна - план реализации

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Окно хода рассылки, «Результаты», «Полный отчёт» и форма «Новая» показывают шесть понятных исходов, состояние каждого аккаунта и итог без путаницы.

**Architecture:** Чистая логика (исходы, свёртка строк, порядок аккаунтов, оценка времени, тексты) - в `settings/sms_campaign_model.h`, только QtCore, проверяется отдельным тестом без сборки приложения. Рисование - в `settings/sms_campaign_widgets.h` (данные на вход, без логики рассылки). `sms_campaign.cpp` хранит состояние рассылки в типах модели и отдаёт их виджетам.

**Tech Stack:** C++20, Qt 6.11 (Mac) / Qt 5.15 (Windows), tdesktop UI (`Ui::RpWidget`, `Ui::AbstractButton`, `rpl`), CMake/Xcode.

**Spec:** `docs/superpowers/specs/2026-10-07-ggtg-campaign-clarity-design.md`

## Global Constraints

- Шесть исходов: Отправлено, Отказ Telegram, Закрыт для ЛС, Не найден, Спам-блок аккаунта, Сбой. Названия - только в `sms_campaign_model.h`, цвета и значки - только в `sms_campaign_widgets.h`.
- Цвета: зелёный `QColor(76, 217, 100)`, янтарный `QColor(255, 159, 10)`, красный `QColor(255, 69, 58)`, серый `st::windowSubTextFg`.
- Логику отправки не меняем, кроме разделения исходов. Обхода антиспама Telegram нет.
- В окне хода нет кнопок «Сохранить остаток в базу» и «Открыть аккаунты».
- Код собирается на Qt 5.15 и Qt 6.
- Живым людям при проверках ничего не отправляем: только демо-режим `GGTG_SMS_DEMO`, он удаляется в задаче 9.
- Проект не в git: вместо коммита каждая задача заканчивается зелёной сборкой. DMG - только по просьбе.

## Review Focus

1. Отказы, которые вердикт @spambot потом переписал в спам-блок: строки и счётчики переходят из «Отказ Telegram» в «Спам-блок аккаунта» (тест `TestSettleAndRewrite`, задача 1).
2. Ответ сервера после таймаута: «Сбой» меняется на «Отправлено»/«Отказ», счётчик «Сбой» не уходит в минус (тест `TestCorrectLateOutcome`, задача 1).
3. Старые записи статистики (`spam`, `error`, `flood`, `dead`, `skip`) открываются в «Результатах» и попадают в правильные плитки (тест `TestFromStatsKey`, задача 1; снимок, задача 6).
4. Аккаунт, снятый по квоте или дневному лимиту, получает «Отработал» с причиной, а не висит «В работе» (задача 3, шаг 6; демо, задача 5).
5. Окно хода закрыли и снова открыли во время рассылки: всё строится из состояния заново, `refreshUi` сбрасывается при закрытии (задача 4; проверка в задаче 9, шаг 4).

## Карта файлов

| Файл | Что делает |
|---|---|
| Create `nicegram-desktop/Telegram/SourceFiles/settings/sms_campaign_model.h` | исходы, фильтры, свёртка, аккаунты, оценка времени, тексты |
| Create `nicegram-desktop/Telegram/SourceFiles/settings/sms_campaign_widgets.h` | цвета и значки, полоса, плитки, чипы, статус-карточка, строки аккаунта и получателя, карточка шага |
| Create `ggtg-tests/sms_campaign_model_test.cpp`, `ggtg-tests/run.sh` | тест модели и его сборка |
| Modify `nicegram-desktop/Telegram/CMakeLists.txt:1865` | два новых заголовка |
| Modify `nicegram-desktop/Telegram/SourceFiles/settings/sms_campaign.cpp` | состояние рассылки, окно хода, «Результаты», «Новая», демо |
| Modify `nicegram-desktop/Telegram/SourceFiles/core/ggtg_sms_stats.h` | ключи `peer`/`closed`, группы для «Полного отчёта» |

Сборка приложения (в конце каждой задачи с кодом приложения):

```bash
cd /Users/yansheva/Desktop/ggtg-macos/nicegram-desktop && cmake --build out --config Release --target Telegram 2>&1 | grep -E "error:|BUILD (SUCCEEDED|FAILED)" | head -40
```

Ожидается `** BUILD SUCCEEDED **` и ни одной строки `error:`. После правки `CMakeLists.txt` сначала `cmake -S . -B out >/dev/null`.

---

### Task 1: Модель исходов и её тест

**Files:**
- Create: `nicegram-desktop/Telegram/SourceFiles/settings/sms_campaign_model.h`
- Create: `ggtg-tests/sms_campaign_model_test.cpp`, `ggtg-tests/run.sh`
- Modify: `nicegram-desktop/Telegram/CMakeLists.txt` (после строки `settings/sms_campaign.h`)

**Interfaces:**
- Produces (namespace `Settings::SmsModel`):
  - `enum class Outcome { Sent, Peer, Closed, NotFound, Spam, Fail }`, `kOutcomeCount = 6`
  - `Title/TileTitle/InlineTitle(Outcome)`, `StatsKey(Outcome)`, `FromStatsKey(const QString&) -> std::optional<Outcome>`
  - `enum class Filter { All, Sent, Peer, Closed, Problems }`, `FilterTitle`, `FilterFor`, `Matches`
  - `using Counts = std::array<int, 6>`, `Total`, `CountFor(Counts, Filter)`
  - `struct Recipient { username, account, reason; std::optional<Outcome> outcome; bool inProgress }`
  - `struct DisplayRow { std::vector<int> indices; std::optional<Outcome> outcome; QString account; bool inProgress }`
  - `Fold(recipients, order, Filter, limit) -> std::vector<DisplayRow>` (серии из 3+ одинаковых неуспешных исходов одного аккаунта сворачиваются, только при `Filter::All`)
  - `Settle`, `RewritePeerToSpam`, `CorrectLateOutcome`
  - `enum class AccountState { Working, Waiting, Done, Retired, SpamBlocked }`, `struct Account { label, state, note, sent, refused, failed }`, `StateTitle`, `SortedAccounts`, `AccountsHeadline`, `AccountCounters`
  - `EstimateSeconds(recent, remaining, lanes)`, `DurationText`
  - `enum class Phase { Running, Paused, Stopped, Done, AccountsOut }`, `PhaseTitle`, `FinishLine(Counts, unprocessed)`, `OftenRefused(refused, attempts)`, `SkipBreakdown(...)`

- [ ] **Step 1: Скрипт сборки теста**

Create `ggtg-tests/run.sh`:

```bash
#!/bin/bash
# Builds and runs the QtCore-only tests of GGTG's pure logic without the app.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
Q="$ROOT/Libraries/local/Qt-6.11.1"
SRC="$ROOT/nicegram-desktop/Telegram/SourceFiles"
OUT="${TMPDIR:-/tmp}/ggtg-tests"
mkdir -p "$OUT"
for test in "$ROOT"/ggtg-tests/*_test.cpp; do
	name="$(basename "$test" .cpp)"
	clang++ -std=c++20 -DGGTG_MODEL_STANDALONE \
		-I"$Q/include" -I"$Q/include/QtCore" -I"$SRC" \
		"$test" "$Q/lib/libQt6Core.a" "$Q/lib/libQt6BundledPcre2.a" -lz \
		-framework IOKit -framework CoreFoundation -framework Foundation \
		-framework DiskArbitration -framework AppKit \
		-framework ApplicationServices -framework CoreServices \
		-framework Security -framework UniformTypeIdentifiers \
		-o "$OUT/$name"
	"$OUT/$name"
done
```

Run: `chmod +x /Users/yansheva/Desktop/ggtg-macos/ggtg-tests/run.sh`

- [ ] **Step 2: Написать падающий тест**

Create `ggtg-tests/sms_campaign_model_test.cpp`:

```cpp
// Tests of settings/sms_campaign_model.h - run with ggtg-tests/run.sh.
#include "settings/sms_campaign_model.h"

#include <cstdio>
#include <cstdlib>

namespace {

int failures = 0;

void Check(bool ok, const char *what, int line) {
	if (!ok) {
		std::fprintf(stderr, "FAIL line %d: %s\n", line, what);
		++failures;
	}
}
#define CHECK(x) Check((x), #x, __LINE__)

void CheckEq(const QString &a, const QString &b, int line) {
	if (a != b) {
		std::fprintf(stderr, "FAIL line %d:\n  got:  %s\n  want: %s\n",
			line, a.toUtf8().constData(), b.toUtf8().constData());
		++failures;
	}
}
#define CHECK_EQ(a, b) CheckEq((a), (b), __LINE__)

using namespace Settings::SmsModel;

void TestFromStatsKey() {
	CHECK(FromStatsKey(u"sent"_q) == Outcome::Sent);
	CHECK(FromStatsKey(u"peer"_q) == Outcome::Peer);
	CHECK(FromStatsKey(u"closed"_q) == Outcome::Closed);
	CHECK(FromStatsKey(u"notfound"_q) == Outcome::NotFound);
	CHECK(FromStatsKey(u"spam"_q) == Outcome::Spam); // old merged records
	CHECK(FromStatsKey(u"error"_q) == Outcome::Fail);
	CHECK(FromStatsKey(u"flood"_q) == Outcome::Fail);
	CHECK(FromStatsKey(u"dead"_q) == Outcome::Fail);
	CHECK(!FromStatsKey(u"skip"_q).has_value());
	CHECK(!FromStatsKey(u"garbage"_q).has_value());
	for (auto i = 0; i != kOutcomeCount; ++i) {
		CHECK(FromStatsKey(StatsKey(Outcome(i))) == Outcome(i));
	}
}

void TestFilters() {
	CHECK(Matches(Filter::All, Outcome::Fail));
	CHECK(Matches(Filter::Problems, Outcome::NotFound));
	CHECK(Matches(Filter::Problems, Outcome::Spam));
	CHECK(!Matches(Filter::Problems, Outcome::Peer));
	CHECK(FilterFor(Outcome::Spam) == Filter::Problems);
	const auto counts = Counts{ 5, 3, 2, 1, 1, 4 };
	CHECK(CountFor(counts, Filter::All) == 16);
	CHECK(CountFor(counts, Filter::Problems) == 6);
	CHECK(Total(counts) == 16);
}

std::vector<Recipient> Sample() {
	auto list = std::vector<Recipient>(7);
	const auto set = [&](int i, Outcome o, const QString &account) {
		list[i].username = u"u"_q + QString::number(i);
		list[i].account = account;
		list[i].outcome = o;
	};
	set(0, Outcome::Sent, u"Maria"_q);
	set(1, Outcome::Peer, u"Daniel"_q);
	set(2, Outcome::Peer, u"Daniel"_q);
	set(3, Outcome::Peer, u"Daniel"_q);
	set(4, Outcome::Peer, u"Maria"_q);
	set(5, Outcome::Closed, u"Maria"_q);
	list[6].username = u"u6"_q;
	list[6].account = u"Daniel"_q;
	list[6].inProgress = true;
	return list;
}

void TestFold() {
	const auto list = Sample();
	const auto order = std::vector<int>{ 6, 5, 4, 3, 2, 1, 0 };
	const auto rows = Fold(list, order, Filter::All, 200);
	CHECK(rows.size() == 5); // u6, u5, u4, [u3 u2 u1], u0
	CHECK(rows[0].inProgress);
	CHECK(rows[3].indices.size() == 3);
	CHECK(rows[3].outcome == Outcome::Peer);
	CHECK_EQ(rows[3].account, u"Daniel"_q);
	const auto peers = Fold(list, order, Filter::Peer, 200);
	CHECK(peers.size() == 4); // a filter never folds
	for (const auto &row : peers) {
		CHECK(row.indices.size() == 1);
	}
	auto sent = std::vector<Recipient>(3);
	for (auto &r : sent) {
		r.account = u"A"_q;
		r.outcome = Outcome::Sent;
	}
	CHECK(Fold(sent, { 2, 1, 0 }, Filter::All, 200).size() == 3);
	auto two = std::vector<Recipient>(2);
	for (auto &r : two) {
		r.account = u"A"_q;
		r.outcome = Outcome::Fail;
	}
	CHECK(Fold(two, { 1, 0 }, Filter::All, 200).size() == 2);
	CHECK(Fold(list, order, Filter::All, 4).size() == 4);
}

void TestSettleAndRewrite() {
	auto list = Sample();
	auto counts = Counts{};
	Settle(list, counts, 6, Outcome::Peer, QString());
	CHECK(!list[6].inProgress);
	CHECK(list[6].outcome == Outcome::Peer);
	CHECK(counts[int(Outcome::Peer)] == 1);
	RewritePeerToSpam(list, counts, { 6, 0 }); // 0 is Sent: left alone
	CHECK(list[6].outcome == Outcome::Spam);
	CHECK(list[0].outcome == Outcome::Sent);
	CHECK(counts[int(Outcome::Peer)] == 0);
	CHECK(counts[int(Outcome::Spam)] == 1);
}

void TestCorrectLateOutcome() {
	auto list = std::vector<Recipient>(1);
	auto counts = Counts{};
	Settle(list, counts, 0, Outcome::Fail, u"таймаут"_q);
	CorrectLateOutcome(list, counts, 0, Outcome::Sent);
	CHECK(list[0].outcome == Outcome::Sent);
	CHECK(list[0].reason.isEmpty());
	CHECK(counts[int(Outcome::Fail)] == 0);
	CHECK(counts[int(Outcome::Sent)] == 1);
	auto fresh = std::vector<Recipient>(1);
	auto zero = Counts{};
	CorrectLateOutcome(fresh, zero, 0, Outcome::Peer);
	CHECK(zero[int(Outcome::Fail)] == 0); // never negative
	CHECK(zero[int(Outcome::Peer)] == 1);
}

void TestAccounts() {
	auto list = std::vector<Account>{
		{ .label = u"Wait"_q, .state = AccountState::Waiting },
		{ .label = u"Done"_q, .state = AccountState::Done },
		{ .label = u"Work"_q, .state = AccountState::Working },
		{ .label = u"Ret"_q, .state = AccountState::Retired },
		{ .label = u"Spam"_q, .state = AccountState::SpamBlocked },
		{ .label = u"Work2"_q, .state = AccountState::Working },
	};
	CHECK(SortedAccounts(list) == (std::vector<int>{ 2, 5, 3, 4, 1, 0 }));
	CHECK_EQ(
		AccountsHeadline(list),
		u"Аккаунты · 6 выбрано · 2 в работе · 1 отработал · 2 сняты"
		" · 1 ждёт"_q);
	CHECK_EQ(
		AccountsHeadline(std::vector<Account>(3)),
		u"Аккаунты · 3 выбрано · 3 ждут"_q);
	CHECK_EQ(
		AccountCounters({ .sent = 12, .refused = 5 }),
		u"✓ 12 · отказ 5"_q);
	CHECK_EQ(
		AccountCounters({ .sent = 0, .refused = 0, .failed = 2 }),
		u"✓ 0 · сбой 2"_q);
}

void TestEstimate() {
	CHECK(EstimateSeconds({ 40, 50 }, 100, 1) == -1);
	CHECK(EstimateSeconds({ 40, 50, 60 }, 100, 2) == 2500);
	CHECK(EstimateSeconds({ 40, 50, 60 }, 0, 2) == 0);
	CHECK_EQ(DurationText(30), u"< 1 мин"_q);
	CHECK_EQ(DurationText(2400), u"≈ 40 мин"_q);
	CHECK_EQ(DurationText(4800), u"≈ 1 ч 20 мин"_q);
	CHECK_EQ(DurationText(7200), u"≈ 2 ч"_q);
}

void TestTexts() {
	CHECK_EQ(PhaseTitle(Phase::AccountsOut), u"Аккаунты закончились"_q);
	CHECK_EQ(
		FinishLine(Counts{ 312, 41, 0, 0, 2, 0 }, 160),
		u"Отправлено 312 · отказ Telegram 41 · спам-блок аккаунта 2"
		" · ждут следующей рассылки 160 (остались в базе)"_q);
	CHECK_EQ(FinishLine(Counts{}, 0), u"Отправлено 0"_q);
	CHECK(OftenRefused(3, 6));
	CHECK(!OftenRefused(2, 6));
	CHECK(!OftenRefused(5, 5)); // too few attempts to judge
	CHECK_EQ(
		SkipBreakdown(80, 20, 0, 3, 0, 1),
		u"уже писали 80 · отказ Telegram 20 · не найдены 3 · боты 1"_q);
	CHECK(SkipBreakdown(0, 0, 0, 0, 0, 0).isEmpty());
}

} // namespace

int main() {
	TestFromStatsKey();
	TestFilters();
	TestFold();
	TestSettleAndRewrite();
	TestCorrectLateOutcome();
	TestAccounts();
	TestEstimate();
	TestTexts();
	if (failures) {
		std::fprintf(stderr, "%d check(s) failed\n", failures);
		return EXIT_FAILURE;
	}
	std::puts("sms_campaign_model_test: OK");
	return EXIT_SUCCESS;
}
```

- [ ] **Step 3: Убедиться, что падает**

Run: `/Users/yansheva/Desktop/ggtg-macos/ggtg-tests/run.sh`
Expected: ошибка компиляции `'settings/sms_campaign_model.h' file not found`.

- [ ] **Step 4: Написать модель**

Create `nicegram-desktop/Telegram/SourceFiles/settings/sms_campaign_model.h`:

```cpp
/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and exceptions, see https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

// What a mailing's numbers mean, apart from how they are drawn: the six
// outcomes, folding of the recipient list, the accounts' order and the texts
// of the summary. QtCore only, so ggtg-tests/ can check it without the app.

#include <QtCore/QString>
#include <QtCore/QStringList>

#include <algorithm>
#include <array>
#include <optional>
#include <vector>

#ifdef GGTG_MODEL_STANDALONE
[[nodiscard]] inline QString operator""_q(const char16_t *data, size_t size) {
	return QString::fromRawData(
		reinterpret_cast<const QChar*>(data),
		qsizetype(size));
}
#else // GGTG_MODEL_STANDALONE
#include "base/basic_types.h"
#endif // GGTG_MODEL_STANDALONE

namespace Settings::SmsModel {

enum class Outcome {
	Sent, // the message went out
	Peer, // Telegram refused this person, the account is free (PEER_FLOOD)
	Closed, // privacy, blocked us, deleted: nobody can write there
	NotFound, // no such username
	Spam, // the account itself is under a spam block (@spambot)
	Fail, // network, timeout, FLOOD_WAIT, a dead session, unknown
};
inline constexpr auto kOutcomeCount = 6;

[[nodiscard]] inline QString Title(Outcome outcome) {
	switch (outcome) {
	case Outcome::Sent: return u"Отправлено"_q;
	case Outcome::Peer: return u"Отказ Telegram"_q;
	case Outcome::Closed: return u"Закрыт для ЛС"_q;
	case Outcome::NotFound: return u"Не найден"_q;
	case Outcome::Spam: return u"Спам-блок аккаунта"_q;
	case Outcome::Fail: return u"Сбой"_q;
	}
	return QString();
}

// Under a number on a tile.
[[nodiscard]] inline QString TileTitle(Outcome outcome) {
	switch (outcome) {
	case Outcome::Sent: return u"Отправлено"_q;
	case Outcome::Peer: return u"Отказ Telegram"_q;
	case Outcome::Closed: return u"Закрыты для ЛС"_q;
	case Outcome::NotFound: return u"Не найдены"_q;
	case Outcome::Spam: return u"Спам-блок акк."_q;
	case Outcome::Fail: return u"Сбой"_q;
	}
	return QString();
}

// Lowercase, for running text ("отказ Telegram 41").
[[nodiscard]] inline QString InlineTitle(Outcome outcome) {
	switch (outcome) {
	case Outcome::Sent: return u"отправлено"_q;
	case Outcome::Peer: return u"отказ Telegram"_q;
	case Outcome::Closed: return u"закрыты для ЛС"_q;
	case Outcome::NotFound: return u"не найдены"_q;
	case Outcome::Spam: return u"спам-блок аккаунта"_q;
	case Outcome::Fail: return u"сбой"_q;
	}
	return QString();
}

// The key in Core::GgtgSmsStats::Attempt::outcome.
[[nodiscard]] inline QString StatsKey(Outcome outcome) {
	switch (outcome) {
	case Outcome::Sent: return u"sent"_q;
	case Outcome::Peer: return u"peer"_q;
	case Outcome::Closed: return u"closed"_q;
	case Outcome::NotFound: return u"notfound"_q;
	case Outcome::Spam: return u"spam"_q;
	case Outcome::Fail: return u"error"_q;
	}
	return QString();
}

// Records written before "peer" and "closed" existed: "spam" was both a
// refusal and a real spam block and stays Spam; "error" held the closed ones
// too and stays Fail. "skip" was never a real attempt.
[[nodiscard]] inline std::optional<Outcome> FromStatsKey(const QString &key) {
	if (key == u"sent"_q) {
		return Outcome::Sent;
	} else if (key == u"peer"_q) {
		return Outcome::Peer;
	} else if (key == u"closed"_q) {
		return Outcome::Closed;
	} else if (key == u"notfound"_q) {
		return Outcome::NotFound;
	} else if (key == u"spam"_q) {
		return Outcome::Spam;
	} else if (key == u"error"_q || key == u"flood"_q || key == u"dead"_q) {
		return Outcome::Fail;
	}
	return std::nullopt;
}

enum class Filter {
	All,
	Sent,
	Peer,
	Closed,
	Problems, // not found, spam block, failure
};

[[nodiscard]] inline QString FilterTitle(Filter filter) {
	switch (filter) {
	case Filter::All: return u"Все"_q;
	case Filter::Sent: return u"Отправлено"_q;
	case Filter::Peer: return u"Отказ Telegram"_q;
	case Filter::Closed: return u"Закрыты"_q;
	case Filter::Problems: return u"Проблемы"_q;
	}
	return QString();
}

[[nodiscard]] inline Filter FilterFor(Outcome outcome) {
	switch (outcome) {
	case Outcome::Sent: return Filter::Sent;
	case Outcome::Peer: return Filter::Peer;
	case Outcome::Closed: return Filter::Closed;
	case Outcome::NotFound:
	case Outcome::Spam:
	case Outcome::Fail: return Filter::Problems;
	}
	return Filter::All;
}

[[nodiscard]] inline bool Matches(Filter filter, Outcome outcome) {
	return (filter == Filter::All) || (FilterFor(outcome) == filter);
}

using Counts = std::array<int, kOutcomeCount>;

[[nodiscard]] inline int Total(const Counts &counts) {
	auto result = 0;
	for (const auto value : counts) {
		result += value;
	}
	return result;
}

[[nodiscard]] inline int CountFor(const Counts &counts, Filter filter) {
	auto result = 0;
	for (auto i = 0; i != kOutcomeCount; ++i) {
		if (Matches(filter, Outcome(i))) {
			result += counts[i];
		}
	}
	return result;
}

struct Recipient {
	QString username;
	QString account; // who handled it, empty until someone did
	QString reason; // short, for Closed / NotFound / Fail
	std::optional<Outcome> outcome; // empty while queued or in progress
	bool inProgress = false;
};

// One line of the recipient list: a single recipient, or several equal
// failures of one account in a row folded together.
struct DisplayRow {
	std::vector<int> indices;
	std::optional<Outcome> outcome;
	QString account;
	bool inProgress = false;
};

inline constexpr auto kFoldMin = 3;

// `order` lists recipient indices newest first. With Filter::All, runs of at
// least kFoldMin settled recipients with one non-Sent outcome and one account
// become a single row; a filter shows every recipient on its own line. At
// most `limit` rows come back.
[[nodiscard]] inline std::vector<DisplayRow> Fold(
		const std::vector<Recipient> &recipients,
		const std::vector<int> &order,
		Filter filter,
		int limit) {
	auto rows = std::vector<DisplayRow>();
	auto run = std::vector<int>();
	const auto single = [&](int index) {
		const auto &r = recipients[index];
		rows.push_back({
			.indices = { index },
			.outcome = r.outcome,
			.account = r.account,
		});
	};
	const auto flush = [&] {
		if (int(run.size()) >= kFoldMin) {
			const auto &first = recipients[run.front()];
			rows.push_back({
				.indices = run,
				.outcome = first.outcome,
				.account = first.account,
			});
		} else {
			for (const auto index : run) {
				single(index);
			}
		}
		run.clear();
	};
	for (const auto index : order) {
		const auto &r = recipients[index];
		if (r.inProgress) {
			flush();
			if (filter == Filter::All) {
				rows.push_back({
					.indices = { index },
					.account = r.account,
					.inProgress = true,
				});
			}
			continue;
		} else if (!r.outcome || !Matches(filter, *r.outcome)) {
			continue;
		}
		const auto foldable = (filter == Filter::All)
			&& (*r.outcome != Outcome::Sent);
		if (!foldable) {
			flush();
			single(index);
			continue;
		}
		if (!run.empty()) {
			const auto &last = recipients[run.back()];
			if (last.outcome != r.outcome || last.account != r.account) {
				flush();
			}
		}
		run.push_back(index);
	}
	flush();
	if (int(rows.size()) > limit) {
		rows.resize(limit);
	}
	return rows;
}

inline void Settle(
		std::vector<Recipient> &recipients,
		Counts &counts,
		int index,
		Outcome outcome,
		const QString &reason) {
	auto &r = recipients[index];
	r.inProgress = false;
	r.outcome = outcome;
	r.reason = reason;
	++counts[int(outcome)];
}

// @spambot answered "blocked" after the refusals were already shown: they
// were the account's own block, not the recipients'.
inline void RewritePeerToSpam(
		std::vector<Recipient> &recipients,
		Counts &counts,
		const std::vector<int> &indices) {
	for (const auto index : indices) {
		auto &r = recipients[index];
		if (r.outcome == Outcome::Peer) {
			r.outcome = Outcome::Spam;
			--counts[int(Outcome::Peer)];
			++counts[int(Outcome::Spam)];
		}
	}
}

// The server answered after the step had already timed out as a failure.
inline void CorrectLateOutcome(
		std::vector<Recipient> &recipients,
		Counts &counts,
		int index,
		Outcome outcome) {
	auto &r = recipients[index];
	if (r.outcome == Outcome::Fail && counts[int(Outcome::Fail)] > 0) {
		--counts[int(Outcome::Fail)];
	}
	r.inProgress = false;
	r.outcome = outcome;
	r.reason = QString();
	++counts[int(outcome)];
}

enum class AccountState {
	Working,
	Waiting,
	Done, // did its share (quota, daily limit, recipients ran out)
	Retired, // taken out: refusals / failures / FLOOD_WAIT in a row
	SpamBlocked,
};

[[nodiscard]] inline QString StateTitle(AccountState state) {
	switch (state) {
	case AccountState::Working: return u"В работе"_q;
	case AccountState::Waiting: return u"Ждёт"_q;
	case AccountState::Done: return u"Отработал"_q;
	case AccountState::Retired: return u"Снят"_q;
	case AccountState::SpamBlocked: return u"Спам-блок"_q;
	}
	return QString();
}

struct Account {
	QString label;
	AccountState state = AccountState::Waiting;
	QString note; // why it was taken out, or what finished it
	int sent = 0;
	int refused = 0; // Peer
	int failed = 0; // Fail
};

// Working first, then the ones taken out (the reason matters), then those
// that did their share, then the ones still waiting. Stable inside a group.
[[nodiscard]] inline std::vector<int> SortedAccounts(
		const std::vector<Account> &accounts) {
	const auto rank = [](AccountState state) {
		switch (state) {
		case AccountState::Working: return 0;
		case AccountState::Retired:
		case AccountState::SpamBlocked: return 1;
		case AccountState::Done: return 2;
		case AccountState::Waiting: return 3;
		}
		return 3;
	};
	auto result = std::vector<int>(accounts.size());
	for (auto i = 0; i != int(accounts.size()); ++i) {
		result[i] = i;
	}
	std::stable_sort(result.begin(), result.end(), [&](int a, int b) {
		return rank(accounts[a].state) < rank(accounts[b].state);
	});
	return result;
}

// 1, 21, 31... take the singular form ("отработал"), 11 does not.
[[nodiscard]] inline bool Singular(int n) {
	return (n % 10 == 1) && (n % 100 != 11);
}

[[nodiscard]] inline QString AccountsHeadline(
		const std::vector<Account> &accounts) {
	auto working = 0;
	auto done = 0;
	auto out = 0;
	auto waiting = 0;
	for (const auto &a : accounts) {
		switch (a.state) {
		case AccountState::Working: ++working; break;
		case AccountState::Done: ++done; break;
		case AccountState::Retired:
		case AccountState::SpamBlocked: ++out; break;
		case AccountState::Waiting: ++waiting; break;
		}
	}
	auto parts = QStringList{
		u"Аккаунты"_q,
		QString::number(accounts.size()) + u" выбрано"_q,
	};
	if (working) {
		parts.push_back(QString::number(working) + u" в работе"_q);
	}
	if (done) {
		parts.push_back(QString::number(done)
			+ (Singular(done) ? u" отработал"_q : u" отработали"_q));
	}
	if (out) {
		parts.push_back(QString::number(out)
			+ (Singular(out) ? u" снят"_q : u" сняты"_q));
	}
	if (waiting) {
		parts.push_back(QString::number(waiting)
			+ (Singular(waiting) ? u" ждёт"_q : u" ждут"_q));
	}
	return parts.join(u" · "_q);
}

[[nodiscard]] inline QString AccountCounters(const Account &account) {
	auto result = u"✓ "_q + QString::number(account.sent);
	if (account.refused) {
		result += u" · отказ "_q + QString::number(account.refused);
	}
	if (account.failed) {
		result += u" · сбой "_q + QString::number(account.failed);
	}
	return result;
}

// `recent`: seconds one lane spent per recipient lately, pauses included.
// -1 while there are too few of them to say anything.
[[nodiscard]] inline int EstimateSeconds(
		const std::vector<int> &recent,
		int remaining,
		int lanes) {
	if (recent.size() < 3) {
		return -1;
	} else if (remaining <= 0) {
		return 0;
	}
	auto sum = 0LL;
	for (const auto value : recent) {
		sum += value;
	}
	const auto average = double(sum) / recent.size();
	return int(average * remaining / std::max(lanes, 1) + 0.5);
}

[[nodiscard]] inline QString DurationText(int seconds) {
	const auto minutes = (seconds + 30) / 60;
	if (minutes < 1) {
		return u"< 1 мин"_q;
	} else if (minutes < 60) {
		return u"≈ "_q + QString::number(minutes) + u" мин"_q;
	}
	const auto hours = minutes / 60;
	const auto rest = minutes % 60;
	return u"≈ "_q + QString::number(hours) + u" ч"_q
		+ (rest ? (u" "_q + QString::number(rest) + u" мин"_q) : QString());
}

enum class Phase {
	Running,
	Paused,
	Stopped, // "Стоп" was pressed
	Done, // everyone was dealt with
	AccountsOut, // every account was used up before the list was
};

[[nodiscard]] inline QString PhaseTitle(Phase phase) {
	switch (phase) {
	case Phase::Running: return u"Идёт рассылка"_q;
	case Phase::Paused: return u"Пауза"_q;
	case Phase::Stopped: return u"Остановлено вручную"_q;
	case Phase::Done: return u"Готово"_q;
	case Phase::AccountsOut: return u"Аккаунты закончились"_q;
	}
	return QString();
}

// "Отправлено 312 · отказ Telegram 41 · ... · ждут следующей рассылки 160
// (остались в базе)": zeros left out, except the sent count.
[[nodiscard]] inline QString FinishLine(
		const Counts &counts,
		int unprocessed) {
	auto parts = QStringList{
		Title(Outcome::Sent) + ' ' + QString::number(counts[0]),
	};
	for (auto i = 1; i != kOutcomeCount; ++i) {
		if (counts[i]) {
			parts.push_back(
				InlineTitle(Outcome(i)) + ' ' + QString::number(counts[i]));
		}
	}
	if (unprocessed > 0) {
		parts.push_back(u"ждут следующей рассылки "_q
			+ QString::number(unprocessed)
			+ u" (остались в базе)"_q);
	}
	return parts.join(u" · "_q);
}

// An account Telegram turns away at least every second time, once there are
// enough attempts to say so.
[[nodiscard]] inline bool OftenRefused(int refused, int attempts) {
	return attempts >= 6 && refused * 2 >= attempts;
}

// Why the "Новая" form will not write to some of the chosen recipients.
[[nodiscard]] inline QString SkipBreakdown(
		int sent,
		int peer,
		int closed,
		int notFound,
		int spam,
		int bots) {
	auto parts = QStringList();
	const auto add = [&](int value, const QString &title) {
		if (value) {
			parts.push_back(title + ' ' + QString::number(value));
		}
	};
	add(sent, u"уже писали"_q);
	add(peer, InlineTitle(Outcome::Peer));
	add(closed, InlineTitle(Outcome::Closed));
	add(notFound, InlineTitle(Outcome::NotFound));
	add(spam, InlineTitle(Outcome::Spam));
	add(bots, u"боты"_q);
	return parts.join(u" · "_q);
}

} // namespace Settings::SmsModel
```

- [ ] **Step 5: Запустить тест**

Run: `/Users/yansheva/Desktop/ggtg-macos/ggtg-tests/run.sh`
Expected: `sms_campaign_model_test: OK`

- [ ] **Step 6: Подключить заголовок**

In `nicegram-desktop/Telegram/CMakeLists.txt`, after the line `    settings/sms_campaign.h` add:

```cmake
    settings/sms_campaign_model.h
```

- [ ] **Step 7: Сборка зелёная**

Run: `cd /Users/yansheva/Desktop/ggtg-macos/nicegram-desktop && cmake -S . -B out >/dev/null && cmake --build out --config Release --target Telegram 2>&1 | grep -E "error:|BUILD (SUCCEEDED|FAILED)" | head -40`
Expected: `** BUILD SUCCEEDED **` (заголовок пока нигде не включён, поэтому цель - только проверить, что CMake его видит).

---

### Task 2: Виджеты рассылки

**Files:**
- Create: `nicegram-desktop/Telegram/SourceFiles/settings/sms_campaign_widgets.h`
- Modify: `nicegram-desktop/Telegram/CMakeLists.txt` (после `settings/sms_campaign_model.h`)
- Modify: `nicegram-desktop/Telegram/SourceFiles/settings/sms_campaign.cpp` (include)

**Interfaces:**
- Consumes: `Settings::SmsModel` (задача 1); `Ggtg::WrapText` из `window/ggtg_widgets.h`.
- Produces (namespace `Settings::SmsWidgets`):
  - `QColor OutcomeColor(SmsModel::Outcome)`, `QColor Tinted(QColor, double alpha)`
  - `void PaintOutcomeIcon(QPainter&, QRect, std::optional<SmsModel::Outcome>)` (nullopt - «в работе»)
  - `class SegmentBar : Ui::RpWidget` - `setCounts(const Counts&, int goal)`
  - `class OutcomeTiles : Ui::RpWidget` - ctor `(parent, bool clickable)`, `setCounts`, `setFilter(Filter)`, `filterRequests() -> rpl::producer<Filter>`
  - `class FilterChips : Ui::RpWidget` - `setCounts`, `setFilter`, `filterRequests()`
  - `struct StatusData { QString title, subtitle; int processed, goal; Counts counts; QString finishLine, earlierLine; }`, `class StatusCard` - `setData(StatusData)`
  - `class AccountRow : Ui::RpWidget` - `setData(const Account&, const QString &live)`
  - `class RecipientRow : Ui::AbstractButton` - `setData(const DisplayRow&, const std::vector<Recipient>&, bool expanded)`
  - `class StepCard : Ui::RpWidget` - ctor `(parent, QString title, rpl::producer<QString> summary, bool expanded)`, `content()`, `expandedValue()`

- [ ] **Step 1: Написать файл**

Шапка `settings/sms_campaign_widgets.h`: `#pragma once`, комментарий «How a mailing looks … Drawing only», includes `settings/sms_campaign_model.h`, `window/ggtg_widgets.h`, `ui/abstract_button.h`, `ui/rp_widget.h`, `ui/painter.h`, `ui/wrap/vertical_layout.h`, `ui/wrap/slide_wrap.h`, `styles/style_boxes.h`, `styles/style_layers.h`, `styles/style_widgets.h`, `<QtGui/QMouseEvent>`, `<QtGui/QPainterPath>`; `namespace Settings::SmsWidgets { using SmsModel::Outcome;`.

Содержимое (каждый пункт - отдельная функция или класс):

1. `OutcomeColor`: `Sent` → `QColor(76, 217, 100)`; `Peer` → `QColor(255, 159, 10)`; `Closed`, `NotFound` → `st::windowSubTextFg->c`; `Spam`, `Fail` → `QColor(255, 69, 58)`. `Tinted` - копия цвета с `setAlphaF(alpha)`.
2. `PaintOutcomeIcon`: линейные значки пером `ConvertScale(1.6)` с круглыми концами в `rect.adjusted(2, 2, -2, -2)`. `Sent` - галочка (0.15,0.55 → 0.42,0.8 → 0.88,0.22); `Peer` - круг с чертой; `Closed` - скруглённый прямоугольник с дужкой сверху (`drawArc`, 0..180°); `NotFound` - круг с «?»; `Spam` - щит (`QPainterPath` из шести точек); `Fail` - треугольник с чёрточкой и точкой; `nullopt` - дуга `drawArc(r, 90*16, -270*16)` цветом `st::windowActiveTextFg`.
3. `PaintCardBackground(p, rect)`: радиус `ConvertScale(10)`, заливка `Tinted(st::windowBoldFg->c, 0.06)`.
4. `PaintSegments(p, QRectF, counts, goal)`: дорожка `Tinted(windowBoldFg, 0.12)`, поверх сегменты по исходам слева направо шириной `rect.width() * counts[i] / max(goal, Total, 1)`, обрезка скруглением через `setClipPath`.
5. `SegmentBar`: высота `ConvertScale(6)`, `paintEvent` зовёт `PaintSegments`.
6. `OutcomeTiles`: 6 плиток, 3 колонки при ширине < `ConvertScale(420)`, иначе 6. Плитка - скруглённый прямоугольник `Tinted(windowBoldFg, 0.05)` (выбранная - 0.12 и рамка цвета исхода); число шрифтом `st::boxTitle.style.font` цветом исхода (серым при 0), подпись `TileTitle` шрифтом `st::normalFont` цветом `windowSubTextFg`. При `clickable` курсор-рука; `mouseReleaseEvent` шлёт `FilterFor(outcome)` или `Filter::All`, если этот фильтр уже выбран. Высота пересчитывается при смене числа колонок.
7. `FilterChips`: «Все N», «Отправлено N», «Отказ Telegram N», «Закрыты N», «Проблемы N» (нулевые, кроме «Все», пропускаются). Таблетки высотой `semiboldFont->height + ConvertScale(8)`, перенос по ширине; выбранный - `Tinted(windowActiveTextFg, 0.18)` и `semiboldFont`, остальные - рамка `Tinted(windowBoldFg, 0.18)`. `mouseReleaseEvent` шлёт фильтр чипа.
8. `StatusCard`: фон `PaintCardBackground`; слева `title` (`semiboldFont`), справа «processed / goal» шрифтом `boxTitle`; ниже `finishLine` (если пуста - `subtitle`) и `earlierLine`, перенесённые `Ggtg::WrapText(st::normalFont, text, inner)`, цвет `windowSubTextFg`; внизу `PaintSegments` высотой `ConvertScale(6)`. Высота пересчитывается в `refit()` при смене ширины и данных.
9. `AccountRow`: высота `ConvertScale(44)`; слева таблетка состояния шириной не меньше `ConvertScale(84)` (`Working` - `windowActiveTextFg`, `Done` - зелёный, `Retired` - янтарный, `SpamBlocked` - красный, `Waiting` - серый; заливка `Tinted(color, 0.16)`); справа `AccountCounters`; посередине имя (`semiboldFont`) и под ним одна строка `live` (если не пуста) или `note`, обе через `elided`.
10. `RecipientRow`: высота `ConvertScale(34)`. Одиночная строка - значок (`PaintOutcomeIcon`, 16 px), «@username», справа серым причина (или «в работе») и аккаунт через « · ». Свёрнутая серия (`indices.size() > 1`) - фон `Tinted(OutcomeColor, 0.1)`, `semiboldFont` цветом исхода «<Title> × N подряд», справа аккаунт и ▾/▴, курсор-рука. Клик обрабатывает владелец через `setClickedCallback`.
11. `StepCard`: `object_ptr<Ui::VerticalLayout> _layout` из заголовка-кнопки `Header` (высота `ConvertScale(52)`: название `semiboldFont`, под ним итог `normalFont` серым, справа ▾/▴) и `Ui::SlideWrap<Ui::VerticalLayout>` с телом. Клик по заголовку переключает `_expanded` (`rpl::variable<bool>`) и `_body->toggle(...)`. Ширина идёт в `_layout->resizeToWidth`, высота карточки следует за `_layout->heightValue()`.

Тела классов пишутся по образцу `Ggtg::InfoRow` и `Ggtg::EmptyState` из `window/ggtg_widgets.h:251-430`: `paintEvent` с `PainterHighQualityEnabler`, `widthValue() | rpl::on_next` для пересчёта высоты.

- [ ] **Step 2: Подключить**

В `CMakeLists.txt` после `settings/sms_campaign_model.h` добавить `settings/sms_campaign_widgets.h`. В `sms_campaign.cpp` после `#include "settings/sms_campaign.h"`:

```cpp
#include "settings/sms_campaign_model.h"
#include "settings/sms_campaign_widgets.h"
```

- [ ] **Step 3: Сборка зелёная**

Run: `cd /Users/yansheva/Desktop/ggtg-macos/nicegram-desktop && cmake -S . -B out >/dev/null && cmake --build out --config Release --target Telegram 2>&1 | grep -E "error:|BUILD (SUCCEEDED|FAILED)" | head -40`
Expected: `** BUILD SUCCEEDED **`.

---

### Task 3: Состояние рассылки на шести исходах

**Files:**
- Modify: `settings/sms_campaign.cpp` - `CampaignRunState` (~3785), `MakeRowLabel`/`ShowRowLabel` (~3885-3927), `FinishCampaign` (~3947), `RunSmsCampaign` (~4096-5014)
- Modify: `core/ggtg_sms_stats.h` (комментарий ключей, `Report`)

**Interfaces:**
- Consumes: `SmsModel::Recipient/Account/Counts/Settle/RewritePeerToSpam/CorrectLateOutcome/Phase`.
- Produces в `CampaignRunState` (для задач 4-5): `recipients`, `order`, `indexOf`, `counts`, `accountViews`, `accountIndex`, `laneLive`, `recentDurations`, `laneLastSettle`, `lanes`, `goal`, `filter`, `expanded`, `refreshUi`, `refreshAccounts`, `phase()`.

- [ ] **Step 1: Поля `CampaignRunState`**

Удалить поля `rowTexts`, `rowColors`, `rows`, `colorAnims`, `shown`, `isShown`, `rowsArea`, `laneViews`, `laneCards`, `summaryData`, `summaryCard`, `sent`, `alreadyOrError`, `spamAcc`, а также `struct SmsLaneView`, `class SmsLaneCard`, `struct SmsSummaryData`, `class SmsSummaryCard`, `kMaxRowLabels`, `MakeRowLabel`, `ShowRowLabel`. Добавить в структуру:

```cpp
	std::vector<SmsModel::Recipient> recipients;
	// Indices of recipients worked on, newest first: what the list in the
	// progress window shows. Capped at kMaxOrder; older ones still count in
	// `counts` and stay in the "Процессы" log.
	std::vector<int> order;
	base::flat_map<QString, int> indexOf; // lowercase username -> index
	SmsModel::Counts counts{};
	std::vector<SmsModel::Account> accountViews;
	base::flat_map<Main::Account*, int> accountIndex;
	// What each lane is doing right now, shown on its account's row.
	std::vector<QString> laneLive;
	// Seconds a lane spent per recipient lately: the time estimate.
	std::vector<int> recentDurations;
	std::vector<crl::time> laneLastSettle;
	int lanes = 1;
	int goal = 0;
	QString summaryText; // for "Процессы" when the run ends
	rpl::variable<SmsModel::Filter> filter = SmsModel::Filter::All;
	// First indices of the folded groups the user opened.
	base::flat_set<int> expanded;
	// Set by the open progress window, empty while it is closed.
	Fn<void()> refreshUi;
	Fn<void()> refreshAccounts;

	[[nodiscard]] SmsModel::Phase phase() const {
		if (!finished) {
			return paused ? SmsModel::Phase::Paused : SmsModel::Phase::Running;
		} else if (stopped) {
			return SmsModel::Phase::Stopped;
		}
		return (unprocessed > 0)
			? SmsModel::Phase::AccountsOut
			: SmsModel::Phase::Done;
	}
```

Вместо удалённых `ShowRowLabel` и `kMaxRowLabels`:

```cpp
constexpr auto kMaxOrder = 1000;
constexpr auto kMaxRecipientRows = 200;
constexpr auto kRecentDurations = 20;

// Recipient `i` is being worked on: it goes to the top of the list.
void PushToOrder(not_null<CampaignRunState*> state, int i) {
	auto &order = state->order;
	order.erase(ranges::remove(order, i), order.end());
	order.insert(order.begin(), i);
	if (int(order.size()) > kMaxOrder) {
		order.resize(kMaxOrder);
	}
}
```

Начальное значение `buttonText` изменить с «Остановить очередь» на «Стоп».

- [ ] **Step 2: Начальное состояние в `RunSmsCampaign`**

Блок от `state->rowTexts = std::vector<QString>(total);` до цикла `laneViews[lane].status = ...` заменить на:

```cpp
	state->lanes = lanes;
	state->goal = goal;
	state->recipients = std::vector<SmsModel::Recipient>(total);
	for (auto i = 0; i != total; ++i) {
		state->recipients[i].username = usernames[i];
		state->indexOf.emplace(usernames[i].toLower(), i);
	}
	for (auto k = 0; k != int(accounts.size()); ++k) {
		state->accountIndex.emplace(accounts[k].get(), k);
		state->accountViews.push_back({ .label = AccountLabel(accounts[k]) });
	}
	state->laneLive = std::vector<QString>(lanes);
	state->laneLastSettle = std::vector<crl::time>(lanes, 0);
	state->laneAccount = std::vector<Main::Account*>(lanes, nullptr);
	state->laneSuccesses = std::vector<int>(lanes, 0);
	state->laneFinished = std::vector<bool>(lanes, false);
	state->laneErrorStreak = std::vector<int>(lanes, 0);
	state->lanePeerRefusalStreak = std::vector<int>(lanes, 0);
	state->laneNotFoundStreak = std::vector<int>(lanes, 0);
	state->laneFloodWaits = std::vector<int>(lanes, 0);
	state->laneNotFoundPending = std::vector<std::vector<QString>>(lanes);
	state->laneRunners.resize(lanes);
```

Если тип элементов `accounts` не `not_null<Main::Account*>`, использовать `.get()` по месту.

- [ ] **Step 3: Помощники вместо `settleRow` / `setRowInProgress` / `setLane*`**

Заменить `settleRow`, `setRowInProgress`, `showLane`, `setLaneLabel`, `setLaneAccount`, `setLaneSteps` на:

```cpp
	const auto refreshUi = [=] {
		if (state->refreshUi) {
			state->refreshUi();
		}
	};
	const auto refreshAccounts = [=] {
		if (state->refreshAccounts) {
			state->refreshAccounts();
		}
	};
	const auto accountView = [=](not_null<Main::Account*> account)
	-> SmsModel::Account& {
		return state->accountViews[state->accountIndex[account.get()]];
	};
	// A settled recipient: counted, shown, logged, its time recorded for the
	// estimate. `lane` is -1 outside a lane.
	const auto settle = [=](
			int i,
			int lane,
			SmsModel::Outcome outcome,
			const QString &reason,
			Main::Account *account) {
		SmsModel::Settle(state->recipients, state->counts, i, outcome, reason);
		auto &r = state->recipients[i];
		if (account) {
			r.account = AccountLabel(account);
			auto &view = accountView(account);
			if (outcome == SmsModel::Outcome::Sent) {
				++view.sent;
			} else if (outcome == SmsModel::Outcome::Peer) {
				++view.refused;
			} else if (outcome == SmsModel::Outcome::Fail) {
				++view.failed;
			}
		}
		if (lane >= 0) {
			const auto now = crl::now();
			if (const auto last = state->laneLastSettle[lane]) {
				state->recentDurations.push_back(int((now - last) / 1000));
				if (int(state->recentDurations.size()) > kRecentDurations) {
					state->recentDurations.erase(
						state->recentDurations.begin());
				}
			}
			state->laneLastSettle[lane] = now;
		}
		if (state->process) {
			state->process->log('@' + r.username + u" — "_q
				+ SmsModel::Title(outcome)
				+ (reason.isEmpty() ? QString() : (u": "_q + reason))
				+ (r.account.isEmpty()
					? QString()
					: (u" ("_q + r.account + ')')));
		}
		refreshUi();
	};
	const auto setInProgress = [=](
			int i,
			not_null<Main::Account*> account) {
		auto &r = state->recipients[i];
		r.inProgress = true;
		r.outcome = std::nullopt;
		r.account = AccountLabel(account);
		PushToOrder(state.get(), i);
		refreshUi();
	};
	// What the lane is doing now ("пауза 42 сек", "открывает профиль @x").
	const auto setLaneLive = [=](int lane, const QString &text) {
		state->laneLive[lane] = text;
		refreshAccounts();
	};
	const auto setAccountState = [=](
			not_null<Main::Account*> account,
			SmsModel::AccountState accountState,
			const QString &note) {
		auto &view = accountView(account);
		view.state = accountState;
		view.note = note;
		refreshUi();
	};
```

Замены по местам вызова:
- `setLaneLabel(lane, X)` → `setLaneLive(lane, X)`.
- `setLaneAccount(lane, label, status)` → `setLaneLive(lane, status)`; когда поток берёт нового кандидата - ещё `setAccountState(candidate, SmsModel::AccountState::Working, QString())`.
- `setLaneSteps(lane, *trail)` → `setLaneLive(lane, trail->isEmpty() ? QString() : trail->back())`.
- `setRowInProgress(i, username, account)` → `setInProgress(i, account)`.
- вызовы `ShowRowLabel(...)` удалить.

- [ ] **Step 4: Пре-проход (прошлые рассылки)**

В цикле пре-прохода оба `settleRow(...)` и строку с `rowTexts[i]` удалить: боты и получатели с записью дают только `++state->earlier;` (в `order` и `counts` не попадают), а в очередь идёт `state->pendingIndices.push_back(i);`.

- [ ] **Step 5: `updateSummary`**

Тело заменить на:

```cpp
	const auto updateSummary = [=] {
		const auto processed = SmsModel::Total(state->counts);
		state->summaryText = SmsModel::FinishLine(
			state->counts,
			state->unprocessed);
		ProgressState() = QString::number(processed) + '/'
			+ QString::number(goal);
		if (state->process) {
			state->process->progress(processed, goal);
		}
		refreshUi();
	};
```

- [ ] **Step 6: Снятие аккаунта и причина**

`retireAccount` заменить на:

```cpp
	const auto retireAccount = [=](
			int lane,
			not_null<Main::Account*> account,
			SmsModel::AccountState accountState,
			const QString &reason) {
		state->laneAccount[lane] = nullptr;
		state->laneNotFoundPending[lane].clear();
		state->laneLive[lane] = QString();
		setAccountState(account, accountState, reason);
		updateSummary();
		if (state->process) {
			state->process->log(AccountLabel(account)
				+ (accountState == SmsModel::AccountState::Done
					? u" отработал: "_q
					: u" снят с рассылки: "_q)
				+ reason);
		}
	};
```

Вызовы (внутри колбэка `done`, где `account` в области видимости):
- AccountDead → `retireAccount(lane, account, SmsModel::AccountState::Retired, u"сессия недействительна ("_q + message + ')')`.
- FloodWait → `Retired`, `u"FLOOD_WAIT "_q + QString::number(seconds) + u" сек"_q`.
- серия NotFound → `Retired`, `QString::number(kNotFoundStreakLimit) + u" раз подряд «не найден»: Telegram ограничил поиск"_q`.
- серия отказов (`peerRefusalRetire`) → `Retired`, `u"Telegram "_q + QString::number(kSmsPeerRefusalStreakLimit) + u" раз подряд не пустил к получателям"_q`.
- серия сбоев (`errorStreakRetire`) → `Retired`, `QString::number(kSmsErrorStreakLimit) + u" сбоев подряд"_q`.
- дневной лимит → `Done`, `u"дневной лимит "_q + QString::number(dailyLimit)`.
- квота (было `state->laneAccount[lane] = nullptr; quotaReached = true;`) → `retireAccount(lane, account, SmsModel::AccountState::Done, u"лимит "_q + QString::number(perAccountQuota) + u" сообщений"_q); quotaReached = true;`.
- спам-блок (`state->laneAccount[lane] = nullptr` в ветке `accountBlocked` и в `done` вердикта, там - только если `state->laneAccount[lane] == account`) → `SmsModel::AccountState::SpamBlocked`, `AccountSpamStatusText(account->session().user()->id.value)`.
- поток закончился, а аккаунт ещё за ним (перед обоими `MarkLaneFinished`): `if (const auto left = state->laneAccount[lane]) { setAccountState(left, SmsModel::AccountState::Done, u"получатели кончились"_q); state->laneAccount[lane] = nullptr; }`.
- кандидат пропущен по дневному лимиту (перед `continue` в цикле выбора): `setAccountState(candidate, SmsModel::AccountState::Done, u"дневной лимит уже исчерпан"_q);`.

- [ ] **Step 7: Исходы отправки**

Ветка `Skip` / `NotFound` вместо `++state->alreadyOrError` и `settleRow`:

```cpp
				settle(
					i,
					lane,
					(outcome == SmsStepOutcome::NotFound)
						? SmsModel::Outcome::NotFound
						: SmsModel::Outcome::Fail,
					(outcome == SmsStepOutcome::NotFound) ? QString() : message,
					account);
```

Тело `requeue(i, username, why)`:

```cpp
		state->retryIndices.push_back(i);
		auto &r = state->recipients[i];
		r.inProgress = false;
		r.outcome = std::nullopt;
		r.account = QString();
		state->order.erase(ranges::remove(state->order, i), state->order.end());
		if (state->process) {
			state->process->log('@' + username + u" — снова в очереди ("_q
				+ why + ')');
		}
		refreshUi();
```

Основная ветка: убрать `++state->sent`, `++state->spamAcc`, `++state->alreadyOrError` и итоговый `settleRow(...)`; перед `updateSummary()` добавить:

```cpp
				const auto outcomeNow = success
					? SmsModel::Outcome::Sent
					: spamAcc
					? (IsAccountSpamBlocked(account)
						? SmsModel::Outcome::Spam
						: SmsModel::Outcome::Peer)
					: (ClassifySmsError(message) == SmsErrorKind::RecipientOnly)
					? SmsModel::Outcome::Closed
					: SmsModel::Outcome::Fail;
				const auto stickerOnly = !success
					&& trail->contains(u"✓ отправил стикер"_q);
				settle(
					i,
					lane,
					outcomeNow,
					(outcomeNow == SmsModel::Outcome::Sent
						|| outcomeNow == SmsModel::Outcome::Peer
						|| outcomeNow == SmsModel::Outcome::Spam)
						? QString()
						: ((stickerOnly
							? u"стикер ушёл, текст нет: "_q
							: QString()) + HumanSmsError(message)),
					account);
```

Переписывание отказов в спам-блок (в `done` вердикта, после цикла `Data::MarkSmsStatus(name, SpamAcc, ...)` по `state->peerLimited[account]`):

```cpp
										auto indices = std::vector<int>();
										for (const auto &[name, base]
												: state->peerLimited[account]) {
											const auto it = state->indexOf.find(
												name.toLower());
											if (it != state->indexOf.end()) {
												indices.push_back(it->second);
											}
										}
										SmsModel::RewritePeerToSpam(
											state->recipients,
											state->counts,
											indices);
										accountView(account).refused = 0;
										updateSummary();
```

Ответ после таймаута (третий колбэк `SendSmsTextMessage`): вместо `++state->spamAcc` / `++state->sent` / `--state->alreadyOrError` / `settleRow(...)`:

```cpp
				SmsModel::CorrectLateOutcome(
					state->recipients,
					state->counts,
					i,
					spamAcc
						? (IsAccountSpamBlocked(account)
							? SmsModel::Outcome::Spam
							: SmsModel::Outcome::Peer)
						: SmsModel::Outcome::Sent);
				auto &view = accountView(account);
				if (view.failed > 0) {
					--view.failed;
				}
				if (spamAcc) {
					++view.refused;
				} else {
					++view.sent;
				}
				updateSummary();
```

Запись статистики (`record`, `info->outcome = ...`): ветку `SpamBlock` заменить на `IsAccountSpamBlocked(account) ? u"spam"_q : u"peer"_q`, а завершающее `u"error"_q` - на `(ClassifySmsError(message) == SmsErrorKind::RecipientOnly) ? u"closed"_q : u"error"_q`.

- [ ] **Step 8: `finalize` и `FinishCampaign`**

В `state->finalize` тело лямбды `mark` заменить на:

```cpp
			auto &r = state->recipients[i];
			r.inProgress = false;
			r.outcome = std::nullopt;
			++state->unprocessed;
```

В `FinishCampaign` три вызова `state->process->succeed(...)` заменить одним:

```cpp
	if (state->process) {
		state->process->succeed(
			SmsModel::PhaseTitle(state->phase()) + u". "_q + state->summaryText);
	}
```

и сразу после `state->running = false;` добавить `if (state->refreshUi) { state->refreshUi(); }`.

- [ ] **Step 9: Ключи статистики**

В `core/ggtg_sms_stats.h`: комментарий у `outcome` (стр. 51) → `// "sent", "peer", "closed", "spam", "skip", "notfound", "flood", "dead", "error"`; в `Report`, лямбда `add` (стр. ~356): `} else if (a.outcome == u"spam"_q || a.outcome == u"peer"_q) {`. Остальные места берут исходы через `FromStatsKey` (задача 6).

- [ ] **Step 10: Сборка зелёная**

`BuildCampaignProgressBox` пока ссылается на удалённые поля: заменить его тело заглушкой `show->show(Box([=](not_null<Ui::GenericBox*> box) { box->setTitle(rpl::single(u"Рассылка"_q)); }));` (задача 4 пишет его заново).

Run the build command. Expected: `** BUILD SUCCEEDED **`.
Run: `grep -n "alreadyOrError\|rowTexts\|laneViews\|summaryData\|settleRow\|SmsLaneCard\|state->spamAcc\|state->sent" nicegram-desktop/Telegram/SourceFiles/settings/sms_campaign.cpp`
Expected: пусто.

---

### Task 4: Окно хода рассылки

**Files:**
- Modify: `settings/sms_campaign.cpp` - `BuildCampaignProgressBox` (~4003)

**Interfaces:**
- Consumes: поля `CampaignRunState` (задача 3); `SmsWidgets::StatusCard/OutcomeTiles/FilterChips/AccountRow/RecipientRow`.
- Produces: `void BuildCampaignProgressBox(std::shared_ptr<Ui::Show>, std::shared_ptr<CampaignRunState>)` - та же подпись.

- [ ] **Step 1: Новое тело**

```cpp
void BuildCampaignProgressBox(
		std::shared_ptr<Ui::Show> show,
		std::shared_ptr<CampaignRunState> state) {
	show->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setWidth(st::accountsBoxWidth);
		box->setTitle(rpl::single(u"Рассылка"_q));
		Ggtg::AddProcessHideButton(box, [=](std::shared_ptr<Ui::Show> to) {
			BuildCampaignProgressBox(std::move(to), state);
		});
		const auto layout = box->verticalLayout();
		Ui::AddSkip(layout);
		const auto status = box->addRow(
			object_ptr<SmsWidgets::StatusCard>(box));
		Ui::AddSkip(layout);
		const auto tiles = box->addRow(
			object_ptr<SmsWidgets::OutcomeTiles>(box, true));

		// The headline "Аккаунты · 23 выбрано · 2 в работе ..." is a label
		// the refresh below rewrites.
		const auto accountsTitle = box->addRow(
			object_ptr<Ui::FlatLabel>(box, QString(), st::defaultSubsectionTitle),
			st::defaultSubsectionTitlePadding);
		const auto accountsWrap = box->addRow(
			object_ptr<Ui::VerticalLayout>(box),
			style::margins(
				st::boxRowPadding.left() - style::ConvertScale(12),
				0,
				st::boxRowPadding.right() - style::ConvertScale(12),
				0));
		const auto showAllAccounts = std::make_shared<bool>(false);

		Ui::AddSubsectionTitle(layout, rpl::single(u"Получатели"_q));
		const auto chips = box->addRow(
			object_ptr<SmsWidgets::FilterChips>(box));
		Ui::AddSkip(layout, style::ConvertScale(6));
		const auto rowsArea = AddScrollableRowsArea(layout);
		const auto emptyRows = box->addRow(
			object_ptr<Ui::FlatLabel>(
				box,
				u"Пока никого: первые получатели появятся здесь."_q,
				st::boxDividerLabel));

		const auto rebuildAccounts = [=] {
			while (accountsWrap->count()) {
				delete accountsWrap->widgetAt(0);
			}
			const auto order = SmsModel::SortedAccounts(state->accountViews);
			const auto limit = *showAllAccounts
				? int(order.size())
				: std::min(int(order.size()), 8);
			for (auto k = 0; k != limit; ++k) {
				const auto index = order[k];
				auto live = QString();
				for (auto lane = 0; lane != state->lanes; ++lane) {
					const auto account = state->laneAccount[lane];
					if (account
						&& state->accountIndex[account] == index) {
						live = state->laneLive[lane];
					}
				}
				const auto row = accountsWrap->add(
					object_ptr<SmsWidgets::AccountRow>(accountsWrap));
				row->setData(state->accountViews[index], live);
			}
			if (limit < int(order.size())) {
				const auto more = accountsWrap->add(
					object_ptr<Ui::LinkButton>(
						accountsWrap,
						u"ещё "_q + QString::number(order.size() - limit)
							+ u" аккаунтов"_q),
					style::margins(style::ConvertScale(12), 4, 0, 8));
				more->setClickedCallback([=] {
					*showAllAccounts = true;
					if (state->refreshAccounts) {
						state->refreshAccounts();
					}
				});
			}
			accountsWrap->resizeToWidth(accountsWrap->width());
		};

		const auto rebuildRows = [=] {
			while (rowsArea->count()) {
				delete rowsArea->widgetAt(0);
			}
			const auto rows = SmsModel::Fold(
				state->recipients,
				state->order,
				state->filter.current(),
				kMaxRecipientRows);
			for (const auto &row : rows) {
				const auto first = row.indices.front();
				const auto expanded = state->expanded.contains(first);
				const auto widget = rowsArea->add(
					object_ptr<SmsWidgets::RecipientRow>(rowsArea));
				widget->setData(row, state->recipients, expanded);
				if (row.indices.size() < 2) {
					continue;
				}
				widget->setClickedCallback([=] {
					if (!state->expanded.remove(first)) {
						state->expanded.emplace(first);
					}
					state->refreshUi();
				});
				if (!expanded) {
					continue;
				}
				for (const auto index : row.indices) {
					const auto single = rowsArea->add(
						object_ptr<SmsWidgets::RecipientRow>(rowsArea),
						style::margins(style::ConvertScale(20), 0, 0, 0));
					single->setData(
						SmsModel::DisplayRow{
							.indices = { index },
							.outcome = row.outcome,
							.account = row.account,
						},
						state->recipients,
						false);
				}
			}
			emptyRows->setVisible(rows.empty());
			rowsArea->resizeToWidth(rowsArea->width());
		};

		const auto refresh = [=] {
			const auto processed = SmsModel::Total(state->counts);
			const auto phase = state->phase();
			auto subtitle = QString::number(state->lanes)
				+ (state->lanes == 1 ? u" поток"_q : u" потока"_q);
			const auto eta = SmsModel::EstimateSeconds(
				state->recentDurations,
				std::max(state->goal - processed, 0),
				state->lanes);
			if (phase == SmsModel::Phase::Running && eta > 0) {
				subtitle += u" · осталось "_q + SmsModel::DurationText(eta);
			}
			status->setData({
				.title = SmsModel::PhaseTitle(phase),
				.subtitle = subtitle,
				.processed = processed,
				.goal = state->goal,
				.counts = state->counts,
				.finishLine = state->finished
					? SmsModel::FinishLine(state->counts, state->unprocessed)
					: QString(),
				.earlierLine = state->earlier
					? (u"Пропущены заранее (уже писали, боты): "_q
						+ QString::number(state->earlier))
					: QString(),
			});
			tiles->setCounts(state->counts);
			chips->setCounts(state->counts);
			accountsTitle->setText(
				SmsModel::AccountsHeadline(state->accountViews));
			rebuildAccounts();
			rebuildRows();
		};
		state->refreshUi = refresh;
		state->refreshAccounts = rebuildAccounts;
		// Closed (or hidden): the run goes on, the window is rebuilt from
		// `state` the next time it opens.
		box->lifetime().add([=] {
			state->refreshUi = nullptr;
			state->refreshAccounts = nullptr;
		});

		state->filter.value(
		) | rpl::on_next([=](SmsModel::Filter filter) {
			tiles->setFilter(filter);
			chips->setFilter(filter);
			rebuildRows();
		}, box->lifetime());
		rpl::merge(
			tiles->filterRequests(),
			chips->filterRequests()
		) | rpl::on_next([=](SmsModel::Filter filter) {
			state->filter = filter;
		}, box->lifetime());
		refresh();

		const auto dismiss = [=] {
			if (CurrentCampaign() == state) {
				CurrentCampaign() = nullptr;
				IndicatorState() = SmsCampaignIndicator::None;
				ProgressState() = QString();
			}
		};
		box->addButton(state->buttonText.value(), [=] {
			if (!state->finished) {
				// Stopping keeps the box open on the final results.
				*state->cancelled = true;
				state->stopped = true;
				FinishCampaign(state.get());
				return;
			}
			dismiss();
			box->closeBox();
		});
		const auto pauseButton = box->addButton(
			state->pauseText.value(),
			[=] {
				if (state->finished) {
					return;
				}
				state->paused = !state->paused;
				state->pauseText = state->paused
					? u"Продолжить"_q
					: u"Пауза"_q;
				if (state->process) {
					state->process->log(state->paused
						? u"Пауза"_q
						: u"Продолжение"_q);
				}
				refresh();
			});
		const auto results = box->addLeftButton(
			rpl::single(u"Результаты"_q),
			[=] {
				box->uiShow()->show(MakeSmsResultsBoxImpl());
			});
		state->running.value(
		) | rpl::on_next([=](bool running) {
			if (const auto button = pauseButton.data()) {
				button->setVisible(running);
			}
			if (const auto button = results.data()) {
				button->setVisible(!running);
			}
		}, box->lifetime());
	}));
}
```

Замечания исполнителю:
- `MakeSmsResultsBoxImpl()` - шаг 2.
- Если `Ui::AddSubsectionTitle` и `st::defaultSubsectionTitle` в этой версии называются иначе, взять стиль и отступы из соседних `Ui::AddSubsectionTitle(box->verticalLayout(), ...)` вызовов в том же файле (`grep -n "AddSubsectionTitle" sms_campaign.cpp`).
- Кнопка закрытия окна и «Стоп/Готово» работают как раньше: `state->buttonText` меняется в `FinishCampaign` на «Готово».

- [ ] **Step 2: «Результаты» без контроллера**

Проверить: `grep -n "controller" ` в теле `MakeSmsResultsBox` (~6791-6917). Если `controller` не используется - переименовать тело в `object_ptr<Ui::BoxContent> MakeSmsResultsBoxImpl()` (объявить в анонимном namespace выше `BuildCampaignProgressBox`), а `MakeSmsResultsBox(controller)` пусть возвращает `MakeSmsResultsBoxImpl()`. Если используется - добавить в `CampaignRunState` поле `base::weak_ptr<Window::SessionController> controller`, заполнить в `RunSmsCampaign` из `weakController`, и вызывать из кнопки `MakeSmsResultsBox(alive)` для живого контроллера.

- [ ] **Step 3: Сборка зелёная**

Run the build command. Expected: `** BUILD SUCCEEDED **`.

- [ ] **Step 4: Перерисовка**

Run: `grep -n "refreshUi()" nicegram-desktop/Telegram/SourceFiles/settings/sms_campaign.cpp | head -30`
Expected: `setLaneLive` вызывает только `refreshAccounts()`, а полная `refreshUi()` - из `settle`, `setInProgress`, `setAccountState`, `updateSummary`, `requeue`, `FinishCampaign`. Пересборка 200 строк на каждый шаг потока не нужна.

---

### Task 5: Демо-режим и проверка окна хода

**Files:**
- Modify: `settings/sms_campaign.cpp` - перед `OpenSmsCampaign` (~5488)

**Interfaces:**
- Consumes: `CampaignRunState`, `BuildCampaignProgressBox`.
- Produces: временная `ShowSmsProgressDemo(std::shared_ptr<Ui::Show>)` (удаляется в задаче 9).

- [ ] **Step 1: Демо-состояние**

```cpp
// TEMPORARY (removed before shipping): GGTG_SMS_DEMO=1 opens the progress
// window on made-up data - every outcome, 23 accounts, folded runs - so the
// layout can be checked without writing to anyone.
void ShowSmsProgressDemo(std::shared_ptr<Ui::Show> show) {
	using namespace SmsModel;
	const auto state = std::make_shared<CampaignRunState>();
	state->lanes = 2;
	state->goal = 500;
	state->earlier = 40;
	const auto names = QStringList{
		u"Daniel"_q, u"Maria"_q, u"Alex"_q, u"Oleg"_q, u"Kate"_q };
	for (auto k = 0; k != 23; ++k) {
		auto view = Account{
			.label = (k < names.size())
				? names[k]
				: (u"Аккаунт "_q + QString::number(k + 1)),
		};
		view.state = (k < 2) ? AccountState::Working
			: (k == 2) ? AccountState::Retired
			: (k == 3) ? AccountState::SpamBlocked
			: (k < 13) ? AccountState::Done
			: AccountState::Waiting;
		if (k == 2) {
			view.note = u"Telegram 6 раз подряд не пустил к получателям"_q;
			view.sent = 3;
			view.refused = 6;
		} else if (k == 3) {
			view.note = u"@spambot: ограничен"_q;
		} else if (view.state == AccountState::Done) {
			view.note = u"лимит 15 сообщений"_q;
			view.sent = 15;
		}
		state->accountViews.push_back(view);
	}
	state->laneLive = { u"открывает профиль"_q, u"пауза 42 сек"_q };
	const auto plan = std::vector<std::pair<Outcome, int>>{
		{ Outcome::Sent, 0 }, { Outcome::Sent, 1 }, { Outcome::Closed, 1 },
		{ Outcome::Peer, 0 }, { Outcome::Peer, 0 }, { Outcome::Peer, 0 },
		{ Outcome::Peer, 0 }, { Outcome::Peer, 0 }, { Outcome::Sent, 1 },
		{ Outcome::NotFound, 1 }, { Outcome::Fail, 0 }, { Outcome::Spam, 3 },
	};
	for (auto i = 0; i != int(plan.size()); ++i) {
		state->recipients.push_back({
			.username = u"demo_user_"_q + QString::number(i),
			.account = state->accountViews[plan[i].second].label,
		});
		Settle(
			state->recipients,
			state->counts,
			i,
			plan[i].first,
			(plan[i].first == Outcome::Closed)
				? u"закрыл сообщения"_q
				: (plan[i].first == Outcome::Fail)
				? u"нет ответа сервера"_q
				: QString());
		state->order.insert(state->order.begin(), i);
	}
	state->recipients.push_back({
		.username = u"kinnardian"_q,
		.account = u"Daniel"_q,
		.inProgress = true,
	});
	state->order.insert(state->order.begin(), int(plan.size()));
	state->counts[int(Outcome::Sent)] = 86;
	state->counts[int(Outcome::Peer)] = 27;
	state->recentDurations = { 45, 50, 55 };
	BuildCampaignProgressBox(show, state);
}
```

В начале `OpenSmsCampaign`:

```cpp
	if (!CurrentCampaign() && qEnvironmentVariableIsSet("GGTG_SMS_DEMO")) {
		ShowSmsProgressDemo(controller->uiShow());
		return;
	}
```

- [ ] **Step 2: Сборка зелёная**

Run the build command. Expected: `** BUILD SUCCEEDED **`.

- [ ] **Step 3: Запустить и посмотреть**

Закрыть установленный GGTG, затем:

```bash
GGTG_SMS_DEMO=1 /Users/yansheva/Desktop/ggtg-macos/nicegram-desktop/out/Release/GGTG.app/Contents/MacOS/GGTG
```

Открыть GGTG, «Рассылка». Снимок окна (computer-use, `scale: 0.5`). Expected:
- «Идёт рассылка · 2 потока · осталось ≈ …», крупно «113 / 500» (86 + 27 + остальные), цветная полоса;
- шесть плиток с числами, «Спам-блок акк.» красная;
- «Аккаунты · 23 выбрано · 2 в работе · 10 отработали · 2 сняты · 10 ждут» (проверить сумму по демо-данным), 8 строк и «ещё 15 аккаунтов»;
- «Отказ Telegram × 5 подряд · Daniel ▾», клик раскрывает 5 имён;
- плитка «Отказ Telegram» оставляет только отказы без свёртки, повторный клик - все.

- [ ] **Step 4: Две темы**

Переключить тему (Настройки → Оформление) и повторить снимок. Expected: текст читается на обеих, серые исходы не сливаются с фоном.

---

### Task 6: «Результаты» на шести исходах

**Files:**
- Modify: `settings/sms_campaign.cpp` - `Funnel`, `AddToFunnel`, `FunnelLine`, `MakeSmsResultsBox` (~6706-6917)

**Interfaces:**
- Consumes: `SmsModel::FromStatsKey`, `Counts`, `OftenRefused`; `SmsWidgets::OutcomeTiles(parent, false)`, `SegmentBar`.
- Produces: `MakeSmsResultsBox` - та же подпись.

- [ ] **Step 1: Воронка знает исходы**

```cpp
struct Funnel {
	SmsModel::Counts counts{};
	int replied = 0;
	int known = 0; // answers whose kind is known
	int positive = 0;

	[[nodiscard]] int sent() const {
		return counts[int(SmsModel::Outcome::Sent)];
	}
	[[nodiscard]] int attempts() const {
		return SmsModel::Total(counts);
	}
};

void AddToFunnel(Funnel &f, const Core::GgtgSmsStats::Attempt &a) {
	const auto outcome = SmsModel::FromStatsKey(a.outcome);
	if (!outcome) {
		return;
	}
	++f.counts[int(*outcome)];
	if (*outcome == SmsModel::Outcome::Sent && a.replied) {
		++f.replied;
		if (!a.replyKind.isEmpty()) {
			++f.known;
		}
		if (Core::GgtgSmsStats::IsPositiveKind(a.replyKind)) {
			++f.positive;
		}
	}
}

[[nodiscard]] QString FunnelLine(const Funnel &f) {
	return u"отправлено "_q + QString::number(f.sent())
		+ u" · ответили "_q + QString::number(f.replied)
		+ u" ("_q + Percent(f.replied, f.sent()) + u") · положительных "_q
		+ QString::number(f.positive);
}
```

Все `f.sent`, `total.sent`, `i->second.sent` в `MakeSmsResultsBox` и в `ShowSmsStatsBox` заменить на `.sent()`; `f.failed` больше нет.

- [ ] **Step 2: Период**

В начало лямбды окна (перед `AddSortRepliesLink`):

```cpp
		// 0 - the last run, 1 - 7 days, 2 - all time. Remembered while the
		// app runs.
		static auto period = 0;
		const auto slider = box->addRow(
			object_ptr<Ui::SettingsSlider>(box, st::defaultTabsSlider),
			st::boxRowPadding);
		slider->setSections({
			u"Последняя"_q,
			u"7 дней"_q,
			u"Всё время"_q,
		});
		slider->setActiveSectionFast(period);
```

(include `ui/widgets/discrete_sliders.h`). В `*rebuild` после `FillReplies(attempts);`:

```cpp
				const auto lastRun = attempts.empty()
					? qint64(0)
					: ranges::max(attempts, ranges::less(), [](const auto &a) {
						return a.run;
					}).run;
				const auto since = qint64(base::unixtime::now()) - 7 * 86400;
				attempts.erase(ranges::remove_if(attempts, [&](const auto &a) {
					return (period == 0) ? (a.run != lastRun)
						: (period == 1) ? (a.at < since)
						: false;
				}), attempts.end());
```

и подписка: `slider->sectionActivated() | rpl::on_next([=](int section) { period = section; (*rebuild)(); }, box->lifetime());`.

- [ ] **Step 3: Плитки и полосы**

Строку «Все рассылки» (`row(...)`) заменить на:

```cpp
				const auto tiles = wrap->add(
					object_ptr<SmsWidgets::OutcomeTiles>(wrap, false),
					st::boxRowPadding);
				tiles->setCounts(total.counts);
				wrap->add(
					object_ptr<Ui::FlatLabel>(
						wrap,
						FunnelLine(total),
						st::boxDividerLabel),
					st::boxRowPadding);
```

После `row(...)` каждой рассылки:

```cpp
					const auto bar = wrap->add(
						object_ptr<SmsWidgets::SegmentBar>(wrap),
						style::margins(
							st::boxRowPadding.left() + style::ConvertScale(14),
							0,
							st::boxRowPadding.right() + style::ConvertScale(14),
							style::ConvertScale(8)));
					bar->setCounts(i->second.counts, 0);
```

Рассылок показывать 8; после них ссылка `Ui::LinkButton` «Показать ещё», которая ставит захваченный `std::shared_ptr<int> runsShown` в `runs.size()` и вызывает `(*rebuild)()`.

- [ ] **Step 4: Аккаунты**

Цикл по аккаунтам:

```cpp
				for (const auto &[name, f] : accounts) {
					const auto refused = f.counts[int(SmsModel::Outcome::Peer)];
					const auto spam = f.counts[int(SmsModel::Outcome::Spam)];
					const auto failed = f.counts[int(SmsModel::Outcome::Fail)];
					auto subtitle = FunnelLine(f);
					if (refused) {
						subtitle += u" · отказ Telegram "_q
							+ QString::number(refused);
					}
					if (failed) {
						subtitle += u" · сбой "_q + QString::number(failed);
					}
					row(
						name,
						subtitle,
						spam ? u"спам-блок"_q
							: SmsModel::OftenRefused(refused, f.attempts())
							? u"часто отказ"_q
							: QString(),
						false);
				}
```

- [ ] **Step 5: Сборка и снимок**

Build → `** BUILD SUCCEEDED **`. Запустить GGTG, «Рассылка» → «Результаты», снимок `scale: 0.5`. Expected: переключатель периода, плитки, строки рассылок с цветной полосой, у аккаунтов метки. Старые записи (`spam`) попадают в «Спам-блок акк.», окно не падает.

---

### Task 7: «Полный отчёт» карточками

**Files:**
- Modify: `core/ggtg_sms_stats.h` - выделить группы из `Report` (~317-440)
- Modify: `settings/sms_campaign.cpp` - `ShowSmsStatsBox` (~6492)

**Interfaces:**
- Produces в `Core::GgtgSmsStats`: `struct Section { QString title; std::vector<std::pair<QString, details::Bucket>> rows; }` и `[[nodiscard]] inline std::vector<Section> Sections(const std::vector<Attempt>&, Fn<QString(QString)> languageName, Fn<QString(QString)> countryName)`. `Report(...)` остаётся и выдаёт тот же текст.

- [ ] **Step 1: Эталон текста**

До правок открыть «Рассылка» → «Результаты» → «Полный отчёт», выделить текст, скопировать и сохранить: `pbpaste > /private/tmp/claude-501/-Users-yansheva-Desktop-ggtg-macos/46a502ce-c969-4a86-ada2-7bd63c8ce4f3/scratchpad/report-before.txt`.

- [ ] **Step 2: `Sections`**

Часть `Report`, которая заполняет корзины (от `auto byCountry` до конца цикла по `attempts`), вынести в `Sections`. Она возвращает разделы в таком порядке:

```cpp
	const auto flat = [](const std::map<QString, details::Bucket> &m) {
		return std::vector<std::pair<QString, details::Bucket>>(
			m.begin(),
			m.end());
	};
	return {
		{ u"По стране номера моего аккаунта"_q, flat(byCountry) },
		{ u"По языку получателя (по имени)"_q, flat(byLanguage) },
		{ u"Premium"_q, flat(byPremium) },
		{ u"Когда получатель был в сети"_q, flat(bySeen) },
		{ u"Фото у получателя"_q, flat(byPhoto) },
		{ u"Перевод"_q, flat(byTranslated) },
		{ u"Стикер"_q, flat(bySticker) },
		{ u"Время отправки"_q, flat(byHour) },
	};
```

`Report` вызывает `Sections` и строит из него таблицы (`std::map(rows.begin(), rows.end())` в `details::Table`), а `all`, `byTemplate`, `bySender`, `errors`, `stickerOnly`, `runs` считает сам, как раньше. Порядок таблиц в тексте отчёта: страна, язык, Premium, «в сети», фото, **шаблон**, перевод, стикер, время, мои аккаунты - то есть «Шаблон» стоит между «Фото» и «Перевод», «Мои аккаунты» - в конце; сохранить именно этот порядок.

- [ ] **Step 3: Карточки в `ShowSmsStatsBox`**

Вместо одного `FlatLabel` с `report`:

```cpp
		const auto sections = Core::GgtgSmsStats::Sections(
			attempts,
			[](QString code) { return LanguageLabel(code); },
			[](QString iso) {
				const auto name = iso.isEmpty()
					? QString()
					: Countries::Instance().countryNameByISO2(iso);
				return name.isEmpty() ? u"не определена"_q : name;
			});
		if (report.isEmpty()) {
			box->addRow(object_ptr<Ggtg::EmptyState>(
				box,
				u"Пока нет данных: они копятся с каждой рассылкой."_q));
		}
		for (const auto &section : sections) {
			if (section.rows.empty()) {
				continue;
			}
			Ui::AddSubsectionTitle(
				box->verticalLayout(),
				rpl::single(section.title));
			for (const auto &[key, b] : section.rows) {
				box->addRow(
					object_ptr<Ggtg::InfoRow>(
						box,
						rpl::single(Ggtg::InfoRowData{
							.title = key.isEmpty() ? u"?"_q : key,
							.subtitle = u"попыток "_q
								+ QString::number(b.total)
								+ u" · ушло "_q
								+ Core::GgtgSmsStats::details::Percent(
									b.sent,
									b.total)
								+ u" · отказ Telegram "_q
								+ QString::number(b.spam)
								+ u" · ответили "_q
								+ Core::GgtgSmsStats::details::Percent(
									b.replied,
									b.sent),
							.chip = Core::GgtgSmsStats::details::Percent(
								b.positive,
								b.sent),
							.chipGood = b.positive > 0,
						})),
					st::boxRowPadding);
			}
		}
```

И рядом с «Очистить»:

```cpp
		box->addLeftButton(rpl::single(u"Копировать отчёт"_q), [=] {
			QGuiApplication::clipboard()->setText(report);
			Ggtg::Notice(box->uiShow(), u"Отчёт скопирован"_q);
		});
```

- [ ] **Step 4: Текст отчёта не изменился**

Сборка → открыть «Полный отчёт», «Копировать отчёт», `pbpaste > .../report-after.txt`. Run: `diff report-before.txt report-after.txt` (оба файла в scratchpad-папке из шага 1). Expected: пусто. Снимок окна: заголовки разделов и строки-карточки с процентом справа.

---

### Task 8: Форма «Новая»

**Files:**
- Modify: `settings/sms_campaign.cpp` - `CampaignState` (~303), `RestoreFormState`/`SaveFormState` (~369-420), `SetupAccountsSection` (~1951), `SetupLimitsSection` (~2140), `RefreshRecipientsPreview` (~2387), `SetupRecipientsSection` (~2597), `SetupTextSection` (~3177), `MakeSmsCampaignSetupBox` (~5016)

**Interfaces:**
- Consumes: `SmsWidgets::StepCard`, `SmsModel::SkipBreakdown`.
- Produces: `Setup*Section(..., not_null<Ui::VerticalLayout*> content)` - новый последний параметр.

- [ ] **Step 1: Секции пишут в свой контейнер**

В каждой из `SetupAccountsSection`, `SetupRecipientsSection`, `SetupTextSection`, `SetupLimitsSection`: добавить последний параметр `not_null<Ui::VerticalLayout*> content`; во внешнем теле (не внутри вложенных `Box([=](not_null<Ui::GenericBox*> box)`) заменить `box->verticalLayout()` на `content`, `box->addRow(X, margins)` на `content->add(X, margins)`, `box->addRow(X)` на `content->add(X, st::boxRowPadding)`; удалить собственные `Ui::AddSubsectionTitle(... "1. Аккаунты" ...)` (и «2. Получатели», «3. Текст», «4. Лимиты») и замыкающий `Ui::AddDivider(box->verticalLayout());`.

Проверка: `awk 'NR>=1951 && NR<=3422' nicegram-desktop/Telegram/SourceFiles/settings/sms_campaign.cpp | grep -n "box->addRow\|box->verticalLayout"` - все оставшиеся совпадения должны находиться внутри вложенных `Box(...)` с собственным `box`.

- [ ] **Step 2: Карточки шагов**

В `CampaignState` добавить `std::array<bool, 4> stepExpanded = { true, true, true, true };` и `rpl::variable<QString> accountsSummary, recipientsSummary, textSummary, limitsSummary;`. `stepExpanded` сохраняется и читается в `SaveFormState` / `RestoreFormState` так же, как остальные поля формы (ключ `"stepExpanded"`, массив из четырёх булевых).

В `MakeSmsCampaignSetupBox` четыре вызова `Setup*Section(...)` заменить на:

```cpp
		const auto step = [&](
				int index,
				const QString &title,
				rpl::producer<QString> summary) {
			const auto card = box->addRow(
				object_ptr<SmsWidgets::StepCard>(
					box,
					title,
					std::move(summary),
					state->stepExpanded[index]),
				style::margins(
					st::boxRowPadding.left() - style::ConvertScale(12),
					0,
					st::boxRowPadding.right() - style::ConvertScale(12),
					style::ConvertScale(4)));
			card->expandedValue(
			) | rpl::on_next([=](bool expanded) {
				state->stepExpanded[index] = expanded;
			}, card->lifetime());
			return card->content();
		};
		SetupAccountsSection(
			box,
			state,
			step(0, u"1. Аккаунты"_q, state->accountsSummary.value()));
		SetupRecipientsSection(
			box,
			controller,
			state,
			step(1, u"2. Получатели"_q, state->recipientsSummary.value()));
		SetupTextSection(
			box,
			controller,
			state,
			step(2, u"3. Текст"_q, state->textSummary.value()));
		SetupLimitsSection(
			box,
			state,
			step(3, u"4. Лимиты"_q, state->limitsSummary.value()));
```

Итоги шагов:
- аккаунты: в `RefreshAccountsSelectionUi` - `state->accountsSummary = u"выбрано "_q + QString::number(selected) + (blocked ? (u" · в спам-блоке "_q + QString::number(blocked)) : QString());` (`selected` - число отмеченных, `blocked` - сколько из них `IsAccountSpamBlocked`);
- получатели: в `RefreshRecipientsPreview` - `state->recipientsSummary = u"будет написано "_q + QString::number(stats.remaining) + u" из "_q + QString::number(stats.total);`;
- текст: там, где меняется список шаблонов - `state->textSummary = QString::number(n) + (n == 1 ? u" шаблон"_q : (n < 5 ? u" шаблона"_q : u" шаблонов"_q));`;
- лимиты: там, где применяется пресет - `state->limitsSummary = presetName;` («Осторожно» / «Стандарт» / «Быстро» / «Свои настройки»).

Пока значение не задано, вторая строка заголовка пуста - допустимо.

- [ ] **Step 3: Причины пропуска**

В `RefreshRecipientsPreview` текст `recipientsStatsLabel` заменить на:

```cpp
		const auto skipped = SmsModel::SkipBreakdown(
			stats.sent,
			stats.peerLimit,
			stats.restricted,
			stats.notFound,
			stats.spamAcc,
			stats.bots);
		label->setText(skipped.isEmpty()
			? u"Пропущенных нет."_q
			: (u"Пропущено: "_q + skipped));
```

а текст `recipientsCountLabel` - на `u"Будет написано %1 из %2"_q.arg(stats.remaining).arg(stats.total)`.

- [ ] **Step 4: Шаблоны одним блоком**

В `SetupTextSection` найти пять строк после редактора шаблона (шаблон, «Сохранить шаблон», ИИ-помощник, «Разбор рассылок», чередование): `grep -n "LinkButton\|ActionRow" ` в границах функции. Заменить на одну строку кнопок:

```cpp
		const auto tools = content->add(
			object_ptr<Ui::RpWidget>(content),
			st::boxRowPadding);
		auto buttons = std::make_shared<std::vector<not_null<Ui::RoundButton*>>>();
		const auto addTool = [=](const QString &text, Fn<void()> click) {
			const auto button = Ui::CreateChild<Ui::RoundButton>(
				tools,
				rpl::single(text),
				st::defaultLightButton);
			button->setTextTransform(
				Ui::RoundButton::TextTransform::NoTransform);
			button->setClickedCallback(std::move(click));
			buttons->push_back(button);
		};
		// One addTool per former row, with that row's existing click handler
		// (moved, not rewritten): "Шаблоны", "Сохранить", "ИИ-помощник",
		// "Чередование".
		tools->widthValue(
		) | rpl::on_next([=](int width) {
			auto x = 0;
			auto y = 0;
			const auto gap = style::ConvertScale(8);
			for (const auto button : *buttons) {
				if (x > 0 && x + button->width() > width) {
					x = 0;
					y += button->height() + gap;
				}
				button->moveToLeft(x, y);
				x += button->width() + gap;
			}
			tools->resize(
				width,
				buttons->empty() ? 0 : (y + buttons->back()->height()));
		}, tools->lifetime());
```

Строка «Разбор рассылок» из формы убирается: тот же экран - вкладка «Результаты». Обработчики остальных строк переносятся без изменений.

- [ ] **Step 5: Защиты в лимитах**

В конец `SetupLimitsSection`:

```cpp
	content->add(
		object_ptr<Ui::FlatLabel>(
			content,
			u"Аккаунт снимается с рассылки после: спам-блока · "_q
				+ QString::number(kSmsPeerRefusalStreakLimit)
				+ u" отказов Telegram подряд · "_q
				+ QString::number(kSmsErrorStreakLimit)
				+ u" сбоев подряд · своего лимита сообщений."_q,
			st::boxDividerLabel),
		st::boxRowPadding);
```

- [ ] **Step 6: Сборка и снимок**

Build → `** BUILD SUCCEEDED **`. «Рассылка» → «Новая», снимок. Expected: четыре карточки с итогом в одну строку; клик сворачивает и разворачивает, состояние помнится после закрытия окна; «Будет написано N из M» и «Пропущено: уже писали … · отказ Telegram …»; кнопки шаблонов в одной-двух строках; строка защит в лимитах. Кнопка запуска и строка проверки работают как раньше.

---

### Task 9: Финиш

**Files:**
- Modify: `settings/sms_campaign.cpp` - удалить `ShowSmsProgressDemo` и проверку `GGTG_SMS_DEMO`
- Modify: `window/window_main_menu.cpp:2299,2305` - описание раздела
- Modify: `docs/superpowers/plans/2026-10-07-ggtg-ui-overhaul.md` - отметка в этапе 3

- [ ] **Step 1: Убрать демо**

Run: `grep -rn "GGTG_SMS_DEMO\|ShowSmsProgressDemo" nicegram-desktop/Telegram/SourceFiles`
Удалить функцию и проверку в `OpenSmsCampaign`, повторить `grep`. Expected: пусто.

- [ ] **Step 2: Описание раздела**

В `window_main_menu.cpp` оба `.description` у «Рассылки» / «Новая»:

```cpp
u"Отправляет сообщение выбранным получателям от одного или нескольких аккаунтов. У каждого получателя один из исходов: отправлено, отказ Telegram (аккаунт свободен, Telegram не пустил к этому человеку), закрыт для ЛС, не найден, спам-блок аккаунта или сбой. Итоги и ответы - во вкладках «Результаты» и «Ответы»."_q
```

- [ ] **Step 3: Тест и сборка**

Run: `/Users/yansheva/Desktop/ggtg-macos/ggtg-tests/run.sh` → `sms_campaign_model_test: OK`. Build → `** BUILD SUCCEEDED **`.

- [ ] **Step 4: Окно без демо**

Запустить GGTG без переменной: «Рассылка» → «Новая», «Результаты», «Полный отчёт» открываются. Настоящую рассылку без согласия пользователя не запускать. Когда пользователь запустит настоящую: свернуть окно хода («Скрыть») и открыть из «Процессов» - строки, аккаунты и раскрытая группа на месте; после «Стоп» итог показывает «Остановлено вручную», а справа внизу вместо паузы кнопка «Результаты».

- [ ] **Step 5: Записи**

В `docs/superpowers/plans/2026-10-07-ggtg-ui-overhaul.md`, этап 3, добавить строку: `Доделано 2026-10-07 (план 2026-10-07-ggtg-campaign-clarity.md): шесть исходов, новое окно хода, «Результаты» с периодом и полосами, «Полный отчёт» карточками, шаги «Новой» сворачиваются, пять строк шаблонов - один блок.`

Android (те же шесть исходов) - отдельный план. DMG - только по просьбе.

---

## Самопроверка плана

- **Покрытие спецификации:** шесть исходов и разделение счётчиков - задачи 1, 3; предохранитель - уже сделан; окно хода (статус, плитки, аккаунты, получатели, итог без кнопок действий) - 4-5; «Результаты» и «Полный отчёт» - 6-7; форма «Новая» - 8; код и проверки - 2, 9.
- **Типы:** `SmsModel::Account`, `Recipient`, `DisplayRow`, `Counts`, `Phase`, `Filter` совпадают в задачах 1-8; `accountView`, `settle`, `setLaneLive`, `retireAccount(lane, account, state, reason)` определены в задаче 3 и используются в 4-5.
- **Плейсхолдеры:** тела виджетов (задача 2) описаны построчно по образцу существующих классов; места, где точное имя стиля или функции может отличаться в этой версии tdesktop, названы прямо в замечаниях задач 4 и 8.

