/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and exceptions, see https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/timer.h"
#include <map>
#include <set>
#include "base/weak_ptr.h"
#include "data/data_msg_id.h"
#include "data/scraped_members_db.h"
#include "mtproto/sender.h"
#include "window/ggtg_squidward_parse.h"

class HistoryItem;
class UserData;

namespace Window {
class SessionController;
} // namespace Window

namespace Window::Squidward {

enum class BotStatus {
	Connecting,
	Ready,
	Failed,
};

enum class CardKind {
	User,
	Group,
	Unknown, // "not found", a limit notice, or a reply we can't read
};

struct Card {
	CardKind kind = CardKind::Unknown;
	std::optional<UserCard> user;
	std::optional<GroupCard> group;
	QString rawText; // the bot's reply, cleaned (see CleanText)
	QString query;
	QString baseTitle; // name for a base saved from this card
	// Which list collect() gathers: User - the person's groups, Group - the
	// group's members, Unknown - none (no matching button in the reply).
	CardKind list = CardKind::Unknown;
	bool refresh = false; // the same card, edited by the bot later
};

struct Progress {
	int page = 0;
	int pages = 0; // 0 until the bot's page counter is seen
	int total = -1;
	int found = 0;
	bool checking = false; // pages done, groups/users being checked
	int floodWait = 0; // seconds Telegram asked to wait, 0 when not waiting
};

struct GroupsResult {
	std::vector<Data::ScrapedGroupEntry> groups;
	int channelsRemoved = 0;
	int closed = 0;
	int unchecked = 0; // kept without the channel check (Telegram's limit)
	std::vector<Data::ScrapedGroupEntry> uncheckedGroups; // those, to resume
	bool stopped = false;
};

struct UsersResult {
	QStringList usernames; // without '@'
	bool stopped = false;
};

// Talks to @skvidvarde_bot from the window's account: a lookup ends in a
// Card, and collect() pages through the card's list (a person's groups, or a
// group's members) until the end, a stop() or a timeout. Paid buttons are
// never pressed.
class Session final : public base::has_weak_ptr {
public:
	explicit Session(not_null<Window::SessionController*> controller);

	[[nodiscard]] rpl::producer<BotStatus> status() const;
	[[nodiscard]] rpl::producer<Card> cards() const;
	[[nodiscard]] rpl::producer<Progress> progress() const;
	[[nodiscard]] rpl::producer<GroupsResult> groupsCollected() const;
	[[nodiscard]] rpl::producer<UsersResult> usersCollected() const;
	[[nodiscard]] rpl::producer<QString> errors() const;

	void lookup(const QString &query);
	// Collects the list of the last card; results come as groupsCollected()
	// for a person's card and usersCollected() for a group's.
	void collect();
	// Ends collecting now; what was collected so far still comes out.
	void stop();
	// Checks the groups a previous result left unchecked; the merged result
	// comes as groupsCollected() again.
	void continueCheck(const GroupsResult &previous);
	// Tries to find the bot again after BotStatus::Failed.
	void reconnect();

	[[nodiscard]] bool ready() const;
	// Name for a base saved from the last card: the real Telegram name once
	// it is known, the bot's (stylized) one until then.
	[[nodiscard]] QString baseTitle() const;
	[[nodiscard]] bool collecting() const;
	// "@skvidvarde_bot" or the mirror in use now.
	[[nodiscard]] QString botName() const;

private:
	struct Collect {
		CardKind kind = CardKind::Unknown;
		FullMsgId listItem;
		bool waitingFirst = true;
		int lastPage = 0;
		int retries = 0;
		int safety = 2000;
		FullMsgId buttonItem;
		std::pair<int, int> button;
		std::vector<GroupRow> groups;
		std::vector<UserRow> users;
		Progress progress;
	};
	struct GroupCheck {
		std::vector<Data::ScrapedGroupEntry> source;
		std::vector<Data::ScrapedGroupEntry> kept;
		size_t next = 0;
		int channels = 0;
		int closed = 0;
		int unchecked = 0;
		std::vector<Data::ScrapedGroupEntry> uncheckedList;
		bool stopped = false;
		int generation = 0;
		mtpRequestId requestId = 0;
	};

	void resolveBot();
	void botResolved(bool ok);
	void startLookup(const QString &text, bool retry);
	[[nodiscard]] bool switchBot(const QString &why);
	[[nodiscard]] bool botEmpty(int index) const;
	void handleNoCrystals();
	void handle(not_null<HistoryItem*> item, bool edited);
	void handleCard(not_null<HistoryItem*> item, bool refresh);
	void handlePage(not_null<HistoryItem*> item, bool edited);
	void click(FullMsgId itemId, std::pair<int, int> button);
	void lookupTimeout();
	void pageTimeout();
	void finish(bool stopped);
	void finishGroups(std::vector<GroupRow> rows, bool stopped);
	void checkNextGroup();
	void resolveGroup(Data::ScrapedGroupEntry entry, int generation);
	void keepRestUnchecked();
	void settleGroup(
		Data::ScrapedGroupEntry entry,
		PeerData *peer,
		crl::time delay,
		int generation,
		bool fullLoaded = false);
	[[nodiscard]] MsgId botFloor() const;
	void finishUsers(std::vector<UserRow> rows, bool stopped);
	void resolveUsers(QVector<MTPInputUser> pending, Fn<void()> done);
	[[nodiscard]] QString baseTitleFor(const QString &name) const;
	void acceptCard(not_null<HistoryItem*> item, Card card);
	void settleUnknown();
	// The bot found several matches and offers buttons to choose from.
	void startChoice(not_null<HistoryItem*> item);
	void pickChoice(int generation, FullMsgId item, std::pair<int, int> button);
	void cancelChoice(int generation);
	void resolveRealTitle();
	void applyRealTitle(PeerData *peer, const QString &username);

	const not_null<Window::SessionController*> _controller;
	MTP::Sender _api;
	UserData *_bot = nullptr;
	rpl::variable<BotStatus> _status = BotStatus::Connecting;
	rpl::event_stream<Card> _cards;
	rpl::event_stream<Progress> _progress;
	rpl::event_stream<GroupsResult> _groups;
	rpl::event_stream<UsersResult> _users;
	rpl::event_stream<QString> _errors;

	// The bot and its mirrors (see BotNames()): the one in use, the ones tried
	// for the current lookup, the ones that could not be found just now, and
	// when each said it was out of crystals.
	int _botIndex = 0;
	std::set<int> _failedBots;
	std::set<int> _resolveFailed;
	std::map<int, crl::time> _noCrystals;
	bool _resend = false; // send the lookup again once the mirror is found

	QString _query;
	bool _awaitingCard = false;
	std::optional<Card> _card;
	FullMsgId _cardItem;
	FullMsgId _choiceItem; // the "several results" message already answered or asked
	std::optional<Collect> _collect;
	std::optional<GroupCheck> _check;
	bool _finishing = false;
	// Bumped by every lookup: results of an older one are dropped.
	int _generation = 0;
	// Newest server message id seen from the bot; a card (or a first list
	// page sent as a new message) must be newer than the floor taken when
	// the lookup (or collect) started, so a late edit of an old message
	// can't be taken for it.
	MsgId _maxSeenId = 0;
	MsgId _cardFloor = 0;
	MsgId _listFloor = 0;
	base::Timer _lookupTimer;
	base::Timer _pageTimer;
	base::Timer _resolveTimer;
	// A reply we can't read waits a moment: the bot may send a notice
	// first and the real card right after it.
	std::optional<Card> _unknown;
	FullMsgId _unknownItem;
	base::Timer _unknownTimer;
	rpl::lifetime _lifetime;

};

} // namespace Window::Squidward
