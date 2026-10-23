/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "main/main_domain.h"
#include "settings/bulk_accounts_actions.h"
#include <QtCore/QJsonDocument>
#include <QtCore/QFileInfo>
#include <QtCore/QDir>
#include <QtCore/QFile>

#include "base/call_delayed.h"
#include "base/random.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "core/process_manager.h"
#include "core/shortcuts.h"
#include "core/crash_reports.h"
#include "main/main_account.h"
#include "main/main_session.h"
#include "data/data_session.h"
#include "data/data_changes.h"
#include "data/data_user.h"
#include "mtproto/mtproto_config.h"
#include "mtproto/facade.h"
#include "mtproto/mtproto_dc_options.h"
#include "mtproto/mtproto_proxy_data.h"
#include "storage/storage_domain.h"
#include "storage/storage_account.h"
#include "storage/storage_tdata_import.h"
#include "storage/localstorage.h"
#include "export/export_settings.h"
#include "wallet/wallet_unlock.h"
#include "wallet/wallet_vault.h"
#include "window/notifications_manager.h"
#include "window/window_controller.h"
#include "data/data_peer_values.h" // Data::AmPremiumValue.

namespace Main {
namespace {

constexpr auto kAccountStaggerDelayMin = crl::time(3000);
constexpr auto kAccountStaggerDelayMax = crl::time(7000);

// Accounts left waiting at launch start this many at a time (see
// Storage::Domain::startPending), the next batch once the previous one has
// connected or after kStartBatchTimeout.
constexpr auto kStartBatchSize = 5;
constexpr auto kStartBatchCheck = crl::time(500);
constexpr auto kStartBatchTimeout = crl::time(15000);

} // namespace

crl::time NextAccountStaggerDelay() {
	return kAccountStaggerDelayMin + crl::time(base::RandomIndex(
		int(kAccountStaggerDelayMax - kAccountStaggerDelayMin) + 1));
}

Domain::Domain(const QString &dataName)
: _dataName(dataName)
, _local(std::make_unique<Storage::Domain>(this, dataName)) {
	_active.changes(
	) | rpl::take(1) | rpl::on_next([=] {
		// In case we had a legacy passcoded app we start settings here.
		Core::App().startSettingsAndBackground();

		crl::on_main(this, [=] {
			Core::App().notifications().createManager();
		});
	}, _lifetime);

	_active.changes(
	) | rpl::map([](Main::Account *account) {
		return account ? account->sessionValue() : rpl::never<Session*>();
		}) | rpl::flatten_latest(
	) | rpl::map([](Main::Session *session) {
		return session
			? session->changes().peerFlagsValue(
				session->user(),
				Data::PeerUpdate::Flag::Username)
			: rpl::never<Data::PeerUpdate>();
	}) | rpl::flatten_latest(
	) | rpl::on_next([](const Data::PeerUpdate &update) {
		CrashReports::SetAnnotation("Username", update.peer->username());
	}, _lifetime);
}

Domain::~Domain() {
	if (_walletKeyring) {
		_walletKeyring->clear();
	}
}

Wallet::VaultRuntime &Domain::walletKeyring() {
	if (!_walletKeyring) {
		_walletKeyring = std::make_shared<Wallet::VaultRuntime>(*this);
	}
	return *_walletKeyring;
}

bool Domain::started() const {
	return !_accounts.empty();
}

Storage::StartResult Domain::start(const QByteArray &passcode) {
	return startWith(_local->prepareOpen(passcode));
}

Storage::StartResult Domain::startWith(
		Storage::PasscodeDerivation derived) {
	Expects(!started());

	const auto result = _local->start(std::move(derived));
	if (result == Storage::StartResult::Success) {
		activateAfterStarting();
		Wallet::DropUnusedPasscode();
		crl::on_main(&Core::App(), [=] { suggestExportIfNeeded(); });
		_pendingAccounts = _local->pendingCount();
		if (_local->pendingCount()) {
			// The first batch started in _local->start() above.
			for (const auto &[index, account] : _accounts) {
				_startBatch.push_back(base::make_weak(account.get()));
			}
			_startBatchAt = crl::now();
			_startBatchTimer.setCallback([=] { checkStartBatch(); });
			_startBatchTimer.callEach(kStartBatchCheck);
		}
	} else {
		Assert(!started());
	}
	return result;
}

bool Domain::tryPasscode(
		const QByteArray &passcode,
		Fn<void(bool correct)> done) {
	if (_passcodeDeriving) {
		return false;
	}
	_passcodeDeriving = true;
	const auto cold = !started();
	Storage::DeriveOnWorker(
		_local->prepareOpen(passcode),
		crl::guard(this, [=](Storage::PasscodeDerivation &&derived) {
			_passcodeDeriving = false;
			if (Core::Quitting()) {
				return;
			}
			const auto correct = (cold == started())
				? false
				: cold
				? (startWith(std::move(derived))
					== Storage::StartResult::Success)
				: _local->checkPasscode(std::move(derived));
			done(correct);
		}));
	return true;
}

void Domain::finish() {
	_startBatchTimer.cancel();
	if (_walletKeyring) {
		_walletKeyring->clear();
	}
	_accountToActivate = -1;
	_active.reset(nullptr);
	base::take(_accounts);
}

void Domain::suggestExportIfNeeded() {
	Expects(started());

	for (const auto &[index, account] : _accounts) {
		if (const auto session = account->maybeSession()) {
			const auto settings = session->local().readExportSettings();
			if (const auto availableAt = settings.availableAt) {
				session->data().suggestStartExport(availableAt);
			}
		}
	}
}

int Domain::pendingAccounts() const {
	return _pendingAccounts.current();
}

rpl::producer<int> Domain::pendingAccountsValue() const {
	return _pendingAccounts.value();
}

bool Domain::indexTaken(int index) const {
	return ranges::contains(_accounts, index, &AccountWithIndex::index)
		|| _local->isPendingIndex(index);
}

// The next batch starts once the previous one is connected (or has
// nothing to connect), or after kStartBatchTimeout at the latest - so a
// dead proxy holds the queue up for seconds, not forever.
void Domain::checkStartBatch() {
	auto waiting = false;
	for (const auto &weak : _startBatch) {
		const auto account = weak.get();
		if (!account || !account->sessionExists()) {
			continue;
		}
		const auto state = account->mtp().dcstate();
		if (state == MTP::DisconnectedState
			|| state == MTP::ConnectingState
			|| state < 0) {
			waiting = true;
			break;
		}
	}
	if (!waiting || (crl::now() - _startBatchAt >= kStartBatchTimeout)) {
		startNextBatch();
	}
}

void Domain::startNextBatch() {
	const auto started = _local->pendingCount()
		? _local->startPending(kStartBatchSize)
		: std::vector<not_null<Account*>>();
	_startBatch.clear();
	for (const auto account : started) {
		watchSession(account);
		_startBatch.push_back(base::make_weak(account));
	}
	_startBatchAt = crl::now();
	_pendingAccounts = _local->pendingCount();
	if (!started.empty()) {
		_accountsChanges.fire({});
	}
	if (!_local->pendingCount()) {
		_startBatchTimer.cancel();
		_startBatch.clear();
		// Accounts with nothing to start dropped out of the list.
		scheduleWriteAccounts();
	}
}

void Domain::accountAddedInStorage(AccountWithIndex accountWithIndex) {
	Expects(accountWithIndex.account != nullptr);

	for (const auto &[index, _] : _accounts) {
		if (index == accountWithIndex.index) {
			Unexpected("Repeated account index.");
		}
	}
	_accounts.push_back(std::move(accountWithIndex));
}

void Domain::activateFromStorage(int index) {
	_accountToActivate = index;
}

int Domain::activeForStorage() const {
	return _accountToActivate;
}

void Domain::resetWithForgottenPasscode() {
	if (_walletKeyring) {
		_walletKeyring->clear();
	}
	if (_accounts.empty()) {
		_local->startFromScratch();
		activateAfterStarting();
	} else {
		
		auto staggerDelay = crl::time(0);
		auto isFirst = true;
		for (const auto &[index, account] : _accounts) {
			const auto raw = account.get();
			if (isFirst) {
				isFirst = false;
				raw->logOut();
			} else {
				staggerDelay += NextAccountStaggerDelay();
				base::call_delayed(staggerDelay, raw, [raw] {
					raw->logOut();
				});
			}
		}
	}
}

bool Domain::finishPasscodeClearAfterReset() {
	if (!passcodeRemovalAuthorized()) {
		return false;
	}
	return clearPasscodeAfterLastLogout();
}

void Domain::activateAfterStarting() {
	Expects(started());

	auto toActivate = _accounts.front().account.get();
	for (const auto &[index, account] : _accounts) {
		if (index == _accountToActivate) {
			toActivate = account.get();
		}
		watchSession(account.get());
	}

	activate(toActivate);
	removePasscodeIfEmpty();
}

const std::vector<Domain::AccountWithIndex> &Domain::accounts() const {
	return _accounts;
}

bool Domain::isLive(not_null<Account*> account) const {
	for (const auto &entry : _accounts) {
		if (entry.account.get() == account.get()) {
			return account->sessionExists();
		}
	}
	return false;
}

std::vector<not_null<Account*>> Domain::orderedAccounts() const {
	const auto order = Core::App().settings().accountsOrder();
	auto accounts = ranges::views::all(
		_accounts
	) | ranges::views::transform([](const Domain::AccountWithIndex &a) {
		return not_null{ a.account.get() };
	}) | ranges::to_vector;
	ranges::stable_sort(accounts, [&](
			not_null<Account*> a,
			not_null<Account*> b) {
		const auto aPinned = a->pinned();
		const auto bPinned = b->pinned();
		if (aPinned != bPinned) {
			return aPinned && !bPinned;
		}
		const auto aIt = a->sessionExists()
			? ranges::find(order, a->session().uniqueId())
			: end(order);
		const auto bIt = b->sessionExists()
			? ranges::find(order, b->session().uniqueId())
			: end(order);
		return aIt < bIt;
	});
	return accounts;
}

rpl::producer<> Domain::accountsChanges() const {
	return _accountsChanges.events();
}

Account *Domain::maybeLastOrSomeAuthedAccount() {
	auto result = (Account*)nullptr;
	for (const auto &[index, account] : _accounts) {
		if (!account->sessionExists()) {
			continue;
		} else if (index == _lastActiveIndex) {
			return account.get();
		} else if (!result) {
			result = account.get();
		}
	}
	return result;
}

int Domain::accountsAuthedCount() const {
	auto result = 0;
	for (const auto &[index, account] : _accounts) {
		if (account->sessionExists()) {
			++result;
		}
	}
	return result;
}

rpl::producer<Account*> Domain::activeValue() const {
	return _active.value();
}

Account &Domain::active() const {
	Expects(!_accounts.empty());

	Ensures(_active.current() != nullptr);
	return *_active.current();
}

rpl::producer<not_null<Account*>> Domain::activeChanges() const {
	return _active.changes() | rpl::map([](Account *value) {
		return not_null{ value };
	});
}

rpl::producer<Session*> Domain::activeSessionChanges() const {
	return _activeSessions.events();
}

rpl::producer<Session*> Domain::activeSessionValue() const {
	const auto current = _accounts.empty()
		? nullptr
		: active().maybeSession();
	return rpl::single(current) | rpl::then(_activeSessions.events());
}

int Domain::unreadBadge() const {
	return _unreadBadge;
}

bool Domain::unreadBadgeMuted() const {
	return _unreadBadgeMuted;
}

rpl::producer<> Domain::unreadBadgeChanges() const {
	return _unreadBadgeChanges.events();
}

void Domain::notifyUnreadBadgeChanged() {
	for (const auto &[index, account] : _accounts) {
		if (const auto session = account->maybeSession()) {
			session->data().notifyUnreadBadgeChanged();
		}
	}
}

void Domain::updateUnreadBadge() {
	_unreadBadge = 0;
	_unreadBadgeMuted = true;
	for (const auto &[index, account] : _accounts) {
		if (account->trashed()) {
			continue; // the «Корзина» does not count in the dock badge
		}
		if (const auto session = account->maybeSession()) {
			const auto data = &session->data();
			_unreadBadge += data->unreadBadge();
			if (!data->unreadBadgeMuted()) {
				_unreadBadgeMuted = false;
			}
		}
	}
	_unreadBadgeChanges.fire({});
}

void Domain::scheduleUpdateUnreadBadge() {
	if (_unreadBadgeUpdateScheduled || Core::Quitting()) {
		return;
	}
	_unreadBadgeUpdateScheduled = true;
	Core::App().postponeCall(crl::guard(&Core::App(), [=] {
		_unreadBadgeUpdateScheduled = false;
		updateUnreadBadge();
	}));
}

not_null<Main::Account*> Domain::add(
		MTP::Environment environment,
		const MTP::ProxyData &proxy) {
	Expects(started());
	Expects(_accounts.size() < kPremiumMaxAccounts);

	static const auto cloneConfig = [](const MTP::Config &config) {
		return std::make_unique<MTP::Config>(config);
	};
	auto mainDcId = MTP::Instance::Fields::kNotSetMainDc;
	const auto accountConfig = [&](not_null<Account*> account) {
		mainDcId = account->mtp().mainDcId();
		return cloneConfig(account->mtp().config());
	};
	auto config = [&] {
		if (_active.current()->mtp().environment() == environment) {
			return accountConfig(_active.current());
		}
		for (const auto &[index, account] : _accounts) {
			if (account->mtp().environment() == environment) {
				return accountConfig(account.get());
			}
		}
		return (environment == MTP::Environment::Production)
			? cloneConfig(Core::App().fallbackProductionConfig())
			: std::make_unique<MTP::Config>(environment);
	}();
	auto index = 0;
	while (indexTaken(index)) {
		++index;
	}
	_accounts.push_back(AccountWithIndex{
		.index = index,
		.account = std::make_unique<Account>(this, _dataName, index)
	});
	const auto account = _accounts.back().account.get();
	account->setMtpMainDcId(mainDcId);
	if (proxy.type != MTP::ProxyData::Type::None) {
		// The account's own storage key only exists *after* start(), so
		// setProxyOverride() (which writes to that storage) would crash
		// here - seed the in-memory MTP fields instead, so startMtp()
		// still picks it up before the very first connection.
		account->setMtpProxyOverride(proxy);
	}
	_local->startAdded(account, std::move(config));
	if (proxy.type != MTP::ProxyData::Type::None) {
		// Now that start() has run, the storage key exists - persist the
		// proxy for real so it survives a restart.
		account->setProxyOverride(proxy);
	}
	// A brand new account gets its own device identity before it logs in.
	account->setDeviceOverride(Settings::RandomDeviceOverride());
	watchSession(account);
	_accountsChanges.fire({});

	auto &settings = Core::App().settings();
	if (_accounts.size() == 2 && !settings.mainMenuAccountsShown()) {
		settings.setMainMenuAccountsShown(true);
		Core::App().saveSettingsDelayed();
	}

	return account;
}

std::vector<not_null<Main::Account*>> Domain::addFromTdata(
		const QString &folderPath,
		MTP::Environment environment,
		Fn<MTP::ProxyData()> nextProxy,
		Fn<void(not_null<Main::Account*>, const QString &folder)> onImported) {
	Expects(started());

	return addFromAuthorizations(
		Storage::ReadForeignTdataAuthorizations(folderPath),
		environment,
		std::move(nextProxy),
		std::move(onImported));
}

std::vector<not_null<Main::Account*>> Domain::addFromAuthorizations(
		const std::vector<Storage::ForeignTdataAuthorization> &authorizations,
		MTP::Environment environment,
		Fn<MTP::ProxyData()> nextProxy,
		Fn<void(not_null<Main::Account*>, const QString &folder)> onImported) {
	Expects(started());

	auto result = std::vector<not_null<Main::Account*>>();
	if (authorizations.empty()) {
		return result;
	}

	static const auto cloneConfig = [](const MTP::Config &config) {
		return std::make_unique<MTP::Config>(config);
	};
	const auto config = [&]() -> std::unique_ptr<MTP::Config> {
		for (const auto &[index, account] : _accounts) {
			if (account->mtp().environment() == environment) {
				return cloneConfig(account->mtp().config());
			}
		}
		return (environment == MTP::Environment::Production)
			? cloneConfig(Core::App().fallbackProductionConfig())
			: std::make_unique<MTP::Config>(environment);
	}();

	// A tdata folder can hold several accounts - import every one of them
	// it lists, stopping early if the account limit is hit, and skipping
	// (not aborting on) any that's already added elsewhere.
	for (const auto &authorization : authorizations) {
		if (_accounts.size() >= kPremiumMaxAccounts) {
			break;
		}
		if (hasAccountWithUserId(
				PeekAuthorizationUserId(authorization.data),
				nullptr)) {
			// Reject before creating any Account object or touching disk -
			// two accounts for the same real Telegram user fight over one
			// MTProto session and the server force-resets both.
			continue;
		}

		auto index = 0;
		while (indexTaken(index)) {
			++index;
		}
		_accounts.push_back(AccountWithIndex{
			.index = index,
			.account = std::make_unique<Account>(this, _dataName, index)
		});
		const auto account = _accounts.back().account.get();
		// setMtpAuthorization derives mainDcId/userId from the imported
		// blob itself, so unlike add() there's no separate
		// setMtpMainDcId() call.
		account->setMtpAuthorization(authorization.data);
		// Called only now - past every check that could still skip this
		// account - so a generator pulling from a proxy pool (see
		// MakeMultiProxyBox) never consumes an entry for an account that
		// ends up not being added.
		const auto proxy = nextProxy();
		if (proxy.type != MTP::ProxyData::Type::None) {
			// The account's own storage key only exists *after* start(),
			// so setProxyOverride() (which writes to that storage) would
			// crash here - seed the in-memory MTP fields instead, so
			// startMtp() still picks it up before the very first
			// connection.
			account->setMtpProxyOverride(proxy);
		}
		_local->startAdded(account, cloneConfig(*config));
		if (proxy.type != MTP::ProxyData::Type::None) {
			// Now that start() has run, the storage key exists - persist
			// the proxy for real so it survives a restart.
			account->setProxyOverride(proxy);
		}
		// The imported keys so far exist only in memory - unlike a normal
		// login, nothing else ever writes this account's keys file. Without
		// this, the next launch finds no keys file, creates an empty one,
		// and the account is gone for good. Synchronous for the same
		// reason writeAccounts() below is.
		account->local().writeMtpData(true);
		// Device identity: what the export left in "ggtg.json" next to this
		// account's tdata, otherwise a random one.
		{
			auto device = MTP::DeviceOverride();
			for (const auto &dir : (authorization.folder.isEmpty()
				|| authorization.folder.startsWith(u"sync:"_q))
				? std::vector<QDir>()
				: std::vector<QDir>{
					QFileInfo(authorization.folder).dir(),
					QDir(authorization.folder) }) {
				auto file = QFile(dir.filePath(u"ggtg.json"_q));
				if (file.open(QIODevice::ReadOnly)) {
					device = Settings::DeviceOverrideFromJson(
						QJsonDocument::fromJson(file.readAll()).object()
							.value(u"device"_q).toObject());
					break;
				}
			}
			account->setDeviceOverride((device == MTP::DeviceOverride() || device.deviceModel.isEmpty())
				? Settings::RandomDeviceOverride()
				: device);
		}
		if (onImported) {
			// Same reasoning as setProxyOverride() just above - this may
			// write to the account's local storage (e.g. the 2FA-sidecar
			// lookup in Settings::AccountsList does), which only exists
			// after startAdded().
			onImported(account, authorization.folder);
		}
		watchSession(account);
		result.push_back(account);
	}

	if (result.empty()) {
		return result;
	}

	// Each imported account's own session data is already on disk (from
	// startAdded() above), but unlike the normal add() + activate() login
	// flow, nothing here ever calls activate() on these accounts - so
	// without an explicit write, the master accounts list on disk never
	// learns these slots exist and they vanish on the next launch.
	//
	// Written synchronously, not via scheduleWriteAccounts() - that defers
	// to the next event-loop tick, which a batch import has no guarantee
	// of reaching before the app quits (no further UI interaction is
	// implied the way an interactive login flow's own subsequent ticks
	// are). A deferred write here is exactly what left previously-
	// imported accounts present in tdata/ but missing on the next launch.
	_local->writeAccounts();
	_accountsChanges.fire({});

	auto &settings = Core::App().settings();
	if (_accounts.size() >= 2 && !settings.mainMenuAccountsShown()) {
		settings.setMainMenuAccountsShown(true);
		Core::App().saveSettingsDelayed();
	}

	return result;
}

int Domain::recoverOrphanedAccounts() {
	Expects(started());

	const auto recovered = _local->recoverOrphanedAccounts();
	for (const auto &account : recovered) {
		watchSession(account);
	}
	if (!recovered.empty()) {
		_accountsChanges.fire({});
	}
	return int(recovered.size());
}

int Domain::cleanupTdata(QString *folder) {
	Expects(started());

	return _local->cleanupTdata(folder);
}

void Domain::addActivated(
		MTP::Environment environment,
		const MTP::ProxyData &proxy,
		bool newWindow) {
	const auto added = [&](not_null<Main::Account*> account) {
		if (newWindow) {
			Core::App().ensureSeparateWindowFor(account);
		} else if (const auto window = Core::App().separateWindowFor(
				account)) {
			window->activate();
		} else {
			activate(account);
		}
	};
	if (accounts().size() < maxAccounts()) {
		added(add(environment, proxy));
	} else {
		for (auto &[index, account] : accounts()) {
			if (!account->sessionExists()
				&& account->mtp().environment() == environment) {
				if (proxy.type != MTP::ProxyData::Type::None) {
					account->setProxyOverride(proxy);
				}
				added(account.get());
				break;
			}
		}
	}
}

void Domain::watchSession(not_null<Account*> account) {
	account->sessionValue(
	) | rpl::filter([=](Session *session) {
		return session != nullptr;
	}) | rpl::on_next([=](Session *session) {
		if (sharesAuthKeyWithOther(account, session->userId())) {
			// A normal phone/QR login can complete for a number that's
			// already logged in under another account slot in this app -
			// the addFromTdata early check can't catch that case. Log it
			// straight back out before it fights the other slot's session
			// (deferred: destroying the session from inside its own
			// creation callback would be a use-after-free).
			LOG(("Main Info: rejecting duplicate account, user id %1"
				).arg(session->userId().bare));
			crl::on_main(&Core::App(), [=] {
				if (account->sessionExists()) {
					account->forcedLogOut();
				}
			});
			return;
		}
		session->data().unreadBadgeChanges(
		) | rpl::on_next([=] {
			scheduleUpdateUnreadBadge();
		}, session->lifetime());

		Data::AmPremiumValue(
			session
		) | rpl::on_next([=] {
			_lastMaxAccounts = maxAccounts();
		}, session->lifetime());
	}, account->lifetime());

	account->sessionChanges(
	) | rpl::filter([=](Session *session) {
		return !session;
	}) | rpl::on_next([=] {
		scheduleUpdateUnreadBadge();
		closeAccountWindows(account);
		if (!Core::Quitting()) {
			crl::on_main(&Core::App(), [=] {
				removeRedundantAccounts();
			});
		}
	}, account->lifetime());
}

void Domain::closeAccountWindows(not_null<Main::Account*> account) {
	auto another = (Main::Account*)nullptr;
	for (auto i = _accounts.begin(); i != _accounts.end(); ++i) {
		const auto other = not_null(i->account.get());
		if (other == account) {
			continue;
		} else if (Core::App().separateWindowFor(other)) {
			const auto that = Core::App().separateWindowFor(account);
			if (that) {
				that->close();
			}
		} else if (!another
			|| (other->sessionExists() && !another->sessionExists())) {
			another = other;
		}
	}
	if (another) {
		activate(another);
	}
}

bool Domain::removePasscodeIfEmpty() {
	if (_accounts.size() != 1 || _active.current()->sessionExists()) {
		return false;
	}
	Local::reset();

	// We completely logged out, remove the passcode if it was there.
	if (Core::App().passcodeLocked()) {
		Core::App().unlockPasscode();
	}
	if (!_local->hasPasscode()) {
		return false;
	} else if (!clearPasscodeAfterLastLogout()) {
		reportFailedPasscodeClear();
		return false;
	}
	return true;
}

bool Domain::passcodeRemovalAuthorized() const {
	return (_accounts.size() == 1)
		&& !_active.current()->sessionExists()
		&& _local->hasPasscode();
}

bool Domain::clearPasscodeAfterLastLogout() {
	Expects(passcodeRemovalAuthorized());

	_local->clearPasscodeAfterReset();
	if (_local->hasPasscode()) {
		return false;
	}
	Core::App().settings().setSystemUnlockEnabled(false);
	Core::App().saveSettingsDelayed();
	return true;
}

bool Domain::sharesAuthKeyWithOther(
		not_null<Account*> account,
		UserId userId) const {
	if (!userId) {
		return false;
	}
	// Only a genuinely shared auth key is a conflict (two slots driving one
	// MTProto session make the server reset both). The same user logged in
	// under two DIFFERENT keys is just two independent sessions - rejecting
	// (and wiping) one of them merely for sharing a user id would throw
	// away a perfectly valid session.
	const auto mine = account->mtp().getKeysForWrite();
	for (const auto &[index, other] : _accounts) {
		if (other.get() == account.get()) {
			continue;
		}
		const auto session = other->maybeSession();
		if (!session || session->userId() != userId) {
			continue;
		}
		for (const auto &theirs : other->mtp().getKeysForWrite()) {
			for (const auto &key : mine) {
				if (key->keyId() == theirs->keyId()) {
					return true;
				}
			}
		}
		LOG(("Main Info: same user id %1 in two slots but with different "
			"auth keys, keeping both.").arg(userId.bare));
	}
	return false;
}

bool Domain::hasAccountWithUserId(UserId userId, Account *except) const {
	if (!userId) {
		return false;
	}
	for (const auto &[index, account] : _accounts) {
		if (account.get() == except) {
			continue;
		}
		if (const auto session = account->maybeSession()) {
			if (session->userId() == userId) {
				return true;
			}
		}
	}
	return false;
}

void Domain::reportFailedPasscodeClear() {
	crl::on_main(this, [=] {
		if (!passcodeRemovalAuthorized()) {
			return;
		}
		if (const auto window = Core::App().activePrimaryWindow()) {
			window->showPasscodeClearFailed(crl::guard(this, [=] {
				return finishPasscodeClearAfterReset();
			}));
		}
	});
}

void Domain::removeRedundantAccounts() {
	Expects(started());

	for (const auto &one : _accounts) {
		if (one.account->destroyingSession()) {
			// We were invoked from a crl::on_main drain that a nested
			// event dispatch started from inside that account's own
			// destroySession(). Erasing it now would free the object
			// under its own stack frame. Retry after unwinding.
			crl::on_main(&Core::App(), [=] {
				removeRedundantAccounts();
			});
			return;
		}
	}

	const auto was = _accounts.size();
	for (auto i = _accounts.begin(); i != _accounts.end();) {
		if (Core::App().separateWindowFor(not_null(i->account.get()))
			|| i->account->sessionExists()) {
			++i;
			continue;
		}
		checkForLastProductionConfig(i->account.get());
		i = _accounts.erase(i);
	}

	if (!removePasscodeIfEmpty() && _accounts.size() != was) {
		scheduleWriteAccounts();
		_accountsChanges.fire({});
	}
	// Session teardown already removed the logged-out account's live keyring
	// membership, including when its window keeps the account in the list.
	// The last-logout case retains its checked clear and retry route, and
	// passcodeRemovalAuthorized() stays true until that clear has landed.
	if (!passcodeRemovalAuthorized()) {
		Wallet::DropUnusedPasscode();
	}
}

void Domain::checkForLastProductionConfig(
		not_null<Main::Account*> account) {
	const auto mtp = &account->mtp();
	if (mtp->environment() != MTP::Environment::Production) {
		return;
	}
	for (const auto &[index, other] : _accounts) {
		if (other.get() != account
			&& other->mtp().environment() == MTP::Environment::Production) {
			return;
		}
	}
	Core::App().refreshFallbackProductionConfig(mtp->config());
}

void Domain::maybeActivate(not_null<Main::Account*> account) {
	if (Core::App().separateWindowFor(account)) {
		activate(account);
	} else {
		Core::App().preventOrInvoke(crl::guard(account, [=] {
			activate(account);
		}));
	}
}

void Domain::activate(not_null<Main::Account*> account) {
	// An account from the «Корзина» connects only once it is opened.
	account->wake();
	if (const auto window = Core::App().separateWindowFor(account)) {
		window->activate();
	}
	if (_active.current() == account.get()) {
		return;
	}
	const auto i = ranges::find(_accounts, account.get(), [](
			const AccountWithIndex &value) {
		return value.account.get();
	});
	Assert(i != end(_accounts));
	const auto changed = (_accountToActivate != i->index);
	auto wasAuthed = false;

	const auto previous = _active.current();
	_activeLifetime.destroy();
	if (_active.current()) {
		_lastActiveIndex = _accountToActivate;
		wasAuthed = _active.current()->sessionExists();
	}
	_accountToActivate = i->index;
	_active = account.get();
	// Back to sleep once the window moved on - but not while a process is
	// running: a campaign / bulk action may be working through this very
	// account, and cutting its connection mid-run (after a sticker, before
	// the text that follows it) fails the step.
	const auto processRunning = ranges::any_of(
		Core::App().processes().list(),
		[](const Core::ProcessSnapshot &process) {
			return process.status == Core::ProcessStatus::Running;
		});
	if (previous
		&& previous != account.get()
		&& previous->trashed()
		&& !processRunning) {
		previous->sleep();
	}
	_active.current()->sessionValue(
	) | rpl::start_to_stream(_activeSessions, _activeLifetime);

	if (changed) {
		if (wasAuthed) {
			scheduleWriteAccounts();
		} else {
			crl::on_main(&Core::App(), [=] {
				removeRedundantAccounts();
			});
		}
	}
}

void Domain::scheduleWriteAccounts() {
	if (_writeAccountsScheduled) {
		return;
	}
	_writeAccountsScheduled = true;
	crl::on_main(&Core::App(), [=] {
		_writeAccountsScheduled = false;
		_local->writeAccounts();
	});
}

int Domain::maxAccounts() const {
	const auto premiumCount = ranges::count_if(accounts(), [](
			const Main::Domain::AccountWithIndex &d) {
		return d.account->sessionExists()
			&& (d.account->session().premium()
				|| d.account->session().isTestMode());
	});
	return std::min(int(premiumCount) + kMaxAccounts, kPremiumMaxAccounts);
}

rpl::producer<int> Domain::maxAccountsChanges() const {
	return _lastMaxAccounts.changes();
}

bool AccountIsLive(not_null<Account*> account) {
	return Core::App().domain().isLive(account);
}

std::vector<not_null<Account*>> LiveAccounts(
		const base::flat_set<not_null<Account*>> &chosen) {
	auto result = std::vector<not_null<Account*>>();
	for (const auto &account : Core::App().domain().orderedAccounts()) {
		if (chosen.contains(account) && account->sessionExists()) {
			result.push_back(account);
		}
	}
	return result;
}

} // namespace Main
