/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "api/api_authorizations.h"
#include "settings/settings_type.h"

namespace Ui {
class RpWidget;
class Show;
class VerticalLayout;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

namespace Settings {

[[nodiscard]] Type SessionsId();

void AddSessionInfoRow(
	not_null<Ui::VerticalLayout*> container,
	rpl::producer<QString> label,
	const QString &value,
	const style::icon &icon);

// One session as a row like in Telegram's own "Active sessions" list: device
// icon, name, app, place and time, and a cross that ends the session
// (`terminate` empty = no cross, e.g. for the session this app runs as).
// `nameSuffix` is shown after the name. `showInfo` is called on a click
// elsewhere on the row. `switchSession` places a switch icon on the right.
[[nodiscard]] object_ptr<Ui::RpWidget> MakeSessionRow(
	not_null<QWidget*> parent,
	const Api::Authorizations::Entry &entry,
	const QString &nameSuffix,
	Fn<void()> showInfo,
	Fn<void()> terminate,
	Fn<void()> switchSession = nullptr);

// The details box of one session (app, system, IP, location) with the
// "Terminate" button, as in the original list.
void ShowSessionInfo(
	std::shared_ptr<Ui::Show> show,
	const Api::Authorizations::Entry &entry,
	Fn<void(uint64)> terminate);

} // namespace Settings
