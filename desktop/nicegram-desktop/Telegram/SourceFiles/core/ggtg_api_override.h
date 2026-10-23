/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "core/ggtg_paths.h"

#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSaveFile>

namespace Core::GgtgApiOverride {

// The user's own api_id / api_hash (from my.telegram.org), used only when
// a new session is made for another device (auth.exportLoginToken): the
// shared public api_id is sometimes refused with API_ID_PUBLISHED_FLOOD.
struct Value {
	int apiId = 0;
	QString apiHash;

	[[nodiscard]] bool valid() const {
		return apiId > 0 && apiHash.size() == 32;
	}
};

[[nodiscard]] inline QString Path() {
	return GgtgPaths::DbDir() + u"api_override.json"_q;
}

[[nodiscard]] inline Value Load() {
	auto file = QFile(Path());
	if (!file.open(QIODevice::ReadOnly)) {
		return {};
	}
	const auto object = QJsonDocument::fromJson(file.readAll()).object();
	return {
		.apiId = object["apiId"].toString().toInt(),
		.apiHash = object["apiHash"].toString().trimmed(),
	};
}

inline void Save(const Value &value) {
	QDir().mkpath(GgtgPaths::DbDir());
	auto file = QSaveFile(Path());
	if (file.open(QIODevice::WriteOnly)) {
		file.write(QJsonDocument(QJsonObject{
			{ "apiId", QString::number(value.apiId) },
			{ "apiHash", value.apiHash },
		}).toJson(QJsonDocument::Compact));
		file.commit();
	}
}

} // namespace Core::GgtgApiOverride
