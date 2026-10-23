/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "mtproto/mtproto_proxy_data.h"
#include "mtproto/mtp_instance.h"

#include <QtCore/QString>
#include <QtCore/QByteArray>
#include <QtCore/QJsonObject>
#include <vector>
#include <optional>
#include <memory>

namespace Ui {
class Show;
} // namespace Ui

namespace Main {
class Account;
} // namespace Main

namespace Settings {

struct SessionData {
	qint32 dcId = 2;
	quint64 userId = 0;
	QByteArray authKey; // 256 bytes
};

// Writes a Telethon-compatible SQLite .session file
[[nodiscard]] bool WriteTelethonSession(
	const QString &sessionPath,
	qint32 dcId,
	const QByteArray &authKey,
	quint64 userId = 0,
	const QString &phone = QString(),
	const QString &username = QString(),
	const QString &name = QString(),
	QString *outError = nullptr);

// Reads a Telethon or Pyrogram SQLite .session file
[[nodiscard]] std::optional<SessionData> ReadTelethonSession(
	const QString &sessionPath);

struct SessionJsonAccount {
	QString sessionPath;
	QString jsonPath;
	qint32 mainDcId = 2;
	quint64 userId = 0;
	QByteArray authKey; // 256 bytes
	std::vector<std::pair<qint32, QByteArray>> allKeys;
	QString phone;
	QString firstName;
	QString lastName;
	QString username;
	QString twoFactor;
	QString folder;
	bool pinned = false;
	MTP::ProxyData proxy;
	MTP::DeviceOverride device;
	QString accountDir;
};

// Recursively scans a directory for accounts packaged as .session + .json or .json with auth_key
[[nodiscard]] std::vector<SessionJsonAccount> ScanSessionJsonDirectory(
	const QString &folderPath);

// Exports an account as Session + JSON (<phone>.session, <phone>.json, 2fa.txt, ggtg.json)
[[nodiscard]] bool ExportAccountAsSessionJson(
	not_null<Main::Account*> account,
	const QString &accountFolder,
	const QByteArray &serializedAuth,
	std::optional<MTP::DeviceOverride> customDevice = std::nullopt,
	QString *outError = nullptr);

// Opens file dialog to import Session + JSON folder into the app
void ImportSessionJsonFolder(std::shared_ptr<Ui::Show> show);

} // namespace Settings

