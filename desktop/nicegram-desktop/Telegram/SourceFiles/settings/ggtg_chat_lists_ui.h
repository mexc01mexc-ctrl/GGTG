/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and exceptions, see https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "core/application.h"
#include "core/ggtg_chat_lists.h"
#include "settings/ggtg_list_send.h"
#include "base/call_delayed.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "ui/boxes/confirm_box.h"
#include "ui/layers/generic_box.h"
#include "ui/rp_widget.h"
#include "ui/ui_utility.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/popup_menu.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"
#include "window/window_session_controller_link_info.h"
#include "window/ggtg_widgets.h"
#include "styles/style_boxes.h"
#include "styles/style_menu_icons.h"
#include "styles/style_layers.h"
#include "styles/style_widgets.h"

#include <QtCore/QTimer>

#include <map>
#include <set>

namespace Settings {

// "Списки чатов": the user's own lists of chats. A chat gets into a list from
// its right-click menu ("В список GGTG"), and the "Списки чатов" section of
// the GGTG window opens any listed chat in one click. The store is
// core/ggtg_chat_lists.h.

// A small window that asks for a name (a new list, a renamed one).
inline void ShowListNamePrompt(
		std::shared_ptr<Ui::Show> show,
		const QString &title,
		const QString &initial,
		Fn<void(QString name)> done) {
	show->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(title));
		const auto field = box->addRow(object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			rpl::single(u"Название списка (например, Заинтересованные)"_q),
			initial));
		const auto submit = [=] {
			const auto name = field->getLastText().trimmed();
			if (name.isEmpty()) {
				field->showError();
				return;
			}
			box->closeBox();
			done(name);
		};
		field->submits(
		) | rpl::on_next(submit, field->lifetime());
		box->addButton(rpl::single(u"Готово"_q), submit);
		box->addButton(rpl::single(u"Отмена"_q), [=] { box->closeBox(); });
		box->setFocusCallback([=] { field->setFocusFast(); });
	}));
}

// The chat in the account it was put from: the account is switched to when
// it is another one.
inline void OpenListedChat(const Core::GgtgLists::Entry &entry) {
	Main::Account *found = nullptr;
	for (const auto &[index, account] : Core::App().domain().accounts()) {
		if (Main::AccountIsLive(account.get())
			&& account->sessionExists()
			&& account->session().user()->id.value == entry.account) {
			found = account.get();
			break;
		}
	}
	const auto open = [=] {
		const auto window = Core::App().activeWindow();
		const auto controller = window ? window->sessionController() : nullptr;
		if (!controller) {
			return;
		}
		const auto peer = controller->session().data().peerLoaded(
			PeerId(entry.peer));
		if (peer) {
			controller->showPeerHistory(peer);
		} else if (!entry.username.isEmpty()) {
			controller->showPeerByLink(Window::PeerByLinkInfo{
				.usernameOrId = entry.username,
				.resolveType = Window::ResolveType::Mention,
			});
		} else {
			controller->showToast(u"Чат пока не найден в этом аккаунте"_q);
			return;
		}
		window->activate();
	};
	if (!found) {
		if (const auto window = Core::App().activeWindow()) {
			if (const auto controller = window->sessionController()) {
				controller->showToast(u"Аккаунт этого чата сейчас "
					"недоступен"_q);
			}
		}
		return;
	}
	const auto active = &Core::App().domain().active() == found;
	if (!active) {
		Core::App().domain().activate(found);
	}
	if (active) {
		open();
	} else {
		base::call_delayed(700, &Core::App(), open);
	}
}

// The submenu of a chat's right-click menu: every list (a check where the
// chat already is; a click puts it in or takes it out) and a new list.
inline void FillGgtgListsMenu(
		not_null<Ui::PopupMenu*> menu,
		not_null<Window::SessionController*> controller,
		not_null<PeerData*> peer) {
	namespace Lists = Core::GgtgLists;
	const auto account = quint64(peer->session().user()->id.value);
	const auto peerId = quint64(peer->id.value);
	const auto entry = [=] {
		return Lists::Entry{
			.account = account,
			.peer = peerId,
			.username = peer->username(),
			.name = peer->name(),
			.accountName = peer->session().user()->name(),
		};
	};
	const auto weak = base::make_weak(controller);
	for (const auto &list : Lists::Lists()) {
		const auto id = list.id;
		const auto name = list.name;
		const auto has = Lists::Has(id, account, peerId);
		menu->addAction((has ? u""_q : QString()) + name, [=] {
			if (has) {
				Lists::Remove(id, account, peerId);
			} else {
				Lists::Add(id, entry());
			}
			if (const auto strong = weak.get()) {
				strong->showToast(has
					? (u"Убран из списка «"_q + name + u"»"_q)
					: (u"Добавлен в список «"_q + name + u"»"_q));
			}
		});
	}
	if (!Lists::Lists().empty()) {
		menu->addSeparator();
	}
	menu->addAction(u"Новый список…"_q, [=] {
		const auto strong = weak.get();
		if (!strong) {
			return;
		}
		ShowListNamePrompt(
			strong->uiShow(),
			u"Новый список"_q,
			QString(),
			[=](QString name) {
				const auto id = Lists::Create(name);
				Lists::Add(id, entry());
				if (const auto again = weak.get()) {
					again->showToast(
						u"Список «"_q + name + u"» создан, чат добавлен"_q);
				}
			});
	});
}

namespace ChatListsUi {

// A row of link buttons.
inline void AddLinks(
		not_null<Ui::VerticalLayout*> layout,
		std::vector<std::pair<QString, Fn<void()>>> buttons) {
	const auto row = layout->add(
		object_ptr<Ui::RpWidget>(layout),
		style::margins(
			st::boxRowPadding.left(),
			0,
			st::boxRowPadding.right(),
			st::defaultVerticalListSkip));
	auto links = std::make_shared<std::vector<Ui::LinkButton*>>();
	for (auto &[text, callback] : buttons) {
		const auto link = Ui::CreateChild<Ui::LinkButton>(row, text);
		link->setClickedCallback(std::move(callback));
		links->push_back(link);
	}
	row->resize(row->width(), links->front()->height());
	row->widthValue(
	) | rpl::on_next([=](int) {
		auto x = 0;
		for (const auto link : *links) {
			link->moveToLeft(x, 0);
			x += link->width() + st::boxRowPadding.left();
		}
	}, row->lifetime());
}

} // namespace ChatListsUi

namespace ChatListsUi {

constexpr auto kPage = 100;

[[nodiscard]] inline bool Matches(
		const Core::GgtgLists::Entry &entry,
		const QString &query) {
	return query.isEmpty()
		|| entry.name.contains(query, Qt::CaseInsensitive)
		|| entry.username.contains(query, Qt::CaseInsensitive)
		|| entry.accountName.contains(query, Qt::CaseInsensitive);
}

[[nodiscard]] inline int CountEntriesForLiveAccounts(const Core::GgtgLists::List &list) {
	auto liveAccounts = base::flat_set<quint64>();
	for (const auto &[index, account] : Core::App().domain().accounts()) {
		if (Main::AccountIsLive(account.get()) && account->sessionExists()) {
			liveAccounts.insert(quint64(account->session().user()->id.value));
		}
	}
	auto count = 0;
	for (const auto &entry : list.entries) {
		if (liveAccounts.contains(entry.account)) {
			++count;
		}
	}
	return count;
}

} // namespace ChatListsUi

// One list of chats as its own window, like the open list of a base: the
// newest chat on top, a click opens the chat (in its account) and the window
// stays where it is - also when the account is switched - so the user goes on
// down the list. A chat can be taken out with the cross.
inline void ShowChatListBox(
		std::shared_ptr<Ui::Show> show,
		const QString &listId) {
	if (!Core::GgtgLists::Find(listId)) {
		return;
	}
	show->show(Box([=](not_null<Ui::GenericBox*> box) {
		namespace Lists = Core::GgtgLists;
		// A roomy window (a list may be long, the names must be read
		// whole), but never bigger than the window it opens in: a small
		// GGTG window gets a box that fits it instead of one that sticks
		// out of it.
		const auto outer = show->toastParent()->size();
		box->setWidth(std::clamp(
			outer.width() - 2 * style::ConvertScale(24),
			style::ConvertScale(340),
			style::ConvertScale(760)));
		box->setMinHeight(std::min(
			style::ConvertScale(620),
			std::max(
				outer.height() - style::ConvertScale(140),
				style::ConvertScale(260))));
		const auto title = [=] {
			const auto list = Lists::Find(listId);
			return list
				? (list->name + u" · "_q
					+ QString::number(ChatListsUi::CountEntriesForLiveAccounts(*list)))
				: QString();
		};
		const auto titleText = box->lifetime().make_state<
			rpl::variable<QString>>(title());
		box->setTitle(titleText->value());
		Ggtg::KeepOpenOnAccountSwitch(box);
		const auto search = box->addRow(object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			rpl::single(u"Найти чат по имени или @username"_q),
			QString()));
		const auto wrap = box->addRow(
			object_ptr<Ui::VerticalLayout>(box),
			style::margins());
		const auto limit = std::make_shared<int>(ChatListsUi::kPage);
		const auto shown = std::make_shared<int>(-1);
		const auto rebuild = std::make_shared<Fn<void()>>();
		*rebuild = [=] {
			const auto list = Lists::Find(listId);
			if (!list) {
				box->closeBox();
				return;
			}
			*shown = Lists::Version();
			*titleText = title();
			const Ggtg::ScrollKeeper keepScroll(wrap);
			wrap->clear();
			const auto again = [=] { Ui::PostponeCall(box, *rebuild); };
			const auto query = search->getLastText().trimmed();
			auto liveAccounts = base::flat_set<quint64>();
			for (const auto &[index, account] : Core::App().domain().accounts()) {
				if (Main::AccountIsLive(account.get()) && account->sessionExists()) {
					liveAccounts.insert(quint64(account->session().user()->id.value));
				}
			}
			auto entries = std::vector<Lists::Entry>();
			for (const auto &entry : list->entries) {
				if (liveAccounts.contains(entry.account) && ChatListsUi::Matches(entry, query)) {
					entries.push_back(entry);
				}
			}
			std::stable_sort(entries.begin(), entries.end(), [](
					const Lists::Entry &a,
					const Lists::Entry &b) {
				return a.at > b.at;
			});
			if (entries.empty()) {
				wrap->add(object_ptr<Ggtg::EmptyState>(
					wrap,
					query.isEmpty()
						? u"Список пуст. Правый клик по чату → «В список "
							"GGTG»."_q
						: u"Ничего не найдено."_q));
				return;
			}
			const auto count = std::min(*limit, int(entries.size()));
			for (auto i = 0; i != count; ++i) {
				const auto entry = entries[i];
				auto wrapOwned = object_ptr<Ui::RpWidget>(wrap);
				const auto row = wrapOwned.data();
				const auto name = entry.name.isEmpty()
					? (u"@"_q + entry.username)
					: entry.name;
				const auto subtitle = (entry.username.isEmpty()
						? QString()
						: (u"@"_q + entry.username + u" · "_q))
					+ entry.accountName
					+ u" · "_q
					+ QDateTime::fromSecsSinceEpoch(entry.at).toString(
						u"dd.MM.yyyy"_q);
				const auto info = Ui::CreateChild<Ggtg::InfoRow>(
					row,
					rpl::single(Ggtg::InfoRowData{
						.title = name,
						.subtitle = subtitle,
					}));
				const auto cross = Ui::CreateChild<Ggtg::RemoveButton>(row);
				const auto layout = [=] {
					const auto width = row->width();
					info->resize(
						std::max(width - cross->width(), 0),
						info->height());
					cross->move(
						width - cross->width(),
						(info->height() - cross->height()) / 2);
					row->resize(width, info->height());
				};
				row->widthValue(
				) | rpl::on_next([=](int) {
					layout();
				}, row->lifetime());
				info->heightValue(
				) | rpl::on_next([=](int) {
					layout();
				}, row->lifetime());
				// The chat opens in the main window; this window stays.
				info->setClickedCallback([=] {
					OpenListedChat(entry);
				});
				cross->setClickedCallback([=] {
					Lists::Remove(listId, entry.account, entry.peer);
					row->hide();
				});
				wrap->add(std::move(wrapOwned), st::boxRowPadding);
			}
			if (count < int(entries.size())) {
				const auto rest = int(entries.size()) - count;
				ChatListsUi::AddLinks(wrap, {
					{ u"Показать ещё %1 (осталось %2)"_q
						.arg(std::min(rest, ChatListsUi::kPage))
						.arg(rest), [=] {
						*limit += ChatListsUi::kPage;
						again();
					} },
				});
			}
		};
		(*rebuild)();
		search->changes(
		) | rpl::on_next([=] {
			*limit = ChatListsUi::kPage;
			(*rebuild)();
		}, search->lifetime());
		box->addButton(rpl::single(u"Написать всем"_q), [=] {
			if (ListSend::Running()) {
				ListSend::ShowProgress(box->uiShow());
			} else {
				ListSend::ShowSetup(box->uiShow(), listId);
			}
		});
		box->addLeftButton(rpl::single(u"Переименовать"_q), [=] {
			const auto list = Lists::Find(listId);
			if (!list) {
				return;
			}
			ShowListNamePrompt(
				box->uiShow(),
				u"Переименовать список"_q,
				list->name,
				[=](QString value) {
					Lists::Rename(listId, value);
				});
		});
		box->addButton(rpl::single(u"Удалить список"_q), [=] {
			const auto list = Lists::Find(listId);
			if (!list) {
				return;
			}
			box->uiShow()->show(Ui::MakeConfirmBox({
				.text = u"Удалить список «"_q + list->name
					+ u"»? Сами чаты не удалятся."_q,
				.confirmed = [=](Fn<void()> &&close) {
					close();
					Lists::Delete(listId);
					box->closeBox();
				},
				.confirmText = u"Удалить"_q,
			}));
		});
		Ggtg::AddCloseCross(box);
		// A chat put in or taken out from a chat's menu shows up here.
		const auto timer = Ui::CreateChild<QTimer>(box.get());
		timer->setInterval(1000);
		QObject::connect(timer, &QTimer::timeout, box, [=] {
			if (Lists::Version() != *shown) {
				(*rebuild)();
			}
		});
		timer->start();
	}), Ui::LayerOption::KeepOther);
}

// The "Списки чатов" section of the GGTG window: the lists, one row each; a
// click opens the list in a window of its own (ShowChatListBox).
[[nodiscard]] inline object_ptr<Ui::BoxContent> MakeChatListsBox() {
	return Box([=](not_null<Ui::GenericBox*> box) {
		namespace Lists = Core::GgtgLists;
		box->setWidth(st::boxWideWidth);
		box->setTitle(rpl::single(u"Списки чатов"_q));
		const auto wrap = box->addRow(
			object_ptr<Ui::VerticalLayout>(box),
			style::margins());
		const auto shown = std::make_shared<int>(-1);
		const auto rebuild = std::make_shared<Fn<void()>>();
		*rebuild = [=] {
			*shown = Lists::Version();
			const Ggtg::ScrollKeeper keepScroll(wrap);
			wrap->clear();
			if (Lists::Lists().empty()) {
				wrap->add(object_ptr<Ggtg::EmptyState>(
					wrap,
					u"Списков пока нет. Нажмите «Новый список» ниже."_q));
				return;
			}
			for (const auto &list : Lists::Lists()) {
				const auto id = list.id;
				const auto activeCount = ChatListsUi::CountEntriesForLiveAccounts(list);
				wrap->add(
					object_ptr<Ggtg::ActionRow>(
						wrap,
						rpl::single(list.name + u" · "_q
							+ QString::number(activeCount)),
						QString(),
						&st::menuIconChats),
					st::boxRowPadding
				)->setClickedCallback([=] {
					ShowChatListBox(box->uiShow(), id);
				});
			}
			Ui::AddSkip(wrap);
		};
		(*rebuild)();
		box->addButton(rpl::single(u"Новый список"_q), [=] {
			ShowListNamePrompt(
				box->uiShow(),
				u"Новый список"_q,
				QString(),
				[=](QString name) {
					const auto id = Lists::Create(name);
					(*rebuild)();
					if (!id.isEmpty()) {
						ShowChatListBox(box->uiShow(), id);
					}
				});
		});
		// Lists made or renamed elsewhere show up while this is open.
		const auto timer = Ui::CreateChild<QTimer>(box.get());
		timer->setInterval(1000);
		QObject::connect(timer, &QTimer::timeout, box, [=] {
			if (Lists::Version() != *shown) {
				(*rebuild)();
			}
		});
		timer->start();
	});
}

} // namespace Settings
