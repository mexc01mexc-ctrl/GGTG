/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and exceptions, see https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "core/ggtg_ai_train.h"
#include "core/application.h"
#include "core/process_manager.h"
#include "apiwrap.h"
#include "base/call_delayed.h"
#include "base/random.h"
#include "base/weak_ptr.h"
#include "data/data_folder.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "dialogs/dialogs_indexed_list.h"
#include "dialogs/dialogs_main_list.h"
#include "dialogs/dialogs_row.h"
#include "history/history.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "mtproto/sender.h"

namespace Settings::AiTrain {

// "Обучение на ваших чатах": a long run in the background that reads the
// personal chats of the chosen accounts where the user wrote and teaches the
// AI from them (what is taken and where it goes: core/ggtg_ai_train.h). It
// can be left for the night: the run is a row in "Процессы", it waits out the
// limits of the free keys, it stops at once on request and goes on from the
// same chat next time - the chats already read are remembered.

namespace details {

constexpr auto kMaxMessages = 600; // read per chat, the newest ones
constexpr auto kPage = 100;
constexpr auto kMinMine = 2; // own messages a chat needs to be worth reading
constexpr auto kConsolidateEvery = 25; // chats

struct Run {
	std::shared_ptr<Core::ProcessHandle> process;
	std::vector<base::weak_ptr<Main::Account>> accounts;
	int accountIndex = 0;
	std::vector<PeerId> queue; // the chats of the current account
	size_t position = 0;
	int total = 0; // chats found so far
	int finished = 0; // chats gone through in this run
	int sinceConsolidation = 0;
	int waits = 0; // in a row, for the limits
	bool ended = false;
	QString status;
};

[[nodiscard]] inline std::shared_ptr<Run> &Active() {
	static auto result = std::shared_ptr<Run>();
	return result;
}

inline void Say(const std::shared_ptr<Run> &run, const QString &text) {
	run->status = text;
	if (run->process) {
		run->process->log(text);
	}
}

inline void Quietly(const std::shared_ptr<Run> &run, const QString &text) {
	run->status = text;
}

inline void NextAccount(const std::shared_ptr<Run> &run);
inline void NextChat(const std::shared_ptr<Run> &run);

// The run is over: what was collected is boiled down into the profile and
// the playbook goes into the "Материалы" base, then the process is closed.
inline void Finish(
		const std::shared_ptr<Run> &run,
		const QString &message,
		bool ok) {
	if (std::exchange(run->ended, true)) {
		return;
	}
	run->status = u"Подвожу итоги..."_q;
	const auto close = [=] {
		Core::GgtgAi::Train::SyncKnowledge();
		run->status = message;
		if (run->process) {
			if (ok) {
				run->process->succeed(message);
			} else {
				run->process->fail(message);
			}
		}
		if (Active() == run) {
			Active().reset();
		}
	};
	if (!Core::GgtgAi::Train::HasNotes()) {
		close();
		return;
	}
	Core::GgtgAi::Train::Consolidate([=](QString) {
		// A failure keeps the notes for the next run.
		close();
	});
}

// True when the run was ended (by "Остановить" or by itself) and the step
// that asks must not go on.
[[nodiscard]] inline bool Over(const std::shared_ptr<Run> &run) {
	return run->ended || Active() != run;
}

inline void After(
		const std::shared_ptr<Run> &run,
		crl::time delay,
		Fn<void()> next) {
	base::call_delayed(delay, &Core::App(), [=] {
		if (!Over(run)) {
			next();
		}
	});
}

inline void Pause(const std::shared_ptr<Run> &run, Fn<void()> next) {
	After(run, 500 + base::RandomIndex(1000), std::move(next));
}

// Waits `seconds`, looking at "Остановить" every ten.
inline void WaitFor(
		const std::shared_ptr<Run> &run,
		int seconds,
		Fn<void()> next) {
	if (seconds <= 0) {
		next();
		return;
	}
	After(run, std::min(seconds, 10) * 1000, [=] {
		WaitFor(run, seconds - 10, next);
	});
}

[[nodiscard]] inline QString AccountKey(not_null<Main::Account*> account) {
	return QString::number(account->session().user()->id.value);
}

[[nodiscard]] inline QString AccountTitle(not_null<Main::Account*> account) {
	const auto user = account->session().user();
	return user->name()
		+ (user->username().isEmpty()
			? QString()
			: (u" (@"_q + user->username() + ')'));
}

// The app loads the list of chats page by page: ask for more until it is all
// here (or stops growing).
inline void LoadDialogs(
		const std::shared_ptr<Run> &run,
		not_null<Main::Account*> account,
		Fn<void()> then) {
	const auto weak = base::make_weak(account.get());
	const auto lastSize = std::make_shared<int>(-1);
	const auto stable = std::make_shared<int>(0);
	const auto polls = std::make_shared<int>(0);
	const auto tick = std::make_shared<Fn<void()>>();
	*tick = [=] {
		const auto strong = weak.get();
		if (!strong) {
			then();
			return;
		}
		auto &session = strong->session();
		session.api().requestDialogs();
		const auto size = session.data().chatsList()->indexed()->size();
		*stable = (size == *lastSize) ? (*stable + 1) : 0;
		*lastSize = size;
		if (session.data().chatsListLoaded()
			|| *stable >= 6
			|| ++*polls > 400) {
			then();
			return;
		}
		Quietly(
			run,
			u"Загружаю список чатов: %1..."_q.arg(size));
		After(run, 2000, *tick);
	};
	(*tick)();
}

// The personal chats of the account that are not read yet: people only, no
// bots, no service accounts, no Saved Messages.
[[nodiscard]] inline std::vector<PeerId> Candidates(
		not_null<Main::Account*> account) {
	auto result = std::vector<PeerId>();
	const auto key = AccountKey(account);
	const auto &done = Core::GgtgAi::Train::Current().done;
	auto &data = account->session().data();
	const auto add = [&](auto chats) {
		for (const auto &row : chats->all()) {
			const auto history = row->history();
			if (!history) {
				continue;
			}
			const auto user = history->peer->asUser();
			if (!user
				|| user->isBot()
				|| user->isSelf()
				|| user->isSupport()
				|| user->isInaccessible()
				|| user->isServiceUser()
				|| user->isNotificationsUser()
				|| done.contains(key + ':'
					+ QString::number(user->id.value))) {
				continue;
			}
			result.push_back(user->id);
		}
	};
	add(data.chatsList()->indexed());
	if (const auto folder = data.folderLoaded(Data::Folder::kId)) {
		add(folder->chatsList()->indexed());
	}
	return result;
}

struct Fetched {
	std::vector<Core::GgtgAi::Train::Line> lines; // newest first
	int mine = 0;
	int offsetId = 0;
	int pages = 0;
	int floods = 0;
};

// The newest messages of a chat, page by page. `done(error)`.
inline void FetchPage(
		const std::shared_ptr<Run> &run,
		not_null<Main::Account*> account,
		not_null<PeerData*> peer,
		std::shared_ptr<Fetched> fetched,
		Fn<void(QString error)> done) {
	if (Over(run)) {
		return;
	}
	account->session().api().request(MTPmessages_GetHistory(
		peer->input(),
		MTP_int(fetched->offsetId), // offset_id
		MTP_int(0), // offset_date
		MTP_int(0), // add_offset
		MTP_int(kPage), // limit
		MTP_int(0), // max_id
		MTP_int(0), // min_id
		MTP_long(0) // hash
	)).done([=](const MTPmessages_Messages &result) {
		if (Over(run)) {
			return;
		}
		auto got = 0;
		auto oldest = 0;
		result.match([&](const MTPDmessages_messagesNotModified &) {
		}, [&](const auto &data) {
			for (const auto &message : data.vmessages().v) {
				message.match([&](const MTPDmessage &data) {
					++got;
					oldest = (oldest ? std::min(oldest, data.vid().v) : data.vid().v);
					auto text = qs(data.vmessage()).trimmed();
					if (text.isEmpty()) {
						if (!data.vmedia()) {
							return;
						}
						text = u"[вложение]"_q;
					}
					const auto mine = data.is_out();
					fetched->mine += (mine && text != u"[вложение]"_q) ? 1 : 0;
					fetched->lines.push_back({
						.mine = mine,
						.text = text,
						.date = qint64(data.vdate().v),
					});
				}, [&](const MTPDmessageService &data) {
					++got;
					oldest = (oldest ? std::min(oldest, data.vid().v) : data.vid().v);
				}, [&](const MTPDmessageEmpty &data) {
					++got;
					oldest = (oldest ? std::min(oldest, data.vid().v) : data.vid().v);
				});
			}
		});
		++fetched->pages;
		fetched->floods = 0;
		if (got < kPage
			|| !oldest
			|| int(fetched->lines.size()) >= kMaxMessages) {
			done(QString());
			return;
		}
		fetched->offsetId = oldest;
		After(run, 700 + base::RandomIndex(900), [=] {
			FetchPage(run, account, peer, fetched, done);
		});
	}).fail([=](const MTP::Error &error) {
		if (Over(run)) {
			return;
		}
		static const auto flood = QRegularExpression(
			u"^(?:FLOOD|FLOOD_PREMIUM)_WAIT_(\\d+)$"_q);
		const auto match = flood.match(error.type());
		if (match.hasMatch() && ++fetched->floods <= 3) {
			const auto seconds = std::min(
				std::max(1, match.captured(1).toInt()) + 2,
				3600);
			Say(
				run,
				u"Telegram просит подождать %1 с, жду..."_q.arg(seconds));
			WaitFor(run, seconds, [=] {
				FetchPage(run, account, peer, fetched, done);
			});
			return;
		}
		done(error.type());
	}).handleFloodErrors().send();
}

// Reads one chat with the model, waiting out the limits of the free keys.
// `done(insight)`: null when the chat was given up on.
inline void LearnChat(
		const std::shared_ptr<Run> &run,
		QStringList windows,
		int attempt,
		Fn<void(std::optional<Core::GgtgAi::Train::Insight>)> done) {
	Core::GgtgAi::Train::Learn(windows, [=](
			Core::GgtgAi::Train::Insight insight,
			QString error) {
		if (Over(run)) {
			return;
		}
		if (error.isEmpty()) {
			run->waits = 0;
			done(std::move(insight));
			return;
		}
		if (error.contains(u"нет ни одного ключа"_q)
			|| error.contains(u"ИИ выключен"_q)) {
			Finish(run, u"Остановлено: "_q + error, false);
			return;
		}
		if (error.contains(u"не ответила ни одна модель"_q)
			&& attempt >= 2) {
			Say(run, u"Чат пропущен: модели не смогли его разобрать"_q);
			done(std::nullopt);
			return;
		}
		if (++run->waits > 36) {
			Finish(
				run,
				u"Остановлено: бесплатные лимиты не вернулись за три часа. "
					"Нажмите «Продолжить» позже или добавьте ещё ключ."_q,
				false);
			return;
		}
		Say(
			run,
			u"Модели отдыхают после лимита, жду 5 минут (ключи в шаге "
				"«Ключи» можно добавить)..."_q);
		WaitFor(run, 300, [=] {
			LearnChat(run, windows, attempt + 1, done);
		});
	});
}

inline void MarkDone(
		const std::shared_ptr<Run> &run,
		not_null<Main::Account*> account,
		PeerId peerId) {
	auto &state = Core::GgtgAi::Train::Current();
	state.done.insert(AccountKey(account) + ':'
		+ QString::number(peerId.value));
	++run->finished;
	if (run->process) {
		run->process->progress(run->finished, std::max(run->total, 1));
	}
	Core::GgtgAi::Train::SaveState();
}

inline void NextChat(const std::shared_ptr<Run> &run) {
	if (Over(run)) {
		return;
	}
	if (run->position >= run->queue.size()) {
		++run->accountIndex;
		NextAccount(run);
		return;
	}
	const auto account = run->accounts[run->accountIndex].get();
	if (!account || !Main::AccountIsLive(account)) {
		Say(run, u"Аккаунт стал недоступен, перехожу к следующему"_q);
		++run->accountIndex;
		NextAccount(run);
		return;
	}
	const auto peerId = run->queue[run->position++];
	const auto peer = account->session().data().peerLoaded(peerId);
	if (!peer) {
		MarkDone(run, account, peerId);
		NextChat(run);
		return;
	}
	Quietly(
		run,
		u"Читаю чат %1 из %2: %3"_q
			.arg(run->finished + 1)
			.arg(run->total)
			.arg(peer->name()));
	const auto weak = base::make_weak(account);
	const auto fetched = std::make_shared<Fetched>();
	const auto next = [=] {
		Pause(run, [=] { NextChat(run); });
	};
	FetchPage(run, account, peer, fetched, [=](QString error) {
		const auto strong = weak.get();
		if (!strong) {
			next();
			return;
		}
		if (!error.isEmpty()) {
			// Not marked: the next run tries this chat again.
			Say(
				run,
				u"Чат «%1» не прочитался (%2), пропускаю"_q
					.arg(peer->name(), error));
			++run->finished;
			next();
			return;
		}
		auto lines = fetched->lines;
		std::reverse(lines.begin(), lines.end());
		if (fetched->mine < kMinMine) {
			++Core::GgtgAi::Train::Current().skipped;
			MarkDone(run, strong, peerId);
			next();
			return;
		}
		{
			const auto me = strong->session().user();
			const auto user = peer->asUser();
			Core::GgtgAi::Train::HideNames(lines, {
				peer->name(),
				user ? user->firstName : QString(),
				user ? user->lastName : QString(),
				user ? user->username() : QString(),
				me->firstName,
				me->lastName,
				me->username(),
			});
		}
		const auto windows = Core::GgtgAi::Train::Windows(lines);
		const auto messages = int(lines.size());
		LearnChat(run, windows, 0, [=](
				std::optional<Core::GgtgAi::Train::Insight> insight) {
			const auto strong = weak.get();
			if (insight && strong) {
				Core::GgtgAi::Train::Keep(*insight, messages);
				++run->sinceConsolidation;
			}
			if (strong) {
				MarkDone(run, strong, peerId);
			}
			if (run->sinceConsolidation >= kConsolidateEvery
				&& Core::GgtgAi::Train::HasNotes()) {
				run->sinceConsolidation = 0;
				Quietly(run, u"Свожу выводы в профиль..."_q);
				Core::GgtgAi::Train::Consolidate([=](QString error) {
					if (Over(run)) {
						return;
					}
					if (error.isEmpty()) {
						Core::GgtgAi::Train::SyncKnowledge();
					}
					next();
				});
				return;
			}
			next();
		});
	});
}

inline void NextAccount(const std::shared_ptr<Run> &run) {
	if (Over(run)) {
		return;
	}
	if (run->accountIndex >= int(run->accounts.size())) {
		const auto &state = Core::GgtgAi::Train::Current();
		Finish(
			run,
			u"Готово. Прочитано чатов за всё время: %1. Вопросов и "
				"возражений с вашими ответами: %2."_q
				.arg(state.chats)
				.arg(int(Core::GgtgAi::Train::Playbook().size())),
			true);
		return;
	}
	const auto account = run->accounts[run->accountIndex].get();
	if (!account || !Main::AccountIsLive(account)) {
		++run->accountIndex;
		NextAccount(run);
		return;
	}
	Say(run, u"Аккаунт %1: загружаю список чатов"_q.arg(AccountTitle(account)));
	const auto weak = base::make_weak(account);
	LoadDialogs(run, account, [=] {
		if (Over(run)) {
			return;
		}
		const auto strong = weak.get();
		if (!strong) {
			++run->accountIndex;
			NextAccount(run);
			return;
		}
		run->queue = Candidates(strong);
		run->position = 0;
		run->total += int(run->queue.size());
		Say(
			run,
			u"Аккаунт %1: личных чатов к чтению: %2"_q
				.arg(AccountTitle(strong))
				.arg(run->queue.size()));
		NextChat(run);
	});
}

} // namespace details

[[nodiscard]] inline bool Running() {
	return details::Active() != nullptr;
}

// What the run is doing now (empty when none).
[[nodiscard]] inline QString Status() {
	const auto &run = details::Active();
	return run ? run->status : QString();
}

// The accounts that can be learned from: live and signed in.
[[nodiscard]] inline std::vector<not_null<Main::Account*>> Accounts() {
	auto result = std::vector<not_null<Main::Account*>>();
	for (const auto &account : Core::App().domain().orderedAccounts()) {
		if (account->sessionExists() && Main::AccountIsLive(account)) {
			result.push_back(account);
		}
	}
	return result;
}

[[nodiscard]] inline QString AccountName(not_null<Main::Account*> account) {
	return details::AccountTitle(account);
}

// Starts (or, after a stop, goes on) with the accounts whose user ids are
// in the saved state. Returns an error text, empty when it started.
inline QString Start() {
	if (Running()) {
		return u"Обучение уже идёт."_q;
	}
	const auto settings = Core::GgtgAi::Load();
	if (!Core::GgtgAi::CloudReady(settings)) {
		return u"Нужен бесплатный ключ: шаг «1. Ключи»."_q;
	}
	auto &state = Core::GgtgAi::Train::Current();
	auto run = std::make_shared<details::Run>();
	for (const auto &account : Accounts()) {
		if (std::find(
				state.accounts.begin(),
				state.accounts.end(),
				qint64(account->session().user()->id.value))
			!= state.accounts.end()) {
			run->accounts.push_back(base::make_weak(account.get()));
		}
	}
	if (run->accounts.empty()) {
		return u"Отметьте хотя бы один аккаунт."_q;
	}
	if (!state.startedAt) {
		state.startedAt = QDateTime::currentSecsSinceEpoch();
		Core::GgtgAi::Train::SaveState();
	}
	run->process = Core::App().processes().start(
		u"Обучение ИИ на ваших чатах"_q);
	run->process->log(state.done.empty()
		? u"Начинаю: читаю личные чаты, где писали вы."_q
		: u"Продолжаю с того же места: уже прочитано чатов: %1."_q
			.arg(state.done.size()));
	details::Active() = run;
	details::NextAccount(run);
	return QString();
}

// Stops at once; what is read stays, the next start goes on from there.
inline void Stop() {
	if (const auto run = details::Active()) {
		details::Finish(
			run,
			u"Остановлено. Нажмите «Продолжить», чтобы идти дальше с того же "
				"места."_q,
			true);
	}
}

} // namespace Settings::AiTrain
