/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and exceptions, see https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "core/ggtg_paths.h"
#include "base/basic_types.h"

#include <algorithm>
#include <map>
#include <set>
#include <vector>

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QRegularExpression>
#include <QtCore/QSaveFile>

// ggtg: the answers of the people written to (sms_replies.json: {"<account id>:<username>": {"kind",
// "text", "at"}}) and the conversations the user marked as a success (ai_successes.json). This is
// what is left of the former "ИИ" feedback after the models were taken out (archive/ai/): the kinds
// of the answers (interest, a question, a refusal, hostility, other) are told by words alone.
namespace Core::GgtgReplies {

namespace details {

[[nodiscard]] inline QString Dir() {
	return Core::GgtgPaths::DbDir();
}

[[nodiscard]] inline QJsonObject ReadObject(const QString &path) {
	auto file = QFile(path);
	return file.open(QIODevice::ReadOnly)
		? QJsonDocument::fromJson(file.readAll()).object()
		: QJsonObject();
}

inline void WriteObject(const QString &path, const QJsonObject &object) {
	QDir().mkpath(Dir());
	auto file = QSaveFile(path);
	if (file.open(QIODevice::WriteOnly)) {
		file.write(QJsonDocument(object).toJson(QJsonDocument::Compact));
		file.commit();
	}
}

} // namespace details

// ---- the answers of the people written to ----
constexpr auto kMaxReplies = 2000;

struct Reply {
	QString text; // the first answer, as it was seen
	QString kind; // "interest", "question", "refusal", "negative", "other"
	qint64 at = 0;
};

[[nodiscard]] inline QString RepliesPath() {
	return details::Dir() + u"sms_replies.json"_q;
}

[[nodiscard]] inline QString ReplyKey(quint64 senderId, const QString &name) {
	return QString::number(senderId) + ':' + name.toLower();
}

[[nodiscard]] inline std::map<QString, Reply> LoadReplies() {
	auto result = std::map<QString, Reply>();
	const auto object = details::ReadObject(RepliesPath());
	for (auto i = object.begin(); i != object.end(); ++i) {
		const auto entry = i.value().toObject();
		result[i.key()] = {
			.text = entry.value("text").toString(),
			.kind = entry.value("kind").toString(),
			.at = qint64(entry.value("at").toDouble()),
		};
	}
	return result;
}

inline void SaveReplies(const std::map<QString, Reply> &replies) {
	auto sorted = std::vector<std::pair<qint64, QString>>();
	for (const auto &[key, reply] : replies) {
		sorted.push_back({ reply.at, key });
	}
	std::sort(sorted.begin(), sorted.end());
	auto skip = std::max(0, int(sorted.size()) - kMaxReplies);
	auto object = QJsonObject();
	for (const auto &[at, key] : sorted) {
		if (skip > 0) {
			--skip;
			continue;
		}
		const auto &reply = replies.at(key);
		object.insert(key, QJsonObject{
			{ "text", reply.text },
			{ "kind", reply.kind },
			{ "at", double(reply.at) },
		});
	}
	details::WriteObject(RepliesPath(), object);
}


namespace details {

[[nodiscard]] inline QString KindWord(const QString &kind) {
	if (kind == u"interest"_q) {
		return u"интерес"_q;
	} else if (kind == u"question"_q) {
		return u"вопрос"_q;
	} else if (kind == u"refusal"_q) {
		return u"отказ"_q;
	} else if (kind == u"negative"_q) {
		return u"негатив"_q;
	}
	return u"другое"_q;
}

[[nodiscard]] inline QString QuickKind(const QString &text) {
	const auto word = text.toLower().simplified();
	if (word.isEmpty() || word.size() > 60) {
		return QString();
	}
	static const auto negative = QRegularExpression(
		u"спам|заблок|жалоб|мошенн|развод|иди\\s|отстань|\\bfuck|"
		"\\bscam"_q);
	static const auto refusal = QRegularExpression(
		u"^(нет,?\\s*)?(спасибо,?\\s*)?(не интересно|не надо|не нужно|"
		"неинтересно|не пишите|не пиши|отпишись|удалите меня|"
		"no,? thanks?|not interested|stop)[.!]*$|^(нет|no)[.!]*$"_q);
	static const auto interest = QRegularExpression(
		u"сколько стоит|какая цена|цена\\??$|подробн|расскажите|"
		"расскажи|интересно|как записаться|how much|tell me more"_q);
	if (negative.match(word).hasMatch()) {
		return u"negative"_q;
	} else if (refusal.match(word).hasMatch()) {
		return u"refusal"_q;
	} else if (interest.match(word).hasMatch()
		&& !word.contains(u"не "_q)
		&& !word.contains(u"неинтерес"_q)
		&& !word.contains(u"нет"_q)) {
		// "Not interested" and the like are left to the model.
		return u"interest"_q;
	}
	return QString();
}


} // namespace details

// Remembers the first answer of a person (what it said stays even when the
// chat is cleaned later). Nothing changes if it is already known.
inline void RememberReply(const QString &key, const QString &text) {
	auto replies = LoadReplies();
	const auto i = replies.find(key);
	if (i != replies.end() && !i->second.text.isEmpty()) {
		return;
	}
	auto &reply = replies[key];
	reply.text = text.simplified().left(300);
	reply.at = QDateTime::currentSecsSinceEpoch();
	SaveReplies(replies);
}

[[nodiscard]] inline QString KindOfReply(const QString &key) {
	const auto replies = LoadReplies();
	const auto i = replies.find(key);
	return (i == replies.end()) ? QString() : i->second.kind;
}

// The answers that have a text and no kind yet (keys).
[[nodiscard]] inline std::vector<QString> UnsortedReplies() {
	auto result = std::vector<QString>();
	for (const auto &[key, reply] : LoadReplies()) {
		if (reply.kind.isEmpty() && !reply.text.isEmpty()) {
			result.push_back(key);
		}
	}
	return result;
}

// Sorts the given answers (keys) by their words; what the words do not tell is "other".
inline void ClassifyReplies(
		std::vector<QString> keys,
		Fn<void(int sorted, QString error)> done) {
	auto replies = LoadReplies();
	auto sorted = 0;
	for (const auto &key : keys) {
		const auto i = replies.find(key);
		if (i == replies.end() || i->second.text.isEmpty()) {
			continue;
		}
		const auto kind = details::QuickKind(i->second.text);
		i->second.kind = kind.isEmpty() ? u"other"_q : kind;
		++sorted;
	}
	if (sorted > 0) {
		SaveReplies(replies);
	}
	if (done) {
		done(sorted, QString());
	}
}

// ---- conversations marked as a success ----

[[nodiscard]] inline QString SuccessPath() {
	return details::Dir() + u"ai_successes.json"_q;
}

[[nodiscard]] inline std::set<QString> LoadSuccesses() {
	auto result = std::set<QString>();
	const auto array = details::ReadObject(SuccessPath())
		.value("items").toArray();
	for (const auto &value : array) {
		const auto name = value.toObject().value("u").toString().toLower();
		if (!name.isEmpty()) {
			result.insert(name);
		}
	}
	return result;
}

[[nodiscard]] inline bool IsSuccess(const QString &username) {
	return !username.isEmpty()
		&& LoadSuccesses().contains(username.toLower());
}

// The first answer of a person by their @username (the same person can have
// answered several of the user's accounts: the newest answer counts).
[[nodiscard]] inline Reply ReplyOfUsername(const QString &username) {
	auto result = Reply();
	const auto tail = ':' + username.toLower();
	if (username.isEmpty()) {
		return result;
	}
	for (const auto &[key, reply] : LoadReplies()) {
		if (key.endsWith(tail) && reply.at >= result.at) {
			result = reply;
		}
	}
	return result;
}

// What to do with an answer, by its kind: the suggestion shown above a draft.
[[nodiscard]] inline QString SuggestedAction(const QString &kind) {
	if (kind == u"interest"_q) {
		return u"Рекомендация: человеку интересно, ответьте сегодня и "
			"предложите следующий шаг."_q;
	} else if (kind == u"question"_q) {
		return u"Рекомендация: ответьте честно: кто вы, чем занимаетесь, "
			"откуда контакт. Не давите."_q;
	} else if (kind == u"refusal"_q) {
		return u"Рекомендация: человек отказался, лучше не писать "
			"больше (ответ ниже только как вариант)."_q;
	} else if (kind == u"negative"_q) {
		return u"Рекомендация: не отвечайте и уберите человека из "
			"рассылок: ответ может ухудшить репутацию аккаунта."_q;
	}
	return QString();
}

inline void SetSuccess(const QString &username, bool success) {
	if (username.isEmpty()) {
		return;
	}
	auto names = LoadSuccesses();
	if (success) {
		names.insert(username.toLower());
	} else {
		names.erase(username.toLower());
	}
	auto items = QJsonArray();
	for (const auto &name : names) {
		items.push_back(QJsonObject{
			{ "u", name },
			{ "at", double(QDateTime::currentSecsSinceEpoch()) },
		});
	}
	details::WriteObject(SuccessPath(), QJsonObject{ { "items", items } });
}

} // namespace Core::GgtgReplies
