/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace Storage {

struct ForeignTdataAuthorization {
	// In the exact format Main::Account::setMtpAuthorization() expects.
	QByteArray data;
	// The actual candidate folder this came from - folderPath itself, or
	// one CollectTdataFolderCandidates() found a few levels under it (see
	// ReadForeignTdataAuthorizations()'s own comment) - not necessarily
	// folderPath verbatim. Lets a caller that needs to read something else
	// sitting alongside this account's own tdata (e.g. Settings::
	// AccountsList's 2FA-sidecar lookup) find the right folder without
	// re-deriving it.
	QString folder;
};

// Reads a *foreign* tdata folder (e.g. copied in from another install) and
// extracts the raw "mtp authorization" blob for every account stored in it
// (a tdata folder can hold several, same as this app's own), in the exact
// format Main::Account::setMtpAuthorization() expects - this is the on-disk
// format Main::Account::serializeMtpAuthorization()/writeMtpData() already
// produce, read back with the same primitives Storage::Domain and
// Storage::Account use for their own (non-foreign) files, just pointed at
// an external folder instead of our own tdata/. Returns an empty vector if
// nothing readable was found (unreadable folder, wrong passcode, ...).
[[nodiscard]] std::vector<ForeignTdataAuthorization> ReadForeignTdataAuthorizations(
	const QString &folderPath,
	const QByteArray &passcode = QByteArray());

} // namespace Storage
