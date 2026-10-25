/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/ggtg_sync.h"

#include "base/unixtime.h"
#include "core/ggtg_log.h"
#include "core/ggtg_paths.h"
#include "core/ggtg_chat_lists.h"
#include "mtproto/proxy_pool.h"
#include "data/scraped_members_db.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QRandomGenerator>
#include <QtCore/QSaveFile>

#include <map>

namespace Core::GgtgSync {
namespace {

constexpr auto kFormat = 1;
constexpr auto kMaxFileSize = 30 * 1024 * 1024;

// Small files with the same shape on every platform: whichever was changed
// later wins as a whole. ai_knowledge/*.json (the "Материалы") is added to
// them.
[[nodiscard]] QStringList PlainFiles() {
	return {
		u"ai_profile.json"_q,
		u"ai_examples.json"_q,
		u"ai_playbook.json"_q,
		u"ai_training.json"_q,
		u"ai_settings.json"_q,
		u"chat_lists.json"_q,
	};
}

[[nodiscard]] QString SettingsPath() {
	return GgtgPaths::DbDir() + u"sync_settings.json"_q;
}

[[nodiscard]] QString NewDeviceId() {
	auto bytes = QByteArray(8, 0);
	QRandomGenerator::system()->fillRange(
		reinterpret_cast<quint32*>(bytes.data()),
		bytes.size() / 4);
	return QString::fromLatin1(bytes.toHex());
}

[[nodiscard]] QByteArray ReadAll(const QString &path) {
	auto file = QFile(path);
	if (!file.open(QIODevice::ReadOnly)) {
		return {};
	}
	return file.readAll();
}

void WriteAtomic(const QString &path, const QByteArray &data, qint64 mtime) {
	QDir().mkpath(QFileInfo(path).absolutePath());
	auto file = QSaveFile(path);
	if (!file.open(QIODevice::WriteOnly)) {
		return;
	}
	file.write(data);
	if (!file.commit()) {
		return;
	}
	if (mtime > 0) {
		auto stamp = QFile(path);
		if (stamp.open(QIODevice::ReadWrite)) {
			stamp.setFileTime(
				QDateTime::fromSecsSinceEpoch(mtime),
				QFileDevice::FileModificationTime);
		}
	}
}

[[nodiscard]] qint64 Int(const QJsonValue &value) {
	return value.isString()
		? value.toString().toLongLong()
		: qint64(value.toDouble());
}

[[nodiscard]] quint64 UInt(const QJsonValue &value) {
	return value.isString()
		? value.toString().toULongLong()
		: quint64(value.toDouble());
}

[[nodiscard]] QString StatusText(Data::SmsSendStatus status) {
	switch (status) {
	case Data::SmsSendStatus::SpamAcc: return u"spam_acc"_q;
	case Data::SmsSendStatus::NotFound: return u"not_found"_q;
	case Data::SmsSendStatus::Restricted: return u"restricted"_q;
	case Data::SmsSendStatus::PeerLimit: return u"peer_limit"_q;
	case Data::SmsSendStatus::AccountFail: return u"account_fail"_q;
	case Data::SmsSendStatus::SuspectedFake: return u"suspected_fake"_q;
	case Data::SmsSendStatus::LongAgo: return u"long_ago"_q;
	case Data::SmsSendStatus::Sent: break;
	}
	return u"ok"_q;
}

[[nodiscard]] Data::SmsSendStatus StatusFromText(const QString &raw) {
	if (raw == u"spam_acc"_q) {
		return Data::SmsSendStatus::SpamAcc;
	} else if (raw == u"not_found"_q) {
		return Data::SmsSendStatus::NotFound;
	} else if (raw == u"restricted"_q) {
		return Data::SmsSendStatus::Restricted;
	} else if (raw == u"peer_limit"_q) {
		return Data::SmsSendStatus::PeerLimit;
	} else if (raw == u"account_fail"_q) {
		return Data::SmsSendStatus::AccountFail;
	} else if (raw == u"suspected_fake"_q) {
		return Data::SmsSendStatus::SuspectedFake;
	} else if (raw == u"long_ago"_q) {
		return Data::SmsSendStatus::LongAgo;
	}
	return Data::SmsSendStatus::Sent;
}

void CollectFiles(QJsonObject &files) {
	const auto dir = GgtgPaths::DbDir();
	auto names = PlainFiles();
	for (const auto &name : QDir(dir + u"ai_knowledge/"_q).entryList(
			{ u"*.json"_q },
			QDir::Files)) {
		names.push_back(u"ai_knowledge/"_q + name);
	}
	for (const auto &name : names) {
		const auto info = QFileInfo(dir + name);
		if (!info.exists() || info.size() > kMaxFileSize) {
			continue;
		}
		const auto data = ReadAll(dir + name);
		if (data.isEmpty()) {
			continue;
		}
		files.insert(name, QJsonObject{
			{ "mtime", QString::number(info.lastModified().toSecsSinceEpoch()) },
			{ "text", QString::fromUtf8(data) },
		});
	}
}

[[nodiscard]] bool SafeFileName(const QString &name) {
	if (name.contains(u".."_q) || name.startsWith('/')) {
		return false;
	}
	if (PlainFiles().contains(name)) {
		return true;
	}
	return name.startsWith(u"ai_knowledge/"_q)
		&& name.endsWith(u".json"_q)
		&& name.count('/') == 1;
}

} // namespace

Settings LoadSettings() {
	auto result = Settings();
	const auto object = QJsonDocument::fromJson(
		ReadAll(SettingsPath())).object();
	result.enabled = object["enabled"].toBool();
	result.mainUserId = object["mainUserId"].toString();
	result.passphrase = object["passphrase"].toString();
	result.deviceId = object["deviceId"].toString();
	result.lastSyncAt = Int(object["lastSyncAt"]);
	result.lastResult = object["lastResult"].toString();
	result.lastUploadedHash = object["lastUploadedHash"].toString();
	const auto seen = object["seen"].toObject();
	for (auto it = seen.begin(); it != seen.end(); ++it) {
		result.seen[it.key()] = Int(it.value());
	}
	if (result.deviceId.isEmpty()) {
		result.deviceId = NewDeviceId();
		SaveSettings(result);
	}
	return result;
}

void SaveSettings(const Settings &settings) {
	auto seen = QJsonObject();
	for (const auto &[device, id] : settings.seen) {
		seen.insert(device, QString::number(id));
	}
	const auto object = QJsonObject{
		{ "seen", seen },
		{ "enabled", settings.enabled },
		{ "mainUserId", settings.mainUserId },
		{ "passphrase", settings.passphrase },
		{ "deviceId", settings.deviceId },
		{ "lastSyncAt", QString::number(settings.lastSyncAt) },
		{ "lastResult", settings.lastResult },
		{ "lastUploadedHash", settings.lastUploadedHash },
	};
	WriteAtomic(
		SettingsPath(),
		QJsonDocument(object).toJson(QJsonDocument::Compact),
		0);
}

Snapshot BuildSnapshot(const Settings &settings) {
	auto bases = QJsonArray();
	for (const auto &meta : Data::LoadScrapedGroupsIndex()) {
		auto items = QJsonArray();
		if (meta.kind == Data::BaseKind::Groups) {
			for (const auto &entry : Data::LoadScrapedGroupEntries(meta.id)) {
				items.append(QJsonObject{
					{ "username", entry.username },
					{ "title", entry.title },
					{ "membersCount", entry.membersCount },
					{ "link", entry.link },
					{ "checked", entry.checked },
				});
			}
		} else {
			for (const auto &name : Data::LoadScrapedGroupUsernames(meta.id)) {
				items.append(name);
			}
		}
		bases.append(QJsonObject{
			{ "id", QString::number(meta.id) },
			{ "kind", (meta.kind == Data::BaseKind::Groups)
				? u"groups"_q
				: u"users"_q },
			{ "title", meta.title },
			{ "savedAt", QString::number(meta.savedAt) },
			{ "items", items },
		});
	}

	auto sent = QJsonObject();
	for (const auto &record : Data::LoadAllSmsSendRecords()) {
		auto object = QJsonObject{
			{ "status", StatusText(record.status) },
			{ "account", record.account },
			{ "at", QString::number(record.at) },
			{ "base", QString::number(record.baseId) },
		};
		if (!record.source.isEmpty()) {
			object.insert("source", record.source);
		}
		if (!record.note.isEmpty()) {
			object.insert("note", record.note);
		}
		sent.insert(record.username, object);
	}

	auto tomb = QJsonObject();
	for (const auto &[key, at] : Data::LoadTombstones()) {
		tomb.insert(key, QString::number(at));
	}

	auto files = QJsonObject();
	CollectFiles(files);

	auto body = QJsonObject{
		{ "format", kFormat },
		{ "bases", bases },
		{ "sent", sent },
		{ "tomb", tomb },
		{ "files", files },
	};
	const auto poolData = MTP::GlobalProxyPool().toJson();
	const auto poolDoc = QJsonDocument::fromJson(poolData);
	if (poolDoc.isArray()) {
		body.insert("proxy_pools", poolDoc.array());
	}
	auto result = Snapshot();
	// The hash covers the contents only, not who made it and when.
	result.hash = QString::fromLatin1(QCryptographicHash::hash(
		QJsonDocument(body).toJson(QJsonDocument::Compact),
		QCryptographicHash::Sha256).toHex());
	body.insert("device", settings.deviceId);
	body.insert("deviceName", u"Mac"_q);
	body.insert("createdAt", QString::number(base::unixtime::now()));
	result.json = QJsonDocument(body).toJson(QJsonDocument::Compact);
	return result;
}

MergeResult MergeSnapshot(const QByteArray &json) {
	auto result = MergeResult();
	auto error = QJsonParseError();
	const auto document = QJsonDocument::fromJson(json, &error);
	if (error.error != QJsonParseError::NoError || !document.isObject()) {
		result.error = u"Файл синхронизации повреждён"_q;
		return result;
	}
	const auto root = document.object();
	if (root["format"].toInt() > kFormat) {
		result.error = u"Файл сделан более новой версией GGTG - "
			"обновите приложение"_q;
		return result;
	}

	// What was deleted on purpose, from both sides.
	auto remoteTomb = std::map<QString, qint64>();
	const auto tombObject = root["tomb"].toObject();
	for (auto it = tombObject.begin(); it != tombObject.end(); ++it) {
		remoteTomb[it.key()] = Int(it.value());
	}
	Data::MergeTombstones(remoteTomb);
	const auto tomb = Data::LoadTombstones();
	const auto deletedAfter = [&](const QString &key, qint64 at) {
		const auto i = tomb.find(key);
		return (i != tomb.end()) && (i->second >= at);
	};

	// The registry of who was written to.
	auto local = std::map<QString, Data::SmsSendRecord>();
	for (auto &record : Data::LoadAllSmsSendRecords()) {
		local.emplace(record.username, std::move(record));
	}
	auto upsert = std::vector<Data::SmsSendRecord>();
	auto remove = std::vector<QString>();
	const auto sentObject = root["sent"].toObject();
	for (auto it = sentObject.begin(); it != sentObject.end(); ++it) {
		const auto object = it.value().toObject();
		const auto username = it.key();
		auto record = Data::SmsSendRecord{
			.username = username,
			.status = StatusFromText(object["status"].toString()),
			.account = object["account"].toString(),
			.at = Int(object["at"]),
			.baseId = UInt(object["base"]),
			.source = object["source"].toString(),
			.note = object["note"].toString(),
		};
		if (deletedAfter(u"sent:"_q + username, record.at)) {
			continue;
		}
		const auto mine = local.find(username);
		if (mine == local.end()) {
			upsert.push_back(std::move(record));
			++result.sentAdded;
			continue;
		}
		const auto &was = mine->second;
		const auto wasSent = (was.status == Data::SmsSendStatus::Sent);
		const auto nowSent = (record.status == Data::SmsSendStatus::Sent);
		// "Written to" is never lost; otherwise the later mark wins.
		const auto take = (wasSent != nowSent)
			? nowSent
			: (record.at > was.at);
		if (take) {
			if (record.source.isEmpty()) {
				record.source = was.source;
			}
			if (!record.baseId) {
				record.baseId = was.baseId;
			}
			upsert.push_back(std::move(record));
		}
	}
	for (const auto &[username, record] : local) {
		if (deletedAfter(u"sent:"_q + username, record.at)) {
			remove.push_back(username);
			++result.sentRemoved;
		}
	}
	Data::ImportSmsRecords(upsert, remove);

	// The bases: the newer save of a base wins as a whole.
	auto localBases = std::map<uint64, Data::ScrapedGroupMeta>();
	for (const auto &meta : Data::LoadScrapedGroupsIndex()) {
		localBases.emplace(meta.id, meta);
	}
	for (const auto &value : root["bases"].toArray()) {
		const auto object = value.toObject();
		auto meta = Data::ScrapedGroupMeta{
			.id = UInt(object["id"]),
			.title = object["title"].toString(),
			.savedAt = Int(object["savedAt"]),
			.kind = (object["kind"].toString() == u"groups"_q)
				? Data::BaseKind::Groups
				: Data::BaseKind::Users,
		};
		if (!meta.id) {
			continue;
		}
		const auto key = u"base:"_q + QString::number(meta.id);
		const auto mine = localBases.find(meta.id);
		if (deletedAfter(key, meta.savedAt)) {
			continue;
		}
		if (mine != localBases.end() && mine->second.savedAt >= meta.savedAt) {
			continue;
		}
		auto names = QStringList();
		auto entries = std::vector<Data::ScrapedGroupEntry>();
		for (const auto &item : object["items"].toArray()) {
			if (meta.kind == Data::BaseKind::Groups) {
				const auto entry = item.toObject();
				entries.push_back({
					.username = entry["username"].toString(),
					.title = entry["title"].toString(),
					.membersCount = entry["membersCount"].toInt(),
					.link = entry["link"].toString(),
					.checked = entry["checked"].toBool(),
				});
			} else {
				names.push_back(item.toString());
			}
		}
		Data::ImportBase(meta, names, entries);
		++result.bases;
	}
	for (const auto &[id, meta] : localBases) {
		if (deletedAfter(u"base:"_q + QString::number(id), meta.savedAt)) {
			Data::RemoveBaseForSync(id);
			++result.bases;
		}
	}
	Data::PruneDeliveredFromBases();

	// Merge proxy pools if present.
	if (root.contains("proxy_pools")) {
		const auto pools = root["proxy_pools"].toArray();
		MTP::GlobalProxyPool().mergeJson(QJsonDocument(pools).toJson(QJsonDocument::Compact));
	}

	// The AI side: whichever copy was changed later.
	const auto dir = GgtgPaths::DbDir();
	const auto files = root["files"].toObject();
	for (auto it = files.begin(); it != files.end(); ++it) {
		if (!SafeFileName(it.key())) {
			continue;
		}
		const auto object = it.value().toObject();
		const auto mtime = Int(object["mtime"]);
		if (it.key() == u"chat_lists.json"_q) {
			Core::GgtgLists::MergeJson(object["text"].toString().toUtf8());
			++result.files;
			continue;
		}
		const auto info = QFileInfo(dir + it.key());
		if (info.exists() && info.lastModified().toSecsSinceEpoch() >= mtime) {
			continue;
		}
		WriteAtomic(
			dir + it.key(),
			object["text"].toString().toUtf8(),
			mtime);
		++result.files;
	}

	result.ok = true;
	Core::GgtgLog::Info(
		u"Синхронизация"_q,
		ResultText(result));
	return result;
}

QString ResultText(const MergeResult &result) {
	if (!result.ok) {
		return result.error;
	}
	return u"баз: "_q + QString::number(result.bases)
		+ u", новых отметок: "_q + QString::number(result.sentAdded)
		+ u", убрано: "_q + QString::number(result.sentRemoved)
		+ u", файлов ИИ: "_q + QString::number(result.files);
}

QString KeyId(const QString &passphrase) {
	if (passphrase.isEmpty()) {
		return QString();
	}
	return QString::fromLatin1(QCryptographicHash::hash(
		(u"ggtgsync-key:"_q + passphrase).toUtf8(),
		QCryptographicHash::Sha256).toHex().left(6)).toUpper();
}

QString NewPassphrase() {
	auto bytes = QByteArray(24, 0);
	QRandomGenerator::system()->fillRange(
		reinterpret_cast<quint32*>(bytes.data()),
		bytes.size() / 4);
	return QString::fromLatin1(bytes.toBase64(
		QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

QString PairingText(const Settings &settings) {
	return u"ggtgsync:1:"_q
		+ settings.mainUserId
		+ ':'
		+ QString::fromLatin1(settings.passphrase.toUtf8().toBase64(
			QByteArray::Base64UrlEncoding
				| QByteArray::OmitTrailingEquals));
}

} // namespace Core::GgtgSync
