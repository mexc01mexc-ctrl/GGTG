/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and exceptions, see https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "core/ggtg_api_override.h"
#include "ui/layers/generic_box.h"
#include "ui/layers/show.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"
#include "window/ggtg_widgets.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"

#include <QtCore/QUrl>
#include <QtGui/QDesktopServices>

namespace Settings {

// "Свой api_id": for when Telegram answers API_ID_PUBLISHED_FLOOD to the
// making of a new session (export to another device).
inline void ShowApiOverrideBox(std::shared_ptr<Ui::Show> show) {
	show->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"Свой api_id"_q));
		box->setWidth(st::boxWideWidth);
		box->addRow(
			object_ptr<Ui::FlatLabel>(
				box,
				u"Telegram иногда отказывает в новой сессии с общим "
				"api_id (ошибка API_ID_PUBLISHED_FLOOD): им пользуются "
				"многие программы. Свой api_id принадлежит только вам.\n\n"
				"Как получить:\n"
				"1. Откройте my.telegram.org и войдите по номеру "
				"телефона (код придёт в Telegram).\n"
				"2. Выберите «API development tools».\n"
				"3. Заполните форму: App title - любое имя, Short name - "
				"латиницей (5-32 символа), платформа - Desktop. Остальное "
				"можно пустым. Нажмите «Create application».\n"
				"4. Скопируйте «App api_id» (число) и «App api_hash» "
				"(32 символа) в поля ниже.\n\n"
				"Не показывайте api_hash другим. Он используется только "
				"при выдаче новых сессий."_q,
				st::boxLabel),
			st::boxRowPadding);
		Ui::AddSkip(box->verticalLayout(), st::defaultVerticalListSkip);
		const auto saved = Core::GgtgApiOverride::Load();
		const auto id = box->addRow(
			object_ptr<Ui::InputField>(
				box,
				st::defaultInputField,
				rpl::single(u"api_id (число)"_q),
				saved.apiId ? QString::number(saved.apiId) : QString()),
			st::boxRowPadding);
		Ui::AddSkip(box->verticalLayout(), st::defaultVerticalListSkip);
		const auto hash = box->addRow(
			object_ptr<Ui::InputField>(
				box,
				st::defaultInputField,
				rpl::single(u"api_hash (32 символа)"_q),
				saved.apiHash),
			st::boxRowPadding);
		const auto link = box->addRow(
			object_ptr<Ui::LinkButton>(box, u"Открыть my.telegram.org"_q),
			st::boxRowPadding);
		link->setClickedCallback([] {
			QDesktopServices::openUrl(QUrl(u"https://my.telegram.org/apps"_q));
		});
		box->addButton(rpl::single(u"Сохранить"_q), [=] {
			const auto value = Core::GgtgApiOverride::Value{
				.apiId = id->getLastText().trimmed().toInt(),
				.apiHash = hash->getLastText().trimmed(),
			};
			if (id->getLastText().trimmed().isEmpty()
				&& hash->getLastText().trimmed().isEmpty()) {
				Core::GgtgApiOverride::Save({});
				Ggtg::Notice(box->uiShow(), u"Свой api_id убран"_q);
				box->closeBox();
				return;
			}
			if (!value.valid()) {
				Ggtg::Notice(
					box->uiShow(),
					u"Проверьте: api_id - число, api_hash - 32 символа"_q);
				return;
			}
			Core::GgtgApiOverride::Save(value);
			Ggtg::Notice(
				box->uiShow(),
				u"Сохранено. Повторите экспорт."_q);
			box->closeBox();
		});
		box->addButton(rpl::single(u"Закрыть"_q), [=] { box->closeBox(); });
	}));
}

} // namespace Settings
