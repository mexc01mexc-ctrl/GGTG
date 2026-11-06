/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and exceptions, see https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

// ggtg: sessions in the sync. One record per session travels between the devices WITHOUT its key, so both
// the Mac and the phone show one picture: which session is on the phone, which on Desktop, which waits in
// quarantine, which is a spare. Sessions this device does not hold are kept apart from the key vault, in
// remote_sessions.json (the "remote catalog"), and are shown read-only; only the note can be edited, and the
// edit goes back to the device that holds the session. Format: docs/ggtg-formats.md ("sessions").

#include "core/ggtg_sessions.h"

#include <QtCore/QDateTime>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSaveFile>

#include <algorithm>
#include <optional>
#include <vector>

namespace Core::GgtgSessionCatalog {

struct Record {
	QString id;
	QString userId;
	QString phone;
	QString role;
	QString note;
	QString state; // "ok", "revoked" or empty
	QString platform; // device family the session was made for
	QString holderId; // sync id of the device that holds the key
	QString holder; // its name
	QString holderKind; // "desktop" or "android"
	bool current = false; // the account works on this session
	bool held = true; // false: only a note about a session whose key left (never put in the catalog)
	bool edited = false; // local only: the note was edited here and must go back to the holder
	qint64 createdAt = 0;
	qint64 updatedAt = 0;
	qint64 quarantineUntil = 0;
	quint64 hash = 0; // Telegram's number of the session
};

[[nodiscard]] inline QJsonObject ToJson(const Record &r, bool local = false) {
	auto result = QJsonObject{
		{ "id", r.id },
		{ "userId", r.userId },
		{ "phone", r.phone },
		{ "role", r.role },
		{ "note", r.note },
		{ "state", r.state },
		{ "platform", r.platform },
		{ "holderId", r.holderId },
		{ "holder", r.holder },
		{ "holderKind", r.holderKind },
		{ "current", r.current },
		{ "held", r.held },
		{ "createdAt", double(r.createdAt) },
		{ "updatedAt", double(r.updatedAt) },
		{ "quarantineUntil", double(r.quarantineUntil) },
		{ "hash", QString::number(r.hash) },
	};
	if (local && r.edited) {
		result.insert("edited", true);
	}
	return result;
}

[[nodiscard]] inline Record FromJson(const QJsonObject &o) {
	auto r = Record();
	r.id = o["id"].toString();
	r.userId = o["userId"].toString();
	r.phone = o["phone"].toString();
	r.role = o["role"].toString();
	r.note = o["note"].toString();
	r.state = o["state"].toString();
	r.platform = o["platform"].toString();
	r.holderId = o["holderId"].toString();
	r.holder = o["holder"].toString();
	r.holderKind = o["holderKind"].toString();
	r.current = o["current"].toBool();
	r.held = o.contains("held") ? o["held"].toBool() : true;
	r.edited = o["edited"].toBool();
	r.createdAt = qint64(o["createdAt"].toDouble());
	r.updatedAt = qint64(o["updatedAt"].toDouble());
	r.quarantineUntil = qint64(o["quarantineUntil"].toDouble());
	r.hash = o["hash"].toString().toULongLong();
	return r;
}

[[nodiscard]] inline QString CatalogPath() {
	return Core::GgtgPaths::DbDir() + u"remote_sessions.json"_q;
}

[[nodiscard]] inline std::vector<Record> Load() {
	auto result = std::vector<Record>();
	auto file = QFile(CatalogPath());
	if (!file.open(QIODevice::ReadOnly)) {
		return result;
	}
	const auto document = QJsonDocument::fromJson(file.readAll());
	for (const auto &value : document.object()["sessions"].toArray()) {
		result.push_back(FromJson(value.toObject()));
	}
	return result;
}

inline bool Save(const std::vector<Record> &records) {
	auto array = QJsonArray();
	for (const auto &r : records) {
		array.push_back(ToJson(r, true));
	}
	QDir().mkpath(QFileInfo(CatalogPath()).absolutePath());
	auto file = QSaveFile(CatalogPath());
	if (!file.open(QIODevice::WriteOnly)) {
		return false;
	}
	file.write(QJsonDocument(QJsonObject{
		{ "version", 1 },
		{ "sessions", array },
	}).toJson(QJsonDocument::Indented));
	return file.commit();
}

// Is this record the same session as a local vault entry? By Telegram's number; failing that by account and
// the moment the session was made. Never by account alone: an account has many sessions.
[[nodiscard]] inline bool SameSession(
		const Record &r,
		const Core::GgtgSessions::StoredSession &s) {
	if (r.userId != s.userId) {
		return false;
	}
	if (r.hash && s.hash) {
		return r.hash == s.hash;
	}
	return r.createdAt > 0 && r.createdAt == s.createdAt;
}

[[nodiscard]] inline QString HolderTitle(const Record &r) {
	if (r.holderKind == u"android"_q) {
		return u"на телефоне"_q
			+ (r.holder.isEmpty() ? QString() : (u" "_q + r.holder));
	}
	return u"на Desktop"_q
		+ (r.holder.isEmpty() ? QString() : (u" "_q + r.holder));
}

// "на телефоне Pixel · текущая · запасная · карантин до 07.11.2026 10:00 · подпись"
[[nodiscard]] inline QString Label(const Record &r) {
	auto parts = QStringList{ HolderTitle(r) };
	if (r.current) {
		parts.push_back(u"текущая"_q);
	}
	if (!r.role.isEmpty() && r.role != u"working"_q) {
		parts.push_back(Core::GgtgSessions::RoleTitle(r.role));
	}
	if (r.quarantineUntil > QDateTime::currentSecsSinceEpoch()) {
		parts.push_back(u"карантин до "_q + QDateTime::fromSecsSinceEpoch(
			r.quarantineUntil).toString(u"dd.MM.yyyy hh:mm"_q));
	}
	if (r.state == u"revoked"_q) {
		parts.push_back(u"завершена"_q);
	}
	if (r.createdAt > 0) {
		parts.push_back(QDateTime::fromSecsSinceEpoch(
			r.createdAt).toString(u"dd.MM.yyyy"_q));
	}
	if (!r.note.isEmpty()) {
		parts.push_back(r.note);
	}
	return parts.join(u" · "_q);
}

[[nodiscard]] inline std::vector<Record> ForUser(const QString &userId) {
	auto result = std::vector<Record>();
	for (const auto &r : Load()) {
		if (r.userId == userId) {
			result.push_back(r);
		}
	}
	return result;
}

// The record for a Telegram session number (to label a row of the account's live session list).
[[nodiscard]] inline std::optional<Record> ByHash(
		const QString &userId,
		quint64 hash) {
	if (!hash) {
		return std::nullopt;
	}
	for (const auto &r : ForUser(userId)) {
		if (r.hash == hash) {
			return r;
		}
	}
	return std::nullopt;
}

// Edits the note of a session held elsewhere. The edit is kept here, marked, and goes back with the next
// snapshot; the holder picks it by its updatedAt.
inline void EditNote(
		const QString &holderId,
		const QString &id,
		const QString &note) {
	auto all = Load();
	const auto now = QDateTime::currentSecsSinceEpoch();
	auto found = std::optional<Record>();
	for (auto &r : all) {
		if (r.holderId == holderId && r.id == id) {
			r.note = note;
			r.updatedAt = now;
			r.edited = true;
			found = r;
		}
	}
	Save(all);
	if (!found) {
		return;
	}
	// A local entry about the very same session (a note-only stub) follows.
	for (const auto &s : Core::GgtgSessions::LoadVault()) {
		if (SameSession(*found, s)) {
			Core::GgtgSessions::UpdateStoredNote(s.id, note);
		}
	}
}

// Merges the records of one snapshot. `snapshotDevice` is who made the snapshot, `myDevice` is this device.
// Returns the number of things that changed.
inline int Merge(
		const std::vector<Record> &records,
		const QString &snapshotDevice,
		const QString &myDevice) {
	namespace Sessions = Core::GgtgSessions;
	auto changed = 0;

	// 1. Sessions of this device that were written about from elsewhere (note, last write wins).
	auto vault = Sessions::LoadVault();
	// The stubs older versions put in the vault (source "sync", no key) were copies of other devices'
	// sessions: they belong in the catalog, not here.
	const auto stubs = std::remove_if(
		vault.begin(),
		vault.end(),
		[](const Sessions::StoredSession &s) {
			return s.source == u"sync"_q && !Sessions::IsHeld(s);
		});
	if (stubs != vault.end()) {
		vault.erase(stubs, vault.end());
		Sessions::SaveVault(vault);
		++changed;
	}
	auto vaultDirty = false;

	auto catalog = Load();
	auto presentFromHolder = std::vector<std::pair<QString, QString>>();
	for (const auto &r : records) {
		if (r.userId.isEmpty()) {
			continue;
		}
		if (r.held && r.holderId == snapshotDevice) {
			presentFromHolder.emplace_back(r.holderId, r.id);
		}
		auto matched = false;
		for (auto &s : vault) {
			if (!SameSession(r, s)
				&& !(r.holderId == myDevice && r.id == s.id)) {
				continue;
			}
			matched = true;
			if (r.updatedAt > s.updatedAt && r.note != s.note) {
				s.note = r.note;
				s.updatedAt = r.updatedAt;
				vaultDirty = true;
				++changed;
			}
			if (r.hash && !s.hash) {
				s.hash = r.hash;
				vaultDirty = true;
			}
		}
		if (matched) {
			continue;
		}
		// The working session of this device: its note lives with its birth date.
		if (r.holderId == myDevice && r.role == u"working"_q) {
			const auto own = Sessions::SessionNote(r.userId);
			if (r.updatedAt > own.at && r.note != own.text) {
				Sessions::SetSessionNote(r.userId, r.note, r.updatedAt);
				++changed;
			}
			continue;
		}
		if (r.holderId == myDevice || !r.held) {
			continue;
		}
		// 2. A session held by another device: the remote catalog.
		auto known = false;
		for (auto &c : catalog) {
			if (c.holderId != r.holderId || c.id != r.id) {
				continue;
			}
			known = true;
			if (c.edited && c.updatedAt > r.updatedAt) {
				// our edit is newer than what the holder knows: keep it, it goes back
				auto keep = r;
				keep.note = c.note;
				keep.updatedAt = c.updatedAt;
				keep.edited = true;
				c = keep;
				++changed;
			} else if (r.updatedAt != c.updatedAt || r.note != c.note
					|| r.state != c.state || r.current != c.current
					|| r.hash != c.hash || r.role != c.role
					|| r.quarantineUntil != c.quarantineUntil
					|| r.holder != c.holder) {
				c = r;
				c.edited = false;
				++changed;
			}
			break;
		}
		if (!known) {
			auto added = r;
			added.edited = false;
			catalog.push_back(added);
			++changed;
		}
	}
	if (vaultDirty) {
		Sessions::SaveVault(vault);
	}

	// 3. The snapshot's own device is the authority on what it holds: what it no longer lists is gone.
	if (!snapshotDevice.isEmpty() && snapshotDevice != myDevice) {
		const auto gone = std::remove_if(
			catalog.begin(),
			catalog.end(),
			[&](const Record &c) {
				return c.holderId == snapshotDevice
					&& std::find(
						presentFromHolder.begin(),
						presentFromHolder.end(),
						std::make_pair(c.holderId, c.id))
						== presentFromHolder.end();
			});
		if (gone != catalog.end()) {
			catalog.erase(gone, catalog.end());
			++changed;
		}
	}
	Save(catalog);
	return changed;
}

} // namespace Core::GgtgSessionCatalog
