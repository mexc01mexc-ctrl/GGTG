/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and exceptions, see https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "core/ggtg_ai.h"

namespace Core::GgtgAi::Inbox {

// The feed of answers with approval ("Лента ответов"): everyone who answered
// a mailing and is waiting for the user's word is an item here, with the
// stage the AI sees them at and a draft in the user's style. Nothing is sent
// without the user's "Отправить". The store is tdata/ggtg_db/ai_inbox.json;
// the search for the answers and the drafting are in settings/ggtg_ai_inbox.h.

enum class Status {
	Pending, // waits for the user (a draft may be there already)
	Sent,
	Skipped,
	Hot, // ready to buy: the AI is silent, the user is called
};

struct Item {
	QString key; // "<account id>:<username>", lower case
	quint64 senderId = 0; // the user's account that wrote to them
	QString sender; // that account's name
	QString username;
	QString name; // the person's name
	qint64 lastId = 0; // their last message the item is about
	qint64 lastAt = 0;
	QString theirText; // what they wrote last
	QString stage; // question, objection, interest, refusal, hot, other
	QString draft;
	QString warning; // links / numbers in the draft that nothing backs
	Status status = Status::Pending;
	bool drafting = false; // not saved
	qint64 updated = 0;
	// What was sent last and at which stage: when the person writes back,
	// that answer worked (see Settings::AiInbox::Scan).
	QString sentText;
	QString sentStage;
	QString sentTheirs; // what they had written before it
	// The autopilot sends the draft by itself at this moment (ms since the
	// epoch), unless the user cancels it; 0 - not planned.
	qint64 autoAt = 0;
};

// What the feed remembers besides the items.
struct Memory {
	// Drafts of each stage sent without a change, in a row: the base for
	// offering the autopilot for that stage.
	std::map<QString, int> streaks;
	QStringList autopilot; // the stages the AI answers by itself
	QString lastBrief; // the day of the last morning brief, "yyyy-MM-dd"
	QString lastNight; // the day of the last night training
	qint64 lastReview = 0; // "Проверить, что ИИ выучил": seen up to then
};

constexpr auto kAutopilotStreak = 10;
constexpr auto kAutopilotDelay = 3 * 60 * 1000; // ms to cancel a send

constexpr auto kMaxItems = 300;

[[nodiscard]] inline QString Path() {
	return Core::GgtgPaths::DbDir() + u"ai_inbox.json"_q;
}

[[nodiscard]] inline QString StatusId(Status status) {
	switch (status) {
	case Status::Pending: return u"pending"_q;
	case Status::Sent: return u"sent"_q;
	case Status::Skipped: return u"skipped"_q;
	case Status::Hot: return u"hot"_q;
	}
	return u"pending"_q;
}

[[nodiscard]] inline Status StatusFromId(const QString &id) {
	if (id == u"sent"_q) {
		return Status::Sent;
	} else if (id == u"skipped"_q) {
		return Status::Skipped;
	} else if (id == u"hot"_q) {
		return Status::Hot;
	}
	return Status::Pending;
}

[[nodiscard]] inline QString StageTitle(const QString &stage) {
	if (stage == u"question"_q) {
		return u"вопрос"_q;
	} else if (stage == u"objection"_q) {
		return u"возражение"_q;
	} else if (stage == u"interest"_q) {
		return u"интерес"_q;
	} else if (stage == u"refusal"_q) {
		return u"отказ"_q;
	} else if (stage == u"hot"_q) {
		return u"готов купить"_q;
	} else if (stage == u"other"_q) {
		return u"другое"_q;
	}
	return u"определяю..."_q;
}

// The stage by the word the model wrote.
[[nodiscard]] inline QString StageFromWord(QString word) {
	word = word.toLower();
	if (word.contains(u"готов"_q) || word.contains(u"купит"_q)
		|| word.contains(u"горяч"_q)) {
		return u"hot"_q;
	} else if (word.contains(u"отказ"_q) || word.contains(u"негатив"_q)) {
		return u"refusal"_q;
	} else if (word.contains(u"возраж"_q) || word.contains(u"сомнен"_q)) {
		return u"objection"_q;
	} else if (word.contains(u"вопрос"_q)) {
		return u"question"_q;
	} else if (word.contains(u"интерес"_q)) {
		return u"interest"_q;
	}
	return u"other"_q;
}

[[nodiscard]] inline QString KeyOf(quint64 senderId, const QString &username) {
	return QString::number(senderId) + ':' + username.toLower();
}

namespace details {

struct Store {
	std::vector<Item> items;
	Memory memory;
	bool loaded = false;
	int version = 0;
};

[[nodiscard]] inline Store &State() {
	static auto result = Store();
	if (!std::exchange(result.loaded, true)) {
		auto file = QFile(Path());
		if (file.open(QIODevice::ReadOnly)) {
			const auto root = QJsonDocument::fromJson(file.readAll())
				.object();
			const auto memory = root.value("memory").toObject();
			const auto streaks = memory.value("streaks").toObject();
			for (auto i = streaks.begin(); i != streaks.end(); ++i) {
				result.memory.streaks[i.key()] = i.value().toInt();
			}
			for (const auto &stage : memory.value("autopilot").toArray()) {
				result.memory.autopilot.push_back(stage.toString());
			}
			result.memory.lastBrief = memory.value("lastBrief").toString();
			result.memory.lastNight = memory.value("lastNight").toString();
			result.memory.lastReview = qint64(
				memory.value("lastReview").toDouble());
			const auto array = root.value("items").toArray();
			for (const auto &value : array) {
				const auto o = value.toObject();
				auto item = Item{
					.key = o.value("key").toString(),
					.senderId = quint64(o.value("senderId").toString()
						.toULongLong()),
					.sender = o.value("sender").toString(),
					.username = o.value("username").toString(),
					.name = o.value("name").toString(),
					.lastId = qint64(o.value("lastId").toDouble()),
					.lastAt = qint64(o.value("lastAt").toDouble()),
					.theirText = o.value("theirText").toString(),
					.stage = o.value("stage").toString(),
					.draft = o.value("draft").toString(),
					.warning = o.value("warning").toString(),
					.status = StatusFromId(o.value("status").toString()),
					.updated = qint64(o.value("updated").toDouble()),
					.sentText = o.value("sentText").toString(),
					.sentStage = o.value("sentStage").toString(),
					.sentTheirs = o.value("sentTheirs").toString(),
					.autoAt = qint64(o.value("autoAt").toDouble()),
				};
				if (!item.key.isEmpty()) {
					result.items.push_back(std::move(item));
				}
			}
		}
	}
	return result;
}

} // namespace details

[[nodiscard]] inline std::vector<Item> &Items() {
	return details::State().items;
}

[[nodiscard]] inline Memory &Remembered() {
	return details::State().memory;
}

// The stage the autopilot could take over now: drafts of it went out
// unchanged `kAutopilotStreak` times in a row. Empty when none.
[[nodiscard]] inline QString AutopilotOffer() {
	const auto &memory = Remembered();
	for (const auto &[stage, count] : memory.streaks) {
		if (count >= kAutopilotStreak
			&& stage != u"hot"_q
			&& stage != u"refusal"_q
			&& !memory.autopilot.contains(stage)) {
			return stage;
		}
	}
	return QString();
}

// Grows on every change: a window that shows the feed rebuilds on it.
[[nodiscard]] inline int Version() {
	return details::State().version;
}

inline void Save() {
	auto &state = details::State();
	++state.version;
	auto &items = state.items;
	if (int(items.size()) > kMaxItems) {
		// The oldest finished ones go first.
		std::stable_sort(items.begin(), items.end(), [](
				const Item &a,
				const Item &b) {
			const auto open = [](const Item &item) {
				return item.status == Status::Pending
					|| item.status == Status::Hot;
			};
			return (open(a) != open(b)) ? open(a) : (a.updated > b.updated);
		});
		items.resize(kMaxItems);
	}
	auto array = QJsonArray();
	for (const auto &item : items) {
		array.push_back(QJsonObject{
			{ "key", item.key },
			{ "senderId", QString::number(item.senderId) },
			{ "sender", item.sender },
			{ "username", item.username },
			{ "name", item.name },
			{ "lastId", double(item.lastId) },
			{ "lastAt", double(item.lastAt) },
			{ "theirText", item.theirText },
			{ "stage", item.stage },
			{ "draft", item.draft },
			{ "warning", item.warning },
			{ "status", StatusId(item.status) },
			{ "updated", double(item.updated) },
			{ "sentText", item.sentText },
			{ "sentStage", item.sentStage },
			{ "sentTheirs", item.sentTheirs },
			{ "autoAt", double(item.autoAt) },
		});
	}
	auto streaks = QJsonObject();
	for (const auto &[stage, count] : state.memory.streaks) {
		streaks.insert(stage, count);
	}
	QDir().mkpath(Core::GgtgPaths::DbDir());
	auto file = QSaveFile(Path());
	if (file.open(QIODevice::WriteOnly)) {
		file.write(QJsonDocument(QJsonObject{
			{ "items", array },
			{ "memory", QJsonObject{
				{ "streaks", streaks },
				{ "autopilot", QJsonArray::fromStringList(
					state.memory.autopilot) },
				{ "lastBrief", state.memory.lastBrief },
				{ "lastNight", state.memory.lastNight },
				{ "lastReview", double(state.memory.lastReview) },
			} },
		}).toJson(QJsonDocument::Compact));
		file.commit();
	}
}

// Tells a window that shows the feed to rebuild, without saving anything.
inline void Touch() {
	++details::State().version;
}

[[nodiscard]] inline Item *Find(const QString &key) {
	for (auto &item : Items()) {
		if (item.key == key) {
			return &item;
		}
	}
	return nullptr;
}

// How many wait for the user, and how many of them are hot.
struct Counts {
	int pending = 0;
	int hot = 0;
};

[[nodiscard]] inline Counts Count() {
	auto result = Counts();
	for (const auto &item : Items()) {
		if (item.status == Status::Hot) {
			++result.hot;
		} else if (item.status == Status::Pending) {
			++result.pending;
		}
	}
	return result;
}

} // namespace Core::GgtgAi::Inbox
