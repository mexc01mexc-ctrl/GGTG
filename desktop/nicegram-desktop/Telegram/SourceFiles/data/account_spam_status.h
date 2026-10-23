/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtCore/QString>

#include <optional>

namespace Data {

// Per-account record of Telegram's own anti-spam limit, kept in the same
// "tdata/ggtg_db/" directory as the recipient-side SMS tracking
// (see scraped_members_db.h) but keyed by the account's own user id, not
// by recipient - a PEER_FLOOD tells us the *account* is limited, not the
// specific recipient we were writing to at the time (see
// settings/sms_campaign.cpp, where hitting PEER_FLOOD triggers a check via
// @spambot instead of just recording the recipient).
enum class AccountSpamState {
	Clear,
	Temporary, // limited until `until`
	Permanent,
};

struct AccountSpamStatus {
	AccountSpamState state = AccountSpamState::Clear;
	qint64 until = 0; // unix seconds, only meaningful for Temporary
	qint64 checkedAt = 0; // unix seconds
};

[[nodiscard]] std::optional<AccountSpamStatus> FindAccountSpamStatus(
	uint64 userId);
void SetAccountSpamStatus(uint64 userId, AccountSpamStatus status);

// Fires the account's user id every time SetAccountSpamStatus changes its
// record, so a visible account row can refresh itself reactively instead
// of the caller having to know which UI, if any, is currently showing
// that account.
[[nodiscard]] rpl::producer<uint64> AccountSpamStatusChanges();

// Fires with the user id each time that account's "sent today" counter grows.
[[nodiscard]] rpl::producer<uint64> SmsSentTodayChanges();

// A same-day tally of PEER_FLOOD warnings seen while sending, used by the
// SMS campaign engine's "Пропуск спам-блок" tolerance (see
// settings/sms_campaign.cpp) to decide when an account is *really* done
// for today rather than retiring it on the very first warning. Resets
// itself the first time it's called on a new calendar day. Deliberately a
// separate store from AccountSpamStatus above: that one persists until the
// next @spambot check overwrites it, so sharing storage would let a fresh
// Clear verdict silently wipe today's warning count too.
[[nodiscard]] int RegisterSpamWarning(uint64 userId);
// @spambot said the account is free: today's refusals no longer count
// against it.
void RegisterSpamWarningReset(uint64 userId);

// A tally of the messages the SMS campaign actually delivered from an
// account in the last 24 hours (a rolling window, "SMS 24h"), kept on disk
// so the "Лимит в сутки" survives a restart (and a second campaign).
[[nodiscard]] int CountSmsSentToday(uint64 userId);
// Adds one delivered message and returns today's new total.
int RegisterSmsSentToday(uint64 userId);
// A message typed by hand in an ordinary chat (e.g. after opening a
// username from a base): counts once per recipient per day for the sending
// account, like one delivered campaign message. Returns whether it counted.
bool RegisterManualSmsSent(uint64 senderUserId, uint64 recipientId);

} // namespace Data
