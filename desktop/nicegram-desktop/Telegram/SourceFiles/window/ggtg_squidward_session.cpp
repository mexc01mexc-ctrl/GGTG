/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and exceptions, see https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "window/ggtg_squidward_session.h"

#include "api/api_bot.h"
#include "api/api_common.h"
#include "apiwrap.h"
#include "base/call_delayed.h"
#include "base/flat_set.h"
#include "core/application.h"
#include "core/click_handler_types.h"
#include "core/ggtg_log.h"
#include "core/process_manager.h"
#include "data/data_changes.h"
#include "data/data_channel.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_item_components.h"
#include "history/history_item_reply_markup.h"
#include "main/main_account.h"
#include "main/main_session.h"
#include "ui/layers/generic_box.h"
#include "ui/text/text_entity.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "window/ggtg_squidward.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"

namespace Window::Squidward {
namespace {

// The bot and its mirrors, in the order they are tried: when one is not found,
// does not answer, or has no crystals left, the next one takes the lookup.
[[nodiscard]] const std::vector<QString> &BotNames() {
	static const auto result = std::vector<QString>{
		u"skvidvarde_bot"_q,
		u"skvidvarded_bot"_q,
		u"fanstaaat_rbot"_q,
	};
	return result;
}
// A bot that said it has no crystals is skipped for this long.
constexpr auto kNoCrystalsMemory = crl::time(10 * 60 * 1000);
constexpr auto kLookupTimeout = crl::time(30000);
constexpr auto kFirstPageTimeout = crl::time(40000);
constexpr auto kPageTimeout = crl::time(20000);
constexpr auto kUnknownGrace = crl::time(4000);
// contacts.resolveUsername is tightly limited: a short FLOOD_WAIT is sat
// out, a long one ends the check (the rest is kept unchecked), and a
// request with no answer at all is given up on.
constexpr auto kMaxFloodWait = 60;
constexpr auto kResolveTimeout = crl::time(20000);

[[nodiscard]] Message ToMessage(not_null<HistoryItem*> item) {
	const auto text = item->originalText();
	auto result = Message{ .text = text.text };
	for (const auto &entity : text.entities) {
		auto type = Entity::Type::Other;
		switch (entity.type()) {
		case EntityType::Bold: type = Entity::Type::Bold; break;
		case EntityType::Url: type = Entity::Type::Url; break;
		case EntityType::CustomUrl: type = Entity::Type::CustomUrl; break;
		case EntityType::Mention: type = Entity::Type::Mention; break;
		case EntityType::MentionName:
			type = Entity::Type::MentionName;
			break;
		default: break;
		}
		result.entities.push_back({
			type,
			entity.offset(),
			entity.length(),
			entity.data(),
		});
	}
	return result;
}

[[nodiscard]] std::optional<std::pair<int, int>> FindButton(
		not_null<HistoryItem*> item,
		Fn<bool(const QString&)> match) {
	const auto markup = item->Get<HistoryMessageReplyMarkup>();
	if (!markup) {
		return std::nullopt;
	}
	const auto &rows = markup->data.rows;
	for (auto row = 0; row != int(rows.size()); ++row) {
		for (auto column = 0; column != int(rows[row].size()); ++column) {
			if (match(rows[row][column].text)) {
				return std::make_pair(row, column);
			}
		}
	}
	return std::nullopt;
}

[[nodiscard]] bool IsGroupsButton(const QString &text) {
	// "👁 Группы", "15💎 Groups" - but not "1💎 Similar groups".
	const auto words = ButtonWords(text);
	return words == Fold(u"группы"_q) || words == Fold(u"groups"_q);
}

[[nodiscard]] bool IsPeopleButton(const QString &text) {
	// "🎖 Топ, люди", "🎖 Top, members".
	const auto last = ButtonLastWord(text);
	return (last == Fold(u"люди"_q)
			|| last == Fold(u"members"_q)
			|| last == Fold(u"people"_q))
		&& !text.contains(u"💎"_q);
}

[[nodiscard]] bool IsNextPageButton(const QString &text) {
	return text.simplified().startsWith(u"➡"_q) && !text.contains(u"💎"_q);
}

// "Найдено несколько результатов, выберете что интересует." - the bot lists
// the matches ("User: …", "Group: …") as buttons instead of a card.
[[nodiscard]] bool IsChoiceText(const QString &text) {
	const auto lower = text.toLower();
	return lower.contains(u"несколько результатов"_q)
		|| lower.contains(u"several results"_q)
		|| lower.contains(u"multiple results"_q);
}

struct ChoiceButton {
	QString text;
	std::pair<int, int> coords;
};

[[nodiscard]] std::vector<ChoiceButton> CallbackButtons(
		not_null<HistoryItem*> item) {
	auto result = std::vector<ChoiceButton>();
	const auto markup = item->Get<HistoryMessageReplyMarkup>();
	if (!markup) {
		return result;
	}
	const auto &rows = markup->data.rows;
	for (auto row = 0; row != int(rows.size()); ++row) {
		for (auto column = 0; column != int(rows[row].size()); ++column) {
			const auto &button = rows[row][column];
			if (button.type == HistoryMessageMarkupButton::Type::Callback) {
				result.push_back({ button.text, { row, column } });
			}
		}
	}
	return result;
}

[[nodiscard]] bool IsGroupChoice(const QString &text) {
	const auto lower = text.toLower();
	return lower.contains(u"group:"_q) || lower.contains(u"группа:"_q);
}

// The bot's notice that the crystals (its paid currency) are not enough. It
// writes in look-alike letters, so the text is folded first.
[[nodiscard]] bool IsNoCrystals(const QString &text) {
	const auto folded = Fold(text);
	const auto has = [&](const QString &word) {
		return folded.contains(Fold(word));
	};
	const auto crystals = has(u"кристал"_q)
		|| has(u"crystal"_q)
		|| text.contains(u"💎"_q);
	const auto shortage = has(u"недостаточно"_q)
		|| has(u"не хватает"_q)
		|| has(u"нехватает"_q)
		|| has(u"закончил"_q)
		|| has(u"пополн"_q)
		|| has(u"not enough"_q)
		|| has(u"insufficient"_q)
		|| has(u"top up"_q)
		|| has(u"run out"_q)
		|| has(u"out of"_q);
	return crystals && shortage;
}

std::shared_ptr<Session> GlobalActiveSquidwardSession;

} // namespace

std::shared_ptr<Session> ActiveSquidwardSession() {
	return GlobalActiveSquidwardSession;
}

void SetActiveSquidwardSession(std::shared_ptr<Session> session) {
	GlobalActiveSquidwardSession = std::move(session);
}

void ClearActiveSquidwardSession() {
	GlobalActiveSquidwardSession.reset();
}

Session::Session(not_null<Window::SessionController*> controller)
: Session(&controller->session(), controller.get()) {
}

Session::Session(
	not_null<Main::Session*> session,
	Window::SessionController *controller)
: _session(session)
, _account(&session->account())
, _controller(controller ? base::make_weak(controller) : nullptr)
, _api(&session->mtp())
, _lookupTimer([=] { lookupTimeout(); })
, _pageTimer([=] { pageTimeout(); })
, _resolveTimer([=] { keepRestUnchecked(); })
, _unknownTimer([=] { settleUnknown(); }) {
	session->data().newItemAdded(
	) | rpl::on_next([=](not_null<HistoryItem*> item) {
		handle(item, false);
	}, _lifetime);
	session->changes().messageUpdates(
		Data::MessageUpdate::Flag::Edited
	) | rpl::on_next([=](const Data::MessageUpdate &update) {
		handle(update.item, true);
	}, _lifetime);
	resolveBot();
}

Session::~Session() {
	if (_process) {
		_process->fail(u"Сбор прерван"_q);
		_process = nullptr;
	}
}

void Session::setController(Window::SessionController *controller) {
	_controller = controller ? base::make_weak(controller) : nullptr;
}

bool Session::isQuery(const QString &query) const {
	if (query.isEmpty() || _query.isEmpty()) {
		return false;
	}
	auto q1 = query.trimmed().toLower();
	auto q2 = _query.trimmed().toLower();
	while (q1.startsWith('@')) q1 = q1.mid(1);
	while (q2.startsWith('@')) q2 = q2.mid(1);
	return q1 == q2;
}

std::optional<Progress> Session::currentProgress() const {
	if (_collect) {
		return _collect->progress;
	}
	return _lastProgress;
}

rpl::producer<BotStatus> Session::status() const {
	return _status.value();
}

rpl::producer<Card> Session::cards() const {
	return _cards.events();
}

rpl::producer<Progress> Session::progress() const {
	return _progress.events();
}

rpl::producer<GroupsResult> Session::groupsCollected() const {
	return _groups.events();
}

rpl::producer<UsersResult> Session::usersCollected() const {
	return _users.events();
}

rpl::producer<QString> Session::errors() const {
	return _errors.events();
}

bool Session::ready() const {
	return _bot != nullptr;
}

QString Session::baseTitle() const {
	return _card ? _card->baseTitle : QString();
}

bool Session::collecting() const {
	return _collect.has_value() || _finishing;
}

void Session::reconnect() {
	if (!_bot && _status.current() == BotStatus::Failed) {
		_status = BotStatus::Connecting;
		resolveBot();
	}
}

MsgId Session::botFloor() const {
	auto result = _maxSeenId;
	if (_bot) {
		const auto history = _session->data().history(_bot);
		if (const auto last = history->lastServerMessage()) {
			result = std::max(result, last->id);
		}
	}
	return result;
}

QString Session::botName() const {
	return '@' + BotNames()[_botIndex];
}

bool Session::botEmpty(int index) const {
	const auto i = _noCrystals.find(index);
	return (i != _noCrystals.end())
		&& (crl::now() - i->second < kNoCrystalsMemory);
}

void Session::resolveBot() {
	const auto username = BotNames()[_botIndex];
	const auto session = _session;
	if (const auto peer = session->data().peerByUsername(username)) {
		if (const auto user = peer->asUser()) {
			_bot = user;
			botResolved(true);
			return;
		}
	}
	_api.request(MTPcontacts_ResolveUsername(
		MTP_flags(MTPcontacts_ResolveUsername::Flags(0)),
		MTP_string(username),
		MTP_string(QString())
	)).done([=](const MTPcontacts_ResolvedPeer &result) {
		result.match([&](const MTPDcontacts_resolvedPeer &data) {
			session->data().processUsers(data.vusers());
			session->data().processChats(data.vchats());
			const auto peerId = peerFromMTP(data.vpeer());
			_bot = peerId ? session->data().peer(peerId)->asUser() : nullptr;
		});
		botResolved(_bot != nullptr);
	}).fail([=](const MTP::Error &error) {
		Core::GgtgLog::Info(
			u"Сквидвард"_q,
			u"Бот "_q + botName() + u" не найден: "_q + error.type());
		_bot = nullptr;
		botResolved(false);
	}).send();
}

// The bot of `_botIndex` was looked up: on success the status turns Ready
// (and a lookup waiting for a mirror is sent), on failure the next mirror is
// tried, and only when none can be found the status is Failed.
void Session::botResolved(bool ok) {
	if (ok) {
		_resolveFailed.clear();
		_status = BotStatus::Ready;
		if (std::exchange(_resend, false) && !_query.isEmpty()) {
			startLookup(_query, true);
		}
		return;
	}
	_resolveFailed.insert(_botIndex);
	const auto count = int(BotNames().size());
	for (auto step = 1; step != count; ++step) {
		const auto next = (_botIndex + step) % count;
		if (!_resolveFailed.contains(next)) {
			_botIndex = next;
			resolveBot();
			return;
		}
	}
	_resolveFailed.clear();
	_resend = false;
	_status = BotStatus::Failed;
}

// Moves the current lookup to a mirror that has not been tried for it (and,
// when possible, one not known to be out of crystals). False when there is
// none.
bool Session::switchBot(const QString &why) {
	const auto count = int(BotNames().size());
	auto pick = -1;
	for (auto pass = 0; pass != 2 && pick < 0; ++pass) {
		for (auto step = 1; step != count; ++step) {
			const auto next = (_botIndex + step) % count;
			if (!_failedBots.contains(next)
				&& (pass == 1 || !botEmpty(next))) {
				pick = next;
				break;
			}
		}
	}
	if (pick < 0) {
		return false;
	}
	Core::GgtgLog::Info(
		u"Сквидвард"_q,
		botName() + u": "_q + why + u", пробую @"_q + BotNames()[pick]);
	_botIndex = pick;
	_bot = nullptr;
	_resend = true;
	_status = BotStatus::Connecting;
	resolveBot();
	return true;
}

void Session::handleNoCrystals() {
	const auto name = botName();
	Core::GgtgLog::Info(u"Сквидвард"_q, name + u": не хватает кристаллов"_q);
	_noCrystals[_botIndex] = crl::now();
	if (_collect) {
		auto message = QString(u"В боте "_q + name + u" закончились кристаллы. "
			"Показано то, что успели собрать. Пополните баланс в боте."_q);
		_errors.fire(std::move(message));
		finish(true);
		return;
	}
	if (switchBot(u"не хватает кристаллов"_q)) {
		return;
	}
	_awaitingCard = false;
	_lookupTimer.cancel();
	_unknownTimer.cancel();
	_unknown.reset();
	auto tried = QStringList();
	for (const auto index : _failedBots) {
		tried.push_back('@' + BotNames()[index]);
	}
	auto message = (tried.size() > 1)
		? QString(u"Кристаллы закончились на всех ботах ("_q + tried.join(u", "_q)
			+ u"). Пополните баланс в любом из них и повторите."_q)
		: QString(u"В боте "_q + name + u" закончились кристаллы. Пополните "
			"баланс в боте и повторите."_q);
	_errors.fire(std::move(message));
}

void Session::lookup(const QString &query) {
	const auto text = query.trimmed();
	if (text.isEmpty()) {
		return;
	}
	if (!_bot) {
		_errors.fire(u"Бот ещё не подключён."_q);
		return;
	}
	if (_collect) {
		stop();
	}
	startLookup(text, false);
}

// `retry`: the same lookup moved to another mirror (the mirrors already
// tried for it are remembered); otherwise a new lookup, which starts on a bot
// that has crystals when the current one is known to be out.
void Session::startLookup(const QString &text, bool retry) {
	if (!retry) {
		_failedBots.clear();
		if (botEmpty(_botIndex)) {
			_failedBots.insert(_botIndex);
			_query = text;
			if (switchBot(u"без кристаллов"_q)) {
				return;
			}
			_failedBots.clear();
		}
	}
	_failedBots.insert(_botIndex);
	++_generation;
	_finishing = false;
	_check.reset();
	_resolveTimer.cancel();
	_query = text;
	_card.reset();
	_lastProgress.reset();
	_lastGroups.reset();
	_lastUsers.reset();
	_cardItem = FullMsgId();
	_choiceItem = FullMsgId();
	_unknown.reset();
	_unknownItem = FullMsgId();
	_unknownTimer.cancel();
	_awaitingCard = true;
	_cardFloor = botFloor();
	Core::GgtgLog::Info(u"Сквидвард"_q, u"Пробив: "_q + text);
	auto message = Api::MessageToSend(Api::SendAction(
		_session->data().history(_bot)));
	message.textWithTags = { text, TextWithTags::Tags() };
	_session->api().sendMessage(std::move(message));
	_lookupTimer.callOnce(kLookupTimeout);
}

void Session::lookupTimeout() {
	if (!_awaitingCard) {
		return;
	}
	if (switchBot(u"не отвечает"_q)) {
		return; // the lookup is sent again to the next mirror
	}
	_awaitingCard = false;
	_unknownTimer.cancel();
	_unknown.reset();
	_errors.fire(u"Бот не отвечает. Попробуйте ещё раз."_q);
}

void Session::handle(not_null<HistoryItem*> item, bool edited) {
	if (!_bot || item->history()->peer != _bot) {
		return;
	}
	const auto isNew = IsServerMsgId(item->id)
		&& (item->id > _cardFloor);
	if (IsServerMsgId(item->id)) {
		_maxSeenId = std::max(_maxSeenId, item->id);
	}
	if (item->out()) {
		return;
	}
	const auto text = item->originalText().text;
	if (IsPlaceholder(text)) {
		return;
	}
	Core::GgtgLog::Info(u"Сквидвард"_q, u"Бот: "_q + text);
	if ((_collect || (_awaitingCard && isNew)) && IsNoCrystals(text)) {
		handleNoCrystals();
		return;
	}
	if (_collect) {
		handlePage(item, edited);
	} else if (_awaitingCard) {
		if (isNew) {
			handleCard(item, false);
		}
	} else if (isNew
		&& !_finishing
		&& _card
		&& _card->kind == CardKind::Unknown
		&& item->fullId() != _cardItem) {
		// The real card came after a reply we couldn't read was shown.
		const auto message = ToMessage(item);
		if (ParseUserCard(message) || ParseGroupCard(message)) {
			handleCard(item, false);
		}
	} else if (edited && item->fullId() == _cardItem && !_finishing) {
		// The bot sometimes fills the card in with a later edit.
		handleCard(item, true);
	}
}

void Session::handleCard(not_null<HistoryItem*> item, bool refresh) {
	if (!refresh && IsChoiceText(item->originalText().text)
		&& !CallbackButtons(item).empty()) {
		// Not a card yet: the match has to be chosen first. The same
		// message edited again (or seen again) is not asked twice.
		if (item->fullId() != _choiceItem) {
			startChoice(item);
		}
		return;
	}
	const auto message = ToMessage(item);
	auto card = Card{
		.rawText = CleanText(message.text),
		.query = _query,
		.refresh = refresh,
	};
	if (auto user = ParseUserCard(message)) {
		card.kind = card.list = CardKind::User;
		card.baseTitle = baseTitleFor(user->name);
		card.user = std::move(user);
	} else if (auto group = ParseGroupCard(message)) {
		card.kind = card.list = CardKind::Group;
		card.baseTitle = baseTitleFor(group->name);
		card.group = std::move(group);
	} else if (refresh) {
		return; // an edit into something we can't read keeps the old card
	} else {
		// Not a card we know, but its buttons may still lead to a list.
		card.baseTitle = baseTitleFor(QString());
		card.list = FindButton(item, &IsGroupsButton)
			? CardKind::User
			: FindButton(item, &IsPeopleButton)
			? CardKind::Group
			: CardKind::Unknown;
		_lookupTimer.cancel();
		_unknown = std::move(card);
		_unknownItem = item->fullId();
		_unknownTimer.callOnce(kUnknownGrace);
		return;
	}
	if (refresh && _card) {
		// Keep the real name already found for this card.
		card.baseTitle = _card->baseTitle;
	}
	acceptCard(item, std::move(card));
}

void Session::acceptCard(not_null<HistoryItem*> item, Card card) {
	const auto fresh = !card.refresh;
	_awaitingCard = false;
	_lookupTimer.cancel();
	_unknownTimer.cancel();
	_unknown.reset();
	_unknownItem = FullMsgId();
	_card = card;
	_cardItem = item->fullId();
	CacheLookupCard(_query, card);
	_cards.fire(std::move(card));
	if (fresh) {
		resolveRealTitle();
	}
}

void Session::startChoice(not_null<HistoryItem*> item) {
	_choiceItem = item->fullId();
	_lookupTimer.cancel();
	const auto buttons = CallbackButtons(item);
	const auto generation = _generation;
	const auto itemId = item->fullId();

	// A link into a private chat or an invite link can only be a group.
	const auto query = _query.toLower();
	if (query.contains(u"t.me/c/"_q)
		|| query.contains(u"t.me/+"_q)
		|| query.contains(u"joinchat"_q)) {
		for (const auto &button : buttons) {
			if (IsGroupChoice(button.text)) {
				pickChoice(generation, itemId, button.coords);
				return;
			}
		}
	}

	const auto weak = base::make_weak(this);
	const auto picked = std::make_shared<bool>(false);
	const auto controller = _controller.get()
		? _controller.get()
		: (Core::App().activeWindow() ? Core::App().activeWindow()->sessionController() : nullptr);
	if (!controller) {
		if (!buttons.empty()) {
			pickChoice(generation, itemId, buttons.front().coords);
		}
		return;
	}
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"Сквидвард нашёл несколько результатов"_q));
		box->addRow(
			object_ptr<Ui::FlatLabel>(
				box,
				u"Что пробить?"_q,
				st::boxLabel),
			st::boxRowPadding);
		for (const auto &button : buttons) {
			const auto row = box->addRow(
				object_ptr<Ui::SettingsButton>(
					box,
					rpl::single(button.text)));
			const auto coords = button.coords;
			row->setClickedCallback([=] {
				*picked = true;
				box->closeBox();
				if (const auto strong = weak.get()) {
					strong->pickChoice(generation, itemId, coords);
				}
			});
		}
		box->addButton(rpl::single(u"Отмена"_q), [=] { box->closeBox(); });
		box->boxClosing(
		) | rpl::on_next([=] {
			if (!*picked) {
				if (const auto strong = weak.get()) {
					strong->cancelChoice(generation);
				}
			}
		}, box->lifetime());
	}));
}

void Session::pickChoice(
		int generation,
		FullMsgId item,
		std::pair<int, int> button) {
	if (generation != _generation || !_awaitingCard) {
		return;
	}
	// The bot now edits that message into the card, or sends a new one.
	_lookupTimer.callOnce(kLookupTimeout);
	click(item, button);
}

void Session::cancelChoice(int generation) {
	if (generation != _generation || !_awaitingCard) {
		return;
	}
	_awaitingCard = false;
	_choiceItem = FullMsgId();
	_lookupTimer.cancel();
	_errors.fire(u"Выбор отменён."_q);
}

void Session::settleUnknown() {
	if (!_awaitingCard || !_unknown) {
		return;
	}
	const auto item = _session->data().message(_unknownItem);
	if (!item) {
		_awaitingCard = false;
		_unknown.reset();
		_errors.fire(u"Ответ бота больше не найден, пробейте заново."_q);
		return;
	}
	auto card = base::take(_unknown);
	acceptCard(item, std::move(*card));
}

// The bot writes names in look-alike letters ("Qiuvviɳɳ"), so a base is
// named after the peer as Telegram knows it: from the cache by username or
// id, else resolved by username. Until then the bot's name stays.
void Session::resolveRealTitle() {
	if (!_card) {
		return;
	}
	const auto &card = *_card;
	auto username = UsernameFromQuery(card.query);
	if (username.isEmpty() && card.user && !card.user->usernames.isEmpty()) {
		username = card.user->usernames.front();
	}
	const auto data = &_session->data();
	if (!username.isEmpty()) {
		if (const auto peer = data->peerByUsername(username)) {
			applyRealTitle(peer, username);
			return;
		}
	}
	const auto rawId = card.user
		? card.user->id
		: card.group
		? card.group->id
		: QString();
	auto digits = rawId.trimmed();
	if (digits.startsWith(u'-')) {
		digits = digits.mid(1);
		if (digits.startsWith(u"100"_q) && digits.size() > 10) {
			digits = digits.mid(3);
		}
	}
	const auto id = digits.toULongLong();
	if (id) {
		const auto peer = card.user
			? static_cast<PeerData*>(data->userLoaded(UserId(id)))
			: card.group
			? static_cast<PeerData*>(data->channelLoaded(ChannelId(id)))
			: nullptr;
		if (peer) {
			applyRealTitle(peer, username);
			return;
		}
	}
	if (username.isEmpty()) {
		return;
	}
	const auto generation = _generation;
	_api.request(MTPcontacts_ResolveUsername(
		MTP_flags(MTPcontacts_ResolveUsername::Flags(0)),
		MTP_string(username),
		MTP_string(QString())
	)).done([=](const MTPcontacts_ResolvedPeer &result) {
		if (generation != _generation) {
			return;
		}
		auto peer = (PeerData*)nullptr;
		result.match([&](const MTPDcontacts_resolvedPeer &data) {
			const auto owner = &_session->data();
			owner->processUsers(data.vusers());
			owner->processChats(data.vchats());
			const auto peerId = peerFromMTP(data.vpeer());
			peer = peerId ? owner->peer(peerId).get() : nullptr;
		});
		applyRealTitle(peer, username);
	}).send();
}

void Session::applyRealTitle(PeerData *peer, const QString &username) {
	if (!_card || !peer || peer->name().trimmed().isEmpty()) {
		return;
	}
	const auto handle = username.isEmpty() ? peer->username() : username;
	_card->baseTitle = Data::GroupBaseTitle(peer->name(), handle);
}

QString Session::baseTitleFor(const QString &name) const {
	const auto username = UsernameFromQuery(_query);
	if (name.isEmpty()) {
		return username.isEmpty() ? _query : (u"@"_q + username);
	}
	return Data::GroupBaseTitle(name, username);
}

void Session::collect() {
	if (collecting() || !_card || _card->list == CardKind::Unknown) {
		return;
	}
	const auto item = _session->data().message(_cardItem);
	if (!item) {
		_errors.fire(u"Ответ бота больше не найден, пробейте заново."_q);
		return;
	}
	const auto isUser = (_card->list == CardKind::User);
	const auto button = FindButton(
		item,
		isUser ? &IsGroupsButton : &IsPeopleButton);
	if (!button) {
		_errors.fire(isUser
			? u"В ответе бота нет кнопки «Группы»."_q
			: u"В ответе бота нет кнопки «Топ, люди»."_q);
		return;
	}
	_collect.emplace();
	_listFloor = botFloor();
	_collect->kind = _card->list;
	_collect->buttonItem = _cardItem;
	_collect->button = *button;
	_lastProgress = _collect->progress;
	_progress.fire_copy(_collect->progress);

	// ggtg: register background scrape so it survives account switches and layer closes
	const auto reopen = [] {
		if (const auto window = Core::App().activeWindow()) {
			if (const auto controller = window->sessionController()) {
				controller->show(MakeSquidwardBox(controller));
			}
		}
	};
	const auto title = (isUser ? u"Сбор групп: "_q : u"Сбор участников: "_q) + baseTitle();
	_process = Core::App().processes().start(title, reopen, Core::ProcessKind::Scrape);
	if (_process) {
		_process->log(u"Запрос списка у бота…"_q);
	}

	click(_cardItem, *button);
	_pageTimer.callOnce(kFirstPageTimeout);
}

void Session::click(FullMsgId itemId, std::pair<int, int> button) {
	const auto item = _session->data().message(itemId);
	if (!item) {
		return;
	}
	const auto markup = item->Get<HistoryMessageReplyMarkup>();
	if (!markup) {
		return;
	}
	if (button.first < 0 || button.first >= int(markup->data.rows.size())) {
		return;
	}
	const auto &row = markup->data.rows[button.first];
	if (button.second < 0 || button.second >= int(row.size())) {
		return;
	}
	const auto &btn = row[button.second];
	if (btn.type == HistoryMessageMarkupButton::Type::Callback) {
		auto flags = MTPmessages_GetBotCallbackAnswer::Flags(0);
		if (!btn.data.isEmpty()) {
			flags |= MTPmessages_GetBotCallbackAnswer::Flag::f_data;
		}
		const auto history = item->history();
		const auto fullId = item->fullId();
		const auto owner = &_session->data();
		const auto rowIdx = button.first;
		const auto colIdx = button.second;

		_session->api().request(
			MTPmessages_GetBotCallbackAnswer(
				MTP_flags(flags),
				history->peer->input(),
				MTP_int(item->id),
				MTP_bytes(btn.data),
				MTP_inputCheckPasswordEmpty())
		).done([=](const MTPmessages_BotCallbackAnswer &result) {
			const auto item = owner->message(fullId);
			if (!item) {
				return;
			}
			if (const auto btnPtr = HistoryMessageMarkupButton::Get(
					owner,
					fullId,
					rowIdx,
					colIdx)) {
				btnPtr->requestId = 0;
				owner->requestItemRepaint(item);
			}
		}).fail([=](const MTP::Error &error) {
			const auto item = owner->message(fullId);
			if (!item) {
				return;
			}
			if (const auto btnPtr = HistoryMessageMarkupButton::Get(
					owner,
					fullId,
					rowIdx,
					colIdx)) {
				btnPtr->requestId = 0;
				owner->requestItemRepaint(item);
			}
		}).send();

		_session->changes().messageUpdated(
			item,
			Data::MessageUpdate::Flag::BotCallbackSent);
	} else if (const auto controller = _controller.get()) {
		Api::ActivateBotCommand(ClickHandlerContext{
			.itemId = itemId,
			.sessionWindow = base::make_weak(controller),
		}, button.first, button.second);
	}
}

void Session::handlePage(not_null<HistoryItem*> item, bool edited) {
	auto &c = *_collect;
	if (!c.waitingFirst && (!edited || item->fullId() != c.listItem)) {
		return;
	}
	const auto message = ToMessage(item);
	const auto info = ParsePageInfo(message.text);
	if (c.waitingFirst) {
		if (item->fullId() == _cardItem) {
			// The card turning into the list in place - but not a late
			// fill-in edit of the card itself.
			const auto looksLikeList = (info || FindButton(item, &IsNextPageButton))
				&& !ParseUserCard(message)
				&& !ParseGroupCard(message);
			if (!looksLikeList) {
				return;
			}
		} else if (!IsServerMsgId(item->id) || item->id <= _listFloor) {
			return; // an edit of some older message
		}
	}
	if (!c.waitingFirst && info && info->page == c.lastPage) {
		return; // the same page edited again
	}
	c.waitingFirst = false;
	c.listItem = item->fullId();
	c.retries = 0;
	_pageTimer.cancel();
	if (info) {
		c.lastPage = info->page;
	}
	if (c.kind == CardKind::User) {
		const auto rows = ExtractGroupRows(message);
		c.groups.insert(c.groups.end(), rows.begin(), rows.end());
		c.progress.found = int(DedupeGroupRows(c.groups).size());
	} else {
		const auto rows = ExtractUserRows(message);
		c.users.insert(c.users.end(), rows.begin(), rows.end());
		c.progress.found = int(c.users.size());
	}
	c.progress.page = info ? info->page : (c.progress.page + 1);
	c.progress.pages = info ? info->pages : 0;
	if (info && info->total >= 0) {
		c.progress.total = info->total;
		CacheLookupTotal(_query, info->total);
	}
	_lastProgress = c.progress;
	_progress.fire_copy(c.progress);

	if (_process) {
		const auto total = (c.progress.pages > 0) ? c.progress.pages : (c.lastPage + 1);
		_process->progress(c.lastPage, total);
		_process->log(
			u"Страница "_q + QString::number(c.lastPage)
			+ ((c.progress.pages > 0) ? (u" из "_q + QString::number(c.progress.pages)) : QString())
			+ u" · найдено "_q + QString::number(c.progress.found));
	}

	const auto next = FindButton(item, &IsNextPageButton);
	if (!next || (info && info->page >= info->pages) || --c.safety <= 0) {
		finish(false);
		return;
	}
	c.buttonItem = c.listItem;
	c.button = *next;
	click(c.buttonItem, c.button);
	_pageTimer.callOnce(kPageTimeout);
}

void Session::pageTimeout() {
	if (!_collect) {
		return;
	}
	auto &c = *_collect;
	if (c.retries++ < (c.waitingFirst ? 2 : 1)) {
		// Look the button up again: the keyboard may have changed, and a
		// stored position could now point at a paid one.
		const auto item = _session->data().message(c.buttonItem);
		const auto button = !item
			? std::nullopt
			: !c.waitingFirst
			? FindButton(item, &IsNextPageButton)
			: FindButton(
				item,
				(c.kind == CardKind::User) ? &IsGroupsButton : &IsPeopleButton);
		if (!button) {
			_errors.fire(u"Кнопка бота пропала — показано то, что успели собрать."_q);
			finish(true);
			return;
		}
		c.button = *button;
		click(c.buttonItem, c.button);
		_pageTimer.callOnce(c.waitingFirst ? kFirstPageTimeout : kPageTimeout);
		return;
	}
	_errors.fire(c.waitingFirst
		? u"Бот не прислал список. Попробуйте ещё раз."_q
		: u"Страница не пришла — показано то, что успели собрать."_q);
	if (_process) {
		_process->log(c.waitingFirst
			? u"Бот не прислал список — завершаем сбор"_q
			: u"Страница не пришла — сохраняем собранное"_q);
	}
	finish(true);
}

void Session::stop() {
	if (_collect) {
		finish(true);
	}
}

void Session::finish(bool stopped) {
	_pageTimer.cancel();
	auto c = std::move(*_collect);
	_collect.reset();
	_finishing = true;
	auto progress = c.progress;
	progress.checking = true;
	progress.page = progress.pages = 0; // checked-count comes next
	_lastProgress = progress;
	_progress.fire_copy(progress);
	if (stopped && _process) {
		_process->log(u"Сбор остановлен — обработка собранного"_q);
	}
	if (c.kind == CardKind::User) {
		finishGroups(std::move(c.groups), stopped);
	} else {
		finishUsers(std::move(c.users), stopped);
	}
}

void Session::finishGroups(std::vector<GroupRow> rows, bool stopped) {
	auto check = GroupCheck{ .stopped = stopped, .generation = _generation };
	for (const auto &row : DedupeGroupRows(std::move(rows))) {
		if (!row.link.isEmpty()) {
			++check.closed;
		}
		check.source.push_back({
			.username = row.username,
			.title = row.title,
			.link = row.link,
		});
	}
	_check = std::move(check);
	checkNextGroup();
}

// Channels are of no use as recipients: every group with a username is
// looked up (cache first, then contacts.resolveUsername with a pause) and
// broadcast channels are dropped. A group that can't be checked now is kept.
void Session::checkNextGroup() {
	if (!_check || _check->generation != _generation) {
		return;
	}
	auto &check = *_check;
	while (check.next < check.source.size()
		&& check.source[check.next].username.isEmpty()) {
		check.kept.push_back(check.source[check.next++]);
	}
	if (check.next >= check.source.size()) {
		auto result = GroupsResult{
			.groups = std::move(check.kept),
			.channelsRemoved = check.channels,
			.closed = check.closed,
			.unchecked = check.unchecked,
			.uncheckedGroups = std::move(check.uncheckedList),
			.stopped = check.stopped,
		};
		_check.reset();
		_finishing = false;
		_lastGroups = result;
		if (_process) {
			_process->succeed(u"Собрано групп: "_q + QString::number(result.groups.size()));
			_process = nullptr;
		}
		CacheLookupGroups(_query, result);
		_groups.fire(std::move(result));
		return;
	}
	auto entry = check.source[check.next++];
	if (_process) {
		_process->progress(int(check.next), int(check.source.size()));
	}
	const auto session = _session;
	const auto generation = check.generation;
	if (const auto peer = session->data().peerByUsername(entry.username)) {
		settleGroup(std::move(entry), peer, 0, generation);
		return;
	}
	resolveGroup(std::move(entry), generation);
}

void Session::resolveGroup(Data::ScrapedGroupEntry entry, int generation) {
	const auto session = _session;
	_resolveTimer.callOnce(kResolveTimeout);
	_check->requestId = _api.request(MTPcontacts_ResolveUsername(
		MTP_flags(MTPcontacts_ResolveUsername::Flags(0)),
		MTP_string(entry.username),
		MTP_string(QString())
	)).done([=](const MTPcontacts_ResolvedPeer &result) {
		_resolveTimer.cancel();
		auto peer = (PeerData*)nullptr;
		result.match([&](const MTPDcontacts_resolvedPeer &data) {
			session->data().processUsers(data.vusers());
			session->data().processChats(data.vchats());
			const auto peerId = peerFromMTP(data.vpeer());
			peer = peerId ? session->data().peer(peerId).get() : nullptr;
		});
		settleGroup(entry, peer, 400, generation);
	}).fail([=](const MTP::Error &error) {
		_resolveTimer.cancel();
		if (!_check || _check->generation != generation) {
			return;
		}
		_check->requestId = 0;
		if (MTP::IsFloodError(error)) {
			const auto seconds = error.type().section(u'_', -1).toInt();
			Core::GgtgLog::Info(
				u"Сквидвард"_q,
				u"Telegram просит подождать "_q
					+ QString::number(seconds) + u" с"_q);
			if (seconds > 0 && seconds <= kMaxFloodWait) {
				auto progress = Progress{
					.page = int(_check->next) - 1,
					.pages = int(_check->source.size()),
					.found = int(_check->kept.size()),
					.checking = true,
					.floodWait = seconds,
				};
				_progress.fire_copy(progress);
				base::call_delayed(seconds * crl::time(1000) + 500, this, [=] {
					if (_check && _check->generation == generation) {
						resolveGroup(entry, generation);
					}
				});
				return;
			}
			--_check->next; // this one goes back to the unchecked rest
			keepRestUnchecked();
			return;
		}
		settleGroup(entry, nullptr, 800, generation);
	}).handleFloodErrors().send();
}

void Session::continueCheck(const GroupsResult &previous) {
	if (collecting() || previous.uncheckedGroups.empty()) {
		return;
	}
	auto pending = base::flat_set<QString>();
	for (const auto &entry : previous.uncheckedGroups) {
		pending.emplace(entry.username.toLower());
	}
	auto check = GroupCheck{
		.source = previous.uncheckedGroups,
		.channels = previous.channelsRemoved,
		.closed = previous.closed,
		.stopped = previous.stopped,
		.generation = _generation,
	};
	for (const auto &entry : previous.groups) {
		if (entry.username.isEmpty()
			|| !pending.contains(entry.username.toLower())) {
			check.kept.push_back(entry);
		}
	}
	_finishing = true;
	_check = std::move(check);
	_progress.fire_copy(Progress{
		.pages = int(_check->source.size()),
		.found = int(_check->kept.size()),
		.checking = true,
	});
	checkNextGroup();
}

// Telegram won't let more usernames be checked now: the rest is kept as is
// (a channel may slip through) and the result comes out at once.
void Session::keepRestUnchecked() {
	if (!_check) {
		return;
	}
	auto &check = *_check;
	if (check.requestId) {
		// Gave up waiting: the one in flight joins the unchecked rest.
		_api.request(base::take(check.requestId)).cancel();
		--check.next;
	}
	while (check.next < check.source.size()) {
		auto &entry = check.source[check.next++];
		if (!entry.username.isEmpty()) {
			++check.unchecked;
			check.uncheckedList.push_back(entry);
		}
		check.kept.push_back(std::move(entry));
	}
	checkNextGroup();
}

void Session::settleGroup(
		Data::ScrapedGroupEntry entry,
		PeerData *peer,
		crl::time delay,
		int generation,
		bool fullLoaded) {
	if (!_check || _check->generation != generation) {
		return;
	}
	_check->requestId = 0;
	const auto channel = peer ? peer->asChannel() : nullptr;
	if (channel
		&& channel->isMegagroup()
		&& !channel->membersCountKnown()
		&& !fullLoaded) {
		// Resolving a username doesn't tell the group's size (it reads
		// as 1 then): the full info does, one more request.
		const auto session = _session;
		_resolveTimer.callOnce(kResolveTimeout);
		_check->requestId = _api.request(MTPchannels_GetFullChannel(
			channel->inputChannel()
		)).done([=](const MTPmessages_ChatFull &result) {
			_resolveTimer.cancel();
			session->api().processFullPeer(channel, result);
			settleGroup(entry, peer, delay + 400, generation, true);
		}).fail([=](const MTP::Error &) {
			_resolveTimer.cancel();
			settleGroup(entry, peer, delay + 400, generation, true);
		}).send();
		return;
	}
	if (channel && !channel->isMegagroup()) {
		++_check->channels;
	} else if (peer && peer->isUser()) {
		// A person or a bot mentioned on the page, not a group.
	} else {
		entry.checked = true; // "Убрать каналы" in «База» skips it
		if (channel) {
			Data::RememberPeerName(channel);
			if (entry.title.isEmpty()) {
				entry.title = channel->name();
			}
			if (channel->membersCountKnown()) {
				entry.membersCount = std::max(
					entry.membersCount,
					channel->membersCount());
			}
		}
		_check->kept.push_back(std::move(entry));
	}
	// In the checking phase page/pages count groups checked / to check.
	auto progress = Progress{
		.page = int(_check->next),
		.pages = int(_check->source.size()),
		.found = int(_check->kept.size()),
		.checking = true,
	};
	_lastProgress = progress;
	_progress.fire_copy(progress);
	base::call_delayed(delay, this, [=] { checkNextGroup(); });
}

void Session::finishUsers(std::vector<UserRow> rows, bool stopped) {
	const auto session = _session;
	const auto generation = _generation;
	auto names = QStringList();
	auto ids = std::vector<uint64>();
	auto toResolve = QVector<MTPInputUser>();
	auto seen = base::flat_set<uint64>();
	for (const auto &row : rows) {
		if (!row.username.isEmpty()) {
			names.push_back(row.username);
			if (!row.name.isEmpty()) {
				Data::RememberNameIfUnknown(row.username, row.name);
			}
			continue;
		}
		const auto fields = TextUtilities::MentionNameDataToFields(
			row.mentionData);
		if (!fields.userId || !seen.emplace(fields.userId).second) {
			continue;
		}
		ids.push_back(fields.userId);
		if (!session->data().userLoaded(UserId(fields.userId))) {
			toResolve.push_back(MTP_inputUser(
				MTP_long(fields.userId),
				MTP_long(fields.accessHash)));
		}
	}
	resolveUsers(std::move(toResolve), [=] {
		if (generation != _generation) {
			return;
		}
		auto all = names;
		for (const auto id : ids) {
			if (const auto user = session->data().userLoaded(UserId(id))) {
				Data::RememberPeerName(user);
				all.push_back(user->username());
			}
		}
		_finishing = false;
		auto result = UsersResult{ DedupeUsernames(all), stopped };
		_lastUsers = result;
		if (_process) {
			_process->succeed(u"Собрано участников: "_q + QString::number(result.usernames.size()));
			_process = nullptr;
		}
		CacheLookupUsers(_query, result);
		if (_autoSave && !stopped && !result.usernames.isEmpty()) {
			_autoSave = false;
			const auto id = Data::GenerateManualBaseId();
			Data::SaveScrapedGroupUsernames(id, baseTitle(), result.usernames);
		}
		_users.fire(std::move(result));
	});
}

void Session::resolveUsers(QVector<MTPInputUser> pending, Fn<void()> done) {
	if (pending.isEmpty()) {
		done();
		return;
	}
	const auto count = std::min<int>(100, pending.size());
	auto chunk = pending.mid(0, count);
	const auto rest = pending.mid(count);
	_api.request(MTPusers_GetUsers(
		MTP_vector<MTPInputUser>(std::move(chunk))
	)).done([=](const MTPVector<MTPUser> &result) {
		_session->data().processUsers(result);
		resolveUsers(rest, done);
	}).fail([=] {
		resolveUsers(rest, done);
	}).send();
}

} // namespace Window::Squidward
