/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QString>

#include <map>
#include <optional>

namespace Core::GgtgSync {

// The sync of GGTG data between devices (this Mac, GGTG Android) through one
// "main" Telegram account: each device puts an encrypted snapshot of its
// data into that account's "Saved messages", and merges the snapshots of the
// other devices into its own. This file is the format and the merge; the
// transport is in core/ggtg_sync_transport.cpp. Format: docs/ggtg-formats.md.
//
// What travels: the bases (people and groups), the registry of who was
// written to (with the base each person came from), what was deleted on
// purpose, and the AI side (profile, examples, playbook, training, settings,
// materials, chat lists).

struct Settings {
	bool enabled = false;
	// Who is the main account (user id), the passphrase that encrypts the
	// snapshots, and this device's own random id.
	QString mainUserId;
	QString passphrase;
	QString deviceId;
	qint64 lastSyncAt = 0;
	QString lastResult;
	// Hash of the last snapshot put up, so unchanged data is not sent again.
	QString lastUploadedHash;
	// The newest message of each other device that was already merged.
	std::map<QString, qint64> seen;
};

[[nodiscard]] Settings LoadSettings();
void SaveSettings(const Settings &settings);

// The snapshot as JSON (see docs/ggtg-formats.md), and a hash of its
// contents (without the time it was made) to tell whether it changed.
struct Snapshot {
	QByteArray json;
	QString hash;
};
[[nodiscard]] Snapshot BuildSnapshot(const Settings &settings);

struct MergeResult {
	bool ok = false;
	QString error;
	int bases = 0;
	int sentAdded = 0;
	int sentRemoved = 0;
	int files = 0;
};

// Merges another device's snapshot into the local data.
[[nodiscard]] MergeResult MergeSnapshot(const QByteArray &json);

// compress + encrypt (AES-256-GCM, key from the passphrase) and back.
[[nodiscard]] QByteArray Pack(
	const QByteArray &json,
	const QString &passphrase);
[[nodiscard]] std::optional<QByteArray> Unpack(
	const QByteArray &blob,
	const QString &passphrase);

[[nodiscard]] QString ResultText(const MergeResult &result);

// A short fingerprint of the phrase ("A1B2C3"): it is in the snapshot's file
// name, so a device only opens the files made with its own phrase, and the
// user can compare it on both devices.
[[nodiscard]] QString KeyId(const QString &passphrase);

// A random phrase for the QR pairing: nobody has to type it.
[[nodiscard]] QString NewPassphrase();
// What the QR code shown to GGTG Android holds: the main account and the
// phrase, "ggtgsync:1:<user id>:<phrase as base64url of UTF-8>". The phone
// that scans it is set up and starts syncing on its own.
[[nodiscard]] QString PairingText(const Settings &settings);

} // namespace Core::GgtgSync
