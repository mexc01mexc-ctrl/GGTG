/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and exceptions, see https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "settings/ggtg_ai_inbox.h"
#include "ui/layers/generic_box.h"
#include "ui/rp_widget.h"
#include "ui/ui_utility.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"
#include "window/ggtg_widgets.h"
#include "window/ggtg_home_widgets.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_widgets.h"

#include <QtCore/QTimer>

namespace Settings {

// "Лента ответов": one list for everyone who answered a mailing. For each
// person the AI shows the stage they are at and a reply in the user's style;
// the user decides: "Отправить", "Изменить" (the change teaches the AI) or
// "Пропустить". A person ready to buy is marked as a hot lead, the AI stays
// silent and the user is called. See settings/ggtg_ai_inbox.h.

namespace AiInboxUi {

// A row of link buttons under an item.
inline void AddButtons(
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

// Buttons at the bottom of a card: the first is the main one.
inline void AddCardButtons(
		not_null<Ui::VerticalLayout*> layout,
		std::vector<std::pair<QString, Fn<void()>>> buttons) {
	const auto row = layout->add(
		object_ptr<Ui::RpWidget>(layout),
		style::margins(0, st::defaultVerticalListSkip, 0, 0));
	auto list = std::make_shared<std::vector<Ui::RoundButton*>>();
	for (auto &[text, callback] : buttons) {
		const auto button = Ui::CreateChild<Ui::RoundButton>(
			row,
			rpl::single(text),
			list->empty()
				? st::defaultActiveButton
				: st::defaultLightButton);
		button->setClickedCallback(std::move(callback));
		list->push_back(button);
	}
	row->widthValue(
	) | rpl::on_next([=](int width) {
		const auto skip = st::defaultVerticalListSkip;
		auto x = 0;
		auto y = 0;
		auto rowHeight = 0;
		for (const auto button : *list) {
			if (x > 0 && x + button->width() > width) {
				x = 0;
				y += rowHeight + skip;
			}
			button->moveToLeft(x, y);
			x += button->width() + skip;
			rowHeight = button->height();
		}
		row->resize(width, y + rowHeight);
	}, row->lifetime());
}

// The reply in a window of its own, to be changed before sending.
inline void ShowEdit(
		std::shared_ptr<Ui::Show> show,
		const QString &key,
		Fn<void()> sent) {
	namespace Inbox = Core::GgtgAi::Inbox;
	const auto item = Inbox::Find(key);
	if (!item) {
		return;
	}
	const auto username = item->username;
	const auto original = item->draft;
	show->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"Ответ для @"_q + username));
		box->setWidth(st::boxWideWidth);
		const auto field = box->addRow(object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			Ui::InputField::Mode::MultiLine,
			rpl::single(u"Ваш ответ"_q),
			original));
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			u"Всё, что вы измените, ИИ запомнит: так он учится писать "
				"как вы."_q,
			st::boxDividerLabel));
		const auto send = [=] {
			const auto text = field->getLastText().trimmed();
			const auto error = AiInbox::Send(
				key,
				text,
				text.simplified() != original.simplified());
			if (!error.isEmpty()) {
				Ggtg::Notice(box->uiShow(), u"Не отправилось: "_q + error);
				return;
			}
			box->closeBox();
			if (sent) {
				sent();
			}
		};
		box->addButton(rpl::single(u"Отправить"_q), send);
		box->addButton(rpl::single(u"Отмена"_q), [=] { box->closeBox(); });
		field->setFocusFast();
	}));
}

} // namespace AiInboxUi

// The feed itself, in `box`: in a window of its own (`standalone`: buttons,
// and the replies are looked for as it opens) or inside another screen
// (the "Главная": a refresh row, nothing is asked of the AI until the user
// presses it - the background pass does that by itself).
inline void FillAiInbox(
		not_null<Ui::GenericBox*> box,
		bool standalone,
		Fn<void()> changed = nullptr) {
	{
		namespace Inbox = Core::GgtgAi::Inbox;
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			standalone
				? u"Все, кто ответил на рассылку. ИИ определяет, на каком "
					"этапе человек (вопрос, возражение, интерес, отказ), и "
					"пишет ответ в вашем стиле. Без вашего «Отправить» "
					"ничего не уходит. Если человек готов купить, ИИ молчит "
					"и зовёт вас."_q
				: u"Без вашего «Отправить» ничего не уходит."_q,
			st::boxDividerLabel));
		const auto settings = Core::GgtgAi::Load();
		const auto auto_ = box->addRow(object_ptr<Ui::Checkbox>(
			box,
			u"Готовить ответы сами (каждые 5 минут, пока открыта программа)"_q,
			settings.inboxAuto));
		auto_->checkedChanges(
		) | rpl::on_next([=](bool checked) {
			auto updated = Core::GgtgAi::Load();
			updated.inboxAuto = checked;
			if (checked) {
				updated.enabled = true;
			}
			Core::GgtgAi::Save(updated);
		}, auto_->lifetime());
		const auto keep = box->addRow(object_ptr<Ui::Checkbox>(
			box,
			u"Оставлять чаты непрочитанными после моих действий "
				"(пропуск, ответ из ленты)"_q,
			settings.keepUnread));
		keep->checkedChanges(
		) | rpl::on_next([=](bool checked) {
			auto updated = Core::GgtgAi::Load();
			updated.keepUnread = checked;
			Core::GgtgAi::Save(updated);
		}, keep->lifetime());
		const auto status = box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			QString(),
			st::boxDividerLabel));
		const auto wrap = box->addRow(
			object_ptr<Ui::VerticalLayout>(box),
			style::margins());
		const auto rebuild = std::make_shared<Fn<void()>>();
		const auto shown = std::make_shared<int>(-1);
		const auto showSkipped = std::make_shared<bool>(false);
		*rebuild = [=] {
			*shown = Inbox::Version();
			// The screen around the feed (its counters) follows too.
			if (changed) {
				Ui::PostponeCall(box, changed);
			}
			const Ggtg::ScrollKeeper keepScroll(wrap);
			wrap->clear();
			const auto again = [=] { Ui::PostponeCall(box, *rebuild); };

			// The autopilot: offered for a stage the user trusts, and the
			// stages it already answers by itself.
			const auto offer = Inbox::AutopilotOffer();
			if (!offer.isEmpty()) {
				Ui::AddSkip(wrap);
				wrap->add(
					object_ptr<Ui::FlatLabel>(
						wrap,
						u"На «%1» вы %2 раз подряд отправили ответ ИИ без "
						"правок. Пусть ИИ отвечает на такое сам? Каждый такой "
						"ответ уходит через 3 минуты, и до этого его можно "
						"отменить. Горячих лидов и отказы автопилот не "
						"трогает."_q
							.arg(Inbox::StageTitle(offer))
							.arg(Inbox::kAutopilotStreak),
						st::boxLabel),
					st::boxRowPadding);
				AiInboxUi::AddButtons(wrap, {
					{ u"Включить автопилот"_q, [=] {
						AiInbox::SetAutopilot(offer, true);
						again();
					} },
					{ u"Не сейчас"_q, [=] {
						Inbox::Remembered().streaks[offer] = 0;
						Inbox::Save();
						again();
					} },
				});
			}
			const auto autopilot = Inbox::Remembered().autopilot;
			if (!autopilot.isEmpty()) {
				auto titles = QStringList();
				auto buttons = std::vector<
					std::pair<QString, Fn<void()>>>();
				for (const auto &stage : autopilot) {
					titles.push_back(Inbox::StageTitle(stage));
					buttons.push_back({
						u"Выключить «"_q + Inbox::StageTitle(stage) + u"»"_q,
						[=] {
							AiInbox::SetAutopilot(stage, false);
							again();
						} });
				}
				wrap->add(
					object_ptr<Ui::FlatLabel>(
						wrap,
						u"Автопилот сам отвечает на: "_q
							+ titles.join(u", "_q),
						st::boxDividerLabel),
					st::boxRowPadding);
				AiInboxUi::AddButtons(wrap, std::move(buttons));
			}

			auto open = std::vector<Inbox::Item>();
			for (const auto &item : Inbox::Items()) {
				if (item.status == Inbox::Status::Pending
					|| item.status == Inbox::Status::Hot) {
					open.push_back(item);
				}
			}
			std::stable_sort(open.begin(), open.end(), [](
					const Inbox::Item &a,
					const Inbox::Item &b) {
				const auto hotA = (a.status == Inbox::Status::Hot);
				const auto hotB = (b.status == Inbox::Status::Hot);
				return (hotA != hotB) ? hotA : (a.lastAt > b.lastAt);
			});
			// The skipped ones are not lost: they wait here, one tap from
			// coming back.
			const auto addSkipped = [=] {
				auto skipped = std::vector<Inbox::Item>();
				for (const auto &item : Inbox::Items()) {
					if (item.status == Inbox::Status::Skipped) {
						skipped.push_back(item);
					}
				}
				if (skipped.empty()) {
					return;
				}
				std::stable_sort(skipped.begin(), skipped.end(), [](
						const Inbox::Item &a,
						const Inbox::Item &b) {
					return a.updated > b.updated;
				});
				Ui::AddSkip(wrap);
				AiInboxUi::AddButtons(wrap, {
					{ (*showSkipped ? u"▾ "_q : u"▸ "_q)
						+ u"Пропущенные ("_q
						+ QString::number(skipped.size()) + ')', [=] {
						*showSkipped = !*showSkipped;
						again();
					} },
				});
				if (!*showSkipped) {
					return;
				}
				for (const auto &item : skipped) {
					const auto key = item.key;
					const auto card = wrap->add(
						object_ptr<Ggtg::CardBox>(wrap),
						st::boxRowPadding);
					card->inner()->add(object_ptr<Ui::FlatLabel>(
						card,
						u"@"_q + item.username
							+ (item.name.isEmpty()
								? QString()
								: (u" ("_q + item.name + ')'))
							+ u" · "_q + Inbox::StageTitle(item.stage),
						st::boxLabel));
					card->inner()->add(object_ptr<Ui::FlatLabel>(
						card,
						QChar(0x00AB) + item.theirText.left(160)
							+ QChar(0x00BB),
						st::boxDividerLabel));
					AiInboxUi::AddCardButtons(card->inner(), {
						{ u"Вернуть в ответы"_q, [=] {
							AiInbox::Restore(key);
							again();
						} },
						{ u"Открыть чат"_q, [=] { AiInbox::OpenChat(key); } },
					});
					Ui::AddSkip(wrap, style::ConvertScale(8));
				}
			};
			if (open.empty()) {
				wrap->add(object_ptr<Ggtg::EmptyState>(
					wrap,
					u"Пока никто не ждёт ответа. Нажмите «Обновить»: лента "
					"ищет тех, кто ответил на рассылки, в чатах, которые "
					"уже загружены в программе."_q));
				addSkipped();
				return;
			}
			for (const auto &item : open) {
				const auto key = item.key;
				const auto hot = (item.status == Inbox::Status::Hot);
				// One card per person; a hot lead has a bright stripe.
				const auto card = wrap->add(
					object_ptr<Ggtg::CardBox>(
						wrap,
						style::margins(16, 12, 14, 12),
						hot
							? std::optional<QColor>(QColor(255, 120, 60))
							: std::nullopt),
					st::boxRowPadding);
				const auto inner = card->inner();
				Ui::AddSkip(wrap, style::ConvertScale(8));
				inner->add(
					object_ptr<Ui::FlatLabel>(
						wrap,
						(hot ? u"ГОРЯЧИЙ ЛИД · "_q : QString())
							+ u"@"_q + item.username
							+ (item.name.isEmpty()
								? QString()
								: (u" ("_q + item.name + ')'))
							+ u" · "_q + Inbox::StageTitle(item.stage)
							+ (item.sender.isEmpty()
								? QString()
								: (u" · от "_q + item.sender)),
						st::boxLabel));
				inner->add(
					object_ptr<Ui::FlatLabel>(
						wrap,
						QChar(0x00AB) + item.theirText.left(300)
							+ QChar(0x00BB),
						st::boxDividerLabel));
				if (hot) {
					inner->add(
						object_ptr<Ui::FlatLabel>(
							wrap,
							u"Человек готов купить. ИИ ему не отвечает: "
							"ответьте сами и быстро."_q,
							st::boxDividerLabel));
					AiInboxUi::AddCardButtons(inner, {
						{ u"Открыть чат"_q, [=] { AiInbox::OpenChat(key); } },
						{ u"Вернуть ИИ"_q, [=] {
							AiInbox::GiveBack(key);
							again();
						} },
						{ u"Пропустить"_q, [=] {
							AiInbox::Skip(key);
							box->uiShow()->showToast(
								u"Пропущено: @"_q + item.username
									+ u". Он ниже, в «Пропущенных»."_q);
							again();
						} },
					});
					continue;
				}
				const auto hasDraft = !item.draft.isEmpty();
				if (item.drafting) {
					inner->add(
						object_ptr<Ui::FlatLabel>(
							wrap,
							u"ИИ пишет ответ..."_q,
							st::boxDividerLabel));
				} else if (hasDraft) {
					const auto label = inner->add(
						object_ptr<Ui::FlatLabel>(
							wrap,
							item.draft,
							st::boxLabel));
					label->setSelectable(true);
					if (!item.warning.isEmpty()) {
						inner->add(
							object_ptr<Ui::FlatLabel>(
								wrap,
								u""_q + item.warning,
								st::boxDividerLabel));
					}
					if (item.autoAt > 0) {
						inner->add(
							object_ptr<Ui::FlatLabel>(
								wrap,
								u"Автопилот отправит это сам около "_q
									+ QDateTime::fromMSecsSinceEpoch(
										item.autoAt).toString(u"HH:mm:ss"_q),
								st::boxDividerLabel));
					}
				} else if (item.stage == u"refusal"_q) {
					inner->add(
						object_ptr<Ui::FlatLabel>(
							wrap,
							u"Человек отказался: лучше ему больше не "
							"писать."_q,
							st::boxDividerLabel));
				} else if (!item.stage.isEmpty()) {
					inner->add(
						object_ptr<Ui::FlatLabel>(
							wrap,
							u"ИИ не придумал ответ."_q,
							st::boxDividerLabel));
				}
				auto buttons = std::vector<
					std::pair<QString, Fn<void()>>>();
				if (item.autoAt > 0) {
					buttons.push_back({ u"Отменить автоотправку"_q, [=] {
						AiInbox::CancelAuto(key);
						again();
					} });
				}
				if (hasDraft && !item.drafting) {
					buttons.push_back({ u"Отправить"_q, [=] {
						const auto found = Inbox::Find(key);
						if (!found) {
							return;
						}
						const auto error = AiInbox::Send(
							key,
							found->draft,
							false);
						if (!error.isEmpty()) {
							Ggtg::Notice(
								box->uiShow(),
								u"Не отправилось: "_q + error);
						}
						again();
					} });
					buttons.push_back({ u"Изменить"_q, [=] {
						AiInboxUi::ShowEdit(box->uiShow(), key, again);
					} });
				} else if (!item.drafting) {
					buttons.push_back({
						(item.stage == u"refusal"_q)
							? u"Всё же ответить"_q
							: u"Составить ответ"_q,
						[=] {
							AiInbox::Draft(
								key,
								true,
								crl::guard(box, [=](QString error) {
									if (!error.isEmpty()) {
										Ggtg::Notice(
											box->uiShow(),
											u"Не вышло: "_q + error);
									}
									again();
								}));
						} });
				}
				buttons.push_back({ u"Пропустить"_q, [=] {
					AiInbox::Skip(key);
					box->uiShow()->showToast(
						u"Пропущено: @"_q + item.username
							+ u". Он ниже, в «Пропущенных»."_q);
					again();
				} });
				buttons.push_back({ u"Открыть чат"_q, [=] {
					AiInbox::OpenChat(key);
				} });
				AiInboxUi::AddCardButtons(inner, std::move(buttons));
			}
			addSkipped();
		};
		(*rebuild)();

		const auto refreshing = std::make_shared<bool>(false);
		const auto refresh = [=] {
			if (std::exchange(*refreshing, true)) {
				return;
			}
			status->setText(u"Ищу тех, кто ответил, и готовлю ответы..."_q);
			AiInbox::Refresh(crl::guard(box, [=](
					int drafted,
					QString error) {
				*refreshing = false;
				status->setText(error.isEmpty()
					? (drafted
						? (u"Готово: подготовлено ответов "_q
							+ QString::number(drafted))
						: u"Готово."_q)
					: (u"Остановилось: "_q + error));
				(*rebuild)();
			}));
		};
		if (standalone) {
			box->addLeftButton(rpl::single(u"Обновить"_q), refresh);
			box->addButton(rpl::single(u"Закрыть"_q), [=] { box->closeBox(); });
		} else {
			box->addRow(object_ptr<Ggtg::ActionRow>(
				box,
				rpl::single(u"Найти новые ответы и подготовить ответы ИИ"_q),
				QString(),
				&st::menuIconSend),
				st::boxRowPadding
			)->setClickedCallback(refresh);
		}

		// Drafts appear while the window is open: look at the store now
		// and then.
		const auto timer = Ui::CreateChild<QTimer>(box.get());
		timer->setInterval(1500);
		QObject::connect(timer, &QTimer::timeout, box, [=] {
			if (Inbox::Version() != *shown) {
				(*rebuild)();
			}
		});
		timer->start();
		if (standalone) {
			refresh();
		}
	}
}

inline void ShowAiInbox(std::shared_ptr<Ui::Show> show) {
	show->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"Лента ответов"_q));
		box->setWidth(st::boxWideWidth);
		FillAiInbox(box, true);
	}));
}

} // namespace Settings
