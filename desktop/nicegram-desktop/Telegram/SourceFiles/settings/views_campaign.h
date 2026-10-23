/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/object_ptr.h"

namespace Ui {
class BoxContent;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

namespace Settings {

enum class ViewsIndicator {
	None,
	Running,
	Done,
};

// Drives the "Просмотры" menu row's spinner/checkmark suffix, the same way
// SmsCampaignIndicatorValue() does for "Отправка SMS".
[[nodiscard]] rpl::producer<ViewsIndicator> ViewsIndicatorValue();

// "Накрутка" (views + joining channels/groups): pick accounts (by folder, individually or all), give post
// links (t.me/name/123, a range t.me/name/123-145, t.me/c/ID/123, one per
// line) and a thread count - every picked account then views every post,
// `threads` accounts at a time; or, in "Вступление" mode, links to
// channels/groups (t.me/name, @name, t.me/+hash) to join. If a run is going or just finished, this
// reopens its live progress box instead of the setup form.
void OpenViews(not_null<Window::SessionController*> controller);

[[nodiscard]] object_ptr<Ui::BoxContent> MakeViewsSetupBoxFresh(
	not_null<Window::SessionController*> controller);

} // namespace Settings
