/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/basic_types.h"

#include <QtCore/QByteArray>
#include <QtCore/QString>

#include <vector>

namespace Main {
class Session;
} // namespace Main

namespace Core::GgtgSync {

struct Report {
	bool ok = false;
	QString text;
};

// A file of this transfer in the main account's "Saved messages". `kind` is
// "sync" (a data snapshot), "pack" (accounts for GGTG Android, a .ggtg) or
// "keys" (accounts for GGTG Desktop, a list of new sessions).
struct Remote {
	qint64 msgId = 0;
	QString kind;
	QString device;
	// KeyId() of the phrase the file was made with ("" in older files).
	QString key;
	TimeId date = 0;
	uint64 docId = 0;
	uint64 accessHash = 0;
	QByteArray reference;
	int dcId = 0;
	qint64 size = 0;
};

// The session of the main account, if it is signed in in this app.
[[nodiscard]] Main::Session *MainSession();

// One round: merge what the other devices put in the main account's
// "Saved messages", then put this device's own snapshot there if it changed.
// `done` is called once on the main thread (the upload itself goes on in the
// background).
void RunSync(Fn<void(Report)> done = nullptr);
[[nodiscard]] bool Running();

// Accounts other devices have put there for this one (`kind` as above).
void FindIncoming(Fn<void(std::vector<Remote>, QString error)> done);
// Puts accounts for another device there: the payload is compressed and
// encrypted with the phrase; `kind` is "pack" or "keys".
void SendAccounts(
	const QString &kind,
	const QByteArray &payload,
	Fn<void(bool ok, QString text)> done);
// Downloads, decrypts and (when it worked) deletes one of them.
void FetchIncoming(
	const Remote &remote,
	Fn<void(QByteArray payload, QString error)> done);

// Called for every session that is created: starts the periodic sync when
// the session is the main account's.
void SessionStarted(not_null<Main::Session*> session);

// Called when the settings were changed.
void Restart();

// Before a mailing starts: when the sync is on, first takes in what the
// other device sent (so nobody it already wrote to is written to again),
// then calls `proceed` - also when the sync is off, failed or took too long.
void SyncBeforeMailing(Fn<void()> proceed);
// After a mailing: puts the new "уже написали" up a few seconds later.
void SyncSoon();

} // namespace Core::GgtgSync
