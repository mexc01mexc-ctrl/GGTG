/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and exceptions, see https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "core/ggtg_paths.h"

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSaveFile>
#include <QtCore/QString>

#include <algorithm>
#include <utility>
#include <vector>

namespace Core::GgtgLists {

// The user's own lists of chats ("Заинтересованные", "Подумать", "Клиенты"):
// a chat is put into a list from its right-click menu, and the GGTG window's
// "Списки чатов" opens any listed chat in one click, in the account it was
// put from. Kept in tdata/ggtg_db/chat_lists.json.

struct Entry {
	quint64 account = 0; // the user id of the account the chat is in
	quint64 peer = 0; // PeerId value
	QString username;
	QString name;
	QString accountName;
	qint64 at = 0; // when it was put in
};

struct List {
	QString id;
	QString name;
	std::vector<Entry> entries;
};

namespace details {

struct Store {
	std::vector<List> lists;
	bool loaded = false;
	int version = 0;
};

[[nodiscard]] inline QString Path() {
	return Core::GgtgPaths::DbDir() + u"chat_lists.json"_q;
}

[[nodiscard]] inline Store &State() {
	static auto result = Store();
	if (!std::exchange(result.loaded, true)) {
		auto file = QFile(Path());
		if (file.open(QIODevice::ReadOnly)) {
			const auto array = QJsonDocument::fromJson(file.readAll())
				.object().value("lists").toArray();
			for (const auto &value : array) {
				const auto o = value.toObject();
				auto list = List{
					.id = o.value("id").toString(),
					.name = o.value("name").toString(),
				};
				for (const auto &item : o.value("entries").toArray()) {
					const auto e = item.toObject();
					list.entries.push_back({
						.account = e.value("account").toString()
							.toULongLong(),
						.peer = e.value("peer").toString().toULongLong(),
						.username = e.value("username").toString(),
						.name = e.value("name").toString(),
						.accountName = e.value("accountName").toString(),
						.at = qint64(e.value("at").toDouble()),
					});
				}
				if (!list.id.isEmpty()) {
					result.lists.push_back(std::move(list));
				}
			}
		}
	}
	return result;
}

} // namespace details

[[nodiscard]] inline std::vector<List> &Lists() {
	return details::State().lists;
}

// Grows on every change: a window that shows the lists rebuilds on it.
[[nodiscard]] inline int Version() {
	return details::State().version;
}

inline void Save() {
	auto &state = details::State();
	++state.version;
	auto lists = QJsonArray();
	for (const auto &list : state.lists) {
		auto entries = QJsonArray();
		for (const auto &entry : list.entries) {
			entries.push_back(QJsonObject{
				{ "account", QString::number(entry.account) },
				{ "peer", QString::number(entry.peer) },
				{ "username", entry.username },
				{ "name", entry.name },
				{ "accountName", entry.accountName },
				{ "at", double(entry.at) },
			});
		}
		lists.push_back(QJsonObject{
			{ "id", list.id },
			{ "name", list.name },
			{ "entries", entries },
		});
	}
	QDir().mkpath(Core::GgtgPaths::DbDir());
	auto file = QSaveFile(details::Path());
	if (file.open(QIODevice::WriteOnly)) {
		file.write(QJsonDocument(QJsonObject{
			{ "lists", lists },
		}).toJson(QJsonDocument::Compact));
		file.commit();
	}
}

[[nodiscard]] inline List *Find(const QString &id) {
	for (auto &list : Lists()) {
		if (list.id == id) {
			return &list;
		}
	}
	return nullptr;
}

// A new list; returns its id (empty when the name is empty).
inline QString Create(const QString &name) {
	const auto trimmed = name.trimmed().left(60);
	if (trimmed.isEmpty()) {
		return QString();
	}
	auto id = QString::number(QDateTime::currentMSecsSinceEpoch(), 36);
	while (Find(id)) {
		id += 'x';
	}
	Lists().push_back({ .id = id, .name = trimmed });
	Save();
	return id;
}

inline void Rename(const QString &id, const QString &name) {
	const auto trimmed = name.trimmed().left(60);
	if (const auto list = Find(id); list && !trimmed.isEmpty()) {
		list->name = trimmed;
		Save();
	}
}

inline void Delete(const QString &id) {
	auto &lists = Lists();
	lists.erase(
		std::remove_if(lists.begin(), lists.end(), [&](const List &list) {
			return list.id == id;
		}),
		lists.end());
	Save();
}

[[nodiscard]] inline bool Has(
		const QString &listId,
		quint64 account,
		quint64 peer) {
	if (const auto list = Find(listId)) {
		for (const auto &entry : list->entries) {
			if (entry.account == account && entry.peer == peer) {
				return true;
			}
		}
	}
	return false;
}

inline void Add(const QString &listId, Entry entry) {
	const auto list = Find(listId);
	if (!list || Has(listId, entry.account, entry.peer)) {
		return;
	}
	entry.at = QDateTime::currentSecsSinceEpoch();
	list->entries.push_back(std::move(entry));
	Save();
}

inline void Remove(const QString &listId, quint64 account, quint64 peer) {
	if (const auto list = Find(listId)) {
		auto &entries = list->entries;
		entries.erase(
			std::remove_if(entries.begin(), entries.end(), [&](
					const Entry &entry) {
				return entry.account == account && entry.peer == peer;
			}),
			entries.end());
		Save();
	}
}

inline void MergeJson(const QByteArray &data) {
	const auto doc = QJsonDocument::fromJson(data);
	if (!doc.isObject()) {
		return;
	}
	const auto incomingLists = doc.object().value("lists").toArray();
	auto &localLists = Lists();
	for (const auto &lVal : incomingLists) {
		const auto lObj = lVal.toObject();
		const auto id = lObj.value("id").toString();
		const auto name = lObj.value("name").toString().trimmed();
		if (name.isEmpty()) {
			continue;
		}
		auto local = Find(id);
		if (!local) {
			for (auto &existing : localLists) {
				if (existing.name.compare(name, Qt::CaseInsensitive) == 0) {
					local = &existing;
					break;
				}
			}
		}
		if (!local) {
			localLists.push_back({ .id = id.isEmpty() ? Create(name) : id, .name = name });
			local = &localLists.back();
		}
		for (const auto &item : lObj.value("entries").toArray()) {
			const auto e = item.toObject();
			const auto account = e.value("account").toString().toULongLong();
			const auto peer = (e.contains("dialog") ? e.value("dialog") : e.value("peer")).toString().toULongLong();
			if (!account || !peer) {
				continue;
			}
			auto it = std::find_if(local->entries.begin(), local->entries.end(), [&](const Entry &existing) {
				return existing.account == account && existing.peer == peer;
			});
			const auto remoteAt = qint64(e.value("at").toDouble());
			if (it == local->entries.end()) {
				local->entries.push_back({
					.account = account,
					.peer = peer,
					.username = e.value("username").toString(),
					.name = e.value("name").toString(),
					.accountName = e.value("accountName").toString(),
					.at = remoteAt,
				});
			} else if (remoteAt > it->at) {
				it->username = e.value("username").toString();
				it->name = e.value("name").toString();
				it->accountName = e.value("accountName").toString();
				it->at = remoteAt;
			}
		}
	}
	Save();
}

} // namespace Core::GgtgLists
