/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "settings/ggtg_session_json.h"
#include "settings/ggtg_export.h"
#include "core/ggtg_geo.h"
#include "core/ggtg_sessions.h"
#include "storage/storage_tdata_export.h"
#include "settings/bulk_accounts_actions.h"
#include "settings/ggtg_add_accounts.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "data/data_user.h"
#include "core/application.h"
#include "ui/layers/generic_box.h"

#include <QtCore/QDir>
#include <QtCore/QDirIterator>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QLibrary>
#include <QtCore/QDataStream>
#include <QtCore/QDateTime>
#include <QtCore/QSaveFile>
#include <QtWidgets/QFileDialog>

#ifdef Q_OS_MAC
#include <dlfcn.h>
#endif

namespace Settings {

namespace {

constexpr auto kAuthKeySize = 256;
constexpr auto kWideIdsTag = ~quint64(0);

struct SqliteApi {
	using sqlite3_ptr = void*;
	using sqlite3_stmt_ptr = void*;

	int (*open_v2)(const char *filename, sqlite3_ptr *ppDb, int flags, const char *zVfs) = nullptr;
	int (*close)(sqlite3_ptr) = nullptr;
	int (*exec)(sqlite3_ptr, const char *sql, int (*callback)(void*,int,char**,char**), void *arg, char **errmsg) = nullptr;
	int (*prepare_v2)(sqlite3_ptr, const char *zSql, int nByte, sqlite3_stmt_ptr *ppStmt, const char **pzTail) = nullptr;
	int (*step)(sqlite3_stmt_ptr) = nullptr;
	int (*finalize)(sqlite3_stmt_ptr) = nullptr;
	int (*bind_int)(sqlite3_stmt_ptr, int, int) = nullptr;
	int (*bind_int64)(sqlite3_stmt_ptr, int, int64_t) = nullptr;
	int (*bind_text)(sqlite3_stmt_ptr, int, const char*, int, void(*)(void*)) = nullptr;
	int (*bind_blob)(sqlite3_stmt_ptr, int, const void*, int, void(*)(void*)) = nullptr;
	int (*column_int)(sqlite3_stmt_ptr, int) = nullptr;
	int64_t (*column_int64)(sqlite3_stmt_ptr, int) = nullptr;
	const void *(*column_blob)(sqlite3_stmt_ptr, int) = nullptr;
	int (*column_bytes)(sqlite3_stmt_ptr, int) = nullptr;
	const unsigned char *(*column_text)(sqlite3_stmt_ptr, int) = nullptr;
	const char *(*column_name)(sqlite3_stmt_ptr, int) = nullptr;
	int (*column_count)(sqlite3_stmt_ptr) = nullptr;

	bool valid() const {
		return open_v2 && close && exec && prepare_v2 && step && finalize
			&& bind_int && bind_blob && column_int && column_blob && column_bytes;
	}
};

const SqliteApi &GetSqliteApi() {
	static const SqliteApi api = [] {
		SqliteApi a;
#ifdef Q_OS_MAC
		void *handle = dlopen("libsqlite3.dylib", RTLD_LAZY);
		if (handle) {
			a.open_v2 = (decltype(a.open_v2))dlsym(handle, "sqlite3_open_v2");
			a.close = (decltype(a.close))dlsym(handle, "sqlite3_close");
			a.exec = (decltype(a.exec))dlsym(handle, "sqlite3_exec");
			a.prepare_v2 = (decltype(a.prepare_v2))dlsym(handle, "sqlite3_prepare_v2");
			a.step = (decltype(a.step))dlsym(handle, "sqlite3_step");
			a.finalize = (decltype(a.finalize))dlsym(handle, "sqlite3_finalize");
			a.bind_int = (decltype(a.bind_int))dlsym(handle, "sqlite3_bind_int");
			a.bind_int64 = (decltype(a.bind_int64))dlsym(handle, "sqlite3_bind_int64");
			a.bind_text = (decltype(a.bind_text))dlsym(handle, "sqlite3_bind_text");
			a.bind_blob = (decltype(a.bind_blob))dlsym(handle, "sqlite3_bind_blob");
			a.column_int = (decltype(a.column_int))dlsym(handle, "sqlite3_column_int");
			a.column_int64 = (decltype(a.column_int64))dlsym(handle, "sqlite3_column_int64");
			a.column_blob = (decltype(a.column_blob))dlsym(handle, "sqlite3_column_blob");
			a.column_bytes = (decltype(a.column_bytes))dlsym(handle, "sqlite3_column_bytes");
			a.column_text = (decltype(a.column_text))dlsym(handle, "sqlite3_column_text");
			a.column_name = (decltype(a.column_name))dlsym(handle, "sqlite3_column_name");
			a.column_count = (decltype(a.column_count))dlsym(handle, "sqlite3_column_count");
			if (a.valid()) return a;
		}
#endif
		static QLibrary lib("sqlite3");
		if (!lib.isLoaded()) {
			lib.load();
		}
		if (lib.isLoaded()) {
			a.open_v2 = (decltype(a.open_v2))lib.resolve("sqlite3_open_v2");
			a.close = (decltype(a.close))lib.resolve("sqlite3_close");
			a.exec = (decltype(a.exec))lib.resolve("sqlite3_exec");
			a.prepare_v2 = (decltype(a.prepare_v2))lib.resolve("sqlite3_prepare_v2");
			a.step = (decltype(a.step))lib.resolve("sqlite3_step");
			a.finalize = (decltype(a.finalize))lib.resolve("sqlite3_finalize");
			a.bind_int = (decltype(a.bind_int))lib.resolve("sqlite3_bind_int");
			a.bind_int64 = (decltype(a.bind_int64))lib.resolve("sqlite3_bind_int64");
			a.bind_text = (decltype(a.bind_text))lib.resolve("sqlite3_bind_text");
			a.bind_blob = (decltype(a.bind_blob))lib.resolve("sqlite3_bind_blob");
			a.column_int = (decltype(a.column_int))lib.resolve("sqlite3_column_int");
			a.column_int64 = (decltype(a.column_int64))lib.resolve("sqlite3_column_int64");
			a.column_blob = (decltype(a.column_blob))lib.resolve("sqlite3_column_blob");
			a.column_bytes = (decltype(a.column_bytes))lib.resolve("sqlite3_column_bytes");
			a.column_text = (decltype(a.column_text))lib.resolve("sqlite3_column_text");
			a.column_name = (decltype(a.column_name))lib.resolve("sqlite3_column_name");
			a.column_count = (decltype(a.column_count))lib.resolve("sqlite3_column_count");
		}
		return a;
	}();
	return api;
}

QString DcToIp(qint32 dcId) {
	switch (dcId) {
	case 1: return u"149.154.175.53"_q;
	case 2: return u"149.154.167.51"_q;
	case 3: return u"149.154.175.100"_q;
	case 4: return u"149.154.167.91"_q;
	case 5: return u"91.108.56.130"_q;
	default: return u"149.154.167.51"_q;
	}
}

QByteArray BuildMtpAuthorization(
		quint64 userId,
		qint32 mainDcId,
		const std::vector<std::pair<qint32, QByteArray>> &keys) {
	auto result = QByteArray();
	QDataStream stream(&result, QIODevice::WriteOnly);
	stream.setVersion(QDataStream::Qt_5_1);
	stream << quint64(kWideIdsTag) << quint64(userId) << qint32(mainDcId);
	stream << qint32(keys.size());
	for (const auto &[dcId, key] : keys) {
		stream << qint32(dcId);
		stream.writeRawData(key.constData(), key.size());
	}
	stream << qint32(0); // 0 keys to destroy
	return result;
}

} // namespace

bool WriteTelethonSession(
		const QString &sessionPath,
		qint32 dcId,
		const QByteArray &authKey,
		quint64 userId,
		const QString &phone,
		const QString &username,
		const QString &name,
		QString *outError) {
	const auto &api = GetSqliteApi();
	if (!api.valid()) {
		if (outError) *outError = u"SQLite library could not be loaded"_q;
		return false;
	}
	if (authKey.size() != kAuthKeySize) {
		if (outError) *outError = u"Invalid auth key size"_q;
		return false;
	}
	QFile::remove(sessionPath);
	SqliteApi::sqlite3_ptr db = nullptr;
	const auto flags = 0x00000002 | 0x00000004; // SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE
	if (api.open_v2(sessionPath.toUtf8().constData(), &db, flags, nullptr) != 0 || !db) {
		if (outError) *outError = u"Failed to create sqlite database"_q;
		return false;
	}

	const char *schema =
		"CREATE TABLE IF NOT EXISTS version (version INTEGER PRIMARY KEY);"
		"DELETE FROM version;"
		"INSERT INTO version VALUES (7);"
		"CREATE TABLE IF NOT EXISTS sessions (dc_id INTEGER PRIMARY KEY, server_address TEXT, port INTEGER, auth_key BLOB, takeout_id INTEGER);"
		"DELETE FROM sessions;"
		"CREATE TABLE IF NOT EXISTS entities (id INTEGER PRIMARY KEY, hash INTEGER NOT NULL, username TEXT, phone INTEGER, name TEXT, date INTEGER);"
		"CREATE TABLE IF NOT EXISTS sent_files (md5_digest BLOB, file_size INTEGER, type INTEGER, id INTEGER, hash INTEGER, PRIMARY KEY(md5_digest, file_size, type));"
		"CREATE TABLE IF NOT EXISTS update_state (id INTEGER PRIMARY KEY, pts INTEGER, qts INTEGER, date INTEGER, seq INTEGER);";

	if (api.exec(db, schema, nullptr, nullptr, nullptr) != 0) {
		api.close(db);
		if (outError) *outError = u"Failed to initialize telethon schema"_q;
		return false;
	}

	const char *insertSession = "INSERT INTO sessions (dc_id, server_address, port, auth_key, takeout_id) VALUES (?, ?, 443, ?, 0);";
	SqliteApi::sqlite3_stmt_ptr stmt = nullptr;
	if (api.prepare_v2(db, insertSession, -1, &stmt, nullptr) == 0 && stmt) {
		const auto ip = DcToIp(dcId).toUtf8();
		api.bind_int(stmt, 1, dcId);
		if (api.bind_text) api.bind_text(stmt, 2, ip.constData(), ip.size(), nullptr);
		api.bind_blob(stmt, 3, authKey.constData(), authKey.size(), nullptr);
		api.step(stmt);
		api.finalize(stmt);
	}

	if (userId != 0 && api.bind_int64 && api.bind_text) {
		const char *insertEntity = "INSERT OR REPLACE INTO entities (id, hash, username, phone, name, date) VALUES (?, 0, ?, ?, ?, ?);";
		stmt = nullptr;
		if (api.prepare_v2(db, insertEntity, -1, &stmt, nullptr) == 0 && stmt) {
			const auto uName = username.toUtf8();
			const auto nName = name.toUtf8();
			auto digits = phone;
			digits.remove('+');
			const auto phoneNum = digits.toLongLong();
			api.bind_int64(stmt, 1, int64_t(userId));
			api.bind_text(stmt, 2, uName.constData(), uName.size(), nullptr);
			api.bind_int64(stmt, 3, phoneNum);
			api.bind_text(stmt, 4, nName.constData(), nName.size(), nullptr);
			api.bind_int(stmt, 5, int(QDateTime::currentSecsSinceEpoch()));
			api.step(stmt);
			api.finalize(stmt);
		}
	}

	api.close(db);
	return true;
}

std::optional<SessionData> ReadTelethonSession(const QString &sessionPath) {
	const auto &api = GetSqliteApi();
	if (!api.valid()) {
		return std::nullopt;
	}
	SqliteApi::sqlite3_ptr db = nullptr;
	if (api.open_v2(sessionPath.toUtf8().constData(), &db, 0x00000001 /* SQLITE_OPEN_READONLY */, nullptr) != 0 || !db) {
		return std::nullopt;
	}

	auto result = SessionData();
	SqliteApi::sqlite3_stmt_ptr stmt = nullptr;

	// 1. Read sessions table
	if (api.prepare_v2(db, "SELECT * FROM sessions LIMIT 1;", -1, &stmt, nullptr) == 0 && stmt) {
		if (api.step(stmt) == 100 /* SQLITE_ROW */) {
			const auto cols = api.column_count ? api.column_count(stmt) : 0;
			for (auto i = 0; i < cols; ++i) {
				const auto name = api.column_name ? QString::fromLatin1(api.column_name(stmt, i)) : QString();
				if (name == u"dc_id"_q) {
					result.dcId = api.column_int(stmt, i);
				} else if (name == u"auth_key"_q) {
					const auto bytes = api.column_bytes(stmt, i);
					const auto blob = api.column_blob(stmt, i);
					if (bytes == kAuthKeySize && blob) {
						result.authKey = QByteArray(reinterpret_cast<const char*>(blob), bytes);
					}
				} else if (name == u"user_id"_q && api.column_int64) {
					result.userId = quint64(api.column_int64(stmt, i));
				}
			}
		}
		api.finalize(stmt);
	}

	// 2. If user_id wasn't in sessions table (standard Telethon schema), check entities
	if (result.userId == 0 && api.column_int64) {
		stmt = nullptr;
		if (api.prepare_v2(db, "SELECT id FROM entities LIMIT 1;", -1, &stmt, nullptr) == 0 && stmt) {
			if (api.step(stmt) == 100) {
				result.userId = quint64(api.column_int64(stmt, 0));
			}
			api.finalize(stmt);
		}
	}

	api.close(db);

	if (result.authKey.size() != kAuthKeySize) {
		return std::nullopt;
	}
	if (result.dcId <= 0 || result.dcId > 5) {
		result.dcId = 2;
	}
	return result;
}

std::vector<SessionJsonAccount> ScanSessionJsonDirectory(const QString &folderPath) {
	auto result = std::vector<SessionJsonAccount>();
	const auto dir = QDir(folderPath);
	if (!dir.exists()) {
		return result;
	}

	// Helper to parse proxy object from JSON
	const auto parseProxy = [](const QJsonObject &p) -> MTP::ProxyData {
		auto data = MTP::ProxyData();
		if (p.isEmpty()) return data;
		const auto type = p.value(u"type"_q).toString().toLower();
		data.type = (type == u"socks5"_q)
			? MTP::ProxyData::Type::Socks5
			: (type == u"http"_q)
			? MTP::ProxyData::Type::Http
			: (type == u"mtproto"_q)
			? MTP::ProxyData::Type::Mtproto
			: MTP::ProxyData::Type::None;
		data.host = p.value(u"host"_q).toString();
		if (data.host.isEmpty()) data.host = p.value(u"address"_q).toString();
		if (data.host.isEmpty()) data.host = p.value(u"ip"_q).toString();
		data.port = p.value(u"port"_q).toInt();
		data.user = p.value(u"user"_q).toString();
		if (data.user.isEmpty()) data.user = p.value(u"username"_q).toString();
		data.password = p.value(u"password"_q).toString();
		if (type == u"mtproto"_q && p.contains(u"secret"_q)) {
			data.password = p.value(u"secret"_q).toString();
		}
		return data;
	};

	// Helper to parse device from JSON
	const auto parseDevice = [](const QJsonObject &obj) -> MTP::DeviceOverride {
		if (obj.contains(u"device"_q) && obj.value(u"device"_q).isObject()) {
			return DeviceOverrideFromJson(obj.value(u"device"_q).toObject());
		}
		auto dev = MTP::DeviceOverride();
		dev.deviceModel = obj.value(u"device_model"_q).toString();
		if (dev.deviceModel.isEmpty()) dev.deviceModel = obj.value(u"deviceModel"_q).toString();
		dev.systemVersion = obj.value(u"system_version"_q).toString();
		if (dev.systemVersion.isEmpty()) dev.systemVersion = obj.value(u"systemVersion"_q).toString();
		dev.appVersion = obj.value(u"app_version"_q).toString();
		if (dev.appVersion.isEmpty()) dev.appVersion = obj.value(u"appVersion"_q).toString();
		dev.systemLangCode = obj.value(u"system_lang_code"_q).toString();
		if (dev.systemLangCode.isEmpty()) dev.systemLangCode = obj.value(u"systemLangCode"_q).toString();
		dev.langPackName = obj.value(u"lang_pack"_q).toString();
		if (dev.langPackName.isEmpty()) dev.langPackName = obj.value(u"langPack"_q).toString();
		dev.cloudLangCode = obj.value(u"cloud_lang_code"_q).toString();
		if (dev.cloudLangCode.isEmpty()) dev.cloudLangCode = obj.value(u"cloudLangCode"_q).toString();
		if (obj.contains(u"app_id"_q)) dev.apiId = obj.value(u"app_id"_q).toInt();
		else if (obj.contains(u"api_id"_q)) dev.apiId = obj.value(u"api_id"_q).toInt();
		if (obj.contains(u"app_hash"_q)) dev.apiHash = obj.value(u"app_hash"_q).toString();
		else if (obj.contains(u"api_hash"_q)) dev.apiHash = obj.value(u"api_hash"_q).toString();
		if (obj.contains(u"tz_offset"_q)) dev.tzOffset = obj.value(u"tz_offset"_q).toInt();
		return dev;
	};

	// Walk folder up to 3 levels deep
	auto it = QDirIterator(folderPath, QStringList{ u"*.session"_q, u"*.json"_q }, QDir::Files, QDirIterator::Subdirectories);
	base::flat_set<QString> handledBases;

	while (it.hasNext()) {
		it.next();
		const auto info = it.fileInfo();
		const auto baseKey = info.dir().absolutePath() + u"/"_q + info.completeBaseName();
		if (handledBases.contains(baseKey)) {
			continue;
		}

		auto sessionFile = QString();
		auto jsonFile = QString();
		const auto dirPath = info.dir().absolutePath();

		if (info.suffix().compare(u"session"_q, Qt::CaseInsensitive) == 0) {
			sessionFile = info.absoluteFilePath();
			// Look for matching json
			const auto cand1 = dirPath + u"/"_q + info.completeBaseName() + u".json"_q;
			const auto cand2 = dirPath + u"/session.json"_q;
			const auto cand3 = dirPath + u"/ggtg.json"_q;
			if (QFile::exists(cand1)) jsonFile = cand1;
			else if (QFile::exists(cand2)) jsonFile = cand2;
			else if (QFile::exists(cand3)) jsonFile = cand3;
		} else if (info.suffix().compare(u"json"_q, Qt::CaseInsensitive) == 0) {
			// Check if there is a matching session
			const auto candSession = dirPath + u"/"_q + info.completeBaseName() + u".session"_q;
			if (QFile::exists(candSession)) {
				sessionFile = candSession;
				jsonFile = info.absoluteFilePath();
			} else {
				// Might be standalone json with auth_key
				jsonFile = info.absoluteFilePath();
			}
		}

		handledBases.insert(baseKey);

		auto acc = SessionJsonAccount();
		acc.accountDir = dirPath;

		// 1. Try reading SQLite session
		if (!sessionFile.isEmpty()) {
			if (const auto sess = ReadTelethonSession(sessionFile)) {
				acc.sessionPath = sessionFile;
				acc.mainDcId = sess->dcId;
				acc.userId = sess->userId;
				acc.authKey = sess->authKey;
				acc.allKeys.emplace_back(sess->dcId, sess->authKey);
			}
		}

		// 2. Read JSON
		auto jsonDoc = QJsonObject();
		if (!jsonFile.isEmpty()) {
			auto f = QFile(jsonFile);
			if (f.open(QIODevice::ReadOnly)) {
				jsonDoc = QJsonDocument::fromJson(f.readAll()).object();
				acc.jsonPath = jsonFile;
			}
		}

		// If authKey was not in .session, check if auth_key is in JSON
		if (acc.authKey.isEmpty() && !jsonDoc.isEmpty()) {
			const auto hexKey = jsonDoc.contains(u"auth_key"_q)
				? jsonDoc.value(u"auth_key"_q).toString()
				: jsonDoc.value(u"session_key"_q).toString();
			if (!hexKey.isEmpty()) {
				const auto keyBytes = QByteArray::fromHex(hexKey.toLatin1());
				if (keyBytes.size() == kAuthKeySize) {
					acc.authKey = keyBytes;
					acc.mainDcId = jsonDoc.value(u"dc_id"_q).toInt(2);
					if (acc.mainDcId <= 0) acc.mainDcId = jsonDoc.value(u"main_dc_id"_q).toInt(2);
					acc.allKeys.emplace_back(acc.mainDcId, acc.authKey);
				}
			}
		}

		// If we have no valid authKey, skip
		if (acc.authKey.size() != kAuthKeySize) {
			continue;
		}

		// Populate metadata from JSON
		if (!jsonDoc.isEmpty()) {
			if (acc.userId == 0) {
				if (jsonDoc.contains(u"user_id"_q)) {
					acc.userId = jsonDoc.value(u"user_id"_q).toVariant().toULongLong();
				} else if (jsonDoc.contains(u"userId"_q)) {
					acc.userId = jsonDoc.value(u"userId"_q).toVariant().toULongLong();
				}
			}
			acc.phone = jsonDoc.value(u"phone"_q).toString();
			acc.firstName = jsonDoc.value(u"first_name"_q).toString();
			acc.lastName = jsonDoc.value(u"last_name"_q).toString();
			acc.username = jsonDoc.value(u"username"_q).toString();

			if (jsonDoc.contains(u"twoFA"_q)) {
				acc.twoFactor = jsonDoc.value(u"twoFA"_q).toString();
			} else if (jsonDoc.contains(u"two_factor"_q)) {
				acc.twoFactor = jsonDoc.value(u"two_factor"_q).toString();
			} else if (jsonDoc.contains(u"twoFactor"_q)) {
				acc.twoFactor = jsonDoc.value(u"twoFactor"_q).toString();
			} else if (jsonDoc.contains(u"password"_q)) {
				acc.twoFactor = jsonDoc.value(u"password"_q).toString();
			}

			if (jsonDoc.contains(u"folder"_q)) {
				acc.folder = jsonDoc.value(u"folder"_q).toString();
			}

			if (jsonDoc.contains(u"proxy"_q) && jsonDoc.value(u"proxy"_q).isObject()) {
				acc.proxy = parseProxy(jsonDoc.value(u"proxy"_q).toObject());
			}

			acc.device = parseDevice(jsonDoc);

			if (jsonDoc.contains(u"settings"_q) && jsonDoc.value(u"settings"_q).isObject()) {
				const auto set = jsonDoc.value(u"settings"_q).toObject();
				if (set.contains(u"pinned"_q)) acc.pinned = set.value(u"pinned"_q).toBool();
			}
		}

		// Sibling 2fa.txt check if still empty
		if (acc.twoFactor.isEmpty()) {
			for (const auto &name : { u"2fa.txt"_q, u"two_factor.txt"_q }) {
				auto f = QFile(dirPath + u"/"_q + name);
				if (f.open(QIODevice::ReadOnly)) {
					acc.twoFactor = QString::fromUtf8(f.readAll()).trimmed();
					break;
				}
			}
		}

		// If userId still 0, derive from phone or fallback
		if (acc.userId == 0 && !acc.phone.isEmpty()) {
			auto d = acc.phone;
			d.remove('+');
			acc.userId = d.toULongLong();
		}
		if (acc.userId == 0) {
			acc.userId = 1000000000ULL + (uint32_t(qHash(acc.authKey)) % 900000000ULL);
		}

		result.push_back(std::move(acc));
	}
	return result;
}

bool ExportAccountAsSessionJson(
		not_null<Main::Account*> account,
		const QString &accountFolder,
		const QByteArray &serializedAuth,
		std::optional<MTP::DeviceOverride> customDevice,
		QString *outError) {
	if (!QDir().mkpath(accountFolder)) {
		if (outError) *outError = u"Не удалось создать папку "_q + accountFolder;
		return false;
	}

	const auto parsed = ParseAuthorization(serializedAuth);
	if (!parsed || parsed->keys.empty()) {
		if (outError) *outError = u"Не удалось прочитать ключи авторизации"_q;
		return false;
	}

	const auto user = account->session().user();
	auto phone = user->phone();
	phone.remove('+');
	phone = phone.trimmed();
	const auto safeName = phone.isEmpty() ? u"account_%1"_q.arg(user->id.value) : phone;

	// Primary key for mainDcId
	auto primaryKey = QByteArray();
	for (const auto &[dcId, key] : parsed->keys) {
		if (dcId == parsed->mainDcId) {
			primaryKey = key;
			break;
		}
	}
	if (primaryKey.isEmpty()) {
		primaryKey = parsed->keys.front().second;
	}

	// 1. Write SQLite <phone>.session
	const auto sessionPath = accountFolder + u"/"_q + safeName + u".session"_q;
	if (!WriteTelethonSession(
			sessionPath,
			parsed->mainDcId,
			primaryKey,
			parsed->userId,
			user->phone(),
			user->username(),
			user->name(),
			outError)) {
		// Log but don't abort - JSON can still be written
	}

	// 2. Prepare JSON
	auto info = QJsonObject();
	info["session_file"] = safeName + u".session"_q;
	info["phone"] = user->phone().isEmpty() ? QString() : (u"+"_q + user->phone());
	info["user_id"] = qint64(parsed->userId);
	info["first_name"] = user->firstName;
	info["last_name"] = user->lastName;
	info["username"] = user->username();
	info["main_dc_id"] = parsed->mainDcId;
	info["dc_id"] = parsed->mainDcId;
	info["auth_key"] = QString::fromLatin1(primaryKey.toHex());

	const auto twoFactor = account->twoFactorPassword();
	if (!twoFactor.isEmpty()) {
		info["twoFA"] = twoFactor;
		info["two_factor"] = twoFactor;
		info["password"] = twoFactor;
	}

	if (!account->folderTag().isEmpty()) {
		info["folder"] = account->folderTag();
	}

	auto dev = customDevice ? *customDevice : EffectiveDeviceOverride(account);
	const auto phoneStr = user->phone();
	if (!phoneStr.isEmpty()) {
		const auto geo = Core::GgtgGeo::MatchForPhone(phoneStr);
		if (dev.systemLangCode.isEmpty()) dev.systemLangCode = geo.systemLang;
		if (dev.cloudLangCode.isEmpty()) dev.cloudLangCode = geo.cloudLang;
		if (!dev.tzOffset) dev.tzOffset = geo.tzOffset;
	}
	info["device_model"] = dev.deviceModel;
	info["system_version"] = dev.systemVersion;
	info["app_version"] = dev.appVersion;
	info["system_lang_code"] = dev.systemLangCode;
	info["lang_pack"] = dev.langPackName;
	info["cloud_lang_code"] = dev.cloudLangCode;
	if (dev.tzOffset) info["tz_offset"] = *dev.tzOffset;
	info["app_id"] = dev.apiId > 0 ? dev.apiId : 2040;
	info["app_hash"] = !dev.apiHash.isEmpty() ? dev.apiHash : u"b18441a1ff607e10a989891a5462e627"_q;
	info["device"] = DeviceOverrideToJson(dev);

	const auto proxy = account->proxyOverride();
	if (proxy.type != MTP::ProxyData::Type::None) {
		info["proxy"] = ProxyToJson(proxy);
	}

	auto settings = QJsonObject();
	settings["pinned"] = account->pinned();
	if (!account->folderTag().isEmpty()) settings["folder"] = account->folderTag();
	if (!twoFactor.isEmpty()) settings["twoFactor"] = twoFactor;
	info["settings"] = settings;

	// All DC keys array
	auto keysArray = QJsonArray();
	for (const auto &[dcId, key] : parsed->keys) {
		auto k = QJsonObject();
		k["dcId"] = dcId;
		k["key"] = QString::fromLatin1(key.toHex());
		keysArray.append(k);
	}
	info["keys"] = keysArray;

	const auto jsonBytes = QJsonDocument(info).toJson(QJsonDocument::Indented);
	const auto writeFile = [&](const QString &name, const QByteArray &data) {
		auto file = QSaveFile(accountFolder + u"/"_q + name);
		if (!file.open(QIODevice::WriteOnly)) return false;
		file.write(data);
		return file.commit();
	};

	writeFile(safeName + u".json"_q, jsonBytes);
	writeFile(u"session.json"_q, jsonBytes);
	writeFile(u"ggtg.json"_q, jsonBytes);
	if (!twoFactor.isEmpty()) {
		writeFile(u"2fa.txt"_q, twoFactor.toUtf8());
	}

	return true;
}

void ImportSessionJsonFolder(std::shared_ptr<Ui::Show> show) {
	const auto folder = QFileDialog::getExistingDirectory(
		nullptr,
		u"Выберите папку с файлами Сессия + JSON"_q);
	if (folder.isEmpty() || AddAccounts::LimitReached(show)) {
		return;
	}

	const auto accounts = ScanSessionJsonDirectory(folder);
	if (accounts.empty()) {
		AddAccounts::Inform(
			show,
			u"В выбранной папке не найдено файлов .session или .json с "
			"ключами авторизации."_q);
		return;
	}

	auto &domain = Core::App().domain();
	auto notYetAdded = std::vector<SessionJsonAccount>();
	for (const auto &acc : accounts) {
		if (!domain.hasAccountWithUserId(acc.userId, nullptr)) {
			notYetAdded.push_back(acc);
		}
	}

	if (notYetAdded.empty()) {
		AddAccounts::Inform(
			show,
			u"Все аккаунты из этой папки уже добавлены в программу."_q);
		return;
	}

	const auto runImport = [=](Fn<MTP::ProxyData()> nextProxy) {
		auto authorizations = std::vector<Storage::ForeignTdataAuthorization>();
		for (const auto &acc : notYetAdded) {
			auto auth = Storage::ForeignTdataAuthorization();
			auth.data = BuildMtpAuthorization(acc.userId, acc.mainDcId, acc.allKeys);
			auth.folder = acc.accountDir;
			authorizations.push_back(std::move(auth));
		}

		auto &domain = Core::App().domain();
		const auto imported = domain.addFromAuthorizations(
			authorizations,
			MTP::Environment::Production,
			nextProxy,
			[=](not_null<::Main::Account*> account, const QString &source) {
				for (const auto &acc : notYetAdded) {
					if (acc.accountDir == source || acc.userId == account->session().userId().bare) {
						if (!acc.twoFactor.isEmpty()) {
							account->setTwoFactorPassword(acc.twoFactor);
						}
						if (!acc.folder.isEmpty()) {
							account->setFolderTag(acc.folder);
						}
						if (acc.pinned) {
							account->setPinned(true);
						}
						if (acc.proxy.type != MTP::ProxyData::Type::None) {
							account->setProxyOverride(acc.proxy);
						}
						if (acc.device.apiId > 0 || !acc.device.deviceModel.isEmpty()) {
							account->setDeviceOverride(acc.device);
						}
						break;
					}
				}
			});

		if (imported.empty()) {
			AddAccounts::Inform(
				show,
				u"Не удалось импортировать аккаунты: проверьте файлы сессий."_q);
			return;
		}

		const auto hadSession = domain.started() && domain.active().sessionExists();
		if (!hadSession) {
			domain.activate(imported.front());
		}

		// Store imported sessions in Session Vault
		for (const auto &accPtr : imported) {
			if (!accPtr->sessionExists() || !accPtr->session().user()) {
				continue;
			}
			namespace Sessions = Core::GgtgSessions;
			const auto userId = QString::number(accPtr->session().userId().bare);
			auto entry = Sessions::StoredSession();
			entry.userId = userId;
			entry.role = u"purchased"_q;
			entry.source = u"import_session_json"_q;
			entry.note = u"исходная сессия (купленная Сессия+JSON)"_q;
			entry.phone = accPtr->session().user()->phone();
			entry.name = accPtr->session().user()->name();
			entry.username = accPtr->session().user()->username();
			entry.folder = accPtr->folderTag();
			entry.twoFactor = accPtr->twoFactorPassword();
			const auto &p = accPtr->proxyOverride();
			if (p.type != MTP::ProxyData::Type::None && !p.host.isEmpty()) {
				entry.proxy = QJsonObject{
					{ "type", int(p.type) },
					{ "host", p.host },
					{ "port", int(p.port) },
					{ "user", p.user },
					{ "password", p.password },
				};
			}
			entry.device = DeviceOverrideToJson(EffectiveDeviceOverride(accPtr));
			entry = Sessions::AddStored(entry);
			auto err = QString();
			Storage::ExportRawAuthorizationAsTdata(
				accPtr->serializeMtpAuthorization(),
				Sessions::VaultFolder(entry),
				&err);
			Sessions::WriteSessionVaultSidecar(entry);
		}

		AddAccounts::Inform(
			show,
			u"Успешно импортировано аккаунтов (Сессия + JSON): %1."_q
				.arg(imported.size()));
	};

	// Check if any accounts have no proxy in their JSON
	const auto needsProxy = std::any_of(notYetAdded.begin(), notYetAdded.end(), [](const SessionJsonAccount &a) {
		return a.proxy.type == MTP::ProxyData::Type::None;
	});

	if (needsProxy) {
		show->showBox(MakeMultiProxyBox([=](Fn<MTP::ProxyData()> nextProxy) {
			runImport(nextProxy);
		}));
	} else {
		runImport([] { return MTP::ProxyData(); });
	}
}

} // namespace Settings
