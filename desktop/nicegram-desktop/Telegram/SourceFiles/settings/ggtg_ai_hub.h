/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and exceptions, see https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "window/ggtg_widgets.h"
#include "window/ggtg_home_widgets.h"
#include "ui/vertical_list.h"
#include "core/ggtg_ai.h"
#include "core/ggtg_ai_knowledge.h"
#include "core/ggtg_ai_profile.h"
#include "core/ggtg_ai_train.h"
#include "data/data_user.h"
#include "main/main_session.h"
#include "settings/bulk_accounts_actions.h"
#include "settings/ggtg_ai_train.h"
#include "settings/ggtg_ai_profile_box.h"
#include "settings/ggtg_ai_review_box.h"
#include "storage/localstorage.h"
#include "ui/layers/generic_box.h"
#include "ui/layers/show.h"
#include "ui/boxes/confirm_box.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/text/text_utilities.h"
#include "ui/wrap/vertical_layout.h"
#include "ui/ui_utility.h"
#include "platform/platform_file_utilities.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_widgets.h"

#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtGui/QDesktopServices>
#include <QtGui/QGuiApplication>
#include <QtGui/QClipboard>
#include <QtCore/QFileInfo>
#include <deque>

namespace Settings {

// A row of buttons at the bottom of a card (the first is the main one).
inline void AddCardRow(
		not_null<Ui::VerticalLayout*> layout,
		std::vector<std::pair<QString, Fn<void()>>> list) {
	const auto row = layout->add(
		object_ptr<Ui::RpWidget>(layout),
		style::margins(0, st::defaultVerticalListSkip, 0, 0));
	const auto buttons = std::make_shared<std::vector<Ui::RoundButton*>>();
	for (auto &[text, callback] : list) {
		const auto button = Ui::CreateChild<Ui::RoundButton>(
			row,
			rpl::single(text),
			buttons->empty()
				? st::defaultActiveButton
				: st::defaultLightButton);
		button->setClickedCallback(std::move(callback));
		buttons->push_back(button);
	}
	if (!buttons->empty()) {
		row->resize(row->width(), buttons->front()->height());
	}
	row->widthValue(
	) | rpl::on_next([=](int width) {
		const auto skip = st::defaultVerticalListSkip;
		auto x = 0;
		auto y = 0;
		auto height = 0;
		for (const auto button : *buttons) {
			if (x > 0 && x + button->width() > width) {
				x = 0;
				y += height + skip;
			}
			button->moveToLeft(x, y);
			x += button->width() + skip;
			height = button->height();
		}
		row->resize(width, y + height);
	}, row->lifetime());
}

// ---- connecting a free service: one key, without a card ----

namespace AiConnect {

// How to get the key of a service, in three short steps.
[[nodiscard]] inline TextWithEntities StepsFor(const Core::GgtgAi::CloudProvider &provider) {
	const auto &id = provider.id;
	const auto &url = provider.keyUrl;
	if (id == u"gemini"_q) {
		auto result = TextWithEntities{ u"1. "_q };
		result.append(Ui::Text::Link(u"Откройте сайт"_q, url));
		result.append(u" и войдите в аккаунт Google.\n"
			"2. Нажмите «Create API key» и скопируйте ключ (начинается с "
			"AIza).\n3. Вставьте его ниже."_q);
		return result;
	} else if (id == u"groq"_q) {
		auto result = TextWithEntities{ u"1. "_q };
		result.append(Ui::Text::Link(u"Откройте сайт"_q, url));
		result.append(u", войдите через Google или почту.\n"
			"2. «Create API Key», скопируйте ключ (начинается с gsk_).\n"
			"3. Вставьте его ниже."_q);
		return result;
	} else if (id == u"openrouter"_q) {
		auto result = TextWithEntities{ u"1. "_q };
		result.append(Ui::Text::Link(u"Откройте сайт"_q, url));
		result.append(u" и войдите.\n2. «Create Key», "
			"скопируйте ключ (начинается с sk-or-).\n3. Вставьте его "
			"ниже."_q);
		return result;
	} else if (id == u"nvidia"_q) {
		auto result = TextWithEntities{ u"1. "_q };
		result.append(Ui::Text::Link(u"Откройте сайт"_q, url));
		result.append(u", войдите и подтвердите телефон.\n"
			"2. «Generate API Key», скопируйте ключ (начинается с "
			"nvapi-).\n3. Вставьте его ниже."_q);
		return result;
	} else if (id == u"llm7"_q) {
		return { u"Регистрация не нужна: просто нажмите «Подключить»."_q };
	}
	return {};
}

struct Added {
	QString key;
	QString provider;
	QString error;
};

// Puts a pasted key into the settings (cleaned of spaces, "Bearer" and
// quotes; the service the key says it is from wins over `hint`).
[[nodiscard]] inline Added AddKey(QString raw, const QString &hint) {
	raw.remove(QRegularExpression(u"\\s+"_q));
	raw.remove(QRegularExpression(u"^(Bearer|bearer)"_q));
	raw.remove(QChar('"'));
	raw.remove(QChar('\''));
	if (raw.isEmpty()) {
		return { .error = u"Сначала вставьте ключ."_q };
	}
	const auto detected = Core::GgtgAi::DetectProvider(raw);
	const auto provider = detected.isEmpty() ? hint : detected;
	if (provider.isEmpty()) {
		return { .error = u"Не узнал, от какого сервиса ключ: выберите "
			"сервис кнопкой выше и вставьте ключ ещё раз."_q };
	}
	auto settings = Core::GgtgAi::Load();
	for (const auto &existing : settings.keys) {
		if (existing.key == raw) {
			return { .key = raw, .provider = existing.provider };
		}
	}
	settings.keys.push_back({ raw, provider });
	settings.enabled = true;
	Core::GgtgAi::Save(settings);
	return { .key = raw, .provider = provider };
}

// The block "connect a free AI": the services as buttons; a tap shows how to
// get its key, a button to the site and the field for the key. `connected`
// is called when a key has answered.
inline void Fill(
		not_null<Ui::VerticalLayout*> layout,
		std::shared_ptr<Ui::Show> show,
		Fn<void()> connected) {
	layout->add(
		object_ptr<Ui::FlatLabel>(
			layout,
			u"Выберите сервис. Все бесплатные и без карты; лучше подключить "
				"два: когда у одного кончится дневной лимит, ответит "
				"другой."_q,
			st::boxDividerLabel),
		style::margins(0, 0, 0, st::defaultVerticalListSkip));
	const auto bar = layout->add(object_ptr<Ui::RpWidget>(layout));
	const auto buttons = std::make_shared<
		std::vector<not_null<Ui::RoundButton*>>>();
	const auto details = layout->add(object_ptr<Ui::VerticalLayout>(layout));
	const auto chosen = std::make_shared<QString>();
	const auto showDetails = std::make_shared<Fn<void()>>();
	for (const auto &provider : Core::GgtgAi::CloudProviders()) {
		const auto id = provider.id;
		const auto button = Ui::CreateChild<Ui::RoundButton>(
			bar,
			rpl::single(provider.name),
			st::defaultLightButton);
		button->setClickedCallback([=] {
			*chosen = id;
			(*showDetails)();
		});
		buttons->push_back(button);
	}
	bar->widthValue(
	) | rpl::on_next([=](int width) {
		const auto skip = st::defaultVerticalListSkip;
		auto x = 0;
		auto y = 0;
		auto rowHeight = 0;
		for (const auto &button : *buttons) {
			if (x > 0 && x + button->width() > width) {
				x = 0;
				y += rowHeight + skip;
			}
			button->moveToLeft(x, y);
			x += button->width() + skip;
			rowHeight = button->height();
		}
		bar->resize(width, y + rowHeight);
	}, bar->lifetime());

	*showDetails = [=] {
		details->clear();
		const auto provider = Core::GgtgAi::CloudProviderById(*chosen);
		if (!provider) {
			return;
		}
		const auto id = provider->id;
		const auto card = details->add(
			object_ptr<Ggtg::CardBox>(details),
			style::margins(0, st::defaultVerticalListSkip, 0, 0));
		const auto inner = card->inner();
		inner->add(object_ptr<Ui::FlatLabel>(
			inner,
			provider->name + u": "_q + provider->note,
			st::boxLabel));
		const auto stepsLabel = inner->add(
			object_ptr<Ui::FlatLabel>(inner, st::boxDividerLabel),
			style::margins(0, st::defaultVerticalListSkip, 0, 0));
		stepsLabel->setMarkedText(StepsFor(*provider));
		stepsLabel->setClickHandlerFilter([url = provider->keyUrl](const ClickHandlerPtr &, Qt::MouseButton) {
			QDesktopServices::openUrl(QUrl(url));
			return false;
		});
		const auto result = std::make_shared<QPointer<Ui::FlatLabel>>();
		const auto check = [=](const QString &key) {
			if (const auto label = result->data()) {
				label->setText(u"Проверяю ключ..."_q);
			}
			Core::GgtgAi::TestKey(key, crl::guard(layout, [=](
					bool ok,
					QString message) {
				if (const auto label = result->data()) {
					label->setText(ok
						? (u"Подключено: "_q + provider->name
							+ u". Можно говорить с ИИ."_q)
						: (u""_q + message));
				}
				if (ok && connected) {
					connected();
				}
			}), id);
		};
		if (id == u"llm7"_q) {
			auto wrapOwned = object_ptr<Ui::RpWidget>(inner);
			const auto wrap = wrapOwned.data();
			const auto connectBtn = Ui::CreateChild<Ui::RoundButton>(
				wrap,
				rpl::single(u"Подключить"_q),
				st::defaultActiveButton);
			connectBtn->setTextTransform(
				Ui::RoundButtonTextTransform::NoTransform);
			connectBtn->setClickedCallback([=] {
				const auto added = AddKey(u"unused"_q, id);
				check(added.key);
			});
			wrap->resize(wrap->width(), connectBtn->height());
			wrap->widthValue(
			) | rpl::on_next([=](int width) {
				connectBtn->setFullWidth(width);
				connectBtn->moveToLeft(0, 0, width);
			}, wrap->lifetime());
			inner->add(
				std::move(wrapOwned),
				style::margins(0, st::defaultVerticalListSkip, 0, 0));
		} else {
			const auto field = inner->add(
				object_ptr<Ui::InputField>(
					inner,
					st::defaultInputField,
					rpl::single(u"Вставьте ключ "_q + provider->name),
					QString()),
				style::margins(0, st::defaultVerticalListSkip, 0, 0));
			const auto doCheck = [=] {
				auto raw = field->getLastText();
				raw.remove(QRegularExpression(u"\\s+"_q));
				raw.remove(QRegularExpression(u"^(Bearer|bearer)"_q));
				raw.remove(QChar('"'));
				raw.remove(QChar('\''));
				if (raw.isEmpty()) {
					if (const auto label = result->data()) {
						label->setText(u"Сначала вставьте ключ."_q);
					}
					return;
				}
				if (const auto label = result->data()) {
					label->setText(u"Проверяю ключ..."_q);
				}
				Core::GgtgAi::TestKey(raw, crl::guard(layout, [=](
						bool ok,
						QString message) {
					if (const auto label = result->data()) {
						label->setText(ok
							? (u"Ключ работает: "_q + provider->name + u"."_q)
							: message);
					}
				}), id);
			};
			const auto doAdd = [=] {
				const auto added = AddKey(field->getLastText(), id);
				if (!added.error.isEmpty()) {
					if (const auto label = result->data()) {
						label->setText(added.error);
					}
					return;
				}
				field->setText(QString());
				check(added.key);
			};
			field->submits(
			) | rpl::on_next([=] { doAdd(); }, field->lifetime());

			auto wrapOwned = object_ptr<Ui::RpWidget>(inner);
			const auto wrap = wrapOwned.data();
			const auto checkBtn = Ui::CreateChild<Ui::RoundButton>(
				wrap,
				rpl::single(u"Проверить"_q),
				st::defaultLightButton);
			checkBtn->setTextTransform(
				Ui::RoundButtonTextTransform::NoTransform);
			checkBtn->setClickedCallback(doCheck);

			const auto addBtn = Ui::CreateChild<Ui::RoundButton>(
				wrap,
				rpl::single(u"Добавить"_q),
				st::defaultActiveButton);
			addBtn->setTextTransform(
				Ui::RoundButtonTextTransform::NoTransform);
			addBtn->setClickedCallback(doAdd);

			wrap->resize(wrap->width(), checkBtn->height());
			wrap->widthValue(
			) | rpl::on_next([=](int width) {
				const auto skip = st::defaultVerticalListSkip;
				const auto each = (width - skip) / 2;
				checkBtn->setFullWidth(each);
				checkBtn->moveToLeft(0, 0, width);
				addBtn->setFullWidth(each);
				addBtn->moveToLeft(each + skip, 0, width);
			}, wrap->lifetime());
			inner->add(
				std::move(wrapOwned),
				style::margins(0, st::defaultVerticalListSkip, 0, 0));
		}
		*result = inner->add(
			object_ptr<Ui::FlatLabel>(inner, QString(), st::boxDividerLabel),
			style::margins(0, st::defaultVerticalListSkip, 0, 0));
	};
}

} // namespace AiConnect

// The small windows the chat with the AI opens: materials, learning from the
// user's chats, forgetting. They used to be blocks of one long settings
// page; now each is one tap away from the conversation.

// What the AI has been given, and the way to give more: files, links,
// YouTube, or just text. A right click on a material removes it.
inline void ShowAiMaterialsBox(std::shared_ptr<Ui::Show> show) {
	namespace Knowledge = Core::GgtgAi::Knowledge;
	show->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"Материалы для ИИ"_q));
		box->setWidth(st::boxWideWidth);
		box->addRow(
			object_ptr<Ui::FlatLabel>(
				box,
				u"Прайсы, презентации, описания, FAQ: ИИ прочитает и "
					"будет опираться на это в ответах. Файлы (PDF, Word, "
					"txt, картинки), ссылки на сайты и YouTube или просто "
					"текст. Тексты уходят облачной модели для чтения, сама "
					"база хранится на этом компьютере."_q,
				st::boxDividerLabel),
			st::boxRowPadding);
		const auto status = box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			QString(),
			st::boxDividerLabel));
		const auto list = box->addRow(object_ptr<Ui::VerticalLayout>(box));
		const auto rebuild = std::make_shared<Fn<void()>>();
		*rebuild = [=] {
			list->clear();
			const auto &all = Knowledge::List();
			if (all.empty()) {
				list->add(object_ptr<Ui::FlatLabel>(
					list,
					u"Пока ничего не добавлено."_q,
					st::boxLabel));
				return;
			}
			for (const auto &material : all) {
				const auto id = material.id;
				const auto name = material.name;
				const auto row = list->add(object_ptr<Ggtg::TextRow>(
					list,
					rpl::single(name + u"  ·  "_q
						+ Knowledge::KindTitle(material.kind)
						+ u", кусков: "_q
						+ QString::number(material.chunks.size()))));
				Ggtg::SetContextMenu(row, [=] {
					auto entries = std::vector<Ggtg::MenuEntry>();
					entries.push_back({
						u"Убрать из материалов"_q,
						[=] {
							Knowledge::Remove(id);
							(*rebuild)();
						},
						&st::menuIconDelete,
						true,
					});
					return entries;
				});
			}
			list->add(object_ptr<Ui::FlatLabel>(
				list,
				u"Правая кнопка мыши по материалу - убрать."_q,
				st::boxDividerLabel));
		};
		(*rebuild)();

		// One after another; each reports into the status line, the
		// problems are shown together at the end.
		const auto queue = std::make_shared<std::deque<
			Fn<void(Fn<void(QString)> progress, Fn<void(QString)> done)>>>();
		const auto names = std::make_shared<std::deque<QString>>();
		const auto problems = std::make_shared<QStringList>();
		const auto running = std::make_shared<bool>(false);
		const auto next = std::make_shared<Fn<void()>>();
		*next = [=] {
			if (queue->empty()) {
				*running = false;
				status->setText(problems->isEmpty()
					? u"Готово: база пополнена."_q
					: (u"Готово, но не всё:\n"_q + problems->join('\n')));
				problems->clear();
				(*rebuild)();
				return;
			}
			*running = true;
			const auto job = std::move(queue->front());
			queue->pop_front();
			const auto name = std::move(names->front());
			names->pop_front();
			job(crl::guard(box, [=](QString text) {
				status->setText(text);
			}), crl::guard(box, [=](QString error) {
				if (!error.isEmpty()) {
					problems->push_back(name + u": "_q + error);
				}
				(*next)();
			}));
		};
		const auto enqueue = [=](
				const QString &name,
				Fn<void(Fn<void(QString)>, Fn<void(QString)>)> job) {
			queue->push_back(std::move(job));
			names->push_back(name);
			if (!*running) {
				(*next)();
			}
		};

		const auto field = box->addRow(object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			Ui::InputField::Mode::MultiLine,
			rpl::single(u"Ссылка (сайт, PDF, YouTube) или текст"_q),
			QString()));
		const auto add = [=] {
			const auto text = field->getLastText().trimmed();
			if (text.isEmpty()) {
				status->setText(u"Вставьте ссылку или текст."_q);
				return;
			}
			// Every line that looks like a link is a link; anything else is
			// one text material.
			static const auto linkRe = QRegularExpression(
				u"^(https?://\\S+|www\\.\\S+|\\S+\\.[a-z]{2,}/\\S*)$"_q);
			auto links = QStringList();
			auto plain = QStringList();
			for (const auto &line : text.split('\n', Qt::SkipEmptyParts)) {
				const auto trimmed = line.trimmed();
				(linkRe.match(trimmed).hasMatch() ? links : plain)
					.push_back(trimmed);
			}
			for (const auto &address : links) {
				enqueue(address, [=](
						Fn<void(QString)> progress,
						Fn<void(QString)> done) {
					Knowledge::AddUrl(address, progress, done);
				});
			}
			if (!plain.isEmpty()) {
				const auto body = plain.join('\n');
				auto name = body.left(40).simplified();
				if (body.size() > 40) {
					name += u"…"_q;
				}
				enqueue(name, [=](
						Fn<void(QString)> progress,
						Fn<void(QString)> done) {
					Knowledge::AddText(name, body, progress, done);
				});
			}
			field->setText(QString());
		};
		box->addButton(rpl::single(u"Добавить"_q), add);
		box->addLeftButton(rpl::single(u"Файлы…"_q), [=] {
			FileDialog::GetOpenPaths(
				box.get(),
				u"Выберите файлы и картинки"_q,
				FileDialog::AllFilesFilter(),
				crl::guard(box, [=](FileDialog::OpenResult &&result) {
					for (const auto &path : result.paths) {
						enqueue(QFileInfo(path).fileName(), [=](
								Fn<void(QString)> progress,
								Fn<void(QString)> done) {
							Knowledge::AddFile(path, progress, done);
						});
					}
				}));
		});
		box->addButton(rpl::single(u"Закрыть"_q), [=] { box->closeBox(); });
	}));
}

// Learning from the user's own chats: which accounts, start / stop, and how
// far it has got.
inline void ShowAiTrainingBox(std::shared_ptr<Ui::Show> show) {
	namespace Train = Core::GgtgAi::Train;
	show->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"Учиться на моих чатах"_q));
		box->setWidth(st::boxWideWidth);
		box->addRow(
			object_ptr<Ui::FlatLabel>(
				box,
				u"ИИ читает личные чаты выбранных аккаунтов, где писали вы, "
					"и берёт оттуда ваш стиль, частые вопросы людей, "
					"возражения и ваши ответы на них, а ещё чем кончались "
					"диалоги. Можно оставить на ночь: ход виден в "
					"«Процессах», при лимитах ключей он ждёт сам, "
					"остановить можно в любой момент и продолжить с того же "
					"места. Читаются последние 600 сообщений каждого чата; "
					"группы, каналы и боты не берутся. Тексты уходят "
					"облачной модели без имён, номеров и @юзернеймов "
					"собеседников."_q,
				st::boxDividerLabel),
			st::boxRowPadding);

		const auto accounts = AiTrain::Accounts();
		if (accounts.empty()) {
			box->addRow(object_ptr<Ui::FlatLabel>(
				box,
				u"Нет подключённых аккаунтов."_q,
				st::boxLabel));
		} else {
			// The accounts in their folders, like in the mailing.
			const auto picked = std::make_shared<
				base::flat_set<not_null<Main::Account*>>>();
			const auto unfolded = std::make_shared<base::flat_set<QString>>();
			for (const auto &account : accounts) {
				const auto id = qint64(account->session().user()->id.value);
				const auto &chosen = Train::Current().accounts;
				if (std::find(chosen.begin(), chosen.end(), id)
					!= chosen.end()) {
					picked->emplace(account);
				}
			}
			const auto accordion = box->addRow(
				object_ptr<Ui::VerticalLayout>(box));
			const auto count = box->addRow(
				object_ptr<Ui::FlatLabel>(box, QString(), st::boxLabel));
			const auto selectAll = std::make_shared<rpl::variable<QString>>(
				u"Выбрать все аккаунты"_q);
			const auto sync = [=] {
				auto &ids = Train::Current().accounts;
				for (const auto &account : accounts) {
					const auto id = qint64(account->session().user()->id.value);
					ids.erase(
						std::remove(ids.begin(), ids.end(), id),
						ids.end());
				}
				for (const auto &account : *picked) {
					ids.push_back(
						qint64(account->session().user()->id.value));
				}
				Train::SaveState();
				count->setText(u"Выбрано аккаунтов: "_q
					+ QString::number(picked->size()));
				*selectAll = picked->empty()
					? u"Выбрать все аккаунты"_q
					: u"Снять выбор"_q;
			};
			const auto rebuildPick = [=] {
				RebuildAccountAccordion(accordion, AccountAccordionHooks{
					.selected = picked.get(),
					.expandedFolders = unfolded.get(),
					.selectionChanged = sync,
					.emptyText = u"Нет подключённых аккаунтов."_q,
				});
			};
			box->addRow(object_ptr<Ggtg::ActionRow>(
				box,
				selectAll->value(),
				QString(),
				&st::menuIconSelect)
			)->setClickedCallback([=] {
				const auto select = picked->empty();
				for (const auto &account : accounts) {
					if (select) {
						picked->emplace(account);
					} else {
						picked->remove(account);
					}
				}
				rebuildPick();
				sync();
			});
			rebuildPick();
			sync();
		}

		const auto nightly = box->addRow(
			object_ptr<Ui::Checkbox>(
				box,
				u"Учиться само по ночам (с 1:00 до 7:00)"_q,
				Core::GgtgAi::Load().trainNightly),
			st::boxRowPadding);
		nightly->checkedChanges(
		) | rpl::on_next([=](bool checked) {
			auto settings = Core::GgtgAi::Load();
			settings.trainNightly = checked;
			Core::GgtgAi::Save(settings);
		}, nightly->lifetime());

		const auto status = box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			QString(),
			st::boxDividerLabel));
		const auto refresh = [=] {
			status->setText(AiTrain::Running()
				? (u"Идёт: "_q + AiTrain::Status())
				: Train::Summary());
		};
		refresh();
		const auto timer = Ui::CreateChild<QTimer>(status);
		timer->setInterval(1000);
		QObject::connect(timer, &QTimer::timeout, status, refresh);
		timer->start();

		box->addButton(rpl::single(u"Начать или продолжить"_q), [=] {
			const auto error = AiTrain::Start();
			if (!error.isEmpty()) {
				status->setText(error);
			} else {
				refresh();
			}
		});
		box->addLeftButton(rpl::single(u"Остановить"_q), [=] {
			AiTrain::Stop();
			refresh();
		});
		box->addButton(rpl::single(u"Закрыть"_q), [=] { box->closeBox(); });
	}));
}

// One place to throw things away, each with a question first.
inline void ShowAiForgetBox(std::shared_ptr<Ui::Show> show) {
	namespace Knowledge = Core::GgtgAi::Knowledge;
	namespace Train = Core::GgtgAi::Train;
	show->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"Что забыть"_q));
		box->setWidth(st::boxWideWidth);
		box->addRow(
			object_ptr<Ui::FlatLabel>(
				box,
				u"Выберите, что убрать. Каждое действие спросит ещё раз. "
					"Можно и словами в чате: «забудь, что мы продаём курс»."_q,
				st::boxDividerLabel),
			st::boxRowPadding);
		const auto ask = [=](
				const QString &text,
				const QString &confirm,
				Fn<void()> action) {
			box->uiShow()->show(Ui::MakeConfirmBox({
				.text = text,
				.confirmed = [=](Fn<void()> &&close) {
					close();
					action();
					Ggtg::Notice(show, u"Готово."_q);
				},
				.confirmText = confirm,
			}));
		};
		box->addRow(object_ptr<Ggtg::ActionRow>(
			box,
			rpl::single(u"Отдельные факты о вас…"_q),
			QString(),
			&st::menuIconEdit)
		)->setClickedCallback([=] {
			ShowAiProfile(box->uiShow());
		});
		box->addRow(object_ptr<Ggtg::ActionRow>(
			box,
			rpl::single(u"Всё выученное из чатов"_q),
			QString(),
			&st::menuIconDelete)
		)->setClickedCallback([=] {
			if (AiTrain::Running()) {
				Ggtg::Notice(show, u"Сначала остановите обучение."_q);
				return;
			}
			ask(
				u"Удалить вопросы, возражения и ответы, выученные из "
					"чатов, и прогресс обучения? Правила стиля и «что "
					"работает» в профиле останутся."_q,
				u"Забыть"_q,
				[] { Train::Forget(); });
		});
		box->addRow(object_ptr<Ggtg::ActionRow>(
			box,
			rpl::single(u"Все материалы"_q),
			QString(),
			&st::menuIconDelete)
		)->setClickedCallback([=] {
			ask(
				u"Убрать все материалы (файлы, ссылки, тексты)?"_q,
				u"Убрать"_q,
				[] { Knowledge::RemoveAll(); });
		});
		box->addRow(object_ptr<Ggtg::ActionRow>(
			box,
			rpl::single(u"Профиль целиком (всё, что ИИ знает о вас)"_q),
			QString(),
			&st::menuIconDelete)
		)->setClickedCallback([=] {
			ask(
				u"Стереть профиль: дело, цель, аудиторию, стиль и правила? "
					"Это нельзя отменить."_q,
				u"Стереть"_q,
				[] { Core::GgtgAi::Profile::Forget(); });
		});
		box->addButton(rpl::single(u"Закрыть"_q), [=] { box->closeBox(); });
	}));
}

// The settings of the AI, all of them, in one window: the services it uses,
// where it helps, how it behaves, and what it spent today. Nothing is hidden
// behind "more".
inline void ShowAiSettingsBox(std::shared_ptr<Ui::Show> show) {
	show->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"Настройки ИИ"_q));
		box->setWidth(st::boxWideWidth * 3 / 2);
		const auto option = [&](
				const QString &title,
				bool value,
				Fn<void(Core::GgtgAi::Settings&, bool)> apply) {
			const auto check = box->addRow(
				object_ptr<Ui::Checkbox>(box, title, value),
				st::boxRowPadding);
			check->checkedChanges(
			) | rpl::on_next([=](bool checked) {
				auto settings = Core::GgtgAi::Load();
				apply(settings, checked);
				Core::GgtgAi::Save(settings);
			}, check->lifetime());
			Ui::AddSkip(box->verticalLayout(), st::defaultVerticalListSkip);
		};
		// A line "Title: value ▾" that opens a short list of choices.
		const auto chooser = [&](
				const QString &title,
				std::vector<std::pair<QString, QString>> choices,
				QString current,
				Fn<void(const QString &)> chosen) {
			const auto text = [=](const QString &value) {
				for (const auto &[key, name] : choices) {
					if (key == value) {
						return title + u": "_q + name + u" ▾"_q;
					}
				}
				return title + u" ▾"_q;
			};
			const auto link = box->addRow(
				object_ptr<Ui::LinkButton>(box, text(current)),
				st::boxRowPadding);
			const auto menu = link->lifetime().make_state<
				base::unique_qptr<Ui::PopupMenu>>();
			link->setClickedCallback([=] {
				*menu = base::make_unique_q<Ui::PopupMenu>(
					link,
					st::popupMenuWithIcons);
				for (const auto &[key, name] : choices) {
					const auto value = key;
					(*menu)->addAction(name, [=] {
						link->setText(text(value));
						chosen(value);
					});
				}
				(*menu)->popup(QCursor::pos());
			});
			Ui::AddSkip(box->verticalLayout(), st::defaultVerticalListSkip);
		};
		const auto settings = Core::GgtgAi::Load();

		// ---- the services ----
		Ui::AddSubsectionTitle(
			box->verticalLayout(),
			rpl::single(u"Подключения"_q));
		const auto keys = box->addRow(
			object_ptr<Ui::VerticalLayout>(box),
			style::margins());
		const auto results = std::make_shared<std::map<QString, QString>>();
		const auto rebuild = std::make_shared<Fn<void()>>();
		*rebuild = [=] {
			keys->clear();
			const auto list = Core::GgtgAi::Load().keys;
			if (list.empty()) {
				keys->add(
					object_ptr<Ui::FlatLabel>(
						keys,
						u"Пока ни одного ключа: подключите сервис ниже."_q,
						st::boxLabel),
					st::boxRowPadding);
			}
			for (const auto &entry : list) {
				const auto key = entry.key;
				const auto i = results->find(key);
				const auto status = (i != results->end())
					? i->second
					: Core::GgtgAi::details::Resting(key, QString())
					? u"отдыхает после лимита"_q
					: QString();
				const auto row = keys->add(
					object_ptr<Ggtg::TextRow>(
						keys,
						rpl::single(
							Core::GgtgAi::CloudProviderName(entry.provider)
								+ u"  ·  "_q + Core::GgtgAi::ShortKey(key)
								+ (status.isEmpty()
									? QString()
									: (u"  —  "_q + status)))),
					st::boxRowPadding);
				const auto checkKey = [=] {
					(*results)[key] = u"проверяю..."_q;
					(*rebuild)();
					Core::GgtgAi::TestKey(key, crl::guard(box, [=](
							bool ok,
							QString message) {
						(*results)[key] = (ok ? u"✓ "_q : u"✗ "_q) + message;
						(*rebuild)();
					}));
				};
				const auto remove = [=] {
					box->uiShow()->show(Ui::MakeConfirmBox({
						.text = u"Удалить ключ "_q
							+ Core::GgtgAi::ShortKey(key) + u"?"_q,
						.confirmed = [=](Fn<void()> &&close) {
							close();
							auto settings = Core::GgtgAi::Load();
							settings.keys.erase(
								std::remove_if(
									settings.keys.begin(),
									settings.keys.end(),
									[&](const Core::GgtgAi::CloudKey &k) {
										return k.key == key;
									}),
								settings.keys.end());
							Core::GgtgAi::Save(settings);
							(*rebuild)();
						},
						.confirmText = u"Удалить"_q,
					}));
				};
				row->setClickedCallback(checkKey);
				Ggtg::SetContextMenu(row, [=] {
					return std::vector<Ggtg::MenuEntry>{
						{ u"Проверить"_q, checkKey, &st::menuIconRestore },
						{ u"Копировать ключ"_q, [=] {
							QGuiApplication::clipboard()->setText(key);
							box->uiShow()->showToast(u"Скопировано"_q);
						}, &st::menuIconCopy },
						{ u"Удалить"_q, remove, &st::menuIconDelete, true },
					};
				});
			}
			if (!list.empty()) {
				keys->add(
					object_ptr<Ui::FlatLabel>(
						keys,
						u"Нажмите на ключ - проверить. Правая кнопка мыши - "
							"копировать или удалить."_q,
						st::boxDividerLabel),
					st::boxRowPadding);
			}
		};
		(*rebuild)();

		Ui::AddSkip(box->verticalLayout());
		Ui::AddSubsectionTitle(
			box->verticalLayout(),
			rpl::single(u"Подключить ещё сервис"_q));
		const auto connect = box->addRow(
			object_ptr<Ui::VerticalLayout>(box),
			st::boxRowPadding);
		AiConnect::Fill(connect, box->uiShow(), [=] { (*rebuild)(); });


		// ---- where it helps ----
		Ui::AddSkip(box->verticalLayout());
		Ui::AddSubsectionTitle(
			box->verticalLayout(),
			rpl::single(u"Помощники"_q));
		option(u"ИИ включён"_q, settings.enabled, [](auto &s, bool v) {
			s.enabled = v;
		});
		option(
			u"Кнопка ИИ в поле ввода чата"_q,
			settings.chatAssistant,
			[](auto &s, bool v) { s.chatAssistant = v; });
		option(
			u"Помощник под текстом рассылки"_q,
			settings.templateAssistant,
			[](auto &s, bool v) { s.templateAssistant = v; });
		option(
			u"Сводка ИИ на процессах"_q,
			settings.summaries,
			[](auto &s, bool v) { s.summaries = v; });
		option(
			u"«Перевести» в меню сообщения (без ИИ, Google)"_q,
			settings.messageTranslate,
			[](auto &s, bool v) { s.messageTranslate = v; });
		{
			auto languages = std::vector<std::pair<QString, QString>>();
			for (const auto &code : { u"ru"_q, u"en"_q, u"uk"_q, u"es"_q,
					u"ko"_q, u"zh"_q, u"tr"_q }) {
				auto name = Core::GgtgAi::LanguageByCode(code).russian;
				if (!name.isEmpty()) {
					name[0] = name[0].toUpper();
				}
				languages.emplace_back(code, name);
			}
			chooser(
				u"Переводить на"_q,
				languages,
				settings.translateTo,
				[](const QString &code) {
					auto settings = Core::GgtgAi::Load();
					settings.translateTo = code;
					Core::GgtgAi::Save(settings);
				});
		}

		// ---- how it behaves ----
		Ui::AddSkip(box->verticalLayout());
		Ui::AddSubsectionTitle(
			box->verticalLayout(),
			rpl::single(u"Поведение"_q));
		option(
			u"Думать дольше в чате (медленнее, но точнее)"_q,
			settings.agentThink,
			[](auto &s, bool v) { s.agentThink = v; });
		option(
			u"Учиться на моих чатах по ночам (1:00-7:00)"_q,
			settings.trainNightly,
			[](auto &s, bool v) { s.trainNightly = v; });
		chooser(
			u"Кнопка ИИ помнит переписку"_q,
			{
				{ u"7"_q, u"7 дней"_q },
				{ u"30"_q, u"30 дней"_q },
				{ u"90"_q, u"90 дней"_q },
				{ u"0"_q, u"всегда"_q },
			},
			QString::number(settings.memoryDays),
			[](const QString &days) {
				auto settings = Core::GgtgAi::Load();
				settings.memoryDays = days.toInt();
				Core::GgtgAi::Save(settings);
			});

		// ---- what it spent ----
		Ui::AddSkip(box->verticalLayout());
		Ui::AddSubsectionTitle(
			box->verticalLayout(),
			rpl::single(u"Сегодня"_q));
		const auto calls = Core::GgtgAi::details::CountCallsToday();
		box->addRow(
			object_ptr<Ui::FlatLabel>(
				box,
				u"Запросов к ИИ: %1 (не прошло: %2). Деньги не списываются: "
					"тратится только бесплатный дневной лимит ключей. ИИ "
					"работает, только когда вы его просите или идёт "
					"обучение."_q.arg(calls.ok).arg(calls.failed),
				st::boxDividerLabel),
			st::boxRowPadding);

		box->addButton(rpl::single(u"Готово"_q), [=] { box->closeBox(); });
	}));
}

// Everything the AI knows, in one window: what it learned about the user
// (each fact can be removed with a right click), the materials, what it took
// from the user's chats, what the chat button remembers, and forgetting.
inline void ShowAiMemoryBox(std::shared_ptr<Ui::Show> show) {
	namespace Profile = Core::GgtgAi::Profile;
	namespace Knowledge = Core::GgtgAi::Knowledge;
	show->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"Память ИИ"_q));
		box->setWidth(st::boxWideWidth * 3 / 2);
		const auto content = box->addRow(
			object_ptr<Ui::VerticalLayout>(box),
			style::margins());
		const auto rebuild = std::make_shared<Fn<void()>>();
		*rebuild = [=] {
			const Ggtg::ScrollKeeper keep(content);
			content->clear();
			const auto again = [=] { Ui::PostponeCall(box, *rebuild); };

			Ui::AddSubsectionTitle(content, rpl::single(u"Что ИИ знает о вас"_q));
			const auto data = Profile::Read();
			if (data.empty()) {
				content->add(
					object_ptr<Ui::FlatLabel>(
						content,
						u"Пока ничего. Расскажите о себе в чате или нажмите "
							"там «Расскажу о себе»: ИИ расспросит и запомнит."_q,
						st::boxDividerLabel),
					st::boxRowPadding);
			}
			for (auto i = 0; i != Profile::kSections; ++i) {
				const auto &list = data.sections[i];
				if (list.empty()) {
					continue;
				}
				content->add(
					object_ptr<Ui::FlatLabel>(
						content,
						Profile::Info(Profile::Section(i)).title,
						st::boxDividerLabel),
					style::margins(
						st::boxRowPadding.left(),
						st::defaultVerticalListSkip,
						st::boxRowPadding.right(),
						0));
				for (const auto &entry : list) {
					const auto text = entry.text;
					const auto row = content->add(
						object_ptr<Ggtg::TextRow>(
							content,
							rpl::single(u"• "_q + text)),
						st::boxRowPadding);
					Ggtg::SetContextMenu(row, [=] {
						return std::vector<Ggtg::MenuEntry>{
							{ u"Копировать"_q, [=] {
								QGuiApplication::clipboard()->setText(text);
								box->uiShow()->showToast(u"Скопировано"_q);
							}, &st::menuIconCopy },
							{ u"Забыть это"_q, [=] {
								AiReview::RemoveFact(i, text);
								again();
							}, &st::menuIconDelete, true },
						};
					});
				}
			}
			const auto fresh = AiReview::NewCount();
			auto actions = std::vector<std::pair<QString, Fn<void()>>>();
			if (fresh > 0) {
				actions.push_back({
					u"Проверить новое ("_q + QString::number(fresh) + ')',
					[=] { ShowLearnedReview(box->uiShow(), again); },
				});
			}
			actions.push_back({ u"Изменить вручную"_q, [=] {
				ShowAiProfile(box->uiShow());
			} });
			const auto buttons = content->add(
				object_ptr<Ui::VerticalLayout>(content),
				st::boxRowPadding);
			AddCardRow(buttons, std::move(actions));
			if (!data.empty()) {
				content->add(
					object_ptr<Ui::FlatLabel>(
						content,
						u"Правая кнопка мыши по факту - забыть его."_q,
						st::boxDividerLabel),
					st::boxRowPadding);
			}

			Ui::AddSkip(content);
			Ui::AddSubsectionTitle(content, rpl::single(u"Знания"_q));
			const auto &materials = Knowledge::List();
			auto chunks = 0;
			for (const auto &material : materials) {
				chunks += int(material.chunks.size());
			}
			content->add(object_ptr<Ggtg::InfoRow>(
				content,
				rpl::single(Ggtg::InfoRowData{
					.title = u"Материалы"_q,
					.subtitle = materials.empty()
						? u"Прайсы, сайты, PDF, видео: ИИ опирается на них в "
							"ответах"_q
						: (QString::number(materials.size())
							+ u" источников, кусков: "_q
							+ QString::number(chunks)),
					.chip = materials.empty() ? u"добавить"_q : QString(),
				})),
				st::boxRowPadding
			)->setClickedCallback([=] { ShowAiMaterialsBox(box->uiShow()); });
			content->add(object_ptr<Ggtg::InfoRow>(
				content,
				rpl::single(Ggtg::InfoRowData{
					.title = u"Выучено из ваших чатов"_q,
					.subtitle = AiTrain::Running()
						? (u"Идёт: "_q + AiTrain::Status())
						: Core::GgtgAi::Train::Summary(),
					.chip = AiTrain::Running() ? u"учится"_q : QString(),
					.chipGood = true,
				})),
				st::boxRowPadding
			)->setClickedCallback([=] { ShowAiTrainingBox(box->uiShow()); });
			const auto remembered = Core::GgtgAi::PurgeChatMemory(true);
			content->add(object_ptr<Ggtg::InfoRow>(
				content,
				rpl::single(Ggtg::InfoRowData{
					.title = u"Переписки, которые помнит кнопка ИИ"_q,
					.subtitle = remembered
						? (QString::number(remembered)
							+ u": краткая сводка каждой, чтобы не читать "
								"заново. Нажмите, чтобы забыть все."_q)
						: u"Пока ни одной."_q,
				})),
				st::boxRowPadding
			)->setClickedCallback([=] {
				if (!remembered) {
					return;
				}
				box->uiShow()->show(Ui::MakeConfirmBox({
					.text = u"Забыть все переписки? Кнопка ИИ прочитает "
						"каждый чат заново, когда понадобится."_q,
					.confirmed = [=](Fn<void()> &&close) {
						close();
						Core::GgtgAi::ForgetAllChats();
						again();
					},
					.confirmText = u"Забыть"_q,
				}));
			});

			Ui::AddSkip(content);
			const auto forget = content->add(
				object_ptr<Ui::VerticalLayout>(content),
				st::boxRowPadding);
			AddCardRow(forget, {
				{ u"Забыть…"_q, [=] { ShowAiForgetBox(box->uiShow()); } },
			});
		};
		(*rebuild)();
		box->addButton(rpl::single(u"Готово"_q), [=] { box->closeBox(); });
	}));
}

} // namespace Settings
