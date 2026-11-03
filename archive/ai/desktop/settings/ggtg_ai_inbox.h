/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and exceptions, see https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "data/data_histories.h"

#include "core/ggtg_ai_inbox.h"
#include "core/ggtg_sms_stats.h"
#include "settings/ggtg_ai_chat.h"
#include "settings/ggtg_ai_train.h"
#include "data/data_changes.h"
#include "api/api_common.h"
#include "apiwrap.h"
#include "base/call_delayed.h"
#include "base/weak_ptr.h"
#include "core/application.h"
#include "data/data_session.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "mtproto/sender.h"
#include "window/window_controller.h"
#include "window/window_session_controller_link_info.h"

#include <set>

namespace Settings::AiInbox {

// The engine of "Лента ответов" (the window is settings/ggtg_ai_inbox_box.h):
//
// - Scan finds the people who answered a mailing and wait for a word: the
//   last message of the chat is theirs. It looks only at the people the
//   mailings wrote to, in the chats the app already knows.
// - Draft reads the conversation, names the stage the person is at (a
//   question, an objection, interest, a refusal, ready to buy) and writes a
//   reply in the user's style. A person ready to buy is not answered: the
//   item becomes a hot lead and the user is called.
// - Send puts the reply into the chat from the account that wrote to the
//   person. What the user changed before sending is a lesson for the AI.
//
// Nothing leaves without "Отправить".

namespace details {

[[nodiscard]] inline Main::Account *AccountOf(quint64 senderId) {
	for (const auto &[index, account] : Core::App().domain().accounts()) {
		if (Main::AccountIsLive(account.get())
			&& account->sessionExists()
			&& account->session().user()->id.value == senderId) {
			return account.get();
		}
	}
	return nullptr;
}

inline void Toast(const QString &text) {
	if (const auto window = Core::App().activeWindow()) {
		if (const auto controller = window->sessionController()) {
			controller->showToast(text);
		}
	}
}

[[nodiscard]] inline PeerData *PeerOf(
		not_null<Main::Account*> account,
		const QString &username) {
	return account->session().data().peerByUsername(username);
}

// After what the program did in a chat (a reply sent from the feed, a
// skipped answer) the chat is marked "unread" again, so the person stays
// visible in the account's own list of chats and is not lost. A moment later:
// sending reads the chat first.
inline void KeepUnread(
		not_null<Main::Account*> account,
		PeerData *peer,
		bool force) {
	if (!peer || !Core::GgtgAi::Load().keepUnread) {
		return;
	}
	const auto history = account->session().data().history(peer);
	if (!force && history->unreadCount() <= 0 && !history->unreadMark()) {
		return;
	}
	base::call_delayed(1500, account, [=] {
		if (Main::AccountIsLive(account)) {
			account->session().data().histories().changeDialogUnreadMark(
				history,
				true);
		}
	});
}

// A message to the user's own Saved Messages in the active account: the
// user's phone gets it as well.
inline void SendToSaved(const QString &text) {
	auto &account = Core::App().domain().active();
	if (!account.sessionExists()) {
		return;
	}
	auto &session = account.session();
	const auto history = session.data().history(session.user());
	auto message = Api::MessageToSend(Api::SendAction(history));
	message.textWithTags = TextWithTags{ text };
	session.api().sendMessage(std::move(message));
}

// A notification of the system itself (on a Mac), seen even when the app is
// hidden.
inline void SystemNotify(const QString &title, const QString &text) {
#ifdef Q_OS_MAC
	const auto escape = [](QString value) {
		value.replace(u"\\"_q, u"\\\\"_q);
		value.replace(u"\""_q, u"\\\""_q);
		return value;
	};
	QProcess::startDetached(u"/usr/bin/osascript"_q, QStringList{
		u"-e"_q,
		u"display notification \""_q + escape(text.left(200))
			+ u"\" with title \""_q + escape(title)
			+ u"\" sound name \"Glass\""_q,
	});
#else // Q_OS_MAC
	(void)title;
	(void)text;
#endif // Q_OS_MAC
}

// A lead turned hot: the user is called everywhere at once - in the app, by
// the system, and in Saved Messages (so the phone rings too).
inline void CallUser(const Core::GgtgAi::Inbox::Item &item) {
	const auto who = u"@"_q + item.username
		+ (item.name.isEmpty() ? QString() : (u" ("_q + item.name + ')'));
	Toast(u"Горячий лид: "_q + who + u" готов купить. Ответьте сами."_q);
	SystemNotify(
		u"Горячий лид"_q,
		who + u" готов купить: "_q + item.theirText.left(120));
	SendToSaved(u"Горячий лид (GGTG)\n"_q
		+ who + u", написал аккаунту "_q
		+ (item.sender.isEmpty() ? u"?"_q : item.sender) + u":\n«"_q
		+ item.theirText.left(400) + u"»\n\nИИ ему не отвечает: ответьте "
			"сами, пока горячо."_q);
}

// The user's answer worked (the person wrote back after it, or turned out
// ready to buy): it goes to the playbook with a win, so it is offered first
// next time. What the person wrote is kept without names.
inline void RecordWin(Core::GgtgAi::Inbox::Item &item, int wins) {
	if (item.sentText.isEmpty()) {
		return;
	}
	const auto what = Core::GgtgAi::details::PlainMarks(
		Core::GgtgAi::details::Mask(item.sentTheirs).text).left(220);
	const auto kind = (item.sentStage == u"objection"_q)
		? u"objection"_q
		: (item.sentStage == u"question"_q)
		? u"question"_q
		: u"message"_q;
	if (what.size() >= 6) {
		Core::GgtgAi::Train::AddWin(kind, what, item.sentText, wins);
	}
	item.sentText.clear();
}

// ---- the quality check: real cases, run again when models change ----

struct Case {
	QString theirs; // without names
	QString sent; // what the user sent in the end
	QString stage;
};

[[nodiscard]] inline QString CasesPath() {
	return Core::GgtgPaths::DbDir() + u"ai_eval.json"_q;
}

[[nodiscard]] inline QJsonObject ReadCases() {
	auto file = QFile(CasesPath());
	return file.open(QIODevice::ReadOnly)
		? QJsonDocument::fromJson(file.readAll()).object()
		: QJsonObject();
}

inline void WriteCases(const QJsonObject &object) {
	auto file = QSaveFile(CasesPath());
	if (file.open(QIODevice::WriteOnly)) {
		file.write(QJsonDocument(object).toJson(QJsonDocument::Compact));
		file.commit();
	}
}

inline void RecordCase(const Case &value) {
	if (value.theirs.size() < 4 || value.sent.size() < 4) {
		return;
	}
	auto object = ReadCases();
	auto cases = object.value("cases").toArray();
	cases.push_back(QJsonObject{
		{ "theirs", value.theirs },
		{ "sent", value.sent },
		{ "stage", value.stage },
	});
	while (cases.size() > 40) {
		cases.removeFirst();
	}
	object.insert("cases", cases);
	WriteCases(object);
}

} // namespace details

// Looks for the people who wait for an answer; returns how many new ones.
inline int Scan() {
	namespace Inbox = Core::GgtgAi::Inbox;
	const auto since = QDateTime::currentSecsSinceEpoch() - 60 * 86400;
	auto labels = std::map<QString, QString>();
	auto targets = std::set<std::pair<quint64, QString>>();
	for (const auto &a : Core::GgtgSmsStats::Load()) {
		if (a.outcome == u"sent"_q && a.at >= since && !a.recipient.isEmpty()) {
			targets.insert({ a.senderId, a.recipient.toLower() });
			labels[Inbox::KeyOf(a.senderId, a.recipient)] = a.sender;
		}
	}
	auto added = 0;
	for (const auto &[senderId, username] : targets) {
		const auto account = details::AccountOf(senderId);
		if (!account) {
			continue;
		}
		const auto peer = details::PeerOf(account, username);
		if (!peer) {
			continue;
		}
		const auto history = account->session().data().historyLoaded(peer->id);
		if (!history) {
			continue;
		}
		const auto last = history->lastMessage();
		if (!last || last->out() || last->isService()) {
			continue;
		}
		const auto key = Inbox::KeyOf(senderId, username);
		const auto lastId = qint64(last->id.bare);
		auto text = last->originalText().text.simplified();
		if (text.isEmpty()) {
			text = u"[вложение или стикер]"_q;
		}
		const auto now = qint64(QDateTime::currentSecsSinceEpoch());
		if (const auto item = Inbox::Find(key)) {
			if (item->lastId >= lastId) {
				continue; // This very message is already dealt with.
			}
			// (If they wrote back after the user's answer, whether it worked
			// is judged once their new message has its stage, see Draft.)
			item->autoAt = 0;
			item->lastId = lastId;
			item->lastAt = qint64(last->date());
			item->theirText = text.left(600);
			item->updated = now;
			if (item->status != Inbox::Status::Hot) {
				// A new message: the old stage and draft are out of date.
				item->status = Inbox::Status::Pending;
				item->stage.clear();
				item->draft.clear();
				item->warning.clear();
			}
		} else {
			Inbox::Items().push_back({
				.key = key,
				.senderId = senderId,
				.sender = labels[key],
				.username = username,
				.name = peer->name(),
				.lastId = lastId,
				.lastAt = qint64(last->date()),
				.theirText = text.left(600),
				.status = Inbox::Status::Pending,
				.updated = now,
			});
		}
		++added;
	}
	if (added) {
		Inbox::Save();
	}
	return added;
}

namespace details {

struct Parsed {
	QString stage;
	QString text;
};

[[nodiscard]] inline Parsed ParseDraft(const QString &result) {
	static const auto stage = QRegularExpression(
		u"(?:^|\\n)\\s*\\**ЭТАП\\**\\s*[:：]\\s*([^\\n]+)"_q);
	static const auto answer = QRegularExpression(
		u"(?:^|\\n)\\s*\\**ОТВЕТ\\**\\s*[:：]\\s*(.*)$"_q,
		QRegularExpression::DotMatchesEverythingOption);
	auto parsed = Parsed();
	if (const auto match = stage.match(result); match.hasMatch()) {
		parsed.stage = Core::GgtgAi::Inbox::StageFromWord(match.captured(1));
	}
	auto text = result.trimmed();
	if (const auto match = answer.match(result); match.hasMatch()) {
		text = match.captured(1).trimmed();
	} else if (!parsed.stage.isEmpty()) {
		text.clear();
	}
	if (text.size() > 2
		&& ((text.startsWith(QChar('"')) && text.endsWith(QChar('"')))
			|| (text.startsWith(QChar(0x00AB))
				&& text.endsWith(QChar(0x00BB))))) {
		text = text.mid(1, text.size() - 2).trimmed();
	}
	if (text.size() < 8 && text.toUpper().startsWith(u"НЕТ"_q)) {
		text.clear();
	}
	parsed.text = text;
	if (parsed.stage.isEmpty()) {
		parsed.stage = u"other"_q;
	}
	return parsed;
}

} // namespace details

// Reads the conversation and prepares the reply for item `key`. With
// `force` a reply is written even for a refusal. `done(error)`.
inline void Draft(
		const QString &key,
		bool force,
		Fn<void(QString error)> done) {
	namespace Inbox = Core::GgtgAi::Inbox;
	const auto item = Inbox::Find(key);
	if (!item
		|| item->drafting
		|| (item->status != Inbox::Status::Pending
			&& item->status != Inbox::Status::Hot)) {
		done(QString());
		return;
	}
	const auto account = details::AccountOf(item->senderId);
	const auto peer = account
		? details::PeerOf(account, item->username)
		: nullptr;
	if (!account || !peer) {
		done(u"аккаунт или чат недоступен"_q);
		return;
	}
	item->drafting = true;
	Inbox::Touch();
	const auto username = item->username;
	const auto name = peer->name();
	const auto weak = base::make_weak(account);
	const auto finish = [=](QString error) {
		if (const auto again = Inbox::Find(key)) {
			again->drafting = false;
		}
		Inbox::Touch();
		done(error);
	};
	account->session().api().request(MTPmessages_GetHistory(
		peer->input(),
		MTP_int(0), // offset_id
		MTP_int(0), // offset_date
		MTP_int(0), // add_offset
		MTP_int(25), // limit
		MTP_int(0), // max_id
		MTP_int(0), // min_id
		MTP_long(0) // hash
	)).done([=](const MTPmessages_Messages &result) {
		auto lines = std::vector<AiChat::Line>();
		result.match([&](const MTPDmessages_messagesNotModified &) {
		}, [&](const auto &data) {
			for (const auto &message : data.vmessages().v) {
				message.match([&](const MTPDmessage &data) {
					auto text = qs(data.vmessage()).trimmed();
					if (text.isEmpty()) {
						if (!data.vmedia()) {
							return;
						}
						text = u"[вложение]"_q;
					}
					if (text.size() > AiChat::kLineLength) {
						text = text.left(AiChat::kLineLength) + u"…"_q;
					}
					lines.push_back({
						.mine = data.is_out(),
						.who = data.is_out()
							? u"Я"_q
							: AiChat::DisplayName(name),
						.text = text,
						.date = TimeId(data.vdate().v),
					});
				}, [](const auto &) {
				});
			}
		});
		std::reverse(lines.begin(), lines.end());
		lines = AiChat::FitTail(std::move(lines));
		const auto strong = weak.get();
		if (lines.empty() || !strong) {
			finish(u"в чате нет сообщений"_q);
			return;
		}
		auto context = AiChat::Context();
		context.tail = lines;
		const auto me = strong->session().user();
		auto myName = me->firstName.trimmed();
		if (myName.isEmpty()) {
			myName = me->name().trimmed();
		}
		auto user = u"Собеседник: "_q + AiChat::DisplayName(name) + '\n'
			+ u"Username: @"_q + username + '\n'
			+ (myName.isEmpty()
				? QString()
				: (u"Моё имя (имя аккаунта пользователя): "_q + myName + '\n'));
		if (const auto note = Core::GgtgAi::Feedback::PersonNote(username);
				!note.isEmpty()) {
			user += note + '\n';
		}
		user += u"\nПереписка (старые сверху):\n"_q
			+ AiChat::TimeContext(lines) + '\n' + AiChat::Transcript(lines);
		const auto query = user.right(800);
		Core::GgtgAi::Profile::WarmFor(query, [=] {
			auto system = AiChat::SystemPrompt(
				AiChatMode::Write,
				false,
				AiChat::LanguageNote(context, QString(), name),
				query);
			system += u"\n\nВАЖНО: в этот раз формат ответа другой (он важнее "
				"формата выше):\n"
				"ЭТАП: <вопрос | возражение | интерес | отказ | готов купить "
				"| другое> - на каком этапе собеседник по его последнему "
				"сообщению.\n"
				"ОТВЕТ: <текст сообщения для отправки>\n"
				"Этап «готов купить»: человек сам хочет купить, оплатить, "
				"записаться, просит реквизиты, ссылку на оплату или цену, "
				"чтобы заплатить сейчас. Для этапов «готов купить» и «отказ» "
				"в ОТВЕТ напиши одно слово: НЕТ (отвечать за пользователя "
				"нельзя, он ответит сам или не будет отвечать)."_q;
			if (force) {
				system += u" Пользователь попросил подготовить ответ в любом "
					"случае, кроме этапа «готов купить»: для отказа напиши "
					"вежливый короткий ответ без давления."_q;
			}
			Core::GgtgAi::Ask(
				system,
				user,
				[=](QString result, QString error) {
					namespace Inbox = Core::GgtgAi::Inbox;
					if (!error.isEmpty()) {
						finish(error);
						return;
					}
					const auto item = Inbox::Find(key);
					if (!item) {
						finish(QString());
						return;
					}
					const auto parsed = details::ParseDraft(result);
					item->stage = parsed.stage;
					item->warning.clear();
					item->updated = QDateTime::currentSecsSinceEpoch();
					// The user's answer before this message worked if the
					// person went on (a question, interest, ready to buy),
					// and did not if they refused.
					if (parsed.stage == u"refusal"_q) {
						item->sentText.clear();
					} else if (parsed.stage != u"hot"_q) {
						details::RecordWin(*item, 1);
					}
					if (parsed.stage == u"hot"_q) {
						const auto already
							= (item->status == Inbox::Status::Hot);
						item->status = Inbox::Status::Hot;
						item->draft.clear();
						item->autoAt = 0;
						// The answer before it led here: a big win.
						details::RecordWin(*item, 3);
						if (!already) {
							details::CallUser(*item);
						}
					} else {
						item->status = Inbox::Status::Pending;
						item->draft = (parsed.stage == u"refusal"_q && !force)
							? QString()
							: parsed.text;
						if (!item->draft.isEmpty()) {
							const auto invented
								= Core::GgtgAi::UngroundedFacts(
									item->draft,
									user
										+ Core::GgtgAi::Profile::Block(4000)
										+ Core::GgtgAi::Knowledge::AllText());
							if (!invented.isEmpty()) {
								item->warning = u"Проверьте: этого нет в "
									"вашем профиле, материалах и "
									"переписке: "_q + invented.join(u", "_q);
							}
						}
						// The autopilot: a stage the user trusts goes out by
						// itself in a few minutes, unless cancelled.
						item->autoAt = (!item->draft.isEmpty()
								&& item->warning.isEmpty()
								&& Inbox::Remembered().autopilot.contains(
									parsed.stage))
							? (QDateTime::currentMSecsSinceEpoch()
								+ Inbox::kAutopilotDelay)
							: 0;
					}
					Inbox::Save();
					finish(QString());
				},
				0.6,
				600,
				Core::GgtgAi::Role::General,
				QStringList(),
				true); // replies to people: the cleverest model
		});
	}).fail([=](const MTP::Error &error) {
		finish(error.type());
	}).send();
}

// Scans, then drafts for the ones that have no stage yet (at most
// `kPerPass`, so that the free limits are not drained in one go).
// `done(drafted, error)`.
inline void Refresh(Fn<void(int drafted, QString error)> done) {
	namespace Inbox = Core::GgtgAi::Inbox;
	constexpr auto kPerPass = 12;
	static auto busy = false;
	if (std::exchange(busy, true)) {
		if (done) {
			done(0, QString());
		}
		return;
	}
	Scan();
	auto keys = std::make_shared<std::vector<QString>>();
	// A reply the model could not draft three times is left alone, so a
	// bad one does not eat the limits again every five minutes.
	static auto failures = std::map<QString, int>();
	for (const auto &item : Inbox::Items()) {
		if (item.status == Inbox::Status::Pending
			&& item.stage.isEmpty()
			&& !item.drafting
			&& failures[item.key] < 3
			&& int(keys->size()) < kPerPass) {
			keys->push_back(item.key);
		}
	}
	const auto drafted = std::make_shared<int>(0);
	const auto step = std::make_shared<Fn<void()>>();
	*step = [=] {
		if (keys->empty()) {
			busy = false;
			if (done) {
				done(*drafted, QString());
			}
			return;
		}
		const auto key = keys->back();
		keys->pop_back();
		Draft(key, false, [=](QString error) {
			if (!error.isEmpty()
				&& (error.contains(u"отдыхают"_q)
					|| error.contains(u"не ответила"_q)
					|| error.contains(u"нет ни одного ключа"_q)
					|| error.contains(u"выключен"_q))) {
				busy = false;
				if (done) {
					done(*drafted, error);
				}
				return;
			}
			if (error.isEmpty()) {
				++*drafted;
			} else {
				++failures[key];
			}
			(*step)();
		});
	};
	(*step)();
}

// Puts `text` into the chat, from the account that wrote to the person.
// `edited`: the user changed the AI's draft - the AI learns from it.
// Returns an error text, empty when sent.
inline QString Send(const QString &key, QString text, bool edited) {
	namespace Inbox = Core::GgtgAi::Inbox;
	text = text.trimmed();
	const auto item = Inbox::Find(key);
	if (!item) {
		return u"запись не найдена"_q;
	} else if (text.isEmpty()) {
		return u"ответ пустой"_q;
	}
	const auto account = details::AccountOf(item->senderId);
	const auto peer = account
		? details::PeerOf(account, item->username)
		: nullptr;
	if (!account || !peer) {
		return u"аккаунт или чат недоступен"_q;
	}
	const auto history = account->session().data().history(peer);
	// Sending reads the chat: the unread mark of the person is put back.
	details::KeepUnread(account, peer, false);
	auto message = Api::MessageToSend(Api::SendAction(history));
	message.textWithTags = TextWithTags{ text };
	account->session().api().sendMessage(std::move(message));
	if (edited && !item->draft.isEmpty()) {
		// Every change of the user's is a lesson: the pair is kept, the
		// words are one more sample of their style, and the AI looks at the
		// pairs as they come together.
		Core::GgtgAi::Feedback::RecordEdit(item->draft, text);
		Core::GgtgAi::AddStyleExample(text);
		Core::GgtgAi::Feedback::MaybeLearnFromEdits();
	}
	// What went out, to know later whether it worked; and how much the user
	// trusts the drafts of this stage.
	auto &streak = Inbox::Remembered().streaks[item->stage];
	streak = edited ? 0 : (streak + 1);
	details::RecordCase({
		.theirs = Core::GgtgAi::details::PlainMarks(
			Core::GgtgAi::details::Mask(item->theirText).text),
		.sent = text,
		.stage = item->stage,
	});
	item->sentText = text;
	item->sentStage = item->stage;
	item->sentTheirs = item->theirText;
	item->autoAt = 0;
	item->status = Inbox::Status::Sent;
	item->updated = QDateTime::currentSecsSinceEpoch();
	Inbox::Save();
	return QString();
}

// "Отменить" under a draft the autopilot was about to send: it waits for the
// user again, and the trust in this stage starts over.
inline void CancelAuto(const QString &key) {
	namespace Inbox = Core::GgtgAi::Inbox;
	if (const auto item = Inbox::Find(key)) {
		item->autoAt = 0;
		Inbox::Remembered().streaks[item->stage] = 0;
		Inbox::Save();
	}
}

inline void SetAutopilot(const QString &stage, bool on) {
	namespace Inbox = Core::GgtgAi::Inbox;
	auto &list = Inbox::Remembered().autopilot;
	list.removeAll(stage);
	if (on) {
		list.push_back(stage);
	} else {
		Inbox::Remembered().streaks[stage] = 0;
	}
	Inbox::Save();
}

inline void Skip(const QString &key) {
	namespace Inbox = Core::GgtgAi::Inbox;
	if (const auto item = Inbox::Find(key)) {
		item->status = Inbox::Status::Skipped;
		item->updated = QDateTime::currentSecsSinceEpoch();
		Inbox::Save();
		// The chat stays marked "unread" in its account: a skipped person
		// can be found there (and in the "Пропущенные" of the feed).
		if (const auto account = details::AccountOf(item->senderId)) {
			details::KeepUnread(
				account,
				details::PeerOf(account, item->username),
				true);
		}
	}
}

// A skipped person comes back to the list of those waiting for an answer.
inline void Restore(const QString &key) {
	namespace Inbox = Core::GgtgAi::Inbox;
	if (const auto item = Inbox::Find(key)) {
		item->status = Inbox::Status::Pending;
		item->updated = QDateTime::currentSecsSinceEpoch();
		Inbox::Save();
	}
}

// A hot lead goes back to the AI: it reads the person's last message anew.
inline void GiveBack(const QString &key) {
	namespace Inbox = Core::GgtgAi::Inbox;
	if (const auto item = Inbox::Find(key)) {
		item->status = Inbox::Status::Pending;
		item->stage.clear();
		item->draft.clear();
		Inbox::Save();
	}
}

// Opens the chat with the person in the account that wrote to them.
inline void OpenChat(const QString &key) {
	namespace Inbox = Core::GgtgAi::Inbox;
	const auto item = Inbox::Find(key);
	if (!item) {
		return;
	}
	const auto account = details::AccountOf(item->senderId);
	if (!account) {
		details::Toast(u"Аккаунт сейчас недоступен"_q);
		return;
	}
	const auto username = item->username;
	Core::App().domain().activate(account);
	base::call_delayed(700, &Core::App(), [=] {
		const auto window = Core::App().activeWindow();
		const auto now = window ? window->sessionController() : nullptr;
		if (!now) {
			return;
		}
		now->showPeerByLink(Window::PeerByLinkInfo{
			.usernameOrId = username,
			.resolveType = Window::ResolveType::Mention,
		});
		window->activate();
	});
}

// The quality check: the saved real cases (what a person wrote and what the
// user sent in the end) are answered again by today's models; the report
// says how often the stage matches and how close the replies are to the
// user's. A drop against the last check is pointed out: free models change.
inline void RunEval(Fn<void(QString report)> done) {
	const auto cases = details::ReadCases().value("cases").toArray();
	if (cases.size() < 3) {
		done(u"Мало примеров: они копятся сами, когда вы отправляете ответы "
			"из ленты (нужно хотя бы 3)."_q);
		return;
	}
	struct Run {
		std::vector<details::Case> cases;
		int index = 0;
		int stages = 0;
		double closeness = 0.;
		int answered = 0;
	};
	const auto run = std::make_shared<Run>();
	for (auto i = std::max(0, int(cases.size()) - 15); i != cases.size(); ++i) {
		const auto o = cases.at(i).toObject();
		run->cases.push_back({
			.theirs = o.value("theirs").toString(),
			.sent = o.value("sent").toString(),
			.stage = o.value("stage").toString(),
		});
	}
	const auto stems = [](const QString &text) {
		auto result = std::set<QString>();
		static const auto word = QRegularExpression(u"[\\p{L}\\p{N}]{3,}"_q);
		auto it = word.globalMatch(text.toLower());
		while (it.hasNext()) {
			result.insert(it.next().captured(0).left(5));
		}
		return result;
	};
	const auto step = std::make_shared<Fn<void()>>();
	*step = [=] {
		if (run->index == int(run->cases.size())) {
			const auto total = int(run->cases.size());
			const auto stagePercent = run->stages * 100 / std::max(total, 1);
			const auto closePercent = run->answered
				? int(run->closeness * 100. / run->answered)
				: 0;
			auto object = details::ReadCases();
			const auto before = object.value("lastStage").toInt(-1);
			object.insert("lastStage", stagePercent);
			object.insert("lastClose", closePercent);
			object.insert("lastAt", double(QDateTime::currentSecsSinceEpoch()));
			details::WriteCases(object);
			auto report = u"Проверено случаев: %1.\nЭтап определён так же, как "
				"было: %2%.\nБлизость ответов ИИ к вашим: %3% (от 30% - "
				"хорошо, от 50% - очень похоже на вас)."_q
				.arg(total)
				.arg(stagePercent)
				.arg(closePercent);
			if (before >= 0 && stagePercent + 10 < before) {
				report += u"\n\nХуже прошлой проверки (было %1%): возможно, "
					"сменились бесплатные модели. Добавьте ключ Gemini или "
					"Groq, если его нет."_q.arg(before);
			}
			done(report);
			return;
		}
		const auto value = run->cases[run->index++];
		Core::GgtgAi::Ask(
			u"Ты отвечаешь от имени пользователя человеку, который ответил на "
			"его рассылку в Telegram. Что известно о пользователе:\n"_q
				+ Core::GgtgAi::Profile::Block(1500, value.theirs)
				+ u"\n\nИз его материалов:\n"_q
				+ Core::GgtgAi::Knowledge::Block(1500, value.theirs)
				+ u"\n\nФормат ответа строго:\nЭТАП: <вопрос | возражение | "
					"интерес | отказ | готов купить | другое>\nОТВЕТ: <короткий "
					"ответ в стиле пользователя>"_q,
			u"Человек написал: "_q + value.theirs,
			[=](QString result, QString error) {
				if (error.isEmpty()) {
					const auto parsed = details::ParseDraft(result);
					run->stages += (parsed.stage == value.stage) ? 1 : 0;
					const auto a = stems(parsed.text);
					const auto b = stems(value.sent);
					auto common = 0;
					for (const auto &stem : a) {
						common += b.contains(stem) ? 1 : 0;
					}
					const auto all = int(a.size() + b.size()) - common;
					if (all > 0) {
						run->closeness += double(common) / all;
						++run->answered;
					}
				}
				(*step)();
			},
			0.4,
			400,
			Core::GgtgAi::Role::General,
			QStringList(),
			true);
	};
	(*step)();
}

namespace details {

// The morning brief: what waits, what is hot, what was learned.
[[nodiscard]] inline QString BriefText() {
	namespace Inbox = Core::GgtgAi::Inbox;
	auto hot = QStringList();
	auto pending = QStringList();
	for (const auto &item : Inbox::Items()) {
		const auto line = u"• @"_q + item.username + u": «"_q
			+ item.theirText.left(80) + u"»"_q;
		if (item.status == Inbox::Status::Hot) {
			hot.push_back(line);
		} else if (item.status == Inbox::Status::Pending) {
			pending.push_back(line + u" ("_q
				+ Inbox::StageTitle(item.stage) + ')');
		}
	}
	auto result = u"Доброе утро! Сводка GGTG на "_q
		+ QDate::currentDate().toString(u"dd.MM"_q) + u"\n\n"_q;
	if (!hot.isEmpty()) {
		result += u"Горячие лиды (ответьте сами):\n"_q
			+ hot.mid(0, 5).join('\n') + u"\n\n"_q;
	}
	result += pending.isEmpty()
		? u"Ждущих ответа нет.\n"_q
		: (u"Ждут ответа: %1\n"_q.arg(pending.size())
			+ pending.mid(0, 5).join('\n') + '\n');
	const auto offer = Inbox::AutopilotOffer();
	if (!offer.isEmpty()) {
		result += u"\nНа «%1» вы %2 раз подряд отправили ответ ИИ без "
			"правок: можно включить автопилот в «Ленте ответов».\n"_q
			.arg(Inbox::StageTitle(offer))
			.arg(Inbox::kAutopilotStreak);
	}
	const auto since = Inbox::Remembered().lastReview;
	const auto learned = int(Core::GgtgAi::Train::LearnedSince(since).size());
	if (learned) {
		result += u"\nИИ выучил новое: %1. Проверьте в разделе «ИИ» → «2. О "
			"бизнесе и цель» → «Проверить, что ИИ выучил».\n"_q.arg(learned);
	}
	result += u"\n"_q + Core::GgtgAi::Train::Summary() + '\n';
	result += u""_q + Core::GgtgAi::EngineStatusText();
	return result;
}

inline void MaybeBrief() {
	namespace Inbox = Core::GgtgAi::Inbox;
	const auto settings = Core::GgtgAi::Load();
	const auto now = QDateTime::currentDateTime();
	const auto today = now.date().toString(u"yyyy-MM-dd"_q);
	if (!settings.enabled
		|| !settings.briefDaily
		|| now.time().hour() < 8
		|| Inbox::Remembered().lastBrief == today) {
		return;
	}
	Inbox::Remembered().lastBrief = today;
	Inbox::Save();
	SendToSaved(BriefText());
}

// "Обучение на чатах" by itself at night, for the accounts the user chose;
// stopped in the morning so the day's limits stay for the replies.
inline void MaybeNight() {
	namespace Inbox = Core::GgtgAi::Inbox;
	static auto startedByNight = false;
	const auto settings = Core::GgtgAi::Load();
	const auto now = QDateTime::currentDateTime();
	const auto hour = now.time().hour();
	const auto today = now.date().toString(u"yyyy-MM-dd"_q);
	if (startedByNight && hour >= 8) {
		startedByNight = false;
		if (AiTrain::Running()) {
			AiTrain::Stop();
		}
		return;
	}
	if (!settings.trainNightly
		|| !Core::GgtgAi::CloudReady(settings)
		|| hour < 1
		|| hour >= 7
		|| AiTrain::Running()
		|| Core::GgtgAi::Train::Current().accounts.empty()
		|| Inbox::Remembered().lastNight == today) {
		return;
	}
	Inbox::Remembered().lastNight = today;
	Inbox::Save();
	if (AiTrain::Start().isEmpty()) {
		startedByNight = true;
	}
}

// The drafts the autopilot planned go out when their time comes.
inline void SendDue() {
	namespace Inbox = Core::GgtgAi::Inbox;
	const auto now = QDateTime::currentMSecsSinceEpoch();
	auto due = QStringList();
	for (const auto &item : Inbox::Items()) {
		if (item.autoAt > 0
			&& item.autoAt <= now
			&& item.status == Inbox::Status::Pending
			&& !item.draft.isEmpty()) {
			due.push_back(item.key);
		}
	}
	for (const auto &key : due) {
		if (const auto item = Inbox::Find(key)) {
			const auto draft = item->draft;
			if (!Send(key, draft, false).isEmpty()) {
				item->autoAt = 0;
			}
		}
	}
}

// A new message from a person somewhere: the feed looks at once (a few
// seconds later, all the news of a moment together).
inline void ScheduleRealtime() {
	static auto planned = false;
	if (std::exchange(planned, true)) {
		return;
	}
	QTimer::singleShot(4000, qApp, [] {
		planned = false;
		const auto settings = Core::GgtgAi::Load();
		if (settings.enabled
			&& settings.inboxAuto
			&& Core::GgtgAi::CloudReady(settings)) {
			Refresh(nullptr);
		} else if (Scan() > 0) {
			Core::GgtgAi::Inbox::Touch();
		}
	});
}

// Every signed-in account tells about its new incoming messages.
inline void Subscribe() {
	static auto subscribed = std::set<uint64>();
	for (const auto &account : Core::App().domain().orderedAccounts()) {
		if (!account->sessionExists()) {
			continue;
		}
		auto &session = account->session();
		const auto id = session.uniqueId();
		if (!subscribed.insert(id).second) {
			continue;
		}
		session.changes().messageUpdates(
			Data::MessageUpdate::Flag::NewAdded
		) | rpl::on_next([=](const Data::MessageUpdate &update) {
			const auto item = update.item;
			if (!item->out()
				&& !item->isService()
				&& item->history()->peer->isUser()) {
				ScheduleRealtime();
			}
		}, session.lifetime());
		session.lifetime().add([=] {
			subscribed.erase(id);
		});
	}
}

} // namespace details

// Starts what works by itself while the app is open: new answers are caught
// as they come, drafts are prepared (when the user wants it), the autopilot
// sends what it planned, the morning brief and the night training.
inline void StartBackground() {
	static auto started = false;
	if (std::exchange(started, true)) {
		return;
	}
	// What the earlier local version left (models, engine, up to 12 GB) is
	// deleted at once, quietly: nothing uses it any more.
	crl::async([] {
		if (Core::GgtgAi::LegacyLocalBytes() > 0) {
			Core::GgtgAi::CleanLegacyLocal();
		}
	});
	// The feed of answers and the morning brief are gone (the user did not
	// want them): only the night training is left to run by itself.
	const auto tick = [] {
		details::MaybeNight();
	};
	const auto timer = new QTimer(qApp);
	timer->setInterval(60 * 1000);
	QObject::connect(timer, &QTimer::timeout, timer, tick);
	timer->start();
	QTimer::singleShot(20 * 1000, qApp, tick);
}

} // namespace Settings::AiInbox
