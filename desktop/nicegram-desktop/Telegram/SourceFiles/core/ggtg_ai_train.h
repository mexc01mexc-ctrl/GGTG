/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and exceptions, see https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "core/ggtg_ai.h"
#include "core/ggtg_ai_knowledge.h"
#include "core/ggtg_ai_profile.h"

#include <array>
#include <set>

namespace Core::GgtgAi::Train {

// Learning from the user's own chats, the part that needs no Telegram: what
// is taken from one conversation, how it is kept and what it becomes. The
// long run itself (the accounts, the chats, pauses) is in
// settings/ggtg_ai_train.h.
//
// From each personal chat where the user wrote, the model takes: how the
// user writes (style), what people ask, what they object to and what the
// user answered, and how the talk ended. The people's names, numbers and
// usernames are replaced by marks before the text leaves the computer, and
// what is kept has no real names in it.
//
// - style and the endings are collected as notes and, every few dozen chats,
//   boiled down by the model into the profile's "Стиль", "Что работает" and
//   "Что не работает";
// - questions and objections with the user's answers go to the playbook
//   (tdata/ggtg_db/ai_playbook.json), which is also a source of the
//   "Материалы" base - so the assistant gets the ones that fit the talk;
// - which chats are done is kept in tdata/ggtg_db/ai_training.json, so a run
//   that was stopped goes on from the same place.

struct Line {
	bool mine = false;
	QString text;
	qint64 date = 0;
};

struct PlaybookEntry {
	QString kind; // "question", "objection" or "message"
	QString what;
	QStringList answers; // up to two
	int count = 1;
	qint64 at = 0;
	// How many times the answer worked: the person wrote back after it, or
	// the talk ended with a deal. The answers that sell go first.
	int wins = 0;
};

// Worked answers count for more than frequent ones.
[[nodiscard]] inline int Weight(const PlaybookEntry &entry) {
	return entry.wins * 3 + entry.count;
}

enum class Ending { Deal, Refusal, Silence, Going };

struct State {
	std::vector<qint64> accounts; // user ids of the accounts to learn from
	std::set<QString> done; // "<account>:<peer>" of the chats already read
	int chats = 0; // chats learned from
	int skipped = 0; // chats with too little of the user's own words
	int messages = 0;
	std::array<int, 4> endings = {};
	QStringList styleNotes; // waiting to be boiled down
	QStringList outcomeNotes;
	qint64 startedAt = 0;
};

namespace details {

constexpr auto kWindow = 6500; // characters sent to the model at once
constexpr auto kMaxWindows = 3; // per chat
constexpr auto kMaxPlaybook = 500;
constexpr auto kNotesBatch = 40; // notes that call for boiling down

[[nodiscard]] inline QString StatePath() {
	return Core::GgtgPaths::DbDir() + u"ai_training.json"_q;
}

[[nodiscard]] inline QString PlaybookPath() {
	return Core::GgtgPaths::DbDir() + u"ai_playbook.json"_q;
}

[[nodiscard]] inline QStringList ToList(const QJsonArray &array) {
	auto result = QStringList();
	for (const auto &value : array) {
		result.push_back(value.toString());
	}
	return result;
}

} // namespace details

[[nodiscard]] inline State &Current() {
	static auto result = State();
	static auto loaded = false;
	if (!std::exchange(loaded, true)) {
		auto file = QFile(details::StatePath());
		if (file.open(QIODevice::ReadOnly)) {
			const auto object = QJsonDocument::fromJson(
				file.readAll()).object();
			for (const auto &value : object.value("accounts").toArray()) {
				result.accounts.push_back(qint64(value.toDouble()));
			}
			for (const auto &key : details::ToList(
					object.value("done").toArray())) {
				result.done.insert(key);
			}
			result.chats = object.value("chats").toInt();
			result.skipped = object.value("skipped").toInt();
			result.messages = object.value("messages").toInt();
			const auto endings = object.value("endings").toArray();
			for (auto i = 0; i != int(result.endings.size()); ++i) {
				result.endings[i] = endings.at(i).toInt();
			}
			result.styleNotes = details::ToList(
				object.value("styleNotes").toArray());
			result.outcomeNotes = details::ToList(
				object.value("outcomeNotes").toArray());
			result.startedAt = qint64(object.value("startedAt").toDouble());
		}
	}
	return result;
}

inline void SaveState() {
	const auto &state = Current();
	auto accounts = QJsonArray();
	for (const auto id : state.accounts) {
		accounts.push_back(double(id));
	}
	auto endings = QJsonArray();
	for (const auto count : state.endings) {
		endings.push_back(count);
	}
	auto file = QSaveFile(details::StatePath());
	if (!file.open(QIODevice::WriteOnly)) {
		return;
	}
	file.write(QJsonDocument(QJsonObject{
		{ "accounts", accounts },
		{ "done", QJsonArray::fromStringList(QStringList(
			state.done.begin(),
			state.done.end())) },
		{ "chats", state.chats },
		{ "skipped", state.skipped },
		{ "messages", state.messages },
		{ "endings", endings },
		{ "styleNotes", QJsonArray::fromStringList(state.styleNotes) },
		{ "outcomeNotes", QJsonArray::fromStringList(state.outcomeNotes) },
		{ "startedAt", double(state.startedAt) },
	}).toJson(QJsonDocument::Compact));
	file.commit();
}

// ---- the playbook: what people ask and object to, and how the user answers ----

[[nodiscard]] inline std::vector<PlaybookEntry> &Playbook() {
	static auto result = std::vector<PlaybookEntry>();
	static auto loaded = false;
	if (!std::exchange(loaded, true)) {
		auto file = QFile(details::PlaybookPath());
		if (file.open(QIODevice::ReadOnly)) {
			const auto array = QJsonDocument::fromJson(
				file.readAll()).object().value("entries").toArray();
			for (const auto &value : array) {
				const auto object = value.toObject();
				auto entry = PlaybookEntry{
					.kind = object.value("kind").toString(),
					.what = object.value("what").toString(),
					.answers = details::ToList(
						object.value("answers").toArray()),
					.count = std::max(object.value("count").toInt(1), 1),
					.at = qint64(object.value("at").toDouble()),
					.wins = object.value("wins").toInt(),
				};
				if (!entry.what.isEmpty()) {
					result.push_back(std::move(entry));
				}
			}
		}
	}
	return result;
}

inline void SavePlaybook() {
	auto array = QJsonArray();
	for (const auto &entry : Playbook()) {
		array.push_back(QJsonObject{
			{ "kind", entry.kind },
			{ "what", entry.what },
			{ "answers", QJsonArray::fromStringList(entry.answers) },
			{ "count", entry.count },
			{ "at", double(entry.at) },
			{ "wins", entry.wins },
		});
	}
	auto file = QSaveFile(details::PlaybookPath());
	if (file.open(QIODevice::WriteOnly)) {
		file.write(QJsonDocument(QJsonObject{
			{ "v", 1 },
			{ "entries", array },
		}).toJson(QJsonDocument::Compact));
		file.commit();
	}
}

// A question or an objection met in a chat. The same one again (by meaning
// of its words) only raises its count and may bring a second way to answer.
inline void AddEntry(PlaybookEntry entry) {
	auto &list = Playbook();
	entry.what = Profile::details::Clean(entry.what);
	for (auto &answer : entry.answers) {
		answer = answer.simplified().left(300);
	}
	entry.answers.removeAll(QString());
	if (entry.what.size() < 6) {
		return;
	}
	entry.at = QDateTime::currentSecsSinceEpoch();
	for (auto &existing : list) {
		if (!Profile::details::Similar(existing.what, entry.what)) {
			continue;
		}
		++existing.count;
		existing.wins += entry.wins;
		existing.at = entry.at;
		for (const auto &answer : entry.answers) {
			auto known = -1;
			for (auto i = 0; i != existing.answers.size(); ++i) {
				if (Profile::details::Similar(existing.answers[i], answer)) {
					known = i;
				}
			}
			if (known < 0 && existing.answers.size() < 2) {
				existing.answers.push_back(answer);
			} else if (known > 0 && entry.wins > 0) {
				// The answer that worked goes first.
				existing.answers.move(known, 0);
			} else if (known < 0 && entry.wins > 0) {
				existing.answers.prepend(answer);
				while (existing.answers.size() > 2) {
					existing.answers.removeLast();
				}
			}
		}
		return;
	}
	list.push_back(std::move(entry));
	if (int(list.size()) > details::kMaxPlaybook) {
		// The rarest and the oldest go first.
		std::stable_sort(list.begin(), list.end(), [](
				const PlaybookEntry &a,
				const PlaybookEntry &b) {
			return (Weight(a) != Weight(b))
				? (Weight(a) > Weight(b))
				: (a.at > b.at);
		});
		list.resize(details::kMaxPlaybook);
	}
}

[[nodiscard]] inline QString EntryChunk(const PlaybookEntry &entry) {
	auto result = (entry.kind == u"objection"_q
		? u"Возражение: "_q
		: entry.kind == u"message"_q
		? u"Собеседник написал: "_q
		: u"Частый вопрос: "_q) + entry.what;
	if (entry.count > 1) {
		result += u" (встречалось %1 раз)"_q.arg(entry.count);
	}
	if (entry.wins > 0) {
		result += u" (этот ответ сработал %1 раз: отвечай так)"_q
			.arg(entry.wins);
	}
	for (auto i = 0; i != entry.answers.size(); ++i) {
		result += (i ? u"\nДругой мой ответ: "_q : u"\nМой ответ: "_q)
			+ entry.answers[i];
	}
	return result;
}

// The playbook as a source of the "Материалы" base, so that the ones that
// fit the talk are put in front of the model with every reply.
inline void SyncKnowledge(Fn<void(QString progress)> progress = nullptr) {
	auto chunks = QStringList();
	auto list = Playbook();
	std::stable_sort(list.begin(), list.end(), [](
			const PlaybookEntry &a,
			const PlaybookEntry &b) {
		return Weight(a) > Weight(b);
	});
	for (const auto &entry : list) {
		if (!entry.answers.isEmpty()) {
			chunks.push_back(EntryChunk(entry));
		}
	}
	Knowledge::SetLearned(
		u"Опыт из ваших чатов: вопросы, возражения и ваши ответы"_q,
		chunks,
		progress ? progress : Fn<void(QString)>([](QString) {}),
		[](QString) {});
}

// ---- one conversation ----

// The words of the names that the chat itself tells (the other side's name
// and username, the user's own) become marks before anything leaves the
// computer: the list of known names (names.json) may not have them.
inline void HideNames(std::vector<Line> &lines, const QStringList &names) {
	static const auto word = QRegularExpression(u"[\\p{L}\\p{N}_]{3,}"_q);
	auto words = QStringList();
	for (const auto &name : names) {
		auto it = word.globalMatch(name);
		while (it.hasNext()) {
			const auto found = it.next().captured(0);
			if (!words.contains(found, Qt::CaseInsensitive)) {
				words.push_back(QRegularExpression::escape(found));
			}
		}
	}
	if (words.isEmpty()) {
		return;
	}
	const auto re = QRegularExpression(
		u"(?<![\\p{L}\\p{N}_])(?:"_q + words.join('|')
			+ u")(?![\\p{L}\\p{N}_])"_q,
		QRegularExpression::CaseInsensitiveOption);
	for (auto &line : lines) {
		line.text.replace(re, u"<N0>"_q);
	}
}

// The conversation as text for the model: newest `kWindow` characters per
// window, at most `kMaxWindows` of them; the beginning (how it started) is
// kept when the talk is longer.
[[nodiscard]] inline QStringList Windows(const std::vector<Line> &lines) {
	auto pieces = QStringList();
	for (const auto &line : lines) {
		auto text = line.text.simplified();
		if (text.isEmpty()) {
			continue;
		}
		if (text.size() > 400) {
			text = text.left(400) + QChar(0x2026);
		}
		pieces.push_back(
			'['
			+ QDateTime::fromSecsSinceEpoch(line.date).toString(
				u"dd.MM.yy HH:mm"_q)
			+ u"] "_q + (line.mine ? u"Я"_q : u"Собеседник"_q) + u": "_q
			+ text);
	}
	auto windows = QStringList();
	auto current = QString();
	for (const auto &piece : pieces) {
		if (!current.isEmpty()
			&& current.size() + piece.size() > details::kWindow) {
			windows.push_back(current);
			current.clear();
		}
		current += piece + '\n';
	}
	if (!current.isEmpty()) {
		windows.push_back(current);
	}
	while (windows.size() > details::kMaxWindows) {
		windows.removeAt(1); // the first and the newest stay
	}
	return windows;
}

struct Insight {
	QStringList styles;
	std::vector<PlaybookEntry> entries;
	std::optional<Ending> ending;
	QString lesson;
};

[[nodiscard]] inline QString ExtractionSystem() {
	return u"Ты изучаешь личную переписку пользователя («Я») в Telegram с "
		"одним собеседником, чтобы помощник отвечал так же, как он. Имена, "
		"номера и ники заменены метками вроде <N1>: так и задумано, не "
		"расшифровывай их и не пиши в ответе.\n"
		"Выведи строки ТОЛЬКО в этих форматах, по одной на строку, без "
		"вступлений и нумерации:\n"
		"СТИЛЬ: <одно наблюдение о том, как пишет «Я»: тон, «ты» или «вы», "
		"длина, эмодзи, приветствия, обороты>\n"
		"ВОПРОС: <вопрос собеседника, сформулированный обобщённо> => <как "
		"ответил «Я», близко к его словам>\n"
		"ВОЗРАЖЕНИЕ: <сомнение или отказ собеседника, сформулированный "
		"обобщённо> => <как ответил «Я», близко к его словам>\n"
		"ИТОГ: <договорились | отказ | молчание | продолжается> | <что в "
		"этом диалоге сработало или не сработало, одной фразой>\n"
		"Правила: вопросы и возражения пиши так, чтобы они подошли и другим "
		"людям: без имён, номеров и личных подробностей собеседника. "
		"Вопрос или возражение берёшь только если «Я» на него ответил. "
		"Ничего не выдумывай, только из переписки. Не больше 3 строк СТИЛЬ, "
		"не больше 6 строк ВОПРОС и ВОЗРАЖЕНИЕ вместе, одна строка ИТОГ. "
		"Если выводов нет, ответь одним словом: НЕТ."_q;
}

inline void Parse(const QString &text, Insight &insight) {
	static const auto head = QRegularExpression(
		u"^\\s*(?:[-*•]|\\d+[.)])?\\s*\\**\\s*"
		"(СТИЛЬ|ВОПРОС|ВОЗРАЖЕНИЕ|ИТОГ)\\s*\\**\\s*:\\s*(.+?)\\s*$"_q,
		QRegularExpression::CaseInsensitiveOption);
	for (const auto &row : text.split(QChar('\n'))) {
		const auto match = head.match(row);
		if (!match.hasMatch()) {
			continue;
		}
		const auto tag = match.captured(1).toUpper();
		const auto body = match.captured(2).trimmed();
		if (tag == u"СТИЛЬ"_q) {
			if (insight.styles.size() < 3 && body.size() >= 8) {
				insight.styles.push_back(body);
			}
		} else if (tag == u"ИТОГ"_q) {
			const auto parts = body.split(QChar('|'));
			const auto word = parts.front().toLower();
			insight.ending = word.contains(u"договор"_q)
				? Ending::Deal
				: word.contains(u"отказ"_q)
				? Ending::Refusal
				: word.contains(u"молч"_q)
				? Ending::Silence
				: Ending::Going;
			insight.lesson = (parts.size() > 1)
				? parts.mid(1).join(u" "_q).trimmed()
				: QString();
		} else {
			auto cut = body.indexOf(u"=>"_q);
			auto skip = 2;
			if (cut < 0) {
				cut = body.indexOf(QChar(0x2192));
				skip = 1;
			}
			if (cut < 0) {
				continue;
			}
			const auto what = body.left(cut).trimmed();
			const auto answer = body.mid(cut + skip).trimmed();
			if (what.size() < 6 || answer.size() < 2 || insight.entries.size() >= 8) {
				continue;
			}
			insight.entries.push_back({
				.kind = (tag == u"ВОПРОС"_q) ? u"question"_q : u"objection"_q,
				.what = what,
				.answers = QStringList{ answer },
			});
		}
	}
}

// Reads the windows of one conversation with the model, one after another.
// `done(insight, error)`: an error is the model's (a limit that has not come
// back, no key); the caller decides whether to wait.
inline void Learn(
		QStringList windows,
		Fn<void(Insight insight, QString error)> done) {
	const auto result = std::make_shared<Insight>();
	const auto index = std::make_shared<int>(0);
	const auto step = std::make_shared<Fn<void()>>();
	*step = [=] {
		if (*index == windows.size()) {
			done(*result, QString());
			return;
		}
		Ask(
			ExtractionSystem(),
			windows[*index],
			[=](QString text, QString error) {
				if (!error.isEmpty()) {
					done(Insight(), error);
					return;
				}
				Parse(text, *result);
				++*index;
				(*step)();
			},
			0.2,
			1200,
			Role::General,
			QStringList(),
			false, // a bulk job: names and numbers leave as marks
			true); // and the marks do not come back into what is kept
	};
	(*step)();
}

// Puts what was taken from a conversation where it belongs.
inline void Keep(const Insight &insight, int messages) {
	auto &state = Current();
	++state.chats;
	state.messages += messages;
	for (const auto &style : insight.styles) {
		state.styleNotes.push_back(style.left(240));
	}
	for (auto entry : insight.entries) {
		// A talk that ended with a deal: its answers worked.
		if (insight.ending == Ending::Deal) {
			entry.wins = 1;
		}
		AddEntry(std::move(entry));
	}
	if (insight.ending) {
		++state.endings[int(*insight.ending)];
		if (!insight.lesson.isEmpty()) {
			static const auto words = std::array<QString, 4>{
				u"договорились"_q, u"отказ"_q, u"молчание"_q,
				u"продолжается"_q,
			};
			state.outcomeNotes.push_back(
				words[int(*insight.ending)] + u": "_q
				+ insight.lesson.left(240));
		}
	}
	while (state.styleNotes.size() > 400) {
		state.styleNotes.removeFirst();
	}
	while (state.outcomeNotes.size() > 400) {
		state.outcomeNotes.removeFirst();
	}
	SavePlaybook();
	SaveState();
}

// An answer of the user's that worked in the feed of answers: the person
// wrote back after it (or turned out ready to buy). `what` must have no real
// names in it already.
inline void AddWin(
		const QString &kind,
		const QString &what,
		const QString &answer,
		int wins = 1) {
	AddEntry({
		.kind = kind,
		.what = what,
		.answers = QStringList{ answer },
		.wins = wins,
	});
	SavePlaybook();
	SyncKnowledge();
}

// The entries learned after `since` (seconds), for "Проверить, что ИИ
// выучил".
[[nodiscard]] inline std::vector<PlaybookEntry> LearnedSince(qint64 since) {
	auto result = std::vector<PlaybookEntry>();
	for (const auto &entry : Playbook()) {
		if (entry.at > since) {
			result.push_back(entry);
		}
	}
	return result;
}

inline void RemoveEntry(const QString &what) {
	auto &list = Playbook();
	list.erase(
		std::remove_if(list.begin(), list.end(), [&](const auto &entry) {
			return entry.what == what;
		}),
		list.end());
	SavePlaybook();
	SyncKnowledge();
}

// ---- boiling the notes down into the profile ----

[[nodiscard]] inline bool NeedsConsolidation() {
	const auto &state = Current();
	return state.styleNotes.size() >= details::kNotesBatch
		|| state.outcomeNotes.size() >= details::kNotesBatch;
}

[[nodiscard]] inline bool HasNotes() {
	const auto &state = Current();
	return !state.styleNotes.isEmpty() || !state.outcomeNotes.isEmpty();
}

// The notes of many chats become a few rules in the profile's "Стиль",
// "Что работает" and "Что не работает"; the notes are cleared after.
inline void Consolidate(Fn<void(QString error)> done) {
	const auto style = Current().styleNotes;
	const auto outcomes = Current().outcomeNotes;
	const auto finishOutcomes = [=] {
		if (outcomes.size() < 3) {
			done(QString());
			return;
		}
		Ask(
			u"Ниже итоги многих личных диалогов пользователя: чем закончился "
			"диалог и что в нём сработало или нет. Сделай до 8 выводов, "
			"каждый одной строкой в формате «работает: ...» или «не "
			"работает: ...». Бери только то, что подтверждается несколькими "
			"диалогами. Без имён и вступлений."_q,
			outcomes.join(QChar('\n')),
			[=](QString result, QString error) {
				if (!error.isEmpty()) {
					done(error);
					return;
				}
				auto data = Profile::Read();
				const auto now = qint64(QDateTime::currentSecsSinceEpoch());
				for (auto row : result.split(QChar('\n'))) {
					row = row.trimmed();
					const auto colon = row.indexOf(QChar(':'));
					if (colon < 0) {
						continue;
					}
					const auto section = Profile::details::SectionByTag(
						row.left(colon));
					if (section == Profile::Section::Works
						|| section == Profile::Section::NotWorks) {
						Profile::Add(data, *section, row.mid(colon + 1), now);
					}
				}
				Profile::Write(data);
				Current().outcomeNotes.clear();
				SaveState();
				done(QString());
			},
			0.2,
			900,
			Role::General,
			QStringList(),
			true);
	};
	if (style.size() < 3) {
		finishOutcomes();
		return;
	}
	Ask(
		u"Ниже наблюдения о манере письма пользователя («Я»), собранные по "
		"многим его перепискам. Объедини их в 6-10 коротких правил стиля, "
		"по одному в строке, каждая начинается с «- »: тон, «ты» или «вы», "
		"длина сообщений, эмодзи, приветствия, характерные обороты. Бери "
		"только то, что повторяется в нескольких наблюдениях. Без имён и "
		"вступлений."_q,
		style.join(QChar('\n')),
		[=](QString result, QString error) {
			if (!error.isEmpty()) {
				done(error);
				return;
			}
			auto data = Profile::Read();
			const auto now = qint64(QDateTime::currentSecsSinceEpoch());
			for (const auto &row : result.split(QChar('\n'))) {
				const auto text = row.trimmed();
				if (text.startsWith(QChar('-')) || text.startsWith(QChar('*'))
					|| text.startsWith(QChar(0x2022))) {
					Profile::Add(data, Profile::Section::Style, text, now);
				}
			}
			Profile::Write(data);
			Current().styleNotes.clear();
			SaveState();
			finishOutcomes();
		},
		0.2,
		900,
		Role::General,
		QStringList(),
		true);
}

// ---- starting over, forgetting ----

// The chats count as unread again (what was learned stays).
inline void StartOver() {
	auto &state = Current();
	state.done.clear();
	state.chats = 0;
	state.skipped = 0;
	state.messages = 0;
	state.endings = {};
	state.startedAt = 0;
	SaveState();
}

// Everything the training collected: the playbook, the notes, the progress.
inline void Forget() {
	Playbook().clear();
	SavePlaybook();
	auto &state = Current();
	state.styleNotes.clear();
	state.outcomeNotes.clear();
	StartOver();
	Knowledge::SetLearned(
		QString(),
		QStringList(),
		[](QString) {},
		[](QString) {});
}

[[nodiscard]] inline QString Summary() {
	const auto &state = Current();
	if (!state.chats && !state.skipped && Playbook().empty()) {
		return u"Ещё не запускалось."_q;
	}
	auto result = u"Прочитано чатов: %1 (пропущено без ваших слов: %2), "
		"сообщений: %3."_q
		.arg(state.chats)
		.arg(state.skipped)
		.arg(state.messages);
	result += u" Концовки: договорились %1, отказ %2, молчание %3, "
		"продолжается %4."_q
		.arg(state.endings[0])
		.arg(state.endings[1])
		.arg(state.endings[2])
		.arg(state.endings[3]);
	result += u" Вопросов и возражений с вашими ответами: %1."_q
		.arg(int(Playbook().size()));
	return result;
}

} // namespace Core::GgtgAi::Train
