/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and exceptions, see https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "window/ggtg_widgets.h"
#include "core/ggtg_ai.h"
#include "core/ggtg_ai_profile.h"
#include "ui/boxes/confirm_box.h"
#include "ui/layers/generic_box.h"
#include "ui/layers/show.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_widgets.h"

#include <QtGui/QTextCursor>

namespace Settings {

// "Профиль: что ИИ знает о вас" - everything the AI has learned about the
// user and their business in one editable text (see Core::GgtgAi::Profile):
// a "## Section" line, then one "- fact" per line, the older facts above.
// The user can fix, add and remove facts here by hand; "Почистить" asks the
// model to merge repeats and settle contradictions (the old text is kept in
// a backup file next to the profile).
inline void ShowAiProfile(
		std::shared_ptr<Ui::Show> show,
		Fn<void()> changed = nullptr) {
	show->show(Box([=](not_null<Ui::GenericBox*> box) {
		namespace Profile = Core::GgtgAi::Profile;
		box->setTitle(rpl::single(u"Профиль: что ИИ знает о вас"_q));
		box->setWidth(st::boxWideWidth);

		struct State {
			bool busy = false;
			QPointer<Ui::InputField> field;
			QPointer<Ui::FlatLabel> status;
		};
		const auto state = box->lifetime().make_state<State>();

		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			u"Здесь всё, что ИИ знает о вас и вашем деле. Его читают "
				"помощник в чатах, помощник текста рассылки и агент, а "
				"пополняют разговоры с ИИ, «Научиться на переписке» и "
				"агент. Правьте как обычный текст: заголовок «## Раздел», "
				"под ним пункты со знака «- ». Внизу раздела самые свежие "
				"пункты; когда пунктов больше лимита раздела, старые "
				"уходят."_q,
			st::boxDividerLabel));
		state->field = box->addRow(object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			Ui::InputField::Mode::MultiLine,
			rpl::single(u"Профиль"_q),
			Profile::ToText(Profile::Read())));
		// A tall field that starts at its top, not at the end of the text.
		state->field->setMinHeight(st::boxWideWidth / 2);
		state->field->setMaxHeight(st::boxWideWidth);
		const auto toTop = [=] {
			if (const auto field = state->field.data()) {
				auto cursor = field->textCursor();
				cursor.movePosition(QTextCursor::Start);
				field->setTextCursor(cursor);
			}
		};
		toTop();
		const auto tidy = box->addRow(
			object_ptr<Ui::LinkButton>(
				box,
				u"Почистить повторы и противоречия (ИИ)"_q),
			st::boxRowPadding);
		state->status = box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			QString(),
			st::boxDividerLabel));

		const auto setStatus = [=](const QString &text) {
			if (const auto status = state->status.data()) {
				status->setText(text);
			}
		};
		const auto reload = [=] {
			if (const auto field = state->field.data()) {
				field->setText(Profile::ToText(Profile::Read()));
			}
			toTop();
		};

		box->addButton(rpl::single(u"Сохранить"_q), [=] {
			if (state->busy) {
				return;
			}
			const auto field = state->field.data();
			if (!field) {
				return;
			}
			const auto parsed = Profile::FromText(
				field->getLastText(),
				Profile::Read());
			if (!parsed) {
				setStatus(u"Не вижу ни одной строки «## Раздел». Верните "
					"заголовки разделов."_q);
				return;
			}
			Profile::Write(*parsed);
			if (changed) {
				changed();
			}
			Ggtg::Notice(box->uiShow(), u"Профиль сохранён"_q);
			box->closeBox();
		});
		box->addButton(rpl::single(u"Закрыть"_q), [=] {
			box->closeBox();
		});
		tidy->setClickedCallback([=] {
			if (state->busy) {
				return;
			}
			state->busy = true;
			setStatus(u"ИИ наводит порядок: объединяет повторы и убирает "
				"противоречия (первый раз модели нужно несколько секунд "
				"на запуск)..."_q);
			// What is on the screen is the user's to keep: it is saved first.
			if (const auto field = state->field.data()) {
				if (const auto parsed = Profile::FromText(
						field->getLastText(),
						Profile::Read())) {
					Profile::Write(*parsed);
				}
			}
			Profile::Consolidate(crl::guard(box, [=](
					int before,
					int after,
					QString error) {
				state->busy = false;
				if (!error.isEmpty()) {
					setStatus(u"Не вышло: "_q + error);
					return;
				}
				reload();
				if (changed) {
					changed();
				}
				setStatus(before == after
					? u"Всё уже в порядке: пунктов "_q
						+ QString::number(after) + '.'
					: u"Было пунктов: "_q + QString::number(before)
						+ u", стало: "_q + QString::number(after)
						+ u". Прежний вариант лежит рядом в "
							"ai_profile.json.backup."_q);
			}));
		});
		box->addLeftButton(rpl::single(u"Забыть всё"_q), [=] {
			box->uiShow()->showBox(Ui::MakeConfirmBox({
				.text = u"Удалить весь профиль и примеры вашего стиля? ИИ "
					"забудет всё, чему научился."_q,
				.confirmed = [=](Fn<void()> &&close) {
					close();
					Profile::Forget();
					reload();
					if (changed) {
						changed();
					}
					setStatus(u"Профиль очищен."_q);
				},
				.confirmText = u"Удалить"_q,
			}));
		});
	}));
}

} // namespace Settings
