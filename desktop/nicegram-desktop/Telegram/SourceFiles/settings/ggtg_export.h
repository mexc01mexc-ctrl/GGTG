/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace Ui {
class BoxContent;
class GenericBox;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

namespace Main {
class Account;
} // namespace Main

namespace MTP {
struct ProxyData;
} // namespace MTP

namespace Settings {

struct ParsedAuthorization {
	quint64 userId = 0;
	qint32 mainDcId = 0;
	std::vector<std::pair<qint32, QByteArray>> keys;
};

[[nodiscard]] std::optional<ParsedAuthorization> ParseAuthorization(
	const QByteArray &serialized);

[[nodiscard]] QJsonObject ProxyToJson(const MTP::ProxyData &proxy);

[[nodiscard]] bool WriteGgtgPack(
	const QString &path,
	const QJsonArray &accounts);

[[nodiscard]] QJsonArray ReadGgtgPack(const QString &path);

[[nodiscard]] bool ExportAccountToGgtg(
	const QString &path,
	const QByteArray &serializedAuth,
	const QString &phone,
	const QString &firstName,
	const QString &lastName,
	const QString &username,
	const QString &twoFactor,
	const MTP::ProxyData &proxy,
	const QJsonObject &device,
	const QString &folder = QString(),
	bool append = false,
	QString *outError = nullptr);

// The GGTG "Экспорт" section: pick accounts (by folder), then export them
// as one .ggtg file for GGTG Android. Every account first gets a brand-new,
// independent session (Main::DuplicateSession()), so the exported copy and
// this app keep working side by side. The .ggtg file is a zip with a
// manifest.json holding each duplicated session's raw auth keys plus the
// account's proxy, folder and 2FA password - GGTG Android installs those
// keys directly, with no second duplication on the phone. Format:
// docs/ggtg-formats.md.
[[nodiscard]] object_ptr<Ui::BoxContent> MakeGgtgExportBox(
	not_null<Window::SessionController*> controller,
	bool viaSync = false);

// ---- Exporting accounts as tdata folders ----
//
// The rule over all of it: a session should only ever be used in one place,
// and every session the app makes or hands out is kept in its session vault
// (labelled in the account's session list, never ended by "end other
// sessions", and the account can be switched onto it).

// Format of the export.
enum class ExportFormat {
	Tdata,
	SessionJson,
};

// What goes into the export.
enum class TdataSource {
	Working, // the main session; the app moves onto a spare if it has one
	Backup, // a stored spare; it becomes a "duplicate" (still kept here)
	New, // a new session made for the export, kept here as a "duplicate"
};

// Which stored spare is handed out.
enum class TdataBackupPick {
	Oldest,
	Newest,
};

struct TdataExportOptions {
	ExportFormat format = ExportFormat::Tdata;
	TdataSource source = TdataSource::Working;
	TdataBackupPick pick = TdataBackupPick::Oldest;
	bool newWhenNoBackup = false; // TdataSource::Backup without a spare
	bool sidecars = true; // 2fa.txt and ggtg.json next to tdata
};

// Adds the "what to export / what the app keeps" choices to `box`; the
// returned getter reads them. `withAndroid` adds a fourth source, the .ggtg
// file for GGTG Android - the getter gives std::nullopt for it.
[[nodiscard]] Fn<std::optional<TdataExportOptions>()> AddTdataExportChoices(
	not_null<Ui::GenericBox*> box,
	bool withAndroid);

// Where each account's tdata goes, once a destination folder is picked.
struct TdataExportItem {
	not_null<Main::Account*> account;
	QString accountFolder;
};
using TdataExportLayout = Fn<std::vector<TdataExportItem>(
	const QString &destination)>;

// Shows what will happen (how many accounts, spares, new logins), asks for
// the destination folder and runs the export one account at a time.
// Defined in settings_information.cpp.
void StartTdataExport(
	not_null<Window::SessionController*> window,
	std::vector<not_null<Main::Account*>> accounts,
	TdataExportOptions options,
	const QString &title,
	TdataExportLayout layout);

// The "Экспорт" section's layout: <destination>/<folder>/<phone>/tdata.
void ExportAccountsAsTdata(
	not_null<Window::SessionController*> window,
	std::vector<not_null<Main::Account*>> accounts,
	TdataExportOptions options);

} // namespace Settings
