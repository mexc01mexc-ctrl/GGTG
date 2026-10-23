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
	// Skipped is written as "skip" and, like every "skip", is not an
	// attempt in the stats.
	for (auto i = 0; i != kTileOutcomeCount; ++i) {
		CHECK(FromStatsKey(StatsKey(Outcome(i))) == Outcome(i));
	}
	CHECK_EQ(StatsKey(Outcome::Skipped), u"skip"_q);
}

void TestFilters() {
	CHECK(Matches(Filter::All, Outcome::Fail));
	CHECK(Matches(Filter::Problems, Outcome::NotFound));
	CHECK(Matches(Filter::Problems, Outcome::Spam));
	CHECK(!Matches(Filter::Problems, Outcome::Peer));
	CHECK(FilterFor(Outcome::Spam) == Filter::Problems);
	CHECK(Matches(Filter::All, Outcome::Skipped));
	CHECK(!Matches(Filter::Problems, Outcome::Skipped));
	CHECK(!Matches(Filter::Sent, Outcome::Skipped));
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
		u"12 · отказ 5"_q);
	CHECK_EQ(
		AccountCounters({ .sent = 0, .refused = 0, .failed = 2 }),
		u"0 · сбой 2"_q);
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
		FinishLine(Counts{ 312, 41, 0, 0, 2, 0, 7 }),
		u"Отправлено 312 · "_q + InlineTitle(Outcome::Peer)
			+ u" 41 · аккаунт в спам-блоке 2 · пропущено 7"_q);
	CHECK_EQ(FinishLine(Counts{}), u"Отправлено 0"_q);
	CHECK(OftenRefused(3, 6));
	CHECK(!OftenRefused(2, 6));
	CHECK(!OftenRefused(5, 5)); // too few attempts to judge
	CHECK_EQ(
		SkipBreakdown(80, 20, 0, 3, 0, 2, 1),
		u"уже писали 80 · "_q + InlineTitle(Outcome::Peer)
			+ u" 20 · не найдены 3 · аккаунт не смог 2 · боты 1"_q);
	CHECK(SkipBreakdown(0, 0, 0, 0, 0, 0, 0).isEmpty());
}

void TestAfterStep() {
	// "Нормально": 2 messages per account, off after 3 refusals.
	const auto normal = LaneLimits{ .quota = 2, .refusals = 3 };
	{
		// Two sends: the second one hands over to the next account, after
		// the pause between accounts.
		auto c = LaneCounters();
		auto v = AfterStep(StepKind::Sent, c, normal);
		CHECK(v.retire == RetireReason::None);
		CHECK(v.pause == PauseKind::Message);
		v = AfterStep(StepKind::Sent, c, normal);
		CHECK(v.retire == RetireReason::Quota);
		CHECK(v.pause == PauseKind::Account);
	}
	{
		// A skip, a closed one, a text error: the same account goes on
		// after the pause between messages, nothing is counted against it.
		auto c = LaneCounters();
		for (const auto kind : {
				StepKind::Skipped,
				StepKind::Closed,
				StepKind::TextError }) {
			const auto v = AfterStep(kind, c, normal);
			CHECK(v.retire == RetireReason::None);
			CHECK(v.pause == PauseKind::Message);
		}
		CHECK(c.sent == 0 && c.errors == 0 && c.refusals == 0);
	}
	{
		// Refusals are counted for the whole campaign: a success in between
		// does not wipe them.
		auto c = LaneCounters();
		const auto premium = LaneLimits{ .quota = 40, .refusals = 3 };
		CHECK(AfterStep(StepKind::Refused, c, premium).retire
			== RetireReason::None);
		CHECK(AfterStep(StepKind::Sent, c, premium).retire
			== RetireReason::None);
		CHECK(AfterStep(StepKind::Refused, c, premium).retire
			== RetireReason::None);
		const auto v = AfterStep(StepKind::Refused, c, premium);
		CHECK(v.retire == RetireReason::Refusals);
		CHECK(v.pause == PauseKind::Account);
	}
	{
		// Five failures, with successes in between, take the account off.
		auto c = LaneCounters();
		const auto many = LaneLimits{ .quota = 100 };
		for (auto i = 0; i != 4; ++i) {
			CHECK(AfterStep(StepKind::Failed, c, many).retire
				== RetireReason::None);
			CHECK(AfterStep(StepKind::Sent, c, many).retire
				== RetireReason::None);
		}
		CHECK(AfterStep(StepKind::Failed, c, many).retire
			== RetireReason::Errors);
	}
	{
		// Five "not found" mean throttled lookups.
		auto c = LaneCounters();
		for (auto i = 0; i != 4; ++i) {
			CHECK(AfterStep(StepKind::NotFound, c, normal).pause
				== PauseKind::Message);
		}
		CHECK(AfterStep(StepKind::NotFound, c, normal).retire
			== RetireReason::NotFound);
	}
	{
		// FLOOD_WAIT, a dead session, a spam block: off at once.
		auto c = LaneCounters();
		auto v = AfterStep(StepKind::FloodWait, c, normal);
		CHECK(v.retire == RetireReason::FloodWait);
		CHECK(v.pause == PauseKind::Account);
		CHECK(AfterStep(StepKind::AccountDead, c, normal).retire
			== RetireReason::AccountDead);
		CHECK(AfterStep(StepKind::SpamBlocked, c, normal).retire
			== RetireReason::SpamBlock);
	}
	{
		// A send cut short counts as sent; what cut it decides the rest.
		auto c = LaneCounters();
		const auto many = LaneLimits{ .quota = 100 };
		CHECK(AfterStep(StepKind::Partial, c, many, StepKind::Failed).retire
			== RetireReason::None);
		CHECK(c.sent == 1);
		CHECK(AfterStep(StepKind::Partial, c, many, StepKind::FloodWait)
			.retire == RetireReason::FloodWait);
		CHECK(AfterStep(StepKind::Partial, c, many, StepKind::AccountDead)
			.retire == RetireReason::AccountDead);
		CHECK(c.sent == 3);
		auto two = LaneCounters{ .sent = 1 };
		CHECK(AfterStep(StepKind::Partial, two, normal).retire
			== RetireReason::Quota);
	}
	{
		// "Стоп": nothing counted, the lane does nothing more.
		auto c = LaneCounters();
		const auto v = AfterStep(StepKind::Aborted, c, normal);
		CHECK(v.retire == RetireReason::None);
		CHECK(v.pause == PauseKind::None);
		CHECK(c.sent == 0);
	}
	{
		// 0 in a limit: never for that reason.
		auto c = LaneCounters();
		const auto none = LaneLimits{ .quota = 0, .refusals = 0 };
		for (auto i = 0; i != 50; ++i) {
			CHECK(AfterStep(StepKind::Sent, c, none).retire
				== RetireReason::None);
			CHECK(AfterStep(StepKind::Refused, c, none).retire
				== RetireReason::None);
		}
	}
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
	TestAfterStep();
	if (failures) {
		std::fprintf(stderr, "%d check(s) failed\n", failures);
		return EXIT_FAILURE;
	}
	std::puts("sms_campaign_model_test: OK");
	return EXIT_SUCCESS;
}
