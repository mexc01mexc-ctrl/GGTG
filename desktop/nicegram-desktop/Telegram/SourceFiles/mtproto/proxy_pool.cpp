/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "mtproto/proxy_pool.h"

#include "core/ggtg_log.h"
#include "settings.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSaveFile>

namespace MTP {
namespace {

[[nodiscard]] QString PoolFilePath() {
	return cWorkingDir() + u"tdata/proxy_pool.json"_q;
}

} // namespace

ProxyPool::ProxyPool() {
	load();
}

const std::vector<ProxyList> &ProxyPool::lists() const {
	return _lists;
}

rpl::producer<> ProxyPool::changes() const {
	return _changes.events();
}

int ProxyPool::addList(const QString &name) {
	_lists.push_back(ProxyList{ .name = name });
	Core::GgtgLog::Info(u"Прокси"_q, u"Создан список \""_q + name + '"');
	save();
	return int(_lists.size()) - 1;
}

void ProxyPool::renameList(int listIndex, const QString &name) {
	if (listIndex < 0 || listIndex >= int(_lists.size())) {
		return;
	}
	if (_lists[listIndex].name == name) {
		return;
	}
	Core::GgtgLog::Info(
		u"Прокси"_q,
		u"Список \""_q + _lists[listIndex].name + u"\" переименован в \""_q
			+ name + '"');
	_lists[listIndex].name = name;
	save();
}

void ProxyPool::removeList(int listIndex) {
	if (listIndex < 0 || listIndex >= int(_lists.size())) {
		return;
	}
	Core::GgtgLog::Info(
		u"Прокси"_q,
		u"Удалён список \""_q + _lists[listIndex].name + u"\" ("_q
			+ QString::number(_lists[listIndex].entries.size())
			+ u" прокси)"_q);
	_lists.erase(_lists.begin() + listIndex);
	save();
}

int ProxyPool::addLines(int listIndex, const QString &text) {
	if (listIndex < 0 || listIndex >= int(_lists.size())) {
		return 0;
	}
	auto added = 0;
	auto &entries = _lists[listIndex].entries;
	for (const auto &rawLine : text.split('\n')) {
		const auto line = rawLine.trimmed();
		if (line.isEmpty()) {
			continue;
		}
		const auto parts = line.split(':');
		if (parts.size() < 2) {
			continue;
		}
		const auto port = parts[1].trimmed().toUShort();
		if (!port) {
			continue;
		}
		auto entry = ProxyData();
		entry.type = ProxyData::Type::Socks5;
		entry.host = parts[0].trimmed();
		entry.port = port;
		if (parts.size() > 2) {
			entry.user = parts[2].trimmed();
		}
		if (parts.size() > 3) {
			entry.password = QStringList(parts.mid(3)).join(':').trimmed();
		}
		entries.push_back(entry);
		++added;
	}
	if (added > 0) {
		Core::GgtgLog::Info(
			u"Прокси"_q,
			u"В список \""_q + _lists[listIndex].name + u"\" добавлено: "_q
				+ QString::number(added));
		save();
	}
	return added;
}

void ProxyPool::removeEntry(int listIndex, int entryIndex) {
	if (listIndex < 0 || listIndex >= int(_lists.size())) {
		return;
	}
	auto &entries = _lists[listIndex].entries;
	if (entryIndex < 0 || entryIndex >= int(entries.size())) {
		return;
	}
	Core::GgtgLog::Info(
		u"Прокси"_q,
		u"Удалён прокси "_q + entries[entryIndex].host + ':'
			+ QString::number(entries[entryIndex].port) + u" из \""_q
			+ _lists[listIndex].name + '"');
	entries.erase(entries.begin() + entryIndex);
	save();
}

std::optional<ProxyData> ProxyPool::take(int listIndex) {
	if (listIndex < 0 || listIndex >= int(_lists.size())) {
		return std::nullopt;
	}
	auto &entries = _lists[listIndex].entries;
	if (entries.empty()) {
		return std::nullopt;
	}
	auto result = entries.front();
	Core::GgtgLog::Info(
		u"Прокси"_q,
		u"Взят прокси "_q + result.host + ':' + QString::number(result.port)
			+ u" из \""_q + _lists[listIndex].name + u"\" (осталось "_q
			+ QString::number(entries.size() - 1) + ')');
	entries.erase(entries.begin());
	save();
	return result;
}

std::optional<ProxyData> ProxyPool::peek(int listIndex) const {
	if (listIndex < 0 || listIndex >= int(_lists.size())) {
		return std::nullopt;
	}
	const auto &entries = _lists[listIndex].entries;
	if (entries.empty()) {
		return std::nullopt;
	}
	return entries.front();
}

bool ProxyPool::removeMatching(const ProxyData &proxy) {
	for (auto &list : _lists) {
		for (auto i = list.entries.begin(); i != list.entries.end(); ++i) {
			if (*i == proxy) {
				Core::GgtgLog::Info(
					u"Прокси"_q,
					u"Использованный прокси "_q + i->host + ':'
						+ QString::number(i->port) + u" убран из \""_q
						+ list.name + '"');
				list.entries.erase(i);
				save();
				return true;
			}
		}
	}
	return false;
}

void ProxyPool::load() {
	auto file = QFile(PoolFilePath());
	if (!file.open(QIODevice::ReadOnly)) {
		return;
	}
	const auto data = file.readAll();
	file.close();
	const auto json = QJsonDocument::fromJson(data);
	if (!json.isArray() && !json.isObject()) {
		if (!data.trimmed().isEmpty()) {
			// Unreadable: keep a copy before the next save() replaces it,
			// instead of silently losing the whole pool.
			const auto backup = PoolFilePath() + u".corrupt"_q;
			QFile::remove(backup);
			QFile::copy(PoolFilePath(), backup);
			Core::GgtgLog::Error(
				u"Прокси"_q,
				u"файл пула повреждён, копия: "_q + backup);
		}
		return;
	}
	// Accept both the new array-of-lists format and, defensively, the
	// old single-list {"name":..,"entries":[..]} format from before
	// named lists existed, so any pool saved with the previous build
	// isn't silently dropped.
	const auto readList = [](const QJsonObject &object) {
		auto list = ProxyList{ .name = object["name"].toString() };
		for (const auto &value : object["entries"].toArray()) {
			const auto item = value.toObject();
			auto entry = ProxyData();
			entry.type = ProxyData::Type::Socks5;
			entry.host = item["host"].toString();
			entry.port = quint16(item["port"].toInt());
			entry.user = item["user"].toString();
			entry.password = item["password"].toString();
			if (!entry.host.isEmpty() && entry.port) {
				list.entries.push_back(entry);
			}
		}
		return list;
	};
	if (json.isArray()) {
		for (const auto &value : json.array()) {
			_lists.push_back(readList(value.toObject()));
		}
	} else {
		_lists.push_back(readList(json.object()));
	}
}

void ProxyPool::save() const {
	auto array = QJsonArray();
	for (const auto &list : _lists) {
		auto entries = QJsonArray();
		for (const auto &entry : list.entries) {
			entries.push_back(QJsonObject{
				{ "host", entry.host },
				{ "port", int(entry.port) },
				{ "user", entry.user },
				{ "password", entry.password },
			});
		}
		array.push_back(QJsonObject{
			{ "name", list.name },
			{ "entries", entries },
		});
	}
	const auto path = PoolFilePath();
	QDir().mkpath(QFileInfo(path).absolutePath());
	// QSaveFile writes to a temporary file and renames it over the old one
	// on commit, so a crash or power loss mid-write can't empty the pool.
	auto file = QSaveFile(path);
	if (!file.open(QIODevice::WriteOnly)) {
		Core::GgtgLog::Error(
			u"Прокси"_q,
			u"не удалось открыть файл пула для записи"_q);
	} else {
		file.write(QJsonDocument(array).toJson(QJsonDocument::Compact));
		if (!file.commit()) {
			Core::GgtgLog::Error(
				u"Прокси"_q,
				u"не удалось сохранить пул прокси"_q);
		}
	}
	_changes.fire({});
}

QByteArray ProxyPool::toJson() const {
	auto array = QJsonArray();
	for (const auto &list : _lists) {
		auto entries = QJsonArray();
		for (const auto &entry : list.entries) {
			entries.push_back(QJsonObject{
				{ "host", entry.host },
				{ "port", int(entry.port) },
				{ "user", entry.user },
				{ "password", entry.password },
			});
		}
		array.push_back(QJsonObject{
			{ "name", list.name },
			{ "entries", entries },
		});
	}
	return QJsonDocument(array).toJson(QJsonDocument::Compact);
}

int ProxyPool::mergeJson(const QByteArray &data) {
	const auto doc = QJsonDocument::fromJson(data);
	if (!doc.isArray()) {
		return 0;
	}
	auto addedCount = 0;
	for (const auto &lVal : doc.array()) {
		const auto lObj = lVal.toObject();
		const auto name = lObj.value("name").toString().trimmed();
		if (name.isEmpty()) {
			continue;
		}
		auto listIt = std::find_if(_lists.begin(), _lists.end(), [&](const ProxyList &l) {
			return l.name.compare(name, Qt::CaseInsensitive) == 0;
		});
		if (listIt == _lists.end()) {
			_lists.push_back({ .name = name });
			listIt = _lists.end() - 1;
		}
		for (const auto &eVal : lObj.value("entries").toArray()) {
			const auto item = eVal.toObject();
			const auto host = item.value("host").toString().trimmed();
			const auto port = quint16(item.value("port").toInt());
			const auto user = item.value("user").toString();
			const auto pass = item.value("password").toString();
			if (host.isEmpty() || !port) {
				continue;
			}
			const auto exists = std::any_of(listIt->entries.begin(), listIt->entries.end(), [&](const ProxyData &p) {
				return p.host == host && p.port == port && p.user == user && p.password == pass;
			});
			if (!exists) {
				auto entry = ProxyData();
				entry.type = ProxyData::Type::Socks5;
				entry.host = host;
				entry.port = port;
				entry.user = user;
				entry.password = pass;
				listIt->entries.push_back(entry);
				++addedCount;
			}
		}
	}
	if (addedCount > 0) {
		save();
	}
	return addedCount;
}

ProxyPool &GlobalProxyPool() {
	static auto instance = ProxyPool();
	return instance;
}

ProxyData UnreachableProxyPlaceholder() {
	auto result = ProxyData();
	result.type = ProxyData::Type::Socks5;
	result.host = u"127.0.0.1"_q;
	result.port = 1;
	return result;
}

} // namespace MTP
