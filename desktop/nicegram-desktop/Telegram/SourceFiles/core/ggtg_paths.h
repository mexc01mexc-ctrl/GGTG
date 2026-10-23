/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and exceptions, see https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "settings.h"

#include <QtCore/QDir>

namespace Core::GgtgPaths {

// Where GGTG keeps its own data (bases, journals, session vault, logs):
// tdata/ggtg_db/. The folder used to be called tdata/nicegram_bulk_db/; on the
// first call it is renamed in place, so nothing is lost on update.
[[nodiscard]] inline QString DbDir() {
	const auto dir = cWorkingDir();
	if (dir.isEmpty()) {
		return QString();
	}
	const auto root = dir + u"tdata/"_q;
	const auto current = root + u"ggtg_db"_q;
	const auto old = root + u"nicegram_bulk_db"_q;
	if (!QDir(current).exists() && QDir(old).exists()) {
		QDir().rename(old, current);
	}
	return current + '/';
}

} // namespace Core::GgtgPaths
