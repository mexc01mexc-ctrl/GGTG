/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/object_ptr.h"

namespace Window {
class SessionController;
} // namespace Window

namespace Ui {
class BoxContent;
class Checkbox;
class FlatLabel;
class RpWidget;
class Show;
class VerticalLayout;
} // namespace Ui

namespace Main {
class Account;
} // namespace Main

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

// Opens the "Массовые действия" flow. If a bulk action queue is currently
// running or just finished, reopens its live progress box (hidden by
// "Закрыть", never lost - see RunBulkAction in bulk_accounts_actions.cpp);
// otherwise starts the normal folder/account/action picker chain via
// MakeBulkFoldersBox.
void OpenBulkActions(not_null<Window::SessionController*> controller);

// Entry point (from the side menu): pick a folder, then pick accounts
// inside it (photo/name/@username/phone shown per row), then pick what to
// do to all of them - replace/add a profile photo, rename, set/remove a
// username, set/remove the bio, set/change/remove the cloud (2FA)
// password, move to a folder, or deactivate their other sessions. Every
// bulk action runs strictly one account at a time with a randomized pause
// between accounts (see bulk_accounts_actions.cpp) - never in parallel
[[nodiscard]] object_ptr<Ui::BoxContent> MakeBulkFoldersBox(
	not_null<Window::SessionController*> controller);

// "Менеджер аккаунтов" (the GGTG window's account list, see
// window/window_main_menu.cpp): every account grouped under its folder
// header, a click on a row switches to that account, and a folder can be
// renamed/removed or an account moved into one. `closeShell` is called
// right before switching - the window this lives in is bound to the
// account being switched away from, exactly like the main menu's own
// account list closes itself first. Every prompt it opens goes through
// this box's own Ui::Show (not `controller`), so it appears inside
// whichever window the manager itself is shown in.
[[nodiscard]] object_ptr<Ui::BoxContent> MakeAccountManagerBox(
	not_null<Window::SessionController*> controller,
	Fn<void()> closeShell);

// Actions "База" can hand off to the main window (they need its session
// controller, which the Ui::Show "База" itself runs on doesn't give). Only
// the main-window opener fills them in; each button they drive is simply
// absent when its action is empty (e.g. inside the "Отправка SMS" box).
struct BasesActions {
	// A users base -> "Отправка SMS" with only that base checked.
	Fn<void(uint64 baseId)> sendToBase;
	// A group of a groups base (by @username) -> collect its members into
	// a base, reading its member list directly.
	Fn<void(const QString &username)> scrapeGroup;
	// The same through the Сквидвард bot, for a group whose member list is
	// hidden; the dialog opens over `show`'s window.
	Fn<void(std::shared_ptr<Ui::Show> show, const QString &username)>
		scrapeGroupViaSquidward;
	// Open the keyword group search.
	Fn<void()> searchGroups;
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

// Below: shared with settings/sms_campaign.cpp - kept here (not anonymous-
// namespace-local in the .cpp) purely so that sibling file can reuse them
// instead of duplicating account-row rendering / the guarded-callback
// idiom used throughout bulk_accounts_actions.cpp's own step queues.

using Accounts = std::vector<not_null<Main::Account*>>;
using StepDone = Fn<void(bool success, QString message)>;
using Step = Fn<void(not_null<Main::Account*> account, StepDone done)>;

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
