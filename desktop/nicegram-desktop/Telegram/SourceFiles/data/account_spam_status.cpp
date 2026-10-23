/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "data/account_spam_status.h"
#include "core/ggtg_paths.h"

#include "base/unixtime.h"
#include "settings.h"

#include <set>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSaveFile>

namespace Data {
namespace {

// Same directory as scraped_members_db.h's bases and SMS log, kept as its
// own file since this is keyed by account, not by recipient.
[[nodiscard]] QString StatusPath() {
	return Core::GgtgPaths::DbDir() + u"account_spam_status.json"_q;
}

[[nodiscard]] QString DailySendsPath() {
	return Core::GgtgPaths::DbDir() + u"account_sms_daily.json"_q;
}

// A separate file from StatusPath above - see RegisterSpamWarning's
// comment in the header for why the two must not share one JSON entry.
[[nodiscard]] QString WarningsPath() {
	return Core::GgtgPaths::DbDir() + u"account_spam_warnings.json"_q;
}

[[nodiscard]] QJsonDocument ReadJson(const QString &path) {
	auto file = QFile(path);
	if (!file.open(QIODevice::ReadOnly)) {
		return QJsonDocument();
	}
	return QJsonDocument::fromJson(file.readAll());
}

void WriteJson(const QString &path, const QJsonDocument &document) {
	QDir().mkpath(QFileInfo(path).absolutePath());
	auto file = QSaveFile(path);
	if (file.open(QIODevice::WriteOnly)) {
		file.write(document.toJson(QJsonDocument::Compact));
		file.commit();
	}
}

[[nodiscard]] QString StateToString(AccountSpamState state) {
	switch (state) {
	case AccountSpamState::Temporary: return u"temporary"_q;
	case AccountSpamState::Permanent: return u"permanent"_q;
	default: return u"clear"_q;
	}
}

[[nodiscard]] AccountSpamState StateFromString(const QString &raw) {
	if (raw == u"temporary"_q) {
		return AccountSpamState::Temporary;
	} else if (raw == u"permanent"_q) {
		return AccountSpamState::Permanent;
	}
	return AccountSpamState::Clear;
}

[[nodiscard]] rpl::event_stream<uint64> &Changes() {
	static auto result = rpl::event_stream<uint64>();
	return result;
}

} // namespace

std::optional<AccountSpamStatus> FindAccountSpamStatus(uint64 userId) {
	const auto key = QString::number(userId);
	const auto object = ReadJson(StatusPath()).object();
	if (!object.contains(key)) {
		return std::nullopt;
	}
	const auto entry = object[key].toObject();
	return AccountSpamStatus{
		.state = StateFromString(entry["state"].toString()),
		.until = entry["until"].toString().toLongLong(),
		.checkedAt = entry["checkedAt"].toString().toLongLong(),
	};
}

void SetAccountSpamStatus(uint64 userId, AccountSpamStatus status) {
	const auto key = QString::number(userId);
	auto object = ReadJson(StatusPath()).object();
	// A Clear verdict is saved too, not dropped - otherwise there would be
	// no way to tell "@spambot was actually asked and said this account is
	// fine" apart from "nobody has ever checked" (both would read back as
	// no entry at all). The badges that show this (see AccountSpamStatusText
	// in settings/sms_campaign.cpp) need exactly that distinction: gray
	// "Статус" (never checked, click to check) versus green "Свободен"
	// (checked, clear).
	object[key] = QJsonObject{
		{ "state", StateToString(status.state) },
		{ "until", QString::number(status.until) },
		{
			"checkedAt",
			QString::number(status.checkedAt
				? status.checkedAt
				: qint64(base::unixtime::now())),
		},
	};
	WriteJson(StatusPath(), QJsonDocument(object));
	Changes().fire_copy(userId);
}

namespace {

[[nodiscard]] rpl::event_stream<uint64> &SentTodayChanges() {
	static auto result = rpl::event_stream<uint64>();
	return result;
}

} // namespace

rpl::producer<uint64> SmsSentTodayChanges() {
	return SentTodayChanges().events();
}

rpl::producer<uint64> AccountSpamStatusChanges() {
	return Changes().events();
}

int RegisterSpamWarning(uint64 userId) {
	const auto key = QString::number(userId);
	auto object = ReadJson(WarningsPath()).object();
	const auto today = qint64(base::unixtime::now()) / 86400;
	const auto entry = object[key].toObject();
	auto day = entry["day"].toString().toLongLong();
	auto count = entry["count"].toInt();
	if (day != today) {
		day = today;
		count = 0;
	}
	++count;
	object[key] = QJsonObject{
		{ "day", QString::number(day) },
		{ "count", count },
	};
	WriteJson(WarningsPath(), QJsonDocument(object));
	return count;
}

void RegisterSpamWarningReset(uint64 userId) {
	auto object = ReadJson(WarningsPath()).object();
	const auto key = QString::number(userId);
	if (object.contains(key)) {
		object.remove(key);
		WriteJson(WarningsPath(), QJsonDocument(object));
	}
}

namespace {

// The send times (unix seconds) of one account that fall into the last 24
// hours; anything older is ignored.
[[nodiscard]] QJsonArray RecentSmsTimes(const QJsonObject &entry, qint64 now) {
	auto result = QJsonArray();
	// The older per-calendar-day format ({day, count}) has no send times: its
	// sends count until that (UTC) day ends, as they did before.
	if (!entry.contains("times") && entry.contains("day")) {
		const auto day = entry["day"].toString().toLongLong();
		if ((day + 1) * 86400 > now) {
			for (auto i = 0, count = entry["count"].toInt(); i < count; ++i) {
				result.append(double(day * 86400));
			}
		}
		return result;
	}
	for (const auto &value : entry["times"].toArray()) {
		if (qint64(value.toDouble()) > now - 86400) {
			result.append(value);
		}
	}
	return result;
}

} // namespace

int CountSmsSentToday(uint64 userId) {
	const auto entry = ReadJson(DailySendsPath()).object()[
		QString::number(userId)].toObject();
	return RecentSmsTimes(entry, qint64(base::unixtime::now())).size();
}

int RegisterSmsSentToday(uint64 userId) {
	const auto key = QString::number(userId);
	auto object = ReadJson(DailySendsPath()).object();
	const auto now = qint64(base::unixtime::now());
	auto times = RecentSmsTimes(object[key].toObject(), now);
	times.append(double(now));
	const auto count = int(times.size());
	object[key] = QJsonObject{ { "times", times } };
	WriteJson(DailySendsPath(), QJsonDocument(object));
	SentTodayChanges().fire_copy(userId);
	return count;
}

bool RegisterManualSmsSent(uint64 senderUserId, uint64 recipientId) {
	static auto day = qint64(0);
	static auto seen = std::set<std::pair<uint64, uint64>>();
	const auto today = qint64(base::unixtime::now()) / 86400;
	if (day != today) {
		day = today;
		seen.clear();
	}
	if (!seen.emplace(senderUserId, recipientId).second) {
		return false;
	}
	RegisterSmsSentToday(senderUserId);
	return true;
}

} // namespace Data
