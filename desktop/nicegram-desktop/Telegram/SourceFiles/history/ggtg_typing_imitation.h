#pragma once

#include "api/api_common.h"
#include "api/api_send_progress.h"
#include "apiwrap.h"
#include "base/random.h"
#include "base/timer.h"
#include "base/weak_ptr.h"
#include "chat_helpers/compose/compose_show.h"
#include "data/data_changes.h"
#include "data/data_drafts.h"
#include "data/data_session.h"
#include "history/history.h"
#include "main/main_session.h"
#include "ui/toast/toast.h"
#include "lang/lang_instance.h"
#include <optional>
#include <vector>
#include <memory>
#include <algorithm>

namespace Ggtg {

class TypingImitation final {
public:
	static TypingImitation &Instance() {
		static TypingImitation instance;
		return instance;
	}

	void start(
			std::shared_ptr<ChatHelpers::Show> show,
			not_null<History*> history,
			MsgId repliesRootId,
			Api::MessageToSend message,
			TextWithTags textWithTags,
			Fn<void(const TextWithTags &)> onRestore = nullptr) {
		cancel(history->peer->id.value, false);

		const auto fullText = textWithTags.text;
		if (fullText.isEmpty()) {
			return;
		}

		auto task = std::make_unique<Task>();
		task->peerId = history->peer->id.value;
		task->history = history.get();
		task->session = &history->session();
		task->repliesRootId = repliesRootId;
		task->message.emplace(std::move(message));
		task->textWithTags = textWithTags;
		task->onRestore = std::move(onRestore);
		task->show = show;

		// Realistic typing duration based on character count:
		// ~60-80 ms per character, bounded between 2.0s and 25.0s
		const auto totalMs = std::clamp(
			int(fullText.size()) * 75 + int(base::RandomIndex(400)),
			2000,
			25000);
		task->remainingMs = totalMs;
		task->lastTypingSent = crl::now();

		// Initial MTProto typing action
		task->session->sendProgressManager().update(
			task->history,
			task->repliesRootId,
			Api::SendProgressType::Typing);

		// Show toast with countdown & info
		const auto isRu = Lang::GetInstance().id().startsWith(u"ru"_q);
		if (show) {
			const auto sec = (totalMs + 999) / 1000;
			const auto toastText = isRu
				? u"Имитация печати... (отправка через %1 с)"_q.arg(sec)
				: u"Typing imitation... (sending in %1 s)"_q.arg(sec);
			task->toast = show->showToast({
				.text = { toastText },
				.duration = crl::time(totalMs + 1000),
			});
		}

		// Timer fires every 500 ms to refresh MTProto typing and countdown
		task->timer.setCallback([this, peerId = task->peerId] {
			tick(peerId);
		});
		task->timer.callEach(500);

		_tasks.push_back(std::move(task));
	}

	void cancel(uint64 peerId, bool restoreText = true) {
		const auto it = std::find_if(_tasks.begin(), _tasks.end(), [&](const auto &t) {
			return t->peerId == peerId;
		});
		if (it == _tasks.end()) {
			return;
		}
		auto task = std::move(*it);
		_tasks.erase(it);

		task->timer.cancel();

		if (const auto session = task->session.get()) {
			session->sendProgressManager().update(
				task->history,
				task->repliesRootId,
				Api::SendProgressType::Typing,
				-1);
		}

		if (const auto toast = task->toast.get()) {
			toast->hideAnimated();
		}

		if (restoreText) {
			if (task->onRestore) {
				task->onRestore(task->textWithTags);
			} else if (task->history) {
				auto draft = std::make_unique<Data::Draft>();
				draft->textWithTags = task->textWithTags;
				task->history->setLocalDraft(std::move(draft));
			}
		}

		if (auto show = task->show.lock()) {
			const auto isRu = Lang::GetInstance().id().startsWith(u"ru"_q);
			show->showToast(isRu
				? u"Имитация печати отменена"_q
				: u"Typing imitation cancelled"_q);
		}
	}

	bool isTyping(uint64 peerId) const {
		return std::any_of(_tasks.begin(), _tasks.end(), [&](const auto &t) {
			return t->peerId == peerId;
		});
	}

private:
	struct Task {
		uint64 peerId = 0;
		History *history = nullptr;
		base::weak_ptr<Main::Session> session;
		MsgId repliesRootId = 0;
		std::optional<Api::MessageToSend> message;
		TextWithTags textWithTags;
		int remainingMs = 0;
		crl::time lastTypingSent = 0;
		base::Timer timer;
		base::weak_ptr<Ui::Toast::Instance> toast;
		std::weak_ptr<ChatHelpers::Show> show;
		Fn<void(const TextWithTags &)> onRestore;
	};

	void tick(uint64 peerId) {
		const auto it = std::find_if(_tasks.begin(), _tasks.end(), [&](const auto &t) {
			return t->peerId == peerId;
		});
		if (it == _tasks.end()) {
			return;
		}
		auto &task = *it;

		const auto session = task->session.get();
		if (!session) {
			task->timer.cancel();
			crl::on_main([this, peerId] {
				dropTask(peerId);
			});
			return;
		}

		task->remainingMs -= 500;

		// Keep MTProto typing active every ~3.5 seconds
		const auto now = crl::now();
		if (now - task->lastTypingSent >= 3500) {
			task->lastTypingSent = now;
			session->sendProgressManager().update(
				task->history,
				task->repliesRootId,
				Api::SendProgressType::Typing);
		}

		// Update toast countdown
		if (task->remainingMs > 0 && task->toast) {
			const auto sec = (task->remainingMs + 999) / 1000;
			const auto isRu = Lang::GetInstance().id().startsWith(u"ru"_q);
			const auto toastText = isRu
				? u"Имитация печати... (отправка через %1 с)"_q.arg(sec)
				: u"Typing imitation... (sending in %1 s)"_q.arg(sec);
			if (auto show = task->show.lock()) {
				task->toast = show->showToast({
					.text = { toastText },
					.duration = crl::time(task->remainingMs + 1000),
				});
			}
		}

		// Time is up? Stop timer and finish safely on next event loop tick
		if (task->remainingMs <= 0) {
			task->timer.cancel();
			crl::on_main([this, peerId] {
				finish(peerId);
			});
		}
	}

	void dropTask(uint64 peerId) {
		const auto it = std::find_if(_tasks.begin(), _tasks.end(), [&](const auto &t) {
			return t->peerId == peerId;
		});
		if (it != _tasks.end()) {
			_tasks.erase(it);
		}
	}

	void finish(uint64 peerId) {
		const auto it = std::find_if(_tasks.begin(), _tasks.end(), [&](const auto &t) {
			return t->peerId == peerId;
		});
		if (it == _tasks.end()) {
			return;
		}
		auto task = std::move(*it);
		_tasks.erase(it);

		task->timer.cancel();

		const auto session = task->session.get();
		if (!session) {
			return;
		}

		// Cancel typing progress indicator
		session->sendProgressManager().update(
			task->history,
			task->repliesRootId,
			Api::SendProgressType::Typing,
			-1);

		if (const auto toast = task->toast.get()) {
			toast->hideAnimated();
		}

		// Send message to server!
		if (task->message) {
			const auto nextLocalMessageId = session->data().nextLocalMessageId();
			session->api().sendMessage(std::move(*task->message), nextLocalMessageId);
			session->changes().historyUpdated(
				task->history,
				Data::HistoryUpdate::Flag::MessageSent);
		}

		if (auto show = task->show.lock()) {
			const auto isRu = Lang::GetInstance().id().startsWith(u"ru"_q);
			show->showToast(isRu
				? u"Сообщение отправлено"_q
				: u"Message sent"_q);
		}
	}

	std::vector<std::unique_ptr<Task>> _tasks;
};

} // namespace Ggtg
