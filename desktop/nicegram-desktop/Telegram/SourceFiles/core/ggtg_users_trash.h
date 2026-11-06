/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and exceptions, see https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "core/ggtg_paths.h"

#include <QtCore/QDateTime>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSaveFile>
#include <QtCore/QStringList>

#include <vector>

namespace Core::GgtgUsersTrash {

// «Корзина пользователей»: the people the user threw out of a base by hand (the ✕ in the opened list) - what the
// user counts as rubbish and fakes. The detector learns from them (see Ggtg::FakeDetector::LearnFromMarks), and a
// person can be put back.

struct Entry {
	QString username; // lowercase, without @
	QString name;
	quint64 id = 0;
	QString lastseenText;
	QString baseTitle; // the base the person was thrown out of
	qint64 at = 0;
};

[[nodiscard]] inline QString Path() {
	return GgtgPaths::DbDir() + u"users_trash.json"_q;
}

[[nodiscard]] inline std::vector<Entry> &Cache() {
	static auto result = [] {
		auto list = std::vector<Entry>();
		auto file = QFile(Path());
		if (file.open(QIODevice::ReadOnly)) {
			for (const auto &value : QJsonDocument::fromJson(file.readAll()).array()) {
				const auto o = value.toObject();
				list.push_back({
					.username = o["username"].toString(),
					.name = o["name"].toString(),
					.id = o["id"].toString().toULongLong(),
					.lastseenText = o["lastseen"].toString(),
					.baseTitle = o["base"].toString(),
					.at = o["at"].toString().toLongLong(),
				});
			}
		}
		return list;
	}();
	return result;
}

inline void Save() {
	auto array = QJsonArray();
	for (const auto &e : Cache()) {
		array.append(QJsonObject{
			{ "username", e.username },
			{ "name", e.name },
			{ "id", QString::number(e.id) },
			{ "lastseen", e.lastseenText },
			{ "base", e.baseTitle },
			{ "at", QString::number(e.at) },
		});
	}
	QDir().mkpath(QFileInfo(Path()).absolutePath());
	auto file = QSaveFile(Path());
	if (file.open(QIODevice::WriteOnly)) {
		file.write(QJsonDocument(array).toJson(QJsonDocument::Compact));
		file.commit();
	}
}

[[nodiscard]] inline int Count() {
	return int(Cache().size());
}

inline void Add(Entry entry) {
	entry.username = entry.username.trimmed().remove('@').toLower();
	if (entry.username.isEmpty()) {
		return;
	}
	if (!entry.at) {
		entry.at = QDateTime::currentSecsSinceEpoch();
	}
	auto &list = Cache();
	for (auto &e : list) {
		if (e.username == entry.username) {
			e = entry;
			Save();
			return;
		}
	}
	list.push_back(entry);
	Save();
}

inline void Remove(const QString &username) {
	auto &list = Cache();
	const auto key = username.trimmed().remove('@').toLower();
	for (auto i = list.begin(); i != list.end(); ++i) {
		if (i->username == key) {
			list.erase(i);
			Save();
			return;
		}
	}
}

inline void Clear() {
	Cache().clear();
	Save();
}

} // namespace Core::GgtgUsersTrash
