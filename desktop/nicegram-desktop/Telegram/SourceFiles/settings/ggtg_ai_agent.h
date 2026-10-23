/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and exceptions, see https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "window/ggtg_widgets.h"
#include "base/call_delayed.h"
#include "core/ggtg_ai.h"
#include "core/ggtg_ai_profile.h"
#include "core/ggtg_ai_inbox.h"
#include "core/ggtg_ai_knowledge.h"
#include "core/ggtg_ai_train.h"
#include "core/ggtg_ai_web.h"
#include "core/application.h"
#include "core/ggtg_paths.h"
#include "data/data_session.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "apiwrap.h"
#include "mtproto/sender.h"
#include "data/scraped_members_db.h"
#include "data/sms_prompts.h"
#include "settings/ggtg_ai_cards.h"
#include "settings/ggtg_ai_chat.h"
#include "settings/ggtg_ai_dialog.h"
#include "settings/ggtg_ai_hub.h"
#include "settings/ggtg_ai_profile_box.h"
#include "settings/ggtg_ai_review_box.h"
#include "settings/ggtg_ai_train.h"
#include "settings/sms_campaign.h"
#include "ui/layers/generic_box.h"
#include "ui/layers/show.h"
#include "ui/text/text_entity.h"
#include "ui/ui_utility.h"
#include "ui/boxes/confirm_box.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/widgets/popup_menu.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_widgets.h"

#include <QtCore/QDate>

namespace Settings {

// The "ИИ-агент": a conversation with the 9B model that decides by itself
// what it needs - a web search, a page, the user's own campaign numbers,
// templates and bases - and remembers what it learns about the user's
// business between conversations (ai_agent.json). Every step it takes is
// shown as it happens; nothing is sent to Telegram, the agent only reads.
namespace AiAgent {

constexpr auto kMaxRounds = 8;
constexpr auto kHistoryTurns = 12;
constexpr auto kKeptTurns = 40;
constexpr auto kProfileLimit = 3000;
constexpr auto kPageChars = 5000;
// What the conversation may hold before old tool results are shortened:
// about 9-10 thousand tokens, the rest of the 16K is the answer's.
constexpr auto kContextChars = 26000;

struct Turn {
	bool mine = false;
	QString text;
	QStringList steps;
};

struct Saved {
	std::vector<Turn> turns;
};

[[nodiscard]] inline QString Path() {
	return Core::GgtgPaths::DbDir() + u"ai_agent.json"_q;
}

[[nodiscard]] inline Saved Load() {
	auto result = Saved();
	auto file = QFile(Path());
	if (!file.open(QIODevice::ReadOnly)) {
		return result;
	}
	const auto object = QJsonDocument::fromJson(file.readAll()).object();
	for (const auto &value : object.value("turns").toArray()) {
		const auto turn = value.toObject();
		auto steps = QStringList();
		for (const auto &step : turn.value("steps").toArray()) {
			steps.push_back(step.toString());
		}
		result.turns.push_back({
			.mine = turn.value("mine").toBool(),
			.text = turn.value("text").toString(),
			.steps = steps,
		});
	}
	return result;
}

inline void Save(const Saved &saved) {
	// The old file still holds the notes of the first version: they move into
	// the profile on its first read, which must come before they are gone.
	(void)Core::GgtgAi::Profile::Read();
	auto turns = QJsonArray();
	const auto from = std::max(0, int(saved.turns.size()) - kKeptTurns);
	for (auto i = from; i < int(saved.turns.size()); ++i) {
		const auto &turn = saved.turns[i];
		auto steps = QJsonArray();
		for (const auto &step : turn.steps) {
			steps.push_back(step);
		}
		turns.push_back(QJsonObject{
			{ "mine", turn.mine },
			{ "text", turn.text },
			{ "steps", steps },
		});
	}
	QDir().mkpath(Core::GgtgPaths::DbDir());
	auto file = QSaveFile(Path());
	if (file.open(QIODevice::WriteOnly)) {
		file.write(QJsonDocument(QJsonObject{
			{ "turns", turns },
		}).toJson(QJsonDocument::Compact));
		file.commit();
	}
}

[[nodiscard]] inline QString Cut(QString text, int limit) {
	if (text.size() > limit) {
		text = text.left(limit) + u"..."_q;
	}
	return text;
}

[[nodiscard]] inline QJsonObject Tool(
		const QString &name,
		const QString &description,
		const QString &argument = QString(),
		const QString &argumentDescription = QString()) {
	auto properties = QJsonObject();
	auto required = QJsonArray();
	if (!argument.isEmpty()) {
		properties.insert(argument, QJsonObject{
			{ "type", "string" },
			{ "description", argumentDescription },
		});
		required.push_back(argument);
	}
	return QJsonObject{
		{ "type", "function" },
		{ "function", QJsonObject{
			{ "name", name },
			{ "description", description },
			{ "parameters", QJsonObject{
				{ "type", "object" },
				{ "properties", properties },
				{ "required", required },
			} },
		} },
	};
}

[[nodiscard]] inline QJsonArray Tools() {
	return QJsonArray{
		Tool(
			u"web_search"_q,
			u"Поиск в интернете: свежие факты, цифры, примеры, тренды, "
				"данные о стране, нише или аудитории. Возвращает до 6 "
				"ссылок с коротким текстом."_q,
			u"query"_q,
			u"Поисковый запрос; для мировых данных лучше по-английски"_q),
		Tool(
			u"open_page"_q,
			u"Прочитать текст одной страницы по адресу (обычно из "
				"результатов web_search)."_q,
			u"url"_q,
			u"Адрес страницы"_q),
		Tool(
			u"sms_stats"_q,
			u"Статистика рассылок пользователя в GGTG: сколько отправлено, "
				"сколько ответили, по странам номеров, языкам, Premium, "
				"времени, шаблонам, переводу, стикеру, ошибки и спам-блоки."_q),
		Tool(
			u"sms_templates"_q,
			u"Тексты рассылки пользователя: текущий текст и сохранённые "
				"шаблоны."_q),
		Tool(
			u"last_mailing"_q,
			u"Статистика только ПОСЛЕДНЕЙ рассылки пользователя (тот же "
				"разбор, что sms_stats, но за один запуск): чтобы разобрать, "
				"что сработало в ней."_q),
		Tool(
			u"ab_results"_q,
			u"Сравнение текстов, которые были в рассылке рядом (A/B): у "
				"кого сколько ушло и сколько положительных ответов, кто "
				"лидирует и достоверна ли разница."_q),
		Tool(
			u"bases_overview"_q,
			u"Базы пользователя в GGTG: названия, сколько в каждой людей "
				"или групп."_q),
		Tool(
			u"study_profile"_q,
			u"Зайти в Telegram и изучить профиль человека или группы/канала "
				"по @username: имя, bio или описание, число участников, "
				"пометки. Только чтение."_q,
			u"username"_q,
			u"@username или t.me-ссылка, например @durov"_q),
		Tool(
			u"my_materials"_q,
			u"Найти в материалах пользователя (прайсы, презентации, сайты, "
				"видео, документы, а также выученные из его чатов вопросы, "
				"возражения и его ответы) то, что относится к вопросу."_q,
			u"query"_q,
			u"О чём искать, своими словами"_q),
		Tool(
			u"set_option"_q,
			u"Включить или выключить настройку GGTG, когда пользователь "
				"просит. Возможные значения: «ai on», «ai off» (весь ИИ), "
				"«chat_button on/off» (кнопка ИИ в поле ввода чата), "
				"«mailing_helper on/off» (помощник под текстом рассылки), "
				"«process_summaries on/off» (сводка ИИ на процессах), "
				"«night_training on/off» (учиться само по ночам)."_q,
			u"change"_q,
			u"Например «chat_button off»"_q),
		Tool(
			u"what_i_learned"_q,
			u"Что ИИ выучил о пользователе: профиль, лучшие (сработавшие) "
				"ответы на вопросы и возражения, итоги обучения на чатах."_q),
		Tool(
			u"remember"_q,
			u"Запомнить надолго важный факт о пользователе или его деле, "
				"который он сам сказал или который подтвердили его цифры. "
				"Одна короткая фраза, начинается с тега и двоеточия: "
				"«дело:», «цель:», «аудитория:», «правило:», «работает:» или "
				"«не работает:». Факты из интернета не запоминай."_q,
			u"fact"_q,
			u"Тег и факт, например «аудитория: Корея, трейдинг»"_q),
		Tool(
			u"forget"_q,
			u"Забыть факт о пользователе: убирает из профиля все записи, "
				"в которых есть эти слова. Вызывай, когда пользователь "
				"просит забыть, убрать или исправить что-то о нём. Сначала "
				"вызови what_i_learned, чтобы увидеть точные формулировки."_q,
			u"text"_q,
			u"Слова или кусок фразы из факта, который надо забыть"_q),
		Tool(
			u"add_material"_q,
			u"Добавить в материалы ИИ ссылку (сайт, PDF, YouTube) или "
				"текст (прайс, условия, FAQ, описание продукта), чтобы ИИ "
				"опирался на это в ответах людям."_q,
			u"source"_q,
			u"Ссылка или сам текст"_q),
		Tool(
			u"list_materials"_q,
			u"Какие материалы уже добавлены: названия, виды, сколько "
				"кусков."_q),
		Tool(
			u"training"_q,
			u"Обучение ИИ на личных чатах пользователя: узнать, идёт ли оно "
				"и как далеко, запустить или остановить. Запускай только "
				"если пользователь прямо просит."_q,
			u"action"_q,
			u"status, start или stop"_q),
	};
}

[[nodiscard]] inline QString SystemPrompt() {
	const auto settings = Core::GgtgAi::Load();
	auto result = u"Ты - ИИ-агент внутри GGTG. GGTG - программа для Telegram: "
		"много аккаунтов, сбор баз людей из групп и каналов, массовая "
		"«Рассылка» (личные сообщения с привет-стикером, прогревом и "
		"переводом на язык получателя), Сквидвард (пробив людей и групп), "
		"анализ и чистка баз. Твоя задача - помочь пользователю получать "
		"больше ответов и продаж: лучшие тексты, аудитории, время, базы, "
		"аккаунты.\n"
		"Сегодня "_q + QDate::currentDate().toString(u"dd.MM.yyyy"_q)
		+ u".\n\nКак работать:\n"
		"• Чтобы узнать человека или группу, вызови study_profile с их @username.\n"
		"• Цифры пользователя бери только из sms_stats, sms_templates, "
		"bases_overview - не выдумывай их.\n"
		"• В статистике «положительные ответы» - это интерес и вопросы "
		"людей (а также переписки, которые пользователь отметил как "
		"успех); конверсия - их доля среди отправленных. Если типы ответов "
		"не определены, скажи пользователю нажать «Определить типы ответов» "
		"в «Разборе рассылок».\n"
		"• Нужны свежие факты, примеры или данные о рынке - web_search, "
		"потом open_page по 1-3 лучшим ссылкам. Называй сайт-источник.\n"
		"• Разбор рассылки: вызови last_mailing (при нужде ещё sms_stats), "
		"скажи, что сработало, что нет и что поменять в следующий раз; "
		"выводы про «работает» и «не работает» запоминай через remember.\n"
		"• Сравнение текстов - ab_results. Не называй победителя, пока "
		"инструмент не написал, что разница достоверна.\n"
		"• Предлагая тексты сообщений, пиши каждый так: «ВАРИАНТ 1: текст» "
		"(на языке аудитории из профиля, [NAME] там, где нужно имя "
		"получателя), каждый вариант - один абзац; пояснения давай отдельно "
		"после всех вариантов, через пустую строку.\n"
		"• Не отправляй пользователя «посмотреть самому»: если для совета "
		"нужны его тексты, базы или статистика - сам вызови инструмент.\n"
		"• После web_search открой 1-2 лучшие ссылки через open_page и "
		"только потом отвечай.\n"
		"• Пиши только о том, что действительно сделал: не говори «открыл "
		"статью» или «посмотрел», если не вызывал для этого инструмент.\n"
		"• Узнал о пользователе что-то важное и долгое - вызови remember.\n"
		"• Ты - единственный помощник пользователя в этом окне: он говорит "
		"с тобой обо всём. Просит забыть или исправить что-то о нём - "
		"вызови forget. Даёт ссылку, прайс или текст - add_material. "
		"Спрашивает, что уже добавлено - list_materials. Про обучение на "
		"его чатах - training. Просит рассказать, что ты знаешь о нём - "
		"what_i_learned, и перескажи по-человечески, по пунктам.\n"
		"• Если пользователь просит расспросить его: задавай ОДИН короткий "
		"вопрос за раз (дело, цель, аудитория, стиль, правила, что работает), "
		"после каждого ответа вызывай remember и переходи к следующему "
		"вопросу; когда всё важное узнал - скажи об этом.\n"
		"• Ты - второй мозг пользователя: про его продукт, цены и условия "
		"сначала смотри my_materials; что работает в его переписках - "
		"what_i_learned. Просит включить или выключить что-то в GGTG - "
		"set_option. "
		"Лучшие ответы (помечены «сработал») предлагай в первую очередь.\n"
		"• Если о деле пользователя почти ничего не известно, сначала "
		"одним коротким вопросом узнай нишу и цель.\n"
		"• Отвечай по-русски, конкретно: что сделать, какой текст, какие "
		"цифры. Без markdown: без **, без #, списки через «•».\n"
		"• Береги аккаунты: советуй то, что не ведёт к спам-блокам и "
		"банам, учитывай лимиты Telegram."_q;
	if (!settings.chatPrompt.trimmed().isEmpty()) {
		result += u"\n\nПользователь о себе:\n"_q
			+ Cut(settings.chatPrompt.trimmed(), 1500);
	}
	if (const auto profile = Core::GgtgAi::Profile::Block(kProfileLimit);
			!profile.isEmpty()) {
		result += u"\n\nПрофиль пользователя (что ты уже знаешь о нём и "
			"его деле, соблюдай и опирайся):\n"_q + profile;
	}
	return result;
}

// The model's own formatting habits out: bold marks, headings, "*" lists.
[[nodiscard]] inline QString CleanAnswer(QString text) {
	static const auto heading = QRegularExpression(
		u"^#{1,6}\\s*"_q,
		QRegularExpression::MultilineOption);
	static const auto bullet = QRegularExpression(
		u"^(\\s*)[*-]\\s+"_q,
		QRegularExpression::MultilineOption);
	text.remove(u"**"_q);
	text.remove(u"__"_q);
	text.remove(heading);
	text.replace(bullet, u"\\1• "_q);
	return text.trimmed();
}

[[nodiscard]] inline QString Host(const QString &url) {
	const auto host = QUrl(url).host();
	return host.startsWith(u"www."_q) ? host.mid(4) : host;
}

[[nodiscard]] inline QString TemplatesText() {
	auto result = QString();
	const auto form = Data::LoadSmsForm();
	if (!form.text.trimmed().isEmpty()) {
		result += u"Текущий текст рассылки:\n"_q
			+ Cut(form.text.trimmed(), 1200) + u"\n\n"_q;
	} else {
		result += u"Поле текста рассылки сейчас пустое.\n"_q;
	}
	const auto prompts = Data::LoadSmsPrompts();
	if (prompts.empty()) {
		result += u"Сохранённых шаблонов нет.\n\n"_q;
	}
	for (const auto &prompt : prompts) {
		const auto used = ranges::contains(form.rotationPromptIds, prompt.id);
		result += u"Шаблон «"_q + prompt.name + u"»"_q
			+ (used ? u" (в ротации)"_q : QString()) + u":\n"_q
			+ Cut(prompt.text.trimmed(), 600) + u"\n\n"_q;
		if (result.size() > 3500) {
			result += u"(остальные шаблоны не показаны)"_q;
			break;
		}
	}
	if (form.saved) {
		result += u"Настройки: привет-стикер "_q
			+ (form.greetingSticker ? u"вкл"_q : u"выкл"_q)
			+ u", прогрев "_q + (form.warmup ? u"вкл"_q : u"выкл"_q)
			+ u", лимит на аккаунт "_q + form.limitPerAccount
			+ u", пауза "_q + form.pauseMin + '-' + form.pauseMax
			+ u" сек."_q;
	}
	return result.trimmed();
}

[[nodiscard]] inline QString BasesText() {
	const auto index = Data::LoadScrapedGroupsIndex();
	if (index.empty()) {
		return u"Баз пока нет."_q;
	}
	auto result = QString();
	auto people = 0, groups = 0, shown = 0;
	for (const auto &base : index) {
		const auto isGroups = (base.kind == Data::BaseKind::Groups);
		(isGroups ? groups : people) += base.count;
		if (shown++ < 60) {
			result += u"• "_q + base.title + u" - "_q
				+ QString::number(base.count)
				+ (isGroups ? u" групп"_q : u" чел."_q) + u", от "_q
				+ QDateTime::fromSecsSinceEpoch(base.savedAt).toString(
					u"dd.MM.yyyy"_q)
				+ '\n';
		}
	}
	return u"Всего баз: "_q + QString::number(index.size())
		+ u", людей: "_q + QString::number(people)
		+ u", групп: "_q + QString::number(groups) + u"\n"_q + result;
}

// Old tool results are shortened first when the conversation grows past
// what the model can hold.
inline void FitContext(QJsonArray &messages) {
	auto total = 0;
	for (const auto &value : messages) {
		total += value.toObject().value("content").toString().size();
	}
	for (auto i = 0; i < messages.size() && total > kContextChars; ++i) {
		auto message = messages.at(i).toObject();
		if (message.value("role").toString() != u"tool"_q) {
			continue;
		}
		const auto content = message.value("content").toString();
		if (content.size() <= 400) {
			continue;
		}
		const auto shorter = content.left(300) + u"... (сокращено)"_q;
		total -= content.size() - shorter.size();
		message.insert("content", shorter);
		messages.replace(i, message);
	}
}

// Looks a @username up with the active account and reads the public profile
// (the full info is requested and given a moment to arrive).
inline void StudyProfile(
		const QString &username,
		Fn<void(QString text, QString error)> done) {
	auto &account = Core::App().domain().active();
	if (!account.sessionExists()) {
		done(QString(), u"нет активного аккаунта Telegram"_q);
		return;
	}
	const auto session = &account.session();
	const auto finish = [=](PeerData *peer) {
		if (!peer) {
			done(QString(), u"такого username нет"_q);
			return;
		}
		peer->updateFull();
		base::call_delayed(1500, session, [=] {
			auto text = u"Название: "_q + peer->name() + '\n'
				+ AiChat::ProfileText(peer);
			done(text, QString());
		});
	};
	if (const auto peer = session->data().peerByUsername(username)) {
		finish(peer);
		return;
	}
	const auto api = static_cast<MTP::Sender*>(&session->api());
	api->request(MTPcontacts_ResolveUsername(
		MTP_flags(MTPcontacts_ResolveUsername::Flags(0)),
		MTP_string(username),
		MTP_string(QString())
	)).handleFloodErrors().done([=](
			const MTPcontacts_ResolvedPeer &result) {
		result.match([&](const MTPDcontacts_resolvedPeer &data) {
			session->data().processUsers(data.vusers());
			session->data().processChats(data.vchats());
			const auto peerId = peerFromMTP(data.vpeer());
			finish(peerId ? session->data().peer(peerId).get() : nullptr);
		});
	}).fail([=](const MTP::Error &error) {
		done(QString(), error.type());
	}).send();
}

// One tool call: `step` gets what is being done now, `done` the result for
// the model and the line that stays in the list of steps.
inline void RunTool(
		const QString &name,
		const QJsonObject &arguments,
		Fn<void(QString step)> step,
		Fn<void(QString result, QString line)> done) {
	if (name == u"web_search"_q) {
		const auto query = arguments.value("query").toString().trimmed();
		if (query.isEmpty()) {
			done(u"Пустой запрос."_q, u"пустой поисковый запрос"_q);
			return;
		}
		step(u"ищет: «"_q + query + u"»"_q);
		Core::GgtgAi::Web::Search(query, [=](
				std::vector<Core::GgtgAi::Web::Hit> hits,
				QString error) {
			if (!error.isEmpty()) {
				done(
					u"Поиск не удался: "_q + error,
					u"поиск не ответил: «"_q + query + u"»"_q);
				return;
			}
			auto result = QString();
			auto i = 0;
			for (const auto &hit : hits) {
				result += QString::number(++i) + u". "_q + hit.title
					+ u"\n   "_q + hit.url + u"\n   "_q
					+ Cut(hit.snippet, 300) + '\n';
			}
			done(
				result,
				u"нашёл "_q + QString::number(hits.size())
					+ u" ссылок: «"_q + query + u"»"_q);
		});
	} else if (name == u"open_page"_q) {
		const auto url = arguments.value("url").toString().trimmed();
		step(u"читает: "_q + Host(url));
		Core::GgtgAi::Web::FetchPage(url, kPageChars, [=](
				QString title,
				QString text,
				QString error) {
			if (!error.isEmpty()) {
				done(
					u"Страница не прочитана: "_q + error,
					u"не открылась: "_q + Host(url) + u" ("_q
						+ Cut(error, 60) + ')');
				return;
			}
			done(
				u"Страница: "_q + title + u" ("_q + url + u")\n"_q + text,
				u"прочитал: "_q + Host(url));
		});
	} else if (name == u"study_profile"_q) {
		auto username = arguments.value("username").toString().trimmed();
		if (const auto at = username.lastIndexOf('/'); at >= 0) {
			username = username.mid(at + 1);
		}
		username = username.remove('@').section('?', 0, 0);
		if (username.isEmpty()) {
			done(u"Пустой username."_q, u"не указан username"_q);
			return;
		}
		step(u"изучает профиль @"_q + username);
		StudyProfile(username, [=](QString text, QString error) {
			if (!error.isEmpty()) {
				done(
					u"Профиль не получен: "_q + error,
					u"профиль @"_q + username + u": "_q + Cut(error, 60));
				return;
			}
			done(text, u"изучил профиль @"_q + username);
		});
	} else if (name == u"sms_stats"_q) {
		step(u"смотрит статистику рассылок"_q);
		const auto report = SmsStatsReportText();
		if (report.isEmpty()) {
			done(
				u"Записей о рассылках пока нет."_q,
				u"статистики рассылок пока нет"_q);
		} else {
			done(Cut(report, 6000), u"посмотрел статистику рассылок"_q);
		}
	} else if (name == u"last_mailing"_q) {
		step(u"смотрит последнюю рассылку"_q);
		const auto report = SmsStatsReportText(true);
		if (report.isEmpty()) {
			done(
				u"Записей о рассылках пока нет."_q,
				u"рассылок пока не было"_q);
		} else {
			done(Cut(report, 6000), u"посмотрел последнюю рассылку"_q);
		}
	} else if (name == u"ab_results"_q) {
		step(u"сравнивает тексты (A/B)"_q);
		done(SmsAbReportText(), u"сравнил тексты (A/B)"_q);
	} else if (name == u"sms_templates"_q) {
		step(u"смотрит тексты рассылки"_q);
		done(TemplatesText(), u"посмотрел тексты рассылки"_q);
	} else if (name == u"bases_overview"_q) {
		step(u"смотрит базы"_q);
		done(BasesText(), u"посмотрел базы"_q);
	} else if (name == u"my_materials"_q) {
		const auto query = arguments.value("query").toString().trimmed();
		step(u"ищет в материалах: «"_q + query + u"»"_q);
		Core::GgtgAi::Knowledge::Warm(query, [=] {
			const auto found = Core::GgtgAi::Knowledge::Block(5000, query);
			done(
				found.isEmpty()
					? u"В материалах пользователя ничего нет."_q
					: found,
				u"поискал в материалах"_q);
		});
	} else if (name == u"set_option"_q) {
		const auto change = arguments.value("change").toString().trimmed()
			.toLower();
		const auto parts = change.split(' ', Qt::SkipEmptyParts);
		if (parts.size() != 2
			|| (parts[1] != u"on"_q && parts[1] != u"off"_q)) {
			done(u"Не понял настройку."_q, QString());
			return;
		}
		const auto on = (parts[1] == u"on"_q);
		auto settings = Core::GgtgAi::Load();
		auto title = QString();
		if (parts[0] == u"ai"_q) {
			settings.enabled = on;
			title = u"ИИ"_q;
		} else if (parts[0] == u"chat_button"_q) {
			settings.chatAssistant = on;
			title = u"кнопка ИИ в чате"_q;
		} else if (parts[0] == u"mailing_helper"_q) {
			settings.templateAssistant = on;
			title = u"помощник под текстом рассылки"_q;
		} else if (parts[0] == u"process_summaries"_q) {
			settings.summaries = on;
			title = u"сводка ИИ на процессах"_q;
		} else if (parts[0] == u"night_training"_q) {
			settings.trainNightly = on;
			title = u"обучение по ночам"_q;
		} else {
			done(u"Такой настройки нет."_q, QString());
			return;
		}
		Core::GgtgAi::Save(settings);
		done(
			u"Готово: "_q + title + (on ? u" включено."_q : u" выключено."_q),
			u""_q + title + (on ? u": включено"_q : u": выключено"_q));
	} else if (name == u"answers_feed"_q) {
		step(u"смотрит ленту ответов"_q);
		namespace Inbox = Core::GgtgAi::Inbox;
		auto result = QString();
		for (const auto &item : Inbox::Items()) {
			if (item.status != Inbox::Status::Pending
				&& item.status != Inbox::Status::Hot) {
				continue;
			}
			result += (item.status == Inbox::Status::Hot
					? u"горячий лид "_q
					: QString())
				+ u"@"_q + item.username + u" ("_q
				+ Inbox::StageTitle(item.stage) + u"): «"_q
				+ Cut(item.theirText, 200) + u"»"_q
				+ (item.draft.isEmpty()
					? QString()
					: (u"\n   ИИ предлагает: "_q + Cut(item.draft, 300)))
				+ '\n';
		}
		const auto counts = Inbox::Count();
		done(
			result.isEmpty()
				? u"Сейчас никто не ждёт ответа."_q
				: (u"Ждут ответа: %1, горячих: %2\n"_q
					.arg(counts.pending)
					.arg(counts.hot) + Cut(result, 6000)),
			u"посмотрел ленту ответов"_q);
	} else if (name == u"what_i_learned"_q) {
		step(u"смотрит, что выучено"_q);
		namespace Train = Core::GgtgAi::Train;
		auto list = Train::Playbook();
		std::stable_sort(list.begin(), list.end(), [](
				const auto &a,
				const auto &b) {
			return Train::Weight(a) > Train::Weight(b);
		});
		auto best = QString();
		for (auto i = 0; i != std::min(int(list.size()), 15); ++i) {
			best += Train::EntryChunk(list[i]) + u"\n\n"_q;
		}
		done(
			u"Профиль:\n"_q + Core::GgtgAi::Profile::Block(3000)
				+ u"\n\nОбучение на чатах: "_q + Train::Summary()
				+ u"\n\nЛучшие ответы:\n"_q
				+ (best.isEmpty() ? u"пока нет"_q : Cut(best, 5000)),
			u"посмотрел, что выучено"_q);
	} else if (name == u"remember"_q) {
		const auto raw = Cut(
			arguments.value("fact").toString().trimmed(),
			300);
		if (raw.isEmpty()) {
			done(u"Нечего запоминать."_q, QString());
			return;
		}
		using Profile = Core::GgtgAi::Profile::Section;
		auto section = Profile::Business;
		auto fact = raw;
		const auto colon = raw.indexOf(':');
		if (colon > 0) {
			if (const auto found = Core::GgtgAi::Profile::details
					::SectionByTag(raw.left(colon))) {
				section = *found;
				fact = raw.mid(colon + 1).trimmed();
			}
		}
		auto profile = Core::GgtgAi::Profile::Read();
		if (!Core::GgtgAi::Profile::Add(
				profile,
				section,
				fact,
				QDateTime::currentSecsSinceEpoch())) {
			// The same fact is there already: nothing to show.
			done(u"Это уже записано в профиле."_q, QString());
			return;
		}
		Core::GgtgAi::Profile::Write(profile);
		done(
			u"Запомнено."_q,
			u"запомнил: "_q
				+ Core::GgtgAi::Profile::Info(section).tag + u": "_q + fact);
	} else if (name == u"forget"_q) {
		const auto needle = arguments.value("text").toString().trimmed();
		if (needle.size() < 3) {
			done(u"Слишком коротко: назови слова из факта."_q, QString());
			return;
		}
		auto profile = Core::GgtgAi::Profile::Read();
		auto removed = QStringList();
		for (auto &list : profile.sections) {
			for (auto i = list.begin(); i != list.end();) {
				if (i->text.contains(needle, Qt::CaseInsensitive)) {
					removed.push_back(i->text);
					i = list.erase(i);
				} else {
					++i;
				}
			}
		}
		if (removed.isEmpty()) {
			done(
				u"Такого факта в профиле нет."_q,
				u"в профиле нет «"_q + needle + u"»"_q);
			return;
		}
		Core::GgtgAi::Profile::Write(profile);
		done(
			u"Забыл: "_q + removed.join(u"; "_q),
			u"забыл: "_q + QString::number(removed.size())
				+ u" факт(ов)"_q);
	} else if (name == u"add_material"_q) {
		const auto source = arguments.value("source").toString().trimmed();
		if (source.isEmpty()) {
			done(u"Нечего добавлять."_q, QString());
			return;
		}
		namespace Knowledge = Core::GgtgAi::Knowledge;
		static const auto linkRe = QRegularExpression(
			u"^(https?://\\S+|www\\.\\S+|\\S+\\.[a-z]{2,}/\\S*)$"_q);
		const auto finish = [=](QString error, QString what) {
			done(
				error.isEmpty()
					? (u"Добавлено в материалы: "_q + what)
					: (u"Не вышло: "_q + error),
				error.isEmpty()
					? (u"добавил материал: "_q + what)
					: (u"материал не добавлен: "_q + error));
		};
		if (linkRe.match(source).hasMatch()) {
			step(u"читает ссылку: "_q + Cut(source, 60));
			Knowledge::AddUrl(source, [=](QString) {}, [=](QString error) {
				finish(error, Cut(source, 60));
			});
		} else {
			auto title = source.left(40).simplified();
			if (source.size() > 40) {
				title += u"…"_q;
			}
			step(u"добавляет текст: "_q + title);
			Knowledge::AddText(title, source, [=](QString) {}, [=](
					QString error) {
				finish(error, title);
			});
		}
	} else if (name == u"list_materials"_q) {
		namespace Knowledge = Core::GgtgAi::Knowledge;
		const auto &all = Knowledge::List();
		if (all.empty()) {
			done(u"Материалов пока нет."_q, u"посмотрел материалы"_q);
			return;
		}
		auto result = QString();
		for (const auto &material : all) {
			result += u"• "_q + material.name + u" ("_q
				+ Knowledge::KindTitle(material.kind) + u", кусков: "_q
				+ QString::number(material.chunks.size()) + u")\n"_q;
		}
		done(result, u"посмотрел материалы: "_q
			+ QString::number(all.size()));
	} else if (name == u"training"_q) {
		const auto action = arguments.value("action").toString().trimmed()
			.toLower();
		if (action == u"start"_q) {
			const auto error = AiTrain::Start();
			done(
				error.isEmpty() ? u"Обучение запущено."_q : error,
				error.isEmpty()
					? u"запустил обучение на чатах"_q
					: (u"обучение не запущено: "_q + error));
		} else if (action == u"stop"_q) {
			AiTrain::Stop();
			done(u"Остановлено."_q, u"остановил обучение"_q);
		} else {
			done(
				AiTrain::Running()
					? (u"Идёт: "_q + AiTrain::Status())
					: Core::GgtgAi::Train::Summary(),
				u"посмотрел, как идёт обучение"_q);
		}
	} else {
		done(u"Такого инструмента нет."_q, QString());
	}
}

// The texts of an answer ("ВАРИАНТ 1: ..."): each one is a single paragraph,
// what follows an empty line is the agent's explanation, not the message.
[[nodiscard]] inline std::vector<QString> VariantsOf(const QString &answer) {
	auto result = std::vector<QString>();
	for (auto variant : AiChat::ParseReply(answer).variants) {
		const auto blank = variant.indexOf(QRegularExpression(
			u"\\n\\s*\\n"_q));
		if (blank > 0) {
			variant = variant.left(blank);
		}
		variant = variant.trimmed();
		if (variant.size() > 2
			&& (variant.startsWith('"') || variant.startsWith(QChar(0x00AB)))
			&& (variant.endsWith('"') || variant.endsWith(QChar(0x00BB)))) {
			variant = variant.mid(1, variant.size() - 2).trimmed();
		}
		if (variant.size() >= 6) {
			result.push_back(variant);
		}
	}
	return result;
}

// What the user thought of an answer goes into the profile: a good answer
// shows what to repeat, a bad one what not to do.
inline void RateAnswer(
		const QString &question,
		const QString &answer,
		bool good,
		Fn<void(Core::GgtgAi::Profile::Change change, QString error)> done) {
	using Section = Core::GgtgAi::Profile::Section;
	Core::GgtgAi::Profile::Extract(
		good
			? u"Пользователь поставил ответу ИИ-агента 👍. Выпиши, что "
				"именно в таком ответе стоит повторять (формат, длина, тон, "
				"какие советы и тексты): тег «стиль» или «работает». Если "
				"устойчивого вывода не видно, ответь НЕТ."_q
			: u"Пользователь поставил ответу ИИ-агента 👎. Выпиши, чего в "
				"таких ответах делать НЕ нужно (тег «правило»: что не "
				"делать) или что не сработало («не работает»). Если "
				"устойчивого вывода не видно, ответь НЕТ."_q,
		good
			? std::vector<Section>{ Section::Style, Section::Works }
			: std::vector<Section>{
				Section::Rules,
				Section::NotWorks,
				Section::Style,
			},
		u"Вопрос пользователя: "_q + Cut(question, 600)
			+ u"\n\nОтвет ИИ-агента:\n"_q + Cut(answer, 1500),
		std::move(done),
		300);
}

// The texts of an answer ("ВАРИАНТ 1: ...") become saved templates and go
// into the rotation of the mailing, so that they are sent side by side and
// the agent can compare them (ab_results).
inline void SaveVariants(
		std::shared_ptr<Ui::Show> show,
		const std::vector<QString> &variants) {
	const auto stamp = QDateTime::currentDateTime().toString(
		u"dd.MM HH:mm"_q);
	auto ids = std::vector<uint64>();
	for (auto i = 0; i != int(variants.size()); ++i) {
		const auto id = Data::GenerateSmsPromptId();
		Data::SaveSmsPrompt(
			id,
			u"Тест "_q + stamp + u" №"_q + QString::number(i + 1),
			variants[i]);
		ids.push_back(id);
	}
	auto form = Data::LoadSmsForm();
	if (form.saved) {
		for (const auto id : ids) {
			form.rotationPromptIds.push_back(id);
		}
		Data::SaveSmsForm(form);
		Ggtg::Notice(show, 
			u"Сохранено вариантов: "_q + QString::number(ids.size())
			+ u". Они в ротации «Рассылки»: включите там «Чередовать "
			"несколько шаблонов»."_q);
	} else {
		Ggtg::Notice(show, 
			u"Сохранено шаблонов: "_q + QString::number(ids.size())
			+ u". Выберите их в «Рассылке» через «Чередовать несколько "
			"шаблонов»."_q);
	}
}

} // namespace AiAgent

// A row of small buttons that wraps to the width of the window.
inline void AddAiButtons(
		not_null<Ui::GenericBox*> box,
		std::vector<std::pair<QString, Fn<void()>>> list) {
	const auto bar = box->addRow(
		object_ptr<Ui::RpWidget>(box),
		st::boxRowPadding);
	const auto buttons = std::make_shared<
		std::vector<not_null<Ui::RoundButton*>>>();
	for (auto &[text, go] : list) {
		const auto button = Ui::CreateChild<Ui::RoundButton>(
			bar,
			rpl::single(text),
			st::defaultLightButton);
		button->setClickedCallback(std::move(go));
		buttons->push_back(button);
	}
	bar->widthValue(
	) | rpl::on_next([=](int width) {
		const auto skip = st::defaultVerticalListSkip;
		auto x = 0;
		auto y = 0;
		auto rowHeight = 0;
		for (const auto &button : *buttons) {
			if (x > 0 && x + button->width() > width) {
				x = 0;
				y += rowHeight + skip;
			}
			button->moveToLeft(x, y);
			x += button->width() + skip;
			rowHeight = button->height();
		}
		bar->resize(width, y + rowHeight);
	}, bar->lifetime());
}

// The chat itself, in `box`: in a window of its own (`standalone`) or as the
// whole "ИИ" section of the GGTG window. When no key works yet, the chat
// starts with connecting a free service; a key pasted into the field is
// connected right here and never goes to a model.
inline void FillAiAgent(
		not_null<Ui::GenericBox*> box,
		bool standalone) {
	using namespace AiAgent;

	struct State {
		std::vector<Turn> turns;
		bool busy = false;
		int generation = 0;
		QStringList steps; // of the question being answered
		QString current; // the step happening right now
		QPointer<Ui::VerticalLayout> list;
		QPointer<Ui::FlatLabel> status;
		// The rounds and the tool steps call each other; kept here so
		// they live as long as the box.
		std::set<int> rated; // turns the user has rated already
		Fn<void(const QString &)> send;
		Fn<void(const QString &)> prefill; // text into the field
		Fn<void()> stopAll; // "Остановить": the answer, the training, the queue
		Fn<void()> newChat; // a clean conversation
		Fn<void(int, bool)> rate;
		Fn<void(int)> saveVariants;
		Fn<void(QJsonArray, int)> round;
		Fn<void(QJsonArray, std::shared_ptr<QJsonArray>, int, int)> tool;
	};
	const auto state = box->lifetime().make_state<State>();
	state->turns = Load().turns;

	// ---- the top: is it working, what it knows, the settings ----
	const auto head = box->addRow(
		object_ptr<Ggtg::CardBox>(box),
		st::boxRowPadding);
	const auto headInner = head->inner();
	const auto state_ = headInner->add(object_ptr<Ui::FlatLabel>(
		headInner,
		QString(),
		st::boxLabel));
	const auto knows = headInner->add(
		object_ptr<Ui::FlatLabel>(headInner, QString(), st::boxDividerLabel),
		style::margins(0, style::ConvertScale(2), 0, 0));
	const auto refreshKnows = [=] {
		namespace Knowledge = Core::GgtgAi::Knowledge;
		const auto settings = Core::GgtgAi::Load();
		auto services = QStringList();
		for (const auto &key : settings.keys) {
			const auto name = Core::GgtgAi::CloudProviderName(key.provider);
			if (Core::GgtgAi::CloudKeyUsable(key) && !services.contains(name)) {
				services.push_back(name);
			}
		}
		using Level = Core::GgtgAi::EngineLevel;
		const auto level = Core::GgtgAi::EngineLevelNow();
		state_->setText(!settings.enabled
			? u"ИИ выключен (включается в настройках)"_q
			: services.isEmpty()
			? u"Нужен бесплатный ключ: подключите сервис ниже"_q
			: (level == Level::Starting)
			? (u"Ключи отдыхают после лимита · "_q + services.join(u", "_q))
			: (u"Работает · "_q + services.join(u", "_q)));
		const auto profile = Core::GgtgAi::Profile::Read();
		auto parts = QStringList();
		parts.push_back(u"О вас: "_q + QString::number(profile.count())
			+ u" "_q + (profile.count() == 1 ? u"факт"_q : u"фактов"_q));
		parts.push_back(u"материалов: "_q
			+ QString::number(Knowledge::List().size()));
		parts.push_back(AiTrain::Running()
			? (u"учится: "_q + AiTrain::Status())
			: Core::GgtgAi::Train::Summary());
		knows->setText(parts.join(u" · "_q));
	};
	refreshKnows();
	const auto knowsTimer = Ui::CreateChild<QTimer>(knows);
	knowsTimer->setInterval(2000);
	QObject::connect(knowsTimer, &QTimer::timeout, knows, refreshKnows);
	knowsTimer->start();
	// "Остановить" is there only while the AI is doing something.
	using StopWrap = Ui::SlideWrap<Ui::RpWidget>;
	const auto stopWrap = headInner->add(object_ptr<StopWrap>(
		headInner,
		object_ptr<Ui::RpWidget>(headInner)));
	{
		const auto row = stopWrap->entity();
		const auto stop = Ui::CreateChild<Ui::RoundButton>(
			row,
			rpl::single(u"■ Остановить ИИ"_q),
			st::defaultActiveButton);
		stop->setClickedCallback([=] {
			if (state->stopAll) {
				state->stopAll();
			}
		});
		row->resize(row->width(), stop->height() + st::defaultVerticalListSkip);
		stop->moveToLeft(0, st::defaultVerticalListSkip);
	}
	const auto aiBusy = [=] {
		const auto &slot = Core::GgtgAi::details::SlotState();
		return state->busy
			|| AiTrain::Running()
			|| slot.active > 0
			|| !slot.queue.empty();
	};
	stopWrap->toggle(aiBusy(), anim::type::instant);
	const auto stopTimer = Ui::CreateChild<QTimer>(stopWrap);
	stopTimer->setInterval(700);
	QObject::connect(stopTimer, &QTimer::timeout, stopWrap, [=] {
		stopWrap->toggle(aiBusy(), anim::type::normal);
	});
	stopTimer->start();
	const auto openSettings = [=] {
		ShowAiSettingsBox(box->uiShow());
	};
	// Three doors, always in the same place: what it knows, how it is set
	// up, a clean page.
	AddCardRow(headInner, {
		{ u"Память"_q, [=] { ShowAiMemoryBox(box->uiShow()); } },
		{ u"Настройки"_q, openSettings },
		{ u"Новый чат"_q, [=] {
			if (state->newChat) {
				state->newChat();
			}
		} },
	});

	// ---- no key yet: connecting a service comes first ----
	using Wrap = Ui::SlideWrap<Ui::VerticalLayout>;
	const auto connectWrap = box->addRow(
		object_ptr<Wrap>(box, object_ptr<Ui::VerticalLayout>(box)),
		style::margins());
	{
		const auto card = connectWrap->entity()->add(
			object_ptr<Ggtg::CardBox>(connectWrap->entity()),
			style::margins(
				st::boxRowPadding.left(),
				st::defaultVerticalListSkip,
				st::boxRowPadding.right(),
				0));
		card->inner()->add(object_ptr<Ui::FlatLabel>(
			card->inner(),
			u"Привет! Я ваш ИИ-помощник. Чтобы я заработал, подключите "
				"бесплатный сервис - это минута, карта не нужна. Ключ можно "
				"и просто вставить в поле внизу."_q,
			st::boxLabel));
		Ui::AddSkip(card->inner());
		AiConnect::Fill(card->inner(), box->uiShow(), [=] {
			refreshKnows();
			connectWrap->toggle(false, anim::type::normal);
		});
	}
	connectWrap->toggle(
		!Core::GgtgAi::CloudReady(Core::GgtgAi::Load()),
		anim::type::instant);

	const auto say = [=](const QString &question) {
		if (state->send) {
			state->send(question);
		}
	};
	state->list = box->addRow(object_ptr<Ui::VerticalLayout>(box));
	// The question field and the steps stay at the bottom of the window,
	// the conversation scrolls above them.
	const auto bottom = box->setPinnedToBottomContent(
		object_ptr<Ui::VerticalLayout>(box));
	box->widthValue(
	) | rpl::filter([](int width) {
		return width > 0;
	}) | rpl::on_next([=](int width) {
		bottom->resizeToWidth(width);
	}, bottom->lifetime());
	state->status = bottom->add(
		object_ptr<Ui::FlatLabel>(bottom, QString(), st::boxDividerLabel),
		st::boxRowPadding);
	const auto field = bottom->add(
		object_ptr<Ui::InputField>(
			bottom,
			st::defaultInputField,
			Ui::InputField::Mode::MultiLine,
			rpl::single(u"Ваш вопрос… (Enter - отправить)"_q)),
		st::boxRowPadding);
	field->setSubmitSettings(Ui::InputField::SubmitSettings::Enter);
	box->setFocusCallback([=] { field->setFocusFast(); });
	state->prefill = [=](const QString &text) {
		field->setText(text);
		field->setFocus();
	};

	const auto padding = style::margins(
		st::boxRowPadding.left(),
		st::defaultVerticalListSkip,
		st::boxRowPadding.right(),
		st::defaultVerticalListSkip);
	const auto showStatus = [=] {
		const auto status = state->status.data();
		if (!status) {
			return;
		}
		if (!state->busy) {
			status->setText(QString());
			return;
		}
		auto lines = state->steps;
		lines.push_back(state->current.isEmpty()
			? u"● думает…"_q
			: (u"● "_q + state->current));
		status->setText(lines.join('\n'));
	};
	const auto render = [=] {
		const auto list = state->list.data();
		if (!list) {
			return;
		}
		list->clear();
		if (state->turns.empty()) {
			// A clean page: where to start, as cards.
			list->add(
				object_ptr<Ui::FlatLabel>(
					list,
					u"Чем помочь?"_q,
					st::boxTitle),
				style::margins(
					st::boxRowPadding.left(),
					st::defaultVerticalListSkip * 2,
					st::boxRowPadding.right(),
					st::defaultVerticalListSkip));
			const auto grid = list->add(
				object_ptr<Ggtg::SuggestGrid>(list),
				st::boxRowPadding);
			const auto ask = [=](const QString &text) {
				return [=] {
					if (state->send) {
						state->send(text);
					}
				};
			};
			grid->add(
				u"Расскажу о себе"_q,
				u"ИИ задаст пару вопросов о деле и запомнит"_q,
				ask(u"Расспроси меня о моём деле: задавай по одному "
					"короткому вопросу и запоминай ответы."_q));
			grid->add(
				u"Первое сообщение"_q,
				u"3 варианта текста для рассылки"_q,
				ask(u"Предложи 3 разных варианта первого сообщения для "
					"моей аудитории, чтобы я проверил их в рассылке рядом."_q));
			grid->add(
				u"Разбор рассылки"_q,
				u"Что сработало, что нет и что поменять"_q,
				ask(u"Разбери мою последнюю рассылку: что сработало, что "
					"нет и что изменить в следующий раз."_q));
			grid->add(
				u"Ответ на возражение"_q,
				u"Вставьте, что написал человек"_q,
				[=] {
					if (state->prefill) {
						state->prefill(u"Человек пишет: «»\nКак лучше "
							"ответить?"_q);
					}
				});
			grid->add(
				u"Дать материалы"_q,
				u"Прайс, сайт, PDF, видео - ИИ будет опираться на них"_q,
				[=] { ShowAiMaterialsBox(box->uiShow()); });
			grid->add(
				u"Учиться на моих чатах"_q,
				u"Ваш стиль и ответы на частые вопросы"_q,
				[=] { ShowAiTrainingBox(box->uiShow()); });
		}
		const auto from = std::max(0, int(state->turns.size()) - 16);
		for (auto i = from; i != int(state->turns.size()); ++i) {
			const auto &turn = state->turns[i];
			if (!turn.mine && !turn.steps.isEmpty()) {
				list->add(
					object_ptr<Ui::FlatLabel>(
						list,
						turn.steps.join('\n'),
						st::boxDividerLabel),
					padding);
			}
			auto text = TextWithEntities{ turn.text };
			TextUtilities::ParseEntities(text, TextParseLinks);
			list->add(
				object_ptr<AiBubble>(list, text, turn.mine),
				padding);
			if (turn.mine
				|| state->busy
				|| i + 1 != int(state->turns.size())
				|| turn.text.isEmpty()) {
				continue;
			}
			// Under the newest answer: what to do with it.
			const auto variants = VariantsOf(turn.text);
			if (!variants.empty()) {
				const auto save = list->add(
					object_ptr<Ui::LinkButton>(
						list,
						u"Сохранить варианты в рассылку ("_q
							+ QString::number(variants.size()) + ')'),
					padding);
				save->setClickedCallback([=] {
					state->saveVariants(i);
				});
			}
			if (!state->rated.contains(i)) {
				const auto good = list->add(
					object_ptr<Ui::LinkButton>(
						list,
						u"Хороший ответ (ИИ запомнит)"_q),
					padding);
				good->setClickedCallback([=] { state->rate(i, true); });
				const auto bad = list->add(
					object_ptr<Ui::LinkButton>(
						list,
						u"Плохой ответ (ИИ запомнит)"_q),
					padding);
				bad->setClickedCallback([=] { state->rate(i, false); });
			}
		}
	};
	render();
	// The newest lines are at the bottom, next to the field.
	const auto scrollDown = [=] {
		base::call_delayed(50, box, [=] {
			box->scrollToY(1'000'000);
		});
	};
	scrollDown();

	const auto saveTurns = [=] {
		auto saved = Load();
		saved.turns = state->turns;
		Save(saved);
	};
	const auto fail = [=](QString error) {
		state->busy = false;
		state->current = QString();
		if (const auto status = state->status.data()) {
			status->setText(state->steps.join('\n')
				+ (state->steps.isEmpty() ? QString() : u"\n"_q)
				+ u"не вышло: "_q + error);
		}
	};

	// What the last question said about the user's business goes into
	// the memory; the facts are added to the answer's steps.
	const auto learn = [=](int generation) {
		auto question = QString();
		for (auto i = int(state->turns.size()) - 1; i >= 0; --i) {
			if (state->turns[i].mine) {
				question = state->turns[i].text;
				break;
			}
		}
		Core::GgtgAi::Profile::LearnFromMessage(question, crl::guard(box, [=](
				Core::GgtgAi::Profile::Change change,
				QString) {
			const auto &facts = change.lines;
			if (facts.isEmpty()
				|| generation != state->generation
				|| state->turns.empty()
				|| state->turns.back().mine) {
				return;
			}
			for (const auto &fact : facts) {
				state->turns.back().steps.push_back(
					u"запомнил: "_q + fact);
			}
			saveTurns();
			render();
			scrollDown();
		}));
	};

	// The tools the model asked for, one after another; then the
	// conversation goes back to it.
	state->tool = [=](
			QJsonArray calls,
			std::shared_ptr<QJsonArray> messages,
			int index,
			int i) {
		const auto generation = state->generation;
		if (i >= calls.size()) {
			state->round(*messages, index + 1);
			return;
		}
		const auto call = calls.at(i).toObject();
		const auto function = call.value("function").toObject();
		const auto arguments = QJsonDocument::fromJson(
			function.value("arguments").toString().toUtf8()).object();
		RunTool(
			function.value("name").toString(),
			arguments,
			crl::guard(box, [=](QString step) {
				if (generation == state->generation) {
					state->current = step;
					showStatus();
				}
			}),
			crl::guard(box, [=](QString result, QString line) {
				if (generation != state->generation) {
					return;
				}
				if (!line.isEmpty()) {
					state->steps.push_back(line);
				}
				state->current = QString();
				showStatus();
				messages->push_back(QJsonObject{
					{ "role", "tool" },
					{ "tool_call_id", call.value("id").toString() },
					{ "content", result },
				});
				state->tool(calls, messages, index, i + 1);
			}));
	};

	// One round of the model: it answers in words (done), or asks for
	// tools. When the rounds run out it must answer with what it has.
	state->round = [=](QJsonArray messages, int index) {
		const auto generation = state->generation;
		const auto last = (index + 1 >= kMaxRounds);
		if (last) {
			messages.push_back(QJsonObject{
				{ "role", "user" },
				{ "content", u"Хватит искать: ответь сейчас по тому, что "
					"уже собрано."_q },
			});
		}
		FitContext(messages);
		state->current = QString();
		showStatus();
		Core::GgtgAi::AskWithTools(
			messages,
			last ? QJsonArray() : Tools(),
			crl::guard(box, [=](QJsonObject message, QString error) {
				if (generation != state->generation) {
					return;
				}
				if (!error.isEmpty()) {
					fail(error);
					return;
				}
				const auto calls = message.value("tool_calls").toArray();
				if (calls.isEmpty()) {
					state->turns.push_back({
						.mine = false,
						.text = CleanAnswer(
							message.value("content").toString()),
						.steps = state->steps,
					});
					state->busy = false;
					state->current = QString();
					state->steps.clear();
					saveTurns();
					render();
					showStatus();
					scrollDown();
					learn(generation);
					return;
				}
				auto next = std::make_shared<QJsonArray>(messages);
				next->push_back(message);
				state->tool(calls, next, index, 0);
			}),
			0.4,
			1400,
			Core::GgtgAi::Load().agentThink);
	};

	const auto ask = [=] {
		auto messages = QJsonArray{
			QJsonObject{
				{ "role", "system" },
				{ "content", SystemPrompt() },
			},
		};
		const auto from = std::max(
			0,
			int(state->turns.size()) - kHistoryTurns);
		for (auto i = from; i != int(state->turns.size()); ++i) {
			messages.push_back(QJsonObject{
				{ "role", state->turns[i].mine ? "user" : "assistant" },
				{ "content", Cut(state->turns[i].text, 1500) },
			});
		}
		++state->generation;
		state->busy = true;
		state->steps.clear();
		state->current = QString();
		state->round(messages, 0);
	};

	state->send = [=](const QString &raw) {
		const auto text = raw.trimmed();
		if (text.isEmpty()) {
			return;
		}
		// A key pasted into the field is connected here and never goes to
		// a model; the chat keeps only its tail.
		if (!text.contains(QChar(' '))
			&& !Core::GgtgAi::DetectProvider(text).isEmpty()) {
			const auto added = AiConnect::AddKey(text, QString());
			if (!added.error.isEmpty()) {
				Ggtg::Notice(box->uiShow(), added.error);
				return;
			}
			const auto name = Core::GgtgAi::CloudProviderName(added.provider);
			state->turns.push_back({
				.mine = true,
				.text = u"Ключ "_q + name + u" "_q
					+ Core::GgtgAi::ShortKey(added.key),
			});
			state->turns.push_back({
				.mine = false,
				.text = u"Проверяю ключ "_q + name + u"..."_q,
			});
			const auto index = int(state->turns.size()) - 1;
			saveTurns();
			render();
			scrollDown();
			Core::GgtgAi::TestKey(added.key, crl::guard(box, [=](
					bool ok,
					QString message) {
				if (index < int(state->turns.size())) {
					state->turns[index].text = ok
						? (u"Подключено: "_q + name + u". Теперь можно "
							"со мной говорить: расскажите о своём деле или "
							"нажмите «Расспроси меня»."_q)
						: (u"Ключ не подошёл: "_q + message);
				}
				saveTurns();
				render();
				refreshKnows();
				if (ok) {
					connectWrap->toggle(false, anim::type::normal);
				}
			}));
			return;
		}
		if (state->busy) {
			Ggtg::Notice(box->uiShow(), u"Агент ещё отвечает"_q);
			return;
		}
		state->turns.push_back({ .mine = true, .text = text });
		saveTurns();
		render();
		ask();
		scrollDown();
	};
	const auto submit = [=] {
		const auto text = field->getLastText().trimmed();
		if (text.isEmpty() || state->busy) {
			if (!text.isEmpty()) {
				Ggtg::Notice(box->uiShow(), u"Агент ещё отвечает"_q);
			}
			return;
		}
		field->setText(QString());
		state->send(text);
	};
	state->rate = [=](int index, bool good) {
		if (index <= 0
			|| index >= int(state->turns.size())
			|| !state->rated.insert(index).second) {
			return;
		}
		auto question = QString();
		for (auto i = index - 1; i >= 0; --i) {
			if (state->turns[i].mine) {
				question = state->turns[i].text;
				break;
			}
		}
		Ggtg::Notice(box->uiShow(), good
			? u"Спасибо! ИИ разбирает, что в ответе было хорошего..."_q
			: u"Понял. ИИ разбирает, чего делать не нужно..."_q);
		Ui::PostponeCall(box, [=] { render(); });
		RateAnswer(
			question,
			state->turns[index].text,
			good,
			crl::guard(box, [=](
					Core::GgtgAi::Profile::Change change,
					QString error) {
				Ggtg::Notice(box->uiShow(), !error.isEmpty()
					? (u"Оценка не записана: "_q + error)
					: change.added
					? (u"ИИ записал в профиль: "_q
						+ change.lines.front())
					: u"Оценка принята, нового для профиля нет."_q);
			}));
	};
	state->saveVariants = [=](int index) {
		if (index < 0 || index >= int(state->turns.size())) {
			return;
		}
		const auto variants = VariantsOf(state->turns[index].text);
		SaveVariants(box->uiShow(), variants);
	};
	state->stopAll = [=] {
		const auto wasTraining = AiTrain::Running();
		++state->generation;
		state->busy = false;
		state->steps.clear();
		state->current = QString();
		AiTrain::Stop();
		const auto dropped = Core::GgtgAi::StopQueued();
		render();
		showStatus();
		refreshKnows();
		box->uiShow()->showToast(u"ИИ остановлен"_q
			+ (wasTraining ? u": обучение на паузе"_q : QString())
			+ (dropped
				? (u", отменено запросов в очереди: "_q
					+ QString::number(dropped))
				: QString())
			+ u". Уже отправленные закончатся сами за секунды."_q);
	};
	field->submits(
	) | rpl::on_next(submit, field->lifetime());

	box->addButton(rpl::single(u"Отправить"_q), submit);
	state->newChat = [=] {
		++state->generation;
		state->busy = false;
		state->steps.clear();
		state->current = QString();
		state->turns.clear();
		saveTurns();
		render();
		showStatus();
	};
	// "+": everything that is done rather than asked, next to the field.
	const auto plusMenu = box->lifetime().make_state<
		base::unique_qptr<Ui::PopupMenu>>();
	box->addLeftButton(rpl::single(u"＋"_q), [=] {
		*plusMenu = base::make_unique_q<Ui::PopupMenu>(
			box,
			st::popupMenuWithIcons);
		const auto add = [&](
				const QString &text,
				Fn<void()> callback,
				const style::icon *icon) {
			(*plusMenu)->addAction(text, std::move(callback), icon);
		};
		const auto say = [=](const QString &text) {
			if (state->send) {
				state->send(text);
			}
		};
		add(u"Добавить материал"_q, [=] {
			ShowAiMaterialsBox(box->uiShow());
		}, &st::menuIconFile);
		add(u"Учиться на моих чатах"_q, [=] {
			ShowAiTrainingBox(box->uiShow());
		}, &st::menuIconDownload);
		add(u"Запомнить правило"_q, [=] {
			if (state->prefill) {
				state->prefill(u"Запомни: "_q);
			}
		}, &st::menuIconEdit);
		add(u"Расспроси меня"_q, [=] {
			say(u"Расспроси меня о моём деле: задавай по одному короткому "
				"вопросу и запоминай ответы."_q);
		}, &st::menuIconBot);
		(*plusMenu)->addSeparator();
		add(u"Разобрать последнюю рассылку"_q, [=] {
			say(u"Разбери мою последнюю рассылку: что сработало, что нет и "
				"что изменить в следующий раз."_q);
		}, &st::menuIconStats);
		add(u"Сравнить тексты (A/B)"_q, [=] {
			say(u"Сравни мои тексты, которые были в рассылке рядом: какой "
				"лучше и хватает ли данных для вывода?"_q);
		}, &st::menuIconStats);
		add(u"3 варианта первого сообщения"_q, [=] {
			say(u"Предложи 3 разных варианта первого сообщения для моей "
				"аудитории, чтобы я проверил их в рассылке рядом."_q);
		}, &st::menuIconSend);
		(*plusMenu)->popup(QCursor::pos());
	});
	if (standalone) {
		box->addButton(rpl::single(u"Закрыть"_q), [=] {
			box->closeBox();
		});
	}
}

inline void ShowAiAgent(std::shared_ptr<Ui::Show> show) {
	show->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"ИИ"_q));
		box->setWidth(st::boxWideWidth * 3 / 2);
		FillAiAgent(box, true);
	}));
}

} // namespace Settings
