/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and exceptions, see https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "core/application.h"
#include "core/file_utilities.h"
#include "core/ggtg_chat_lists.h"
#include "core/process_manager.h"
#include "api/api_common.h"
#include "apiwrap.h"
#include "base/call_delayed.h"
#include "base/random.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "history/history.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "storage/localimageloader.h"
#include "storage/storage_media_prepare.h"
#include "ui/boxes/confirm_box.h"
#include "ui/chat/attach/attach_prepare.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"
#include "window/ggtg_widgets.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_widgets.h"

#include <QtCore/QTimer>

namespace Settings::ListSend {

// "Написать всему списку": one message (a text, or a photo / file with a
// caption) to every chat of a list of chats, each from the account the chat
// is in, with a random pause between the chats. It is a process of "Процессы":
// its window shows the progress and has "Остановить"; a stopped run is not
// continued, it is started again if needed.
//
// In the text, "[NAME]" (or "[NAME|друг]", the word after "|" is used when
// the person's name is not a name) is the person's first name, and
// "{привет|здравствуйте|добрый день}" is one of the variants, chosen anew for
// each person.

struct Params {
	QString text;
	QString file; // a photo, a video or any file; empty - a text only
	int pauseMin = 20; // seconds between two chats
	int pauseMax = 40;
};

namespace details {

struct Run {
	Params params;
	QString listName;
	std::vector<Core::GgtgLists::Entry> entries;
	size_t index = 0;
	int sent = 0;
	int failed = 0;
	bool ended = false;
	std::shared_ptr<Core::ProcessHandle> process;
	QString status;
};

[[nodiscard]] inline std::shared_ptr<Run> &Active() {
	static auto result = std::shared_ptr<Run>();
	return result;
}

[[nodiscard]] inline Main::Account *AccountOf(quint64 userId) {
	for (const auto &[index, account] : Core::App().domain().accounts()) {
		if (Main::AccountIsLive(account.get())
			&& account->sessionExists()
			&& account->session().user()->id.value == userId) {
			return account.get();
		}
	}
	return nullptr;
}

// The person's first name, letters only; empty when it is clearly not a name.
[[nodiscard]] inline QString FirstName(const QString &full) {
	static const auto word = QRegularExpression(u"\\p{L}[\\p{L}\\p{M}'’-]*"_q);
	const auto match = word.match(full);
	const auto name = match.hasMatch() ? match.captured(0) : QString();
	return (name.size() < 2) ? QString() : name;
}

// [NAME], [NAME|fallback] and {a|b|c} filled in for one person.
[[nodiscard]] inline QString Expand(QString text, const QString &fullName) {
	const auto name = FirstName(fullName);
	static const auto nameRe = QRegularExpression(
		u"\\[NAME(?:\\|([^\\]]*))?\\]"_q);
	auto result = QString();
	auto last = 0;
	auto it = nameRe.globalMatch(text);
	while (it.hasNext()) {
		const auto match = it.next();
		result += text.mid(last, match.capturedStart() - last);
		result += !name.isEmpty() ? name : match.captured(1);
		last = match.capturedEnd();
	}
	text = result + text.mid(last);
	static const auto choice = QRegularExpression(u"\\{([^{}]*\\|[^{}]*)\\}"_q);
	result.clear();
	last = 0;
	it = choice.globalMatch(text);
	while (it.hasNext()) {
		const auto match = it.next();
		const auto variants = match.captured(1).split(QChar('|'));
		result += text.mid(last, match.capturedStart() - last);
		result += variants[base::RandomIndex(int(variants.size()))].trimmed();
		last = match.capturedEnd();
	}
	return (result + text.mid(last)).trimmed();
}

inline void Say(const std::shared_ptr<Run> &run, const QString &line) {
	run->status = line;
	if (run->process) {
		run->process->log(line);
	}
}

inline void Finish(
		const std::shared_ptr<Run> &run,
		const QString &message,
		bool ok) {
	if (std::exchange(run->ended, true)) {
		return;
	}
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
}

// One chat of the list: the message goes out. Returns an error text, empty
// when it was handed over to sending.
[[nodiscard]] inline QString SendOne(
		const Params &params,
		const Core::GgtgLists::Entry &entry) {
	const auto account = AccountOf(entry.account);
	if (!account) {
		return u"аккаунт недоступен"_q;
	}
	auto &session = account->session();
	auto peer = session.data().peerLoaded(PeerId(entry.peer));
	if (!peer && !entry.username.isEmpty()) {
		peer = session.data().peerByUsername(entry.username);
	}
	if (!peer) {
		return u"чат не найден в аккаунте"_q;
	}
	const auto history = session.data().history(peer);
	const auto text = Expand(params.text, peer->name());
	if (params.file.isEmpty()) {
		if (text.isEmpty()) {
			return u"пустой текст"_q;
		}
		auto message = Api::MessageToSend(Api::SendAction(history));
		message.textWithTags = TextWithTags{ text };
		session.api().sendMessage(std::move(message));
		return QString();
	}
	auto list = Storage::PrepareMediaList(
		QStringList{ params.file },
		st::sendMediaPreviewSize,
		session.user()->isPremium());
	if (list.error != Ui::PreparedList::Error::None || list.files.empty()) {
		return u"файл не читается"_q;
	}
	const auto isFile = (list.files.front().type
		== Ui::PreparedFile::Type::File);
	list.files.front().caption = TextWithTags{ text };
	list.overrideSendImagesAsPhotos = true;
	session.api().sendFiles(
		std::move(list),
		isFile ? SendMediaType::File : SendMediaType::Photo,
		nullptr,
		Api::SendAction(history));
	return QString();
}

inline void Step(const std::shared_ptr<Run> &run) {
	if (run->ended || Active() != run) {
		return;
	}
	if (run->index >= run->entries.size()) {
		Finish(
			run,
			u"Готово. Отправлено: %1, не вышло: %2."_q
				.arg(run->sent)
				.arg(run->failed),
			true);
		return;
	}
	const auto entry = run->entries[run->index++];
	const auto who = entry.name.isEmpty()
		? (u"@"_q + entry.username)
		: entry.name;
	const auto error = SendOne(run->params, entry);
	if (error.isEmpty()) {
		++run->sent;
		Say(run, u""_q + who + u": отправлено"_q);
	} else {
		++run->failed;
		Say(run, u""_q + who + u": "_q + error);
	}
	if (run->process) {
		run->process->progress(int(run->index), int(run->entries.size()));
	}
	if (run->index >= run->entries.size()) {
		// Give the last upload a moment before the process is closed.
		base::call_delayed(3000, &Core::App(), [=] { Step(run); });
		return;
	}
	const auto seconds = run->params.pauseMin
		+ base::RandomIndex(
			std::max(run->params.pauseMax - run->params.pauseMin, 0) + 1);
	run->status = u"Пауза %1 с перед следующим (%2 из %3)"_q
		.arg(seconds)
		.arg(run->index + 1)
		.arg(run->entries.size());
	base::call_delayed(crl::time(seconds) * 1000, &Core::App(), [=] {
		Step(run);
	});
}

} // namespace details

[[nodiscard]] inline bool Running() {
	return details::Active() != nullptr;
}

inline void Stop() {
	if (const auto run = details::Active()) {
		details::Finish(
			run,
			u"Остановлено. Отправлено: %1 из %2."_q
				.arg(run->sent)
				.arg(run->entries.size()),
			true);
	}
}

// The live window of a run: where it is, and "Остановить".
inline void ShowProgress(std::shared_ptr<Ui::Show> show) {
	show->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"Рассылка по списку"_q));
		box->setWidth(st::boxWideWidth);
		const auto label = box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			QString(),
			st::boxLabel));
		const auto refresh = [=] {
			const auto run = details::Active();
			if (!run) {
				label->setText(u"Рассылка по списку не идёт. Итоги в "
					"«Процессах»."_q);
				return;
			}
			label->setText(
				u"Список «%1»: отправлено %2 из %3, не вышло %4.\n%5"_q
					.arg(run->listName)
					.arg(run->sent)
					.arg(run->entries.size())
					.arg(run->failed)
					.arg(run->status));
		};
		refresh();
		const auto timer = Ui::CreateChild<QTimer>(box.get());
		timer->setInterval(1000);
		QObject::connect(timer, &QTimer::timeout, box, refresh);
		timer->start();
		box->addButton(rpl::single(u"Остановить"_q), [=] {
			Stop();
			refresh();
		});
		Ggtg::AddProcessHideButton(box, [=](std::shared_ptr<Ui::Show> to) {
			ShowProgress(std::move(to));
		});
	}));
}

// Starts the run. Returns an error text, empty when it started.
inline QString Start(const QString &listId, const Params &params) {
	if (Running()) {
		return u"Другая рассылка по списку уже идёт: дождитесь её или "
			"остановите."_q;
	}
	const auto list = Core::GgtgLists::Find(listId);
	if (!list || list->entries.empty()) {
		return u"В списке нет чатов."_q;
	} else if (params.text.trimmed().isEmpty() && params.file.isEmpty()) {
		return u"Напишите текст или выберите файл."_q;
	}
	auto run = std::make_shared<details::Run>();
	run->params = params;
	run->listName = list->name;
	run->entries = list->entries;
	std::stable_sort(run->entries.begin(), run->entries.end(), [](
			const auto &a,
			const auto &b) {
		return a.at < b.at;
	});
	run->process = Core::App().processes().start(
		u"Рассылка по списку «"_q + list->name + u"»"_q,
		[] {
			// A click on the row in "Процессы" brings the live window back.
			const auto window = Core::App().activeWindow();
			if (const auto controller = window
				? window->sessionController()
				: nullptr) {
				ShowProgress(controller->uiShow());
			}
		});
	run->process->log(
		u"Получателей: %1, пауза %2-%3 с, %4."_q
			.arg(run->entries.size())
			.arg(params.pauseMin)
			.arg(params.pauseMax)
			.arg(params.file.isEmpty()
				? u"только текст"_q
				: (u"с файлом "_q + QFileInfo(params.file).fileName())));
	details::Active() = run;
	details::Step(run);
	return QString();
}

// The window where the message and the pauses are set.
inline void ShowSetup(
		std::shared_ptr<Ui::Show> show,
		const QString &listId) {
	const auto list = Core::GgtgLists::Find(listId);
	if (!list) {
		return;
	}
	const auto name = list->name;
	const auto count = int(list->entries.size());
	show->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"Написать всему списку «"_q + name + u"»"_q));
		// Wide, but never wider than the window it opens in.
		box->setWidth(std::clamp(
			show->toastParent()->width() - 2 * style::ConvertScale(24),
			style::ConvertScale(340),
			style::ConvertScale(640)));
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			u"Сообщение уйдёт каждому чату списка (%1) из того аккаунта, в "
			"котором этот чат. В тексте: [NAME] или [NAME|друг] - имя "
			"человека, {привет|здравствуйте} - случайный вариант для "
			"каждого."_q.arg(count),
			st::boxDividerLabel));
		const auto text = box->addRow(object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			Ui::InputField::Mode::MultiLine,
			rpl::single(u"Текст или подпись к файлу"_q),
			QString()));
		const auto file = std::make_shared<QString>();
		const auto fileLabel = box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			u"Файл не выбран (будет только текст)."_q,
			st::boxDividerLabel));
		const auto pick = box->addRow(object_ptr<Ui::LinkButton>(
			box,
			u"Выбрать фото или файл…"_q));
		pick->setClickedCallback([=] {
			FileDialog::GetOpenPath(
				box.get(),
				u"Выберите фото или файл"_q,
				FileDialog::AllFilesFilter(),
				crl::guard(box, [=](FileDialog::OpenResult &&result) {
					if (result.paths.isEmpty()) {
						return;
					}
					*file = result.paths.front();
					fileLabel->setText(
						u"Файл: "_q + QFileInfo(*file).fileName());
				}));
		});
		const auto clear = box->addRow(object_ptr<Ui::LinkButton>(
			box,
			u"Убрать файл"_q));
		clear->setClickedCallback([=] {
			file->clear();
			fileLabel->setText(u"Файл не выбран (будет только текст)."_q);
		});
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			u"Пауза между чатами, секунд (случайная от и до):"_q,
			st::boxLabel));
		const auto from = box->addRow(object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			rpl::single(u"От"_q),
			u"20"_q));
		const auto to = box->addRow(object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			rpl::single(u"До"_q),
			u"40"_q));
		const auto status = box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			QString(),
			st::boxDividerLabel));
		const auto params = [=]() -> std::optional<Params> {
			auto result = Params{
				.text = text->getLastText().trimmed(),
				.file = *file,
				.pauseMin = from->getLastText().trimmed().toInt(),
				.pauseMax = to->getLastText().trimmed().toInt(),
			};
			if (result.pauseMin < 3 || result.pauseMax < result.pauseMin) {
				status->setText(u"Пауза: от 3 секунд, и «до» не меньше "
					"«от»."_q);
				return std::nullopt;
			} else if (result.text.isEmpty() && result.file.isEmpty()) {
				status->setText(u"Напишите текст или выберите файл."_q);
				return std::nullopt;
			} else if (!result.file.isEmpty()
				&& result.text.size() > 1000) {
				status->setText(u"Подпись к файлу не длиннее 1000 "
					"символов."_q);
				return std::nullopt;
			}
			return result;
		};
		const auto refreshEstimate = [=] {
			const auto value = params();
			if (!value) {
				return;
			}
			const auto seconds = count
				* (value->pauseMin + value->pauseMax) / 2;
			status->setText(u"Получателей: %1. Примерно %2 мин."_q
				.arg(count)
				.arg((seconds + 59) / 60));
		};
		from->changes() | rpl::on_next(refreshEstimate, from->lifetime());
		to->changes() | rpl::on_next(refreshEstimate, to->lifetime());
		refreshEstimate();
		box->addButton(rpl::single(u"Начать"_q), [=] {
			const auto value = params();
			if (!value) {
				return;
			}
			box->uiShow()->show(Ui::MakeConfirmBox({
				.text = u"Отправить это сообщение %1 чатам списка «%2»? "
					"Остановить рассылку можно в любой момент."_q
						.arg(count)
						.arg(name),
				.confirmed = [=](Fn<void()> &&close) {
					close();
					const auto error = Start(listId, *value);
					if (!error.isEmpty()) {
						Ggtg::Notice(box->uiShow(), error);
						return;
					}
					const auto show = box->uiShow();
					box->closeBox();
					ShowProgress(show);
				},
				.confirmText = u"Отправить"_q,
			}));
		});
		box->addButton(rpl::single(u"Отмена"_q), [=] { box->closeBox(); });
		box->setFocusCallback([=] { text->setFocusFast(); });
	}));
}

} // namespace Settings::ListSend
