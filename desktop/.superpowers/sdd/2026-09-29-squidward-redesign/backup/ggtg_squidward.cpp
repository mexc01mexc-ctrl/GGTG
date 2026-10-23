/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and exceptions, see https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "window/ggtg_squidward.h"

#include "window/window_main_menu.h"
#include "apiwrap.h"
#include "base/event_filter.h"
#include "base/flat_set.h"
#include "base/qt_signal_producer.h"
#include "base/timer.h"
#include "mainwindow.h"
#include "settings/process_box.h"
#include "boxes/about_box.h"
#include "core/update_channel.h"
#include "boxes/peer_list_controllers.h"
#include "boxes/premium_preview_box.h"
#include "calls/group/calls_group_common.h"
#include "calls/calls_box_controller.h"
#include "calls/calls_instance.h"
#include "core/application.h"
#include "core/click_handler_types.h"
#include "data/data_changes.h"
#include "data/data_document_media.h"
#include "data/data_folder.h"
#include "data/data_group_call.h"
#include "data/data_channel.h"
#include "base/call_delayed.h"
#include "data/data_session.h"
#include "data/data_stories.h"
#include "data/data_user.h"
#include "info/info_memento.h"
#include "info/profile/info_profile_badge.h"
#include "settings/settings_common.h"
#include "info/profile/info_profile_emoji_status_panel.h"
#include "info/profile/info_profile_icon.h"
#include "info/stories/info_stories_widget.h"
#include "lang/lang_keys.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "main/main_session_settings.h"
#include "mtproto/mtproto_config.h"
#include "settings/sections/settings_advanced.h"
#include "settings/sections/settings_calls.h"
#include "settings/sections/settings_information.h"
#include "settings/sections/settings_processes.h"
#include "core/ggtg_log.h"
#include "core/process_manager.h"
#include "storage/localstorage.h"
#include "storage/storage_account.h"
#include "support/support_templates.h"
#include "tde2e/tde2e_api.h"
#include "tde2e/tde2e_integration.h"
#include "ui/boxes/confirm_box.h"
#include "ui/chat/chat_theme.h"
#include "ui/controls/swipe_handler.h"
#include "ui/controls/userpic_button.h"
#include "ui/effects/snowflakes.h"
#include "ui/effects/toggle_arrow.h"
#include "ui/painter.h"
#include "ui/text/text_options.h"
#include "ui/text/text_utilities.h"
#include "ui/ui_utility.h"
#include "ui/unread_badge_paint.h"
#include "ui/vertical_list.h"
#include "ui/widgets/menu/menu_add_action_callback_factory.h"
#include "ui/widgets/popup_menu.h"
#include "ui/widgets/scroll_area.h"
#include "ui/widgets/shadow.h"
#include "ui/wrap/slide_wrap.h"
#include "window/themes/window_theme.h"
#include "window/window_controller.h"
#include "window/window_main_menu_helpers.h"
#include "boxes/connection_box.h"
#include "settings/bulk_accounts_actions.h"
#include "data/scraped_members_db.h"
#include "settings/sms_campaign.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/labels.h"
#include "api/api_bot.h"
#include "api/api_common.h"
#include "base/weak_ptr.h"
#include "base/weak_qptr.h"
#include "data/data_peer_id.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_item_components.h"
#include "history/history_item_reply_markup.h"
#include "mtproto/sender.h"
#include "ui/abstract_button.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/separate_panel.h"
#include "ui/wrap/vertical_layout.h"
#include "styles/style_layers.h"
#include "window/window_peer_menu.h"
#include "window/window_session_controller.h"
#include "styles/style_chat.h" // popupMenuExpandedSeparator
#include "styles/style_dialogs.h" // dialogsMenuToggle, for the sidebar collapse button
#include "styles/style_menu_icons.h"
#include "styles/style_boxes.h"
#include "styles/style_settings.h"
#include "styles/style_widgets.h"
#include "styles/style_window.h"
#include "styles/style_window_main_menu.h"
#include <QtGui/QWindow>
#include <QtGui/QScreen>
#include <QtGui/QGuiApplication>
#include <QtGui/QClipboard>
#include <QtCore/QRegularExpression>
#include <QtCore/QPointer>
#include "ui/text/text_entity.h"

namespace Window {
namespace {

// "Сквидвард": a curated front end for two of @skvidvarde_bot's lookup
// functions (by-user and by-group/channel) - resolves the bot, sends
// exactly what typing the identifier by hand would, and mirrors the
// exchange (including any keyboard the bot replies with, e.g. pagination
// on a result) as it would look in a normal chat. The bot has several
// other lookup kinds (by sticker, by forwarded message/post, text/name
// search) and its own paid-extras menu (tokens, invites, incognito,
// mirrors) - deliberately left unexposed here rather than mirrored as a
// free-text chat, so this box only ever does the two things it's for.
// Results of both lookups can be saved into "База" (see
// SquidwardShowGroupsResult / SquidwardShowUsersResult).
constexpr auto kSquidwardBotUsername = "skvidvarde_bot";
constexpr auto kSquidwardLogHeight = 260;
// The on-screen log keeps only its newest rows (each is a widget); the
// full history goes to ggtg.log.
constexpr auto kSquidwardMaxLogRows = 200;

struct SquidwardBotState {
	UserData *bot = nullptr;
	Ui::VerticalLayout *log = nullptr;
	Ui::FlatLabel *status = nullptr;
	mtpRequestId resolveRequestId = 0;
	// Set right before sending a group/channel identifier (see
	// promptAndSend's group case below), consumed by the very next bot
	// message: that reply is the group's own profile card, and this is
	// what makes this box auto-click its "Топ, люди" button (free) itself
	// instead of making the user find and press it - see
	// SquidwardCollectNextPage's callers.
	bool autoFetchTopPeopleNext = false;
	// Same idea for a user lookup: the profile card's "Группы" button.
	bool autoFetchGroupsNext = false;
	// Whatever was last typed into either prompt - used as the new base's
	// name if a collected list is saved to "База" (see
	// SquidwardShowGroupsResult/SquidwardShowUsersResult): the looked-up
	// user's own identifier for a "Группы" collection, or the looked-up
	// group's identifier for a "Топ, люди" one, exactly per the ask that
	// each base be named after whichever one it came from.
	QString lastLookupIdentifier;
	// A group to look up as soon as the bot is connected (see
	// MakeSquidwardBox's `lookupGroup`); cleared once sent.
	QString pendingGroupLookup;
	// Guards SquidwardStartCollection's wait for the bot's response to a
	// button click (new message or edit, see its own comment) - reset
	// (replacing, so destroying, whatever the previous attempt's own
	// timer was) on every fresh attempt, so a click that genuinely never
	// gets any answer still ends in a clear status instead of leaving
	// "Запрашиваем список..." on screen forever with nothing to show.
	std::shared_ptr<base::Timer> pendingCollectionTimeout;
};

constexpr auto kSquidwardResultBoxWidth = 540;

[[nodiscard]] not_null<Ui::VerticalLayout*> SquidwardAddLogArea(
		not_null<Ui::VerticalLayout*> container) {
	auto wrapOwned = object_ptr<Ui::RpWidget>(container);
	const auto wrap = wrapOwned.data();
	const auto scroll = Ui::CreateChild<Ui::ScrollArea>(wrap, st::boxScroll);
	wrap->widthValue(
	) | rpl::on_next([=](int width) {
		scroll->setGeometry(0, 0, width, kSquidwardLogHeight);
	}, wrap->lifetime());
	wrap->resize(wrap->width(), kSquidwardLogHeight);
	container->add(std::move(wrapOwned), st::boxRowPadding);

	const auto inner = scroll->setOwnedWidget(
		object_ptr<Ui::VerticalLayout>(scroll)).data();
	scroll->widthValue(
	) | rpl::on_next([=](int width) {
		inner->resizeToWidth(width);
	}, inner->lifetime());
	return inner;
}

void SquidwardAppendLogLine(
		not_null<Ui::VerticalLayout*> log,
		const QString &text) {
	Core::GgtgLog::Info(u"Сквидвард"_q, text);
	log->add(
		object_ptr<Ui::FlatLabel>(log, text, st::boxLabel),
		st::boxRowPadding);
	while (log->count() > kSquidwardMaxLogRows) {
		delete log->widgetAt(0).get();
	}
}

// Every "what's happening right now" update (bot connected, sending,
// paginating, done, or nothing found) goes through here instead of only
// the one-line status label above the log - that label is easy to miss
// mid-flow, so every update it gets is also dropped into the scrollable
// log itself, which is what "нет лога" (the automation runs with no
// visible trace of what it's doing) was actually asking for.
void SquidwardSetStatus(
		not_null<SquidwardBotState*> state,
		const QString &text) {
	state->status->setText(text);
	SquidwardAppendLogLine(state->log, u"• "_q + text);
}

// ---- auto-paginated collection: "Группы" (a user's own chats) and
// "Топ, люди" (a group's members) both come back as a bot message with an
// "N из M" page counter and a "➡" button to advance it - one manual click
// per page (each one free after the listing's own one-time cost, if any)
// is the tedious part this automates away, not the cost itself.

// A markup button's own label, once any leading icon/cost prefix
// ("15💎 ", "🏆 ", ...) is stripped down to its last word - matched
// against by exact equality so e.g. "Группы" doesn't also match "Похожие
// группы", and "Топ, люди" matches on "люди" specifically.
[[nodiscard]] QString SquidwardButtonLastWord(const QString &text) {
	const auto parts = text.simplified().split(' ', Qt::SkipEmptyParts);
	if (parts.isEmpty()) {
		return QString();
	}
	// The bot sprinkles Latin/Greek lookalikes into its own texts, so
	// fold the common ones back to Cyrillic before comparing.
	static const auto fold = QHash<QChar, QChar>{
		{ QChar(u'a'), QChar(u'а') }, { QChar(u'c'), QChar(u'с') },
		{ QChar(u'e'), QChar(u'е') }, { QChar(u'o'), QChar(u'о') },
		{ QChar(u'p'), QChar(u'р') }, { QChar(u'x'), QChar(u'х') },
		{ QChar(u'y'), QChar(u'у') }, { QChar(u'r'), QChar(u'г') },
		{ QChar(0x03BF), QChar(u'о') }, { QChar(0x03B3), QChar(u'у') }, { QChar(0x03C1), QChar(u'р') },
		{ QChar(0x03B1), QChar(u'а') }, { QChar(0x03C0), QChar(u'п') },
		{ QChar(0x03B7), QChar(u'н') }, { QChar(0x03BD), QChar(u'н') },
		{ QChar(0x1D0F), QChar(u'о') }, { QChar(0x2113), QChar(u'л') },
	};
	auto word = parts.back().toLower();
	for (auto &ch : word) {
		ch = fold.value(ch, ch);
	}
	return word;
}

[[nodiscard]] std::optional<std::pair<int, int>> SquidwardFindButton(
		const HistoryMessageMarkupData &markup,
		Fn<bool(const QString &text)> match) {
	for (auto row = 0; row != int(markup.rows.size()); ++row) {
		const auto &buttons = markup.rows[row];
		for (auto column = 0; column != int(buttons.size()); ++column) {
			if (match(buttons[column].text)) {
				return std::make_pair(row, column);
			}
		}
	}
	return std::nullopt;
}

// Never matches a button that itself carries a "💎" price, so this can
// never be tricked into auto-clicking a paid action - only ever the bot's
// own plain "➡" page-advance buttons are free navigation in both listings
// this is used for.
[[nodiscard]] std::optional<std::pair<int, int>> SquidwardFindNextPageButton(
		const HistoryMessageMarkupData &markup) {
	return SquidwardFindButton(markup, [](const QString &text) {
		return text.simplified().startsWith(u"➡"_q)
			&& !text.contains(u"💎"_q);
	});
}

// The bot's own "страница N из M" counter, wherever it sits in the text.
[[nodiscard]] std::optional<std::pair<int, int>> SquidwardParsePageInfo(
		const QString &text) {
	static const auto expression = QRegularExpression(
		u"страниц\\D*(\\d+)\\D+(\\d+)"_q);
	const auto match = expression.match(text);
	if (!match.hasMatch()) {
		return std::nullopt;
	}
	return std::make_pair(
		match.capturedView(1).toInt(),
		match.capturedView(2).toInt());
}

template <typename Entry>
using SquidwardExtractFn = Fn<void(
	not_null<HistoryItem*>,
	std::vector<Entry>&)>;

// A plain public "t.me/name" (or "telegram.me/name") link's bare username,
// or empty for anything else.
[[nodiscard]] QString SquidwardUsernameFromLink(const QString &link) {
	static const auto expression = QRegularExpression(
		u"^(?:https?://)?t(?:elegram)?\\.me/([A-Za-z0-9_]{4,32})/?$"_q);
	const auto match = expression.match(link.trimmed());
	return match.hasMatch() ? match.captured(1) : QString();
}

// A closed group's link: "t.me/c/<id>[/<message>]", or a private invite
// ("t.me/+AbCdEf", "t.me/joinchat/..."). Empty for anything else.
[[nodiscard]] QString SquidwardPrivateLink(const QString &link) {
	static const auto expression = QRegularExpression(
		u"^(?:https?://)?t(?:elegram)?\\.me/(?:c/\\d+(?:/\\d+)?|\\+[A-Za-z0-9_-]+|joinchat/[A-Za-z0-9_-]+)/?$"_q);
	const auto trimmed = link.trimmed();
	return expression.match(trimmed).hasMatch() ? trimmed : QString();
}

// "Группы": each line is a chat name shown as a clickable link - a plain
// "@username" mention, a bare t.me/username URL, or a closed group's link
// (t.me/c/... or an invite), either as bare text or as a CustomUrl whose
// *display text* is the chat's title and whose own data() is the link. The
// closed ones are kept with that link (ScrapedGroupEntry::link) so "База"
// can mark them with a 🔒 and look them up through Сквидвард.
void SquidwardExtractGroupEntries(
		not_null<HistoryItem*> item,
		std::vector<Data::ScrapedGroupEntry> &into) {
	const auto text = item->originalText();
	const auto addLink = [&](const QString &shown, const QString &link) {
		const auto username = SquidwardUsernameFromLink(link);
		if (!username.isEmpty()) {
			into.push_back({ .username = username });
			return;
		}
		const auto closed = SquidwardPrivateLink(link);
		into.push_back(closed.isEmpty()
			? Data::ScrapedGroupEntry{ .title = shown }
			: Data::ScrapedGroupEntry{
				.title = (shown == closed) ? QString() : shown,
				.link = closed,
			});
	};
	for (const auto &entity : text.entities) {
		const auto substr = text.text.mid(entity.offset(), entity.length());
		if (entity.type() == EntityType::Mention) {
			into.push_back({ .username = substr.mid(1) });
		} else if (entity.type() == EntityType::Url) {
			addLink(substr, substr);
		} else if (entity.type() == EntityType::CustomUrl) {
			addLink(substr, entity.data());
		}
	}
}

// "Топ, люди": each line names a member two different ways depending on
// the bot's mood, it turns out - most commonly as a plain "t.me/username"
// link (same shape as "Группы", see SquidwardExtractGroupEntries below and
// SquidwardUsernameFromLink's own comment), sometimes instead as a
// MentionName entity (a Bot API "text_mention" - display text plus a
// hidden user id+access hash, decoded via MentionNameDataToFields) for
// someone this account has never interacted with and so can't resolve
// locally - see SquidwardResolveMentionedUsers, which is what turns *that*
// case into a username (or "no public username", left out same as always)
// once the pages are all in. `username` is already known outright for the
// link case, empty (with `userId` set instead) for the id-only case.
struct SquidwardMentionedUser {
	QString username;
	uint64 userId = 0;
	uint64 accessHash = 0;
};

void SquidwardExtractUsernames(
		not_null<HistoryItem*> item,
		std::vector<SquidwardMentionedUser> &into) {
	const auto text = item->originalText();
	for (const auto &entity : text.entities) {
		if (entity.type() == EntityType::MentionName) {
			const auto fields = TextUtilities::MentionNameDataToFields(
				entity.data());
			if (fields.userId) {
				into.push_back({
					.userId = fields.userId,
					.accessHash = fields.accessHash,
				});
			}
		} else if (entity.type() == EntityType::Url) {
			const auto substr = text.text.mid(
				entity.offset(),
				entity.length());
			const auto username = SquidwardUsernameFromLink(substr);
			if (!username.isEmpty()) {
				into.push_back({ .username = username });
			}
		} else if (entity.type() == EntityType::CustomUrl) {
			const auto username = SquidwardUsernameFromLink(entity.data());
			if (!username.isEmpty()) {
				into.push_back({ .username = username });
			}
		}
	}
}

// Batch-fetches whichever of `pending` aren't already loaded locally (100
// per request, sequentially - the API limits how many inputUsers a single
// users.getUsers can take) so their username (or lack of one) can actually
// be read, then hands the deduplicated, resolved list to
// SquidwardShowUsersResult. A chunk that fails is skipped rather than
// aborting the rest - a handful of stale access hashes shouldn't lose
// everyone else's already-successful pages.
void SquidwardResolveMentionedUsers(
		not_null<Main::Session*> session,
		QVector<MTPInputUser> pending,
		Fn<void()> done) {
	if (pending.isEmpty()) {
		done();
		return;
	}
	constexpr auto kChunk = 100;
	const auto count = std::min<int>(kChunk, pending.size());
	auto chunk = pending.mid(0, count);
	auto rest = pending.mid(count);
	session->api().request(MTPusers_GetUsers(
		MTP_vector<MTPInputUser>(std::move(chunk))
	)).done([=](const MTPVector<MTPUser> &result) {
		session->data().processUsers(result);
		SquidwardResolveMentionedUsers(session, std::move(rest), done);
	}).fail([=] {
		SquidwardResolveMentionedUsers(session, std::move(rest), done);
	}).send();
}

// `baseTitle` is whatever the user typed into "Пробить пользователя" that
// led here (see SquidwardBotState::lastLookupIdentifier) - saved as-is as
// the new base's name, per the explicit ask: name the groups base after
// whose groups they are.
void SquidwardShowGroupsResult(
		std::shared_ptr<Ui::Show> show,
		const QString &baseTitle,
		std::vector<Data::ScrapedGroupEntry> entries) {
	show->showBox(Box([=](not_null<Ui::GenericBox*> box) {
		// Wide enough for all three buttons in one row; at the default
		// width the middle one was clipped.
		box->setWidth(kSquidwardResultBoxWidth);
		box->setTitle(rpl::single(
			u"Группы ("_q + QString::number(entries.size()) + ')'));
		auto lines = QStringList();
		for (const auto &entry : entries) {
			lines.push_back(!entry.username.isEmpty()
				? ('@' + entry.username)
				: (!entry.link.isEmpty() ? entry.link : entry.title));
		}
		const auto joined = lines.join('\n');
		const auto area = SquidwardAddLogArea(box->verticalLayout());
		SquidwardAppendLogLine(
			area,
			entries.empty() ? u"Ничего не найдено."_q : joined);
		box->addButton(rpl::single(u"Копировать всё"_q), [=] {
			QGuiApplication::clipboard()->setText(joined);
		});
		box->addButton(rpl::single(u"В базу (Группы)"_q), [=] {
			Data::SaveScrapedGroupEntries(
				Data::GenerateManualBaseId(),
				baseTitle,
				entries);
			show->showToast(u"Сохранено в базу групп: "_q + baseTitle);
		});
		// addLeftButton, not addButton: with two action buttons already
		// on the right, a third one there could get squeezed past the
		// box's left edge and become unreachable - the far-left slot is
		// always clear regardless of how the other two wrap.
		box->addLeftButton(rpl::single(u"Закрыть"_q), [=] { box->closeBox(); });
	}), Ui::LayerOption::KeepOther);
}

// The bot's "Группы" list mixes groups with broadcast channels, and channels
// are of no use as a recipients source. Every named entry is looked up (the
// local cache first, then contacts.resolveUsername, one at a time with a
// pause so the lookups don't trip a flood limit); broadcast channels are
// dropped, and the found group's title and size fill in what the bot's text
// didn't carry. An entry that can't be resolved right now is kept as it is -
// better one channel too many than a real group lost to a network hiccup.
void SquidwardFilterAndShowGroups(
		not_null<Window::SessionController*> controller,
		std::shared_ptr<Ui::Show> show,
		const QString &baseTitle,
		std::vector<Data::ScrapedGroupEntry> entries) {
	struct State {
		std::vector<Data::ScrapedGroupEntry> source;
		std::vector<Data::ScrapedGroupEntry> kept;
		size_t next = 0;
		int channels = 0;
	};
	const auto session = &controller->session();
	const auto state = std::make_shared<State>();
	state->source = std::move(entries);
	if (!state->source.empty()) {
		show->showToast(u"Отделяем группы от каналов..."_q);
	}
	const auto step = std::make_shared<Fn<void()>>();
	const auto advance = [=](crl::time delay) {
		base::call_delayed(delay, session, [=] { (*step)(); });
	};
	// `peer` null: unknown, keep the entry untouched.
	const auto settle = [=](
			Data::ScrapedGroupEntry entry,
			PeerData *peer,
			crl::time delay) {
		const auto channel = peer ? peer->asChannel() : nullptr;
		if (channel && !channel->isMegagroup()) {
			++state->channels;
		} else {
			if (channel) {
				if (entry.title.isEmpty()) {
					entry.title = channel->name();
				}
				entry.membersCount = std::max(
					entry.membersCount,
					channel->membersCount());
			}
			state->kept.push_back(std::move(entry));
		}
		advance(delay);
	};
	*step = [=] {
		if (state->next >= state->source.size()) {
			const auto channels = state->channels;
			auto result = std::move(state->kept);
			// Break the step <-> closure cycle now that the work is done -
			// not from inside the very function being destroyed.
			base::call_delayed(0, session, [=] { *step = nullptr; });
			if (channels) {
				show->showToast(
					u"Каналов убрано: "_q + QString::number(channels));
			}
			SquidwardShowGroupsResult(show, baseTitle, std::move(result));
			return;
		}
		auto entry = state->source[state->next++];
		if (entry.username.isEmpty()) {
			state->kept.push_back(std::move(entry));
			advance(0);
			return;
		}
		if (const auto peer = session->data().peerByUsername(
				entry.username)) {
			settle(std::move(entry), peer, 0);
			return;
		}
		session->api().request(MTPcontacts_ResolveUsername(
			MTP_flags(MTPcontacts_ResolveUsername::Flags(0)),
			MTP_string(entry.username),
			MTP_string(QString())
		)).done([=](const MTPcontacts_ResolvedPeer &result) {
			auto peer = (PeerData*)nullptr;
			result.match([&](const MTPDcontacts_resolvedPeer &data) {
				session->data().processUsers(data.vusers());
				session->data().processChats(data.vchats());
				const auto peerId = peerFromMTP(data.vpeer());
				peer = peerId ? session->data().peer(peerId).get() : nullptr;
			});
			settle(entry, peer, 400);
		}).fail([=] {
			settle(entry, nullptr, 800);
		}).send();
	};
	(*step)();
}

// `baseTitle` is whatever group/channel identifier led here (see
// SquidwardBotState::lastLookupIdentifier) - saved as the new recipients
// base's name, per the explicit ask: name it after the group the people
// were pulled from.
// The looked-up group's own name plus "(@username)", so the saved base reads
// the same as one scraped from the group itself and is matched with it in
// "База"; falls back to whatever was typed if it can't be resolved.
void SquidwardGroupTitle(
		not_null<Main::Session*> session,
		const QString &typed,
		Fn<void(QString)> done) {
	const auto parsed = Data::ParseUsernames({ typed });
	if (parsed.valid.isEmpty()) {
		done(typed);
		return;
	}
	const auto username = parsed.valid.front();
	if (const auto peer = session->data().peerByUsername(username)) {
		done(Data::GroupBaseTitle(peer->name(), username));
		return;
	}
	session->api().request(MTPcontacts_ResolveUsername(
		MTP_flags(MTPcontacts_ResolveUsername::Flags(0)),
		MTP_string(username),
		MTP_string(QString())
	)).done([=](const MTPcontacts_ResolvedPeer &result) {
		result.match([&](const MTPDcontacts_resolvedPeer &data) {
			session->data().processUsers(data.vusers());
			session->data().processChats(data.vchats());
			const auto peerId = peerFromMTP(data.vpeer());
			const auto peer = peerId
				? session->data().peer(peerId).get()
				: nullptr;
			done(peer ? Data::GroupBaseTitle(peer->name(), username) : typed);
		});
	}).fail([=] {
		done(typed);
	}).send();
}

void SquidwardShowUsersResult(
		not_null<Main::Session*> session,
		std::shared_ptr<Ui::Show> show,
		const QString &baseTitle,
		std::vector<QString> usernames) {
	show->showBox(Box([=](not_null<Ui::GenericBox*> box) {
		// Wide enough for all three buttons in one row; at the default
		// width the middle one was clipped.
		box->setWidth(kSquidwardResultBoxWidth);
		box->setTitle(rpl::single(
			u"Топ, люди ("_q + QString::number(usernames.size()) + ')'));
		const auto joined = QStringList(usernames.begin(), usernames.end())
			.join('\n');
		const auto area = SquidwardAddLogArea(box->verticalLayout());
		SquidwardAppendLogLine(
			area,
			usernames.empty() ? u"Ничего не найдено."_q : joined);
		box->addButton(rpl::single(u"Копировать всё"_q), [=] {
			QGuiApplication::clipboard()->setText(joined);
		});
		box->addButton(rpl::single(u"В базу (Получатели)"_q), [=] {
			auto stripped = QStringList();
			for (const auto &name : usernames) {
				stripped.push_back(
					name.startsWith('@') ? name.mid(1) : name);
			}
			SquidwardGroupTitle(session, baseTitle, [=](QString title) {
				Data::SaveScrapedGroupUsernames(
					Data::GenerateManualBaseId(),
					title,
					stripped);
				show->showToast(u"Сохранено в базу получателей: "_q + title);
			});
		});
		// See SquidwardShowGroupsResult's identical comment: addLeftButton
		// keeps this reachable regardless of how wide the other two are.
		box->addLeftButton(rpl::single(u"Закрыть"_q), [=] { box->closeBox(); });
	}), Ui::LayerOption::KeepOther);
}

void SquidwardResolveAndShowUsers(
		not_null<Window::SessionController*> controller,
		not_null<SquidwardBotState*> state,
		std::shared_ptr<Ui::Show> show,
		const QString &baseTitle,
		std::vector<SquidwardMentionedUser> mentioned) {
	const auto session = &controller->session();
	// Only the id-only entries (see SquidwardMentionedUser's own comment)
	// need resolving at all - anything that already came with a username
	// straight off its link goes through `mentioned` again, untouched, once
	// `finish` runs below.
	auto seenIds = base::flat_set<uint64>();
	auto pendingIds = std::vector<SquidwardMentionedUser>();
	auto toResolve = QVector<MTPInputUser>();
	for (const auto &entry : mentioned) {
		if (!entry.username.isEmpty() || !entry.userId) {
			continue;
		}
		if (!seenIds.insert(entry.userId).second) {
			continue;
		}
		pendingIds.push_back(entry);
		if (!session->data().userLoaded(UserId(entry.userId))) {
			toResolve.push_back(MTP_inputUser(
				MTP_long(entry.userId),
				MTP_long(entry.accessHash)));
		}
	}
	const auto finish = [=] {
		auto seen = base::flat_set<QString>();
		auto usernames = std::vector<QString>();
		const auto add = [&](const QString &raw) {
			if (raw.isEmpty()
					|| raw.endsWith(u"bot"_q, Qt::CaseInsensitive)
					|| !seen.insert(raw.toLower()).second) {
				return;
			}
			usernames.push_back('@' + raw);
		};
		for (const auto &entry : mentioned) {
			if (!entry.username.isEmpty()) {
				add(entry.username);
			}
		}
		for (const auto &entry : pendingIds) {
			const auto user = session->data().userLoaded(
				UserId(entry.userId));
			if (user) {
				add(user->username());
			}
		}
		SquidwardShowUsersResult(
			session,
			show,
			baseTitle,
			std::move(usernames));
	};
	if (toResolve.isEmpty()) {
		finish();
		return;
	}
	SquidwardSetStatus(
		state,
		u"Уточняем %1 участников без данных..."_q.arg(
			int(toResolve.size())));
	SquidwardResolveMentionedUsers(session, std::move(toResolve), finish);
}

// Recurses one page at a time (never in parallel - the same account has to
// wait for each edit before the next click makes sense anyway), extracting
// from every page it sees along the way, until the bot's own page counter
// says this was the last one, no further "➡" button can be found, or the
// safety cap is spent (a bug in that counter's parsing must never turn
// into an unbounded click loop).
template <typename Entry>
void SquidwardCollectNextPage(
		not_null<Window::SessionController*> controller,
		not_null<SquidwardBotState*> state,
		not_null<HistoryItem*> item,
		SquidwardExtractFn<Entry> extract,
		std::shared_ptr<std::vector<Entry>> collected,
		int pagesLeftSafety,
		Fn<void(std::vector<Entry>)> done) {
	extract(item, *collected);
	const auto pageInfo = SquidwardParsePageInfo(item->originalText().text);
	if (pageInfo) {
		SquidwardSetStatus(state, u"Собираем... страница %1 из %2 (найдено %3)"_q
			.arg(pageInfo->first)
			.arg(pageInfo->second)
			.arg(int(collected->size())));
	}
	const auto markup = item->Get<HistoryMessageReplyMarkup>();
	const auto next = (pagesLeftSafety > 0 && markup)
		? SquidwardFindNextPageButton(markup->data)
		: std::nullopt;
	if (!next || (pageInfo && pageInfo->first >= pageInfo->second)) {
		SquidwardSetStatus(
			state,
			u"Готово, собрано: %1."_q.arg(int(collected->size())));
		done(std::move(*collected));
		return;
	}
	const auto itemId = item->fullId();
	const auto lifetime = std::make_shared<rpl::lifetime>();
	const auto settled = std::make_shared<bool>(false);
	controller->session().changes().messageUpdates(
		Data::MessageUpdate::Flag::Edited
	) | rpl::filter([=](const Data::MessageUpdate &update) {
		return update.item->fullId() == itemId;
	}) | rpl::on_next([=](const Data::MessageUpdate &update) {
		if (std::exchange(*settled, true)) {
			return;
		}
		lifetime->destroy();
		SquidwardCollectNextPage(
			controller,
			state,
			update.item,
			extract,
			collected,
			pagesLeftSafety - 1,
			done);
	}, *lifetime);
	Api::ActivateBotCommand(ClickHandlerContext{
		.itemId = itemId,
		.sessionWindow = base::make_weak(controller),
	}, next->first, next->second);
}

// Clicks a button that starts a fresh listing (row/column on `source`, the
// message showing it) and waits for page 1 - which, in practice, arrives
// either as the bot's *new* message, or as an edit of `source` itself (the
// profile card turning into the listing in place) - the bot mixes both
// patterns for its own initial replies (see handleBotItem's "⏳
// Вычисляю..." placeholder comment), so both are covered here; only page-
// to-page navigation *after* this is reliably always an edit, handled by
// SquidwardCollectNextPage.
template <typename Entry>
void SquidwardStartCollection(
		not_null<Window::SessionController*> controller,
		not_null<SquidwardBotState*> state,
		not_null<HistoryItem*> source,
		int row,
		int column,
		SquidwardExtractFn<Entry> extract,
		Fn<void(std::vector<Entry>)> done) {
	const auto sourceId = source->fullId();
	const auto lifetime = std::make_shared<rpl::lifetime>();
	const auto settled = std::make_shared<bool>(false);
	const auto attempts = std::make_shared<int>(0);
	const auto click = [=] {
		Api::ActivateBotCommand(ClickHandlerContext{
			.itemId = sourceId,
			.sessionWindow = base::make_weak(controller),
		}, row, column);
	};
	// The bot first answers with a "⏳ Вычисляю..." placeholder (no
	// keyboard) that only later becomes the real page by an edit, so a
	// placeholder is never taken as page 1 - the wait continues until an
	// edit (or a new message) brings a real page.
	const auto isPlaceholder = [](not_null<HistoryItem*> candidate) {
		return candidate->originalText().text.contains(u"ычисля"_q)
			&& !candidate->Get<HistoryMessageReplyMarkup>();
	};
	const auto begin = [=](not_null<HistoryItem*> first) {
		if (*settled || first->out() || isPlaceholder(first)) {
			return;
		}
		*settled = true;
		lifetime->destroy();
		SquidwardCollectNextPage<Entry>(
			controller,
			state,
			first,
			extract,
			std::make_shared<std::vector<Entry>>(),
			2000,
			done);
	};
	controller->session().data().newItemAdded(
	) | rpl::filter([=](not_null<HistoryItem*> added) {
		return state->bot && added->history()->peer == state->bot;
	}) | rpl::on_next(begin, *lifetime);
	controller->session().changes().messageUpdates(
		Data::MessageUpdate::Flag::Edited
	) | rpl::filter([=](const Data::MessageUpdate &update) {
		return state->bot && update.item->history()->peer == state->bot;
	}) | rpl::on_next([=](const Data::MessageUpdate &update) {
		begin(update.item);
	}, *lifetime);
	// Replacing (so destroying) whatever the previous attempt's own timer
	// was. The bot can take a good while on a fresh (paid) lookup, so wait
	// 40 s per attempt and click again (up to twice more) if nothing at
	// all came back; a repeat click on an already-paid lookup is free.
	state->pendingCollectionTimeout = std::make_shared<base::Timer>();
	const auto timer = state->pendingCollectionTimeout.get();
	timer->setCallback([=] {
		if (*settled) {
			return;
		}
		if (++*attempts > 2) {
			*settled = true;
			lifetime->destroy();
			SquidwardSetStatus(
				state,
				u"Не дождались ответа бота на клик - попробуй вручную."_q);
			return;
		}
		SquidwardSetStatus(
			state,
			u"Бот молчит, нажимаем ещё раз (%1/2)..."_q.arg(*attempts));
		click();
		timer->callOnce(40000);
	});
	timer->callOnce(40000);
	SquidwardSetStatus(state, u"Запрашиваем список..."_q);
	click();
}

// One clickable row per keyboard row - reuses Api::ActivateBotCommand so a
// click behaves exactly as it would on a real keyboard button (sends the
// callback query / plain text / opens the url, depending on button type).
// The one exception is "Группы" (see SquidwardStartCollection's comment).
void SquidwardAppendButtonsRow(
		not_null<Ui::VerticalLayout*> log,
		not_null<Window::SessionController*> controller,
		std::shared_ptr<Ui::Show> show,
		not_null<SquidwardBotState*> state,
		not_null<HistoryItem*> item,
		int row,
		const std::vector<HistoryMessageMarkupButton> &buttons) {
	auto wrapOwned = object_ptr<Ui::RpWidget>(log);
	const auto wrap = wrapOwned.data();
	// Paid actions of the bot aren't offered here at all - a misclick on
	// one would spend the user's tokens. `column` stays the button's real
	// index in the bot's keyboard (that's what gets activated), `slot` is
	// its position among the ones actually shown.
	auto shown = std::vector<int>();
	for (auto i = 0; i != int(buttons.size()); ++i) {
		if (!buttons[i].text.contains(u"💎"_q)
			|| SquidwardButtonLastWord(buttons[i].text) == u"группы"_q) {
			shown.push_back(i);
		}
	}
	if (shown.empty()) {
		return;
	}
	const auto columns = int(shown.size());
	wrap->resize(wrap->width(), st::defaultBoxButton.height);
	for (auto slot = 0; slot != columns; ++slot) {
		const auto column = shown[slot];
		const auto button = Ui::CreateChild<Ui::RoundButton>(
			wrap,
			rpl::single(buttons[column].text),
			st::defaultBoxButton);
		const auto itemId = item->fullId();
		const auto isGroupsButton = (SquidwardButtonLastWord(
			buttons[column].text) == u"группы"_q);
		button->setClickedCallback([=] {
			if (isGroupsButton) {
				const auto baseTitle = state->lastLookupIdentifier;
				SquidwardStartCollection<Data::ScrapedGroupEntry>(
					controller,
					state,
					item,
					row,
					column,
					&SquidwardExtractGroupEntries,
					[=](std::vector<Data::ScrapedGroupEntry> entries) {
						SquidwardFilterAndShowGroups(
							controller,
							show,
							baseTitle,
							std::move(entries));
					});
				return;
			}
			Api::ActivateBotCommand(ClickHandlerContext{
				.itemId = itemId,
				.sessionWindow = base::make_weak(controller),
			}, row, column);
		});
		wrap->widthValue(
		) | rpl::on_next([=](int width) {
			const auto skip = st::boxRowPadding.left();
			const auto buttonWidth = (width - skip * (columns - 1))
				/ columns;
			button->setFullWidth(buttonWidth);
			button->moveToLeft(slot * (buttonWidth + skip), 0, width);
		}, wrap->lifetime());
	}
	log->add(std::move(wrapOwned), st::boxRowPadding);
}

void SquidwardSendText(
		not_null<Window::SessionController*> controller,
		not_null<UserData*> bot,
		const QString &text) {
	auto message = Api::MessageToSend(Api::SendAction(
		controller->session().data().history(bot)));
	message.textWithTags = { text, TextWithTags::Tags() };
	controller->session().api().sendMessage(std::move(message));
}

// Deliberately doesn't nudge the bot with "/start" here - its reply to
// that is the full generic help text listing every lookup kind and its
// whole paid-extras menu, exactly the "лишнее" (extra clutter) this
// curated box exists to avoid. The log starts empty; the first thing in
// it is whatever the bot says about an actual lookup once one of the two
// buttons below is used.
void SquidwardBotConnected(
		not_null<Window::SessionController*> controller,
		not_null<SquidwardBotState*> state,
		not_null<UserData*> bot,
		const QString &username) {
	state->bot = bot;
	state->status->setText(u"Бот @%1 подключен."_q.arg(username));
	if (const auto group = std::exchange(state->pendingGroupLookup, {});
		!group.isEmpty()) {
		// Exactly what "Пробить группу" does after the prompt is filled in.
		state->autoFetchTopPeopleNext = true;
		state->autoFetchGroupsNext = false;
		state->lastLookupIdentifier = group;
		SquidwardSetStatus(state, u"→ Отправляем боту: "_q + group);
		SquidwardSendText(controller, bot, group);
	}
}

void SquidwardResolveBot(
		not_null<Window::SessionController*> controller,
		not_null<SquidwardBotState*> state,
		base::weak_qptr<Ui::GenericBox> weak) {
	const auto username = QString::fromLatin1(kSquidwardBotUsername);
	if (const auto peer = controller->session().data().peerByUsername(
			username)) {
		if (const auto user = peer->asUser()) {
			SquidwardBotConnected(controller, state, user, username);
		}
		return;
	}
	state->status->setText(u"Ищем @%1..."_q.arg(username));
	state->resolveRequestId = controller->session().api().request(
		MTPcontacts_ResolveUsername(
			MTP_flags(MTPcontacts_ResolveUsername::Flags(0)),
			MTP_string(username),
			MTP_string(QString()))
	).done([=](const MTPcontacts_ResolvedPeer &result) {
		if (!weak) {
			return;
		}
		result.match([&](const MTPDcontacts_resolvedPeer &data) {
			controller->session().data().processUsers(data.vusers());
			controller->session().data().processChats(data.vchats());
			const auto peerId = peerFromMTP(data.vpeer());
			const auto user = peerId
				? controller->session().data().peer(peerId)->asUser()
				: nullptr;
			if (user) {
				SquidwardBotConnected(controller, state, user, username);
			} else {
				SquidwardSetStatus(
					state,
					u"@%1 не найден или не является пользователем."_q.arg(
						username));
			}
		});
	}).fail([=](const MTP::Error &error) {
		if (!weak) {
			return;
		}
		SquidwardSetStatus(
			state,
			u"Ошибка поиска @%1: %2"_q.arg(username, error.type()));
	}).send();
}

// The "Сквидвард" section of the GGTG window (see OpenGgtgShell below).

} // namespace

[[nodiscard]] object_ptr<Ui::BoxContent> MakeSquidwardBox(
		not_null<Window::SessionController*> controller,
		const QString &lookupGroup) {
	return Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"Сквидвард"_q));

		const auto state = box->lifetime().make_state<
			SquidwardBotState>();
		state->pendingGroupLookup = lookupGroup;

		Ui::AddSubsectionTitle(
			box->verticalLayout(),
			rpl::single(u"Состояние"_q));
		state->status = box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			u"Подключение..."_q,
			st::boxLabel));
		Ui::AddSkip(box->verticalLayout());
		Ui::AddDivider(box->verticalLayout());
		Ui::AddSkip(box->verticalLayout());
		Ui::AddSubsectionTitle(
			box->verticalLayout(),
			rpl::single(u"Что пробить"_q));

		const auto sendText = [=](const QString &text) {
			if (!state->bot) {
				SquidwardSetStatus(
					state,
					u"Бот ещё не подключен, подождите."_q);
				return;
			}
			if (text.trimmed().isEmpty()) {
				return;
			}
			SquidwardSetStatus(state, u"→ Отправляем боту: "_q + text.trimmed());
			SquidwardSendText(
				controller,
				not_null<UserData*>(state->bot),
				text.trimmed());
		};

		// A focused prompt for just the one value each lookup needs,
		// instead of a free-text field the bot's own /start help has
		// to be read to figure out how to use - sends exactly what
		// typing it by hand would. Only these two of the bot's
		// several lookup kinds are exposed here on purpose (see
		// AGENTS-facing task notes); its own menu of paid extras
		// (tokens, invites, incognito, mirrors - the /menu screen)
		// is never reached from this box.
		const auto promptAndSend = [=](
				const QString &title,
				const QString &placeholder,
				bool autoFetchTopPeople,
				bool autoFetchGroups = false) {
			// box->uiShow(), not controller->show(): this box may be
			// hosted inside a floating panel (see FloatingBoxHost above)
			// rather than the main window, and this prompt needs to open
			// wherever *that* box is, or the user ends up typing into a
			// dialog hidden behind the wrong window - see the "Сквидвард"
			// investigation this comment was added for.
			box->uiShow()->showBox(Box([=](not_null<Ui::GenericBox*> prompt) {
				prompt->setTitle(rpl::single(title));
				const auto field = prompt->addRow(
					object_ptr<Ui::InputField>(
						prompt,
						st::defaultInputField,
						rpl::single(placeholder)));
				const auto submit = [=] {
					const auto text = field->getLastText().trimmed();
					if (text.isEmpty()) {
						return;
					}
					// Consumed by the box's own newItemAdded mirror
					// below, on whatever the bot sends back next.
					state->autoFetchTopPeopleNext = autoFetchTopPeople;
					state->autoFetchGroupsNext = autoFetchGroups;
					state->lastLookupIdentifier = text;
					sendText(text);
					prompt->closeBox();
				};
				field->submits(
				) | rpl::on_next([=](Qt::KeyboardModifiers) {
					submit();
				}, prompt->lifetime());
				prompt->addButton(rpl::single(u"Пробить"_q), submit);
				prompt->addButton(
					rpl::single(u"Отмена"_q),
					[=] { prompt->closeBox(); });
			}), Ui::LayerOption::KeepOther);
		};

		box->addRow(object_ptr<Ui::SettingsButton>(
			box,
			rpl::single(u"🔎 Пробить пользователя"_q))
		)->setClickedCallback([=] {
			promptAndSend(
				u"Пробить пользователя"_q,
				u"@username или ID"_q,
				false,
				true);
		});
		box->addRow(object_ptr<Ui::SettingsButton>(
			box,
			rpl::single(u"🔎 Пробить группу/канал"_q))
		)->setClickedCallback([=] {
			promptAndSend(
				u"Пробить группу/канал"_q,
				u"@username, ID или ссылка"_q,
				true);
		});
		Ui::AddDividerText(
			box->verticalLayout(),
			rpl::single(
				u"Пользователь: покажет его группы, их можно сохранить в "
				"«Базу». Группа или канал: соберёт участников с "
				"юзернеймами в «Базу». Платные действия бота скрыты."_q));
		Ui::AddSkip(box->verticalLayout());
		Ui::AddSubsectionTitle(
			box->verticalLayout(),
			rpl::single(u"Ход выполнения"_q));

		// The log sits below the two lookup buttons (not between them and
		// the status line) so the buttons are always at the top.
		state->log = SquidwardAddLogArea(box->verticalLayout());

		// Some of the bot's replies (this profile card among them, per
		// the "⏳ Вычисляю..." placeholder actually seen from it) start
		// as bare text with no keyboard yet and only turn into the real
		// answer - buttons included - once it edits that same message
		// in place, the same way page turns already do (see
		// SquidwardCollectNextPage). So this can't just process the
		// *first* reply and stop: it has to watch for an edit of that
		// placeholder too, and only actually consume
		// autoFetchTopPeopleNext once it sees a message that has a
		// keyboard - a markup-less reply leaves the flag untouched for
		// whatever comes next instead of wrongly giving up on it.
		const auto handleBotItem = [=](not_null<HistoryItem*> item) {
			if (item->out()) {
				// What was sent is already logged as "→ Отправляем боту".
				return;
			}
			SquidwardAppendLogLine(
				state->log,
				u"Бот: "_q + item->originalText().text);
			const auto markup = item->Get<HistoryMessageReplyMarkup>();
			if (!markup) {
				return;
			}
			const auto &rows = markup->data.rows;
			for (auto row = 0; row != int(rows.size()); ++row) {
				SquidwardAppendButtonsRow(
					state->log,
					controller,
					box->uiShow(),
					state,
					item,
					row,
					rows[row]);
			}
			for (const auto &row : rows) {
				auto labels = QStringList();
				for (const auto &b : row) {
					labels.push_back(b.text);
				}
				Core::GgtgLog::Info(
					u"Сквидвард"_q,
					u"Кнопки: "_q + labels.join(u" | "_q));
			}
			if (state->autoFetchGroupsNext) {
				const auto groups = SquidwardFindButton(
					markup->data,
					[](const QString &text) {
						return SquidwardButtonLastWord(text) == u"группы"_q;
					});
				if (groups) {
					state->autoFetchGroupsNext = false;
					const auto baseTitle = state->lastLookupIdentifier;
					const auto show = box->uiShow();
					SquidwardStartCollection<Data::ScrapedGroupEntry>(
						controller,
						state,
						item,
						groups->first,
						groups->second,
						&SquidwardExtractGroupEntries,
						[=](std::vector<Data::ScrapedGroupEntry> entries) {
							SquidwardFilterAndShowGroups(
								controller,
								show,
								baseTitle,
								std::move(entries));
						});
				}
			}
			// See promptAndSend's group case and
			// SquidwardBotState::autoFetchTopPeopleNext's own comment:
			// this reply is that group's profile card - auto-click its
			// "Топ, люди" (free) for the user instead of making them
			// find it.
			if (std::exchange(state->autoFetchTopPeopleNext, false)) {
				const auto topPeople = SquidwardFindButton(
					markup->data,
					[](const QString &text) {
						return SquidwardButtonLastWord(text) == u"люди"_q
							&& !text.contains(u"💎"_q);
					});
				if (topPeople) {
					const auto baseTitle = state->lastLookupIdentifier;
					SquidwardStartCollection<SquidwardMentionedUser>(
						controller,
						state,
						item,
						topPeople->first,
						topPeople->second,
						[=](
								not_null<HistoryItem*> page,
								std::vector<SquidwardMentionedUser> &into) {
							SquidwardExtractUsernames(page, into);
						},
						[=](std::vector<SquidwardMentionedUser> mentioned) {
							SquidwardResolveAndShowUsers(
								controller,
								state,
								box->uiShow(),
								baseTitle,
								std::move(mentioned));
						});
				} else {
					// Otherwise this would fail completely silently -
					// exactly the "ничего не происходит" this was
					// reported as: the reply came back, it just didn't
					// have a button whose last word is exactly "люди"
					// (see SquidwardButtonLastWord's own comment on why
					// it's an exact match).
					SquidwardSetStatus(
						state,
						u"В ответе бота не нашлась кнопка «Топ, люди» - открой список вручную."_q);
				}
			}
		};

		controller->session().data().newItemAdded(
		) | rpl::filter([=](not_null<HistoryItem*> item) {
			return state->bot && item->history()->peer == state->bot;
		}) | rpl::on_next([=](not_null<HistoryItem*> item) {
			handleBotItem(item);
			if (item->out() || item->Get<HistoryMessageReplyMarkup>()) {
				return;
			}
			const auto itemId = item->fullId();
			const auto lifetime = std::make_shared<rpl::lifetime>();
			controller->session().changes().messageUpdates(
				Data::MessageUpdate::Flag::Edited
			) | rpl::filter([=](const Data::MessageUpdate &update) {
				return update.item->fullId() == itemId;
			}) | rpl::on_next([=](const Data::MessageUpdate &update) {
				lifetime->destroy();
				handleBotItem(update.item);
			}, *lifetime);
		}, box->lifetime());

		SquidwardResolveBot(controller, state, base::make_weak(box));

		box->addButton(rpl::single(u"Закрыть"_q), [=] { box->closeBox(); });
	});
}

} // namespace Window
