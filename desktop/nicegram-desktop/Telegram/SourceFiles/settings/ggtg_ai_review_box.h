/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and exceptions, see https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "core/ggtg_ai_inbox.h"
#include "core/ggtg_ai_profile.h"
#include "core/ggtg_ai_train.h"
#include "ui/layers/generic_box.h"
#include "ui/ui_utility.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"
#include "window/ggtg_widgets.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_widgets.h"

namespace Settings {

// "Проверить, что ИИ выучил": what got into the profile and the playbook
// since the last look, each with "Убрать". A mistake the AI learned would
// otherwise stay quiet and spread into every reply. "Всё верно" marks it all
// as seen.

namespace AiReview {

// How many things were learned since the last look.
[[nodiscard]] inline int NewCount() {
	const auto since = Core::GgtgAi::Inbox::Remembered().lastReview;
	auto result = int(Core::GgtgAi::Train::LearnedSince(since).size());
	const auto data = Core::GgtgAi::Profile::Read();
	for (const auto &list : data.sections) {
		for (const auto &entry : list) {
			result += (entry.at > since) ? 1 : 0;
		}
	}
	return result;
}

inline void RemoveFact(int section, const QString &text) {
	auto data = Core::GgtgAi::Profile::Read();
	auto &list = data.sections[section];
	list.erase(
		std::remove_if(list.begin(), list.end(), [&](const auto &entry) {
			return entry.text == text;
		}),
		list.end());
	Core::GgtgAi::Profile::Write(data);
}

} // namespace AiReview

inline void ShowLearnedReview(
		std::shared_ptr<Ui::Show> show,
		Fn<void()> closed = nullptr) {
	show->show(Box([=](not_null<Ui::GenericBox*> box) {
		namespace Profile = Core::GgtgAi::Profile;
		namespace Train = Core::GgtgAi::Train;
		box->setTitle(rpl::single(u"Что ИИ выучил"_q));
		box->setWidth(st::boxWideWidth);
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			u"Новое с прошлой проверки. Уберите то, что неверно или "
				"устарело: иначе ИИ будет повторять это в ответах."_q,
			st::boxDividerLabel));
		const auto since = Core::GgtgAi::Inbox::Remembered().lastReview;
		const auto wrap = box->addRow(
			object_ptr<Ui::VerticalLayout>(box),
			style::margins());
		const auto rebuild = std::make_shared<Fn<void()>>();
		*rebuild = [=] {
			wrap->clear();
			const auto again = [=] { Ui::PostponeCall(box, *rebuild); };
			auto any = false;
			const auto data = Profile::Read();
			for (auto i = 0; i != Profile::kSections; ++i) {
				for (const auto &entry : data.sections[i]) {
					if (entry.at <= since) {
						continue;
					}
					any = true;
					const auto text = entry.text;
					wrap->add(
						object_ptr<Ui::FlatLabel>(
							wrap,
							Profile::Info(Profile::Section(i)).title
								+ u": "_q + text,
							st::boxLabel),
						st::boxRowPadding);
					wrap->add(
						object_ptr<Ui::LinkButton>(wrap, u"Убрать"_q),
						st::boxRowPadding
					)->setClickedCallback([=] {
						AiReview::RemoveFact(i, text);
						again();
					});
					Ui::AddSkip(wrap);
				}
			}
			for (const auto &entry : Train::LearnedSince(since)) {
				any = true;
				const auto what = entry.what;
				wrap->add(
					object_ptr<Ui::FlatLabel>(
						wrap,
						Train::EntryChunk(entry),
						st::boxLabel),
					st::boxRowPadding);
				wrap->add(
					object_ptr<Ui::LinkButton>(wrap, u"Убрать"_q),
					st::boxRowPadding
				)->setClickedCallback([=] {
					Train::RemoveEntry(what);
					again();
				});
				Ui::AddSkip(wrap);
			}
			if (!any) {
				wrap->add(object_ptr<Ggtg::EmptyState>(
					wrap,
					u"С прошлой проверки ничего нового."_q));
			}
		};
		(*rebuild)();
		box->addButton(rpl::single(u"Всё верно"_q), [=] {
			Core::GgtgAi::Inbox::Remembered().lastReview
				= QDateTime::currentSecsSinceEpoch();
			Core::GgtgAi::Inbox::Save();
			box->closeBox();
			if (closed) {
				closed();
			}
		});
		box->addButton(rpl::single(u"Закрыть"_q), [=] { box->closeBox(); });
	}));
}

} // namespace Settings
