/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and exceptions, see https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "core/ggtg_ai.h"
#include "core/ggtg_ai_report.h"
#include "core/ggtg_ai_embed.h"
#include "core/ggtg_ai_knowledge.h"
#include "settings/ggtg_ai_train.h"
#include "settings/bulk_accounts_actions.h"
#include "settings/ggtg_ai_inbox_box.h"
#include "settings/ggtg_ai_review_box.h"
#include "styles/style_menu_icons.h"
#include "settings/ggtg_ai_cards.h"
#include "settings/ggtg_ai_agent.h"
#include "settings/ggtg_ai_dialog.h"
#include "settings/ggtg_ai_profile_box.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/boxes/confirm_box.h"
#include "core/file_utilities.h"

#include <QtCore/QTimer>
#include <QtCore/QPointer>

#include <set>
#include <QtGui/QDesktopServices>
#include <QtGui/QGuiApplication>
#include <QtGui/QClipboard>
#include "ui/layers/generic_box.h"
#include "ui/vertical_list.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"
#include "window/ggtg_widgets.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_widgets.h"

namespace Settings {

// The "ИИ" section of the GGTG window: the free cloud keys and which helpers
// are on. The helpers themselves live where they are used (under the message
// text of "Отправка SMS", on a process in "Процессы") - see Core::GgtgAi.
// Every change is saved at once, there is nothing to confirm.
//
// For an ordinary user it is one step: paste a free key ("Ключи"), the
// provider is told by the key and checked at once; after that each job goes
// to the model that suits it by itself.
[[nodiscard]] inline object_ptr<Ui::BoxContent> MakeGgtgAiBox() {
	return Box([=](not_null<Ui::GenericBox*> box) {
		using namespace Core::GgtgAi;
		box->setTitle(rpl::single(u"Ещё настройки ИИ"_q));
		box->setWidth(st::boxWideWidth * 3 / 2);
		const auto state = box->lifetime().make_state<
			Core::GgtgAi::Settings>(Load());
		// Switches that another window (the feed of answers) can change are
		// not overwritten by this screen's older copy: only what was touched
		// here since the last save is written over them.
		const auto touched = std::make_shared<std::set<QString>>();
		const auto save = [=] {
			const auto fresh = Load();
			if (!touched->contains(u"enabled"_q)) {
				state->enabled = fresh.enabled;
			}
			if (!touched->contains(u"inboxAuto"_q)) {
				state->inboxAuto = fresh.inboxAuto;
			}
			touched->clear();
			Save(*state);
		};
		// One column; the first page is the five steps, the second (folded)
		// holds the rest. What the code below adds goes to the column being
		// built.
		const auto current = std::make_shared<Ui::VerticalLayout*>(nullptr);
		const auto page = [=] {
			return not_null<Ui::VerticalLayout*>(*current);
		};
		const auto addRow = [=](
				auto &&widget,
				style::margins margins = st::boxRowPadding) {
			using Widget = std::remove_pointer_t<decltype(widget.data())>;
			if constexpr (std::is_base_of_v<Ui::Checkbox, Widget>) {
				// Check boxes and radio buttons get room above and below:
				// their own height is just the glyph, so without it the rows
				// touch each other.
				margins = style::margins(
					margins.left(),
					st::defaultVerticalListSkip,
					margins.right(),
					st::defaultVerticalListSkip);
			}
			return (*current)->add(std::move(widget), margins);
		};
		const auto green = QColor(0x3f, 0xc2, 0x5c);
		const auto amber = QColor(0xe8, 0xa8, 0x2c);
		// A block of the section: a gap and a heading.
		const auto heading = [=](const QString &text) {
			Ui::AddSkip(page());
			Ui::AddSubsectionTitle(page(), rpl::single(text));
		};
		// A numbered step of the screen: a grey band, then the heading.
		const auto step = [=](const QString &text) {
			Ui::AddSkip(page());
			Ui::AddDivider(page());
			Ui::AddSkip(page());
			Ui::AddSubsectionTitle(page(), rpl::single(text));
		};
		// A small grey explanation under a control.
		const auto hint = [=](const QString &text) {
			addRow(
				object_ptr<Ui::FlatLabel>(
					box,
					text,
					st::boxDividerLabel),
				style::margins(
					st::boxRowPadding.left(),
					0,
					st::boxRowPadding.right(),
					st::defaultVerticalListSkip));
		};

		using Wrap = Ui::SlideWrap<Ui::VerticalLayout>;
		const auto pages = std::make_shared<std::vector<Wrap*>>();
		const auto addPage = [=] {
			const auto wrap = box->addRow(
				object_ptr<Wrap>(
					box,
					object_ptr<Ui::VerticalLayout>(box)),
				style::margins());
			pages->push_back(wrap);
			*current = wrap->entity();
		};

		addPage();
		// ---- one screen: five steps, the rest folded below ----

		Ui::AddDividerText(
			page(),
			rpl::single(
				u"Подробные настройки. Обычно всё нужное есть в самом чате "
					"ИИ: там вы говорите с ним, подключаете ключи и "
					"включаете или выключаете помощников."_q));

		// ---- what the AI does right now, and when it spends the limits ----
		{
			const auto doing = addRow(object_ptr<AiCard>(box));
			const auto doingView = [=] {
				namespace Inbox = Core::GgtgAi::Inbox;
				auto view = AiCardView();
				const auto settings = Core::GgtgAi::Load();
				const auto &slot = Core::GgtgAi::details::SlotState();
				const auto calls = Core::GgtgAi::details::CountCallsToday();
				auto detail = QStringList();
				if (!settings.enabled
					|| !Core::GgtgAi::CloudReady(settings)) {
					view.dot = st::windowSubTextFg->c;
					view.title = u"Сейчас ИИ ничего не делает"_q;
					view.badge = u"Выключен"_q;
					view.badgeColor = st::windowSubTextFg->c;
					detail.push_back(u"Нет ключа или ИИ выключен: никакие "
						"запросы не уходят, токены не тратятся."_q);
				} else if (AiTrain::Running()) {
					view.dot = green;
					view.title = u"Учится на ваших чатах"_q;
					view.badge = u"Тратит лимит"_q;
					view.badgeColor = amber;
					detail.push_back(AiTrain::Status());
				} else if (slot.active > 0) {
					view.dot = green;
					view.title = u"Выполняет запрос"_q;
					view.badge = u"Тратит лимит"_q;
					view.badgeColor = amber;
					detail.push_back(u"Запросов в работе: %1, в очереди: %2."_q
						.arg(slot.active)
						.arg(slot.queue.size()));
				} else {
					view.dot = green;
					view.title = u"Ждёт, ничего не делает"_q;
					view.badge = u"Токены не тратятся"_q;
					view.badgeColor = green;
					detail.push_back(u"ИИ включается только когда нужно: вы "
						"пишете ему, нажали кнопку ИИ или идёт обучение. Пока "
						"этого нет, запросов нет."_q);
				}
				detail.push_back(u"Запросов к ИИ сегодня: %1 (не прошло: %2). "
					"Деньги не списываются: тратится только бесплатный лимит "
					"ключей."_q.arg(calls.ok).arg(calls.failed));
				auto auto_ = QStringList();
				if (settings.trainNightly) {
					auto_.push_back(u"обучение на чатах ночью с 1 до 7"_q);
				}
				detail.push_back(auto_.isEmpty()
					? u"Сама по себе ничего не запускается."_q
					: (u"Сама по себе запускается: "_q + auto_.join(u"; "_q)
						+ u". Выключается в «Что включено» ниже."_q));
				view.detail = detail.join(u"\n"_q);
				return view;
			};
			doing->setView(doingView());
			const auto doingTimer = Ui::CreateChild<QTimer>(doing);
			doingTimer->setInterval(2000);
			QObject::connect(doingTimer, &QTimer::timeout, doing, [=] {
				doing->setView(doingView());
			});
			doingTimer->start();
		}

		step(u"1. Ключи"_q);
		const auto statusCard = addRow(object_ptr<AiCard>(box));
		const auto result = addRow(object_ptr<Ui::FlatLabel>(
			box,
			QString(),
			st::boxDividerLabel));
		const auto statusView = [=] {
			auto view = AiCardView();
			view.detail = EngineStatusText();
			const auto level = EngineLevelNow();
			if (level == EngineLevel::Ready) {
				view.dot = green;
				view.title = u"ИИ работает"_q;
				view.badge = details::SlotState().busy
					? u"Занят запросом"_q
					: u"Готов"_q;
				view.badgeColor = green;
			} else if (level == EngineLevel::Starting) {
				view.dot = amber;
				view.title = u"Все ключи отдыхают"_q;
				view.badge = u"Лимит"_q;
				view.badgeColor = amber;
			} else {
				view.dot = st::windowSubTextFg->c;
				view.title = state->enabled
					? u"Нужен ключ"_q
					: u"ИИ выключен"_q;
				view.badge = u"Не работает"_q;
				view.badgeColor = st::windowSubTextFg->c;
			}
			view.actions.push_back({ u"Проверить все ключи"_q, [=] {
				result->setText(u"Проверяю..."_q);
				const auto weak = QPointer<Ui::FlatLabel>(result);
				TestConnection([=](bool ok, QString message) {
					if (const auto label = weak.data()) {
						label->setText(message);
					}
				}, [=](QString text) {
					if (const auto label = weak.data()) {
						label->setText(text);
					}
				});
			} });
			return view;
		};
		statusCard->setView(statusView());
		const auto statusTimer = Ui::CreateChild<QTimer>(statusCard);
		statusTimer->setInterval(1000);
		QObject::connect(statusTimer, &QTimer::timeout, statusCard, [=] {
			statusCard->setView(statusView());
		});
		statusTimer->start();

		const auto rebuildKeys = std::make_shared<Fn<void()>>();
		const auto keyResults = std::make_shared<std::map<QString, QString>>();

		// The switches: one lambda, used in several sections.
		const auto addOption = [=](
				const QString &text,
				bool value,
				const QString &explanation,
				Fn<void(bool)> apply) {
			const auto check = addRow(object_ptr<Ui::Checkbox>(
				box,
				text,
				value));
			check->checkedChanges(
			) | rpl::on_next([=](bool checked) {
				apply(checked);
				save();
			}, check->lifetime());
			hint(explanation);
		};
		addOption(
			u"Включить ИИ-помощников"_q,
			state->enabled,
			u"Главный выключатель: без него ни один помощник ничего не "
				"отправляет в облако."_q,
			[=](bool value) {
				state->enabled = value;
				touched->insert(u"enabled"_q);
			});
		heading(u"Ваши ключи по сервисам"_q);
		hint(u"У каждого сервиса свой список ключей: вставьте ключ в поле "
			"своего сервиса. Ключей может быть сколько угодно, они "
			"подменяют друг друга: когда у модели кончился лимит или она "
			"не отвечает, она «отдыхает», а работу сразу берёт следующая. "
			"Ключи хранятся только на этом компьютере. Карта не нужна. "
			"Подробно: docs/ggtg-ai-cloud.md."_q);
		const auto keysList = addRow(object_ptr<Ui::VerticalLayout>(box));
		// The line of a key: the service, the key's tail and what the last
		// check said. It is changed in place, so the buttons under the
		// cursor stay the same widgets and a check can be repeated at once.
		const auto labels = std::make_shared<
			std::map<QString, QPointer<Ui::FlatLabel>>>();
		const auto keyLine = [=](const CloudKey &entry) {
			const auto key = entry.key;
			const auto i = keyResults->find(key);
			const auto known = CloudKeyUsable(entry);
			const auto status = (i != keyResults->end())
				? i->second
				: !known
				? u"Сервис не узнан - этот ключ не используется."_q
				: Core::GgtgAi::details::Resting(key, QString())
				? u"Отдыхает после ошибки или лимита (кнопка «Проверить» "
					"его разбудит)."_q
				: QString();
			return ShortKey(key)
				+ (status.isEmpty() ? QString() : (u"\n"_q + status));
		};
		const auto showKey = [=](const QString &key) {
			const auto i = labels->find(key);
			if (i == labels->end() || !i->second) {
				return;
			}
			for (const auto &entry : state->keys) {
				if (entry.key == key) {
					i->second->setText(keyLine(entry));
					return;
				}
			}
		};
		const auto checkKey = [=](const QString &key) {
			(*keyResults)[key] = u"Проверяю..."_q;
			if (labels->find(key) != labels->end()) {
				showKey(key);
			} else {
				Ui::PostponeCall(box, *rebuildKeys);
			}
			TestKey(key, crl::guard(box, [=](bool ok, QString message) {
				(*keyResults)[key] = (ok ? u"✓ "_q : u"✗ "_q) + message;
				if (labels->find(key) != labels->end()) {
					showKey(key);
				} else {
					(*rebuildKeys)();
				}
			}));
		};
		// One key of a service: its tail with the last check, and two links.
		const auto addKeyRow = [=](const CloudKey &entry) {
			const auto key = entry.key;
			const auto label = keysList->add(
				object_ptr<Ui::FlatLabel>(
					keysList,
					keyLine(entry),
					st::boxLabel),
				style::margins(
					st::boxRowPadding.left(),
					st::defaultVerticalListSkip,
					st::boxRowPadding.right(),
					0));
			(*labels)[key] = QPointer<Ui::FlatLabel>(label);
			const auto askDelete = [=] {
					box->uiShow()->show(Ui::MakeConfirmBox({
						.text = u"Удалить ключ "_q + ShortKey(key)
							+ u"? На странице сервиса он останется "
								"рабочим."_q,
						.confirmed = [=](Fn<void()> &&close) {
							close();
							auto &keys = state->keys;
							keys.erase(
								std::remove_if(
									keys.begin(),
									keys.end(),
									[&](const CloudKey &k) {
										return k.key == key;
									}),
								keys.end());
							keyResults->erase(key);
							save();
							Ui::PostponeCall(box, *rebuildKeys);
						},
						.confirmText = u"Удалить"_q,
					}));
			};
			// The two buttons stay as they were; the right-click menu of the
			// key's line has the same plus copying the key.
			AiInboxUi::AddButtons(keysList, {
				{ u"Проверить"_q, [=] { checkKey(key); } },
				{ u"Удалить"_q, askDelete },
			});
			Ggtg::SetWidgetContextMenu(label, [=] {
				auto entries = std::vector<Ggtg::MenuEntry>();
				entries.push_back({
					u"Проверить"_q,
					[=] { checkKey(key); },
					&st::menuIconRestore,
				});
				entries.push_back({
					u"Копировать ключ"_q,
					[=] {
						QGuiApplication::clipboard()->setText(key);
						box->uiShow()->showToast(u"Скопировано"_q);
					},
					&st::menuIconCopy,
				});
				entries.push_back({
					u"Удалить"_q,
					askDelete,
					&st::menuIconDelete,
					true,
				});
				return entries;
			});
		};
		// Puts a pasted key into the list of `providerId` (the list of the
		// service the key really belongs to, when it says so itself).
		const auto addKey = [=](
				const QString &providerId,
				not_null<Ui::InputField*> field,
				not_null<Ui::FlatLabel*> result) {
			auto key = field->getLastText();
			key.remove(QRegularExpression(u"\\s+"_q));
			key.remove(QRegularExpression(u"^(Bearer|bearer)"_q));
			key.remove(QChar('"'));
			key.remove(QChar('\''));
			if (key.isEmpty()) {
				result->setText(u"Сначала вставьте ключ."_q);
				return;
			}
			auto provider = providerId;
			const auto detected = DetectProvider(key);
			if (!detected.isEmpty() && detected != providerId) {
				provider = detected;
			}
			for (const auto &existing : state->keys) {
				if (existing.key == key) {
					result->setText(u"Этот ключ уже есть."_q);
					return;
				}
			}
			state->keys.push_back({ key, provider });
			state->enabled = true;
			touched->insert(u"enabled"_q);
			save();
			field->setText(QString());
			result->setText((provider == providerId)
				? u"Ключ добавлен, проверяю."_q
				: (u"Этот ключ от "_q + CloudProviderName(provider)
					+ u": добавил его в список этого сервиса."_q));
			checkKey(key);
		};
		*rebuildKeys = [=] {
			keysList->clear();
			labels->clear();
			for (const auto &provider : CloudProviders()) {
				const auto id = provider.id;
				Ui::AddSkip(keysList);
				Ui::AddSubsectionTitle(
					keysList,
					rpl::single(provider.name));
				keysList->add(
					object_ptr<Ui::FlatLabel>(
						keysList,
						provider.note,
						st::boxDividerLabel),
					st::boxRowPadding);
				auto count = 0;
				for (const auto &entry : state->keys) {
					if (entry.provider == id) {
						++count;
						addKeyRow(entry);
					}
				}
				if (!count) {
					keysList->add(
						object_ptr<Ui::FlatLabel>(
							keysList,
							u"Ключей этого сервиса пока нет."_q,
							st::boxLabel),
						style::margins(
							st::boxRowPadding.left(),
							st::defaultVerticalListSkip,
							st::boxRowPadding.right(),
							0));
				}
				const auto field = keysList->add(
					object_ptr<Ui::InputField>(
						keysList,
						st::defaultInputField,
						rpl::single(u"Вставьте ключ "_q + provider.name),
						QString()),
					st::boxRowPadding);
				const auto result = keysList->add(
					object_ptr<Ui::FlatLabel>(
						keysList,
						QString(),
						st::boxDividerLabel),
					st::boxRowPadding);
				field->submits(
				) | rpl::on_next([=] {
					addKey(id, field, result);
				}, field->lifetime());
				keysList->add(
					object_ptr<Ggtg::ActionRow>(
						keysList,
						rpl::single(u"Добавить и проверить"_q),
						QString(),
						&st::menuIconAdd),
					st::boxRowPadding
				)->setClickedCallback([=] {
					addKey(id, field, result);
				});
				keysList->add(
					object_ptr<Ggtg::ActionRow>(
						keysList,
						rpl::single(u"Получить бесплатный ключ "_q
							+ provider.name),
						QString(),
						&st::menuIconNewWindow),
					st::boxRowPadding
				)->setClickedCallback([=] {
					QDesktopServices::openUrl(QUrl(provider.keyUrl));
				});
			}
			// Keys of a service this version does not know: only to remove.
			for (const auto &entry : state->keys) {
				if (!CloudProviderById(entry.provider)) {
					Ui::AddSkip(keysList);
					Ui::AddSubsectionTitle(
						keysList,
						rpl::single(u"Другое"_q));
					addKeyRow(entry);
				}
			}
		};
		(*rebuildKeys)();
		hint(u"Для GitHub Models нужен токен с правом «Models: read». "
			"Лучше взять несколько сервисов: Gemini - самый умный и видит "
			"картинки, Groq - самый быстрый для массового разбора, "
			"OpenRouter - много бесплатных моделей."_q);

		heading(u"Бесплатные модели OpenRouter"_q);
		const auto freeStatus = addRow(object_ptr<Ui::FlatLabel>(
			box,
			QString(),
			st::boxDividerLabel));
		const auto refreshFree = [=] {
			const auto &list = OpenRouterFree();
			auto names = QStringList();
			for (auto i = 0; i != std::min(int(list.size()), 5); ++i) {
				names.push_back(list[i].id);
			}
			freeStatus->setText(list.empty()
				? u"Список ещё не скачан: он скачивается сам раз в день, "
					"когда есть ключ OpenRouter."_q
				: (u"Сейчас бесплатных: %1. Лучшие: %2. Список обновляется "
					"сам раз в день; когда одна модель упирается в лимит, "
					"работу берёт следующая."_q
					.arg(list.size())
					.arg(names.join(u", "_q))));
		};
		refreshFree();
		for (const auto &key : state->keys) {
			if (key.provider == u"openrouter"_q) {
				// The day's list is fetched when this screen is opened.
				RefreshOpenRouterFree(false, crl::guard(box, [=](bool) {
					refreshFree();
				}));
				break;
			}
		}
		addRow(object_ptr<Ggtg::ActionRow>(
			box,
			rpl::single(u"Обновить список сейчас"_q),
			QString(),
			&st::menuIconRestore)
		)->setClickedCallback([=] {
			freeStatus->setText(u"Скачиваю список..."_q);
			RefreshOpenRouterFree(true, crl::guard(box, [=](bool) {
				refreshFree();
			}));
		});
		Ui::AddSkip(page());

		// ---- everything else, folded ----

		Ui::AddSkip(page());
		const auto moreRow = addRow(object_ptr<Ggtg::ActionRow>(
			box,
			rpl::single(u"Дополнительно: профиль, материалы, обучение, "
				"переключатели, отчёт, перевод"_q),
			QString(),
			&st::menuIconSettings));
		addPage();
		const auto extras = pages->back();
		extras->toggle(false, anim::type::instant);
		moreRow->setClickedCallback([=] {
			extras->toggle(!extras->toggled(), anim::type::normal);
		});

		// The same things are one tap away in the "Мозг ИИ" chat; here is
		// the long form of each.
		heading(u"Профиль, материалы и обучение (подробно)"_q);
		hint(u"Всё это удобнее делать в чате «Мозг ИИ»: просто пишите ему, "
			"что запомнить или забыть, кидайте ссылки и тексты. Здесь то "
			"же самое списком."_q);

		heading(u"О бизнесе и цель"_q);
		hint(u"Опишите себя и цель диалога: кто вы, чем занимаетесь, к чему "
			"нужно вести собеседника, как общаться и что можно называть. "
			"Помощник пишет от вашего имени и подводит разговор к этой цели. "
			"Учитываются первые 2000 символов: пишите самое важное в начале. "
			"Правый клик по кнопке ИИ в чате тоже открывает этот промт."_q);
		const auto prompt = addRow(object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			Ui::InputField::Mode::MultiLine,
			rpl::single(u"Ваш промт"_q),
			state->chatPrompt));
		prompt->changes(
		) | rpl::on_next([=] {
			state->chatPrompt = prompt->getLastText().trimmed();
			save();
		}, prompt->lifetime());
		addRow(object_ptr<Ggtg::ActionRow>(
			box,
			rpl::single(u"Вставить шаблон для заполнения"_q),
			QString(),
			&st::menuIconArticle)
		)->setClickedCallback([=] {
			if (prompt->getLastText().trimmed().isEmpty()) {
				prompt->setText(ChatPromptTemplate());
			}
			prompt->setFocus();
		});

		// ---- learning: the assistant gets to know the user ----

		heading(u"Расскажите ИИ о себе в разговоре"_q);
		hint(u"Модель сама не переобучается, но ИИ ведёт ваш профиль: чем вы "
			"занимаетесь, цель, аудитория, стиль, правила, что работает и "
			"что нет. Профиль читают все помощники, а пополняют разговоры с "
			"ИИ, «Научиться на переписке» и агент. Ещё помощник "
			"подстраивается под ваш стиль по сообщениям, которые вы "
			"исправили после его черновика. Вместо ручного промта можно "
			"просто поговорить с ИИ."_q);
		addRow(object_ptr<Ggtg::ActionRow>(
			box,
			rpl::single(u"Диалог: ИИ расспросит меня"_q),
			QString(),
			&st::menuIconFaq)
		)->setClickedCallback([=] {
			ShowAiDialog(box->uiShow(), true);
		});
		addRow(object_ptr<Ggtg::ActionRow>(
			box,
			rpl::single(u"Диалог: у меня замечания и советы"_q),
			QString(),
			&st::menuIconEdit)
		)->setClickedCallback([=] {
			ShowAiDialog(box->uiShow(), false);
		});
		const auto learnedStatus = addRow(object_ptr<Ui::FlatLabel>(
			box,
			QString(),
			st::boxDividerLabel));
		const auto rebuildLearned = std::make_shared<Fn<void()>>();
		*rebuildLearned = [=] {
			const auto count = Profile::Read().count();
			learnedStatus->setText(
				(count
					? (u"В профиле пунктов: "_q + QString::number(count))
					: u"Профиль пока пуст"_q)
				+ u". Примеров вашего стиля: "_q
				+ QString::number(LoadExamples().size()) + '.');
		};
		(*rebuildLearned)();
		addRow(object_ptr<Ggtg::ActionRow>(
			box,
			rpl::single(u"Профиль: что ИИ знает о вас"_q),
			QString(),
			&st::menuIconProfile)
		)->setClickedCallback([=] {
			ShowAiProfile(box->uiShow(), [=] { (*rebuildLearned)(); });
		});
		const auto reviewLabel = [] {
			const auto count = AiReview::NewCount();
			return u"Проверить, что ИИ выучил"_q
				+ (count ? (u" (новое: "_q + QString::number(count) + ')')
					: QString());
		};
		const auto reviewText = std::make_shared<rpl::event_stream<QString>>();
		const auto review = addRow(object_ptr<Ggtg::ActionRow>(
			box,
			rpl::single(reviewLabel()) | rpl::then(reviewText->events()),
			QString(),
			&st::menuIconMarkRead));
		review->setClickedCallback([=] {
			ShowLearnedReview(box->uiShow(), [=] {
				(*rebuildLearned)();
				reviewText->fire(reviewLabel());
			});
		});
		hint(u"Раз в неделю загляните сюда: ИИ покажет, что запомнил нового, "
			"а вы уберёте ошибки, пока они не попали в ответы."_q);

		// ---- materials: one knowledge base ----

		heading(u"Материалы"_q);
		hint(u"Одна база для всего, о чём идёт разговор: презентации, "
			"прайсы, описания, FAQ. Принимает PDF, Word, txt и другие "
			"документы, картинки (текст на них читается), ссылки на сайты и "
			"YouTube (видео берётся по субтитрам, а если их нет, его "
			"смотрит Gemini). Всё режется на смысловые куски, и в каждый "
			"ответ подтягиваются те, что подходят к разговору. Тексты "
			"уходят облачной модели для чтения; сама база хранится на этом "
			"компьютере."_q);
		const auto materialsStatus = addRow(object_ptr<Ui::FlatLabel>(
			box,
			QString(),
			st::boxDividerLabel));
		const auto materialsList = addRow(
			object_ptr<Ui::VerticalLayout>(box));
		const auto rebuildMaterials = std::make_shared<Fn<void()>>();
		*rebuildMaterials = [=] {
			materialsList->clear();
			const auto &list = Knowledge::List();
			if (list.empty()) {
				materialsList->add(object_ptr<Ui::FlatLabel>(
					materialsList,
					u"Пока ничего не добавлено."_q,
					st::boxDividerLabel));
				return;
			}
			for (const auto &material : list) {
				const auto id = material.id;
				materialsList->add(object_ptr<Ggtg::ActionRow>(
					materialsList,
					rpl::single(u"Убрать: "_q + material.name + u" ("_q
						+ Knowledge::KindTitle(material.kind) + u", кусков: "_q
						+ QString::number(material.chunks.size()) + ')'),
					QString(),
					&st::menuIconDelete)
				)->setClickedCallback([=] {
					Knowledge::Remove(id);
					(*rebuildMaterials)();
				});
			}
		};
		(*rebuildMaterials)();
		// Sources are added one after another; each reports into the status
		// line, and the problems are shown together at the end.
		const auto queue = std::make_shared<std::deque<
			Fn<void(Fn<void(QString)> progress, Fn<void(QString)> done)>>>();
		const auto names = std::make_shared<std::deque<QString>>();
		const auto problems = std::make_shared<QStringList>();
		const auto running = std::make_shared<bool>(false);
		const auto next = std::make_shared<Fn<void()>>();
		*next = [=] {
			if (queue->empty()) {
				*running = false;
				materialsStatus->setText(problems->isEmpty()
					? u"Готово: база пополнена."_q
					: (u"Готово, но не всё:\n"_q + problems->join('\n')));
				problems->clear();
				(*rebuildMaterials)();
				return;
			}
			*running = true;
			const auto job = std::move(queue->front());
			queue->pop_front();
			const auto name = std::move(names->front());
			names->pop_front();
			job(crl::guard(box, [=](QString text) {
				materialsStatus->setText(text);
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
		addRow(object_ptr<Ggtg::ActionRow>(
			box,
			rpl::single(u"Добавить файлы или картинки…"_q),
			QString(),
			&st::menuIconFile)
		)->setClickedCallback([=] {
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
		addRow(object_ptr<Ggtg::ActionRow>(
			box,
			rpl::single(u"Добавить ссылку: сайт, PDF или YouTube…"_q),
			QString(),
			&st::menuIconLink)
		)->setClickedCallback([=] {
			box->uiShow()->show(Box([=](not_null<Ui::GenericBox*> inner) {
				inner->setTitle(rpl::single(u"Добавить ссылку"_q));
				const auto field = inner->addRow(object_ptr<Ui::InputField>(
					inner,
					st::defaultInputField,
					Ui::InputField::Mode::MultiLine,
					rpl::single(u"Ссылки, по одной в строке"_q),
					QString()));
				const auto add = [=] {
					const auto lines = field->getLastText().split(
						QChar('\n'),
						Qt::SkipEmptyParts);
					for (const auto &line : lines) {
						const auto address = line.trimmed();
						if (address.isEmpty()) {
							continue;
						}
						enqueue(address, [=](
								Fn<void(QString)> progress,
								Fn<void(QString)> done) {
							Knowledge::AddUrl(address, progress, done);
						});
					}
					inner->closeBox();
				};
				inner->addButton(rpl::single(u"Добавить"_q), add);
				inner->addButton(
					rpl::single(u"Отмена"_q),
					[=] { inner->closeBox(); });
			}));
		});
		hint(u"Совет: для YouTube лучше всего подходят видео с субтитрами. "
			"Без ключа Gemini видео без субтитров и PDF-сканы прочитать "
			"нельзя."_q);

		// ---- learning from the user's own chats: a long background run ----

		heading(u"Обучение на чатах"_q);
		hint(u"Долгий фоновый процесс: ИИ читает личные чаты выбранных "
			"аккаунтов, где писали вы, и берёт оттуда ваш стиль, частые "
			"вопросы людей, возражения и ваши ответы на них, а ещё чем "
			"кончались диалоги. Можно оставить на ночь: ход виден в "
			"«Процессах», при лимитах ключей он ждёт сам, остановить можно в "
			"любой момент и продолжить с того же места. Читаются последние "
			"600 сообщений каждого чата; группы, каналы и боты не берутся. "
			"Тексты уходят облачной модели без имён, номеров и @юзернеймов "
			"собеседников; в память попадают только обобщённые вопросы и "
			"ваши ответы."_q);
		const auto trainAccounts = AiTrain::Accounts();
		if (trainAccounts.empty()) {
			hint(u"Нет подключённых аккаунтов."_q);
		}
		if (!trainAccounts.empty()) {
			// The accounts in their folders, like in the mailing: a tap on
			// a folder selects or folds it, a tap on an account selects it.
			const auto picked = std::make_shared<
				base::flat_set<not_null<Main::Account*>>>();
			const auto unfolded = std::make_shared<base::flat_set<QString>>();
			for (const auto &account : trainAccounts) {
				const auto id = qint64(account->session().user()->id.value);
				const auto &chosen = Train::Current().accounts;
				if (std::find(chosen.begin(), chosen.end(), id)
					!= chosen.end()) {
					picked->emplace(account);
				}
			}
			const auto accordion = addRow(object_ptr<Ui::VerticalLayout>(box));
			const auto count = addRow(
				object_ptr<Ui::FlatLabel>(box, QString(), st::boxLabel),
				style::margins(
					st::boxRowPadding.left(),
					st::defaultVerticalListSkip,
					st::boxRowPadding.right(),
					st::defaultVerticalListSkip));
			const auto selectAll = std::make_shared<rpl::variable<QString>>(
				u"Выбрать все аккаунты"_q);
			const auto sync = [=] {
				auto &ids = Train::Current().accounts;
				for (const auto &account : trainAccounts) {
					const auto id = qint64(account->session().user()->id.value);
					ids.erase(std::remove(ids.begin(), ids.end(), id), ids.end());
				}
				for (const auto &account : *picked) {
					ids.push_back(qint64(account->session().user()->id.value));
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
			addRow(object_ptr<Ggtg::ActionRow>(
				box,
				selectAll->value(),
				QString(),
				&st::menuIconSelect)
			)->setClickedCallback([=] {
				const auto select = picked->empty();
				for (const auto &account : trainAccounts) {
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
		addOption(
			u"Учиться само по ночам (с 1:00 до 7:00)"_q,
			state->trainNightly,
			u"Для отмеченных аккаунтов: читает новые чаты, пока вы спите, и "
				"останавливается утром, чтобы лимиты ключей остались на день."_q,
			[=](bool value) { state->trainNightly = value; });
		const auto trainStatus = addRow(object_ptr<Ui::FlatLabel>(
			box,
			QString(),
			st::boxDividerLabel));
		const auto refreshTrain = [=] {
			trainStatus->setText(AiTrain::Running()
				? (u"Идёт: "_q + AiTrain::Status())
				: Train::Summary());
		};
		refreshTrain();
		const auto trainTimer = Ui::CreateChild<QTimer>(trainStatus);
		trainTimer->setInterval(1000);
		QObject::connect(trainTimer, &QTimer::timeout, trainStatus, refreshTrain);
		trainTimer->start();
		addRow(object_ptr<Ggtg::ActionRow>(
			box,
			rpl::single(u"Начать или продолжить обучение"_q),
			QString(),
			&st::menuIconSend)
		)->setClickedCallback([=] {
			const auto error = AiTrain::Start();
			if (!error.isEmpty()) {
				trainStatus->setText(error);
			} else {
				refreshTrain();
			}
		});
		addRow(object_ptr<Ggtg::ActionRow>(
			box,
			rpl::single(u"Остановить"_q),
			QString(),
			&st::menuIconCancel)
		)->setClickedCallback([=] {
			AiTrain::Stop();
			refreshTrain();
		});
		addRow(object_ptr<Ggtg::ActionRow>(
			box,
			rpl::single(u"Прочитать все чаты заново (выученное остаётся)"_q),
			QString(),
			&st::menuIconRestore)
		)->setClickedCallback([=] {
			if (AiTrain::Running()) {
				trainStatus->setText(u"Сначала остановите обучение."_q);
				return;
			}
			box->uiShow()->show(Ui::MakeConfirmBox({
				.text = u"Все чаты снова станут непрочитанными. Выученное "
					"останется."_q,
				.confirmed = [=](Fn<void()> &&close) {
					close();
					Train::StartOver();
					refreshTrain();
				},
				.confirmText = u"Начать заново"_q,
			}));
		});
		addRow(object_ptr<Ggtg::ActionRow>(
			box,
			rpl::single(u"Забыть всё выученное из чатов"_q),
			QString(),
			&st::menuIconDelete)
		)->setClickedCallback([=] {
			if (AiTrain::Running()) {
				trainStatus->setText(u"Сначала остановите обучение."_q);
				return;
			}
			box->uiShow()->show(Ui::MakeConfirmBox({
				.text = u"Удалить вопросы, возражения и ответы, выученные из "
					"чатов, и прогресс обучения? Правила стиля и «что "
					"работает» в вашем профиле останутся: их можно убрать "
					"там."_q,
				.confirmed = [=](Fn<void()> &&close) {
					close();
					Train::Forget();
					(*rebuildMaterials)();
					refreshTrain();
				},
				.confirmText = u"Забыть"_q,
			}));
		});


		heading(u"Что включено"_q);
		addOption(
			u"ИИ-помощник под текстом рассылки"_q,
			state->templateAssistant,
			u"В «Рассылке» под текстом: улучшить, перевести, придумать "
				"варианты."_q,
			[=](bool value) { state->templateAssistant = value; });
		addOption(
			u"«Сводка ИИ» на процессах"_q,
			state->summaries,
			u"В «Процессах»: коротко о том, что произошло и что делать "
				"дальше."_q,
			[=](bool value) { state->summaries = value; });
		addOption(
			u"Кнопка ИИ в поле ввода чата"_q,
			state->chatAssistant,
			u"Рядом со скрепкой: читает переписку и сама набирает ответ в "
				"поле, отправляете вы. Правый клик - ещё действия."_q,
			[=](bool value) { state->chatAssistant = value; });

		heading(u"Где ещё работает ИИ"_q);
		Ui::AddDividerText(
			page(),
			rpl::single(
				u"• Рассылка: под текстом - помощник и «Разбор рассылок»; "
					"галочка «Переводить на язык получателя».\n"
					"• База: «Анализ и чистка базы» - проверка, очистка, "
					"разделение по языкам, Premium, активным.\n"
					"• Чат: кнопка ИИ рядом со скрепкой, правый клик - меню; "
					"ПКМ по сообщению → «Перевести».\n"
					"• Процессы: «Сводка ИИ».\n"
					"• Значок робота в меню слева зелёный, когда есть "
					"рабочий ключ, и жёлтый, когда все ключи отдыхают."_q));


		const auto refreshMemoryHook = std::make_shared<Fn<void()>>();
		addRow(object_ptr<Ggtg::ActionRow>(
			box,
			rpl::single(u"Забыть все переписки (сбросить память помощника)"_q),
			QString(),
			&st::menuIconDelete)
		)->setClickedCallback([=] {
			ForgetAllChats();
			if (*refreshMemoryHook) {
				(*refreshMemoryHook)();
			}
			Ggtg::Notice(box->uiShow(), u"Помощник забыл все переписки. Если "
				"нужно забыть один чат - правый клик по кнопке ИИ в нём."_q);
		});
		hint(u"Помощник помнит краткую сводку каждой переписки, чтобы не "
			"читать её заново. Если он «зациклился» на старой теме или "
			"запутался - сбросьте память: он прочитает переписку с нуля."_q);

		// Each conversation's memory is a separate entry with the date it
		// was last used; an unused one disappears by itself.
		heading(u"Сколько хранить память о переписках"_q);
		const auto memoryCount = addRow(object_ptr<Ui::FlatLabel>(
			box,
			QString(),
			st::boxLabel));
		const auto refreshMemoryCount = [=] {
			const auto count = PurgeChatMemory(true);
			memoryCount->setText(count
				? (u"Сейчас помню переписок: "_q + QString::number(count)
					+ u". Каждая хранится отдельно, с датой последнего "
						"использования."_q)
				: u"Сейчас ничего не помню."_q);
		};
		refreshMemoryCount();
		*refreshMemoryHook = refreshMemoryCount;
		const auto days = std::vector<int>{ 7, 30, 90, 0 };
		const auto daysChosen = std::find(
			days.begin(),
			days.end(),
			state->memoryDays);
		const auto daysGroup = std::make_shared<Ui::RadiobuttonGroup>(
			(daysChosen == days.end())
				? 1
				: int(daysChosen - days.begin()));
		const auto daysNames = std::vector<QString>{
			u"Забывать переписку через 7 дней без использования"_q,
			u"Через 30 дней (рекомендуется)"_q,
			u"Через 90 дней"_q,
			u"Не забывать само (только вручную)"_q,
		};
		for (auto i = 0; i != int(days.size()); ++i) {
			addRow(object_ptr<Ui::Radiobutton>(
				box,
				daysGroup,
				i,
				daysNames[i]));
		}
		daysGroup->setChangedCallback([=](int value) {
			state->memoryDays = days[value];
			save();
			refreshMemoryCount();
		});
		hint(u"Если с помощником в чате не пользовались дольше этого срока, "
			"его сводка этой переписки удаляется. Самой переписки в "
			"Telegram это не касается: при следующем вызове помощник "
			"прочитает её заново."_q);

		heading(u"Отчёт и проверка качества"_q);
		hint(u"Отчёт: что говорят ответы на рассылки, как работает модель за "
			"неделю и что стоит поправить. Проверка: насколько надёжно "
			"программа разбирает ответы людей."_q);
		const auto showText = [=](const QString &title, const QString &text) {
			box->getDelegate()->show(Box([=](not_null<Ui::GenericBox*> inner) {
				inner->setTitle(rpl::single(title));
				const auto label = inner->addRow(object_ptr<Ui::FlatLabel>(
					inner,
					text,
					st::boxLabel));
				label->setSelectable(true);
				inner->addButton(
					rpl::single(u"Закрыть"_q),
					[=] { inner->closeBox(); });
			}));
		};
		addRow(object_ptr<Ggtg::ActionRow>(
			box,
			rpl::single(u"Отчёт: что сработало и что поправить"_q),
			QString(),
			&st::menuIconStats)
		)->setClickedCallback([=] {
			showText(u"Отчёт ИИ"_q, Report::Daily());
		});
		addRow(object_ptr<Ggtg::ActionRow>(
			box,
			rpl::single(u"Проверка: верно ли ИИ сортирует ответы людей"_q),
			QString(),
			&st::menuIconFactcheck)
		)->setClickedCallback([=] {
			Report::Evaluate([=](QString text) {
				showText(u"Проверка качества"_q, text);
			});
		});
		addRow(object_ptr<Ggtg::ActionRow>(
			box,
			rpl::single(u"Проверка: похожи ли ответы ИИ на ваши"_q),
			QString(),
			&st::menuIconQualityHigh)
		)->setClickedCallback([=] {
			Ggtg::Notice(box->uiShow(), u"Проверяю, это займёт минуту..."_q);
			AiInbox::RunEval(crl::guard(box, [=](QString text) {
				showText(u"Проверка ответов"_q, text);
			}));
		});
		hint(u"Берёт до 15 настоящих случаев из ленты (что написал человек и "
			"что вы отправили) и отвечает на них заново: видно, не стал ли "
			"ИИ хуже, когда сменились бесплатные модели."_q);

		heading(u"Смысловой поиск"_q);
		hint(u"Понимает смысл, а не только слова: «сколько стоит» и «какая "
			"цена» для него одно и то же. С ним ИИ точнее выбирает, что из "
			"вашего профиля важно для разговора, а ответы людей на рассылки "
			"разбираются по похожим уже разобранным, без вопроса к модели. "
			"Включается сам, когда есть ключ Gemini: тексты уходят в Google "
			"без имён, номеров и @юзернеймов."_q);
		const auto embedStatus = addRow(object_ptr<Ui::FlatLabel>(
			box,
			Embed::Available()
				? u"Работает (ключ Gemini есть)."_q
				: u"Не работает: добавьте ключ Gemini в шаге «1. Ключи»."_q,
			st::boxDividerLabel));
		addRow(object_ptr<Ggtg::ActionRow>(
			box,
			rpl::single(u"Очистить запомненные векторы"_q),
			QString(),
			&st::menuIconClear)
		)->setClickedCallback([=] {
			Embed::ClearCache();
			embedStatus->setText(u"Очищено: векторы сделаются заново, "
				"когда понадобятся."_q);
		});

		heading(u"Очистка"_q);
		{
			const auto cleanup = addRow(object_ptr<Ui::FlatLabel>(
				box,
				QString(),
				st::boxDividerLabel));
			const auto refreshClean = [=] {
				const auto bytes = Core::GgtgAi::LegacyLocalBytes();
				cleanup->setText(bytes > 0
					? (u"Остались файлы старых локальных моделей и движка: "_q
						+ Core::GgtgAi::SizeText(bytes)
						+ u". Они больше не нужны."_q)
					: u"Старых локальных моделей и движка нет, лишнего "
						"на диске ИИ не занимает."_q);
			};
			refreshClean();
			addRow(object_ptr<Ggtg::ActionRow>(
				box,
				rpl::single(u"Удалить старые локальные модели и движок"_q),
				QString(),
				&st::menuIconDelete)
			)->setClickedCallback([=] {
				const auto freed = Core::GgtgAi::CleanLegacyLocal();
				refreshClean();
				cleanup->setText(freed > 0
					? (u"Удалено: "_q + Core::GgtgAi::SizeText(freed) + '.')
					: u"Удалять нечего: старых моделей нет."_q);
			});
			hint(u"Программа делает это сама при запуске. Ваши ключи, "
				"материалы и всё выученное остаются. LM Studio и другие "
				"программы с моделями GGTG не трогает: если они стоят на "
				"компьютере, удалите их там."_q);
		}

		heading(u"Перевод сообщений в чате"_q);
		hint(u"ПКМ по сообщению → «Перевести»: текст сообщения заменяется переводом прямо в чате, а «Оригинал» возвращает его. Работает без ИИ и "
			"без Premium: текст сообщения уходит в бесплатный онлайн-"
			"переводчик Google. Нужен интернет; больше ничего не "
			"отправляется. Переводить на:"_q);
		const auto codes = std::vector<QString>{
			u"ru"_q, u"en"_q, u"uk"_q, u"es"_q, u"ko"_q, u"zh"_q, u"tr"_q,
		};
		const auto chosen = std::find(
			codes.begin(),
			codes.end(),
			state->translateTo);
		const auto languageGroup = std::make_shared<Ui::RadiobuttonGroup>(
			(chosen == codes.end()) ? 0 : int(chosen - codes.begin()));
		for (auto i = 0; i != int(codes.size()); ++i) {
			auto name = LanguageByCode(codes[i]).russian;
			name[0] = name[0].toUpper();
			addRow(object_ptr<Ui::Radiobutton>(
				box,
				languageGroup,
				i,
				name));
		}
		languageGroup->setChangedCallback([=](int value) {
			state->translateTo = codes[value];
			save();
		});


	});
}

} // namespace Settings
