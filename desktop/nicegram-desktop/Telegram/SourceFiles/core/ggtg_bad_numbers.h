/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and exceptions, see https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "core/ggtg_paths.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QSaveFile>
#include <QtCore/QSet>

// Numbers that already failed in a registration (taken, banned, invalid, a code to the app, a paid SMS...): kept
// between runs so they are never paid for twice, and shared with the phone through the sync ("bad_numbers").
namespace Core::GgtgBadNumbers {

constexpr auto kMaxKept = 50000;

[[nodiscard]] inline QString Path() {
	return Core::GgtgPaths::DbDir() + u"registration_bad_numbers.json"_q;
}

[[nodiscard]] inline QSet<QString> &Set() {
	static auto set = [] {
		auto result = QSet<QString>();
		auto file = QFile(Path());
		if (file.open(QIODevice::ReadOnly)) {
			const auto doc = QJsonDocument::fromJson(file.readAll());
			for (const auto &value : doc.array()) {
				result.insert(value.toString());
			}
		}
		return result;
	}();
	return set;
}

[[nodiscard]] inline QJsonArray ToJson() {
	auto array = QJsonArray();
	for (const auto &number : Set()) {
		array.push_back(number);
	}
	return array;
}

inline void Save() {
	QDir().mkpath(Core::GgtgPaths::DbDir());
	auto file = QSaveFile(Path());
	if (!file.open(QIODevice::WriteOnly)) {
		return;
	}
	file.write(QJsonDocument(ToJson()).toJson(QJsonDocument::Compact));
	file.commit();
}

[[nodiscard]] inline bool Contains(const QString &phone) {
	return Set().contains(phone);
}

inline bool Add(const QString &phone) {
	if (phone.isEmpty() || Set().contains(phone) || Set().size() >= kMaxKept) {
		return false;
	}
	Set().insert(phone);
	Save();
	return true;
}

// Adds what another device knows; returns how many were new.
inline int Merge(const QJsonArray &remote) {
	auto added = 0;
	for (const auto &value : remote) {
		const auto phone = value.toString();
		if (phone.isEmpty() || Set().contains(phone) || Set().size() >= kMaxKept) {
			continue;
		}
		Set().insert(phone);
		++added;
	}
	if (added) {
		Save();
	}
	return added;
}

} // namespace Core::GgtgBadNumbers
