/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace Main {
class Account;
} // namespace Main

namespace Storage {

// Writes a standalone, passcode-less tdata folder containing just this one
// account's MTP authorization - readable back via our own tdata-import
// feature (ReadForeignTdataAuthorizations) or by a real Telegram Desktop
// install. The live app's own storage is untouched; this is a plain copy.
[[nodiscard]] bool ExportAccountAsTdata(
	not_null<Main::Account*> account,
	const QString &folderPath,
	QString *error = nullptr);

// The disk-writing half of ExportAccountAsTdata(), taking an already-
// serialized authorization blob (Main::SerializeMtpAuthorization()'s
// format) directly instead of reading one off a live Account. Used by
// Main::DuplicateSession() (main/session_duplicate.h) to export a freshly
// minted, independent second session for an account without ever adding
// that session as a real Account/Domain slot here first.
[[nodiscard]] bool ExportRawAuthorizationAsTdata(
	const QByteArray &serializedAuth,
	const QString &folderPath,
	QString *error = nullptr);

} // namespace Storage
