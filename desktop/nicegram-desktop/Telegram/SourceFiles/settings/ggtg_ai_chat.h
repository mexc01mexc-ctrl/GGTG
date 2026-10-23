/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and exceptions, see https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "core/ggtg_ai.h"
#include "data/scraped_members_db.h"
#include "core/ggtg_ai_feedback.h"
#include "core/ggtg_ai_profile.h"
#include "data/data_peer.h"
#include "data/data_channel.h"
#include "data/data_chat.h"
#include "data/data_user.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/view/history_view_element.h"
#include "main/main_session.h"
#include "settings/ggtg_ai_translate.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_widgets.h"
#include "styles/style_window_main_menu.h" // ggtgTokenButton

namespace Settings {

// The "ИИ" button of a chat's input field. It reads the conversation (every
// chat remembers a short summary of what was said long ago, so only the
// fresh messages are read again; in a group just the last 15 messages) and
// offers a few replies. Nothing is ever sent by itself: a reply goes into the
// input field only when the user presses "Вставить", and is theirs to edit.
// What the assistant is told about the user and the way they talk comes from
// their own prompt ("ИИ" section / the right-click menu of the button).
enum class AiChatMode {
	Suggest, // what to answer
	Summary, // what the conversation is about
	Intent, // what the other side wants
	ReplyLast, // an answer to the last message only
	Polish, // fix and improve what the user has typed
	Write, // write one reply straight into the input field
};

namespace AiChat {

struct Line {
	bool mine = false;
	QString who;
	QString text;
	MsgId id = 0;
	TimeId date = 0; // when the message was sent
};

constexpr auto kLineLength = 400; // one message in the prompt, at most
constexpr auto kTranscriptLength = 4500; // all the raw messages, at most
constexpr auto kRawTail = 20; // private chat: how many stay unsummarised
constexpr auto kGroupLines = 15;
constexpr auto kLoadedLines = 200;
// What may go into the assistant's instructions (characters). The small
// models take 8K tokens in all - a Russian text is about two characters per
// token - and the conversation itself needs its share, so every part has its
// own limit instead of the sum growing without a bound.
constexpr auto kPromptLimit = 2000; // the user's own description
constexpr auto kProfileLimit = 3000; // what the profile says about them
constexpr auto kMaterialsLimit = 3500;

[[nodiscard]] inline QString MemoryPath() {
	return Core::GgtgPaths::DbDir() + u"ai_chat_memory.json"_q;
}

[[nodiscard]] inline QJsonObject LoadMemory() {
	auto file = QFile(MemoryPath());
	if (!file.open(QIODevice::ReadOnly)) {
		return QJsonObject();
	}
	return QJsonDocument::fromJson(file.readAll()).object();
}

inline void SaveMemory(const QJsonObject &memory) {
	auto file = QSaveFile(MemoryPath());
	if (file.open(QIODevice::WriteOnly)) {
		file.write(QJsonDocument(memory).toJson(QJsonDocument::Compact));
		file.commit();
	}
}

// One entry per (account, conversation): the same person writing to two of
// the user's accounts is two different conversations.
[[nodiscard]] inline QString PeerKey(not_null<History*> history) {
	return QString::number(history->session().userId().bare)
		+ ':'
		+ QString::number(history->peer->id.value);
}

inline void ForgetChat(not_null<History*> history) {
	auto memory = LoadMemory();
	memory.remove(PeerKey(history));
	SaveMemory(memory);
}

// The loaded messages of the chat, oldest first; at most `limit` of them.
// A person's name for the prompt: letters only (no emoji from the profile
// name), and "Собеседник" when the name is clearly not a name.
[[nodiscard]] inline QString DisplayName(const QString &raw) {
	const auto name = Core::GgtgAi::CleanPersonName(raw);
	return name.isEmpty() ? u"Собеседник"_q : name;
}

[[nodiscard]] inline std::vector<Line> CollectLines(
		not_null<History*> history,
		int limit) {
	auto all = std::vector<Line>();
	for (const auto &block : history->blocks) {
		for (const auto &view : block->messages) {
			const auto item = view->data();
			if (item->isService() || item->isEmpty()) {
				continue;
			}
			auto text = item->originalText().text.trimmed();
			if (text.isEmpty()) {
				text = u"[вложение]"_q;
			}
			if (text.size() > kLineLength) {
				text = text.left(kLineLength) + u"…"_q;
			}
			all.push_back({
				.mine = item->out(),
				.who = item->out()
					? u"Я"_q
					: DisplayName(item->from()->name()),
				.text = text,
				.id = item->id,
				.date = item->date(),
			});
		}
	}
	if (int(all.size()) > limit) {
		all.erase(all.begin(), all.end() - limit);
	}
	return all;
}

// "6 окт 14:05": the day and the time of a message, local.
[[nodiscard]] inline QString StampText(TimeId date) {
	if (!date) {
		return QString();
	}
	static const auto months = QStringList{
		u"янв"_q, u"фев"_q, u"мар"_q, u"апр"_q, u"мая"_q, u"июн"_q,
		u"июл"_q, u"авг"_q, u"сен"_q, u"окт"_q, u"ноя"_q, u"дек"_q,
	};
	const auto time = QDateTime::fromSecsSinceEpoch(date);
	return QString::number(time.date().day()) + ' '
		+ months[time.date().month() - 1] + ' '
		+ (time.date().year() != QDate::currentDate().year()
			? (QString::number(time.date().year()) + ' ')
			: QString())
		+ time.toString(u"HH:mm"_q);
}

// How long ago, in words: "3 часа", "5 дней", "2 месяца".
[[nodiscard]] inline QString AgoText(TimeId date) {
	const auto seconds = std::max<qint64>(
		0,
		QDateTime::currentSecsSinceEpoch() - date);
	const auto plural = [](qint64 n, const QString &one, const QString &few, const QString &many) {
		const auto mod100 = n % 100;
		const auto mod10 = n % 10;
		return QString::number(n) + ' '
			+ ((mod100 >= 11 && mod100 <= 14)
				? many
				: (mod10 == 1)
				? one
				: (mod10 >= 2 && mod10 <= 4)
				? few
				: many);
	};
	if (seconds < 90) {
		return u"только что"_q;
	} else if (seconds < 3600) {
		return plural(seconds / 60, u"минуту"_q, u"минуты"_q, u"минут"_q);
	} else if (seconds < 86400) {
		return plural(seconds / 3600, u"час"_q, u"часа"_q, u"часов"_q);
	} else if (seconds < 86400 * 60) {
		return plural(seconds / 86400, u"день"_q, u"дня"_q, u"дней"_q);
	} else if (seconds < 86400 * 730) {
		return plural(seconds / (86400 * 30), u"месяц"_q, u"месяца"_q, u"месяцев"_q);
	}
	return plural(seconds / (86400 * 365), u"год"_q, u"года"_q, u"лет"_q);
}

// The messages with the time of each: "[6 окт 14:05] Я: привет".
[[nodiscard]] inline QString Transcript(const std::vector<Line> &lines) {
	auto result = QString();
	for (const auto &line : lines) {
		const auto stamp = StampText(line.date);
		result += (stamp.isEmpty() ? QString() : ('[' + stamp + u"] "_q))
			+ line.who + u": "_q + line.text + '\n';
	}
	return result;
}

// What the assistant is told about time: now, when the last message was and
// how long ago - so that an old conversation is not continued as if it was
// still going on.
[[nodiscard]] inline QString TimeContext(const std::vector<Line> &lines) {
	auto result = u"Сейчас: "_q
		+ StampText(QDateTime::currentSecsSinceEpoch()) + u".\n"_q;
	if (!lines.empty() && lines.back().date) {
		const auto &last = lines.back();
		result += u"Последнее сообщение в переписке: "_q
			+ StampText(last.date) + u" ("_q
			+ (last.mine ? u"моё"_q : u"собеседника"_q) + u"), прошло: "_q
			+ AgoText(last.date) + u".\n"_q;
		const auto gap = QDateTime::currentSecsSinceEpoch() - last.date;
		if (gap > 86400 * 2) {
			result += u"ВАЖНО: разговор давно остановился. Ответ ДОЛЖЕН "
				"начинаться с приветствия и показывать, что вы давно не "
				"общались (без упрёков и без слов вроде «давно не "
				"писал» в обвинительном тоне), затем мягко вернуть к теме "
				"или спросить, актуально ли это и как дела. НЕ реагируй на "
				"последнее сообщение так, будто оно только что пришло.\n"_q;
		} else if (gap > 86400) {
			result += u"ВАЖНО: с последнего сообщения прошли сутки или "
				"больше: начни с короткого приветствия и верни "
				"разговор к теме.\n"_q;
		}
	}
	return result;
}

// The newest messages that fit the prompt.
[[nodiscard]] inline std::vector<Line> FitTail(std::vector<Line> lines) {
	auto size = 0;
	auto from = int(lines.size());
	while (from > 0) {
		const auto add = int(lines[from - 1].who.size()
			+ lines[from - 1].text.size()) + 3;
		if (size + add > kTranscriptLength) {
			break;
		}
		size += add;
		--from;
	}
	return std::vector<Line>(lines.begin() + from, lines.end());
}

// "The dossier" of a conversation: older messages boiled down to a few
// sentences, chunk by chunk, each chunk folded into the summary so far.
inline void Summarize(
		QString summary,
		std::vector<Line> lines,
		Fn<void(QString status)> progress,
		Fn<void(QString summary, QString error)> done) {
	auto chunks = std::make_shared<std::vector<QString>>();
	auto current = QString();
	for (const auto &line : lines) {
		const auto stamp = StampText(line.date);
		const auto piece = (stamp.isEmpty()
				? QString()
				: ('[' + stamp + u"] "_q))
			+ line.who + u": "_q + line.text + '\n';
		if (!current.isEmpty()
			&& current.size() + piece.size() > 3800) {
			chunks->push_back(current);
			current.clear();
		}
		current += piece;
	}
	if (!current.isEmpty()) {
		chunks->push_back(current);
	}
	// Only the newest stretch is worth reading: a few chunks.
	while (chunks->size() > 6) {
		chunks->erase(chunks->begin());
	}
	const auto total = int(chunks->size());
	const auto index = std::make_shared<int>(0);
	const auto running = std::make_shared<QString>(summary);
	const auto step = std::make_shared<Fn<void()>>();
	*step = [=] {
		if (*index >= total) {
			done(*running, QString());
			return;
		}
		progress(u"Читаю переписку: часть %1 из %2..."_q
			.arg(*index + 1)
			.arg(total));
		Core::GgtgAi::Ask(
			u"Ты ведёшь краткое досье переписки двух людей (\"Я\" - "
			"пользователь). Тебе дают прежнюю сводку и новый фрагмент. "
			"Верни обновлённую сводку: до 8 коротких предложений - кто "
			"собеседник и что о нём известно, чего он хочет, о чём "
			"договорились, что осталось открытым, тон общения. Только факты "
			"из текста, ничего не выдумывай. Без вступлений."_q,
			u"Прежняя сводка:\n"_q
				+ (running->isEmpty() ? u"(пока нет)"_q : *running)
				+ u"\n\nНовый фрагмент:\n"_q + (*chunks)[*index],
			[=](QString result, QString error) {
				if (!error.isEmpty()) {
					done(*running, error);
					return;
				}
				*running = result;
				++*index;
				(*step)();
			},
			0.3,
			500);
	};
	(*step)();
}

struct Context {
	QString summary;
	std::vector<Line> tail;
	bool group = false;
};

// Reads the conversation: a group - its last messages; a private chat - the
// remembered summary plus whatever came after it (the first time: everything
// that is loaded, boiled down).
inline void PrepareContext(
		not_null<History*> history,
		Fn<void(QString status)> progress,
		Fn<void(Context context, QString error)> done) {
	const auto peer = history->peer;
	if (!peer->isUser()) {
		auto context = Context();
		context.group = true;
		context.tail = CollectLines(history, kGroupLines);
		done(std::move(context), QString());
		return;
	}
	const auto all = CollectLines(history, kLoadedLines);
	Core::GgtgAi::PurgeChatMemory(); // forgets what nobody used for long
	const auto memory = LoadMemory();
	const auto key = PeerKey(history);
	const auto saved = memory.value(key).toObject();
	const auto savedSummary = saved.value("summary").toString();
	const auto savedId = MsgId(saved.value("lastId").toVariant().toLongLong());

	// The raw tail stays as it is; everything older than it must be inside
	// the summary.
	auto raw = FitTail(std::vector<Line>(
		all.size() > size_t(kRawTail) ? all.end() - kRawTail : all.begin(),
		all.end()));
	const auto rawStart = all.size() - raw.size();
	auto fresh = std::vector<Line>();
	for (auto i = size_t(0); i != rawStart; ++i) {
		if (!savedSummary.isEmpty() && all[i].id <= savedId) {
			continue; // Already in the summary.
		}
		fresh.push_back(all[i]);
	}
	if (fresh.empty()) {
		if (!savedSummary.isEmpty()) {
			Core::GgtgAi::TouchChatMemory(key);
		}
		auto context = Context();
		context.summary = savedSummary;
		context.tail = std::move(raw);
		done(std::move(context), QString());
		return;
	}
	const auto lastSummarized = fresh.back().id;
	Summarize(
		savedSummary,
		std::move(fresh),
		progress,
		[=](QString summary, QString error) {
			if (!error.isEmpty()) {
				done(Context(), error);
				return;
			}
			auto updated = LoadMemory();
			updated.insert(key, QJsonObject{
				{ "summary", summary },
				{ "lastId", double(lastSummarized.bare) },
				{ "usedAt", double(QDateTime::currentSecsSinceEpoch()) },
			});
			SaveMemory(updated);
			auto context = Context();
			context.summary = summary;
			context.tail = raw;
			done(std::move(context), QString());
		});
}

// The name of the user's own account, so the assistant signs with it
// instead of leaving a "[Имя]" placeholder.
[[nodiscard]] inline QString MyNameLine(not_null<History*> history) {
	const auto me = history->session().user();
	auto name = me->firstName.trimmed();
	if (name.isEmpty()) {
		name = me->name().trimmed();
	}
	return name.isEmpty()
		? QString()
		: (u"Моё имя (имя аккаунта пользователя): "_q + name + '\n');
}

// Which language the reply must be in. The prompts are written in Russian,
// the conversation may be in any language: it is judged by the letters of the
// latest messages of the other side (a Latin text is left to the model - it
// can be English, Spanish, Turkish...), and the reply goes out in that
// language.
[[nodiscard]] inline QString LanguageNote(
		const Context &context,
		const QString &draft,
		const QString &peerName = QString()) {
	auto theirs = QString();
	auto mine = QString();
	for (const auto &line : context.tail) {
		(line.mine ? mine : theirs) += line.text + ' ';
	}
	// The other side's own words decide. While they have not written, the
	// script of their name is the best hint (a Chinese name - Chinese): what
	// the user wrote to them so far may be in the user's own language.
	auto code = QString();
	if (!theirs.trimmed().isEmpty()) {
		code = Core::GgtgAi::DetectLanguage(theirs.right(1500));
	}
	auto fromName = false;
	if (code.isEmpty() && !peerName.trimmed().isEmpty()) {
		const auto byName = Core::GgtgAi::DetectLanguage(peerName);
		if (!byName.isEmpty() && byName != u"en"_q) {
			code = byName;
			fromName = true;
		}
	}
	if (code.isEmpty()) {
		const auto sample = mine.trimmed().isEmpty() ? draft : mine;
		code = Core::GgtgAi::DetectLanguage(sample.right(1500));
	}
	if (code.isEmpty()) {
		return QString();
	}
	if (code == u"en"_q) {
		// Latin script: English, Spanish, Turkish... - told by the common
		// words; the reply must be in that language, never in the language
		// of these instructions.
		code = Core::GgtgAi::DetectLatinLanguage(
			theirs.trimmed().isEmpty() ? (mine + ' ' + draft) : theirs);
	}
	return u"Язык переписки: "_q
		+ Core::GgtgAi::LanguageByCode(code).russian
		+ (fromName
			? u" (собеседник ещё не писал - язык выбран по его имени)"_q
			: QString())
		+ u". Пиши ответ ТОЛЬКО на этом языке, даже если эти инструкции "
			"написаны по-русски: отвечать по-русски собеседнику, который "
			"пишет на другом языке, нельзя. Имя собеседника пиши так, "
			"как оно записано в его профиле, не переводя и не "
			"транслитерируя. В имени собеседника никаких смайликов и символов: если имя в профиле с "
			"эмодзи - используй только буквы, а если оно явно ненастоящее "
			"(одна буква, «qqqq», набор клавиш) - обращайся «друг» на языке "
			"переписки или без имени. Если нужно назвать себя - используй имя "
			"из строки «Моё имя» и пиши его в этой же письменности "
			"языка переписки. Не оставляй в тексте заготовок в квадратных "
			"скобках вроде [Имя]: вместо них подставляй настоящее имя, а "
			"если чего-то не знаешь - просто опусти.\n"_q;
}

// "Научиться": the assistant reads the whole loaded conversation and learns
// how the user ("Я") writes - tone, address, length, greetings, how they lead
// the talk to the goal, what the other side answers willingly - and keeps it
// as rules (no names: they differ between accounts and conversations), plus a
// few of the user's own messages as examples of their style.
inline void LearnFromChat(
		not_null<History*> history,
		Fn<void(QString status)> progress,
		Fn<void(QString error)> done) {
	const auto lines = CollectLines(history, 400);
	auto mineCount = 0;
	for (const auto &line : lines) {
		mineCount += line.mine ? 1 : 0;
	}
	if (mineCount < 2) {
		done(u"в этой переписке почти нет ваших сообщений - учиться не на "
			"чем"_q);
		return;
	}
	// Names stay out of it: the user is "Я", the other side "Собеседник".
	auto chunks = std::make_shared<std::vector<QString>>();
	auto current = QString();
	for (auto i = size_t(0); i != lines.size(); ++i) {
		const auto &line = lines[i];
		const auto stamp = StampText(line.date);
		// What came of the user's message: did the other side answer, how
		// fast, how much. Only the last message of a run is marked.
		auto outcome = QString();
		if (line.mine
			&& (i + 1 == lines.size() || !lines[i + 1].mine)) {
			if (i + 1 == lines.size()) {
				outcome = u"  <<ответа нет>>"_q;
			} else {
				const auto &reply = lines[i + 1];
				const auto seconds = std::max<qint64>(
					0,
					qint64(reply.date) - qint64(line.date));
				const auto after = (seconds < 90)
					? u"сразу"_q
					: (seconds < 3600)
					? (u"через %1 мин"_q.arg(seconds / 60))
					: (seconds < 86400)
					? (u"через %1 ч"_q.arg(seconds / 3600))
					: (u"через %1 дн"_q.arg(seconds / 86400));
				outcome = u"  <<ответили %1, ответ: %2 симв.>>"_q
					.arg(after)
					.arg(reply.text.size());
			}
		}
		const auto piece = (stamp.isEmpty()
				? QString()
				: ('[' + stamp + u"] "_q))
			+ (line.mine ? u"Я"_q : u"Собеседник"_q) + u": "_q + line.text
			+ outcome + '\n';
		if (!current.isEmpty() && current.size() + piece.size() > 3600) {
			chunks->push_back(current);
			current.clear();
		}
		current += piece;
	}
	if (!current.isEmpty()) {
		chunks->push_back(current);
	}
	// A very long conversation: the newest part is the most telling.
	while (chunks->size() > 6) {
		chunks->erase(chunks->begin());
	}
	// Examples of the real style: the user's latest meaningful messages.
	auto examples = 0;
	for (auto i = int(lines.size()); i != 0 && examples < 6; --i) {
		const auto &line = lines[i - 1];
		if (line.mine && line.text.size() >= 20 && line.text.size() <= 300
			&& line.text != u"[вложение]"_q) {
			Core::GgtgAi::AddStyleExample(line.text);
			++examples;
		}
	}
	const auto shortChat = (lines.size() < 14) || (mineCount < 5);
	const auto index = std::make_shared<int>(0);
	const auto step = std::make_shared<Fn<void()>>();
	*step = [=] {
		if (*index == int(chunks->size())) {
			done(QString());
			return;
		}
		progress(u"Читаю переписку: часть %1 из %2..."_q
			.arg(*index + 1)
			.arg(chunks->size()));
		const auto fragment = (shortChat
			? u"ВНИМАНИЕ: переписка короткая, мало данных для выводов о "
				"том, что работает. Запиши ТОЛЬКО факты о стиле "
				"(«+ стиль: ...»), остальное не заполняй.\n\n"_q
			: QString())
			+ u"Переписка:\n"_q + (*chunks)[*index];
		Core::GgtgAi::Profile::Extract(
			u"Ты изучаешь переписку, чтобы помощник писал в Telegram точно "
			"как пользователь («Я») и ещё лучше: с результатом. Тебе дают "
			"профиль и фрагмент переписки. После сообщений пользователя "
			"стоят пометки <<ответили сразу / через ... / ответа нет>> и "
			"длина ответа: по ним видно, что сработало. «стиль» - как "
			"пишет пользователь (тон, «ты» или «вы», длина, эмодзи, "
			"приветствия, обороты); «работает» - что вызывало быстрый и "
			"развёрнутый ответ или тёплую реакцию (вопросы, формулировки, "
			"длина, время, как начинал разговор); «не работает» - после "
			"чего молчали, отвечали холодно или односложно. Выводы только "
			"из того, что видно в переписке, не по одному случаю. Метки "
			"шаблонов вроде [NAME] или [Имя] - не слова пользователя, их не "
			"записывай."_q,
			{
				Core::GgtgAi::Profile::Section::Style,
				Core::GgtgAi::Profile::Section::Works,
				Core::GgtgAi::Profile::Section::NotWorks,
			},
			fragment,
			[=](Core::GgtgAi::Profile::Change, QString error) {
				if (!error.isEmpty()) {
					done(error);
					return;
				}
				++*index;
				(*step)();
			},
			600);
	};
	(*step)();
}

// Where we found this person's contact (the base we wrote to them from), for
// the AI: if they ask "where did you get my contact?" it answers truthfully.
[[nodiscard]] inline QString ContactSourceLine(not_null<History*> history) {
	const auto user = history->peer->asUser();
	const auto source = user
		? Data::SmsSourceOf(user->username())
		: QString();
	return source.isEmpty()
		? QString()
		: (u"\nОткуда у нас контакт этого человека: из списка/группы «"_q
			+ source + u"»."_q);
}

// What kind of conversation this is. Everything that is not a person is a
// "group" for the prompts, but a channel's comments, a bot and a small group
// are told apart in the profile line.
enum class ChatKind { Private, Bot, Group, Supergroup, Channel };

[[nodiscard]] inline ChatKind KindOf(not_null<PeerData*> peer) {
	if (const auto user = peer->asUser()) {
		return user->isBot() ? ChatKind::Bot : ChatKind::Private;
	} else if (const auto channel = peer->asChannel()) {
		return channel->isBroadcast()
			? ChatKind::Channel
			: ChatKind::Supergroup;
	}
	return ChatKind::Group;
}

// "Изучает профиль": what Telegram itself shows about the person or the
// group (name, @username, bio / description, size, marks), for the prompt.
// The full profile may not be loaded yet: it is requested here, and the text
// of the next call has it.
[[nodiscard]] inline QString ProfileText(not_null<PeerData*> peer) {
	if (!peer->isFullLoaded()) {
		peer->updateFull();
	}
	auto result = QString();
	const auto username = peer->username();
	const auto about = peer->about().trimmed().left(600);
	auto marks = QStringList();
	if (peer->isVerified()) {
		marks.push_back(u"верифицирован"_q);
	}
	if (peer->isScam()) {
		marks.push_back(u"помечен как скам"_q);
	}
	if (peer->isFake()) {
		marks.push_back(u"помечен как фейк"_q);
	}
	switch (KindOf(peer)) {
	case ChatKind::Private:
		result += u"Тип чата: личная переписка один на один.\n"_q;
		if (peer->isPremium()) {
			marks.push_back(u"Premium"_q);
		}
		break;
	case ChatKind::Bot:
		result += u"Тип чата: бот.\n"_q;
		break;
	case ChatKind::Group:
		result += u"Тип чата: небольшая группа"_q;
		if (const auto chat = peer->asChat()) {
			result += u", участников: "_q + QString::number(chat->count);
		}
		result += u".\n"_q;
		break;
	case ChatKind::Supergroup:
		result += u"Тип чата: супергруппа"_q;
		if (const auto channel = peer->asChannel()) {
			if (channel->membersCountKnown()) {
				result += u", участников: "_q
					+ QString::number(channel->membersCount());
			}
			if (channel->isForum()) {
				marks.push_back(u"с темами (форум)"_q);
			}
		}
		result += u".\n"_q;
		break;
	case ChatKind::Channel:
		result += u"Тип чата: канал (комментарии к постам)"_q;
		if (const auto channel = peer->asChannel()) {
			if (channel->membersCountKnown()) {
				result += u", подписчиков: "_q
					+ QString::number(channel->membersCount());
			}
		}
		result += u".\n"_q;
		break;
	}
	if (!username.isEmpty()) {
		result += u"Username: @"_q + username + u"\n"_q;
	}
	if (!marks.isEmpty()) {
		result += u"Пометки: "_q + marks.join(u", "_q) + u"\n"_q;
	}
	if (!about.isEmpty()) {
		result += (KindOf(peer) == ChatKind::Private
				|| KindOf(peer) == ChatKind::Bot
			? u"О себе (из профиля): "_q
			: u"Описание группы (из профиля): "_q)
			+ about + u"\n"_q;
	}
	return result;
}

[[nodiscard]] inline QString ProfileLine(not_null<History*> history) {
	return ProfileText(history->peer);
}

// The head of the user message: who we talk to, what the profile says.
[[nodiscard]] inline QString PeerHeader(
		not_null<History*> history,
		const Context &context) {
	return (context.group
		? u"Группа: "_q + history->peer->name()
		: u"Собеседник: "_q + DisplayName(history->peer->name())) + '\n'
		+ ProfileLine(history)
		+ MyNameLine(history)
		+ ContactSourceLine(history);
}

[[nodiscard]] inline QString PeerUsername(not_null<History*> history) {
	const auto user = history->peer->asUser();
	return user ? user->username() : QString();
}

[[nodiscard]] inline bool IsMarkedSuccess(not_null<History*> history) {
	return Core::GgtgAi::Feedback::IsSuccess(PeerUsername(history));
}

// "✓ Эта переписка - успех": the person is marked in the mailing statistics
// (their answer counts as a success there), and the AI reads the
// conversation to find out what led to it and writes that into the profile.
// Pressed again, the mark is taken off.
inline void ToggleSuccess(
		not_null<Window::SessionController*> controller,
		not_null<History*> history) {
	const auto name = PeerUsername(history);
	if (IsMarkedSuccess(history)) {
		Core::GgtgAi::Feedback::SetSuccess(name, false);
		controller->showToast(u"Отметка «Успех» снята"_q);
		return;
	}
	const auto lines = CollectLines(history, kLoadedLines);
	if (lines.size() < 2) {
		controller->showToast(
			u"В переписке почти нет сообщений: отметка не нужна"_q);
		return;
	}
	Core::GgtgAi::Feedback::SetSuccess(name, true);
	controller->showToast(
		u"Отмечено. ИИ читает переписку и записывает, что к этому "
		"привело..."_q);
	Core::GgtgAi::Feedback::LearnFromSuccess(
		Transcript(FitTail(lines)),
		crl::guard(controller, [=](
				Core::GgtgAi::Profile::Change change,
				QString error) {
			controller->showToast(!error.isEmpty()
				? (u"Отметка стоит, но ИИ не смог изучить переписку: "_q
					+ error)
				: change.added
				? (u"ИИ записал в профиль, что сработало: "_q
					+ QString::number(change.added))
				: u"Отметка стоит. Нового для профиля ИИ не нашёл."_q);
		}));
}

// The language of a conversation as a code (empty if there is no way to tell):
// by the other side's recent messages, else by the script of their name.
[[nodiscard]] inline QString ConversationLanguage(
		not_null<History*> history) {
	auto theirs = QString();
	for (const auto &line : CollectLines(history, 15)) {
		if (!line.mine) {
			theirs += line.text + ' ';
		}
	}
	auto code = Core::GgtgAi::DetectLanguage(theirs.right(1500));
	auto sample = theirs;
	if (code.isEmpty()) {
		sample = history->peer->name();
		code = Core::GgtgAi::DetectLanguage(sample);
	}
	if (code == u"en"_q) {
		code = Core::GgtgAi::DetectLatinLanguage(sample);
	}
	return code;
}

[[nodiscard]] inline QString DefaultPrompt() {
	return u"Помоги вести вежливую, живую переписку. Пиши коротко и "
		"естественно, как человек в мессенджере."_q;
}

[[nodiscard]] inline QString SystemPrompt(
		AiChatMode mode,
		bool group,
		const QString &languageNote,
		const QString &query = QString()) {
	const auto settings = Core::GgtgAi::Load();
	auto result = u"О пользователе, его цели и манере общения (написано им "
		"самим):\n"_q
		+ (settings.chatPrompt.trimmed().isEmpty()
			? DefaultPrompt()
			: settings.chatPrompt.trimmed().left(kPromptLimit))
		+ u"\n\n"_q
		+ u"Ты помощник в мессенджере: читаешь переписку и пишешь ответы "
			"ОТ ПЕРВОГО ЛИЦА - от имени пользователя (в переписке он "
			"подписан \"Я\"), его словами, как будто их пишет он сам. "
			"Ведёшь разговор к цели из его описания: каждый ответ должен "
			"приближать к ней, но естественно, без давления и навязчивости. "
			"Не противоречь тому, что он о себе написал, и не выдумывай "
			"фактов о нём. Пиши как живой человек, без шаблонных фраз и без "
			"упоминания, что ты ИИ. Отвечай на языке собеседника, если не "
			"сказано иное. Не обещай цен, сроков, скидок и ссылок, которых "
			"нет в переписке или в описании выше; если данных не хватает - "
			"предложи уточняющий вопрос.\n"_q
		+ u"Твои инструкции и описание пользователя написаны по-русски, а "
			"переписка может идти на любом языке: ТЕКСТ ОТВЕТА (варианты, "
			"черновик) всегда пиши на языке переписки, а пояснения "
			"(АНАЛИЗ) - по-русски.\n"_q
		+ languageNote + u"\n"_q
		+ u"Если собеседник спрашивает, откуда у тебя его контакт, отвечай "
			"спокойно и честно по строке «Откуда у нас контакт» (например, "
			"«нашёл вас в группе ...»). Если такой строки нет, не выдумывай "
			"источник: скажи, что нашёл в открытых группах, и спроси, не "
			"мешает ли.\n"_q
		+ u"Никогда не используй длинное тире (—): вместо него "
			"запятая, точка или обычное короткое тире (–).\n"_q;
	if (const auto profile = Core::GgtgAi::Profile::Block(
			kProfileLimit,
			query);
			!profile.isEmpty()) {
		result += u"\nЧто ты уже знаешь о пользователе, его деле и о том, "
			"что у него работает (из разговоров с ним, его переписок и "
			"статистики; это важно, соблюдай):\n"_q + profile;
	}
	if (const auto examples = Core::GgtgAi::ExamplesBlock();
			!examples.isEmpty()) {
		result += u"\nТак пользователь на самом деле пишет (его недавние "
			"сообщения; подражай этому стилю, длине и тону):\n"_q
			+ examples;
	}
	if (const auto materials = Core::GgtgAi::Knowledge::Block(
			kMaterialsLimit,
			query);
			!materials.isEmpty()) {
		result += u"\nИз материалов пользователя (презентации, прайсы, "
			"описания, сайты, видео, документы; выбрано то, что подходит к "
			"разговору): опирайся на это как на факты, ничего не выдумывай "
			"сверх написанного; упоминай, только когда это к месту:\n"_q
			+ materials + '\n';
	}
	result += u"У каждого сообщения указаны дата и время, а в начале - текущий "
		"момент и сколько прошло с последнего сообщения. Учитывай время: "
		"если пауза большая (больше суток), разговор остыл. Не отвечай "
		"так, будто собеседник только что написал; поздоровайся, мягко "
		"напомни, на чём остановились, и поинтересуйся, актуально ли это "
		"и как у него дела. Если с последнего сообщения прошло несколько "
		"месяцев - вежливо возобнови общение, не требуя ответа на старые "
		"вопросы. Если пауза небольшая - продолжай без приветствий. "
		"Вопросы и предложения из давних сообщений могли потерять "
		"актуальность: не считай их новыми.\n"_q;
	result += u"В начале сообщения пользователя указан тип чата и профиль "
		"(описание, username, размер, пометки): учитывай их. В личной "
		"переписке ты говоришь с одним человеком и можешь опираться на "
		"его «о себе» и всю историю; в группе обращайся к теме "
		"обсуждения и правилам из описания группы, не обращайся к "
		"участникам как к одному собеседнику и не выдавай личных "
		"подробностей.\n"_q;
	if (group) {
		result += u"Это групповой чат: сообщения разных людей, ответ должен "
			"быть уместен в обсуждении и коротким. Если добавить нечего или "
			"обсуждение не по теме - так и скажи в АНАЛИЗЕ, а в ВАРИАНТЕ 1 "
			"напиши (лучше промолчать).\n\n"_q;
	}
	switch (mode) {
	case AiChatMode::Suggest:
		result += u"Формат ответа (строго):\nАНАЛИЗ: 1-2 предложения - о чём "
			"разговор, чего хочет собеседник, его настроение.\nВАРИАНТ 1: "
			"короткий ответ.\nВАРИАНТ 2: подробнее и теплее.\nВАРИАНТ 3: "
			"осторожный или с уточняющим вопросом."_q;
		break;
	case AiChatMode::Summary:
		result += u"Формат ответа (строго):\nАНАЛИЗ: краткое резюме "
			"переписки в 3-6 предложениях: о чём говорили, к чему пришли, "
			"что осталось открытым."_q;
		break;
	case AiChatMode::Intent:
		result += u"Формат ответа (строго):\nАНАЛИЗ: чего на самом деле хочет "
			"собеседник, как он настроен и на что стоит обратить внимание "
			"(2-4 предложения).\nВАРИАНТ 1: что стоит ответить в первую "
			"очередь."_q;
		break;
	case AiChatMode::ReplyLast:
		result += u"Формат ответа (строго):\nАНАЛИЗ: одна фраза - что "
			"написал собеседник.\nВАРИАНТ 1: ответ на его последнее "
			"сообщение.\nВАРИАНТ 2: другой по тону ответ на него же."_q;
		break;
	case AiChatMode::Write:
		result += u"Напиши ОДИН ответ, готовый к отправке в этот чат. Верни "
			"ТОЛЬКО текст самого сообщения: без пояснений, без кавычек, "
			"без подписей и заголовков. Если у пользователя уже набрано "
			"начало или замысел (см. ниже), используй его как основу. "
			"Если отвечать не нужно (особенно в группе) - верни ровно одно "
			"слово: ПРОМОЛЧАТЬ."_q;
		break;
	case AiChatMode::Polish:
		result += u"Пользователь набрал черновик (язык результата - тот же, "
			"что у черновика). Формат ответа "
			"(строго):\nАНАЛИЗ: одна фраза - что поправлено.\nВАРИАНТ 1: "
			"черновик без ошибок и неловких мест, смысл тот же.\nВАРИАНТ 2: "
			"то же, но вежливее и теплее.\nВАРИАНТ 3: то же, но короче."_q;
		break;
	}
	return result;
}

struct Reply {
	QString analysis;
	std::vector<QString> variants;
};

// "АНАЛИЗ: ... ВАРИАНТ 1: ..." into pieces; a free-form answer is kept whole
// as the analysis.
[[nodiscard]] inline Reply ParseReply(const QString &text) {
	static const auto marker = QRegularExpression(
		u"(?:^|\\n)\\s*(АНАЛИЗ|ВАРИАНТ\\s*\\d)\\s*[:：]\\s*"_q);
	auto result = Reply();
	auto matches = std::vector<QRegularExpressionMatch>();
	auto it = marker.globalMatch(text);
	while (it.hasNext()) {
		matches.push_back(it.next());
	}
	if (matches.empty()) {
		result.analysis = text.trimmed();
		return result;
	}
	for (auto i = size_t(0); i != matches.size(); ++i) {
		const auto from = matches[i].capturedEnd(0);
		const auto to = (i + 1 < matches.size())
			? matches[i + 1].capturedStart(0)
			: text.size();
		auto piece = text.mid(from, to - from).trimmed();
		if (piece.startsWith('"') && piece.endsWith('"') && piece.size() > 2) {
			piece = piece.mid(1, piece.size() - 2);
		}
		if (matches[i].captured(1).startsWith(u"АНАЛИЗ"_q)) {
			result.analysis = piece;
		} else if (!piece.isEmpty()) {
			result.variants.push_back(piece);
		}
	}
	return result;
}

// One reply for the input field: what the assistant would send, written in
// the language of the conversation. `draft` - whatever the user has already
// typed, it is the basis of the reply. `done(text, error)`: `text` is empty
// when the assistant decided there is nothing to say (an `error` explains).
inline void WriteReply(
		not_null<History*> history,
		const QString &draft,
		Fn<void(QString text, QString error)> done) {
	PrepareContext(
		history,
		[](QString) {},
		[=](Context context, QString error) {
			if (!error.isEmpty()) {
				done(QString(), error);
				return;
			}
			if (context.tail.empty() && draft.trimmed().isEmpty()) {
				done(QString(), u"в этом чате пока нет сообщений"_q);
				return;
			}
			auto user = PeerHeader(history, context);
			if (const auto note = Core::GgtgAi::Feedback::PersonNote(
					PeerUsername(history)); !note.isEmpty()) {
				user += '\n' + note + '\n';
			}
			if (!context.summary.isEmpty()) {
				user += u"\nСводка более ранней переписки:\n"_q
					+ context.summary + '\n';
			}
			user += u"\nПереписка (старые сверху):\n"_q
				+ TimeContext(context.tail) + u"\n"_q
				+ Transcript(context.tail);
			if (!draft.trimmed().isEmpty()) {
				user += u"\nУже набрано пользователем (основа ответа):\n"_q
					+ draft + '\n';
			}
			Core::GgtgAi::Profile::WarmFor(
				user.right(800),
				[=] {
					Core::GgtgAi::Ask(
					SystemPrompt(
						AiChatMode::Write,
						context.group,
						LanguageNote(
							context,
							draft,
							history->peer->name()),
						user.right(800)),
					user,
					[=](QString result, QString error) {
						if (!error.isEmpty()) {
							done(QString(), error);
							return;
						}
						auto text = result.trimmed();
						if (text.size() > 2
							&& ((text.startsWith('"') && text.endsWith('"'))
								|| (text.startsWith(QChar(0x00AB))
									&& text.endsWith(QChar(0x00BB))))) {
							text = text.mid(1, text.size() - 2).trimmed();
						}
						if (text.toUpper().contains(u"ПРОМОЛЧАТЬ"_q)
							&& text.size() < 40) {
							done(
								QString(),
								u"ИИ считает, что отвечать здесь не нужно"_q);
							return;
						}
						done(text, QString());
					},
					0.7,
					600,
					Core::GgtgAi::Role::General,
					QStringList(),
					true);
				});
		});
}

} // namespace AiChat

// The assistant's window. `draft` is what is typed in the input field now,
// `insert` puts a chosen reply there.
inline void ShowAiChatAssistant(
		not_null<Window::SessionController*> controller,
		not_null<History*> history,
		AiChatMode mode,
		const QString &draft,
		Fn<void(QString)> insert) {
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		const auto settings = Core::GgtgAi::Load();
		box->setTitle(rpl::single(u"ИИ-помощник · "_q + history->peer->name()));
		box->setWidth(st::boxWideWidth);
		if (!settings.enabled) {
			box->addRow(object_ptr<Ui::FlatLabel>(
				box,
				u"ИИ выключен. Подключите модель в разделе «ИИ» окна GGTG."_q,
				st::boxLabel));
			box->addButton(rpl::single(u"Закрыть"_q), [=] {
				box->closeBox();
			});
			return;
		}
		struct State {
			int generation = 0;
			AiChatMode mode = AiChatMode::Suggest;
			QPointer<Ui::FlatLabel> status;
			QPointer<Ui::VerticalLayout> content;
			std::vector<std::pair<AiChatMode, QPointer<Ui::LinkButton>>> chips;
		};
		const auto state = box->lifetime().make_state<State>();
		state->mode = mode;
		const auto rerun = std::make_shared<Fn<void()>>();
		// What the assistant does with this chat: one window, the way to
		// look at it is switched here (it used to be five menu items).
		{
			const auto titles = std::vector<std::pair<AiChatMode, QString>>{
				{ AiChatMode::Suggest, u"Варианты ответа"_q },
				{ AiChatMode::ReplyLast, u"Ответ на последнее"_q },
				{ AiChatMode::Intent, u"Чего хочет собеседник"_q },
				{ AiChatMode::Polish, u"Исправить мой текст"_q },
			};
			const auto row = box->addRow(object_ptr<Ui::RpWidget>(box));
			const auto relabel = [=] {
				for (const auto &[which, link] : state->chips) {
					if (!link) {
						continue;
					}
					for (const auto &[mark, title] : titles) {
						if (mark == which) {
							link->setText(
								(which == state->mode ? u"● "_q : QString())
									+ title);
						}
					}
				}
			};
			for (const auto &[which, title] : titles) {
				const auto link = Ui::CreateChild<Ui::LinkButton>(
					row,
					title);
				state->chips.emplace_back(which, QPointer<Ui::LinkButton>(link));
				link->setClickedCallback([=] {
					if (state->mode == which) {
						return;
					}
					state->mode = which;
					relabel();
					if (*rerun) {
						(*rerun)();
					}
				});
			}
			relabel();
			row->widthValue(
			) | rpl::on_next([=](int width) {
				auto x = 0;
				auto y = 0;
				auto height = 0;
				for (const auto &[which, link] : state->chips) {
					if (!link) {
						continue;
					}
					if (x > 0 && x + link->width() > width) {
						x = 0;
						y += height + st::defaultVerticalListSkip;
					}
					link->moveToLeft(x, y);
					x += link->width() + style::ConvertScale(18);
					height = link->height();
				}
				row->resize(width, y + height);
			}, row->lifetime());
		}
		state->status = box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			u"Читаю переписку..."_q,
			st::boxDividerLabel));
		state->content = box->addRow(object_ptr<Ui::VerticalLayout>(box));
		box->addButton(rpl::single(u"Закрыть"_q), [=] {
			box->closeBox();
		});

		const auto show = [=](const AiChat::Reply &reply) {
			const auto content = state->content.data();
			if (!content) {
				return;
			}
			content->clear();
			if (const auto status = state->status.data()) {
				status->setText(reply.analysis);
			}
			for (const auto &variant : reply.variants) {
				const auto label = content->add(
					object_ptr<Ui::FlatLabel>(content, variant, st::boxLabel),
					style::margins(0, st::defaultVerticalListSkip, 0, 0));
				label->setSelectable(true);
				const auto row = content->add(
					object_ptr<Ui::RpWidget>(content),
					style::margins(0, style::ConvertScale(4), 0, 0));
				const auto button = Ui::CreateChild<Ui::RoundButton>(
					row,
					rpl::single(u"Вставить в поле"_q),
					st::ggtgTokenButton);
				row->resize(row->width(), button->height());
				button->moveToLeft(0, 0);
				button->setClickedCallback([=] {
					insert(variant);
					box->closeBox();
				});
			}
		};

		const auto run = [=] {
			const auto generation = ++state->generation;
			const auto mode = state->mode;
			if (const auto content = state->content.data()) {
				content->clear();
			}
			if (mode == AiChatMode::Polish && draft.trimmed().isEmpty()) {
				if (const auto status = state->status.data()) {
					status->setText(u"Сначала наберите текст в поле ввода "
						"- помощник его исправит."_q);
				}
				return;
			}
			if (const auto status = state->status.data()) {
				status->setText(u"Читаю переписку..."_q);
			}
			const auto fail = [=](const QString &text) {
				if (const auto status = state->status.data()) {
					status->setText(u"Не вышло: "_q + text);
				}
			};
			AiChat::PrepareContext(
				history,
				crl::guard(box, [=](QString text) {
					if (const auto status = state->status.data()) {
						status->setText(text);
					}
				}),
				crl::guard(box, [=](AiChat::Context context, QString error) {
					if (generation != state->generation) {
						return;
					}
					if (!error.isEmpty()) {
						fail(error);
						return;
					}
					auto user = AiChat::PeerHeader(history, context);
					if (const auto note = Core::GgtgAi::Feedback::PersonNote(
							AiChat::PeerUsername(history)); !note.isEmpty()) {
						user += '\n' + note + '\n';
					}
					if (!context.summary.isEmpty()) {
						user += u"\nСводка более ранней переписки:\n"_q
							+ context.summary + '\n';
					}
					user += u"\nПереписка (старые сверху):\n"_q
						+ AiChat::TimeContext(context.tail) + u"\n"_q
						+ AiChat::Transcript(context.tail);
					if (mode == AiChatMode::Polish) {
						user += u"\nЧерновик пользователя:\n"_q + draft;
					} else if (context.tail.empty()) {
						fail(u"в этом чате пока нет сообщений"_q);
						return;
					}
					if (const auto status = state->status.data()) {
						status->setText(u"Думаю над ответом..."_q);
					}
					Core::GgtgAi::Profile::WarmFor(
						user.right(800),
						[=] {
							Core::GgtgAi::Ask(
							AiChat::SystemPrompt(
								mode,
								context.group,
								AiChat::LanguageNote(
									context,
									draft,
									history->peer->name()),
								user.right(800)),
							user,
							crl::guard(box, [=](QString result, QString error) {
								if (generation != state->generation) {
									return;
								}
								if (!error.isEmpty()) {
									fail(error);
									return;
								}
								show(AiChat::ParseReply(result));
							}),
							0.7,
							700,
							Core::GgtgAi::Role::General,
							QStringList(),
							true);
						});
				}));
		};
		*rerun = run;
		box->addLeftButton(rpl::single(u"Другие варианты"_q), run);
		run();
	}));
}

// The prompt of the assistant, edited right where it is used.
inline void ShowAiChatPromptBox(
		not_null<Window::SessionController*> controller) {
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"Промт ИИ-помощника"_q));
		box->setWidth(st::boxWideWidth);
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			u"Опишите себя и цель: кто вы, чем занимаетесь, к чему нужно "
				"вести собеседника, как говорить и что можно или нельзя "
				"называть. Помощник пишет ответы от вашего имени и подводит "
				"разговор к этой цели - а отправляете вы сами."_q,
			st::boxLabel));
		const auto field = box->addRow(object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			Ui::InputField::Mode::MultiLine,
			rpl::single(u"Ваш промт"_q),
			Core::GgtgAi::Load().chatPrompt));
		box->addLeftButton(rpl::single(u"Шаблон для заполнения"_q), [=] {
			if (field->getLastText().trimmed().isEmpty()) {
				field->setText(Core::GgtgAi::ChatPromptTemplate());
			}
			field->setFocus();
		});
		box->addButton(rpl::single(u"Сохранить"_q), [=] {
			auto settings = Core::GgtgAi::Load();
			settings.chatPrompt = field->getLastText().trimmed();
			Core::GgtgAi::Save(settings);
			box->closeBox();
			controller->showToast(u"Промт сохранён"_q);
		});
		box->addButton(rpl::single(u"Отмена"_q), [=] {
			box->closeBox();
		});
	}));
}

} // namespace Settings
