/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "mtproto/mtproto_auth_key.h"
#include "mtproto/mtp_instance.h"
#include "base/weak_ptr.h"

namespace Storage {
class Account;
class Domain;
enum class StartResult : uchar;
} // namespace Storage

namespace MTP {
class AuthKey;
class Config;
} // namespace MTP

namespace Main {

class Domain;
class Session;
class SessionSettings;
class AppConfig;

// Reads just the user id out of a serializeMtpAuthorization()-shaped blob,
// without touching any account/session state - used to reject importing a
// tdata folder for a user that's already added, before writing anything
// to disk for it. Returns UserId() if the blob can't be parsed.
[[nodiscard]] UserId PeekAuthorizationUserId(const QByteArray &serialized);

// The actual serialization Account::serializeMtpAuthorization() writes,
// pulled out as a free function so it can also be used against a raw
// MTP::Instance that isn't wrapped in an Account at all - see
// Main::DuplicateSession() (main/session_duplicate.h), which mints an
// independent second session for an already-logged-in user via a
// throwaway, never-persisted Instance and needs this exact on-disk format
// for the result, just without an Account to read it off of. `userId`
// isn't derivable from a session-less Instance the way
// Account::serializeMtpAuthorization() reads it off its own session, so
// callers pass it in directly.
[[nodiscard]] QByteArray SerializeMtpAuthorization(
	UserId userId,
	MTP::DcId mainDcId,
	const MTP::AuthKeysList &keys,
	const MTP::AuthKeysList &keysToDestroy = {});

class Account final : public base::has_weak_ptr {
public:
	Account(not_null<Domain*> domain, const QString &dataName, int index);
	~Account();

	[[nodiscard]] Domain &domain() const {
		return *_domain;
	}

	[[nodiscard]] Storage::Domain &domainLocal() const;

	[[nodiscard]] Storage::StartResult legacyStart(
		const QByteArray &passcode);
	[[nodiscard]] std::unique_ptr<MTP::Config> prepareToStart(
		std::shared_ptr<MTP::AuthKey> localKey);
	void prepareToStartAdded(
		std::shared_ptr<MTP::AuthKey> localKey);
	void start(std::unique_ptr<MTP::Config> config);

	[[nodiscard]] uint64 willHaveSessionUniqueId(MTP::Config *config) const;
	void createSession(
		const MTPUser &user,
		std::unique_ptr<SessionSettings> settings = nullptr);
	void createSession(
		UserId id,
		QByteArray serialized,
		int streamVersion,
		std::unique_ptr<SessionSettings> settings);

	void logOut();
	void forcedLogOut();
	[[nodiscard]] bool loggingOut() const;

	[[nodiscard]] AppConfig &appConfig() const {
		Expects(_appConfig != nullptr);

		return *_appConfig;
	}

	[[nodiscard]] Storage::Account &local() const {
		return *_local;
	}

	[[nodiscard]] bool sessionExists() const;
	[[nodiscard]] Session &session() const;
	[[nodiscard]] Session *maybeSession() const;
	[[nodiscard]] bool destroyingSession() const;
	[[nodiscard]] rpl::producer<Session*> sessionValue() const;
	[[nodiscard]] rpl::producer<Session*> sessionChanges() const;

	[[nodiscard]] MTP::Instance &mtp() const {
		return *_mtp;
	}

	// Per-account proxy override - each account can connect through its own
	// proxy independently of the app-wide "Settings > Advanced" proxy.
	[[nodiscard]] MTP::ProxyData proxyOverride() const;
	void setProxyOverride(const MTP::ProxyData &proxy);

	// Arbitrary user-defined folder name for organizing the accounts list.
	// Empty string means the account is not in any folder.
	[[nodiscard]] QString folderTag() const;
	void setFolderTag(const QString &folder);

	// Pinned accounts sort first within their group (folder or ungrouped).
	[[nodiscard]] bool pinned() const;
	void setPinned(bool pinned);

	// In the account manager's «Корзина»: still a working account, only
	// listed apart from the others until restored or deleted for good.
	[[nodiscard]] bool trashed() const;
	void setTrashed(bool trashed);

	// A trashed account is «asleep»: it is loaded (name, chats, userpic) but
	// has no network connection - so no notifications and no dock badge -
	// until wake(): it is opened in the manager, chosen in Squidward or used
	// by a campaign. The window it is open in is always awake.
	[[nodiscard]] bool asleep() const;
	void sleep();
	void wake();

	// Suppresses this account's own notifications (message/reaction/poll
	// alerts and sounds), independent of any chat's own mute state and of
	// the app-wide "notify from all accounts" setting.
	[[nodiscard]] bool muted() const;
	void setMuted(bool muted);

	// Forces this account to always report itself as offline to the server
	// (Api::Updates::updateOnline checks this), regardless of whether the
	// app window is actually focused/active for it.
	[[nodiscard]] bool alwaysOffline() const;
	// When start() ran: an account still connecting shortly after that is
	// "подключается", not "нет связи".
	[[nodiscard]] crl::time startedAt() const {
		return _startedAt;
	}
	void setAlwaysOffline(bool alwaysOffline);

	// The account's two-step verification password, kept here purely so it
	// isn't lost - e.g. picked up automatically from a sibling file next to
	// an imported tdata folder (see AccountsList's import flow), or entered
	// by hand from the account's context menu. Not used to actually log in
	// or unlock anything; Telegram itself still asks for it as normal.
	// This account's own device name and OS as the server sees them (the
	// "Devices" list of Telegram); empty = the program-wide / platform value.
	[[nodiscard]] MTP::DeviceOverride deviceOverride() const;
	void setDeviceOverride(const MTP::DeviceOverride &value);
	[[nodiscard]] QString twoFactorPassword() const;
	void setTwoFactorPassword(const QString &password);
	[[nodiscard]] rpl::producer<not_null<MTP::Instance*>> mtpValue() const;

	// Each time the main session changes a new copy of the pointer is fired.
	// This allows to resend the requests that were not requiring auth, and
	// which could be forgotten without calling .done() or .fail() because
	// of the main dc changing.
	[[nodiscard]] auto mtpMainSessionValue() const
		-> rpl::producer<not_null<MTP::Instance*>>;

	// Set from legacy storage.
	void setLegacyMtpKey(std::shared_ptr<MTP::AuthKey> key);

	void setMtpMainDcId(MTP::DcId mainDcId);

	// In-memory-only proxy seed for an account that hasn't start()-ed
	// yet: setProxyOverride() below needs the account's own storage key
	// (only ready *after* start()), so it can't be used before that
	// point. This just primes _mtpFields so startMtp() picks it up and
	// the very first connection already goes through the proxy.
	void setMtpProxyOverride(const MTP::ProxyData &proxy);
	void setSessionUserId(UserId userId);
	void setSessionFromStorage(
		std::unique_ptr<SessionSettings> data,
		QByteArray &&selfSerialized,
		int32 selfStreamVersion);
	[[nodiscard]] SessionSettings *getSessionSettings();
	[[nodiscard]] rpl::producer<> mtpNewSessionCreated() const;
	[[nodiscard]] rpl::producer<MTPUpdates> mtpUpdates() const;

	// Serialization.
	[[nodiscard]] QByteArray serializeMtpAuthorization() const;
	void setMtpAuthorization(const QByteArray &serialized);

	void suggestMainDcId(MTP::DcId mainDcId);
	void destroyStaleAuthorizationKeys();

	void setHandleLoginCode(Fn<void(QString)> callback);
	void handleLoginCode(const QString &code) const;

	[[nodiscard]] rpl::lifetime &lifetime() {
		return _lifetime;
	}

private:
	static constexpr auto kDefaultSaveDelay = crl::time(1000);
	enum class DestroyReason {
		Quitting,
		LoggedOut,
	};

	void startMtp(std::unique_ptr<MTP::Config> config);
	void createSession(
		const MTPUser &user,
		QByteArray serialized,
		int streamVersion,
		std::unique_ptr<SessionSettings> settings);
	void watchProxyChanges();
	void watchSessionChanges();
	bool checkForUpdates(const MTP::Response &message);
	bool checkForNewSession(const MTP::Response &message);

	void destroyMtpKeys(MTP::AuthKeysList &&keys);
	void resetAuthorizationKeys();

	void loggedOut();
	void destroySession(DestroyReason reason);

	const not_null<Domain*> _domain;
	const std::unique_ptr<Storage::Account> _local;

	std::unique_ptr<MTP::Instance> _mtp;
	rpl::variable<MTP::Instance*> _mtpValue;
	std::unique_ptr<MTP::Instance> _mtpForKeysDestroy;
	rpl::event_stream<MTPUpdates> _mtpUpdates;
	rpl::event_stream<> _mtpNewSessionCreated;

	std::unique_ptr<AppConfig> _appConfig;

	std::unique_ptr<Session> _session;
	rpl::variable<Session*> _sessionValue;

	Fn<void(QString)> _handleLoginCode = nullptr;

	UserId _sessionUserId = 0;
	QByteArray _sessionUserSerialized;
	int32 _sessionUserStreamVersion = 0;
	std::unique_ptr<SessionSettings> _storedSessionSettings;
	MTP::Instance::Fields _mtpFields;
	MTP::AuthKeysList _mtpKeysToDestroy;
	bool _loggingOut = false;
	bool _destroyingSession = false;

	crl::time _startedAt = 0;
	bool _asleep = false;
	rpl::lifetime _lifetime;

};

} // namespace Main
