/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and exceptions, see https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "core/ggtg_ai.h"
#include "core/ggtg_ai_embed.h"
#include "core/ggtg_ai_profile.h"
#include "core/ggtg_sms_stats.h"

// What the AI learns from results, not only from words (see also
// ggtg_ai_profile.h). Three kinds of signals are kept next to the profile
// in tdata/ggtg_db:
//
//  * the answers people gave to a mailing, sorted by their meaning (interest,
//    a question, a refusal, hostility, something else) - sms_replies.json:
//    {"<account id>:<username>":{"kind":"interest","text":"...","at":1791000000}}
//    So "answered" stops being the measure: the share of good answers is.
//  * the conversations the user marked as a success - ai_successes.json:
//    {"items":[{"u":"username","at":1791000000}]}
//  * what the user changed in the assistant's drafts before sending -
//    ai_pairs.json:
//    {"items":[{"draft":"...","sent":"...","at":1791000000,"learned":false}]}
namespace Core::GgtgAi::Feedback {

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

// A few answers of this very mailing that are already sorted, as examples:
// the model sees how this audience writes (and the sorting follows what was
// decided before). At most two of each type, short ones.
[[nodiscard]] inline QString ExamplesNote() {
	const auto replies = LoadReplies();
	auto taken = std::map<QString, int>();
	auto result = QString();
	for (auto i = replies.rbegin(); i != replies.rend(); ++i) {
		const auto &reply = i->second;
		if (reply.kind.isEmpty() || reply.text.isEmpty()
			|| reply.text.size() > 100 || taken[reply.kind] >= 2) {
			continue;
		}
		++taken[reply.kind];
		result += u"«"_q + reply.text.simplified() + u"» - "_q
			+ KindWord(reply.kind) + '\n';
	}
	return result.isEmpty()
		? QString()
		: (u"\nПримеры уже разобранных ответов этой аудитории:\n"_q
			+ result);
}

// Obvious short answers are sorted by words alone: instantly, for free, and
// the same way every time. Anything longer or less clear goes to the model.
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

[[nodiscard]] inline QString ClassifySystem(bool withExamples = true) {
	return u"Ты разбираешь ответы людей на первое сообщение от незнакомого "
		"человека (рассылка в Telegram). На каждую строку вида «N: текст» "
		"ответь строкой «N: тип». Типы:\n"
		"интерес - человеку интересно: просит подробности, соглашается, "
		"благодарит, спрашивает цену или условия;\n"
		"вопрос - уточняет, кто вы, что это и откуда у вас его контакт;\n"
		"отказ - вежливо не интересно («нет, спасибо», «не пишите»);\n"
		"негатив - грубость, жалоба на спам, угроза, ругань;\n"
		"другое - непонятно, шутка, стикер, одно слово без смысла.\n"
		"Если сомневаешься между «интерес» и «вопрос», выбирай «интерес»: "
		"лучше лишний раз заметить живого человека.\n"
		"Ответ только строки «N: тип», по одной на каждую, без пояснений."_q
		+ (withExamples ? ExamplesNote() : QString());
}

[[nodiscard]] inline QString KindByWord(QString word) {
	word = word.toLower().simplified();
	word.remove('*');
	word.remove('.');
	if (word.startsWith(u"интерес"_q)) {
		return u"interest"_q;
	} else if (word.startsWith(u"вопрос"_q)) {
		return u"question"_q;
	} else if (word.startsWith(u"отказ"_q)) {
		return u"refusal"_q;
	} else if (word.startsWith(u"негатив"_q)) {
		return u"negative"_q;
	} else if (word.startsWith(u"друг"_q)) {
		return u"other"_q;
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

// Sorts the given answers (keys) by meaning, twelve in one question to the
// model; what is sorted is saved at once, so a stop in the middle loses
// nothing. `done(sorted, error)`.
inline void ClassifyRepliesByModel(
		std::vector<QString> keys,
		Fn<void(int sorted, QString error)> done) {
	const auto pending = std::make_shared<std::vector<QString>>(
		std::move(keys));
	const auto sorted = std::make_shared<int>(0);
	const auto step = std::make_shared<Fn<void()>>();
	*step = [=] {
		if (pending->empty()) {
			done(*sorted, QString());
			return;
		}
		constexpr auto kBatch = 12;
		const auto take = std::min<int>(kBatch, int(pending->size()));
		const auto batch = std::vector<QString>(
			pending->begin(),
			pending->begin() + take);
		pending->erase(pending->begin(), pending->begin() + take);
		auto replies = LoadReplies();
		// The obvious ones are sorted right here, the rest go to the model.
		auto rest = std::vector<QString>();
		auto quick = 0;
		for (const auto &key : batch) {
			const auto j = replies.find(key);
			const auto kind = (j == replies.end() || !j->second.kind.isEmpty())
				? QString()
				: details::QuickKind(j->second.text);
			if (!kind.isEmpty()) {
				j->second.kind = kind;
				++quick;
			} else {
				rest.push_back(key);
			}
		}
		if (quick) {
			SaveReplies(replies);
			*sorted += quick;
		}
		if (rest.empty()) {
			(*step)();
			return;
		}
		auto material = QString();
		for (auto i = 0; i != int(rest.size()); ++i) {
			const auto j = replies.find(rest[i]);
			material += QString::number(i + 1) + u": "_q
				+ (j == replies.end() ? QString() : j->second.text.left(200))
				+ '\n';
		}
		Core::GgtgAi::Ask(
			details::ClassifySystem(),
			material,
			[=](QString result, QString error) {
				if (!error.isEmpty()) {
					done(*sorted, error);
					return;
				}
				static const auto line = QRegularExpression(
					u"^\\s*(?:№|#)?\\s*(\\d+)\\s*[:.\\-)]\\s*(.+?)\\s*$"_q);
				auto all = LoadReplies();
				for (const auto &row : result.split('\n')) {
					const auto match = line.match(row);
					if (!match.hasMatch()) {
						continue;
					}
					const auto index = match.captured(1).toInt() - 1;
					const auto kind = details::KindByWord(match.captured(2));
					if (index < 0 || index >= int(rest.size())
						|| kind.isEmpty()) {
						continue;
					}
					const auto j = all.find(rest[index]);
					if (j != all.end() && j->second.kind.isEmpty()) {
						j->second.kind = kind;
						++*sorted;
					}
				}
				SaveReplies(all);
				(*step)();
			},
			0.1,
			200);
	};
	(*step)();
}

// Sorts by meaning first: an answer whose five nearest neighbours among the
// already sorted ones agree (at least four of five, and close enough: 0.68 and above, measured:
// the same meaning gives 0.73+, a different one up to 0.53) gets
// their type with no question to the model. The rest, and everything when the
// meaning search is not installed or there are too few sorted answers to
// learn from, go the usual way.
inline void ClassifyReplies(
		std::vector<QString> keys,
		Fn<void(int sorted, QString error)> done) {
	constexpr auto kCorpus = 400;
	constexpr auto kMinCorpus = 20;
	if (!Embed::Available() || keys.empty()) {
		ClassifyRepliesByModel(std::move(keys), done);
		return;
	}
	const auto replies = LoadReplies();
	auto labeled = std::vector<std::pair<qint64, QString>>();
	for (const auto &[key, reply] : replies) {
		if (!reply.kind.isEmpty() && !reply.text.isEmpty()) {
			labeled.push_back({ reply.at, key });
		}
	}
	if (int(labeled.size()) < kMinCorpus) {
		ClassifyRepliesByModel(std::move(keys), done);
		return;
	}
	std::sort(labeled.begin(), labeled.end(), [](auto &a, auto &b) {
		return a.first > b.first;
	});
	if (int(labeled.size()) > kCorpus) {
		labeled.resize(kCorpus);
	}
	auto corpus = QStringList();
	auto kinds = std::vector<QString>();
	for (const auto &[at, key] : labeled) {
		corpus.push_back(replies.at(key).text);
		kinds.push_back(replies.at(key).kind);
	}
	auto texts = corpus;
	for (const auto &key : keys) {
		const auto i = replies.find(key);
		if (i != replies.end()) {
			texts.push_back(i->second.text);
		}
	}
	Embed::Prepare(texts, [=](bool ok) {
		if (!ok) {
			ClassifyRepliesByModel(keys, done);
			return;
		}
		auto all = LoadReplies();
		auto remaining = std::vector<QString>();
		auto assigned = 0;
		for (const auto &key : keys) {
			const auto i = all.find(key);
			if (i == all.end() || !i->second.kind.isEmpty()) {
				continue;
			}
			const auto neighbours = Embed::Nearest(i->second.text, corpus, 5);
			auto votes = std::map<QString, int>();
			for (const auto &[similarity, index] : neighbours) {
				++votes[kinds[index]];
			}
			auto best = QString();
			auto bestVotes = 0;
			for (const auto &[kind, count] : votes) {
				if (count > bestVotes) {
					best = kind;
					bestVotes = count;
				}
			}
			if (neighbours.size() == 5 && neighbours.front().first >= 0.68
				&& bestVotes >= 4) {
				i->second.kind = best;
				++assigned;
			} else {
				remaining.push_back(key);
			}
		}
		if (assigned) {
			SaveReplies(all);
		}
		ClassifyRepliesByModel(
			std::move(remaining),
			[=](int sorted, QString error) {
				done(sorted + assigned, error);
			});
	});
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

// What the program already knows about a person for the prompt: whether they
// answered a mailing, what kind of answer it was, whether the user marked the
// talk a success. Empty when nothing is known.
[[nodiscard]] inline QString PersonNote(const QString &username) {
	const auto reply = ReplyOfUsername(username);
	auto parts = QStringList();
	if (!reply.text.isEmpty()) {
		parts.push_back(u"ответил на рассылку ("_q
			+ details::KindWord(reply.kind) + u"): «"_q
			+ reply.text.simplified().left(160) + u"»"_q);
	}
	if (IsSuccess(username)) {
		parts.push_back(u"вы отметили этот разговор успешным"_q);
	}
	return parts.isEmpty()
		? QString()
		: (u"Что известно о собеседнике: "_q + parts.join(u"; "_q) + '.');
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

// What led to the success: the conversation boiled down to facts for the
// profile ("what works"), the people who answer (the audience), the style.
inline void LearnFromSuccess(
		const QString &conversation,
		Fn<void(Profile::Change change, QString error)> done) {
	Profile::Extract(
		u"Пользователь отметил эту переписку как УСПЕХ: он добился своей "
			"цели с этим собеседником. Тебе дают профиль и переписку («Я» - "
			"пользователь). Выпиши, что в сообщениях и ходе разговора "
			"привело к успеху: какое начало, вопросы, формулировки, длина, "
			"что ответило собеседнику (теги «работает»), и кто этот "
			"собеседник как представитель аудитории (тег «аудитория»). "
			"Выводы только из переписки, без имён и номеров."_q,
		{
			Profile::Section::Works,
			Profile::Section::Audience,
			Profile::Section::Style,
		},
		u"Переписка:\n"_q + conversation,
		std::move(done),
		500);
}

// ---- what the user changed in the drafts ----

constexpr auto kMaxPairs = 30;
constexpr auto kPairsToLearn = 3;

[[nodiscard]] inline QString PairsPath() {
	return details::Dir() + u"ai_pairs.json"_q;
}

struct Pair {
	QString draft;
	QString sent;
	qint64 at = 0;
	bool learned = false;
};

[[nodiscard]] inline std::vector<Pair> LoadPairs() {
	auto result = std::vector<Pair>();
	const auto array = details::ReadObject(PairsPath())
		.value("items").toArray();
	for (const auto &value : array) {
		const auto o = value.toObject();
		result.push_back({
			.draft = o.value("draft").toString(),
			.sent = o.value("sent").toString(),
			.at = qint64(o.value("at").toDouble()),
			.learned = o.value("learned").toBool(),
		});
	}
	return result;
}

inline void SavePairs(const std::vector<Pair> &pairs) {
	auto items = QJsonArray();
	const auto from = std::max(0, int(pairs.size()) - kMaxPairs);
	for (auto i = from; i != int(pairs.size()); ++i) {
		items.push_back(QJsonObject{
			{ "draft", pairs[i].draft },
			{ "sent", pairs[i].sent },
			{ "at", double(pairs[i].at) },
			{ "learned", pairs[i].learned },
		});
	}
	details::WriteObject(PairsPath(), QJsonObject{ { "items", items } });
}

// A draft of the assistant and what the user really sent after it. A draft
// sent as it was says nothing new and is not kept.
inline void RecordEdit(const QString &draft, const QString &sent) {
	const auto a = draft.simplified();
	const auto b = sent.simplified();
	if (a.isEmpty() || b.isEmpty() || a == b || a.size() > 800
		|| b.size() > 800) {
		return;
	}
	auto pairs = LoadPairs();
	pairs.push_back({
		.draft = a,
		.sent = b,
		.at = qint64(QDateTime::currentSecsSinceEpoch()),
	});
	SavePairs(pairs);
}

// When enough new edits have come together, the model compares the drafts
// with what was sent and writes down what the user changes (style, rules).
// Runs only when a key can take the job now: sending a message must not
// wait for a resting one.
inline void MaybeLearnFromEdits(Fn<void(Profile::Change)> done = nullptr) {
	const auto settings = Core::GgtgAi::Load();
	if (!settings.enabled
		|| Core::GgtgAi::EngineLevelNow()
			!= Core::GgtgAi::EngineLevel::Ready) {
		return;
	}
	static auto running = false;
	if (running) {
		return;
	}
	auto pairs = LoadPairs();
	auto fresh = std::vector<int>();
	for (auto i = 0; i != int(pairs.size()); ++i) {
		if (!pairs[i].learned) {
			fresh.push_back(i);
		}
	}
	if (int(fresh.size()) < kPairsToLearn) {
		return;
	}
	if (int(fresh.size()) > 8) {
		fresh.erase(fresh.begin(), fresh.end() - 8);
	}
	auto material = QString();
	for (const auto index : fresh) {
		material += u"Черновик ИИ: "_q + pairs[index].draft
			+ u"\nОтправил пользователь: "_q + pairs[index].sent + u"\n\n"_q;
	}
	running = true;
	Profile::Extract(
		u"Пользователь поправил черновики ИИ-помощника перед отправкой. "
			"По разнице видно, как он пишет на самом деле и что ему не "
			"нравится в ответах ИИ (что убирает, добавляет, как "
			"переформулирует). Тебе дают профиль и пары «черновик → что "
			"отправлено». Выпиши устойчивые привычки, которые повторяются "
			"или очевидны; случайные правки не записывай."_q,
		{ Profile::Section::Style, Profile::Section::Rules },
		material,
		[=](Profile::Change change, QString error) {
			running = false;
			if (!error.isEmpty()) {
				return;
			}
			auto now = LoadPairs();
			for (const auto index : fresh) {
				if (index < int(now.size())) {
					now[index].learned = true;
				}
			}
			SavePairs(now);
			if (done) {
				done(change);
			}
		},
		400);
}

} // namespace Core::GgtgAi::Feedback
