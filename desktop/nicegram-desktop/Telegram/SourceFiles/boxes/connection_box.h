/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/timer.h"
#include "base/object_ptr.h"
#include "base/weak_ptr.h"
#include "core/core_settings_proxy.h"
#include "mtproto/connection_abstract.h"
#include "mtproto/mtproto_proxy_data.h"

namespace Ui {
class Show;
class BoxContent;
class InputField;
class PortInput;
class PasswordInput;
class Checkbox;
template <typename Enum>
class RadioenumGroup;
template <typename Enum>
class Radioenum;
} // namespace Ui

namespace Main {
class Account;
} // namespace Main

namespace Window {
class SessionController;
} // namespace Window

// Edits a single account's own proxy override (Main::Account::proxyOverride/
// setProxyOverride), independent of the app-wide proxy list/settings above.
[[nodiscard]] object_ptr<Ui::BoxContent> MakeAccountProxyBox(
	not_null<Main::Account*> account);

// Prompts for a one-off proxy with no account attached yet - for setting a
// proxy up front, before a not-yet-created account's first connection
// (e.g. tdata import). Calls submit() only if the user actually saves.
// If MTP::GlobalProxyPool() has any non-empty list, offers to pick a list
// by name first (taking its first entry, which is then removed) before
// falling back to manual entry.
[[nodiscard]] object_ptr<Ui::BoxContent> MakeNewProxyBox(
	Fn<void(const MTP::ProxyData &proxy)> submit);

// Same idea, but for importing several accounts at once (e.g. a tdata
// folder with multiple accounts in it) where each one should end up on its
// *own* proxy rather than sharing one. If a list is picked, submit() gets a
// generator that hands out a fresh, distinct entry from that list each time
// it's called - once the list runs out, it returns
// MTP::UnreachableProxyPlaceholder() instead of falling back to manual/
// direct, so extra accounts stay proxied-but-disconnected until the user
// sets a real proxy for them. Manual entry has only one value to give, so
// every account ends up sharing it.
[[nodiscard]] object_ptr<Ui::BoxContent> MakeMultiProxyBox(
	Fn<void(Fn<MTP::ProxyData()> nextProxy)> submit);

// Top-level manager: shows all named proxy lists (e.g. "India",
// "Indonesia") with their counts, lets the user create a new one, and
// opens a per-list editor (rename, bulk-add "host:port:login:password"
// lines, remove entries, delete the list) when one is picked.
[[nodiscard]] object_ptr<Ui::BoxContent> MakeProxyPoolManagerBox();

// Makes a plain HTTPS request through the given proxy to an external
// IP-echo service and shows the result - proves the proxy actually works
// and which exit IP it connects through. MTProto-type proxies can't be
// checked this way (they only relay Telegram's own protocol, not generic
// HTTP), the box says so instead of silently testing a direct connection.
[[nodiscard]] object_ptr<Ui::BoxContent> MakeProxyCheckBox(
	const MTP::ProxyData &proxy);

class ProxiesBoxController {
public:
	using ProxyData = MTP::ProxyData;
	using Type = ProxyData::Type;

	explicit ProxiesBoxController(not_null<Main::Account*> account);

	static void ShowApplyConfirmation(
		Window::SessionController *controller,
		Type type,
		const QMap<QString, QString> &fields);

	[[nodiscard]] static ProxyData ProxyFromLink(const QString &link);

	static object_ptr<Ui::BoxContent> CreateOwningBox(
		not_null<Main::Account*> account,
		const QString &highlightId = QString());
	static void Show(
		not_null<Window::SessionController*> controller,
		const QString &highlightId = QString());
	object_ptr<Ui::BoxContent> create(const QString &highlightId = QString());

	enum class ItemState {
		Connecting,
		Online,
		Checking,
		NotTested,
		Available,
		Unavailable,
		WaitingForBrowser,
	};
	struct ItemView {
		int id = 0;
		QString type;
		QString host;
		uint32 port = 0;
		int ping = 0;
		bool selected = false;
		bool deleted = false;
		bool supportsShare = false;
		bool supportsCalls = false;
		bool web = false;
		bool canOpenBrowser = false;
		ItemState state = ItemState::Checking;
		// GGTG: whose proxy the row is (an account's name), and whether it
		// can be deleted (an account's proxy can't: without one it never
		// connects).
		QString label;
		bool removable = true;

	};

	// GGTG: the list is the proxies of the accounts (the ones they really
	// connect through), not the app-wide list, which no account uses.
	[[nodiscard]] bool accountMode() const {
		return _accountMode;
	}

	void deleteItem(int id);
	void deleteItems();
	void restoreItem(int id);
	void shareItem(int id, bool qr);
	void shareItems();
	void applyItem(int id);
	void openBrowser(int id);
	object_ptr<Ui::BoxContent> editItemBox(int id);
	object_ptr<Ui::BoxContent> addNewItemBox();
	bool setProxySettings(ProxyData::Settings value);
	void setProxyForCalls(bool enabled);
	void setProxyRotationEnabled(bool enabled);
	void setProxyRotationTimeout(int value);
	void setTryIPv6(bool enabled);
	rpl::producer<ProxyData::Settings> proxySettingsValue() const;

	[[nodiscard]] bool contains(const ProxyData &proxy) const;
	void addNewItem(const ProxyData &proxy);

	rpl::producer<ItemView> views() const;

	rpl::producer<bool> listShareableChanges() const;

	~ProxiesBoxController();

private:
	using Checker = MTP::details::ConnectionPointer;
	struct Item {
		int id = 0;
		ProxyData data;
		bool deleted = false;
		Checker checker;
		Checker checkerv6;
		ItemState state = ItemState::Checking;
		int ping = 0;
		// GGTG (accountMode): the account this proxy belongs to.
		base::weak_ptr<Main::Account> account;
		QString label;

	};

	std::vector<Item>::iterator findById(int id);
	std::vector<Item>::iterator findByProxy(const ProxyData &proxy);
	void setDeleted(int id, bool deleted);
	void updateView(const Item &item);
	void share(const ProxyData &proxy, bool qr = false);
	void saveDelayed();
	void refreshChecker(Item &item);

	void replaceItemWith(
		std::vector<Item>::iterator which,
		std::vector<Item>::iterator with);
	void replaceItemValue(
		std::vector<Item>::iterator which,
		const ProxyData &proxy);

	void fillAccountItems();
	[[nodiscard]] std::vector<Item>::iterator findCurrent();
	[[nodiscard]] object_ptr<Ui::BoxContent> accountProxyBox(int id);
	void setAccountProxy(int id, const ProxyData &proxy);

	const not_null<Main::Account*> _account;
	const bool _accountMode = true;
	Core::SettingsProxy &_settings;
	int _idCounter = 0;
	std::vector<Item> _list;
	rpl::event_stream<ItemView> _views;
	base::Timer _saveTimer;
	rpl::event_stream<ProxyData::Settings> _proxySettingsChanges;
	std::shared_ptr<Ui::Show> _show;

	ProxyData _lastSelectedProxy;
	bool _lastSelectedProxyUsed = false;

	rpl::lifetime _lifetime;

};
