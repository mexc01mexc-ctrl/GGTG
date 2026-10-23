/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and exceptions, see https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "core/ggtg_paths.h"

#include <QtCore/QDateTime>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSaveFile>

#include <map>

namespace Core::GgtgSmsStats {

// Every SMS attempt of a campaign, with everything that may explain why it
// went or did not: who sent (the country of the account's number, Premium,
// how much it sent today), to whom (language of the name, Premium, when the
// person was last seen, a photo, a bio), what was sent (template, language
// it was translated into, sticker, text, parts) and how it ended. One JSON
// object per line in tdata/ggtg_db/sms_attempts.jsonl; the AI reads the
// totals of it ("Разбор рассылок") to tell what works and what does not.
struct Attempt {
	qint64 at = 0; // unix seconds
	qint64 run = 0; // the campaign's start, to tell runs apart
	QString sender; // account label
	quint64 senderId = 0;
	QString senderCountry; // ISO2 by the account's phone number
	bool senderPremium = false;
	int senderSentToday = 0;
	QString recipient; // username
	QString language; // of the name: "ko", "ru"... "lat" or ""
	bool recipientPremium = false;
	int seen = 0; // 0 unknown, 1 recently, 2 week, 3 month, 4 long ago
	bool photo = false;
	bool about = false;
	int templateIndex = 0;
	// The beginning of the template itself (before [NAME] is filled in): the
	// same number means another text in another run, the text does not.
	QString templateText;
	QString translated; // the language code the text went out in, if any
	QString sticker; // "off", "sent", "failed", "none" (not found)
	QString text; // "sent", "failed", "none" (not tried)
	int parts = 0;
	QString outcome; // "sent", "spam", "skip", "notfound", "flood", "dead", "error"
	QString error;
	int hour = 0; // local hour of the attempt
	int seconds = 0; // how long the attempt took
	bool replied = false; // filled in later, when the report is built
	// What the answer was: "interest", "question", "refusal", "negative",
	// "other" (see Feedback in ggtg_ai_feedback.h), or "success" when the
	// user marked the conversation as one. Filled in with `replied`.
	QString replyKind;
};

// The kinds of an answer, as the reader sees them.
[[nodiscard]] inline QString ReplyKindTitle(const QString &kind) {
	if (kind == u"interest"_q) {
		return u"интерес"_q;
	} else if (kind == u"question"_q) {
		return u"вопрос"_q;
	} else if (kind == u"refusal"_q) {
		return u"отказ"_q;
	} else if (kind == u"negative"_q) {
		return u"негатив"_q;
	} else if (kind == u"success"_q) {
		return u"успех"_q;
	} else if (kind == u"other"_q) {
		return u"другое"_q;
	}
	return QString();
}

// A good answer: the person is interested, asks, or the deal is done.
[[nodiscard]] inline bool IsPositiveKind(const QString &kind) {
	return kind == u"interest"_q
		|| kind == u"question"_q
		|| kind == u"success"_q;
}

constexpr auto kMaxBytes = qint64(8 * 1024 * 1024);

[[nodiscard]] inline QString Path() {
	return Core::GgtgPaths::DbDir() + u"sms_attempts.jsonl"_q;
}

[[nodiscard]] inline QJsonObject Serialize(const Attempt &a) {
	return QJsonObject{
		{ "at", double(a.at) },
		{ "run", double(a.run) },
		{ "sender", a.sender },
		{ "senderId", QString::number(a.senderId) },
		{ "senderCountry", a.senderCountry },
		{ "senderPremium", a.senderPremium },
		{ "senderSentToday", a.senderSentToday },
		{ "recipient", a.recipient },
		{ "lang", a.language },
		{ "recPremium", a.recipientPremium },
		{ "seen", a.seen },
		{ "photo", a.photo },
		{ "about", a.about },
		{ "template", a.templateIndex },
		{ "templateText", a.templateText },
		{ "translated", a.translated },
		{ "sticker", a.sticker },
		{ "text", a.text },
		{ "parts", a.parts },
		{ "outcome", a.outcome },
		{ "error", a.error },
		{ "hour", a.hour },
		{ "seconds", a.seconds },
	};
}

[[nodiscard]] inline Attempt Parse(const QJsonObject &o) {
	return Attempt{
		.at = qint64(o["at"].toDouble()),
		.run = qint64(o["run"].toDouble()),
		.sender = o["sender"].toString(),
		.senderId = o["senderId"].toString().toULongLong(),
		.senderCountry = o["senderCountry"].toString(),
		.senderPremium = o["senderPremium"].toBool(),
		.senderSentToday = o["senderSentToday"].toInt(),
		.recipient = o["recipient"].toString(),
		.language = o["lang"].toString(),
		.recipientPremium = o["recPremium"].toBool(),
		.seen = o["seen"].toInt(),
		.photo = o["photo"].toBool(),
		.about = o["about"].toBool(),
		.templateIndex = o["template"].toInt(),
		.templateText = o["templateText"].toString(),
		.translated = o["translated"].toString(),
		.sticker = o["sticker"].toString(),
		.text = o["text"].toString(),
		.parts = o["parts"].toInt(),
		.outcome = o["outcome"].toString(),
		.error = o["error"].toString(),
		.hour = o["hour"].toInt(),
		.seconds = o["seconds"].toInt(),
	};
}

// Appends one attempt; past kMaxBytes the older half of the file goes.
inline void Append(const Attempt &attempt) {
	const auto path = Path();
	auto line = QJsonDocument(Serialize(attempt)).toJson(
		QJsonDocument::Compact);
	line += '\n';
	auto file = QFile(path);
	if (file.size() > kMaxBytes && file.open(QIODevice::ReadOnly)) {
		const auto all = file.readAll();
		file.close();
		const auto from = all.indexOf('\n', all.size() / 2);
		auto save = QSaveFile(path);
		if (save.open(QIODevice::WriteOnly)) {
			save.write((from >= 0) ? all.mid(from + 1) : QByteArray());
			save.commit();
		}
	}
	if (file.open(QIODevice::WriteOnly | QIODevice::Append)) {
		file.write(line);
	}
}

// The file is parsed again only when it changed (size or time): screens ask
// for the attempts often, the file changes once per sent message.
[[nodiscard]] inline std::vector<Attempt> Load(int days = 0) {
	struct Cache {
		qint64 size = -1;
		QDateTime modified;
		std::vector<Attempt> all;
	};
	static auto cache = Cache();
	const auto info = QFileInfo(Path());
	if (!info.exists()) {
		cache = Cache();
		return {};
	}
	if (cache.size != info.size() || cache.modified != info.lastModified()) {
		auto parsed = std::vector<Attempt>();
		auto file = QFile(Path());
		if (!file.open(QIODevice::ReadOnly)) {
			return {};
		}
		while (!file.atEnd()) {
			const auto line = file.readLine().trimmed();
			if (line.isEmpty()) {
				continue;
			}
			const auto object = QJsonDocument::fromJson(line).object();
			if (!object.isEmpty()) {
				parsed.push_back(Parse(object));
			}
		}
		cache = Cache{ info.size(), info.lastModified(), std::move(parsed) };
	}
	if (days <= 0) {
		return cache.all;
	}
	const auto since = QDateTime::currentSecsSinceEpoch()
		- qint64(days) * 86400;
	auto result = std::vector<Attempt>();
	for (const auto &attempt : cache.all) {
		if (attempt.at >= since) {
			result.push_back(attempt);
		}
	}
	return result;
}

inline void Clear() {
	QFile::remove(Path());
}

// The AI's conclusions from the last "Разбор рассылок", kept so the
// template assistant can lean on them.
[[nodiscard]] inline QString InsightsPath() {
	return Core::GgtgPaths::DbDir() + u"ai_sms_insights.txt"_q;
}

[[nodiscard]] inline QString LoadInsights() {
	auto file = QFile(InsightsPath());
	return file.open(QIODevice::ReadOnly)
		? QString::fromUtf8(file.readAll()).trimmed()
		: QString();
}

inline void SaveInsights(const QString &text) {
	auto file = QSaveFile(InsightsPath());
	if (file.open(QIODevice::WriteOnly)) {
		file.write(text.trimmed().toUtf8());
		file.commit();
	}
}

// The start of a template for the records and the report: one line, 70
// characters.
[[nodiscard]] inline QString TemplatePreview(QString text) {
	text = text.simplified();
	if (text.size() > 70) {
		text = text.left(70).trimmed() + u"..."_q;
	}
	return text;
}

namespace details {

struct Bucket {
	int total = 0;
	int sent = 0;
	int spam = 0;
	int replied = 0;
	int known = 0; // answers whose kind is known
	int positive = 0; // of them: interest, a question, a success
};

[[nodiscard]] inline QString Percent(int part, int whole) {
	return whole
		? (QString::number(int(100. * part / whole + 0.5)) + '%')
		: u"-"_q;
}

[[nodiscard]] inline QString Table(
		const QString &title,
		const std::map<QString, Bucket> &buckets) {
	auto result = title + u":\n"_q;
	for (const auto &[key, b] : buckets) {
		result += u"  "_q + (key.isEmpty() ? u"?"_q : key)
			+ u": попыток "_q + QString::number(b.total)
			+ u", ушло "_q + QString::number(b.sent)
			+ u" ("_q + Percent(b.sent, b.total) + u")"_q
			+ u", Telegram не пустил (PEER_FLOOD) "_q + QString::number(b.spam)
			+ u", ответили "_q + QString::number(b.replied)
			+ u" ("_q + Percent(b.replied, b.sent) + u")"_q
			+ (b.known
				? (u", положительных "_q + QString::number(b.positive)
					+ u" ("_q + Percent(b.positive, b.sent) + u")"_q
					+ (b.known < b.replied
						? (u" [типы известны у "_q
							+ QString::number(b.known) + u" из "_q
							+ QString::number(b.replied) + ']')
						: QString()))
				: QString())
			+ '\n';
	}
	return buckets.empty() ? QString() : result;
}

[[nodiscard]] inline QString SeenText(int seen) {
	switch (seen) {
	case 1: return u"был недавно"_q;
	case 2: return u"на этой неделе"_q;
	case 3: return u"в этом месяце"_q;
	case 4: return u"давно"_q;
	}
	return u"скрыто"_q;
}

[[nodiscard]] inline QString HourText(int hour) {
	const auto from = (hour / 3) * 3;
	return u"%1:00-%2:59"_q
		.arg(from, 2, 10, QChar('0'))
		.arg(from + 2, 2, 10, QChar('0'));
}

} // namespace details

struct Collected {
	details::Bucket all;
	std::map<QString, details::Bucket> byCountry;
	std::map<QString, details::Bucket> byLanguage;
	std::map<QString, details::Bucket> byPremium;
	std::map<QString, details::Bucket> bySeen;
	std::map<QString, details::Bucket> byTemplate;
	std::map<QString, details::Bucket> byTranslated;
	std::map<QString, details::Bucket> bySticker;
	std::map<QString, details::Bucket> byHour;
	std::map<QString, details::Bucket> bySender;
	std::map<QString, details::Bucket> byPhoto;
	std::map<QString, int> errors;
	int stickerOnly = 0;
	std::map<qint64, int> runs;
};

// Every real attempt put into the groups the report and the report screen
// share. A username that was not found, a skipped bot and a FLOOD_WAIT say
// nothing about delivery and are left out.
[[nodiscard]] inline Collected Collect(
		const std::vector<Attempt> &attempts,
		Fn<QString(QString)> languageName,
		Fn<QString(QString)> countryName) {
	using details::Bucket;
	auto c = Collected();
	auto &all = c.all;
	auto &byCountry = c.byCountry;
	auto &byLanguage = c.byLanguage;
	auto &byPremium = c.byPremium;
	auto &bySeen = c.bySeen;
	auto &byTemplate = c.byTemplate;
	auto &byTranslated = c.byTranslated;
	auto &bySticker = c.bySticker;
	auto &byHour = c.byHour;
	auto &bySender = c.bySender;
	auto &byPhoto = c.byPhoto;
	auto &errors = c.errors;
	auto &stickerOnly = c.stickerOnly;
	auto &runs = c.runs;
	for (const auto &a : attempts) {
		if (a.outcome == u"skip"_q
			|| a.outcome == u"notfound"_q
			|| a.outcome == u"flood"_q
			|| a.outcome == u"dead"_q) {
			continue;
		}
		const auto add = [&](Bucket &b) {
			++b.total;
			if (a.outcome == u"sent"_q) {
				++b.sent;
				if (a.replied) {
					++b.replied;
					if (!a.replyKind.isEmpty()) {
						++b.known;
						if (IsPositiveKind(a.replyKind)) {
							++b.positive;
						}
					}
				}
			} else if (a.outcome == u"spam"_q || a.outcome == u"peer"_q) {
				++b.spam;
			}
		};
		++runs[a.run];
		add(all);
		add(byCountry[countryName(a.senderCountry)]);
		add(byLanguage[languageName(a.language)]);
		add(byPremium[(a.senderPremium
			? u"у меня Premium"_q
			: u"у меня без Premium"_q)
			+ u", "_q
			+ (a.recipientPremium
				? u"у получателя Premium"_q
				: u"у получателя без Premium"_q)]);
		add(bySeen[details::SeenText(a.seen)]);
		add(byTemplate[a.templateText.isEmpty()
			? (u"шаблон №"_q + QString::number(a.templateIndex + 1)
				+ u" (старые записи)"_q)
			: (u"«"_q + a.templateText + u"»"_q)]);
		add(byTranslated[a.translated.isEmpty()
			? u"без перевода"_q
			: (u"переведён: "_q + languageName(a.translated))]);
		add(bySticker[(a.sticker == u"off"_q)
			? u"без стикера"_q
			: u"со стикером"_q]);
		add(byHour[details::HourText(a.hour)]);
		add(bySender[a.sender]);
		add(byPhoto[a.photo ? u"есть фото"_q : u"нет фото"_q]);
		if (!a.error.isEmpty() && a.outcome != u"sent"_q) {
			++errors[a.error];
		}
		if (a.sticker == u"sent"_q && a.text == u"failed"_q) {
			++stickerOnly;
		}
	}
	return c;
}

struct Section {
	QString title;
	std::vector<std::pair<QString, details::Bucket>> rows;
};

[[nodiscard]] inline std::vector<Section> Sections(
		const std::vector<Attempt> &attempts,
		Fn<QString(QString)> languageName,
		Fn<QString(QString)> countryName) {
	const auto c = Collect(attempts, languageName, countryName);
	const auto flat = [](const std::map<QString, details::Bucket> &m) {
		return std::vector<std::pair<QString, details::Bucket>>(
			m.begin(),
			m.end());
	};
	return {
		{ u"По стране номера моего аккаунта"_q, flat(c.byCountry) },
		{ u"По языку получателя (по имени)"_q, flat(c.byLanguage) },
		{ u"Premium"_q, flat(c.byPremium) },
		{ u"Когда получатель был в сети"_q, flat(c.bySeen) },
		{ u"Фото у получателя"_q, flat(c.byPhoto) },
		{ u"Перевод"_q, flat(c.byTranslated) },
		{ u"Стикер"_q, flat(c.bySticker) },
		{ u"Время отправки"_q, flat(c.byHour) },
	};
}

// Plain-text totals by everything that was recorded - what the user reads
// and what the AI gets for its conclusions. Only real attempts count
// (a username that was not found or a skipped bot says nothing).
[[nodiscard]] inline QString Report(
		const std::vector<Attempt> &attempts,
		Fn<QString(QString)> languageName,
		Fn<QString(QString)> countryName) {
	const auto c = Collect(attempts, languageName, countryName);
	const auto &all = c.all;
	const auto &byCountry = c.byCountry;
	const auto &byLanguage = c.byLanguage;
	const auto &byPremium = c.byPremium;
	const auto &bySeen = c.bySeen;
	const auto &byTemplate = c.byTemplate;
	const auto &byTranslated = c.byTranslated;
	const auto &bySticker = c.bySticker;
	const auto &byHour = c.byHour;
	const auto &bySender = c.bySender;
	const auto &byPhoto = c.byPhoto;
	const auto &errors = c.errors;
	const auto stickerOnly = c.stickerOnly;
	const auto &runs = c.runs;
	if (!all.total) {
		return QString();
	}
	auto result = u"Всего попыток: "_q + QString::number(all.total)
		+ u" (рассылок: "_q + QString::number(runs.size()) + u")"_q
		+ u", ушло "_q + QString::number(all.sent)
		+ u" ("_q + details::Percent(all.sent, all.total) + u")"_q
		+ u", Telegram не пустил (PEER_FLOOD) "_q + QString::number(all.spam)
		+ u", ответили "_q + QString::number(all.replied)
		+ u" ("_q + details::Percent(all.replied, all.sent) + u")\n"_q;
	if (all.known) {
		auto kinds = std::map<QString, int>();
		for (const auto &a : attempts) {
			if (a.outcome == u"sent"_q && a.replied
				&& !a.replyKind.isEmpty()) {
				++kinds[ReplyKindTitle(a.replyKind)];
			}
		}
		result += u"Типы ответов:"_q;
		for (const auto &[title, count] : kinds) {
			result += u" "_q + title + ' ' + QString::number(count) + ';';
		}
		result += u" положительных всего "_q + QString::number(all.positive)
			+ u" ("_q + details::Percent(all.positive, all.sent)
			+ u" от ушедших)\n"_q;
	}
	if (stickerOnly) {
		result += u"Стикер ушёл, а текст нет: "_q
			+ QString::number(stickerOnly) + '\n';
	}
	result += '\n'
		+ details::Table(u"По стране номера моего аккаунта"_q, byCountry)
		+ details::Table(u"По языку получателя (по имени)"_q, byLanguage)
		+ details::Table(u"Premium"_q, byPremium)
		+ details::Table(u"Когда получатель был в сети"_q, bySeen)
		+ details::Table(u"Фото у получателя"_q, byPhoto)
		+ details::Table(u"Шаблон"_q, byTemplate)
		+ details::Table(u"Перевод"_q, byTranslated)
		+ details::Table(u"Стикер"_q, bySticker)
		+ details::Table(u"Время отправки"_q, byHour)
		+ details::Table(u"Мои аккаунты"_q, bySender);
	if (!errors.empty()) {
		result += u"Ошибки:\n"_q;
		for (const auto &[error, count] : errors) {
			result += u"  "_q + error + u": "_q + QString::number(count)
				+ '\n';
		}
	}
	return result;
}

} // namespace Core::GgtgSmsStats
