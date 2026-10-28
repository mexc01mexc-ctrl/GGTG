/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and exceptions, see https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/random.h"
#include "core/ggtg_log.h"
#include "settings.h"
#include "core/ggtg_paths.h"

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSaveFile>

#include <algorithm>

namespace Core::GgtgSessions {

// Journal of accounts whose session the server (or someone) ended while the
// app was running: tdata/ggtg_db/sessions.json. The app logs such
// an account out and wipes its local data (Telegram Desktop's own
// behaviour on any 401 from the main DC), so without this note it would
// just vanish. Each entry keeps who it was, when, and the error Telegram
// answered with - the reason (see ReasonTitle/ReasonHint) - plus what is
// needed to log the account back in (phone, folder, stored 2FA password).
struct LostSession {
	qint64 at = 0;
	QString userId;
	QString phone;
	QString name;
	QString username;
	QString folder;
	QString twoFactor;
	QString proxy;
	QString reason;
	QString description;
	int code = 0;
	int liveAccounts = 0;
};

constexpr auto kMaxEntries = 5000;

[[nodiscard]] inline QString Path() {
	return Core::GgtgPaths::DbDir() + u"sessions.json"_q;
}

// What happened, in plain words (the raw Telegram code stays next to it for
// anyone who needs it).
[[nodiscard]] inline QString ReasonTitle(const QString &reason) {
	if (reason == u"AUTH_KEY_DUPLICATED"_q) {
		return u"Аккаунт открыли в двух местах одновременно"_q;
	} else if (reason == u"AUTH_KEY_UNREGISTERED"_q) {
		return u"Telegram больше не признаёт эту сессию"_q;
	} else if (reason == u"SESSION_REVOKED"_q) {
		return u"Сессию завершили"_q;
	} else if (reason == u"SESSION_EXPIRED"_q) {
		return u"Сессия истекла"_q;
	} else if (reason == u"USER_DEACTIVATED"_q) {
		return u"Аккаунт удалён"_q;
	} else if (reason == u"USER_DEACTIVATED_BAN"_q) {
		return u"Аккаунт заблокирован Telegram"_q;
	} else if (reason == u"SWAP"_q) {
		return u"Переключение сессии не завершилось"_q;
	}
	return u"Telegram закрыл сессию"_q;
}

[[nodiscard]] inline QString ReasonHint(const QString &reason) {
	if (reason == u"AUTH_KEY_DUPLICATED"_q) {
		return u"Одну и ту же сессию (одну и ту же папку tdata) запустили "
			"сразу в двух приложениях - например на Mac и на Windows, или "
			"включили копию сессии, пока оригинал был в сети. "
			"В таком случае Telegram закрывает сессию. Для копии используйте "
			"в экспорте «Новую сессию»: у неё своя отдельная сессия."_q;
	} else if (reason == u"AUTH_KEY_UNREGISTERED"_q) {
		return u"Сессию закрыли в другом месте, либо Telegram снял её сам. "
			"Чаще всего так бывает из-за подозрительной активности (много "
			"входов с одного адреса, массовые действия, частая смена "
			"прокси) или если аккаунт заблокирован."_q;
	} else if (reason == u"SESSION_REVOKED"_q) {
		return u"Сессию закрыли вручную: в настройках аккаунта («Активные "
			"сеансы»), через массовое действие «Завершить другие сессии» "
			"или с другого устройства."_q;
	} else if (reason == u"SESSION_EXPIRED"_q) {
		return u"Аккаунтом долго не пользовались, и сессия закрылась сама "
			"(в настройках Telegram можно выбрать, через сколько неактивные "
			"сессии завершаются)."_q;
	} else if (reason == u"USER_DEACTIVATED"_q) {
		return u"Аккаунт удалён - им самим или из-за долгого "
			"неиспользования."_q;
	} else if (reason == u"USER_DEACTIVATED_BAN"_q) {
		return u"Telegram заблокировал аккаунт. Войти в него заново "
			"обычно нельзя."_q;
	} else if (reason == u"SWAP"_q) {
		return u"Приложение начало переключать аккаунт на другую сессию, "
			"но не довело это до конца (сбой или закрытие). Прежняя "
			"сессия сохранена как резервная: нажмите «Восстановить»."_q;
	}
	return u"Telegram ответил, что сессия недействительна. Точная причина "
		"не указана, код ошибки показан рядом."_q;
}

[[nodiscard]] inline QJsonObject ToJson(const LostSession &entry) {
	return QJsonObject{
		{ "at", double(entry.at) },
		{ "userId", entry.userId },
		{ "phone", entry.phone },
		{ "name", entry.name },
		{ "username", entry.username },
		{ "folder", entry.folder },
		{ "twoFactor", entry.twoFactor },
		{ "proxy", entry.proxy },
		{ "reason", entry.reason },
		{ "description", entry.description },
		{ "code", entry.code },
		{ "liveAccounts", entry.liveAccounts },
	};
}

[[nodiscard]] inline LostSession FromJson(const QJsonObject &object) {
	auto result = LostSession();
	result.at = qint64(object["at"].toDouble());
	result.userId = object["userId"].toString();
	result.phone = object["phone"].toString();
	result.name = object["name"].toString();
	result.username = object["username"].toString();
	result.folder = object["folder"].toString();
	result.twoFactor = object["twoFactor"].toString();
	result.proxy = object["proxy"].toString();
	result.reason = object["reason"].toString();
	result.description = object["description"].toString();
	result.code = object["code"].toInt();
	result.liveAccounts = object["liveAccounts"].toInt();
	return result;
}

// Oldest first, as stored.
[[nodiscard]] inline std::vector<LostSession> Load() {
	auto result = std::vector<LostSession>();
	auto file = QFile(Path());
	if (!file.open(QIODevice::ReadOnly)) {
		return result;
	}
	const auto document = QJsonDocument::fromJson(file.readAll());
	for (const auto &value : document.object()["lost"].toArray()) {
		result.push_back(FromJson(value.toObject()));
	}
	return result;
}

inline bool Save(const std::vector<LostSession> &entries) {
	auto array = QJsonArray();
	for (const auto &entry : entries) {
		array.push_back(ToJson(entry));
	}
	const auto path = Path();
	QDir().mkpath(QFileInfo(path).absolutePath());
	auto file = QSaveFile(path);
	if (!file.open(QIODevice::WriteOnly)) {
		return false;
	}
	file.write(QJsonDocument(QJsonObject{
		{ "version", 1 },
		{ "lost", array },
	}).toJson(QJsonDocument::Indented));
	return file.commit();
}

// Appends one entry (and mirrors it to ggtg.log).
inline void Record(LostSession entry) {
	GgtgLog::Error(
		u"Сессии"_q,
		u"Сессия разорвана: +%1 %2 (%3). %4 [%5, код %6]"_q
			.arg(entry.phone)
			.arg(entry.name)
			.arg(entry.username.isEmpty()
				? entry.userId
				: ('@' + entry.username))
			.arg(ReasonTitle(entry.reason))
			.arg(entry.reason)
			.arg(entry.code));
	auto entries = Load();
	entries.push_back(std::move(entry));
	if (int(entries.size()) > kMaxEntries) {
		entries.erase(
			entries.begin(),
			entries.begin() + (int(entries.size()) - kMaxEntries));
	}
	if (!Save(entries)) {
		GgtgLog::Error(u"Сессии"_q, u"не удалось записать sessions.json"_q);
	}
}

inline void Clear() {
	Save({});
}

// Adds an entry without the "session lost" line in ggtg.log - for the note
// that a session switch is in progress.
inline void RecordQuiet(LostSession entry) {
	auto entries = Load();
	entries.push_back(std::move(entry));
	Save(entries);
}

// Drops the entries of one account that carry this exact reason.
inline void RemoveReason(const QString &userId, const QString &reason) {
	auto entries = Load();
	entries.erase(
		std::remove_if(
			entries.begin(),
			entries.end(),
			[&](const LostSession &entry) {
				return entry.userId == userId && entry.reason == reason;
			}),
		entries.end());
	Save(entries);
}

// Every entry for one account (by Telegram user id), oldest first.
[[nodiscard]] inline std::vector<LostSession> ForUser(
		const std::vector<LostSession> &entries,
		const QString &userId) {
	auto result = std::vector<LostSession>();
	for (const auto &entry : entries) {
		if (entry.userId == userId) {
			result.push_back(entry);
		}
	}
	return result;
}

// The newest entry of each account, newest first - what the account manager
// lists as accounts whose session is gone.
[[nodiscard]] inline std::vector<LostSession> LatestPerUser(
		const std::vector<LostSession> &entries) {
	auto result = std::vector<LostSession>();
	for (auto i = entries.rbegin(); i != entries.rend(); ++i) {
		const auto seen = std::any_of(
			result.begin(),
			result.end(),
			[&](const LostSession &entry) {
				return entry.userId == i->userId;
			});
		if (!seen) {
			result.push_back(*i);
		}
	}
	return result;
}

// Drops every entry of one account (it was dealt with: logged in again or
// no longer needed in the list).
inline void RemoveUser(const QString &userId) {
	auto entries = Load();
	entries.erase(
		std::remove_if(
			entries.begin(),
			entries.end(),
			[&](const LostSession &entry) { return entry.userId == userId; }),
		entries.end());
	Save(entries);
}

// ---- Vault: other sessions of an account that this app keeps track of ----
//
// A Telegram session is its auth key, and one key must only ever be used in
// one place at a time (two users of the same key make Telegram end it:
// AUTH_KEY_DUPLICATED). So a spare is never a copy of the working session:
// it is a separate session of its own, made with Main::DuplicateSession(),
// and it is never run while another one is in use.
//
//  - "backup": a spare kept here (its key in session_vault/<user>/<id>/
//    tdata, the same format an export writes); it is swapped in when the
//    working session dies or by hand;
//  - "exported": a session that was handed out (another device, GGTG for
//    Android, ...); only a note is kept - the key left with it.
//
// `hash` is Telegram's own number of that session in the account's active
// sessions list (0 = not known). It lets the list tell which entry is which
// and whether it is still alive, without ever connecting with its key.
struct StoredSession {
	QString id;
	QString userId;
	QString role;
	QString source;
	QString note;
	QString state; // "ok", "revoked" or empty (not checked).
	qint64 createdAt = 0;
	qint64 checkedAt = 0;
	quint64 hash = 0;
	// What is needed to bring the account back with this session.
	QString phone;
	QString name;
	QString username;
	QString folder;
	QString twoFactor;
	QJsonObject proxy;
	QJsonObject device;
};

[[nodiscard]] inline QString VaultRoot() {
	return Core::GgtgPaths::DbDir() + u"session_vault/"_q;
}

[[nodiscard]] inline QString VaultIndexPath() {
	return VaultRoot() + u"index.json"_q;
}

// Folder of one stored session; the key itself is in its "tdata" subfolder.
[[nodiscard]] inline QString VaultFolder(const StoredSession &entry) {
	return VaultRoot() + entry.userId + '/' + entry.id;
}

[[nodiscard]] inline QJsonObject ToJson(const StoredSession &entry) {
	return QJsonObject{
		{ "id", entry.id },
		{ "userId", entry.userId },
		{ "role", entry.role },
		{ "source", entry.source },
		{ "note", entry.note },
		{ "state", entry.state },
		{ "createdAt", double(entry.createdAt) },
		{ "checkedAt", double(entry.checkedAt) },
		{ "hash", QString::number(entry.hash) },
		{ "phone", entry.phone },
		{ "name", entry.name },
		{ "username", entry.username },
		{ "folder", entry.folder },
		{ "twoFactor", entry.twoFactor },
		{ "proxy", entry.proxy },
		{ "device", entry.device },
	};
}

[[nodiscard]] inline StoredSession StoredFromJson(const QJsonObject &object) {
	auto result = StoredSession();
	result.id = object["id"].toString();
	result.userId = object["userId"].toString();
	result.role = object["role"].toString();
	result.source = object["source"].toString();
	result.note = object["note"].toString();
	result.state = object["state"].toString();
	result.createdAt = qint64(object["createdAt"].toDouble());
	result.checkedAt = qint64(object["checkedAt"].toDouble());
	result.hash = object["hash"].toString().toULongLong();
	result.phone = object["phone"].toString();
	result.name = object["name"].toString();
	result.username = object["username"].toString();
	result.folder = object["folder"].toString();
	result.twoFactor = object["twoFactor"].toString();
	result.proxy = object["proxy"].toObject();
	result.device = object["device"].toObject();
	return result;
}

inline bool WriteSessionVaultSidecar(const StoredSession &entry) {
	const auto folder = VaultFolder(entry);
	QDir().mkpath(folder);
	auto info = QJsonObject();
	info["folder"] = entry.folder;
	if (!entry.twoFactor.isEmpty()) {
		info["twoFactor"] = entry.twoFactor;
	}
	if (!entry.device.isEmpty()) {
		info["device"] = entry.device;
	}
	if (!entry.proxy.isEmpty()) {
		info["proxy"] = entry.proxy;
	}
	auto file = QFile(folder + u"/ggtg.json"_q);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		return false;
	}
	return file.write(QJsonDocument(info).toJson(QJsonDocument::Indented)) > 0;
}

[[nodiscard]] inline std::vector<StoredSession> LoadVault() {
	auto result = std::vector<StoredSession>();
	auto file = QFile(VaultIndexPath());
	if (!file.open(QIODevice::ReadOnly)) {
		return result;
	}
	const auto document = QJsonDocument::fromJson(file.readAll());
	for (const auto &value : document.object()["sessions"].toArray()) {
		result.push_back(StoredFromJson(value.toObject()));
	}
	return result;
}

inline bool SaveVault(const std::vector<StoredSession> &entries) {
	auto array = QJsonArray();
	for (const auto &entry : entries) {
		array.push_back(ToJson(entry));
	}
	QDir().mkpath(VaultRoot());
	auto file = QSaveFile(VaultIndexPath());
	if (!file.open(QIODevice::WriteOnly)) {
		return false;
	}
	file.write(QJsonDocument(QJsonObject{
		{ "version", 1 },
		{ "sessions", array },
	}).toJson(QJsonDocument::Indented));
	return file.commit();
}

// Fills in a fresh id and creation time, then stores the entry.
inline StoredSession AddStored(StoredSession entry) {
	entry.id = QString::number(base::RandomValue<uint64>(), 16);
	if (!entry.createdAt) {
		entry.createdAt = QDateTime::currentSecsSinceEpoch();
	}
	auto entries = LoadVault();
	entries.push_back(entry);
	if (!SaveVault(entries)) {
		GgtgLog::Error(u"Сессии"_q, u"не удалось записать хранилище сессий"_q);
	}
	return entry;
}

// Removes the entry and its key folder (if it has one).
inline void RemoveStored(const QString &id) {
	auto entries = LoadVault();
	for (auto i = entries.begin(); i != entries.end(); ++i) {
		if (i->id == id) {
			QDir(VaultFolder(*i)).removeRecursively();
			entries.erase(i);
			break;
		}
	}
	SaveVault(entries);
}

// Roles of stored sessions. A session can be switched onto if its key
// files exist in the vault folder, or if it has an actionable role.
[[nodiscard]] inline bool HasKey(const StoredSession &entry) {
	if (QFileInfo::exists(VaultFolder(entry) + u"/tdata"_q)) {
		return true;
	}
	return entry.role == u"backup"_q
		|| entry.role == u"duplicate"_q
		|| entry.role == u"imported"_q
		|| entry.role == u"purchased"_q
		|| entry.role == u"anti_theft_backup"_q
		|| entry.role == u"exported"_q
		|| entry.role == u"android"_q
		|| entry.role == u"desktop"_q
		|| entry.role == u"previous"_q;
}

// Labels for UI representation (strict UI style: official Telegram wording, no emoji).
[[nodiscard]] inline QString RoleTitle(const QString &role) {
	if (role == u"backup"_q) {
		return u"резервная"_q;
	} else if (role == u"anti_theft_backup"_q) {
		return u"резервная (анти-угон)"_q;
	} else if (role == u"purchased"_q) {
		return u"купленная"_q;
	} else if (role == u"duplicate"_q) {
		return u"дублированная"_q;
	} else if (role == u"imported"_q) {
		return u"импортированная"_q;
	} else if (role == u"exported"_q) {
		return u"экспортированная"_q;
	} else if (role == u"android"_q) {
		return u"Android"_q;
	} else if (role == u"desktop"_q) {
		return u"десктоп"_q;
	} else if (role == u"previous"_q) {
		return u"бывшая рабочая"_q;
	}
	return u"сохранённая"_q;
}

// Anti-theft trust cooldown: Telegram requires a newly created authorization
// to be at least 24 hours (86400 seconds) old before it is allowed to terminate
// other authorizations (FRESH_RESET_AUTHORISATION_FORBIDDEN) or change the phone number
// (FRESH_CHANGE_PHONE_FORBIDDEN).
constexpr qint64 kAntiTheftTrustCooldown = 86400;

[[nodiscard]] inline bool IsSessionTrusted(const StoredSession &entry) {
	if (entry.createdAt <= 0) {
		return false;
	}
	const auto now = QDateTime::currentSecsSinceEpoch();
	return (now - entry.createdAt) >= kAntiTheftTrustCooldown;
}

[[nodiscard]] inline qint64 SecondsUntilTrusted(const StoredSession &entry) {
	if (entry.createdAt <= 0) {
		return kAntiTheftTrustCooldown;
	}
	const auto now = QDateTime::currentSecsSinceEpoch();
	const auto elapsed = now - entry.createdAt;
	return (elapsed >= kAntiTheftTrustCooldown) ? 0 : (kAntiTheftTrustCooldown - elapsed);
}

[[nodiscard]] inline QString TrustCountdownString(const StoredSession &entry) {
	const auto remaining = SecondsUntilTrusted(entry);
	if (remaining <= 0) {
		return u"выдержана (> 24 ч., готова к сбросу сессий)"_q;
	}
	const auto hours = remaining / 3600;
	const auto minutes = (remaining % 3600) / 60;
	return u"отлёжка: осталось %1 ч. %2 мин."_q
		.arg(hours)
		.arg(minutes, 2, 10, QChar('0'));
}

// A spare handed out with an export: it stays here (the account can still
// be switched onto it), but is not a spare any more.
inline void UpdateStoredRole(
		const QString &id,
		const QString &role,
		const QString &note) {
	auto entries = LoadVault();
	for (auto &entry : entries) {
		if (entry.id == id) {
			entry.role = role;
			entry.note = note;
		}
	}
	SaveVault(entries);
}

// Telegram's number of the stored session, once it is found out.
inline void UpdateStoredHash(const QString &id, quint64 hash) {
	auto entries = LoadVault();
	for (auto &entry : entries) {
		if (entry.id == id) {
			entry.hash = hash;
			entry.state = u"ok"_q;
			entry.checkedAt = QDateTime::currentSecsSinceEpoch();
		}
	}
	SaveVault(entries);
}

inline void UpdateStoredState(const QString &id, const QString &state) {
	auto entries = LoadVault();
	for (auto &entry : entries) {
		if (entry.id == id) {
			entry.state = state;
			entry.checkedAt = QDateTime::currentSecsSinceEpoch();
		}
	}
	SaveVault(entries);
}

// The account's proxy changed: all stored sessions must carry the
// new proxy so switching to them preserves proxy connection.
inline void UpdateStoredProxy(const QString &userId, const QJsonObject &proxy) {
	auto entries = LoadVault();
	auto changed = false;
	for (auto &entry : entries) {
		if (entry.userId == userId) {
			entry.proxy = proxy;
			WriteSessionVaultSidecar(entry);
			changed = true;
		}
	}
	if (changed) {
		SaveVault(entries);
	}
}

// The account's phone number changed: update all stored sessions
// so the vault reflects the new phone.
inline void UpdateStoredPhone(const QString &userId, const QString &phone) {
	auto entries = LoadVault();
	auto changed = false;
	for (auto &entry : entries) {
		if (entry.userId == userId) {
			entry.phone = phone;
			WriteSessionVaultSidecar(entry);
			changed = true;
		}
	}
	if (changed) {
		SaveVault(entries);
	}
}

[[nodiscard]] inline std::vector<StoredSession> StoredForUser(
		const std::vector<StoredSession> &entries,
		const QString &userId) {
	auto result = std::vector<StoredSession>();
	for (const auto &entry : entries) {
		if (entry.userId == userId) {
			result.push_back(entry);
		}
	}
	return result;
}

} // namespace Core::GgtgSessions
