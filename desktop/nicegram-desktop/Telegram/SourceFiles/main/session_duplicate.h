/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "mtproto/mtp_instance.h"
#include <optional>

namespace Main {

class Account;

// Mints a genuinely new, independent MTProto session for `account`'s
// already-logged-in user - its own auth key, its own entry in Telegram's
// "Active Sessions" (revocable there on its own, unaffected by anything
// that happens to `account`'s own session) - via the same auth.
// exportLoginToken/auth.acceptLoginToken pair a QR-code login normally
// uses, just without any camera or second device: a throwaway, never-
// persisted MTP::Instance plays the "new device" role locally in this same
// process, and `account`'s own live session plays the "phone that scanned
// the code" role, confirming it immediately.
//
// The throwaway instance is never added as a Main::Account/Domain slot -
// on success, `done` receives the resulting session already serialized
// (Main::SerializeMtpAuthorization()'s format, ready for
// Storage::ExportRawAuthorizationAsTdata()) and the instance is torn down
// before `done` is even called. `account`'s own session is never touched
// beyond the one accept call.
//
// If the account has a cloud password (2FA) set, completing this needs it.
// account->twoFactorPassword() (this app's own locally-stored copy, if any)
// is tried first automatically; if that's missing or turns out wrong,
// `requestPassword` is invoked with the two ways to proceed - call its
// `submit` with a password to try that one instead (wrong again loops back
// to another `requestPassword` call, right away finishes normally), or
// call `cancel` to give up on this account without trying further. The
// caller is expected to ask the person operating this app - this account's
// own password isn't guessable from anything else stored here. Pass
// nullptr for `requestPassword` to skip straight to failing when the
// stored password (if any) doesn't work, e.g. for a context with nobody to
// ask.
//
// Uses the account's current proxy override, if it has one, for the
// throwaway instance's own connection too (not just the exported blob it
// produces) - this account should never suddenly connect from this
// machine's raw IP mid-duplication, matching everywhere else in this
// feature that carries the proxy along (settings_information.cpp's
// ExportAccountsSequential()).
//
// This makes real, live requests against Telegram's servers using the
// account's real authorization - unlike everything else in the Move/
// `device`: the device identity the new session is born with (what Telegram
// records for it). Default = this account's own, so a copy of the session
// is the same "device" as the account itself; the Android export passes
// the Android phone the account will have there. `asAndroid` also makes the
// session be born as GGTG Android (its api id / hash / version).
// Export-by-duplicating-app-copy features, which are pure local
// filesystem/IPC and touch no server at all. `done` is always called on
// the main thread; nothing here blocks it.
void DuplicateSession(
	not_null<Account*> account,
	Fn<void(Fn<void(QString password)> submit, Fn<void()> cancel)>
		requestPassword,
	Fn<void(QString error, QByteArray serializedAuth)> done,
	std::optional<MTP::DeviceOverride> device = std::nullopt,
	bool asAndroid = false);

// The numbers Telegram gives the sessions in `account`'s "Active sessions"
// list (the one this app is running as is not in it under its own number).
// `error` is empty on success.
void FetchSessionHashes(
	not_null<Account*> account,
	Fn<void(std::vector<uint64> hashes, QString error)> done);

// How Telegram records the session with this number: "device, app version,
// app name" (empty on any failure). Used to show, right after an export, what
// the new session looks like in the account's "Active sessions" list.
void FetchSessionDescription(
	not_null<Account*> account,
	uint64 hash,
	Fn<void(QString description)> done);

// DuplicateSession() that also finds out Telegram's number of the session it
// made: the list is read before and after, and the entry that appeared is
// it (0 if that could not be told). `done` gets the new session serialized,
// as DuplicateSession() does.
void DuplicateSessionTracked(
	not_null<Account*> account,
	Fn<void(Fn<void(QString password)> submit, Fn<void()> cancel)>
		requestPassword,
	Fn<void(QString error, QByteArray serializedAuth, uint64 hash)> done,
	std::optional<MTP::DeviceOverride> device = std::nullopt,
	bool asAndroid = false);

} // namespace Main
