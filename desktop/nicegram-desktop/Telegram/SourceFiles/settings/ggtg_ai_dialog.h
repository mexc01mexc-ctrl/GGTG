/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and exceptions, see https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "window/ggtg_widgets.h"
#include "core/ggtg_ai.h"
#include "base/call_delayed.h"
#include "core/ggtg_ai_profile.h"
#include "core/ggtg_paths.h"
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSaveFile>
#include "settings/ggtg_ai_profile_box.h"
#include "settings/ggtg_ai_cards.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"
#include "ui/vertical_list.h"
#include "ui/layers/show.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_widgets.h"

namespace Settings {

// The assistant is not set up by a prompt written by hand: it talks to the
// user. "ИИ расспросит меня" - it asks, one question at a time, about who the
// user is, what they offer and how they talk. "У меня замечания" - the user
// writes what they like, what to fix, advice. Whatever is said is boiled down
// into short rules that every later answer of the chat assistant follows
// (see Core::GgtgAi::Profile). From the talk a prompt can be written.
inline void ShowAiPromptProposal(
		std::shared_ptr<Ui::Show> show,
		const QString &proposal) {
	show->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"Промт из диалога"_q));
		box->setWidth(st::boxWideWidth);
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			u"Вот описание, которое ИИ составил по диалогу. Поправьте, что "
				"нужно, и сохраните: оно заменит ваш промт ИИ-помощника "
				"в чатах."_q,
			st::boxDividerLabel));
		const auto field = box->addRow(object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			Ui::InputField::Mode::MultiLine,
			rpl::single(u"Промт"_q),
			proposal));
		box->addButton(rpl::single(u"Сохранить как мой промт"_q), [=] {
			auto settings = Core::GgtgAi::Load();
			settings.chatPrompt = field->getLastText().trimmed();
			Core::GgtgAi::Save(settings);
			Ggtg::Notice(box->uiShow(), u"Промт сохранён"_q);
			box->closeBox();
		});
		box->addButton(rpl::single(u"Закрыть"_q), [=] {
			box->closeBox();
		});
	}));
}

// The conversation survives closing the window (switching the account
// closes it too): it is kept in a file and comes back as it was.
[[nodiscard]] inline QString AiDialogPath() {
	return Core::GgtgPaths::DbDir() + u"ai_dialog.json"_q;
}

inline void ShowAiDialog(
		std::shared_ptr<Ui::Show> show,
		bool interview) {
	show->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"Диалог с ИИ"_q));
		box->setWidth(st::boxWideWidth);

		struct Turn {
			bool mine = false;
			QString text;
		};
		struct State {
			std::vector<Turn> turns;
			bool interview = true;
			bool busy = false;
			bool learning = false;
			bool learnAgain = false;
			Fn<void()> learnRepeat;
			int generation = 0;
			QPointer<Ui::VerticalLayout> list;
			QPointer<Ui::FlatLabel> status;
			// How many turns already have their bubble, and the "ИИ
			// пишет" one while the answer is being made: the chat only
			// grows at the bottom, nothing is rebuilt and jumps.
			int shown = 0;
			QPointer<AiBubble> typing;
			Fn<void()> scrollDown;
			Fn<void()> ask;
		};
		const auto state = box->lifetime().make_state<State>();
		state->interview = interview;
		const auto saveTurns = [=] {
			auto array = QJsonArray();
			const auto from = std::max(0, int(state->turns.size()) - 60);
			for (auto i = from; i != int(state->turns.size()); ++i) {
				array.push_back(QJsonObject{
					{ "mine", state->turns[i].mine },
					{ "text", state->turns[i].text },
				});
			}
			auto file = QSaveFile(AiDialogPath());
			if (file.open(QIODevice::WriteOnly)) {
				file.write(QJsonDocument(array).toJson(
					QJsonDocument::Compact));
				file.commit();
			}
		};
		{
			auto file = QFile(AiDialogPath());
			if (file.open(QIODevice::ReadOnly)) {
				const auto array = QJsonDocument::fromJson(
					file.readAll()).array();
				for (const auto &value : array) {
					const auto object = value.toObject();
					const auto text = object.value("text").toString();
					if (!text.isEmpty()) {
						state->turns.push_back({
							.mine = object.value("mine").toBool(),
							.text = text,
						});
					}
				}
			}
		}

		const auto transcript = [=] {
			auto result = QString();
			const auto from = std::max(0, int(state->turns.size()) - 24);
			for (auto i = from; i != int(state->turns.size()); ++i) {
				result += (state->turns[i].mine ? u"Пользователь: "_q : u"ИИ: "_q)
					+ state->turns[i].text + '\n';
			}
			return result;
		};

		// Tabs and the hint stay on top, the field stays at the bottom:
		// only the conversation between them scrolls.
		const auto top = box->setPinnedToTopContent(
			object_ptr<Ui::VerticalLayout>(box));
		box->widthValue(
		) | rpl::filter([](int width) {
			return width > 0;
		}) | rpl::on_next([=](int width) {
			top->resizeToWidth(width);
		}, top->lifetime());
		const auto tabs = top->add(
			object_ptr<AiTabs>(
				top,
				QStringList{
					u"ИИ расспросит меня"_q,
					u"У меня замечания"_q,
				},
				interview ? 0 : 1),
			style::margins(
				st::boxRowPadding.left(),
				st::defaultVerticalListSkip,
				st::boxRowPadding.right(),
				st::defaultVerticalListSkip));
		const auto intro = top->add(
			object_ptr<Ui::FlatLabel>(top, QString(), st::boxDividerLabel),
			st::boxRowPadding);
		const auto describeMode = [=] {
			intro->setText(state->interview
				? u"ИИ сам задаст вопросы: кто вы, чем занимаетесь, чего "
					"хотите от диалогов, как говорите. Отвечайте своими "
					"словами. Всё важное он запомнит."_q
				: u"Напишите, что нравится в ответах ИИ, что исправить, "
					"какие дать советы или запреты. ИИ запомнит это "
					"и будет учитывать в каждом ответе."_q);
		};
		describeMode();
		const auto memory = top->add(
			object_ptr<Ui::LinkButton>(
				top,
				u"Что ИИ уже знает о вас ▸"_q),
			st::boxRowPadding);
		memory->setClickedCallback([=] {
			ShowAiProfile(box->uiShow());
		});
		const auto restart = top->add(
			object_ptr<Ui::LinkButton>(top, u"Начать разговор заново"_q),
			st::boxRowPadding);
		restart->setClickedCallback([=] {
			if (state->busy) {
				return;
			}
			state->turns.clear();
			state->shown = 0;
			saveTurns();
			if (const auto list = state->list.data()) {
				list->clear();
			}
			if (state->interview && state->ask) {
				state->ask();
			}
		});
		Ui::AddSkip(top);

		const auto column = box->addRow(
			object_ptr<Ui::VerticalLayout>(box),
			style::margins());
		state->list = column->add(object_ptr<Ui::VerticalLayout>(column));
		// The conversation area has a comfortable height from the start,
		// not one that grows message by message.
		const auto filler = column->add(object_ptr<Ui::RpWidget>(column));
		state->list->heightValue(
		) | rpl::on_next([=](int height) {
			filler->resize(
				filler->width(),
				std::max(0, style::ConvertScale(300) - height));
		}, filler->lifetime());
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
				rpl::single(u"Ваш ответ или замечание… (Enter - отправить)"_q)),
			st::boxRowPadding);
		field->setSubmitSettings(Ui::InputField::SubmitSettings::Enter);
		box->setFocusCallback([=] { field->setFocusFast(); });

		state->scrollDown = [=] {
			base::call_delayed(40, box, [=] {
				box->scrollToY(1'000'000);
			});
		};

		const auto render = [=] {
			const auto list = state->list.data();
			if (!list) {
				return;
			}
			if (const auto typing = state->typing.data()) {
				delete typing;
			}
			const auto margins = style::margins(
				st::boxRowPadding.left(),
				st::defaultVerticalListSkip,
				st::boxRowPadding.right(),
				st::defaultVerticalListSkip);
			for (; state->shown < int(state->turns.size());
					++state->shown) {
				const auto &turn = state->turns[state->shown];
				list->add(
					object_ptr<AiBubble>(
						list,
						TextWithEntities{ turn.text },
						turn.mine),
					margins);
			}
			if (state->busy) {
				state->typing = list->add(
					object_ptr<AiBubble>(
						list,
						TextWithEntities{ u"ИИ пишет…"_q },
						false),
					margins);
			}
			state->scrollDown();
		};

		// Brings the rules up to date after a round; one update at a time, a
		// round that came meanwhile is picked up by a repeat.
		const auto learn = [=] {
			if (state->learning) {
				state->learnAgain = true;
				return;
			}
			state->learning = true;
			Core::GgtgAi::Profile::LearnFromDialog(
				transcript(),
				crl::guard(box, [=](
						Core::GgtgAi::Profile::Change,
						QString) {
					state->learning = false;
					if (std::exchange(state->learnAgain, false)) {
						state->learnRepeat();
					}
				}));
		};
		state->learnRepeat = learn;

		// One round: the AI answers (asks the next question, or confirms what
		// it understood), then the rules are brought up to date.
		const auto answer = [=] {
			const auto generation = ++state->generation;
			state->busy = true;
			render();
			if (const auto status = state->status.data()) {
				status->setText(u"ИИ думает (первый раз модели нужно "
					"несколько секунд на запуск)..."_q);
			}
			const auto system = state->interview
				? u"Ты знакомишься с пользователем, чтобы потом писать "
					"сообщения от его имени в Telegram (переписки с "
					"клиентами, рассылки). Имя НЕ спрашивай и не записывай: "
					"у пользователя несколько аккаунтов, имя берётся из "
					"аккаунта, с которого идёт переписка. Узнай по очереди: "
					"чем он занимается; что предлагает; какая цель у диалогов и "
					"кто его собеседники; как он говорит (стиль, «ты» или "
					"«вы», длина сообщений, эмодзи); что можно говорить "
					"(цены, ссылки, условия) и чего нельзя; куда вести "
					"разговор. Задавай ОДИН короткий вопрос за раз, "
					"по-русски, без нумерации и без списков. Учитывай уже "
					"сказанное, не повторяйся. Когда всё главное "
					"выяснено, скажи, что можно нажать «Составить промт»."_q
				: u"Пользователь даёт замечания, советы и пожелания к "
					"ответам помощника, который пишет за него в Telegram. "
					"Коротко (1-2 предложения, по-русски) ответь: что ты "
					"понял и что запомнил. Если нужно, задай ОДИН "
					"уточняющий вопрос. Не спорь и не оправдывайся."_q;
			const auto text = transcript();
			const auto known = Core::GgtgAi::Profile::Block(1500).trimmed();
			Core::GgtgAi::Ask(
				system
					+ (known.isEmpty()
						? QString()
						: (state->interview
							? u"\n\nЭто ты о нём уже знаешь, не спрашивай "
								"об этом снова: спрашивай о том, чего здесь "
								"нет.\n"_q
							: u"\n\nЭто ты о нём уже знаешь:\n"_q) + known),
				text.isEmpty()
					? u"(Диалог ещё не начался. Задай первый вопрос.)"_q
					: (u"Диалог:\n"_q + text + u"\nТвой ответ:"_q),
				crl::guard(box, [=](QString result, QString error) {
					if (generation != state->generation) {
						return;
					}
					if (!error.isEmpty()) {
						state->busy = false;
						render();
						if (const auto status = state->status.data()) {
							status->setText(u"Не вышло: "_q + error);
						}
						return;
					}
					state->turns.push_back({ .mine = false, .text = result });
					saveTurns();
					state->busy = false;
					render();
					// The talk goes on at once; what was said is folded
					// into the rules in the background.
					if (const auto status = state->status.data()) {
						status->setText(QString());
					}
					learn();
				}),
				0.5,
				300,
				Core::GgtgAi::Role::General,
				QStringList(),
				true);
		};

		state->ask = answer;

		const auto submit = [=] {
			const auto text = field->getLastText().trimmed();
			if (text.isEmpty() || state->busy) {
				return;
			}
			field->setText(QString());
			state->turns.push_back({ .mine = true, .text = text });
			saveTurns();
			render();
			answer();
		};
		field->submits(
		) | rpl::on_next(submit, field->lifetime());

		tabs->activeChanges(
		) | rpl::on_next([=](int index) {
			state->interview = (index == 0);
			describeMode();
			if (state->interview && state->turns.empty() && !state->busy) {
				answer();
			}
		}, tabs->lifetime());

		box->addButton(rpl::single(u"Отправить"_q), submit);
		box->addLeftButton(rpl::single(u"Составить промт"_q), [=] {
			if (state->turns.empty() || state->busy) {
				Ggtg::Notice(box->uiShow(), 
					u"Сначала ответьте ИИ хотя бы на несколько вопросов"_q);
				return;
			}
			state->busy = true;
			if (const auto status = state->status.data()) {
				status->setText(u"ИИ составляет промт..."_q);
			}
			const auto notes = Core::GgtgAi::Profile::Block(2500).trimmed();
			Core::GgtgAi::Ask(
				u"По диалогу составь описание пользователя для помощника, "
				"который пишет сообщения от его имени. Имя не указывай "
				"(у пользователя несколько аккаунтов). От первого лица, "
				"по-русски, строго в таком виде:\n"
				"Чем занимаюсь: ...\nЦель диалога (к чему "
				"вести собеседника): ...\nКак общаться (стиль, «ты» или "
				"«вы»): ...\nЧто можно говорить (цены, условия, ссылки): ...\n"
				"Чего нельзя: ...\n"
				"Только факты из диалога, ничего не выдумывай; строку, о "
				"которой ничего не известно, пропусти. До 1200 символов."_q,
				u"Диалог:\n"_q + transcript()
					+ (notes.isEmpty()
						? QString()
						: (u"\nПрофиль пользователя:\n"_q + notes)),
				crl::guard(box, [=](QString result, QString error) {
					state->busy = false;
					if (const auto status = state->status.data()) {
						status->setText(error.isEmpty()
							? QString()
							: (u"Не вышло: "_q + error));
					}
					if (error.isEmpty()) {
						ShowAiPromptProposal(show, result);
					}
				}),
				0.3,
				700,
				Core::GgtgAi::Role::General,
				QStringList(),
				true);
		});
		box->addButton(rpl::single(u"Закрыть"_q), [=] {
			box->closeBox();
		});

		// The talk starts by itself when the AI is the one to ask.
		// A saved conversation continues where it stopped; only a new one
		// starts with the AI's first question.
		render();
		if (interview && state->turns.empty()) {
			answer();
		}
	}));
}

} // namespace Settings
