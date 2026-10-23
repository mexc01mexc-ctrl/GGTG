/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "settings/views_campaign.h"

#include "apiwrap.h"
#include "base/call_delayed.h"
#include "base/random.h"
#include "base/weak_ptr.h"
#include "core/application.h"
#include "core/process_manager.h"
#include "data/data_channel.h"
#include "data/data_peer_id.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "mtproto/sender.h"
#include "settings/bulk_accounts_actions.h"
#include "ui/layers/generic_box.h"
#include "ui/ui_utility.h"
#include "ui/vertical_list.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "window/ggtg_click.h"
#include "window/ggtg_widgets.h"
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_window_main_menu.h"
#include "styles/style_widgets.h"

#include <array>

#include <QtCore/QRegularExpression>

namespace Settings {
namespace {

// Random gap before a lane starts on its next account, so N accounts
// don't hit Telegram in perfect lockstep.
constexpr auto kViewsAccountPauseMin = crl::time(1'000);
constexpr auto kViewsAccountPauseMax = crl::time(3'000);
constexpr auto kViewsStepTimeout = crl::time(45'000);
// "Лёгкий прогрев", always on (same idea as in "Отправка SMS"): after
// opening the channel the account "reads" it for a few seconds before
// actually viewing/joining.
constexpr auto kWarmupPauseMin = crl::time(3'000);
constexpr auto kWarmupPauseMax = crl::time(7'000);
// messages.getMessagesViews takes at most 100 ids per call; a range is
// capped so a typo like 1-9999999 can't turn into a request storm.
constexpr auto kViewsChunk = 100;
constexpr auto kViewsMaxRange = 500;

[[nodiscard]] crl::time NextAccountPause() {
	return kViewsAccountPauseMin + crl::time(base::RandomIndex(
		int(kViewsAccountPauseMax - kViewsAccountPauseMin) + 1));
}

[[nodiscard]] crl::time NextWarmupPause() {
	return kWarmupPauseMin + crl::time(base::RandomIndex(
		int(kWarmupPauseMax - kWarmupPauseMin) + 1));
}

// One channel and the message ids of it to view.
struct ViewsTarget {
	QString username; // public link (t.me/name/123)...
	uint64 channelId = 0; // ...or private one (t.me/c/ID/123)
	std::vector<int> ids;
	QString inviteHash; // join mode: t.me/+hash
	QString label;
};

// t.me/name/123, t.me/name/123-145, t.me/name/topic/123, t.me/c/ID/123,
// with or without scheme / "@"-less; several per text, any separators.
[[nodiscard]] std::vector<ViewsTarget> ParseViewsLinks(
		const QString &text,
		QStringList *bad) {
	static const auto regex = QRegularExpression(
		u"(?:https?://)?(?:t|telegram)\\.me/(?:c/(\\d+)|([A-Za-z][A-Za-z0-9_]{3,}))"
		"/(\\d+)(?:/(\\d+))?(?:\\s*-\\s*(\\d+))?"_q);
	auto result = std::vector<ViewsTarget>();
	auto found = 0;
	auto it = regex.globalMatch(text);
	while (it.hasNext()) {
		const auto m = it.next();
		++found;
		const auto first = m.captured(4).isEmpty()
			? m.captured(3).toInt()
			: m.captured(4).toInt();
		const auto last = m.captured(5).isEmpty()
			? first
			: m.captured(5).toInt();
		if (first <= 0 || last < first || last - first >= kViewsMaxRange) {
			if (bad) {
				bad->push_back(m.captured(0));
			}
			continue;
		}
		auto target = ViewsTarget();
		target.username = m.captured(2);
		target.channelId = m.captured(1).toULongLong();
		for (auto id = first; id <= last; ++id) {
			target.ids.push_back(id);
		}
		target.label = (target.username.isEmpty()
			? (u"c/"_q + m.captured(1))
			: target.username)
			+ '/' + QString::number(first)
			+ (last > first ? ('-' + QString::number(last)) : QString());
		result.push_back(std::move(target));
	}
	if (!found && !text.trimmed().isEmpty() && bad) {
		bad->push_back(text.trimmed());
	}
	return result;
}

// Join mode: t.me/name, @name, t.me/+hash, t.me/joinchat/hash - one per
// line (or separated by spaces/commas), no ranges.
[[nodiscard]] std::vector<ViewsTarget> ParseJoinLinks(
		const QString &text,
		QStringList *bad) {
	static const auto invite = QRegularExpression(
		u"^(?:https?://)?(?:t|telegram)\\.me/(?:\\+|joinchat/)([A-Za-z0-9_-]+)/?$"_q);
	static const auto pub = QRegularExpression(
		u"^(?:(?:https?://)?(?:t|telegram)\\.me/|@)([A-Za-z][A-Za-z0-9_]{3,})/?$"_q);
	auto result = std::vector<ViewsTarget>();
	auto seen = base::flat_set<QString>();
	const auto tokens = text.split(
		QRegularExpression(u"[\\s,;]+"_q),
		Qt::SkipEmptyParts);
	for (const auto &token : tokens) {
		auto target = ViewsTarget();
		if (const auto m = invite.match(token); m.hasMatch()) {
			target.inviteHash = m.captured(1);
			target.label = u"+"_q + target.inviteHash;
		} else if (const auto m = pub.match(token); m.hasMatch()) {
			target.username = m.captured(1);
			target.label = target.username;
		} else {
			if (bad) {
				bad->push_back(token);
			}
			continue;
		}
		if (seen.emplace(target.label.toLower()).second) {
			result.push_back(std::move(target));
		}
	}
	return result;
}

void ResolvePeerRaw(
		not_null<Main::Account*> account,
		const ViewsTarget &target,
		Fn<void(PeerData *peer, QString error)> done) {
	const auto session = &account->session();
	if (target.username.isEmpty()) {
		const auto channel = session->data().channelLoaded(
			ChannelId(target.channelId));
		done(channel, channel ? QString() : u"нет доступа к каналу"_q);
		return;
	}
	if (const auto peer = session->data().peerByUsername(target.username)) {
		done(peer, QString());
		return;
	}
	const auto api = static_cast<MTP::Sender*>(&session->api());
	api->request(MTPcontacts_ResolveUsername(
		MTP_flags(MTPcontacts_ResolveUsername::Flags(0)),
		MTP_string(target.username),
		MTP_string(QString())
	)).done([=, api = api](const MTPcontacts_ResolvedPeer &result) {
		result.match([&](const MTPDcontacts_resolvedPeer &data) {
			session->data().processUsers(data.vusers());
			session->data().processChats(data.vchats());
			const auto peerId = peerFromMTP(data.vpeer());
			done(
				peerId ? session->data().peer(peerId).get() : nullptr,
				peerId ? QString() : u"канал не найден"_q);
		});
	}).fail([=, api = api](const MTP::Error &error) {
		done(nullptr, error.type());
	}).send();
}

// Resolves the channel, then holds for the warm-up pause before handing
// it over - see kWarmupPauseMin.
void ResolvePeerForAccount(
		not_null<Main::Account*> account,
		const ViewsTarget &target,
		Fn<void(PeerData *peer, QString error)> done) {
	ResolvePeerRaw(account, target, [=](PeerData *peer, QString error) {
		if (!peer) {
			done(nullptr, error);
			return;
		}
		base::call_delayed(NextWarmupPause(), &Core::App(), [=] {
			done(peer, QString());
		});
	});
}

// Views every id of `target` from `account`: resolve the channel, then
// messages.getMessagesViews(increment: true), 100 ids at a time. Calls
// `done` exactly once (a silent request is cut off by a timeout).
void ViewTarget(
		not_null<Main::Account*> account,
		const ViewsTarget &target,
		Fn<void(bool ok, QString error)> done) {
	const auto answered = std::make_shared<bool>(false);
	const auto finish = [=](bool ok, QString error) {
		if (std::exchange(*answered, true)) {
			return;
		}
		done(ok, error);
	};
	base::call_delayed(kViewsStepTimeout, &Core::App(), [=] {
		finish(false, u"таймаут"_q);
	});
	ResolvePeerForAccount(account, target, [=](
			PeerData *peer,
			QString error) {
		if (!peer) {
			finish(false, error.isEmpty() ? u"канал не найден"_q : error);
			return;
		}
		const auto api = static_cast<MTP::Sender*>(&account->session().api());
		const auto input = peer->input();
		const auto offset = std::make_shared<int>(0);
		const auto next = std::make_shared<Fn<void()>>();
		*next = [=] {
			if (*answered) {
				return;
			}
			const auto total = int(target.ids.size());
			if (*offset >= total) {
				finish(true, QString());
				return;
			}
			const auto count = std::min(kViewsChunk, total - *offset);
			auto ids = QVector<MTPint>();
			for (auto i = 0; i != count; ++i) {
				ids.push_back(MTP_int(target.ids[*offset + i]));
			}
			*offset += count;
			api->request(MTPmessages_GetMessagesViews(
				input,
				MTP_vector<MTPint>(std::move(ids)),
				MTP_bool(true)
			)).done([=](const MTPmessages_MessageViews &) {
				(*next)();
			}).fail([=](const MTP::Error &error) {
				finish(false, error.type());
			}).send();
		};
		(*next)();
	});
}

// Joins the channel/group of `target` from `account`. "Already a member"
// and "join request sent" count as success. Calls `done` exactly once.
void JoinTarget(
		not_null<Main::Account*> account,
		const ViewsTarget &target,
		Fn<void(bool ok, QString note)> done) {
	const auto answered = std::make_shared<bool>(false);
	const auto finish = [=](bool ok, QString note) {
		if (std::exchange(*answered, true)) {
			return;
		}
		done(ok, note);
	};
	base::call_delayed(kViewsStepTimeout, &Core::App(), [=] {
		finish(false, u"таймаут"_q);
	});
	const auto session = &account->session();
	const auto api = static_cast<MTP::Sender*>(&session->api());
	const auto onFail = [=](const MTP::Error &error) {
		const auto type = error.type();
		if (type == u"USER_ALREADY_PARTICIPANT"_q) {
			finish(true, u"уже в группе"_q);
		} else if (type == u"INVITE_REQUEST_SENT"_q) {
			finish(true, u"заявка отправлена"_q);
		} else {
			finish(false, type);
		}
	};
	const auto onDone = [=](const MTPmessages_ChatInviteJoinResult &result) {
		result.match([&](const MTPDmessages_chatInviteJoinResultOk &data) {
			session->api().applyUpdates(data.vupdates());
		}, [](const MTPDmessages_chatInviteJoinResultWebView &) {});
		finish(true, QString());
	};
	if (!target.inviteHash.isEmpty()) {
		base::call_delayed(NextWarmupPause(), &Core::App(), [=] {
			if (*answered) {
				return;
			}
			api->request(MTPmessages_ImportChatInvite(
				MTP_string(target.inviteHash)
			)).done([=, api = api](
				const MTPmessages_ChatInviteJoinResult &result) {
				onDone(result);
			}).fail([=, api = api](const MTP::Error &error) {
				onFail(error);
			}).send();
		});
		return;
	}
	ResolvePeerForAccount(account, target, [=](
			PeerData *peer,
			QString error) {
		if (!peer) {
			finish(false, error.isEmpty() ? u"не найдено"_q : error);
			return;
		}
		const auto channel = peer->asChannel();
		if (!channel) {
			finish(false, u"это не канал и не группа"_q);
			return;
		}
		if (channel->amIn()) {
			finish(true, u"уже в группе"_q);
			return;
		}
		api->request(MTPchannels_JoinChannel(
			channel->inputChannel()
		)).done([=, api = api](
			const MTPmessages_ChatInviteJoinResult &result) {
			onDone(result);
		}).fail([=, api = api](const MTP::Error &error) {
			onFail(error);
		}).send();
	});
}

// ---- run state + progress box ----

struct ViewsRunState {
	std::vector<QString> rowTexts;
	std::vector<std::optional<QColor>> rowColors;
	std::vector<QPointer<Ui::FlatLabel>> rows;

	QString summaryText;
	QPointer<Ui::FlatLabel> summary;

	rpl::variable<QString> buttonText
		= rpl::variable<QString>(u"Остановить"_q);
	std::shared_ptr<bool> cancelled = std::make_shared<bool>(false);
	bool finished = false;

	int nextIndex = 0;
	int done = 0;
	int total = 0;
	int failed = 0;
	int activeLanes = 0;
	int postCount = 0;
	bool join = false;

	std::shared_ptr<Core::ProcessHandle> process;
};

// Views and Join are two independent runs: index 0 = views, 1 = join.
[[nodiscard]] rpl::variable<ViewsIndicator> &IndicatorState(bool join) {
	static auto result = std::array<rpl::variable<ViewsIndicator>, 2>{
		rpl::variable<ViewsIndicator>(ViewsIndicator::None),
		rpl::variable<ViewsIndicator>(ViewsIndicator::None),
	};
	return result[join ? 1 : 0];
}

[[nodiscard]] std::shared_ptr<ViewsRunState> &CurrentRun(bool join) {
	static auto result = std::array<std::shared_ptr<ViewsRunState>, 2>();
	return result[join ? 1 : 0];
}

void FinishRun(not_null<ViewsRunState*> state) {
	if (state->finished) {
		return;
	}
	state->finished = true;
	state->buttonText = u"Готово"_q;
	IndicatorState(state->join) = ViewsIndicator::Done;
	if (state->process) {
		state->process->succeed(state->summaryText);
	}
}

void OpenMode(
	not_null<Window::SessionController*> controller,
	bool join);

void BuildProgressBox(
		std::shared_ptr<Ui::Show> show,
		std::shared_ptr<ViewsRunState> state) {
	show->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setWidth(st::accountsBoxWidth);
		box->setTitle(rpl::single(
			state->join ? u"Вступление"_q : u"Просмотры"_q));
		Ggtg::AddProcessHideButton(box, [=](std::shared_ptr<Ui::Show> to) {
			BuildProgressBox(std::move(to), state);
		});
		Ui::AddSkip(box->verticalLayout());
		state->summary = box->addRow(object_ptr<Ui::FlatLabel>(
			box, state->summaryText, st::boxLabel));
		Ui::AddSkip(box->verticalLayout());
		const auto rows = AddScrollableRowsArea(box->verticalLayout());
		for (auto i = 0; i != int(state->rowTexts.size()); ++i) {
			const auto label = rows->add(
				object_ptr<Ui::FlatLabel>(
					rows, state->rowTexts[i], st::boxLabel),
				st::boxRowPadding);
			if (state->rowColors[i]) {
				label->setTextColorOverride(state->rowColors[i]);
			}
			state->rows[i] = label;
		}
		box->addButton(state->buttonText.value(), [=] {
			if (!state->finished) {
				*state->cancelled = true;
			}
			FinishRun(state.get());
			if (CurrentRun(state->join) == state) {
				CurrentRun(state->join) = nullptr;
				IndicatorState(state->join) = ViewsIndicator::None;
			}
			box->closeBox();
		});
	}));
}

void RunViews(
		not_null<Window::SessionController*> controller,
		Accounts accounts,
		std::vector<ViewsTarget> targets,
		int threads,
		bool join) {
	Expects(!accounts.empty() && !targets.empty());

	if (!Core::GuardAllows(
			controller->uiShow(),
			Core::GuardedAction::StartViews)) {
		return;
	}
	// Accounts from the «Корзина» sleep without a connection - wake the
	// ones this run uses.
	for (const auto &account : accounts) {
		account->wake();
	}
	const auto state = std::make_shared<ViewsRunState>();
	state->join = join;
	state->total = int(accounts.size());
	state->postCount = int(targets.size());
	state->rowTexts.resize(state->total);
	state->rowColors.resize(state->total);
	state->rows.resize(state->total);
	for (auto i = 0; i != state->total; ++i) {
		state->rowTexts[i] = AccountLabel(accounts[i]) + u" — в очереди"_q;
	}
	state->summaryText = u"Готово: 0 / "_q + QString::number(state->total);

	CurrentRun(join) = state;
	IndicatorState(join) = ViewsIndicator::Running;

	const auto weakController = base::make_weak(controller);
	state->process = Core::App().processes().start(
		(join
			? u"Вступление (%1 акк., %2 ссыл.)"_q
			: u"Просмотры (%1 акк., %2 пост.)"_q).arg(state->total).arg(
				state->postCount),
		[=] {
			if (const auto alive = CurrentSessionController(weakController.get())) {
				OpenMode(alive, join);
			}
		},
		Core::ProcessKind::Views);

	BuildProgressBox(controller->uiShow(), state);

	const auto setRow = [=](
			int i,
			const QString &text,
			std::optional<QColor> color) {
		state->rowTexts[i] = text;
		state->rowColors[i] = color;
		if (const auto label = state->rows[i].data()) {
			label->setText(text);
			label->setTextColorOverride(color);
		}
		if (state->process) {
			state->process->log(text);
		}
	};
	const auto setSummary = [=] {
		state->summaryText = u"Готово: "_q + QString::number(state->done)
			+ u" / "_q + QString::number(state->total)
			+ (state->failed
				? (u", с ошибками: "_q + QString::number(state->failed))
				: QString());
		if (const auto label = state->summary.data()) {
			label->setText(state->summaryText);
		}
		if (state->process) {
			state->process->progress(state->done, state->total);
		}
	};

	const auto shared = std::make_shared<std::vector<ViewsTarget>>(
		std::move(targets));
	const auto runLane = std::make_shared<Fn<void()>>();
	*runLane = [=] {
		if (*state->cancelled) {
			return;
		}
		if (state->nextIndex >= state->total) {
			if (--state->activeLanes == 0) {
				FinishRun(state.get());
			}
			return;
		}
		const auto i = state->nextIndex++;
		const auto account = accounts[i];
		setRow(i, AccountLabel(account) + (join ? u" — вступает..."_q : u" — смотрит..."_q), std::nullopt);
		const auto okCount = std::make_shared<int>(0);
		const auto errors = std::make_shared<QStringList>();
		const auto notes = std::make_shared<QStringList>();
		const auto index = std::make_shared<int>(0);
		const auto step = std::make_shared<Fn<void()>>();
		*step = [=] {
			if (*state->cancelled) {
				return;
			}
			if (*index >= int(shared->size())) {
				const auto success = errors->isEmpty();
				setRow(
					i,
					AccountLabel(account) + u" — "_q + (success
						? ((join
							? u"вступил: "_q
							: u"просмотрено постов: "_q)
							+ QString::number(*okCount)
							+ (notes->isEmpty()
								? QString()
								: (u" ("_q + notes->join(u", "_q) + ')')))
						: (u"ошибок: "_q + QString::number(errors->size())
							+ u" ("_q + errors->front() + ')')),
					SmsRowColor(!success && !*okCount, success));
				++state->done;
				if (!success) {
					++state->failed;
				}
				setSummary();
				base::call_delayed(
					NextAccountPause(),
					&Core::App(),
					[=] { (*runLane)(); });
				return;
			}
			const auto &target = (*shared)[(*index)++];
			const auto action = join ? JoinTarget : ViewTarget;
			action(account, target, [=](bool ok, QString error) {
				if (ok) {
					++*okCount;
					if (!error.isEmpty()) {
						notes->push_back(target.label + u": "_q + error);
					}
				} else {
					errors->push_back(target.label + u": "_q + error);
				}
				(*step)();
			});
		};
		(*step)();
	};
	const auto lanes = std::clamp(threads, 1, state->total);
	state->activeLanes = lanes;
	for (auto lane = 0; lane != lanes; ++lane) {
		(*runLane)();
	}
}

// ---- setup box ----

struct ViewsFormState : AccountPickerState {
	QPointer<Ui::InputField> links;
	QPointer<Ui::InputField> joinLinks;
	QPointer<Ui::InputField> threads;
};

// Survives closing the box: the last-used links/threads come back on the
// next open within this app run.
struct ViewsFormMemory {
	QString links;
	QString threads = u"1"_q;
};

[[nodiscard]] ViewsFormMemory &FormMemory(bool join) {
	static auto result = std::array<ViewsFormMemory, 2>();
	return result[join ? 1 : 0];
}

[[nodiscard]] object_ptr<Ui::BoxContent> MakeSetupBox(
		not_null<Window::SessionController*> controller,
		bool initialJoin) {
	return Box([=](not_null<Ui::GenericBox*> box) {
		box->setWidth(st::accountsBoxWidth);
		box->setTitle(rpl::single(u"Накрутка"_q));

		const auto state = box->lifetime().make_state<ViewsFormState>();
		const auto joinMode = box->lifetime().make_state<bool>(initialJoin);

		Ui::AddSubsectionTitle(
			box->verticalLayout(), rpl::single(u"1. Что делать"_q));
		const auto group = std::make_shared<Ui::RadiobuttonGroup>(
			initialJoin ? 1 : 0);
		box->addRow(
			object_ptr<Ui::Radiobutton>(
				box, group, 0, u"Просмотры постов"_q, st::defaultCheckbox),
			st::ggtgChoiceRowPadding);
		box->addRow(
			object_ptr<Ui::Radiobutton>(
				box, group, 1, u"Вступление в каналы и группы"_q,
				st::defaultCheckbox),
			st::ggtgChoiceRowPadding);
		Ui::AddSkip(box->verticalLayout());
		Ui::AddDivider(box->verticalLayout());

		Ui::AddSubsectionTitle(
			box->verticalLayout(), rpl::single(u"2. Аккаунты"_q));
		AddAccountPicker(box, state);
		Ui::AddDivider(box->verticalLayout());

		Ui::AddSubsectionTitle(
			box->verticalLayout(), rpl::single(u"3. Ссылки"_q));
		// One block per mode, only the picked one shown; each keeps its own
		// text.
		const auto addLinksBlock = [&](
				bool join,
				const QString &placeholder,
				const QString &hint) {
			const auto wrap = box->addRow(
				object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
					box,
					object_ptr<Ui::VerticalLayout>(box)));
			const auto inner = wrap->entity();
			const auto field = inner->add(
				object_ptr<Ui::InputField>(
					inner,
					st::defaultInputField,
					Ui::InputField::Mode::MultiLine,
					rpl::single(placeholder),
					FormMemory(join).links));
			inner->add(
				object_ptr<Ui::FlatLabel>(
					inner, hint, st::boxDividerLabel));
			wrap->toggle(join == initialJoin, anim::type::instant);
			(join ? state->joinLinks : state->links) = field;
			return wrap;
		};
		const auto viewsWrap = addLinksBlock(
			false,
			u"https://t.me/channel/123  ·  https://t.me/channel/123-145"_q,
			u"По одной ссылке на строку: t.me/канал/123, закрытый "
				"t.me/c/ID/123 (аккаунт должен быть участником). Через "
				"дефис в конце - сразу несколько постов подряд (максимум "
				"500)."_q);
		const auto joinWrap = addLinksBlock(
			true,
			u"https://t.me/channel  ·  @group  ·  https://t.me/+AbCdEf123"_q,
			u"По одной ссылке на строку: t.me/канал, @канал, t.me/+хеш "
				"или t.me/joinchat/хеш. Без диапазонов."_q);
		group->setChangedCallback([=](int value) {
			*joinMode = (value == 1);
			viewsWrap->toggle(value == 0, anim::type::instant);
			joinWrap->toggle(value == 1, anim::type::instant);
		});
		Ui::AddSkip(box->verticalLayout());
		Ui::AddDivider(box->verticalLayout());

		Ui::AddSubsectionTitle(
			box->verticalLayout(), rpl::single(u"4. Потоки"_q));
		state->threads = box->addRow(object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			rpl::single(u"Потоков (одновременно)"_q),
			FormMemory(false).threads));
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			u"Сколько аккаунтов работают одновременно."_q,
			st::boxDividerLabel));
		Ui::AddSkip(box->verticalLayout());

		const auto remember = [=] {
			if (const auto field = state->links.data()) {
				FormMemory(false).links = field->getLastText();
			}
			if (const auto field = state->joinLinks.data()) {
				FormMemory(true).links = field->getLastText();
			}
			if (const auto field = state->threads.data()) {
				FormMemory(false).threads = field->getLastText();
			}
		};
		box->boxClosing() | rpl::on_next(remember, box->lifetime());

		box->addButton(rpl::single(u"Запустить"_q), [=] {
			const auto join = *joinMode;
			auto accounts = Main::LiveAccounts(state->selected);
			if (accounts.empty()) {
				Ggtg::Notice(controller->uiShow(), 
					u"Выберите хотя бы один аккаунт"_q);
				return;
			}
			auto bad = QStringList();
			const auto field = join ? state->joinLinks : state->links;
			const auto linksText = field
				? field->getLastText()
				: QString();
			auto targets = join
				? ParseJoinLinks(linksText, &bad)
				: ParseViewsLinks(linksText, &bad);
			if (!bad.isEmpty()) {
				Ggtg::Notice(controller->uiShow(), 
					u"Не удалось разобрать: "_q + bad.front());
				return;
			}
			if (targets.empty()) {
				Ggtg::Notice(controller->uiShow(), 
					u"Укажите хотя бы одну ссылку"_q);
				return;
			}
			auto ok = false;
			auto threads = state->threads
				? state->threads->getLastText().trimmed().toInt(&ok)
				: 1;
			if (!ok || threads < 1) {
				threads = 1;
			}
			remember();
			box->closeBox();
			RunViews(
				controller,
				std::move(accounts),
				std::move(targets),
				threads,
				join);
		}, st::defaultActiveButton);
		Ggtg::AddCloseCross(box);
	});
}

void OpenMode(
		not_null<Window::SessionController*> controller,
		bool join) {
	if (const auto state = CurrentRun(join)) {
		BuildProgressBox(controller->uiShow(), state);
		return;
	}
	controller->show(MakeSetupBox(controller, join));
}

} // namespace

// One menu row for both: Running while either run is going, else Done if
// either just finished.
rpl::producer<ViewsIndicator> ViewsIndicatorValue() {
	return rpl::combine(
		IndicatorState(false).value(),
		IndicatorState(true).value()
	) | rpl::map([](ViewsIndicator views, ViewsIndicator join) {
		if (views == ViewsIndicator::Running
			|| join == ViewsIndicator::Running) {
			return ViewsIndicator::Running;
		} else if (views == ViewsIndicator::Done
			|| join == ViewsIndicator::Done) {
			return ViewsIndicator::Done;
		}
		return ViewsIndicator::None;
	});
}

void OpenViews(not_null<Window::SessionController*> controller) {
	// A running (or just finished) run's progress box comes first.
	if (CurrentRun(false)) {
		OpenMode(controller, false);
	} else if (CurrentRun(true)) {
		OpenMode(controller, true);
	} else {
		controller->show(MakeSetupBox(controller, false));
	}
}

object_ptr<Ui::BoxContent> MakeViewsSetupBoxFresh(
		not_null<Window::SessionController*> controller) {
	return MakeSetupBox(controller, false);
}

} // namespace Settings
