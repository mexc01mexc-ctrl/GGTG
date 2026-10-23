/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and exceptions, see https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "settings.h"
#include "core/ggtg_paths.h"

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QMutex>
#include <QtCore/QMutexLocker>
#include <QtCore/QTextStream>

namespace Core::GgtgLog {

// One persistent journal for every GGTG section (SMS, Базы, Сквидвард,
// Прокси, Массовые действия...): tdata/ggtg_db/ggtg.log, a line
// per event - "2026-01-01 12:00:00 [INFO] [Отправка SMS] text". When it
// passes kMaxBytes it is rotated: ggtg.log -> ggtg.log.1 -> ... ->
// ggtg.log.<kKeptFiles>, the oldest dropped, so at most about
// (kKeptFiles + 1) * kMaxBytes stay on disk. ProcessManager mirrors every
// process's start/log/finish here, so anything already reporting to the
// "Процессы" list is in the file too; code with no process (opening/
// editing a base, Сквидвард lookups) calls Write directly.
constexpr auto kMaxBytes = qint64(2 * 1024 * 1024);
constexpr auto kKeptFiles = 5;

[[nodiscard]] inline QString Path() {
	return Core::GgtgPaths::DbDir() + u"ggtg.log"_q;
}

namespace details {

// The file stays open between lines: reopening it (plus a mkpath and a
// size check) for every line made a campaign that logs thousands of lines
// from the UI thread needlessly slow.
struct Sink {
	QMutex mutex;
	QFile file;
	QString path;
	qint64 size = 0;
};

[[nodiscard]] inline Sink &SinkInstance() {
	static auto sink = Sink();
	return sink;
}

[[nodiscard]] inline bool EnsureOpen(Sink &sink) {
	const auto path = Path();
	if (sink.file.isOpen() && sink.path == path) {
		return true;
	}
	sink.file.close();
	QDir().mkpath(QFileInfo(path).absolutePath());
	sink.path = path;
	sink.file.setFileName(path);
	if (!sink.file.open(QIODevice::WriteOnly | QIODevice::Append)) {
		return false;
	}
	sink.size = sink.file.size();
	return true;
}

inline void Rotate(Sink &sink) {
	sink.file.close();
	const auto numbered = [&](int index) {
		return sink.path + '.' + QString::number(index);
	};
	QFile::remove(numbered(kKeptFiles));
	for (auto i = kKeptFiles - 1; i >= 1; --i) {
		QFile::rename(numbered(i), numbered(i + 1));
	}
	QFile::rename(sink.path, numbered(1));
}

} // namespace details

inline void Write(
		const QString &level,
		const QString &section,
		const QString &text) {
	const auto line = (QDateTime::currentDateTime().toString(
		u"yyyy-MM-dd HH:mm:ss"_q)
		+ u" ["_q + level + u"] ["_q + section + u"] "_q
		+ QString(text).replace('\n', u" | "_q)
		+ '\n').toUtf8();
	auto &sink = details::SinkInstance();
	const auto locker = QMutexLocker(&sink.mutex);
	if (!details::EnsureOpen(sink)) {
		return;
	}
	if (sink.size > kMaxBytes) {
		details::Rotate(sink);
		if (!details::EnsureOpen(sink)) {
			return;
		}
		if (sink.size > kMaxBytes) {
			// The rename failed (e.g. the file is open in an editor on
			// Windows): don't retry on every line, try again after
			// another kMaxBytes.
			sink.size = 0;
		}
	}
	sink.size += sink.file.write(line);
	sink.file.flush();
}

inline void Info(const QString &section, const QString &text) {
	Write(u"INFO"_q, section, text);
}
inline void Warn(const QString &section, const QString &text) {
	Write(u"WARN"_q, section, text);
}
inline void Error(const QString &section, const QString &text) {
	Write(u"ERROR"_q, section, text);
}

} // namespace Core::GgtgLog
