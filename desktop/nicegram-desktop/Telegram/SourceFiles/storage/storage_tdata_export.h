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

namespace MTP {
struct ProxyData;
} // namespace MTP

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

// Writes tdata's own "settings" file (tdata/settingss) next to an exported
// folder's key and data files, holding just the account's proxy - the file a
// real Telegram Desktop keeps its settings (proxy list included) in, so an
// ordinary Telegram opening this tdata shows and uses the same proxy.
// `tdataFolder` is the "tdata" folder itself. Does nothing (returns true)
// for an empty proxy.
[[nodiscard]] bool WriteTdataProxySettings(
	const QString &tdataFolder,
	const MTP::ProxyData &proxy,
	QString *error = nullptr);

} // namespace Storage
