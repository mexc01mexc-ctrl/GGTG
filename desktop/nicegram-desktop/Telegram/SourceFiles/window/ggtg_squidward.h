/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and exceptions, see https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/object_ptr.h"

#include <QtCore/QString>

namespace Ui {
class BoxContent;
} // namespace Ui

namespace Window {

class SessionController;

// The GGTG "Сквидвард" section: a curated front end for two of
// @skvidvarde_bot's lookups (by user, by group/channel) whose results can
// be saved into "База".
//
// `lookupGroup`, when not empty, is a group's @username to be looked up
// ("Пробить группу") the moment the bot is connected, ending in the same
// "Топ, люди" result the manual lookup gives - used by "База"'s per-group
// Сквидвард button, for groups whose member list can't be read directly.
[[nodiscard]] object_ptr<Ui::BoxContent> MakeSquidwardBox(
	not_null<SessionController*> controller,
	const QString &lookupGroup = QString());

} // namespace Window
