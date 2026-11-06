/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and exceptions, see https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "core/ggtg_paths.h"

#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSaveFile>
#include <QtCore/QDateTime>

namespace Core::GgtgLayout {

// The arrangement of the accounts list (the order of accounts - which also gives the order of the folders - and
// which accounts are pinned) is shared between the devices by the sync. The device that changed it last wins:
// `at` is when this device's arrangement last changed (by the user, or taken over from another device).

[[nodiscard]] inline QString Path() {
	return GgtgPaths::DbDir() + u"layout_at.json"_q;
}

[[nodiscard]] inline qint64 At() {
	auto file = QFile(Path());
	if (!file.open(QIODevice::ReadOnly)) {
		return 0;
	}
	return QJsonDocument::fromJson(file.readAll()).object()
		.value(u"at"_q).toString().toLongLong();
}

inline bool &Applying() {
	static auto value = false; // true while a received arrangement is being applied: it is not a user change
	return value;
}

inline void SetAt(qint64 at) {
	const auto path = Path();
	if (path.isEmpty()) {
		return;
	}
	QDir().mkpath(QFileInfo(path).absolutePath());
	auto file = QSaveFile(path);
	if (file.open(QIODevice::WriteOnly)) {
		file.write(QJsonDocument(QJsonObject{
			{ u"at"_q, QString::number(at) },
		}).toJson(QJsonDocument::Compact));
		file.commit();
	}
}

// The user reordered or pinned something here.
inline void NoteChanged() {
	if (!Applying()) {
		SetAt(QDateTime::currentSecsSinceEpoch());
	}
}

} // namespace Core::GgtgLayout
