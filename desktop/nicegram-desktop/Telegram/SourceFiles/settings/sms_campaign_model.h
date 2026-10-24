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
	// Not written to on purpose, nothing was sent: a bot, last seen long
	// ago, already in touch... Not a failure of anyone's.
	Skipped,
};
inline constexpr auto kOutcomeCount = 7;
// The outcomes that get a tile (every one but Skipped).
inline constexpr auto kTileOutcomeCount = 6;

[[nodiscard]] inline QString Title(Outcome outcome) {
	switch (outcome) {
	case Outcome::Sent: return u"Отправлено"_q;
	case Outcome::Peer: return u"Telegram не дал написать"_q;
	case Outcome::Closed: return u"Получатель закрыл ЛС"_q;
	case Outcome::NotFound: return u"Не найден"_q;
	case Outcome::Spam: return u"Аккаунт в спам-блоке"_q;
	case Outcome::Fail: return u"Сбой"_q;
	case Outcome::Skipped: return u"Пропущен"_q;
	}
	return QString();
}

// Under a number on a tile.
[[nodiscard]] inline QString TileTitle(Outcome outcome) {
	switch (outcome) {
	case Outcome::Sent: return u"Отправлено"_q;
	case Outcome::Peer: return u"Не дал написать"_q;
	case Outcome::Closed: return u"Закрыли ЛС"_q;
	case Outcome::NotFound: return u"Не найдены"_q;
	case Outcome::Spam: return u"Спам-блок акк."_q;
	case Outcome::Fail: return u"Сбой"_q;
	case Outcome::Skipped: return u"Пропущены"_q;
	}
	return QString();
}

// Lowercase, for running text ("отказ Telegram 41").
[[nodiscard]] inline QString InlineTitle(Outcome outcome) {
	switch (outcome) {
	case Outcome::Sent: return u"отправлено"_q;
	case Outcome::Peer: return u"Telegram не дал написать"_q.toLower();
	case Outcome::Closed: return u"закрыли ЛС"_q;
	case Outcome::NotFound: return u"не найдены"_q;
	case Outcome::Spam: return u"аккаунт в спам-блоке"_q;
	case Outcome::Fail: return u"сбой"_q;
	case Outcome::Skipped: return u"пропущено"_q;
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
	case Outcome::Skipped: return u"skip"_q;
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
	case Outcome::Skipped: return Filter::All; // shown under "Все" only
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
	// Phone and @username (when there is one): tells apart accounts that
	// have the same name.
	QString info;
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
	auto result = u""_q + QString::number(account.sent);
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
	if (seconds < 45) {
		return u"< 1 мин"_q;
	}
	const auto minutes = (seconds + 30) / 60;
	if (minutes < 60) {
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

// "Отправлено 312 · отказ Telegram 41 · ...": zeros left out, except the
// sent count. Recipients nobody got to are not listed: they simply stay in
// the base for the next run.
[[nodiscard]] inline QString FinishLine(const Counts &counts) {
	auto parts = QStringList{
		Title(Outcome::Sent) + ' ' + QString::number(counts[0]),
	};
	for (auto i = 1; i != kOutcomeCount; ++i) {
		if (counts[i]) {
			parts.push_back(
				InlineTitle(Outcome(i)) + ' ' + QString::number(counts[i]));
		}
	}
	return parts.join(u" · "_q);
}

// ---- what one finished step does to its account ----
//
// Kept apart from Telegram and from the window, so every rule can be
// checked on its own (see ggtg-tests/sms_campaign_model_test.cpp).

enum class StepKind {
	Sent, // delivered
	Partial, // the sticker or the first parts went out, then `cause`
	Refused, // PEER_FLOOD while @SpamBot says the account is free
	SpamBlocked, // PEER_FLOOD and @SpamBot says the account is blocked
	Closed, // the recipient can't be written to (privacy, deleted...)
	TextError, // the text itself was refused
	Failed, // timeout, network, unknown: maybe the account's fault
	NotFound, // no such username (as far as this account could tell)
	Skipped, // checked on the spot and left out, or already written to
	FloodWait, // Telegram told the account to back off
	AccountDead, // the session is gone, frozen or banned
	Aborted, // "Стоп" before anything was sent
};

enum class RetireReason {
	None,
	Quota, // did its share: "Сообщений с одного аккаунта"
	Refusals, // "Снять аккаунт после отказов"
	Errors, // too many failures
	NotFound, // too many "not found": its lookups are throttled
	FloodWait,
	AccountDead,
	SpamBlock,
};

enum class PauseKind {
	None, // nothing more for this lane ("Стоп")
	Message, // the same account goes on: "Пауза между сообщениями"
	Account, // the next account takes over: "Пауза между аккаунтами"
};

// One account's counters for the whole campaign: a success never resets
// them.
struct LaneCounters {
	int sent = 0;
	int refusals = 0;
	int errors = 0;
	int notFound = 0;
};

// 0 in any of them: never for that reason.
struct LaneLimits {
	int quota = 0;
	int refusals = 0;
	int errors = 5;
	int notFound = 5;
};

struct StepVerdict {
	RetireReason retire = RetireReason::None;
	PauseKind pause = PauseKind::Message;
};

// Counts the step into `counters` and says whether the account comes off
// and how long the lane waits before its next step. `cause` is what cut a
// Partial send short (FloodWait, AccountDead or anything else).
[[nodiscard]] inline StepVerdict AfterStep(
		StepKind kind,
		LaneCounters &counters,
		const LaneLimits &limits,
		StepKind cause = StepKind::Failed) {
	const auto reached = [](int value, int limit) {
		return (limit > 0) && (value >= limit);
	};
	auto retire = RetireReason::None;
	switch (kind) {
	case StepKind::Aborted:
		return { RetireReason::None, PauseKind::None };
	case StepKind::FloodWait:
		retire = RetireReason::FloodWait;
		break;
	case StepKind::AccountDead:
		retire = RetireReason::AccountDead;
		break;
	case StepKind::SpamBlocked:
		retire = RetireReason::SpamBlock;
		break;
	case StepKind::Sent:
	case StepKind::Partial:
		++counters.sent;
		counters.notFound = 0;
		if (kind == StepKind::Partial && cause == StepKind::FloodWait) {
			retire = RetireReason::FloodWait;
		} else if (kind == StepKind::Partial
				&& cause == StepKind::AccountDead) {
			retire = RetireReason::AccountDead;
		} else if (reached(counters.sent, limits.quota)) {
			retire = RetireReason::Quota;
		}
		break;
	case StepKind::Refused:
		counters.notFound = 0;
		if (reached(++counters.refusals, limits.refusals)) {
			retire = RetireReason::Refusals;
		}
		break;
	case StepKind::Failed:
		if (reached(++counters.errors, limits.errors)) {
			retire = RetireReason::Errors;
		}
		break;
	case StepKind::NotFound:
		if (reached(++counters.notFound, limits.notFound)) {
			retire = RetireReason::NotFound;
		}
		break;
	case StepKind::Closed:
		counters.notFound = 0;
		break;
	case StepKind::TextError:
		counters.notFound = 0;
		break;
	case StepKind::Skipped:
		counters.notFound = 0;
		break;
	}
	return {
		retire,
		(retire == RetireReason::None) ? PauseKind::Message : PauseKind::Account,
	};
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
		int accountFail,
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
	add(accountFail, u"аккаунт не смог"_q);
	add(bots, u"боты"_q);
	return parts.join(u" · "_q);
}

} // namespace Settings::SmsModel
