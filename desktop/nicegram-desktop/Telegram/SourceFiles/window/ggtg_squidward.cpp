/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and exceptions, see https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "window/ggtg_squidward.h"

#include "base/flat_map.h"
#include "base/unixtime.h"
#include "core/application.h"
#include "core/ggtg_paths.h"
#include "data/data_user.h"
#include "data/scraped_members_db.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "settings.h"
#include "settings/bulk_accounts_actions.h"
#include "settings/settings_common.h"
#include "ui/boxes/confirm_box.h"
#include "ui/layers/generic_box.h"
#include "ui/text/text_utilities.h"
#include "ui/vertical_list.h"
#include "ui/ui_utility.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "window/ggtg_squidward_parse.h"
#include "window/ggtg_squidward_session.h"
#include "window/ggtg_widgets.h"
#include "window/window_session_controller.h"
#include "settings/sms_campaign.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"
#include "styles/style_widgets.h"

#include <range/v3/algorithm/find.hpp>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QPointer>
#include <QtCore/QRegularExpression>
#include <QtCore/QSaveFile>
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>

namespace Window {
namespace {

using namespace Squidward;

constexpr auto kRecentLimit = 8;
constexpr auto kPreviewCount = 8;

[[nodiscard]] QString RecentPath() {
	return cWorkingDir() + u"tdata/ggtg_squidward_recent.json"_q;
}

[[nodiscard]] QStringList LoadRecent() {
	auto file = QFile(RecentPath());
	if (!file.open(QIODevice::ReadOnly)) {
		return {};
	}
	auto result = QStringList();
	for (const auto &value : QJsonDocument::fromJson(file.readAll()).array()) {
		const auto text = value.toString().trimmed();
		if (!text.isEmpty()) {
			result.push_back(text);
		}
	}
	return result.mid(0, kRecentLimit);
}

void SaveRecent(const QStringList &list) {
	auto file = QSaveFile(RecentPath());
	if (file.open(QIODevice::WriteOnly)) {
		file.write(QJsonDocument(QJsonArray::fromStringList(list)).toJson(
			QJsonDocument::Compact));
		file.commit();
	}
}

// ---- Saved lookups ----
//
// Every card and every collected list is kept in
// tdata/ggtg_db/squidward_cache.json, so a second lookup of the same person
// or group shows the same result at once, without asking the bot (and
// without spending its limits). "Пробить заново" bypasses it.

constexpr auto kCacheLimit = 300;

[[nodiscard]] QString CachePath() {
	return Core::GgtgPaths::DbDir() + u"squidward_cache.json"_q;
}

// "@Name", "name", "t.me/name" and "https://t.me/name/" are one key.
[[nodiscard]] QString CacheKey(QString query) {
	query = query.trimmed().toLower();
	for (const auto &prefix : { u"https://"_q, u"http://"_q }) {
		if (query.startsWith(prefix)) {
			query = query.mid(prefix.size());
		}
	}
	for (const auto &prefix : { u"t.me/"_q, u"telegram.me/"_q }) {
		if (query.startsWith(prefix)) {
			query = query.mid(prefix.size());
		}
	}
	while (query.startsWith('@')) {
		query = query.mid(1);
	}
	while (query.endsWith('/')) {
		query.chop(1);
	}
	return query;
}

[[nodiscard]] QJsonObject UserCardToJson(const UserCard &user) {
	auto names = QJsonArray();
	for (const auto &change : user.names) {
		names.push_back(QJsonObject{
			{ u"d"_q, change.date },
			{ u"n"_q, change.name },
		});
	}
	return QJsonObject{
		{ u"name"_q, user.name },
		{ u"id"_q, user.id },
		{ u"since"_q, user.activeSince },
		{ u"until"_q, user.activeUntil },
		{ u"messages"_q, user.messages },
		{ u"chats"_q, user.chats },
		{ u"diversity"_q, user.diversity },
		{ u"replies"_q, user.replies },
		{ u"media"_q, user.media },
		{ u"round"_q, user.roundVideos },
		{ u"voices"_q, user.voices },
		{ u"favorite"_q, user.favoriteChat },
		{ u"channel"_q, user.channel },
		{ u"searched"_q, user.searched },
		{ u"usernames"_q, QJsonArray::fromStringList(user.usernames) },
		{ u"names"_q, names },
	};
}

[[nodiscard]] UserCard UserCardFromJson(const QJsonObject &object) {
	auto result = UserCard();
	result.name = object.value(u"name"_q).toString();
	result.id = object.value(u"id"_q).toString();
	result.activeSince = object.value(u"since"_q).toString();
	result.activeUntil = object.value(u"until"_q).toString();
	result.messages = object.value(u"messages"_q).toInt(-1);
	result.chats = object.value(u"chats"_q).toInt(-1);
	result.diversity = object.value(u"diversity"_q).toString();
	result.replies = object.value(u"replies"_q).toString();
	result.media = object.value(u"media"_q).toString();
	result.roundVideos = object.value(u"round"_q).toInt(-1);
	result.voices = object.value(u"voices"_q).toInt(-1);
	result.favoriteChat = object.value(u"favorite"_q).toString();
	result.channel = object.value(u"channel"_q).toString();
	result.searched = object.value(u"searched"_q).toInt(-1);
	for (const auto &name : object.value(u"usernames"_q).toArray()) {
		result.usernames.push_back(name.toString());
	}
	for (const auto &value : object.value(u"names"_q).toArray()) {
		const auto change = value.toObject();
		result.names.push_back({
			.date = change.value(u"d"_q).toString(),
			.name = change.value(u"n"_q).toString(),
		});
	}
	return result;
}

[[nodiscard]] QJsonObject GroupCardToJson(const GroupCard &group) {
	auto top = QJsonArray();
	for (const auto &member : group.topDay) {
		top.push_back(QJsonObject{
			{ u"n"_q, member.name },
			{ u"m"_q, member.messages },
		});
	}
	return QJsonObject{
		{ u"name"_q, group.name },
		{ u"id"_q, group.id },
		{ u"place"_q, group.topPlace },
		{ u"of"_q, group.topOf },
		{ u"people"_q, group.peopleDay },
		{ u"messages"_q, group.messagesDay },
		{ u"media"_q, group.mediaDay },
		{ u"mediaPercent"_q, group.mediaDayPercent },
		{ u"round"_q, group.roundVideosDay },
		{ u"voices"_q, group.voicesDay },
		{ u"diversity"_q, group.diversity },
		{ u"description"_q, group.description },
		{ u"top"_q, top },
	};
}

[[nodiscard]] GroupCard GroupCardFromJson(const QJsonObject &object) {
	auto result = GroupCard();
	result.name = object.value(u"name"_q).toString();
	result.id = object.value(u"id"_q).toString();
	result.topPlace = object.value(u"place"_q).toInt(-1);
	result.topOf = object.value(u"of"_q).toInt(-1);
	result.peopleDay = object.value(u"people"_q).toInt(-1);
	result.messagesDay = object.value(u"messages"_q).toInt(-1);
	result.mediaDay = object.value(u"media"_q).toInt(-1);
	result.mediaDayPercent = object.value(u"mediaPercent"_q).toString();
	result.roundVideosDay = object.value(u"round"_q).toInt(-1);
	result.voicesDay = object.value(u"voices"_q).toInt(-1);
	result.diversity = object.value(u"diversity"_q).toString();
	result.description = object.value(u"description"_q).toString();
	for (const auto &value : object.value(u"top"_q).toArray()) {
		const auto member = value.toObject();
		result.topDay.push_back({
			.name = member.value(u"n"_q).toString(),
			.messages = member.value(u"m"_q).toInt(),
		});
	}
	return result;
}

[[nodiscard]] QJsonObject CardToJson(const Card &card) {
	auto result = QJsonObject{
		{ u"kind"_q, int(card.kind) },
		{ u"list"_q, int(card.list) },
		{ u"raw"_q, card.rawText },
		{ u"query"_q, card.query },
		{ u"title"_q, card.baseTitle },
	};
	if (card.user) {
		result.insert(u"user"_q, UserCardToJson(*card.user));
	}
	if (card.group) {
		result.insert(u"group"_q, GroupCardToJson(*card.group));
	}
	return result;
}

[[nodiscard]] Card CardFromJson(const QJsonObject &object) {
	auto result = Card();
	result.kind = CardKind(object.value(u"kind"_q).toInt());
	result.list = CardKind(object.value(u"list"_q).toInt());
	result.rawText = object.value(u"raw"_q).toString();
	result.query = object.value(u"query"_q).toString();
	result.baseTitle = object.value(u"title"_q).toString();
	if (object.contains(u"user"_q)) {
		result.user = UserCardFromJson(object.value(u"user"_q).toObject());
	}
	if (object.contains(u"group"_q)) {
		result.group = GroupCardFromJson(object.value(u"group"_q).toObject());
	}
	return result;
}

[[nodiscard]] QJsonObject GroupEntryToJson(
		const Data::ScrapedGroupEntry &entry) {
	return QJsonObject{
		{ u"u"_q, entry.username },
		{ u"t"_q, entry.title },
		{ u"m"_q, entry.membersCount },
		{ u"l"_q, entry.link },
		{ u"c"_q, entry.checked },
	};
}

[[nodiscard]] Data::ScrapedGroupEntry GroupEntryFromJson(
		const QJsonObject &object) {
	return {
		.username = object.value(u"u"_q).toString(),
		.title = object.value(u"t"_q).toString(),
		.membersCount = object.value(u"m"_q).toInt(),
		.link = object.value(u"l"_q).toString(),
		.checked = object.value(u"c"_q).toBool(),
	};
}

[[nodiscard]] QJsonArray GroupEntriesToJson(
		const std::vector<Data::ScrapedGroupEntry> &list) {
	auto result = QJsonArray();
	for (const auto &entry : list) {
		result.push_back(GroupEntryToJson(entry));
	}
	return result;
}

[[nodiscard]] std::vector<Data::ScrapedGroupEntry> GroupEntriesFromJson(
		const QJsonArray &array) {
	auto result = std::vector<Data::ScrapedGroupEntry>();
	result.reserve(array.size());
	for (const auto &value : array) {
		result.push_back(GroupEntryFromJson(value.toObject()));
	}
	return result;
}

struct SavedLookup {
	std::optional<Card> card;
	std::optional<GroupsResult> groups;
	std::optional<UsersResult> users;
	int total = -1; // "Всего N" from the list's first page
	TimeId savedAt = 0;
};

class LookupCache final {
public:
	LookupCache() {
		load();
	}

	[[nodiscard]] const SavedLookup *find(const QString &query) const {
		auto key = CacheKey(query);
		if (const auto i = _aliases.find(key); i != _aliases.end()) {
			key = i.value();
		}
		const auto i = _entries.find(key);
		return (i != _entries.end() && i->card) ? &*i : nullptr;
	}

	// A fresh card starts the entry over (its old lists are another day's);
	// an edit of the same card only updates it.
	void putCard(const QString &query, const Card &card) {
		if (card.kind == CardKind::Unknown) {
			return;
		}
		const auto key = CacheKey(query);
		if (key.isEmpty()) {
			return;
		}
		auto &entry = _entries[key];
		if (!card.refresh) {
			entry = SavedLookup();
		}
		entry.card = card;
		entry.card->refresh = false;
		entry.savedAt = base::unixtime::now();
		auto aliases = QStringList{ card.query };
		if (card.user) {
			aliases.push_back(card.user->id);
			aliases += card.user->usernames;
		}
		if (card.group) {
			aliases.push_back(card.group->id);
		}
		for (const auto &alias : aliases) {
			const auto normalized = CacheKey(alias);
			if (!normalized.isEmpty() && normalized != key) {
				_aliases[normalized] = key;
			}
		}
		trim();
		save();
	}

	void putGroups(const QString &query, const GroupsResult &result) {
		if (const auto entry = existing(query)) {
			entry->groups = result;
			entry->users.reset();
			entry->savedAt = base::unixtime::now();
			save();
		}
	}

	void putUsers(const QString &query, const UsersResult &result) {
		if (const auto entry = existing(query)) {
			entry->users = result;
			entry->groups.reset();
			entry->savedAt = base::unixtime::now();
			save();
		}
	}

	// Written with the next put: the total comes before the result.
	void setTotal(const QString &query, int total) {
		if (const auto entry = existing(query)) {
			entry->total = total;
		}
	}

private:
	[[nodiscard]] SavedLookup *existing(const QString &query) {
		const auto i = _entries.find(CacheKey(query));
		return (i != _entries.end() && i->card) ? &*i : nullptr;
	}

	void trim() {
		while (_entries.size() > kCacheLimit) {
			auto oldest = _entries.begin();
			for (auto i = _entries.begin(); i != _entries.end(); ++i) {
				if (i->savedAt < oldest->savedAt) {
					oldest = i;
				}
			}
			const auto key = oldest.key();
			_entries.erase(oldest);
			for (auto i = _aliases.begin(); i != _aliases.end();) {
				i = (i.value() == key) ? _aliases.erase(i) : std::next(i);
			}
		}
	}

	void load() {
		auto file = QFile(CachePath());
		if (!file.open(QIODevice::ReadOnly)) {
			return;
		}
		const auto root = QJsonDocument::fromJson(file.readAll()).object();
		const auto entries = root.value(u"entries"_q).toObject();
		for (auto i = entries.begin(); i != entries.end(); ++i) {
			const auto object = i.value().toObject();
			auto entry = SavedLookup();
			entry.card = CardFromJson(object.value(u"card"_q).toObject());
			entry.total = object.value(u"total"_q).toInt(-1);
			entry.savedAt = TimeId(object.value(u"at"_q).toDouble());
			if (object.contains(u"groups"_q)) {
				const auto groups = object.value(u"groups"_q).toObject();
				auto result = GroupsResult();
				result.groups = GroupEntriesFromJson(
					groups.value(u"list"_q).toArray());
				result.channelsRemoved = groups.value(u"channels"_q).toInt();
				result.closed = groups.value(u"closed"_q).toInt();
				result.unchecked = groups.value(u"unchecked"_q).toInt();
				result.uncheckedGroups = GroupEntriesFromJson(
					groups.value(u"uncheckedList"_q).toArray());
				entry.groups = std::move(result);
			}
			if (object.contains(u"users"_q)) {
				auto result = UsersResult();
				for (const auto &name : object.value(u"users"_q).toArray()) {
					result.usernames.push_back(name.toString());
				}
				entry.users = std::move(result);
			}
			_entries.insert(i.key(), std::move(entry));
		}
		const auto aliases = root.value(u"aliases"_q).toObject();
		for (auto i = aliases.begin(); i != aliases.end(); ++i) {
			_aliases.insert(i.key(), i.value().toString());
		}
	}

	void save() const {
		auto entries = QJsonObject();
		for (auto i = _entries.begin(); i != _entries.end(); ++i) {
			auto object = QJsonObject{
				{ u"card"_q, CardToJson(*i->card) },
				{ u"total"_q, i->total },
				{ u"at"_q, qint64(i->savedAt) },
			};
			if (i->groups) {
				object.insert(u"groups"_q, QJsonObject{
					{ u"list"_q, GroupEntriesToJson(i->groups->groups) },
					{ u"channels"_q, i->groups->channelsRemoved },
					{ u"closed"_q, i->groups->closed },
					{ u"unchecked"_q, i->groups->unchecked },
					{
						u"uncheckedList"_q,
						GroupEntriesToJson(i->groups->uncheckedGroups),
					},
				});
			}
			if (i->users) {
				object.insert(
					u"users"_q,
					QJsonArray::fromStringList(i->users->usernames));
			}
			entries.insert(i.key(), object);
		}
		auto aliases = QJsonObject();
		for (auto i = _aliases.begin(); i != _aliases.end(); ++i) {
			aliases.insert(i.key(), i.value());
		}
		const auto path = CachePath();
		QDir().mkpath(QFileInfo(path).absolutePath());
		auto file = QSaveFile(path);
		if (file.open(QIODevice::WriteOnly)) {
			file.write(QJsonDocument(QJsonObject{
				{ u"entries"_q, entries },
				{ u"aliases"_q, aliases },
			}).toJson(QJsonDocument::Compact));
			file.commit();
		}
	}

	QMap<QString, SavedLookup> _entries;
	QMap<QString, QString> _aliases;

};

[[nodiscard]] LookupCache &Lookups() {
	static auto result = LookupCache();
	return result;
}

} // namespace

namespace Squidward {

void CacheLookupCard(const QString &query, const Card &card) {
	Lookups().putCard(query, card);
}

void CacheLookupGroups(const QString &query, const GroupsResult &result) {
	Lookups().putGroups(query, result);
}

void CacheLookupUsers(const QString &query, const UsersResult &result) {
	Lookups().putUsers(query, result);
}

void CacheLookupTotal(const QString &query, int total) {
	Lookups().setTotal(query, total);
}

} // namespace Squidward

namespace {

not_null<Ui::FlatLabel*> AddLabel(
		not_null<Ui::VerticalLayout*> to,
		const QString &text) {
	const auto label = to->add(
		object_ptr<Ui::FlatLabel>(to, text, st::boxLabel),
		st::boxRowPadding);
	label->setSelectable(true);
	return label;
}

// One or two buttons sharing a row.
void AddButtons(
		not_null<Ui::VerticalLayout*> to,
		std::vector<std::tuple<QString, const style::RoundButton*, Fn<void()>>> list) {
	auto wrapOwned = object_ptr<Ui::RpWidget>(to);
	const auto wrap = wrapOwned.data();
	auto buttons = std::vector<not_null<Ui::RoundButton*>>();
	for (auto &[text, st, callback] : list) {
		const auto button = Ui::CreateChild<Ui::RoundButton>(
			wrap,
			rpl::single(text),
			*st);
		button->setTextTransform(Ui::RoundButtonTextTransform::NoTransform);
		button->setClickedCallback(std::move(callback));
		buttons.push_back(button);
	}
	wrap->resize(wrap->width(), buttons.front()->height());
	wrap->widthValue(
	) | rpl::on_next([=](int width) {
		const auto skip = st::boxRowPadding.left() / 2;
		const auto count = int(buttons.size());
		const auto each = (width - skip * (count - 1)) / count;
		for (auto i = 0; i != count; ++i) {
			buttons[i]->setFullWidth(each);
			buttons[i]->moveToLeft(i * (each + skip), 0, width);
		}
	}, wrap->lifetime());
	to->add(std::move(wrapOwned), st::boxRowPadding);
}

// "▸ Title" that opens `body` below it.
void AddExpander(
		not_null<Ui::VerticalLayout*> to,
		const QString &title,
		const QString &body) {
	const auto shown = to->lifetime().make_state<rpl::variable<bool>>(false);
	const auto button = to->add(object_ptr<Ui::SettingsButton>(
		to,
		shown->value() | rpl::map([=](bool open) {
			return (open ? u"▾ "_q : u"▸ "_q) + title;
		})));
	const auto wrap = to->add(
		object_ptr<Ui::SlideWrap<Ui::FlatLabel>>(
			to,
			object_ptr<Ui::FlatLabel>(to, body, st::boxLabel),
			st::boxRowPadding));
	wrap->entity()->setSelectable(true);
	wrap->toggleOn(shown->value());
	wrap->finishAnimating();
	button->setClickedCallback([=] {
		*shown = !shown->current();
	});
}

[[nodiscard]] QString GroupName(const Data::ScrapedGroupEntry &entry) {
	if (!entry.link.isEmpty()) {
		return u"Закрытая: "_q + (entry.title.isEmpty() ? entry.link : entry.title);
	}
	return entry.title.isEmpty() ? (u"@"_q + entry.username) : entry.title;
}

[[nodiscard]] QString GroupLine(const Data::ScrapedGroupEntry &entry) {
	return !entry.username.isEmpty()
		? (u"@"_q + entry.username)
		: !entry.link.isEmpty()
		? entry.link
		: entry.title;
}

[[nodiscard]] QString GroupsCopyText(const GroupsResult &result) {
	auto lines = QStringList();
	for (const auto &entry : result.groups) {
		lines.push_back(GroupLine(entry));
	}
	return lines.join(u'\n');
}

// "Имя (@username)" or "Имя (+7...)" of the account this window uses.
[[nodiscard]] QString ViaAccount(
		not_null<Main::Account*> account) {
	if (!account->sessionExists()) {
		return u"не подключён"_q;
	}
	const auto user = account->session().user();
	const auto name = user->name().trimmed();
	const auto username = user->username();
	const auto handle = !username.isEmpty()
		? (u"@"_q + username)
		: !user->phone().isEmpty()
		? (u"+"_q + user->phone())
		: (u"ID "_q + QString::number(user->id.value));
	return name.isEmpty() ? handle : (name + u" ("_q + handle + u")"_q);
}

class Screen final {
public:
	Screen(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller,
		const QString &autoQuery);

private:
	void setupHeader();
	void setupSearch();
	void setupEvents();
	void submit();
	void start(const QString &query, bool force = false);
	[[nodiscard]] bool showCached(const QString &query);
	void collectNow();
	void refreshLookup();
	void renderRecent();
	void showRecentList();
	void renderIdle();
	void renderSearching(const QString &text);
	void renderLookupError(const QString &text);
	void renderCard(const Card &card);
	void renderUser(const Card &card);
	void renderGroup(const Card &card);
	void addHeader(
		const QString &title,
		const QString &subtitle,
		const QString &id);
	void renderCollectIdle();
	void renderProgress(const Progress &progress);
	void renderGroups(const GroupsResult &result);
	void renderUsers(const UsersResult &result);
	void renderSaved(
		uint64 id,
		const QString &title,
		int saved,
		int collected,
		const QString &copyText);
	[[nodiscard]] bool listOfGroups() const;
	[[nodiscard]] QString countSuffix() const;
	void saveGroups();
	void saveUsers();
	void addContinueCheck();

	const not_null<Ui::GenericBox*> _box;
	const not_null<Window::SessionController*> _controller;
	std::shared_ptr<Session> _session;
	// 🦑 from «База»: looked up, collected and saved without any clicks.
	const QString _autoQuery;
	QString _via; // the account the bot is talked to from
	rpl::lifetime _sessionLifetime;
	bool _autoCollect = false; // the next group card starts collecting
	bool _autoSave = false; // the running collection was auto-started
	// The card on screen came from the saved lookups, not from the bot.
	bool _fromCache = false;
	TimeId _cachedAt = 0;
	bool _collectNext = false; // the next fresh card starts collecting
	// The progress block, updated in place while it stays in one mode.
	QPointer<Ggtg::ProgressBar> _progressBar;
	QPointer<Ui::FlatLabel> _progressLabel;
	bool _progressChecking = false;
	Ui::FlatLabel *_status = nullptr;
	Ui::InputField *_field = nullptr;
	Ui::SlideWrap<Ui::FlatLabel> *_recent = nullptr;
	Ui::VerticalLayout *_card = nullptr;
	Ui::VerticalLayout *_collect = nullptr;
	QStringList _recentList;
	QString _lastQuery;
	QString _pending; // waiting for the bot to connect
	std::optional<Card> _current;
	std::optional<GroupsResult> _groups;
	std::optional<UsersResult> _users;
	// The groups base saved from _groups: a resumed check updates it.
	uint64 _savedGroupsId = 0;
	QString _savedGroupsTitle;
	// "Всего N" from a list's first page, per query: shown on the collect
	// button once the bot has told it.
	base::flat_map<QString, int> _totals;

};

// A group handed over from a base comes as a bare "name" or as its public
// link "t.me/name": the bot wants it as "@name". A closed group's invite
// link (t.me/+..., joinchat) goes as it is.
[[nodiscard]] QString AsLookupQuery(QString query) {
	query = query.trimmed();
	static const auto linkRe = QRegularExpression(
		u"^(?:https?://)?(?:www\\.)?(?:t|telegram)\\.me/([A-Za-z][A-Za-z0-9_]{3,31})/?$"_q);
	if (const auto match = linkRe.match(query); match.hasMatch()) {
		return '@' + match.captured(1);
	}
	static const auto nameRe = QRegularExpression(
		u"^[A-Za-z][A-Za-z0-9_]{3,31}$"_q);
	return nameRe.match(query).hasMatch() ? ('@' + query) : query;
}

Screen::Screen(
	not_null<Ui::GenericBox*> box,
	not_null<Window::SessionController*> controller,
	const QString &autoQuery)
: _box(box)
, _controller(controller)
, _autoQuery(AsLookupQuery(autoQuery))
, _recentList(LoadRecent()) {
	const auto autoQueryClean = _autoQuery;
	const auto active = ActiveSquidwardSession();
	const auto isCollecting = active && active->collecting();
	const auto hasActiveResult = active && (
		(autoQueryClean.isEmpty() && active->hasResults())
		|| (!autoQueryClean.isEmpty() && active->isQuery(autoQueryClean)));

	if (isCollecting || hasActiveResult) {
		_session = active;
		_session->setController(controller);
	} else {
		if (active && !active->collecting()) {
			ClearActiveSquidwardSession();
		}
		_session = std::make_shared<Session>(controller);
		SetActiveSquidwardSession(_session);
	}
	_via = ViaAccount(_session->account());

	setupHeader();
	setupSearch();
	Ui::AddDivider(_box->verticalLayout());
	_card = _box->addRow(
		object_ptr<Ui::VerticalLayout>(_box),
		style::margins());
	_collect = _box->addRow(
		object_ptr<Ui::VerticalLayout>(_box),
		style::margins());
	setupEvents();
	renderRecent();

	if (_session->currentCard()) {
		const auto card = *_session->currentCard();
		_lastQuery = _session->currentQuery();
		_field->setText(_lastQuery);
		renderCard(card);
		if (_session->collecting()) {
			const auto progress = _session->currentProgress().value_or(Progress{});
			renderProgress(progress);
		} else if (const auto groups = _session->lastGroups()) {
			_groups = *groups;
			renderGroups(*groups);
		} else if (const auto users = _session->lastUsers()) {
			_users = *users;
			renderUsers(*users);
		}
	} else {
		renderIdle();
		if (!_autoQuery.isEmpty()) {
			_autoCollect = true;
			_session->setAutoSave(true);
			_field->setText(_autoQuery);
			start(_autoQuery);
		}
	}
}

void Screen::setupHeader() {
	_box->setTitle(rpl::single(u"Сквидвард"_q));
	_status = _box->addRow(object_ptr<Ui::FlatLabel>(
		_box,
		u"Подключаемся к боту…"_q,
		st::boxLabel));

	// Squidward talks to the bot from the account this window is on. An
	// account put in the «Корзина» is hidden from Telegram's own account
	// list, so there is no other way to pick it - this list shows every
	// account (trashed ones marked with the trash icon) and switches the window to the one
	// chosen, exactly like the manager's «Открыть».
	const auto link = _box->addRow(
		object_ptr<Ui::LinkButton>(_box, u"Сменить аккаунт…"_q),
		st::boxRowPadding);
	link->setClickedCallback([=] {
		const auto current = _session ? _session->account().get() : &_controller->session().account();
		_box->getDelegate()->show(Box([=](not_null<Ui::GenericBox*> picker) {
			picker->setTitle(rpl::single(u"Через какой аккаунт пробивать"_q));
			picker->setWidth(st::boxWideWidth);
			const auto search = picker->addRow(
				object_ptr<Ui::InputField>(
					picker,
					st::defaultInputField,
					rpl::single(
						u"Имя, @username или телефон"_q)),
				st::boxRowPadding);
			const auto list = picker->addRow(
				object_ptr<Ui::VerticalLayout>(picker));

			// Folders and the trash start folded; a click on a header
			// unfolds / folds it. A search unfolds everything it found.
			const auto open = std::make_shared<base::flat_set<QString>>();
			const auto choose = [=](not_null<Main::Account*> account) {
				if (account.get() == current) {
					picker->closeBox();
					return;
				}
				// Not from inside this click: switching accounts rebuilds
				// the window (and this very box with it). The box is not
				// closed here: the shell drops every layer itself when the
				// account changes (a box mid-close-animation under that
				// instant hide crashed). Opening wakes an account from the
				// «Корзина» (see Domain::activate).
				Ui::PostponeCall(picker, [=] {
					const auto all = Core::App().domain().orderedAccounts();
					if (ranges::find(all, account) != end(all)) {
						Core::App().domain().maybeActivate(account);
					}
				});
			};
			const auto rebuildSelf
				= std::make_shared<Fn<void()>>();
			const auto rebuild = [=] {
				list->clear();
				auto query = search->getLastText().trimmed().toLower();
				if (query.startsWith('@')) {
					query = query.mid(1);
				}
				auto currentRow = std::vector<not_null<Main::Account*>>();
				auto folderOrder = std::vector<QString>();
				auto folders = base::flat_map<
					QString,
					std::vector<not_null<Main::Account*>>>();
				auto trash = std::vector<not_null<Main::Account*>>();
				for (const auto &account
						: Core::App().domain().orderedAccounts()) {
					if (!Main::AccountIsLive(account)) {
						continue;
					}
					const auto haystack = (Settings::AccountPickerText(account)
						+ ' ' + account->folderTag()).toLower();
					if (!query.isEmpty() && !haystack.contains(query)) {
						continue;
					}
					if (account.get() == current) {
						currentRow.push_back(account);
					} else if (account->trashed()) {
						trash.push_back(account);
					} else {
						const auto tag = account->folderTag();
						auto &bucket = folders[tag];
						if (bucket.empty()) {
							folderOrder.push_back(tag);
						}
						bucket.push_back(account);
					}
				}
				ranges::stable_sort(folderOrder, [](
						const QString &a,
						const QString &b) {
					return !a.isEmpty() && b.isEmpty();
				});
				const auto searching = !query.isEmpty();
				const auto addRows = [&](
						const QString &title,
						const std::vector<not_null<Main::Account*>> &rows,
						const QString &glyph,
						bool foldable = true,
						const style::icon *icon = nullptr) {
					if (rows.empty()) {
						return;
					}
					const auto counted = title + u" · "_q
						+ QString::number(rows.size());
					const auto unfolded = !foldable
						|| searching
						|| open->contains(title);
					if (foldable) {
						const auto header = list->add(
							object_ptr<Ggtg::ActionRow>(
								list,
								rpl::single((unfolded ? u"▾  "_q : u"▸  "_q)
									+ counted),
								QString(),
								&st::menuIconShowInFolder));
						header->setClickedCallback([=] {
							if (!open->remove(title)) {
								open->insert(title);
							}
							// Not a direct rebuild: this header is about to
							// be destroyed.
							Ui::PostponeCall(picker, [=] {
								(*rebuildSelf)();
							});
						});
					} else {
						Ui::AddSubsectionTitle(list, rpl::single(counted));
					}
					if (!unfolded) {
						return;
					}
					for (const auto &account : rows) {
						auto text = Settings::AccountPickerText(account);
						if (account->asleep()) {
							text += u" · не подключён"_q;
						}
						const auto row = list->add(
							object_ptr<Ggtg::ActionRow>(
								list,
								rpl::single(text),
								glyph,
								icon,
								account->sessionExists()
									? account->session().user().get()
									: nullptr));
						row->setClickedCallback([=] { choose(account); });
					}
				};
				addRows(u"Сейчас"_q, currentRow, u"✓"_q, false);
				for (const auto &tag : folderOrder) {
					addRows(
						tag.isEmpty() ? u"Без папки"_q : tag,
						folders[tag],
						u"›"_q);
				}
				addRows(
					u"Корзина (подключатся при выборе)"_q,
					trash,
					QString(),
					true,
					&st::menuIconDelete);
				if (currentRow.empty() && folderOrder.empty() && trash.empty()) {
					list->add(
						object_ptr<Ui::FlatLabel>(
							list,
							u"Ничего не найдено"_q,
							st::boxLabel),
						st::boxRowPadding);
				}
			};
			*rebuildSelf = rebuild;
			search->changes(
			) | rpl::on_next(rebuild, search->lifetime());
			rebuild();
			picker->setFocusCallback([=] { search->setFocusFast(); });
			picker->addButton(
				rpl::single(u"Закрыть"_q),
				[=] { picker->closeBox(); });
		}));
	});
}

void Screen::setupSearch() {
	auto wrapOwned = object_ptr<Ui::RpWidget>(_box);
	const auto wrap = wrapOwned.data();
	_field = Ui::CreateChild<Ui::InputField>(
		wrap,
		st::defaultInputField,
		rpl::single(u"@username, ID или ссылка"_q));
	const auto button = Ui::CreateChild<Ui::RoundButton>(
		wrap,
		rpl::single(u"Пробить"_q),
		st::defaultActiveButton);
	button->setTextTransform(Ui::RoundButtonTextTransform::NoTransform);
	wrap->widthValue(
	) | rpl::on_next([=](int width) {
		const auto skip = st::boxRowPadding.left() / 2;
		_field->resizeToWidth(width - button->width() - skip);
		const auto height = std::max(_field->height(), button->height());
		_field->moveToLeft(0, height - _field->height(), width);
		button->moveToRight(0, height - button->height(), width);
		wrap->resize(width, height);
	}, wrap->lifetime());
	_box->addRow(std::move(wrapOwned));
	button->setClickedCallback([=] { submit(); });
	_field->submits(
	) | rpl::on_next([=](Qt::KeyboardModifiers) {
		submit();
	}, _field->lifetime());
	_box->setFocusCallback([=] { _field->setFocusFast(); });

	_recent = _box->addRow(
		object_ptr<Ui::SlideWrap<Ui::FlatLabel>>(
			_box,
			object_ptr<Ui::FlatLabel>(_box, QString(), st::boxLabel)),
		st::boxRowPadding);
	_recent->entity()->setClickHandlerFilter([=](
			const ClickHandlerPtr &handler,
			Qt::MouseButton) {
		const auto url = handler ? handler->url() : QString();
		const auto prefix = u"squid:"_q;
		if (url.startsWith(prefix)) {
			showRecentList();
		}
		return false;
	});
}

void Screen::setupEvents() {
	_sessionLifetime.destroy();
	_session->status(
	) | rpl::on_next([=](BotStatus status) {
		_status->setText(status == BotStatus::Ready
			? (u"Бот "_q + _session->botName() + u" подключён через "_q + _via)
			: status == BotStatus::Failed
			? u"Бот недоступен — проверьте сеть и откройте раздел заново"_q
			: u"Подключаемся к боту…"_q);
		// The state is told by color, not by an emoji dot.
		_status->setTextColorOverride(status == BotStatus::Ready
			? st::boxTextFgGood->c
			: status == BotStatus::Failed
			? st::boxTextFgError->c
			: st::windowSubTextFg->c);
		if (status == BotStatus::Ready && !_pending.isEmpty()) {
			start(base::take(_pending), true);
		} else if (status == BotStatus::Failed && !_pending.isEmpty()) {
			_lastQuery = base::take(_pending);
			renderLookupError(
				u"Бот (и его зеркала) недоступен. Проверьте сеть."_q);
		}
	}, _sessionLifetime);
	_session->cards(
	) | rpl::on_next([=](const Card &card) {
		_fromCache = false;
		renderCard(card);
		Lookups().putCard(_lastQuery, card);
		if (card.refresh) {
			return;
		}
		const auto collectNext = std::exchange(_collectNext, false);
		if (std::exchange(_autoCollect, false)
			&& card.list == CardKind::Group) {
			_autoSave = true;
			_session->collect();
		} else if (collectNext && card.list != CardKind::Unknown) {
			_session->collect();
		}
	}, _sessionLifetime);
	_session->progress(
	) | rpl::on_next([=](const Progress &progress) {
		if (progress.total > 0 && !_lastQuery.isEmpty()) {
			_totals[_lastQuery] = progress.total;
			Lookups().setTotal(_lastQuery, progress.total);
		}
		renderProgress(progress);
	}, _sessionLifetime);
	_session->groupsCollected(
	) | rpl::on_next([=](const GroupsResult &result) {
		_groups = result;
		if (!_savedGroupsId && !result.stopped) {
			Lookups().putGroups(_lastQuery, result);
		}
		if (_savedGroupsId) {
			// A resumed check: the saved base drops the channels found.
			Data::SaveScrapedGroupEntries(
				_savedGroupsId,
				_savedGroupsTitle,
				result.groups);
			renderSaved(
				_savedGroupsId,
				_savedGroupsTitle,
				int(Data::LoadScrapedGroupEntries(_savedGroupsId).size()),
				int(result.groups.size()),
				GroupsCopyText(result));
		} else {
			renderGroups(result);
		}
	}, _sessionLifetime);
	_session->usersCollected(
	) | rpl::on_next([=](const UsersResult &result) {
		_users = result;
		if (!result.stopped) {
			Lookups().putUsers(_lastQuery, result);
		}
		if (std::exchange(_autoSave, false)
			&& !result.stopped
			&& !result.usernames.isEmpty()) {
			saveUsers();
		} else {
			renderUsers(result);
		}
	}, _sessionLifetime);
	_session->errors(
	) | rpl::on_next([=](const QString &text) {
		_autoCollect = false;
		if (!_current) {
			renderLookupError(text);
		} else {
			Ggtg::Notice(_box->uiShow(), text);
		}
	}, _sessionLifetime);
}

void Screen::submit() {
	const auto query = _field->getLastText().trimmed();
	if (query.isEmpty()) {
		return;
	}
	if (!_session->collecting()) {
		start(query);
		return;
	}
	const auto weak = QPointer<Ui::GenericBox>(_box.get());
	_box->uiShow()->showBox(Ui::MakeConfirmBox({
		.text = u"Идёт сбор списка. Остановить его и пробить новое?"_q,
		.confirmed = [=](Fn<void()> &&close) {
			close();
			if (weak) {
				_session->stop();
				start(query);
			}
		},
		.confirmText = u"Остановить"_q,
	}));
}

void Screen::start(const QString &query, bool force) {
	if (_session->account() != &_controller->session().account() && !_session->collecting()) {
		_session = std::make_shared<Session>(_controller);
		SetActiveSquidwardSession(_session);
		_via = ViaAccount(_session->account());
		setupEvents();
	}
	// Only 🦑's own query collects and saves by itself.
	_autoCollect = _autoCollect && (query == _autoQuery);
	_session->setAutoSave(_autoCollect);
	_autoSave = false;
	_lastQuery = query;
	_recentList = PushRecent(_recentList, query, kRecentLimit);
	SaveRecent(_recentList);
	renderRecent();
	_current.reset();
	_groups.reset();
	_users.reset();
	_savedGroupsId = 0;
	_collect->clear();
	_fromCache = false;
	if (!force) {
		_collectNext = false;
		if (showCached(query)) {
			return;
		}
	}
	if (!_session->ready()) {
		_pending = query;
		renderSearching(u"Подключаемся к боту…"_q);
		_session->reconnect();
		return;
	}
	renderSearching(u"Пробиваем "_q + query + u"…"_q);
	_session->lookup(query);
}

bool Screen::showCached(const QString &query) {
	const auto saved = Lookups().find(query);
	if (!saved) {
		return false;
	}
	// 🦑 from «База» needs the member list: a card alone isn't enough.
	const auto wantsList = _autoCollect
		&& saved->card->list == CardKind::Group;
	if (wantsList && !saved->users) {
		return false;
	}
	auto card = *saved->card;
	card.refresh = false;
	_fromCache = true;
	_cachedAt = saved->savedAt;
	if (saved->total > 0) {
		_totals[_lastQuery] = saved->total;
	}
	renderCard(card);
	if (listOfGroups() && saved->groups) {
		_groups = *saved->groups;
		renderGroups(*_groups);
	} else if (!listOfGroups() && saved->users) {
		_users = *saved->users;
		if (std::exchange(_autoCollect, false)
			&& !_users->usernames.isEmpty()) {
			saveUsers();
		} else {
			renderUsers(*_users);
		}
	}
	return true;
}

// A list can only be collected from the bot's live card; one shown from the
// saved lookups is first asked for again.
void Screen::collectNow() {
	if (_fromCache) {
		_collectNext = true;
		start(_lastQuery, true);
	} else {
		_session->collect();
	}
}

void Screen::refreshLookup() {
	_collectNext = _groups.has_value() || _users.has_value();
	start(_lastQuery, true);
}

void Screen::renderRecent() {
	auto text = TextWithEntities();
	text.append(Ui::Text::Link(
		u"Недавние ("_q + QString::number(_recentList.size()) + u") ▸"_q,
		u"squid:list"_q));
	_recent->entity()->setMarkedText(text);
	_recent->toggle(!_recentList.isEmpty(), anim::type::instant);
}

// A list of the recent lookups; a click shows what was saved for it (no new
// run through the bot) and only asks the bot when nothing was saved.
void Screen::showRecentList() {
	const auto list = _recentList;
	_box->getDelegate()->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"Недавние пробивы"_q));
		box->setWidth(st::boxWideWidth);
		for (const auto &query : list) {
			const auto saved = Lookups().find(query);
			auto title = query;
			if (saved) {
				title += u"  ·  сохранено "_q
					+ QDateTime::fromSecsSinceEpoch(saved->savedAt).toString(
						u"dd.MM HH:mm"_q);
			} else {
				title += u"  ·  результата нет, пробьём заново"_q;
			}
			const auto button = box->addRow(
				object_ptr<Ui::SettingsButton>(
					box,
					rpl::single(title)),
				style::margins());
			button->setClickedCallback([=] {
				box->closeBox();
				_field->setText(query);
				submit();
			});
		}
		box->addButton(
			rpl::single(u"Закрыть"_q),
			[=] { box->closeBox(); });
	}));
}

void Screen::renderIdle() {
	_card->clear();
	_card->add(object_ptr<Ggtg::EmptyState>(
		_card,
		u"Введите @username, ID или ссылку на группу — "
			"здесь появится досье."_q));
}

void Screen::renderSearching(const QString &text) {
	_card->clear();
	Ui::AddSkip(_card);
	AddLabel(_card, text);
}

void Screen::renderLookupError(const QString &text) {
	_card->clear();
	Ui::AddSkip(_card);
	AddLabel(_card, text);
	const auto query = _lastQuery;
	AddButtons(_card, {
		{ u"Повторить"_q, &st::defaultLightButton, [=] { start(query); } },
	});
}

void Screen::addHeader(
		const QString &title,
		const QString &subtitle,
		const QString &id) {
	const auto row = _card->add(
		object_ptr<Ggtg::InfoRow>(
			_card,
			rpl::single(Ggtg::InfoRowData{
				.title = title,
				.subtitle = subtitle,
				.chip = id.isEmpty() ? QString() : (u"ID "_q + id),
			})),
		st::boxRowPadding);
	if (!id.isEmpty()) {
		row->setClickedCallback([=] {
			QGuiApplication::clipboard()->setText(id);
			Ggtg::Notice(_box->uiShow(), u"ID скопирован"_q);
		});
	}
}

void Screen::renderCard(const Card &card) {
	// A later edit of the same card redraws only the card: a list being
	// collected, or already collected, stays below it.
	const auto keepCollect = card.refresh
		&& (_session->collecting() || _groups || _users);
	_current = card;
	_card->clear();
	Ui::AddSkip(_card);
	if (_fromCache) {
		AddLabel(_card, u"Сохранённый результат от "_q
			+ QDateTime::fromSecsSinceEpoch(_cachedAt).toString(
				u"dd.MM.yyyy HH:mm"_q)
			+ u" — бот не запрашивался"_q);
		AddButtons(_card, {
			{ u"Пробить заново"_q, &st::defaultLightButton, [=] {
				refreshLookup();
			} },
		});
		Ui::AddSkip(_card);
	}
	if (card.kind == CardKind::User) {
		renderUser(card);
		AddExpander(_card, u"Исходный ответ бота"_q, card.rawText);
	} else if (card.kind == CardKind::Group) {
		renderGroup(card);
		AddExpander(_card, u"Исходный ответ бота"_q, card.rawText);
	} else {
		AddLabel(_card, u"Бот ответил:"_q);
		AddLabel(_card, card.rawText);
	}
	if (!keepCollect) {
		_groups.reset();
		_users.reset();
		_savedGroupsId = 0;
		renderCollectIdle();
	}
}

bool Screen::listOfGroups() const {
	return _current && (_current->list == CardKind::User);
}

QString Screen::countSuffix() const {
	const auto i = _totals.find(_lastQuery);
	return (i != end(_totals)) ? (u" · "_q + FormatCount(i->second)) : QString();
}

void Screen::renderUser(const Card &card) {
	const auto &user = *card.user;
	const auto handle = user.usernames.isEmpty()
		? card.query
		: (u"@"_q + user.usernames.front());
	addHeader(
		(user.name.isEmpty() ? handle : user.name),
		handle,
		user.id);
	const auto details = FormatUserDetails(user);
	if (!details.isEmpty()) {
		AddLabel(_card, details.join(u'\n'));
	}
	if (!user.names.empty()) {
		auto lines = QStringList();
		for (const auto &change : user.names) {
			lines.push_back(change.date + u" — "_q + change.name);
		}
		AddExpander(
			_card,
			u"Имена: "_q + QString::number(user.names.size()) + u" изм."_q,
			lines.join(u'\n'));
	}
}

void Screen::renderGroup(const Card &card) {
	const auto &group = *card.group;
	const auto username = UsernameFromQuery(card.query);
	addHeader(
		(group.name.isEmpty() ? card.query : group.name),
		username.isEmpty() ? card.query : (u"@"_q + username),
		group.id);
	const auto details = FormatGroupDetails(group);
	if (!details.isEmpty()) {
		AddLabel(_card, details.join(u'\n'));
	}
	if (!group.description.isEmpty()) {
		AddExpander(_card, u"Описание"_q, group.description);
	}
	if (!group.topDay.empty()) {
		auto lines = QStringList();
		for (auto i = 0; i != int(group.topDay.size()); ++i) {
			lines.push_back(QString::number(i + 1) + u". "_q
				+ group.topDay[i].name + u" — "_q
				+ FormatCount(group.topDay[i].messages));
		}
		AddExpander(_card, u"Топ за сутки"_q, lines.join(u'\n'));
	}
}

void Screen::renderCollectIdle() {
	_collect->clear();
	if (!_current || _current->list == CardKind::Unknown) {
		return;
	}
	const auto isUser = listOfGroups();
	Ui::AddSkip(_collect);
	AddButtons(_collect, {
		{
			(isUser ? u"Собрать группы"_q : u"Собрать участников"_q)
				+ countSuffix(),
			&st::defaultActiveButton,
			[=] { collectNow(); },
		},
	});
}

void Screen::renderProgress(const Progress &progress) {
	const auto isUser = listOfGroups();
	// Rebuilt only when the block changes mode: a "Стоп" recreated on every
	// page would swallow a press that lands just as a page arrives.
	const auto rebuild = !_progressBar
		|| !_progressLabel
		|| (_progressChecking != progress.checking);
	if (rebuild) {
		_collect->clear();
		Ui::AddSkip(_collect);
		_progressBar = _collect->add(
			object_ptr<Ggtg::ProgressBar>(_collect),
			st::boxRowPadding);
		_progressLabel = AddLabel(_collect, QString());
		_progressChecking = progress.checking;
		if (!progress.checking) {
			AddButtons(_collect, {
				{ u"Стоп"_q, &st::defaultLightButton, [=] {
					_session->stop();
				} },
			});
		}
	}
	const auto bar = _progressBar.data();
	bar->setValue((progress.checking && progress.pages <= 0)
		? 1.
		: (progress.pages > 0)
		? (double(progress.page) / progress.pages)
		: 0.);
	const auto found = FormatCount(progress.found);
	const auto wait = (progress.floodWait > 0)
		? (u"\nTelegram попросил паузу — ждём "_q
			+ QString::number(progress.floodWait) + u" с…"_q)
		: QString();
	const auto checked = u"проверено "_q + FormatCount(progress.page)
		+ u" из "_q + FormatCount(progress.pages) + u", осталось "_q
		+ FormatCount(std::max(progress.pages - progress.page, 0));
	_progressLabel->setText(progress.checking
		? (isUser
			? ((progress.pages > 0)
				? (u"Отделяем группы от каналов: "_q + checked
					+ u"\nГрупп оставлено: "_q + found + wait)
				: (u"Отделяем группы от каналов… оставлено "_q + found))
			: u"Уточняем участников… найдено "_q + found)
		: (progress.pages > 0)
		? (u"Страница "_q + FormatCount(progress.page) + u" из "_q
			+ FormatCount(progress.pages) + u" · найдено "_q + found)
		: u"Запрашиваем список у бота…"_q);
}

void Screen::renderGroups(const GroupsResult &result) {
	_collect->clear();
	Ui::AddSkip(_collect);
	if (result.groups.empty()) {
		AddLabel(_collect, result.stopped
			? u"Сбор остановлен, групп не собрано."_q
			: u"Групп не найдено."_q);
		AddButtons(_collect, {
			{ u"Собрать заново"_q + countSuffix(), &st::defaultLightButton, [=] {
				collectNow();
			} },
		});
		return;
	}
	auto summary = u"✓ Групп: "_q + FormatCount(result.groups.size());
	if (result.channelsRemoved) {
		summary += u" · каналов убрано: "_q
			+ FormatCount(result.channelsRemoved);
	}
	if (result.closed) {
		summary += u" · закрытых: "_q + FormatCount(result.closed);
	}
	if (result.stopped) {
		summary += u" · сбор остановлен"_q;
	}
	AddLabel(_collect, summary);
	if (result.unchecked) {
		AddLabel(_collect, u"Внимание: "_q + FormatCount(result.unchecked)
			+ u" не проверено на каналы: Telegram временно ограничил "
				"проверку. Среди них могут быть каналы."_q);
		addContinueCheck();
	}
	auto names = QStringList();
	auto lines = QStringList();
	for (const auto &entry : result.groups) {
		if (names.size() < kPreviewCount) {
			names.push_back(GroupName(entry));
		}
		lines.push_back(GroupLine(entry));
	}
	AddLabel(
		_collect,
		names.join(u" · "_q)
			+ (int(result.groups.size()) > kPreviewCount ? u" …"_q : QString()));
	const auto joined = lines.join(u'\n');
	AddButtons(_collect, {
		{ u"Сохранить в базу"_q, &st::defaultActiveButton, [=] {
			saveGroups();
		} },
		{ u"Копировать"_q, &st::defaultLightButton, [=] {
			QGuiApplication::clipboard()->setText(joined);
			Ggtg::Notice(_box->uiShow(), u"Скопировано"_q);
		} },
	});
}

void Screen::renderUsers(const UsersResult &result) {
	_collect->clear();
	Ui::AddSkip(_collect);
	if (result.usernames.isEmpty()) {
		AddLabel(_collect, result.stopped
			? u"Сбор остановлен, юзернеймов не собрано."_q
			: u"Участников с юзернеймом не найдено."_q);
		AddButtons(_collect, {
			{ u"Собрать заново"_q + countSuffix(), &st::defaultLightButton, [=] {
				collectNow();
			} },
		});
		return;
	}
	AddLabel(
		_collect,
		u"✓ Юзернеймов: "_q + FormatCount(result.usernames.size())
			+ (result.stopped ? u" · сбор остановлен"_q : QString()));
	auto handles = QStringList();
	for (const auto &name : result.usernames) {
		handles.push_back(u"@"_q + name);
	}
	AddLabel(
		_collect,
		handles.mid(0, kPreviewCount).join(u" · "_q)
			+ (handles.size() > kPreviewCount ? u" …"_q : QString()));
	const auto joined = handles.join(u'\n');
	AddButtons(_collect, {
		{ u"Сохранить в базу"_q, &st::defaultActiveButton, [=] {
			saveUsers();
		} },
		{ u"Копировать"_q, &st::defaultLightButton, [=] {
			QGuiApplication::clipboard()->setText(joined);
			Ggtg::Notice(_box->uiShow(), u"Скопировано"_q);
		} },
	});
}

void Screen::saveGroups() {
	if (!_groups || !_current) {
		return;
	}
	const auto title = _session->baseTitle();
	const auto id = Data::GenerateManualBaseId();
	Data::SaveScrapedGroupEntries(id, title, _groups->groups);
	_savedGroupsId = id;
	_savedGroupsTitle = title;
	renderSaved(
		id,
		title,
		int(Data::LoadScrapedGroupEntries(id).size()),
		int(_groups->groups.size()),
		GroupsCopyText(*_groups));
}

void Screen::addContinueCheck() {
	if (!_groups || _groups->uncheckedGroups.empty()) {
		return;
	}
	AddButtons(_collect, {
		{
			u"Продолжить проверку · "_q
				+ FormatCount(_groups->uncheckedGroups.size()),
			&st::defaultLightButton,
			[=] {
				if (_groups) {
					_session->continueCheck(*_groups);
				}
			},
		},
	});
}

void Screen::saveUsers() {
	if (!_users || !_current) {
		return;
	}
	auto handles = QStringList();
	for (const auto &name : _users->usernames) {
		handles.push_back(u"@"_q + name);
	}
	const auto title = _session->baseTitle();
	const auto id = Data::GenerateManualBaseId();
	Data::SaveScrapedGroupUsernames(id, title, _users->usernames);
	renderSaved(
		id,
		title,
		int(Data::LoadScrapedGroupUsernames(id).size()),
		int(_users->usernames.size()),
		handles.join(u'\n'));
}

void Screen::renderSaved(
		uint64 id,
		const QString &title,
		int saved,
		int collected,
		const QString &copyText) {
	_collect->clear();
	Ui::AddSkip(_collect);
	auto text = u"✓ Сохранено в «"_q + title + u"»: "_q + FormatCount(saved);
	if (saved < collected) {
		text += u" (ещё "_q + FormatCount(collected - saved)
			+ u" уже есть в других базах)"_q;
	}
	AddLabel(_collect, text);
	const auto show = _box->uiShow();
	using ButtonSpec = std::tuple<
		QString,
		const style::RoundButton*,
		Fn<void()>>;
	auto buttons = std::vector<ButtonSpec>{
		{ u"✓ В базе — открыть"_q, &st::defaultActiveButton, [=] {
			Settings::ShowSavedBase(show, id);
		} },
	};
	if (id != _savedGroupsId) {
		// The next step of the chain: write to the people just saved.
		const auto controller = _controller;
		buttons.push_back({
			u"Разослать по этой базе"_q,
			&st::defaultLightButton,
			[=] { Settings::OpenSmsCampaign(controller, id, show); },
		});
	}
	buttons.push_back({
		u"Копировать"_q, &st::defaultLightButton, [=] {
			QGuiApplication::clipboard()->setText(copyText);
			Ggtg::Notice(show, u"Скопировано"_q);
		},
	});
	AddButtons(_collect, std::move(buttons));
	if (id == _savedGroupsId && _groups && _groups->unchecked) {
		AddLabel(_collect, u"Внимание: "_q + FormatCount(_groups->unchecked)
			+ u" в базе не проверено на каналы."_q);
		addContinueCheck();
	}
	Ggtg::Notice(show, text);
}

} // namespace

object_ptr<Ui::BoxContent> MakeSquidwardBox(
		not_null<SessionController*> controller,
		const QString &lookupGroup) {
	return Box([=](not_null<Ui::GenericBox*> box) {
		box->setWidth(st::boxWideWidth);
		box->lifetime().make_state<Screen>(box, controller, lookupGroup);
		Ggtg::AddCloseCross(box);
	});
}

} // namespace Window
