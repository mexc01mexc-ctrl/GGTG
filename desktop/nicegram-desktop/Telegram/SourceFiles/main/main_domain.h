/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/flat_set.h"
#include "base/timer.h"
#include "base/weak_ptr.h"
#include "data/data_peer_id.h"
#include "storage/storage_tdata_import.h"

namespace Storage {
class Domain;
class PasscodeDerivation;
enum class StartResult : uchar;
} // namespace Storage

namespace MTP {
enum class Environment : uchar;
struct ProxyData;
} // namespace MTP

namespace Wallet {
class VaultRuntime;
} // namespace Wallet

namespace Main {

class Account;
class Session;

// Randomized delay (~3-7s) used to stagger connecting or disconnecting
// several accounts in bulk (app startup, tdata import, forgotten-passcode
// logout) so they don't all hit the server within the same instant.
[[nodiscard]] crl::time NextAccountStaggerDelay();

class Domain final : public base::has_weak_ptr {
public:
	struct AccountWithIndex {
		int index = 0;
		std::unique_ptr<Account> account;
	};

	static constexpr auto kMaxAccounts = 500;
	static constexpr auto kPremiumMaxAccounts = 500;

	explicit Domain(const QString &dataName);
	~Domain();

	[[nodiscard]] bool started() const;
	[[nodiscard]] Storage::StartResult start(const QByteArray &passcode);

	// One passcode attempt at a time for the whole application: while the
	// derivation runs on a worker this returns true, and every further call
	// answers false without starting anything or ever invoking its done.
	// The verdict is applied here on the main thread - a cold attempt starts
	// the domain, a warm one only checks the passcode - and done(correct)
	// runs last. An attempt whose started-ness changed underneath it (the
	// lock screen's Log out started the domain from scratch, or finish() ran
	// during shutdown) applies nothing and reports the passcode incorrect.
	// An attempt answered while the application is quitting applies nothing
	// and drops its done as well: finish() leaves a cold domain as unstarted
	// as it found it, so started-ness alone cannot tell a quitting cold
	// attempt from one that should start every account. An attempt that
	// outlives the domain is dropped together with its done.
	[[nodiscard]] bool tryPasscode(
		const QByteArray &passcode,
		Fn<void(bool correct)> done);

	void resetWithForgottenPasscode();
	void finish();

	[[nodiscard]] int maxAccounts() const;
	[[nodiscard]] rpl::producer<int> maxAccountsChanges() const;

	[[nodiscard]] Storage::Domain &local() const {
		return *_local;
	}

	[[nodiscard]] Wallet::VaultRuntime &walletKeyring();

	[[nodiscard]] auto accounts() const
		-> const std::vector<AccountWithIndex> &;
	[[nodiscard]] std::vector<not_null<Account*>> orderedAccounts() const;
	// UI state that keeps a raw Account* (a selection in an open box, a
	// running campaign) goes stale when that account is removed from the
	// app, and touching it then is a crash. True only while `account` is
	// still one of this domain's accounts and has a live session; it
	// compares pointers and never dereferences a removed one.
	[[nodiscard]] bool isLive(not_null<Account*> account) const;
	[[nodiscard]] rpl::producer<Account*> activeValue() const;
	[[nodiscard]] rpl::producer<> accountsChanges() const;
	[[nodiscard]] Account *maybeLastOrSomeAuthedAccount();
	[[nodiscard]] int accountsAuthedCount() const;

	// Expects(started());
	[[nodiscard]] Account &active() const;
	[[nodiscard]] rpl::producer<not_null<Account*>> activeChanges() const;

	[[nodiscard]] rpl::producer<Session*> activeSessionValue() const;
	[[nodiscard]] rpl::producer<Session*> activeSessionChanges() const;

	[[nodiscard]] int unreadBadge() const;
	[[nodiscard]] bool unreadBadgeMuted() const;
	[[nodiscard]] rpl::producer<> unreadBadgeChanges() const;
	void notifyUnreadBadgeChanged();

	[[nodiscard]] not_null<Main::Account*> add(
		MTP::Environment environment,
		const MTP::ProxyData &proxy);
	void maybeActivate(not_null<Main::Account*> account);
	void activate(not_null<Main::Account*> account);
	void addActivated(
		MTP::Environment environment,
		const MTP::ProxyData &proxy,
		bool newWindow = false);

	// Imports every account found in a foreign tdata folder (e.g. copied
	// in from another install - it can hold several, same as this app's
	// own) as brand new logged-in accounts, skipping phone/code login
	// entirely. Stops early if the account limit is reached, so this can
	// return fewer accounts than the folder actually holds. Returns an
	// empty vector if the folder didn't contain any readable
	// authorization, or every account in it is already added elsewhere.
	// nextProxy() is called once per account, right before it's added, and
	// its result is applied *before* that account's very first connection
	// attempt, so that connection never happens unproxied - pass a
	// generator that hands out a different proxy per call (e.g. from a
	// pool list) if each account needs its own.
	// onImported(), if given, fires once per account actually added, with
	// the exact on-disk folder its authorization was read from (one of
	// folderPath itself or a candidate found a few levels under it - see
	// Storage::ReadForeignTdataAuthorizations) - lets a caller locate
	// something else sitting alongside that specific account's own tdata
	// (e.g. Settings::AccountsList's 2FA-sidecar lookup) without having to
	// re-guess which of possibly several candidate folders it came from.
	std::vector<not_null<Main::Account*>> addFromTdata(
		const QString &folderPath,
		MTP::Environment environment,
		Fn<MTP::ProxyData()> nextProxy,
		Fn<void(not_null<Main::Account*>, const QString &folder)> onImported
			= nullptr);

	// The same for authorizations that are already read (a tdata folder or
	// a transfer from another GGTG device); `folder` may be empty.
	std::vector<not_null<Main::Account*>> addFromAuthorizations(
		const std::vector<Storage::ForeignTdataAuthorization> &authorizations,
		MTP::Environment environment,
		Fn<MTP::ProxyData()> nextProxy,
		Fn<void(not_null<Main::Account*>, const QString &folder)> onImported
			= nullptr);

	// Drops session-less accounts that have no window open for them.
	void removeRedundantAccounts();

	// Scans this app's own tdata/ for account indices with genuine session
	// data that aren't currently loaded - see Storage::Domain::
	// recoverOrphanedAccounts()'s own comment for why a slot can end up
	// like that (the write-timing gap addFromTdata() used to have, fixed
	// now, but not retroactively for accounts already lost to it). Wires
	// up watchSession() and fires accountsChanges() for whatever it finds,
	// the same way activateAfterStarting() already does for accounts
	// loaded at launch, so the UI picks them up immediately instead of
	// needing a restart. Returns how many were recovered.
	int recoverOrphanedAccounts();

	// Moves tdata/ files and folders that belong to no loaded account into
	// a "_removed_<time>" subfolder - see Storage::Domain::cleanupTdata().
	int cleanupTdata(QString *folder);

	// True if another loaded account with this same Telegram user id also
	// uses one of this account's auth keys (a real conflict - see the
	// definition for why a shared user id alone is not).
	[[nodiscard]] bool sharesAuthKeyWithOther(
		not_null<Account*> account,
		UserId userId) const;

	// True if some other already-authorized account (besides "except", if
	// given) has this same Telegram user id. Two accounts sharing one real
	// Telegram user fight over the same MTProto session and the server
	// force-resets both, so this is used to reject that before it happens.
	[[nodiscard]] bool hasAccountWithUserId(
		UserId userId,
		Account *except) const;

	// Accounts waiting to start (see Storage::Domain::startPending): they
	// start a batch at a time after launch.
	[[nodiscard]] int pendingAccounts() const;
	[[nodiscard]] rpl::producer<int> pendingAccountsValue() const;

	// Interface for Storage::Domain.
	void accountAddedInStorage(AccountWithIndex accountWithIndex);
	void activateFromStorage(int index);
	[[nodiscard]] int activeForStorage() const;

private:
	[[nodiscard]] Storage::StartResult startWith(
		Storage::PasscodeDerivation derived);
	void activateAfterStarting();
	void closeAccountWindows(not_null<Main::Account*> account);

	// Answers whether a removal ran whose checked write already persisted
	// the current accounts info - not whether a passcode is gone, which
	// would make the no-passcode case skip the caller's accounts write.
	bool removePasscodeIfEmpty();

	// True when a completed last logout has left the passcode guarding
	// nothing that can still be asked for: one account, its session gone,
	// and a passcode still installed. The one definition both the logout
	// call site and the retry read.
	[[nodiscard]] bool passcodeRemovalAuthorized() const;
	bool clearPasscodeAfterLastLogout();
	void reportFailedPasscodeClear();

	// Retries the removal a completed last logout authorized, after its
	// checked write did not reach the disk. The authorization is re-read
	// here and never carried over from the failed attempt, so a passcode
	// already gone, an account signed in again or a second account all
	// make this do nothing at all. Answers whether this call removed the
	// passcode, so the caller can say so.
	[[nodiscard]] bool finishPasscodeClearAfterReset();

	void watchSession(not_null<Account*> account);
	void scheduleWriteAccounts();
	void checkForLastProductionConfig(not_null<Main::Account*> account);
	void updateUnreadBadge();
	void scheduleUpdateUnreadBadge();
	void suggestExportIfNeeded();
	[[nodiscard]] bool indexTaken(int index) const;
	void checkStartBatch();
	void startNextBatch();

	const QString _dataName;
	const std::unique_ptr<Storage::Domain> _local;

	std::shared_ptr<Wallet::VaultRuntime> _walletKeyring;
	std::vector<AccountWithIndex> _accounts;
	rpl::event_stream<> _accountsChanges;
	rpl::variable<Account*> _active = nullptr;
	int _accountToActivate = -1;
	int _lastActiveIndex = -1;
	bool _writeAccountsScheduled = false;

	rpl::event_stream<Session*> _activeSessions;

	rpl::event_stream<> _unreadBadgeChanges;
	int _unreadBadge = 0;
	bool _unreadBadgeMuted = true;
	bool _unreadBadgeUpdateScheduled = false;
	bool _passcodeDeriving = false;

	rpl::variable<int> _lastMaxAccounts;

	rpl::variable<int> _pendingAccounts = 0;
	std::vector<base::weak_ptr<Account>> _startBatch;
	crl::time _startBatchAt = 0;
	base::Timer _startBatchTimer;

	rpl::lifetime _activeLifetime;
	rpl::lifetime _lifetime;

};

// Shorthand for Core::App().domain().isLive(account).
[[nodiscard]] bool AccountIsLive(not_null<Account*> account);

// The accounts from `chosen` that are still live, in the app's own order.
[[nodiscard]] std::vector<not_null<Account*>> LiveAccounts(
	const base::flat_set<not_null<Account*>> &chosen);

} // namespace Main
