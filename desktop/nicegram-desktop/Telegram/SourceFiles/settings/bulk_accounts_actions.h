/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/flat_set.h"
#include "base/object_ptr.h"
#include "rpl/variable.h"
#include "mtproto/mtp_instance.h"
#include <QtCore/QJsonObject>
#include <QtGui/QColor>
#include <optional>
#include <QtCore/QPointer>

namespace Window {
class SessionController;
} // namespace Window

namespace Ui {
class BoxContent;
class Checkbox;
class FlatLabel;
class GenericBox;
class RpWidget;
class Show;
class VerticalLayout;
} // namespace Ui

namespace Main {
class Account;
} // namespace Main

namespace Core::GgtgSessions {
struct StoredSession;
} // namespace Core::GgtgSessions

namespace Settings {

enum class BulkActionIndicator {
	None,
	Running,
	Done,
};

// Drives the "Массовые действия" menu row's spinner/checkmark suffix (see
// window_main_menu.cpp), the same way SmsCampaignIndicatorValue() does for
// "Отправка SMS" - see sms_campaign.h for the exact state meanings.
[[nodiscard]] rpl::producer<BulkActionIndicator> BulkActionIndicatorValue();

// Sets this account's language and time zone from the IP its proxy (or this
// computer, with none) goes out through; device model / OS are kept. `done`
// gets the outcome. Called after a proxy changes and from "Массовые действия".
void AutoDeviceGeo(
	not_null<Main::Account*> account,
	Fn<void(bool ok, QString info)> done = nullptr);

// Gives this account a random device model + OS from the presets (and, if it
// has a proxy, language / time zone from the proxy's location).
void RandomizeAccountDevice(
	not_null<Main::Account*> account,
	Fn<void(bool ok, QString info)> done = nullptr);

void ShowEditDeviceBox(
	std::shared_ptr<Ui::Show> show,
	not_null<Main::Account*> account,
	Fn<void()> onSaved = nullptr);

[[nodiscard]] bool IsAndroidSession(const MTP::DeviceOverride &device);
[[nodiscard]] bool IsMacSession(const MTP::DeviceOverride &device);
[[nodiscard]] bool IsIosSession(const MTP::DeviceOverride &device);
[[nodiscard]] MTP::DeviceOverride RandomDesktopDeviceOverride(const QString &phone = QString());
[[nodiscard]] MTP::DeviceOverride RandomWindowsDeviceOverride(const QString &phone = QString());
[[nodiscard]] MTP::DeviceOverride RandomMacDeviceOverride(const QString &phone = QString());
[[nodiscard]] MTP::DeviceOverride RandomIosDeviceOverride(const QString &phone = QString());
[[nodiscard]] MTP::DeviceOverride RandomAndroidDeviceOverride(const QString &phone = QString());
[[nodiscard]] MTP::DeviceOverride RandomDeviceOverrideForAccount(not_null<Main::Account*> account);
[[nodiscard]] MTP::DeviceOverride RandomDeviceOverride(const QString &phone = QString());

// The device block of an account's "ggtg.json" (empty object = default).
[[nodiscard]] QJsonObject DeviceOverrideToJson(const MTP::DeviceOverride &value);
// A random Android phone (model + "SDK n") for an account exported for the
// Android app; the Android side adopts it on import.
// `effective` = EffectiveDeviceOverride() of the account it is made for.
[[nodiscard]] QJsonObject RandomAndroidDeviceJson(
	const MTP::DeviceOverride &effective);
[[nodiscard]] QJsonObject RandomDesktopDeviceJson(
	const MTP::DeviceOverride &effective);
[[nodiscard]] QJsonObject RandomIosDeviceJson(
	const MTP::DeviceOverride &effective);
// Everything Telegram is told for this account right now (own values, the
// app's usual ones where none is set) - what an export has to carry so the
// account stays the same "device" wherever it is opened.
[[nodiscard]] MTP::DeviceOverride EffectiveDeviceOverride(
	not_null<Main::Account*> account);
[[nodiscard]] MTP::DeviceOverride DeviceOverrideFromJson(
	const QJsonObject &object);

// Reopens the live progress box of a bulk action that is running or just
// finished (hidden by "Закрыть", never lost - see RunBulkAction in
// bulk_accounts_actions.cpp). Starting one is done from the account manager:
// tick accounts there, then "Массовое действие...".
void OpenBulkActions(not_null<Window::SessionController*> controller);

// "Менеджер аккаунтов" (the GGTG window's account list, see
// window/window_main_menu.cpp): every account in a table filtered by folder
// cards and a search, with its own «Открыть» button that switches to the
// account while the GGTG window stays open (the window follows the new
// account, see OpenGgtgShell), and a «Корзина» of accounts put aside.
// Every prompt it opens goes through this box's own Ui::Show (not
// `controller`), so it appears inside whichever window the manager itself
// is shown in.
[[nodiscard]] object_ptr<Ui::BoxContent> MakeAccountManagerBox(
	not_null<Window::SessionController*> controller);

// ---- «Корзина» ----
// An account in the trash keeps working (it can be opened and used), it is
// only listed apart from the rest until restored or deleted for good.

// Fires after accounts were moved into or out of the trash or deleted.
[[nodiscard]] rpl::producer<> AccountTrashChanges();

void MoveAccountsToTrash(const std::vector<not_null<Main::Account*>> &list);
void RestoreAccountsFromTrash(
	const std::vector<not_null<Main::Account*>> &list);

// Asks first, then removes the accounts from the app for good: the session
// is dropped locally (nothing is sent to Telegram) and everything the app
// kept about them - local data and caches, spare sessions, journal entries
// - is deleted. The account that is open right now is skipped.
void ConfirmDeleteAccountsForever(
	std::shared_ptr<Ui::Show> show,
	const std::vector<not_null<Main::Account*>> &list);

// Actions "База" can hand off to the main window (they need its session
// controller, which the Ui::Show "База" itself runs on doesn't give). Only
// the main-window opener fills them in; each button they drive is simply
// absent when its action is empty (e.g. inside the "Отправка SMS" box).
struct BasesActions {
	// A users base -> "Отправка SMS" with only that base checked.
	Fn<void(uint64 baseId)> sendToBase;
	// A users base -> "Анализ и чистка базы" (see settings/sms_campaign.h).
	Fn<void(std::shared_ptr<Ui::Show> show, uint64 baseId)> analyzeBase;
	// A group of a groups base (by @username) -> collect its members into
	// a base, reading its member list directly.
	Fn<void(const QString &username)> scrapeGroup;
	// The same through the Сквидвард bot, for a group whose member list is
	// hidden; the dialog opens over `show`'s window.
	Fn<void(std::shared_ptr<Ui::Show> show, const QString &username)>
		scrapeGroupViaSquidward;
	// Open the keyword group search.
	Fn<void()> searchGroups;
	// "Убрать каналы" for a groups base; `changed` after it is rewritten.
	Fn<void(std::shared_ptr<Ui::Show> show, uint64 baseId, Fn<void()> changed)>
		removeChannels;
};

// Viewer for the member lists saved via the "scrape to database" icon on
// a group's member list (see data/scraped_members_db.h) - browse, copy,
// or delete a saved group's list. Takes a Ui::Show (not a
// SessionController) so it can be opened from the main window's own side
// menu ("База") or from inside the "Отправка SMS" box's own "Управлять
// списками получателей..." button (see settings/sms_campaign.cpp) and
// stay there for its own nested navigation either way.
//
// `sendToBase`, when given, adds "Отправить SMS по этой базе" to a users
// base's editor and is called with that base's id - only the main-window
// opener passes it (inside the "Отправка SMS" box the bases are already
// right there as checkboxes).
[[nodiscard]] object_ptr<Ui::BoxContent> MakeScrapedGroupsDbBox(
	std::shared_ptr<Ui::Show> show,
	BasesActions actions = {});

// Opens a saved base's editor (users or groups) over `show` - the same one
// a click on it in «База» opens.
void ShowSavedBase(std::shared_ptr<Ui::Show> show, uint64 id);

// Below: shared with settings/sms_campaign.cpp - kept here (not anonymous-
// namespace-local in the .cpp) purely so that sibling file can reuse them
// instead of duplicating account-row rendering / the guarded-callback
// idiom used throughout bulk_accounts_actions.cpp's own step queues.

using Accounts = std::vector<not_null<Main::Account*>>;
using StepDone = Fn<void(bool success, QString message)>;
using Step = Fn<void(not_null<Main::Account*> account, StepDone done)>;
using StepProgress = Fn<void(QString progress)>;
using StepWithProgress = Fn<void(not_null<Main::Account*> account, StepProgress progress, StepDone done)>;

struct GuardedStep {
	StepDone done;
	Fn<void()> cancel;
};

// Wraps done() so a request that never calls back doesn't leave a queue
// stuck on this account forever.
[[nodiscard]] GuardedStep GuardedDone(StepDone done);

// The color an SMS status - live or recalled from data/scraped_members_db
// - is shown in: green for an actual send, amber for anything skipped
// (an error, or a username already messaged before), red for SPAM_ACC
// (the sending account hit Telegram's "mutual contacts only" limit).
[[nodiscard]] QColor SmsRowColor(bool spamAcc, bool ok);

[[nodiscard]] QString AccountLabel(not_null<Main::Account*> account);
[[nodiscard]] QString AccountPickerText(not_null<Main::Account*> account);

inline constexpr auto kPickerUserpicSize = 36;

// A row with the account's real photo on the left and a checkbox (name,
// @username, phone all in its label) filling the rest.
[[nodiscard]] not_null<Ui::Checkbox*> AddAccountPickerRow(
	not_null<Ui::VerticalLayout*> container,
	not_null<Main::Account*> account);

// A folder has no photo of its own - this paints a colored circle with
// its initial instead (same stand-in used for the active-account switcher
// in settings/sections/settings_information.cpp's addFolderHeader),
// square `size` x `size`, added as a child of `parent` (lives exactly as
// long as it does - not an object_ptr the caller needs to place/own
// separately) at (0, 0); reposition it with moveToLeft/moveToRight same
// as any other child widget.
[[nodiscard]] not_null<Ui::RpWidget*> MakeFolderIcon(
	not_null<Ui::RpWidget*> parent,
	const QString &folderName,
	int size);

// The folder accordion every "pick accounts" box shows: one row per folder
// (a tick selecting all of it, a click folding/unfolding it) with that
// folder's accounts under it. Shared by "Отправка SMS", "Накрутка" and
// "Экспорт" - each only supplies where its selection lives and what, if
// anything, differs.
struct AccountAccordionHooks {
	// Where the selection and the unfolded folders live (owned by the box).
	not_null<base::flat_set<not_null<Main::Account*>>*> selected;
	not_null<base::flat_set<QString>*> expandedFolders;
	// After the selection changed: refresh counters and labels.
	Fn<void()> selectionChanged;
	// Orders one folder's accounts (default: the app's own order).
	Fn<void(std::vector<not_null<Main::Account*>>&)> sortFolder;
	// Adds one account's row. Default: AddAccountPickerRow wired to
	// `selected`. A custom row has to keep `selected` up to date itself.
	Fn<void(
		not_null<Ui::VerticalLayout*> container,
		not_null<Main::Account*> account)> addAccountRow;
	// Optional extra widget on a folder's header (a button of its own).
	// Returns it, so a click on it isn't also taken as fold/select.
	Fn<QWidget*(
		not_null<Ui::RpWidget*> header,
		const std::vector<not_null<Main::Account*>> &accounts)> addHeaderWidget;
	QString emptyText = u"Нет добавленных аккаунтов"_q;
};

// The whole "pick accounts" block of a form: a select-all row, the folder
// accordion and a "Выбрано аккаунтов: N" line. A form's own state derives
// from this (so it keeps reading `selected`) and calls AddAccountPicker once.
struct AccountPickerState {
	base::flat_set<not_null<Main::Account*>> selected;
	base::flat_set<QString> expandedFolders;
	QPointer<Ui::VerticalLayout> accordion;
	QPointer<Ui::FlatLabel> selectedLabel;
	rpl::variable<int> selectedCount = 0;
	rpl::variable<QString> selectAllText = u"Выбрать всех"_q;
};

// The controller of the window the user is in right now, or `fallback`.
// A "Процессы" row reopens its window through this: the controller a process
// started with is gone after switching accounts.
[[nodiscard]] Window::SessionController *CurrentSessionController(
	Window::SessionController *fallback);

void AddAccountPicker(
	not_null<Ui::GenericBox*> box,
	not_null<AccountPickerState*> state,
	const QString &emptyText = QString());

// (Re)builds `accordion` from the live accounts and drops from the
// selection any account that has gone away since. Safe to call from the
// accordion's own handlers: rebuilding after a click is postponed.
void RebuildAccountAccordion(
	not_null<Ui::VerticalLayout*> accordion,
	const AccountAccordionHooks &hooks);

// ---- Sessions: spares, switching, restoring ----
// (see core/ggtg_sessions.h for the model: one key is only ever used in one
// place at a time, so a spare is a separate session, never a copy of one)

// The "type the 2FA password" box for DuplicateSession's requestPassword
// (shown in the active window; closing it skips the account).
[[nodiscard]] Fn<void(Fn<void(QString password)> submit, Fn<void()> cancel)>
MakeSessionPasswordPrompt(not_null<Main::Account*> account);

// Makes a separate spare session of `account` (a fresh login confirmed from
// the live one) and keeps it in the session vault. Needs the account's 2FA
// password saved when it has one. `error` is empty on success.
void CreateBackupSession(
	not_null<Main::Account*> account,
	const QString &note,
	Fn<void(QString error)> done);

// Same, but asks for the 2FA password through `requestPassword` when the
// saved one is missing or wrong (nullptr: don't ask), and gives the new
// spare's vault id.
void CreateBackupSessionWith(
	not_null<Main::Account*> account,
	const QString &note,
	Fn<void(Fn<void(QString password)> submit, Fn<void()> cancel)>
		requestPassword,
	Fn<void(QString error, QString id)> done,
	bool asAndroid = false);

void CreateMobileSessionWith(
	not_null<Main::Account*> account,
	const QString &note,
	Fn<void(Fn<void(QString password)> submit, Fn<void()> cancel)>
		requestPassword,
	Fn<void(QString error, QString id)> done);

void RunSequentialAccountsAction(
	not_null<Window::SessionController*> controller,
	std::vector<not_null<Main::Account*>> accounts,
	StepWithProgress step,
	const QString &title,
	std::shared_ptr<Ui::Show> show = nullptr);

void RunSequentialAccountsAction(
	not_null<Window::SessionController*> controller,
	std::vector<not_null<Main::Account*>> accounts,
	Step step,
	const QString &title,
	std::shared_ptr<Ui::Show> show = nullptr);

// Stops running `account` on its current session - locally only, nothing is
// sent to Telegram, so that session stays valid for whoever takes it - and
// runs the account on the session stored in `folder` (the folder that holds
// its "tdata"). The folder tag, saved 2FA password and proxy carry over.
void SwitchAccountToFolder(
	not_null<Main::Account*> account,
	const QString &folder,
	Fn<void(QString error)> done,
	std::optional<MTP::ProxyData> targetProxy = std::nullopt,
	std::optional<MTP::DeviceOverride> targetDevice = std::nullopt);

// Keeps `serialized` (a session of `account`, `hash` its number or 0) in the
// session vault under `role` ("backup", "duplicate", "android", "imported", "exported"). Returns
// the new entry's id, empty with `*error` on failure.
[[nodiscard]] QString StoreSessionKey(
	not_null<Main::Account*> account,
	const QString &role,
	const QString &note,
	const QByteArray &serialized,
	uint64 hash,
	QString *error,
	std::optional<MTP::DeviceOverride> device = std::nullopt);

// Exports a specific stored session to .ggtg, Session + JSON, or tdata folder.
void ExportSpecificSession(
	std::shared_ptr<Ui::Show> show,
	not_null<Main::Account*> account,
	const Core::GgtgSessions::StoredSession &stored);

// Ends the account's other sessions one by one, except the ones this app
// made or handed out (everything in the session vault with a known number).
// `report` says what was done.
void TerminateOtherSessions(
	not_null<Main::Account*> account,
	Fn<void(QString error, QString report)> done);

// What to warn about before switching onto a stored session that was handed
// out (a duplicate in an export, an Android session); empty for a spare.
[[nodiscard]] QString SwitchWarning(
	const Core::GgtgSessions::StoredSession &entry);

// SwitchAccountToFolder() that also finds out Telegram's number of the
// session being left and writes it to the vault entry `leftStoredId` (when
// not empty), so that session is recognized - and kept - later.
void SwitchAccountToFolderTracked(
	not_null<Main::Account*> account,
	const QString &folder,
	const QString &leftStoredId,
	Fn<void(QString error)> done,
	std::optional<MTP::ProxyData> targetProxy = std::nullopt,
	std::optional<MTP::DeviceOverride> targetDevice = std::nullopt);

// Switches `account` to its stored session `id` (a spare, a duplicate or an
// Android one). The session it leaves is kept as a spare, so nothing is
// lost and no key is ever used twice.
void SwapToStoredSession(
	not_null<Main::Account*> account,
	const QString &id,
	Fn<void(QString error)> done);

// The other half of handing an account's original session out: the original
// is already written wherever it goes. Keeps `newKey` (a session made with
// Main::DuplicateSessionTracked, `newHash` its number) as a spare, notes that
// the original was handed out (`handedOutNote`), then runs the account on
// the new session; the spare is dropped once it is the working one. If this
// is cut short the new session is still there as a spare.
void MoveToNewSession(
	not_null<Main::Account*> account,
	const QByteArray &newKey,
	uint64 newHash,
	const QString &handedOutNote,
	Fn<void(QString error)> done);

// Brings back an account that is not in the app any more (its session was
// lost) using its stored spare `id`.
void RestoreFromStoredSession(
	const QString &id,
	Fn<void(QString error)> done);

// What is wrong with an account right now - "Заморожен", "Спам-блок до …",
// "Спам-блок навсегда" - the very same answer the account manager gives, so
// the account list and the manager never disagree. Nothing when it is fine.
struct AccountTroubleLabel {
	QString text;
	QColor color;
};
[[nodiscard]] std::optional<AccountTroubleLabel> AccountTrouble(
	not_null<Main::Account*> account);

// A long text preview (settings/sms_campaign.cpp's recipients preview)
// capped to st::previewListHeight and scrollable internally instead of
// growing the whole box taller - see st::previewListScroll's comment in
// boxes.style. Returns the label so the caller can keep updating its text.
[[nodiscard]] not_null<Ui::FlatLabel*> AddScrollablePreview(
	not_null<Ui::VerticalLayout*> container,
	const QString &text);

// Same idea as AddScrollablePreview, but for a column of individually
// styled rows (settings/bulk_accounts_actions.cpp's saved-list viewers,
// each username/entry colored by its own send status) instead of one
// block of plain text - add rows into the returned layout the same way
// you would into any other Ui::VerticalLayout.
[[nodiscard]] not_null<Ui::VerticalLayout*> AddScrollableRowsArea(
	not_null<Ui::VerticalLayout*> container);

} // namespace Settings
