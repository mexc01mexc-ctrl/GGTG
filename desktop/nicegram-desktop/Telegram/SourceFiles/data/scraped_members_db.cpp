/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "data/scraped_members_db.h"
#include "window/ggtg_widgets.h"
#include "data/account_spam_status.h"
#include "core/ggtg_paths.h"

#include "api/api_chat_participants.h"
#include "apiwrap.h"
#include "base/call_delayed.h"
#include "base/flat_set.h"
#include "base/random.h"
#include "base/timer.h"
#include "base/unixtime.h"
#include "chat_helpers/compose/compose_show.h"
#include "core/application.h"
#include "core/ggtg_log.h"
#include "core/ggtg_ai_feedback.h"
#include "core/process_manager.h"
#include "data/data_channel.h"
#include "data/data_chat.h"
#include "data/data_lastseen_status.h"
#include "data/data_peer_values.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "history/history.h"
#include "history/history_item.h"
#include "main/main_account.h"
#include "main/main_session.h"
#include "mtproto/sender.h"
#include "settings.h"
#include "ui/boxes/confirm_box.h"
#include "ui/layers/generic_box.h"
#include "ui/layers/show.h"
#include "ui/widgets/checkbox.h"
#include "ui/vertical_list.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"

#include <deque>
#include <map>

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QRegularExpression>
#include <QtCore/QSaveFile>

namespace Data {
namespace {

// A base with tens of thousands of usernames would still make one JSON
// file of a few hundred KB - fine on its own, but nowhere near the
// per-account encrypted prefs blob (Storage::Account::writePref), which
// keeps every pref in memory and rewrites all of them on every save.
[[nodiscard]] QString DbDir() {
	return Core::GgtgPaths::DbDir();
}

[[nodiscard]] QString IndexPath() {
	return DbDir() + u"index.json"_q;
}

[[nodiscard]] QString GroupPath(uint64 id) {
	return DbDir() + QString::number(id) + u".json"_q;
}

[[nodiscard]] QString SmsSentPath() {
	return DbDir() + u"sms_sent.json"_q;
}

[[nodiscard]] QJsonDocument ReadJson(const QString &path) {
	auto file = QFile(path);
	if (!file.open(QIODevice::ReadOnly)) {
		return QJsonDocument();
	}
	return QJsonDocument::fromJson(file.readAll());
}

void WriteJson(const QString &path, const QJsonDocument &document) {
	QDir().mkpath(DbDir());
	auto file = QSaveFile(path);
	if (file.open(QIODevice::WriteOnly)) {
		file.write(document.toJson(QJsonDocument::Compact));
		file.commit();
	}
}

[[nodiscard]] QString NamesPath() {
	return DbDir() + u"names.json"_q;
}

[[nodiscard]] QString UsersMetaPath() {
	return DbDir() + u"users_meta.json"_q;
}

[[nodiscard]] base::flat_map<QString, QString> &NamesCache() {
	static auto result = [] {
		auto map = base::flat_map<QString, QString>();
		const auto object = ReadJson(NamesPath()).object();
		for (auto i = object.begin(); i != object.end(); ++i) {
			map.emplace(i.key(), i.value().toString());
		}
		return map;
	}();
	return result;
}

[[nodiscard]] std::map<QString, UserMeta> &UsersMetaCache() {
	static auto result = [] {
		auto map = std::map<QString, UserMeta>();
		const auto object = ReadJson(UsersMetaPath()).object();
		for (auto i = object.begin(); i != object.end(); ++i) {
			const auto userObj = i.value().toObject();
			map.emplace(i.key(), UserMeta{
				.name = userObj.value(u"name"_q).toString(),
				.id = userObj.value(u"id"_q).toString().toULongLong(),
				.lastseen = userObj.value(u"lastseen"_q).toVariant().toLongLong(),
				.lastseenText = userObj.value(u"lastseenText"_q).toString(),
			});
		}
		return map;
	}();
	return result;
}

void SaveNamesNow() {
	auto object = QJsonObject();
	for (const auto &[username, name] : NamesCache()) {
		object.insert(username, name);
	}
	WriteJson(NamesPath(), QJsonDocument(object));
}

void SaveUsersMetaNow() {
	auto object = QJsonObject();
	for (const auto &[username, meta] : UsersMetaCache()) {
		object.insert(username, QJsonObject{
			{ u"name"_q, meta.name },
			{ u"id"_q, QString::number(meta.id) },
			{ u"lastseen"_q, qint64(meta.lastseen) },
			{ u"lastseenText"_q, meta.lastseenText },
		});
	}
	WriteJson(UsersMetaPath(), QJsonDocument(object));
}

void RememberName(const QString &username, const QString &name) {
	const auto key = username.trimmed().remove('@').toLower();
	const auto value = name.trimmed();
	if (key.isEmpty() || value.isEmpty()) {
		return;
	}
	auto &cache = NamesCache();
	const auto i = cache.find(key);
	if (i != cache.end() && i->second == value) {
		return;
	}
	cache[key] = value;
	// Many names come in a burst (a page of members): write once after it.
	static auto timer = base::Timer([] { SaveNamesNow(); });
	timer.callOnce(2000);
}

[[nodiscard]] QString NormalizeUsername(const QString &raw) {
	auto username = raw.trimmed();
	if (username.startsWith('@')) {
		username.remove(0, 1);
	}
	return username.toLower();
}

[[nodiscard]] QString SmsStatusToString(SmsSendStatus status) {
	switch (status) {
	case SmsSendStatus::SpamAcc: return u"spam_acc"_q;
	case SmsSendStatus::NotFound: return u"not_found"_q;
	case SmsSendStatus::Restricted: return u"restricted"_q;
	case SmsSendStatus::PeerLimit: return u"peer_limit"_q;
	case SmsSendStatus::AccountFail: return u"account_fail"_q;
	case SmsSendStatus::SuspectedFake: return u"suspected_fake"_q;
	case SmsSendStatus::LongAgo: return u"long_ago"_q;
	case SmsSendStatus::Sent: break;
	}
	return u"ok"_q;
}

[[nodiscard]] SmsSendStatus SmsStatusFromString(const QString &raw) {
	if (raw == u"spam_acc"_q) {
		return SmsSendStatus::SpamAcc;
	} else if (raw == u"not_found"_q) {
		return SmsSendStatus::NotFound;
	} else if (raw == u"restricted"_q) {
		return SmsSendStatus::Restricted;
	} else if (raw == u"peer_limit"_q) {
		return SmsSendStatus::PeerLimit;
	} else if (raw == u"account_fail"_q) {
		return SmsSendStatus::AccountFail;
	} else if (raw == u"suspected_fake"_q) {
		return SmsSendStatus::SuspectedFake;
	} else if (raw == u"long_ago"_q) {
		return SmsSendStatus::LongAgo;
	}
	return SmsSendStatus::Sent;
}

// One stream for "something in the bases or the registry changed" - see
// BasesChanged().
[[nodiscard]] rpl::event_stream<> &ChangesStream() {
	static auto result = rpl::event_stream<>();
	return result;
}

// Bumped on every change of the registry or of a base's people, so the
// per-base counters (ComputeBaseStats) are recounted only when something
// really changed, not on every redraw of the "База" list.
[[nodiscard]] uint64 &StatsGeneration() {
	static auto result = uint64(1);
	return result;
}

// What was deleted on purpose and when ("base:<id>", "sent:<username>"):
// without it a sync with another device would bring the deleted thing back
// from that device's copy. Kept for 90 days, written a moment after a change.
[[nodiscard]] std::map<QString, qint64> &TombstoneCache() {
	static auto result = [] {
		auto map = std::map<QString, qint64>();
		const auto object = ReadJson(DbDir() + u"sync_tombstones.json"_q)
			.object();
		const auto limit = qint64(base::unixtime::now()) - 90 * 86400;
		for (auto it = object.begin(); it != object.end(); ++it) {
			const auto at = it.value().toString().toLongLong();
			if (at > limit) {
				map[it.key()] = at;
			}
		}
		return map;
	}();
	return result;
}

void SaveTombstonesNow() {
	auto object = QJsonObject();
	for (const auto &[key, at] : TombstoneCache()) {
		object.insert(key, QString::number(at));
	}
	WriteJson(DbDir() + u"sync_tombstones.json"_q, QJsonDocument(object));
}

void SaveTombstones() {
	static auto timer = base::Timer([] { SaveTombstonesNow(); });
	static auto hooked = false;
	if (!std::exchange(hooked, true)) {
		QObject::connect(qApp, &QCoreApplication::aboutToQuit, [] {
			SaveTombstonesNow();
		});
	}
	if (!timer.isActive()) {
		timer.callOnce(2000);
	}
}

void NoteDeletedKey(const QString &key) {
	TombstoneCache()[key] = qint64(base::unixtime::now());
	SaveTombstones();
}

// The whole sms_sent.json registry, parsed once and then kept in memory:
// FindSmsSendRecord is called once per recipient (the campaign's pre-pass,
// and the recipients preview on every checkbox toggle), and re-reading and
// re-parsing the entire file for each call froze the UI on big lists.
// Mutations write through to disk immediately (they only happen once per
// real send, so the O(n) rewrite is fine); reads never touch the disk again.
[[nodiscard]] std::map<QString, SmsSendRecord> &SmsSentCache() {
	static auto loaded = false;
	static auto records = std::map<QString, SmsSendRecord>();
	if (!std::exchange(loaded, true)) {
		const auto object = ReadJson(SmsSentPath()).object();
		for (auto it = object.begin(); it != object.end(); ++it) {
			const auto entry = it.value().toObject();
			records[it.key()] = {
				.username = it.key(),
				.status = SmsStatusFromString(entry["status"].toString()),
				.account = entry["account"].toString(),
				.at = entry["at"].toString().toLongLong(),
				.baseId = entry["base"].toString().toULongLong(),
				.source = entry["source"].toString(),
				.note = entry["note"].toString(),
			};
		}
	}
	return records;
}

void SaveSmsSentCacheNow();

// A mailing records every message it sends; rewriting the whole registry
// (and refreshing every screen) for each one costs more and more as the
// registry grows. The cache in memory is always current, the file follows
// within two seconds (and at exit), the screens within a third of a second.
void SaveSmsSentCache() {
	++StatsGeneration();
	static auto saveTimer = base::Timer([] { SaveSmsSentCacheNow(); });
	static auto notifyTimer = base::Timer([] { ChangesStream().fire({}); });
	static auto hooked = false;
	if (!std::exchange(hooked, true)) {
		QObject::connect(qApp, &QCoreApplication::aboutToQuit, [] {
			SaveSmsSentCacheNow();
		});
	}
	if (!saveTimer.isActive()) {
		saveTimer.callOnce(2000);
	}
	if (!notifyTimer.isActive()) {
		notifyTimer.callOnce(300);
	}
}

void SaveSmsSentCacheNow() {
	auto object = QJsonObject();
	for (const auto &[username, record] : SmsSentCache()) {
		object[username] = QJsonObject{
			{ "status", SmsStatusToString(record.status) },
			{ "account", record.account },
			{ "at", QString::number(record.at) },
			{ "base", QString::number(record.baseId) },
			{ "source", record.source },
			{ "note", record.note },
		};
	}
	WriteJson(SmsSentPath(), QJsonDocument(object));
}

void SetSmsSendRecord(
		const QString &username,
		SmsSendStatus status,
		const QString &account,
		uint64 baseId) {
	const auto normalized = NormalizeUsername(username);
	if (normalized.isEmpty()) {
		return;
	}
	// The base's name goes along; a later update without a base (a late
	// answer, a status change) keeps what was known before.
	auto source = QString();
	if (baseId) {
		for (const auto &meta : LoadScrapedGroupsIndex()) {
			if (meta.id == baseId) {
				source = meta.title;
				break;
			}
		}
	}
	auto &previous = SmsSentCache()[normalized];
	if (source.isEmpty()) {
		source = previous.source;
		if (!baseId) {
			baseId = previous.baseId;
		}
	}
	previous = {
		.username = normalized,
		.status = status,
		.account = account,
		.at = qint64(base::unixtime::now()),
		.baseId = baseId,
		.source = source,
	};
	Core::GgtgLog::Info(
		u"Реестр отправок"_q,
		'@' + normalized + u" → "_q + SmsStatusToString(status)
			+ u" ("_q + account + u", база "_q + QString::number(baseId)
			+ u')');
	SaveSmsSentCache();
	if (status == SmsSendStatus::Sent) {
		// "Written to" must survive even a crash a second later: losing it
		// would mean writing to the same person again next time.
		SaveSmsSentCacheNow();
	}
	// Delivered for good or permanently inactive: the base it came from no longer needs it.
	if ((status == SmsSendStatus::Sent || status == SmsSendStatus::LongAgo) && baseId) {
		PruneSentFromBase(baseId, normalized);
	}
}

// Only the status "был(а) давно" counts: "недавно", "на этой неделе",
// "в этом месяце" and an exact time all stay unless exact time is > 30 days ago.
[[nodiscard]] bool IsLongInactive(
		const Data::LastseenStatus &status,
		TimeId now) {
	if (status.isLongAgo()) {
		return true;
	}
	if (const auto till = status.onlineTill()) {
		if (till > 0 && till < (now - 30 * 86400)) {
			return true;
		}
	}
	return false;
}

[[nodiscard]] QString BaseKindToString(BaseKind kind) {
	return (kind == BaseKind::Groups) ? u"groups"_q : u"users"_q;
}

[[nodiscard]] BaseKind BaseKindFromString(const QString &raw) {
	return (raw == u"groups"_q) ? BaseKind::Groups : BaseKind::Users;
}

// In-memory copies of the index and of each base's username list: the
// campaign screen re-reads every selected base on each checkbox toggle,
// and re-parsing tens of thousands of usernames from disk each time was the
// slow part. Every write below goes through here and keeps them current.
[[nodiscard]] std::optional<std::vector<ScrapedGroupMeta>> &IndexCache() {
	static auto result = std::optional<std::vector<ScrapedGroupMeta>>();
	return result;
}

[[nodiscard]] std::map<uint64, QStringList> &UsernamesCache() {
	static auto result = std::map<uint64, QStringList>();
	return result;
}

void WriteIndex(const std::vector<ScrapedGroupMeta> &index) {
	auto array = QJsonArray();
	for (const auto &entry : index) {
		array.append(QJsonObject{
			{ "id", QString::number(entry.id) },
			{ "title", entry.title },
			{ "count", entry.count },
			{ "savedAt", QString::number(entry.savedAt) },
			{ "kind", BaseKindToString(entry.kind) },
		});
	}
	WriteJson(IndexPath(), QJsonDocument(array));
	IndexCache() = index;
	ChangesStream().fire({});
}

// `kind` matters only when creating a brand new entry - overwriting an
// existing base always keeps its original kind (a base's shape on disk,
// usernames vs. group entries, never changes once created).
void AppendToIndex(
		uint64 id,
		const QString &title,
		int count,
		BaseKind kind) {
	auto index = LoadScrapedGroupsIndex();
	const auto now = qint64(base::unixtime::now());
	auto found = false;
	for (auto &entry : index) {
		if (entry.id == id) {
			entry.title = title;
			entry.count = count;
			entry.savedAt = now;
			found = true;
			break;
		}
	}
	if (!found) {
		index.push_back({
			.id = id,
			.title = title,
			.count = count,
			.savedAt = now,
			.kind = kind,
		});
	}
	WriteIndex(index);
}

// Bases whose in-memory list lost a delivered username and still has to
// be written to disk. A campaign delivers many messages a minute, several
// lanes at once, and rewriting a base of tens of thousands of names for
// every one of them would be wasteful - so the writes are coalesced into
// one a moment later (and FlushBasePrunes forces it, e.g. when the campaign
// ends). Nothing is lost if the app dies before the write: the registry
// already skips those names.
[[nodiscard]] base::flat_set<uint64> &DirtyPrunedBases() {
	static auto result = base::flat_set<uint64>();
	return result;
}

void FinishScrape(
		not_null<Window::SessionController*> controller,
		uint64 id,
		const QString &title,
		QStringList usernames) {
	if (usernames.isEmpty()) {
		// Hidden member lists, or a small group whose participants haven't
		// loaded, come back empty - saving that would just leave a
		// useless empty base (and, on a re-scrape, wipe a good one).
		controller->uiShow()->showToast(
			u"Не найдено ни одного участника с юзернеймом - база не "
				"создана и не изменена"_q);
		return;
	}
	const auto save = [=](const QStringList &list) {
		SaveScrapedGroupUsernames(id, title, list);
		controller->uiShow()->showToast(
			u"База: сохранено пользователей с юзернеймом в \""_q + title
				+ u"\": "_q
				+ QString::number(LoadScrapedGroupUsernames(id).size()));
	};
	const auto index = LoadScrapedGroupsIndex();
	const auto existing = ranges::find(index, id, &ScrapedGroupMeta::id);
	if (existing == index.end()) {
		save(usernames);
		return;
	}
	// A re-scrape of a group that already has a base would silently
	// overwrite it - including anything added or removed by hand since.
	const auto text = u"База \""_q + existing->title + u"\" уже есть ("_q
		+ QString::number(existing->count) + u"). Сейчас найдено: "_q
		+ QString::number(usernames.size())
		+ u". Объединить (добавить новых к имеющимся) или заменить "
			"целиком?"_q;
	// "Отменить" on the left leaves the existing base exactly as it is and
	// drops what was just found; Esc does the same.
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->addRow(object_ptr<Ui::FlatLabel>(box, text, st::boxLabel));
		box->addButton(rpl::single(u"Объединить"_q), [=] {
			box->closeBox();
			auto merged = LoadScrapedGroupUsernames(id);
			merged.append(usernames);
			save(merged);
		});
		box->addButton(rpl::single(u"Заменить"_q), [=] {
			box->closeBox();
			save(usernames);
		});
		box->addLeftButton(rpl::single(u"Отменить"_q), [=] {
			box->closeBox();
		});
	}));
}

// The `reason` a scrape reports when its "Остановить и сохранить" was used.
const auto kScrapeStoppedManually = u"остановлено вручную"_q;

// What to keep while collecting - shown as checkboxes before every scrape
// (see ShowScrapeOptions) and remembered between runs.
struct ScrapeOptions {
	bool skipBots = true;
	bool skipAdmins = true;
	bool skipInactive = true; // status "last seen a long time ago"
	bool messageAuthors = false;
	int messageAuthorsPages = 10;
	bool joinLeaveEvents = false;
	int joinLeaveEventsPages = 10;
};

[[nodiscard]] QString ScrapeOptionsPath() {
	return DbDir() + u"scrape_options.json"_q;
}

[[nodiscard]] ScrapeOptions LoadScrapeOptions() {
	auto result = ScrapeOptions();
	const auto document = ReadJson(ScrapeOptionsPath());
	if (!document.isObject()) {
		return result;
	}
	const auto object = document.object();
	const auto readBool = [&](const char *key, bool &value) {
		if (object.contains(QLatin1String(key))) {
			value = object[QLatin1String(key)].toBool(value);
		}
	};
	const auto readInt = [&](const char *key, int &value) {
		if (object.contains(QLatin1String(key))) {
			value = object[QLatin1String(key)].toInt(value);
		}
	};
	readBool("skipBots", result.skipBots);
	readBool("skipAdmins", result.skipAdmins);
	readBool("skipInactive", result.skipInactive);
	readBool("messageAuthors", result.messageAuthors);
	readInt("messageAuthorsPages", result.messageAuthorsPages);
	readBool("joinLeaveEvents", result.joinLeaveEvents);
	readInt("joinLeaveEventsPages", result.joinLeaveEventsPages);
	if (result.messageAuthorsPages <= 0) {
		result.messageAuthorsPages = 10;
	}
	if (result.joinLeaveEventsPages <= 0) {
		result.joinLeaveEventsPages = 10;
	}
	return result;
}

void SaveScrapeOptions(const ScrapeOptions &options) {
	WriteJson(ScrapeOptionsPath(), QJsonDocument(QJsonObject{
		{ "skipBots", options.skipBots },
		{ "skipAdmins", options.skipAdmins },
		{ "skipInactive", options.skipInactive },
		{ "messageAuthors", options.messageAuthors },
		{ "messageAuthorsPages", options.messageAuthorsPages },
		{ "joinLeaveEvents", options.joinLeaveEvents },
		{ "joinLeaveEventsPages", options.joinLeaveEventsPages },
	}));
}

// Everything except the admin check (which differs between a basic group's
// and a channel's participant data - see the two scrapers).
[[nodiscard]] bool UserAllowed(
		not_null<UserData*> user,
		const ScrapeOptions &options,
		TimeId now) {
	if (user->isSelf() || user->username().isEmpty()) {
		return false;
	}
	if (options.skipBots
		&& (user->isBot() || IsBotUsername(user->username()))) {
		return false;
	}
	if (options.skipInactive && IsLongInactive(user->lastseen(), now)) {
		return false;
	}
	return true;
}

// FLOOD_WAIT_30 and friends -> 30; 0 for any other error.
[[nodiscard]] int FloodWaitSeconds(const QString &type) {
	static const auto re = QRegularExpression(
		u"^(?:FLOOD|FLOOD_PREMIUM|SLOWMODE)_WAIT_(\\d+)$"_q);
	const auto match = re.match(type);
	return match.hasMatch() ? std::max(1, match.captured(1).toInt()) : 0;
}

void ScrapeChatMembers(
		not_null<Window::SessionController*> controller,
		not_null<ChatData*> chat,
		const ScrapeOptions &options) {
	const auto process = Core::App().processes().start(
		u"Сбор участников: "_q + chat->name(),
		nullptr,
		Core::ProcessKind::Scrape);
	auto collected = QStringList();
	const auto now = base::unixtime::now();
	const auto creatorId = peerFromUser(chat->creator);
	for (const auto &user : chat->participants) {
		if (options.skipAdmins
			&& ((user->id == creatorId) || chat->admins.contains(user))) {
			continue;
		}
		if (UserAllowed(user, options, now)) {
			collected.push_back(user->username());
			RememberPeerName(user);
		}
	}
	process->log(u"Участников в списке: "_q
		+ QString::number(chat->participants.size())
		+ u", подошло: "_q + QString::number(collected.size()));
	if (collected.isEmpty()) {
		process->fail(u"Ничего не найдено"_q);
	} else {
		process->succeed(u"Собрано: "_q + QString::number(collected.size()));
	}
	FinishScrape(
		controller,
		chat->id.value,
		GroupBaseTitle(chat->name(), QString()),
		std::move(collected));
}

// Unlike a bulk action's fixed-size account queue, the number of pages a
// scrape will take isn't known up front - so instead of pre-building one
// row per item (like RunBulkAction/RunSmsCampaign do), this appends one
// log line per page fetched, live, into a growing VerticalLayout, plus a
// running totals line at top. The same lines go to the "Процессы" log.
//
// It walks up to three sources in turn ("phases"): the plain recent-members
// listing; optionally the same listing filtered by search queries, which
// gets past the listing's own depth limit; optionally the recent message
// history, which still works when admins hide the member list. A pause
// between requests and a real FLOOD_WAIT wait keep it from being throttled,
// and whatever was gathered before any stop is still saved.
// `done` gets everything collected plus why it stopped early ("" = ran to
// the end) - a lone scrape saves it as a base (see ScrapeGroupMembers), a
// batch over a groups base merges it with the other groups' members.
void ScrapeChannelMembers(
		not_null<Window::SessionController*> controller,
		not_null<ChannelData*> channel,
		const ScrapeOptions &options,
		Fn<void(QStringList collected, QString reason)> done) {
	struct Phase {
		enum class Kind { Recent, History };
		Kind kind = Kind::Recent;
	};
	auto phases = std::vector<Phase>{ { Phase::Kind::Recent } };
	if (options.messageAuthors || options.joinLeaveEvents) {
		phases.push_back({ Phase::Kind::History });
	}

	const auto api = static_cast<MTP::Sender*>(&channel->session().api());
	const auto collected = std::make_shared<QStringList>();
	const auto seen = std::make_shared<base::flat_set<uint64>>();
	const auto now = base::unixtime::now();
	// A megagroup can be huge - cap the scrape so a mistaken click on a
	// 200k-member channel can't wedge the app in an endless fetch loop.
	constexpr auto kHardCap = 50'000;
	constexpr auto kPerPage = 200;
	// Telegram serves a listing only this deep per query.
	constexpr auto kPhaseDepth = 10'000;
	constexpr auto kHistoryPage = 100;
	constexpr auto kMaxFloodWaitSeconds = 300;
	constexpr auto kMaxFloodRetries = 5;

	struct State {
		base::weak_qptr<Ui::BoxContent> box;
		QPointer<Ui::FlatLabel> summary;
		QPointer<Ui::VerticalLayout> log;
		QStringList logLines; // kept so a reopened box shows the history
		std::shared_ptr<bool> cancelled = std::make_shared<bool>(false);
		std::shared_ptr<Core::ProcessHandle> process;
		int page = 0;
		int phase = 0;
		int offset = 0; // participants offset, or history messages fetched
		int historyOffsetId = 0;
		int historyPage = 0;
		int floodRetries = 0;
		bool finished = false;
	};
	const auto state = std::make_shared<State>();
	// The live progress box can be closed (the scrape keeps running in the
	// background, listed in "Процессы") and reopened from there - this
	// points the process's "reopen" at the show lambda defined once `state`
	// is ready.
	const auto reopenHolder = std::make_shared<Fn<void()>>();
	state->process = Core::App().processes().start(
		u"Сбор участников: "_q + channel->name(),
		[reopenHolder] { if (*reopenHolder) (*reopenHolder)(); },
		Core::ProcessKind::Scrape);

	const auto refreshSummary = [=] {
		if (const auto label = state->summary.data()) {
			label->setText(u"Страниц обработано: "_q
				+ QString::number(state->page)
				+ u". Найдено с юзернеймом: "_q
				+ QString::number(collected->size()) + '.');
		}
		state->process->progress(
			int(collected->size()),
			std::max(channel->membersCount(), int(collected->size())));
	};
	const auto appendLogLine = [=](const QString &text) {
		state->logLines.push_back(text);
		if (const auto log = state->log.data()) {
			log->add(object_ptr<Ui::FlatLabel>(log, text, st::boxLabel));
		}
		state->process->log(text);
	};
	// `reason` empty = ran to the end; otherwise why it stopped early. What
	// was collected is saved either way.
	const auto finish = [=](const QString &reason) {
		if (std::exchange(state->finished, true)) {
			return;
		}
		*state->cancelled = true;
		if (const auto box = state->box.get()) {
			box->closeBox();
		}
		const auto summary = u"Собрано: "_q + QString::number(collected->size())
			+ (reason.isEmpty() ? QString() : (u". Остановлено: "_q + reason));
		if (collected->isEmpty() && !reason.isEmpty()) {
			state->process->fail(summary);
		} else {
			state->process->succeed(summary);
		}
		done(std::move(*collected), reason);
	};

	// Builds (or rebuilds, on reopen from "Процессы") the live progress box,
	// repainting whatever log has accumulated so far. Closing it with the
	// cross or Esc only hides it - the scrape goes on until it finishes or
	// the "Остановить" button is pressed.
	const auto showScrapeBox = [=] {
		if (state->box.get()) {
			return; // already open
		}
		const auto shownBox = controller->show(
			Box([=](not_null<Ui::GenericBox*> box) {
				box->setWidth(st::boxWideWidth);
				box->setTitle(
					rpl::single(u"Сбор участников: "_q + channel->name()));
				state->summary = box->addRow(object_ptr<Ui::FlatLabel>(
					box, QString(), st::boxLabel));
				refreshSummary();
				Ui::AddSkip(box->verticalLayout());
				const auto log = box->addRow(
					object_ptr<Ui::VerticalLayout>(box));
				state->log = log;
				for (const auto &line : state->logLines) {
					log->add(object_ptr<Ui::FlatLabel>(
						log, line, st::boxLabel));
				}
				if (!state->finished) {
					box->addButton(
						rpl::single(u"Остановить и сохранить"_q),
						[=] { finish(kScrapeStoppedManually); });
				}
				// "Скрыть"/the cross/Esc just hide it; the scrape keeps
				// running.
				Ggtg::AddHideButton(box);
				Ggtg::AddCloseCross(box);
			}));
		state->box = shownBox;
	};
	*reopenHolder = showScrapeBox;
	showScrapeBox();

	const auto loadNext = std::make_shared<Fn<void()>>();
	const auto pauseThen = [=](crl::time delay) {
		base::call_delayed(delay, &channel->session(), [=] {
			if (!*state->cancelled) {
				(*loadNext)();
			}
		});
	};
	// A short random gap between two requests to the same source.
	const auto pageGap = [] {
		return crl::time(600 + base::RandomIndex(700));
	};
	const auto nextPhase = [=] {
		++state->phase;
		state->offset = 0;
		state->historyOffsetId = 0;
		state->historyPage = 0;
	};
	// After a page: stop at the cap, move on when the source is exhausted,
	// otherwise keep paging it.
	const auto afterPage = [=](bool phaseDone) {
		++state->page;
		refreshSummary();
		if (int(collected->size()) >= kHardCap) {
			finish(u"достигнут предел "_q + QString::number(kHardCap));
			return;
		}
		if (phaseDone) {
			nextPhase();
		}
		pauseThen(pageGap());
	};
	const auto onError = [=](const MTP::Error &error) {
		if (*state->cancelled) {
			return;
		}
		const auto type = error.type();
		const auto wait = FloodWaitSeconds(type);
		if (wait > 0) {
			if (wait > kMaxFloodWaitSeconds
				|| ++state->floodRetries > kMaxFloodRetries) {
				appendLogLine(u"FLOOD_WAIT "_q + QString::number(wait)
					+ u" сек - слишком долго, останавливаемся."_q);
				finish(u"Telegram просит подождать "_q
					+ QString::number(wait) + u" сек (FLOOD_WAIT)"_q);
				return;
			}
			appendLogLine(u"FLOOD_WAIT: ждём "_q + QString::number(wait)
				+ u" сек и продолжаем..."_q);
			pauseThen(crl::time(wait + 1) * 1000);
			return;
		}
		const auto isHistory = (state->phase < int(phases.size()))
			&& (phases[state->phase].kind == Phase::Kind::History);
		if (!isHistory && (type == u"CHAT_ADMIN_REQUIRED"_q)) {
			// The member list is hidden. Skip to the message-history phase
			// if it was asked for.
			appendLogLine(
				u"Список участников скрыт администраторами."_q);
			while (state->phase < int(phases.size())
				&& phases[state->phase].kind != Phase::Kind::History) {
				++state->phase;
			}
			state->offset = 0;
			state->historyOffsetId = 0;
			state->historyPage = 0;
			if (state->phase >= int(phases.size())) {
				finish(u"список участников скрыт (включите сбор авторов или вступивших в настройках)"_q);
			} else {
				pauseThen(pageGap());
			}
			return;
		}
		appendLogLine(u"Ошибка: "_q + type);
		finish(type);
	};

	*loadNext = [=] {
		if (*state->cancelled) {
			return;
		}
		if (state->phase >= int(phases.size())) {
			finish(QString());
			return;
		}
		const auto &phase = phases[state->phase];
		if (phase.kind == Phase::Kind::History) {
			const auto maxHistoryPages = std::max(
				options.messageAuthors ? options.messageAuthorsPages : 0,
				options.joinLeaveEvents ? options.joinLeaveEventsPages : 0);
			if (state->historyPage >= maxHistoryPages) {
				afterPage(true);
				return;
			}
			api->request(MTPmessages_GetHistory(
				channel->input(),
				MTP_int(state->historyOffsetId),
				MTP_int(0), // offset_date
				MTP_int(0), // add_offset
				MTP_int(kHistoryPage),
				MTP_int(0), // max_id
				MTP_int(0), // min_id
				MTP_long(0) // hash
			)).done([=](const MTPmessages_Messages &result) {
				if (*state->cancelled) {
					return;
				}
				++state->historyPage;
				auto messagesCount = 0;
				auto lowestId = 0;
				auto added = 0;
				result.match([&](const MTPDmessages_messagesNotModified &) {
				}, [&](const auto &data) {
					channel->owner().processUsers(data.vusers());

					// Update lastseen from raw status if available
					for (const auto &mtpUser : data.vusers().v) {
						mtpUser.match([&](const MTPDuser &u) {
							if (const auto status = u.vstatus()) {
								if (const auto user = channel->owner().userLoaded(UserId(u.vid().v))) {
									user->updateLastseen(LastseenFromMTP(*status, user->lastseen()));
								}
							}
						}, [](const auto &) {});
					}

					const auto allowAuthors = options.messageAuthors
						&& (state->historyPage <= options.messageAuthorsPages);
					const auto allowEvents = options.joinLeaveEvents
						&& (state->historyPage <= options.joinLeaveEventsPages);

					auto candidateUids = std::vector<uint64>();

					for (const auto &message : data.vmessages().v) {
						const auto id = int(IdFromMessage(message).bare);
						lowestId = lowestId ? std::min(lowestId, id) : id;
						++messagesCount;

						message.match([&](const MTPDmessage &msg) {
							if (allowAuthors) {
								if (const auto from = msg.vfrom_id()) {
									const auto peer = peerFromMTP(*from);
									if (peerIsUser(peer)) {
										candidateUids.push_back(peerToUser(peer).bare);
									}
								}
							}
						}, [&](const MTPDmessageService &msg) {
							if (allowEvents) {
								msg.vaction().match([&](const MTPDmessageActionChatAddUser &act) {
									for (const auto &uid : act.vusers().v) {
										candidateUids.push_back(UserId(uid.v).bare);
									}
									if (const auto from = msg.vfrom_id()) {
										const auto peer = peerFromMTP(*from);
										if (peerIsUser(peer)) {
											candidateUids.push_back(peerToUser(peer).bare);
										}
									}
								}, [&](const MTPDmessageActionChatJoinedByLink &act) {
									if (const auto from = msg.vfrom_id()) {
										const auto peer = peerFromMTP(*from);
										if (peerIsUser(peer)) {
											candidateUids.push_back(peerToUser(peer).bare);
										}
									}
								}, [&](const MTPDmessageActionChatJoinedByRequest &act) {
									if (const auto from = msg.vfrom_id()) {
										const auto peer = peerFromMTP(*from);
										if (peerIsUser(peer)) {
											candidateUids.push_back(peerToUser(peer).bare);
										}
									}
								}, [&](const MTPDmessageActionChatDeleteUser &act) {
									candidateUids.push_back(UserId(act.vuser_id().v).bare);
								}, [](const auto &) {
								});
							}
						}, [](const MTPDmessageEmpty &) {
						});
					}

					for (const auto uid : candidateUids) {
						if (!seen->emplace(uid).second) {
							continue;
						}
						const auto user = channel->owner().userLoaded(UserId(uid));
						if (user && UserAllowed(user, options, now)) {
							collected->push_back(user->username());
							RememberPeerName(user);
							++added;
						}
					}
				});

				state->offset += messagesCount;
				state->historyOffsetId = lowestId;
				appendLogLine(
					u"История (стр. "_q + QString::number(state->historyPage)
						+ u"/" + QString::number(maxHistoryPages)
						+ u"): +" + QString::number(added)
						+ u" (сообщений "_q + QString::number(state->offset)
						+ u", всего участников "_q + QString::number(collected->size())
						+ ')');

				const auto phaseDone = !messagesCount
					|| !lowestId
					|| (state->historyPage >= maxHistoryPages);
				afterPage(phaseDone);
			}).fail(onError).send();
			return;
		}
		api->request(MTPchannels_GetParticipants(
			channel->inputChannel(),
			MTP_channelParticipantsRecent(),
			MTP_int(state->offset),
			MTP_int(kPerPage),
			MTP_long(0)
		)).done([=](const MTPchannels_ChannelParticipants &result) {
			if (*state->cancelled) {
				return;
			}
			auto phaseDone = false;
			auto addedThisPage = 0;
			result.match([&](const MTPDchannels_channelParticipants &data) {
				const auto parsed = Api::ChatParticipants::Parse(
					channel,
					data);
				for (const auto &participant : parsed.list) {
					if (options.skipAdmins && participant.isCreatorOrAdmin()) {
						continue;
					}
					const auto userId = participant.userId();
					if (!userId || !seen->emplace(userId.bare).second) {
						continue;
					}
					const auto user = channel->owner().userLoaded(userId);
					if (user && UserAllowed(user, options, now)) {
						collected->push_back(user->username());
						RememberPeerName(user);
						++addedThisPage;
					}
				}
				state->offset += int(parsed.list.size());
				phaseDone = parsed.list.empty()
					|| (state->offset >= kPhaseDepth);
			}, [&](const MTPDchannels_channelParticipantsNotModified &) {
				phaseDone = true;
			});
			appendLogLine(
				u"Участники: +"_q + QString::number(addedThisPage)
					+ u" (всего "_q + QString::number(collected->size())
					+ ')');
			afterPage(phaseDone);
		}).fail(onError).send();
	};
	(*loadNext)();
}

} // namespace

std::vector<ScrapedGroupMeta> LoadScrapedGroupsIndex() {
	if (const auto &cached = IndexCache()) {
		return *cached;
	}
	auto result = std::vector<ScrapedGroupMeta>();
	const auto document = ReadJson(IndexPath());
	for (const auto &value : document.array()) {
		const auto object = value.toObject();
		result.push_back({
			.id = object["id"].toString().toULongLong(),
			.title = object["title"].toString(),
			.count = object["count"].toInt(),
			.savedAt = object["savedAt"].toString().toLongLong(),
			// Absent in every base saved before this field existed -
			// BaseKindFromString defaults those to Users, which is what
			// they always were.
			.kind = BaseKindFromString(object["kind"].toString()),
		});
	}
	IndexCache() = result;
	return result;
}

ParsedUsernames ParseUsernames(const QStringList &lines) {
	static const auto separators = QRegularExpression(u"[\\s,;]+"_q);
	static const auto linkRe = QRegularExpression(
		u"^(?:https?://)?(?:www\\.)?(?:t|telegram)\\.me/(.+)$"_q,
		QRegularExpression::CaseInsensitiveOption);
	static const auto validRe = QRegularExpression(
		u"^[A-Za-z][A-Za-z0-9_]{3,31}$"_q);
	static const auto reservedPaths = {
		u"joinchat"_q, u"addstickers"_q, u"addemoji"_q, u"addlist"_q,
		u"c"_q, u"s"_q, u"proxy"_q, u"socks"_q, u"share"_q, u"boost"_q,
		u"invoice"_q, u"login"_q, u"setlanguage"_q, u"bg"_q,
	};

	auto result = ParsedUsernames();
	auto seen = base::flat_set<QString>();
	for (const auto &line : lines) {
		for (const auto &token : line.split(separators, Qt::SkipEmptyParts)) {
			auto name = token;
			if (const auto link = linkRe.match(name); link.hasMatch()) {
				// t.me/name, t.me/name/123, t.me/name?start=x - the first
				// path segment is the username, unless it's one of the
				// non-username kinds (invite, sticker set, ...).
				const auto path = link.captured(1);
				const auto cut = path.indexOf(QRegularExpression(u"[/?#]"_q));
				const auto first = (cut < 0) ? path : path.left(cut);
				if (ranges::contains(reservedPaths, first.toLower())
					|| first.startsWith('+')) {
					result.rejected.push_back(token);
					continue;
				}
				name = first;
			}
			if (name.startsWith('@')) {
				name.remove(0, 1);
			}
			if (!validRe.match(name).hasMatch()) {
				result.rejected.push_back(token);
				continue;
			}
			if (!seen.emplace(name.toLower()).second) {
				++result.duplicates;
				continue;
			}
			result.valid.push_back(name);
		}
	}
	return result;
}

QStringList LoadScrapedGroupUsernames(uint64 id) {
	auto &cache = UsernamesCache();
	if (const auto it = cache.find(id); it != cache.end()) {
		return it->second;
	}
	auto result = QStringList();
	const auto document = ReadJson(GroupPath(id));
	for (const auto &value : document.array()) {
		const auto username = value.toString();
		if (!username.isEmpty()) {
			result.push_back(username);
		}
	}
	cache[id] = result;
	return result;
}

// Lower-cased usernames held by every base of `kind` other than `exceptId`.
[[nodiscard]] base::flat_set<QString> NamesInOtherBases(
		uint64 exceptId,
		BaseKind kind) {
	auto result = base::flat_set<QString>();
	for (const auto &meta : LoadScrapedGroupsIndex()) {
		if (meta.id == exceptId || meta.kind != kind) {
			continue;
		}
		if (kind == BaseKind::Users) {
			for (const auto &name : LoadScrapedGroupUsernames(meta.id)) {
				result.emplace(name.toLower());
			}
		} else {
			for (const auto &entry : LoadScrapedGroupEntries(meta.id)) {
				if (!entry.username.isEmpty()) {
					result.emplace(entry.username.toLower());
				}
			}
		}
	}
	return result;
}

void WriteUsernamesBase(
		uint64 id,
		const QString &title,
		const QStringList &normalized) {
	auto array = QJsonArray();
	for (const auto &username : normalized) {
		array.append(username);
	}
	WriteJson(GroupPath(id), QJsonDocument(array));
	UsernamesCache()[id] = normalized;
	++StatsGeneration();
	AppendToIndex(id, title, int(normalized.size()), BaseKind::Users);
}

void WriteEntriesBase(
		uint64 id,
		const QString &title,
		const std::vector<ScrapedGroupEntry> &normalized) {
	auto array = QJsonArray();
	for (const auto &entry : normalized) {
		array.append(QJsonObject{
			{ "username", entry.username },
			{ "title", entry.title },
			{ "membersCount", entry.membersCount },
			{ "link", entry.link },
			{ "checked", entry.checked },
		});
	}
	WriteJson(GroupPath(id), QJsonDocument(array));
	UsernamesCache().erase(id);
	++StatsGeneration();
	AppendToIndex(id, title, int(normalized.size()), BaseKind::Groups);
}

void SaveScrapedGroupUsernames(
		uint64 id,
		const QString &title,
		const QStringList &usernames) {
	auto seen = base::flat_set<QString>();
	auto normalized = QStringList();
	for (const auto &raw : usernames) {
		auto username = raw.trimmed();
		if (username.startsWith('@')) {
			username.remove(0, 1);
		}
		if (username.isEmpty()) {
			continue;
		}
		if (!seen.emplace(username.toLower()).second) {
			continue;
		}
		normalized.push_back(username);
	}

	// A person lives in one base only: names another base already holds
	// are left out (see also RemoveDuplicatesAcrossBases for old data).
	const auto others = NamesInOtherBases(id, BaseKind::Users);
	const auto before = normalized.size();
	normalized.erase(
		std::remove_if(normalized.begin(), normalized.end(), [&](
				const QString &name) {
			return others.contains(name.toLower());
		}),
		normalized.end());
	if (const auto dropped = before - normalized.size()) {
		Core::GgtgLog::Info(
			u"Базы"_q,
			u"Не добавлено, уже есть в другой базе: "_q
				+ QString::number(dropped));
	}
	WriteUsernamesBase(id, title, normalized);
}

std::vector<ScrapedGroupEntry> LoadScrapedGroupEntries(uint64 id) {
	auto result = std::vector<ScrapedGroupEntry>();
	const auto document = ReadJson(GroupPath(id));
	for (const auto &value : document.array()) {
		const auto object = value.toObject();
		const auto username = object["username"].toString();
		const auto title = object["title"].toString();
		const auto link = object["link"].toString();
		if (username.isEmpty() && title.isEmpty() && link.isEmpty()) {
			continue;
		}
		result.push_back({
			.username = username,
			.title = title,
			.membersCount = object["membersCount"].toInt(),
			.link = link,
			.checked = object["checked"].toBool(),
		});
	}
	return result;
}

void SaveScrapedGroupEntries(
		uint64 id,
		const QString &title,
		const std::vector<ScrapedGroupEntry> &entries) {
	auto seen = base::flat_set<QString>();
	auto normalized = std::vector<ScrapedGroupEntry>();
	for (const auto &raw : entries) {
		auto entry = raw;
		if (entry.username.startsWith('@')) {
			entry.username.remove(0, 1);
		}
		if (entry.username.isEmpty()
			&& entry.title.isEmpty()
			&& entry.link.isEmpty()) {
			continue;
		}
		if (!entry.username.isEmpty()
			&& !seen.emplace(entry.username.toLower()).second) {
			continue;
		}
		if (entry.username.isEmpty()
			&& !entry.link.isEmpty()
			&& !seen.emplace(entry.link.toLower()).second) {
			continue;
		}
		normalized.push_back(std::move(entry));
	}

	const auto others = NamesInOtherBases(id, BaseKind::Groups);
	const auto before = normalized.size();
	normalized.erase(
		std::remove_if(normalized.begin(), normalized.end(), [&](
				const ScrapedGroupEntry &entry) {
			return !entry.username.isEmpty()
				&& others.contains(entry.username.toLower());
		}),
		normalized.end());
	if (const auto dropped = before - normalized.size()) {
		Core::GgtgLog::Info(
			u"Базы"_q,
			u"Групп не добавлено, уже есть в другой базе: "_q
				+ QString::number(dropped));
	}
	WriteEntriesBase(id, title, normalized);
}

uint64 GenerateManualBaseId() {
	// Real Telegram peer ids never set the top bit, so this can never
	// collide with a scraped group's id.
	return base::RandomValue<uint64>() | (uint64(1) << 63);
}

int RemoveDuplicatesAcrossBases() {
	// The oldest base (index order) keeps a name; later ones lose it.
	auto seenUsers = base::flat_set<QString>();
	auto seenGroups = base::flat_set<QString>();
	auto removed = 0;
	for (const auto &meta : LoadScrapedGroupsIndex()) {
		if (meta.kind == BaseKind::Users) {
			auto list = LoadScrapedGroupUsernames(meta.id);
			const auto before = list.size();
			list.erase(
				std::remove_if(list.begin(), list.end(), [&](
						const QString &name) {
					return !seenUsers.emplace(name.toLower()).second;
				}),
				list.end());
			if (list.size() != before) {
				removed += before - list.size();
				WriteUsernamesBase(meta.id, meta.title, list);
			}
		} else {
			auto entries = LoadScrapedGroupEntries(meta.id);
			const auto before = entries.size();
			entries.erase(
				std::remove_if(entries.begin(), entries.end(), [&](
						const ScrapedGroupEntry &entry) {
					return !entry.username.isEmpty()
						&& !seenGroups.emplace(
							entry.username.toLower()).second;
				}),
				entries.end());
			if (entries.size() != before) {
				removed += int(before - entries.size());
				WriteEntriesBase(meta.id, meta.title, entries);
			}
		}
	}
	if (removed) {
		Core::GgtgLog::Info(
			u"Базы"_q,
			u"Убраны повторы между базами: "_q + QString::number(removed));
	}
	return removed;
}

void PruneSentFromBase(uint64 baseId, const QString &username) {
	const auto index = LoadScrapedGroupsIndex();
	const auto meta = ranges::find(index, baseId, &ScrapedGroupMeta::id);
	if (meta == end(index) || meta->kind != BaseKind::Users) {
		return;
	}
	(void)LoadScrapedGroupUsernames(baseId);
	auto &list = UsernamesCache()[baseId];
	const auto it = ranges::find_if(list, [&](const QString &name) {
		return NormalizeUsername(name) == username;
	});
	if (it == list.end()) {
		return;
	}
	list.erase(it);
	++StatsGeneration();
	DirtyPrunedBases().emplace(baseId);

	static auto timer = base::Timer([] { FlushBasePrunes(); });
	if (!timer.isActive()) {
		timer.callOnce(1500);
	}
}

void PruneMemberFromAllBases(const QString &username) {
	const auto normalized = NormalizeUsername(username);
	if (normalized.isEmpty()) {
		return;
	}
	for (const auto &meta : LoadScrapedGroupsIndex()) {
		if (meta.kind == BaseKind::Users) {
			PruneSentFromBase(meta.id, normalized);
		}
	}
}

void FlushBasePrunes() {
	auto dirty = std::exchange(DirtyPrunedBases(), {});
	if (dirty.empty()) {
		return;
	}
	auto index = LoadScrapedGroupsIndex();
	auto changed = false;
	for (const auto id : dirty) {
		const auto meta = ranges::find(index, id, &ScrapedGroupMeta::id);
		const auto cached = UsernamesCache().find(id);
		// Deleted (or turned into something else) since the prune.
		if (meta == end(index)
			|| meta->kind != BaseKind::Users
			|| cached == UsernamesCache().end()) {
			continue;
		}
		auto array = QJsonArray();
		for (const auto &username : cached->second) {
			array.append(username);
		}
		WriteJson(GroupPath(id), QJsonDocument(array));
		meta->count = int(cached->second.size());
		changed = true;
	}
	if (changed) {
		WriteIndex(index);
	}
}

void RemoveBaseNow(uint64 id) {
	DirtyPrunedBases().erase(id);
	QFile::remove(GroupPath(id));
	UsernamesCache().erase(id);
	++StatsGeneration();
	auto index = LoadScrapedGroupsIndex();
	index.erase(ranges::remove(index, id, &ScrapedGroupMeta::id), index.end());
	WriteIndex(index);
	Core::GgtgLog::Info(u"Базы"_q, u"Удалена база "_q + QString::number(id));
}

void DeleteScrapedGroup(uint64 id) {
	NoteDeletedKey(u"base:"_q + QString::number(id));
	RemoveBaseNow(id);
}

std::optional<SmsSendRecord> FindSmsSendRecord(const QString &username) {
	const auto normalized = NormalizeUsername(username);
	if (normalized.isEmpty()) {
		return std::nullopt;
	}
	const auto &records = SmsSentCache();
	const auto it = records.find(normalized);
	if (it == records.end()) {
		return std::nullopt;
	}
	return it->second;
}

void MarkSmsStatus(
		const QString &username,
		SmsSendStatus status,
		const QString &account,
		uint64 baseId) {
	SetSmsSendRecord(username, status, account, baseId);
}

void MarkSmsSent(
		const QString &username,
		const QString &account,
		uint64 baseId) {
	SetSmsSendRecord(username, SmsSendStatus::Sent, account, baseId);
}

void MarkSmsSpamAcc(
		const QString &username,
		const QString &account,
		uint64 baseId) {
	SetSmsSendRecord(username, SmsSendStatus::SpamAcc, account, baseId);
}

int CountSmsResettable() {
	auto count = 0;
	for (const auto &[username, record] : SmsSentCache()) {
		if (record.status != SmsSendStatus::Sent) {
			++count;
		}
	}
	return count;
}

QString SmsSourceOf(const QString &username) {
	const auto record = FindSmsSendRecord(username);
	if (!record) {
		return QString();
	}
	if (!record->source.isEmpty()) {
		return record->source;
	}
	if (record->baseId) {
		for (const auto &meta : LoadScrapedGroupsIndex()) {
			if (meta.id == record->baseId) {
				return meta.title;
			}
		}
	}
	return QString();
}

void SetSmsNote(const QString &username, const QString &note) {
	const auto normalized = NormalizeUsername(username);
	auto &cache = SmsSentCache();
	const auto i = cache.find(normalized);
	if (i == cache.end() || i->second.note == note) {
		return;
	}
	i->second.note = note;
	SaveSmsSentCache();
}

std::vector<SmsSendStatus> SmsMarkKinds() {
	return {
		SmsSendStatus::PeerLimit,
		SmsSendStatus::SpamAcc,
		SmsSendStatus::NotFound,
		SmsSendStatus::Restricted,
		SmsSendStatus::AccountFail,
		SmsSendStatus::SuspectedFake,
		SmsSendStatus::LongAgo,
	};
}

QString SmsMarkTitle(SmsSendStatus status) {
	switch (status) {
	case SmsSendStatus::PeerLimit: return u"Telegram не дал написать"_q;
	case SmsSendStatus::SpamAcc: return u"аккаунт был в спам-блоке"_q;
	case SmsSendStatus::NotFound: return u"не найден"_q;
	case SmsSendStatus::Restricted: return u"получатель закрыл ЛС"_q;
	case SmsSendStatus::AccountFail: return u"аккаунт не смог написать"_q;
	case SmsSendStatus::SuspectedFake: return u"предположительный фейк"_q;
	case SmsSendStatus::LongAgo: return u"был(а) давно"_q;
	case SmsSendStatus::Sent: break;
	}
	return u"OK"_q;
}

int CountSmsStatus(SmsSendStatus status) {
	auto count = 0;
	for (const auto &[username, record] : SmsSentCache()) {
		if (record.status == status) {
			++count;
		}
	}
	return count;
}

int ClearSmsStatus(SmsSendStatus status) {
	const auto removed = int(std::erase_if(
		SmsSentCache(),
		[=](const auto &entry) {
			if (entry.second.status != status) {
				return false;
			}
			NoteDeletedKey(u"sent:"_q + entry.first);
			return true;
		}));
	if (removed) {
		Core::GgtgLog::Info(
			u"Реестр отправок"_q,
			u"Возвращены в очередь («"_q + SmsMarkTitle(status) + u"»): "_q
				+ QString::number(removed));
		SaveSmsSentCache();
	}
	return removed;
}

int CountSmsPeerLimit() {
	auto count = 0;
	for (const auto &[username, record] : SmsSentCache()) {
		if (record.status == SmsSendStatus::PeerLimit) {
			++count;
		}
	}
	return count;
}

int ClearSmsPeerLimit() {
	const auto removed = int(std::erase_if(
		SmsSentCache(),
		[](const auto &entry) {
			if (entry.second.status != SmsSendStatus::PeerLimit) {
				return false;
			}
			NoteDeletedKey(u"sent:"_q + entry.first);
			return true;
		}));
	if (removed) {
		Core::GgtgLog::Info(
			u"Реестр отправок"_q,
			u"Сброшены записи «не пустило»: "_q + QString::number(removed));
		SaveSmsSentCache();
	}
	return removed;
}

void ClearAllSmsResettable() {
	const auto removed = std::erase_if(SmsSentCache(), [](const auto &entry) {
		if (entry.second.status == SmsSendStatus::Sent) {
			return false;
		}
		NoteDeletedKey(u"sent:"_q + entry.first);
		return true;
	});
	if (removed) {
		Core::GgtgLog::Info(
			u"Реестр отправок"_q,
			u"Сброшены записи без OK: "_q + QString::number(removed));
		SaveSmsSentCache();
	}
}

bool IsBotUsername(const QString &username) {
	return NormalizeUsername(username).endsWith(u"bot"_q);
}

// Names in the bases are already stored normalized (see
// SaveScrapedGroupUsernames), so the counting below skips the copying
// NormalizeUsername does for each of tens of thousands of names.
[[nodiscard]] bool LooksNormalized(const QString &name) {
	if (name.isEmpty()) {
		return false;
	}
	for (const auto ch : name) {
		if (ch == '@' || ch.isSpace() || ch.isUpper()) {
			return false;
		}
	}
	return true;
}

BaseStats ComputeBaseStats(const QStringList &usernames) {
	auto result = BaseStats();
	const auto &records = SmsSentCache();
	for (const auto &username : usernames) {
		++result.total;
		const auto fast = LooksNormalized(username);
		const auto normalized = fast ? QString() : NormalizeUsername(username);
		const auto &name = fast ? username : normalized;
		if (name.endsWith(u"bot"_q)) {
			++result.bots;
			continue;
		}
		const auto it = records.find(name);
		if (it == records.end()) {
			++result.remaining;
			continue;
		}
		switch (it->second.status) {
		case SmsSendStatus::Sent: ++result.sent; break;
		case SmsSendStatus::SpamAcc: ++result.spamAcc; break;
		case SmsSendStatus::NotFound: ++result.notFound; break;
		case SmsSendStatus::Restricted: ++result.restricted; break;
		case SmsSendStatus::PeerLimit: ++result.peerLimit; break;
		case SmsSendStatus::AccountFail: ++result.accountFail; break;
		case SmsSendStatus::SuspectedFake: ++result.suspectedFake; break;
		case SmsSendStatus::LongAgo: ++result.longAgo; break;
		}
	}
	return result;
}

BaseStats ComputeBaseStats(uint64 baseId) {
	struct Cached {
		uint64 generation = 0;
		BaseStats stats;
	};
	static auto cache = std::map<uint64, Cached>();
	const auto generation = StatsGeneration();
	const auto it = cache.find(baseId);
	if (it != cache.end() && it->second.generation == generation) {
		return it->second.stats;
	}
	auto stats = ComputeBaseStats(LoadScrapedGroupUsernames(baseId));
	cache[baseId] = { generation, stats };
	return stats;
}

rpl::producer<> BasesChanged() {
	return ChangesStream().events();
}

int CountSmsSendRecords() {
	return int(SmsSentCache().size());
}

std::vector<SmsSendRecord> LoadAllSmsSendRecords() {
	auto result = std::vector<SmsSendRecord>();
	result.reserve(SmsSentCache().size());
	for (const auto &[username, record] : SmsSentCache()) {
		result.push_back(record);
	}
	ranges::sort(result, std::greater<>(), &SmsSendRecord::at);
	return result;
}

void DeleteSmsSendRecord(const QString &username) {
	const auto normalized = NormalizeUsername(username);
	if (normalized.isEmpty()) {
		return;
	}
	if (SmsSentCache().erase(normalized)) {
		NoteDeletedKey(u"sent:"_q + normalized);
		Core::GgtgLog::Info(
			u"Реестр отправок"_q,
			u"Сброшен статус @"_q + normalized);
		SaveSmsSentCache();
	}
}

void ClearAllSmsSendRecords() {
	Core::GgtgLog::Warn(
		u"Реестр отправок"_q,
		u"Очищен весь реестр ("_q + QString::number(SmsSentCache().size())
			+ u" записей)"_q);
	for (const auto &[username, record] : SmsSentCache()) {
		NoteDeletedKey(u"sent:"_q + username);
	}
	SmsSentCache().clear();
	++StatsGeneration();
	QFile::remove(SmsSentPath());
	ChangesStream().fire({});
}

std::map<QString, qint64> LoadTombstones() {
	return TombstoneCache();
}

void MergeTombstones(const std::map<QString, qint64> &other) {
	auto changed = false;
	auto &mine = TombstoneCache();
	for (const auto &[key, at] : other) {
		auto &was = mine[key];
		if (at > was) {
			was = at;
			changed = true;
		}
	}
	if (changed) {
		SaveTombstones();
	}
}

void ImportBase(
		const ScrapedGroupMeta &meta,
		const QStringList &usernames,
		const std::vector<ScrapedGroupEntry> &entries) {
	auto array = QJsonArray();
	auto count = 0;
	if (meta.kind == BaseKind::Groups) {
		for (const auto &entry : entries) {
			array.append(QJsonObject{
				{ "username", entry.username },
				{ "title", entry.title },
				{ "membersCount", entry.membersCount },
				{ "link", entry.link },
				{ "checked", entry.checked },
			});
		}
		count = int(entries.size());
	} else {
		for (const auto &username : usernames) {
			array.append(username);
		}
		count = int(usernames.size());
	}
	WriteJson(GroupPath(meta.id), QJsonDocument(array));
	UsernamesCache().erase(meta.id);
	DirtyPrunedBases().erase(meta.id);
	++StatsGeneration();
	auto index = LoadScrapedGroupsIndex();
	auto fresh = meta;
	fresh.count = count;
	const auto at = ranges::find(index, meta.id, &ScrapedGroupMeta::id);
	if (at != end(index)) {
		*at = fresh;
	} else {
		index.push_back(fresh);
	}
	WriteIndex(index);
}

void RemoveBaseForSync(uint64 id) {
	RemoveBaseNow(id);
}

void ImportSmsRecords(
		const std::vector<SmsSendRecord> &upsert,
		const std::vector<QString> &remove) {
	auto &cache = SmsSentCache();
	for (const auto &record : upsert) {
		cache[record.username] = record;
	}
	for (const auto &username : remove) {
		cache.erase(username);
	}
	if (upsert.empty() && remove.empty()) {
		return;
	}
	SaveSmsSentCache();
	SaveSmsSentCacheNow();
}

int PruneDeliveredFromBases() {
	auto removed = 0;
	for (const auto &meta : LoadScrapedGroupsIndex()) {
		if (meta.kind != BaseKind::Users) {
			continue;
		}
		const auto names = LoadScrapedGroupUsernames(meta.id);
		auto kept = QStringList();
		kept.reserve(names.size());
		for (const auto &name : names) {
			const auto record = FindSmsSendRecord(name);
			if (record && (record->status == SmsSendStatus::Sent || record->status == SmsSendStatus::LongAgo)) {
				++removed;
			} else {
				kept.push_back(name);
			}
		}
		if (kept.size() != names.size()) {
			UsernamesCache()[meta.id] = kept;
			DirtyPrunedBases().emplace(meta.id);
		}
	}
	if (removed) {
		++StatsGeneration();
		FlushBasePrunes();
	}
	return removed;
}

// The filters, remembered from the last run, before anything is fetched.
// `channelLike` adds the two extra ways through (search sweep, message
// authors) that only make sense for supergroups and channels.
void ShowScrapeOptions(
		not_null<Window::SessionController*> controller,
		const QString &name,
		bool channelLike,
		Fn<void(ScrapeOptions)> start) {
	struct State {
		QPointer<Ui::Checkbox> bots;
		QPointer<Ui::Checkbox> admins;
		QPointer<Ui::Checkbox> inactive;
		QPointer<Ui::Checkbox> authors;
		QPointer<Ui::InputField> authorsPages;
		QPointer<Ui::Checkbox> joinLeave;
		QPointer<Ui::InputField> joinLeavePages;
	};
	const auto state = std::make_shared<State>();
	const auto saved = LoadScrapeOptions();
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setWidth(st::boxWideWidth);
		box->setTitle(rpl::single(u"Сбор участников: "_q + name));

		const auto addCheckboxRow = [&](
				const QString &text,
				bool value,
				const QString &hint = QString()) {
			const auto checkbox = box->addRow(
				object_ptr<Ui::Checkbox>(box, text, value),
				style::margins(
					st::boxRowPadding.left(),
					style::ConvertScale(4),
					st::boxRowPadding.right(),
					style::ConvertScale(4)));
			checkbox->setAllowTextLines(0);
			if (!hint.isEmpty()) {
				box->addRow(
					object_ptr<Ui::FlatLabel>(
						box,
						hint,
						st::boxDividerLabel),
					style::margins(
						st::boxRowPadding.left() + style::ConvertScale(32),
						0,
						st::boxRowPadding.right(),
						style::ConvertScale(8)));
			} else {
				Ui::AddSkip(box->verticalLayout(), style::ConvertScale(4));
			}
			return checkbox;
		};

		state->bots = addCheckboxRow(u"Пропускать ботов"_q, saved.skipBots);
		state->admins = addCheckboxRow(
			u"Пропускать администраторов и создателя"_q,
			saved.skipAdmins);
		state->inactive = addCheckboxRow(
			u"Пропускать неактивных"_q,
			saved.skipInactive,
			u"Не добавлять пользователей со статусом «был(а) давно»."_q);

		if (channelLike) {
			Ui::AddSkip(box->verticalLayout(), style::ConvertScale(6));
			Ui::AddDividerText(
				box->verticalLayout(),
				rpl::single(
					u"Сбор из истории группы (1 страница = 100 сообщений)"_q));
			Ui::AddSkip(box->verticalLayout(), style::ConvertScale(6));

			state->authors = addCheckboxRow(
				u"Кто писал в чате (авторы сообщений)"_q,
				saved.messageAuthors,
				u"Сбор авторов последних сообщений из истории группы."_q);

			const auto authorsWrap = box->addRow(
				object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
					box,
					object_ptr<Ui::VerticalLayout>(box)),
				style::margins(
					st::boxRowPadding.left() + style::ConvertScale(32),
					0,
					st::boxRowPadding.right(),
					style::ConvertScale(10)));

			const auto authorsInner = authorsWrap->entity();
			state->authorsPages = authorsInner->add(
				object_ptr<Ui::InputField>(
					authorsInner,
					st::defaultInputField,
					rpl::single(u"Количество страниц истории"_q),
					QString::number(std::max(1, saved.messageAuthorsPages))));

			authorsWrap->toggleOn(state->authors->checkedChanges());
			authorsWrap->toggle(saved.messageAuthors, anim::type::instant);

			state->joinLeave = addCheckboxRow(
				u"Кто вступил и покинул группу"_q,
				saved.joinLeaveEvents,
				u"Сбор по сервисным событиям входа и выхода из группы."_q);

			const auto joinLeaveWrap = box->addRow(
				object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
					box,
					object_ptr<Ui::VerticalLayout>(box)),
				style::margins(
					st::boxRowPadding.left() + style::ConvertScale(32),
					0,
					st::boxRowPadding.right(),
					style::ConvertScale(10)));

			const auto joinLeaveInner = joinLeaveWrap->entity();
			state->joinLeavePages = joinLeaveInner->add(
				object_ptr<Ui::InputField>(
					joinLeaveInner,
					st::defaultInputField,
					rpl::single(u"Количество страниц истории"_q),
					QString::number(std::max(1, saved.joinLeaveEventsPages))));

			joinLeaveWrap->toggleOn(state->joinLeave->checkedChanges());
			joinLeaveWrap->toggle(saved.joinLeaveEvents, anim::type::instant);
		}

		box->addButton(rpl::single(u"Собрать"_q), [=] {
			const auto checked = [](const QPointer<Ui::Checkbox> &box) {
				return box && box->checked();
			};
			const auto parsePages = [](const QPointer<Ui::InputField> &field, int def) {
				if (!field) return def;
				bool ok = false;
				const auto val = field->getLastText().trimmed().toInt(&ok);
				return (ok && val > 0) ? std::min(val, 1000) : def;
			};
			const auto options = ScrapeOptions{
				.skipBots = checked(state->bots),
				.skipAdmins = checked(state->admins),
				.skipInactive = checked(state->inactive),
				.messageAuthors = checked(state->authors),
				.messageAuthorsPages = parsePages(state->authorsPages, 10),
				.joinLeaveEvents = checked(state->joinLeave),
				.joinLeaveEventsPages = parsePages(state->joinLeavePages, 10),
			};
			SaveScrapeOptions(options);
			box->closeBox();
			start(options);
		});
		box->addButton(rpl::single(u"Отмена"_q), [=] { box->closeBox(); });
	}));
}

void ScrapeGroupMembers(
		not_null<Window::SessionController*> controller,
		not_null<PeerData*> peer) {
	// Only one scrape at a time: a second attempt reopens the one already
	// running (its live box, from "Процессы") instead of starting another.
	if (const auto running = Core::App().processes().running(
			Core::ProcessKind::Scrape)) {
		if (running->reopen) {
			running->reopen();
		} else {
			controller->uiShow()->showToast(
				u"Сбор уже идёт - он в «Процессах»."_q);
		}
		return;
	}
	if (const auto chat = peer->asChat()) {
		ShowScrapeOptions(controller, chat->name(), false, [=](
				ScrapeOptions options) {
			ScrapeChatMembers(controller, chat, options);
		});
	} else if (const auto channel = peer->asChannel()) {
		ShowScrapeOptions(controller, channel->name(), true, [=](
				ScrapeOptions options) {
			ScrapeChannelMembers(controller, channel, options, [=](
					QStringList collected,
					QString reason) {
				if (!reason.isEmpty()) {
					controller->uiShow()->showToast(
						u"Сбор остановлен: "_q + reason);
				}
				FinishScrape(
					controller,
					channel->id.value,
					GroupBaseTitle(channel->name(), channel->username()),
					std::move(collected));
			});
		});
	} else {
		controller->uiShow()->showToast(u"Не поддерживается для этого чата"_q);
	}
}

void ScrapeGroupByUsername(
		not_null<Window::SessionController*> controller,
		const QString &username) {
	const auto session = &controller->session();
	const auto bare = username.trimmed().remove('@');
	if (bare.isEmpty()) {
		return;
	}
	if (const auto peer = session->data().peerByUsername(bare)) {
		ScrapeGroupMembers(controller, peer);
		return;
	}
	controller->uiShow()->showToast(u"Ищем @"_q + bare + u"..."_q);
	session->api().request(MTPcontacts_ResolveUsername(
		MTP_flags(MTPcontacts_ResolveUsername::Flags(0)),
		MTP_string(bare),
		MTP_string(QString())
	)).done([=](const MTPcontacts_ResolvedPeer &result) {
		result.match([&](const MTPDcontacts_resolvedPeer &data) {
			session->data().processUsers(data.vusers());
			session->data().processChats(data.vchats());
			const auto peerId = peerFromMTP(data.vpeer());
			const auto peer = peerId
				? session->data().peer(peerId).get()
				: nullptr;
			if (peer) {
				ScrapeGroupMembers(controller, peer);
			} else {
				controller->uiShow()->showToast(
					u"@"_q + bare + u" не найден"_q);
			}
		});
	}).fail([=](const MTP::Error &error) {
		controller->uiShow()->showToast(
			u"Не удалось найти @"_q + bare + u": "_q + error.type());
	}).send();
}

namespace {

constexpr auto kChannelsMaxFloodWait = 60;
constexpr auto kChannelsResolveTimeout = crl::time(20000);
constexpr auto kChannelsSaveEvery = 20;

[[nodiscard]] QString WaitText(int seconds) {
	const auto hours = seconds / 3600;
	const auto minutes = std::max((seconds % 3600) / 60, 1);
	return hours
		? (QString::number(hours) + u" ч "_q
			+ QString::number(minutes) + u" мин"_q)
		: (QString::number(minutes) + u" мин"_q);
}

struct ChannelsCheck {
	explicit ChannelsCheck(not_null<Main::Session*> session)
	: api(&session->mtp()) {
	}

	MTP::Sender api;
	QStringList queue; // lowercase usernames still to look up
	int next = 0;
	int total = 0;
	base::flat_set<QString> drop; // channels and people
	base::flat_set<QString> ok; // groups, or nothing found
	int unsaved = 0;
	mtpRequestId requestId = 0;
	bool finished = false;
	base::Timer timeout;
	QPointer<Ui::FlatLabel> label;
	QPointer<Ui::GenericBox> box;
	Fn<void()> changed;
};

void ApplyChannelsCheck(uint64 baseId, const std::shared_ptr<ChannelsCheck> &state) {
	if (state->drop.empty() && state->ok.empty()) {
		return;
	}
	auto title = QString();
	for (const auto &meta : LoadScrapedGroupsIndex()) {
		if (meta.id == baseId) {
			title = meta.title;
			break;
		}
	}
	auto list = LoadScrapedGroupEntries(baseId);
	auto result = std::vector<ScrapedGroupEntry>();
	result.reserve(list.size());
	for (auto &entry : list) {
		const auto key = entry.username.toLower();
		if (!key.isEmpty() && state->drop.contains(key)) {
			continue;
		}
		if (!key.isEmpty() && state->ok.contains(key)) {
			entry.checked = true;
		}
		result.push_back(std::move(entry));
	}
	SaveScrapedGroupEntries(baseId, title, result);
	state->unsaved = 0;
	if (state->changed) {
		state->changed();
	}
}

void FinishChannelsCheck(
		uint64 baseId,
		const std::shared_ptr<ChannelsCheck> &state,
		const QString &text) {
	if (state->finished) {
		return;
	}
	state->finished = true;
	state->timeout.cancel();
	if (state->requestId) {
		state->api.request(base::take(state->requestId)).cancel();
	}
	ApplyChannelsCheck(baseId, state);
	Core::GgtgLog::Info(u"Базы"_q, text);
	if (const auto label = state->label.data()) {
		label->setText(text);
	}
	if (const auto box = state->box.data()) {
		box->clearButtons();
		Ggtg::AddCloseCross(box);
	}
}

void ChannelsCheckNext(
		not_null<Window::SessionController*> controller,
		uint64 baseId,
		std::shared_ptr<ChannelsCheck> state);

void ChannelsCheckSettle(
		not_null<Window::SessionController*> controller,
		uint64 baseId,
		std::shared_ptr<ChannelsCheck> state,
		const QString &key,
		PeerData *peer,
		crl::time delay) {
	if (state->finished) {
		return;
	}
	state->requestId = 0;
	state->timeout.cancel();
	const auto channel = peer ? peer->asChannel() : nullptr;
	if ((channel && !channel->isMegagroup()) || (peer && peer->isUser())) {
		state->drop.emplace(key);
	} else {
		if (peer) {
			RememberPeerName(peer);
		}
		state->ok.emplace(key);
	}
	if (++state->unsaved >= kChannelsSaveEvery) {
		ApplyChannelsCheck(baseId, state);
	}
	if (const auto label = state->label.data()) {
		label->setText(u"Проверено "_q + QString::number(state->next)
			+ u" из "_q + QString::number(state->total)
			+ u", осталось "_q + QString::number(state->total - state->next)
			+ u"\nКаналов найдено и убрано: "_q
			+ QString::number(state->drop.size()));
	}
	base::call_delayed(delay, controller, [=] {
		ChannelsCheckNext(controller, baseId, state);
	});
}

void ChannelsCheckNext(
		not_null<Window::SessionController*> controller,
		uint64 baseId,
		std::shared_ptr<ChannelsCheck> state) {
	if (state->finished) {
		return;
	}
	if (state->next >= state->total) {
		FinishChannelsCheck(baseId, state, u"✓ Готово: проверено "_q
			+ QString::number(state->total) + u", каналов убрано: "_q
			+ QString::number(state->drop.size()) + u"."_q);
		return;
	}
	const auto key = state->queue[state->next++];
	const auto session = &controller->session();
	if (const auto peer = session->data().peerByUsername(key)) {
		ChannelsCheckSettle(controller, baseId, state, key, peer, 0);
		return;
	}
	const auto stopLimited = [=](const QString &why) {
		--state->next; // this one is still unchecked
		FinishChannelsCheck(baseId, state, why
			+ u"\nПроверено "_q + QString::number(state->next)
			+ u" из "_q + QString::number(state->total)
			+ u", каналов убрано: "_q + QString::number(state->drop.size())
			+ u".\nНажмите «Убрать каналы» позже — проверка продолжится "
				"с того же места."_q);
	};
	state->timeout.setCallback([=] {
		stopLimited(u"Telegram не отвечает на проверку."_q);
	});
	state->timeout.callOnce(kChannelsResolveTimeout);
	state->requestId = state->api.request(MTPcontacts_ResolveUsername(
		MTP_flags(MTPcontacts_ResolveUsername::Flags(0)),
		MTP_string(key),
		MTP_string(QString())
	)).done([=](const MTPcontacts_ResolvedPeer &result) {
		auto peer = (PeerData*)nullptr;
		result.match([&](const MTPDcontacts_resolvedPeer &data) {
			session->data().processUsers(data.vusers());
			session->data().processChats(data.vchats());
			const auto peerId = peerFromMTP(data.vpeer());
			peer = peerId ? session->data().peer(peerId).get() : nullptr;
		});
		ChannelsCheckSettle(controller, baseId, state, key, peer, 400);
	}).fail([=](const MTP::Error &error) {
		if (state->finished) {
			return;
		}
		state->requestId = 0;
		state->timeout.cancel();
		if (!MTP::IsFloodError(error)) {
			ChannelsCheckSettle(controller, baseId, state, key, nullptr, 800);
			return;
		}
		const auto seconds = error.type().section(u'_', -1).toInt();
		if (seconds > 0 && seconds <= kChannelsMaxFloodWait) {
			if (const auto label = state->label.data()) {
				label->setText(u"Telegram попросил паузу — ждём "_q
					+ QString::number(seconds) + u" с…"_q);
			}
			--state->next;
			base::call_delayed(
				seconds * crl::time(1000) + 500,
				controller,
				[=] { ChannelsCheckNext(controller, baseId, state); });
			return;
		}
		stopLimited(u"Telegram временно ограничил проверку"_q
			+ (seconds > 0
				? (u" (примерно на "_q + WaitText(seconds) + u")"_q)
				: QString())
			+ u"."_q);
	}).handleFloodErrors().send();
}

} // namespace

void RemoveChannelsFromGroupsBase(
		not_null<Window::SessionController*> controller,
		std::shared_ptr<Ui::Show> show,
		uint64 baseId,
		Fn<void()> changed) {
	const auto state = std::make_shared<ChannelsCheck>(&controller->session());
	state->changed = std::move(changed);
	auto seen = base::flat_set<QString>();
	for (const auto &entry : LoadScrapedGroupEntries(baseId)) {
		const auto key = entry.username.toLower();
		if (!key.isEmpty() && !entry.checked && seen.emplace(key).second) {
			state->queue.push_back(key);
		}
	}
	state->total = int(state->queue.size());
	if (!state->total) {
		show->showToast(u"Все группы этой базы уже проверены."_q);
		return;
	}
	show->showBox(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"Убрать каналы"_q));
		state->box = box.get();
		state->label = box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			u"Проверяем "_q + QString::number(state->total)
				+ u" групп…"_q,
			st::boxLabel));
		box->addButton(rpl::single(u"Остановить"_q), [=] {
			FinishChannelsCheck(baseId, state, u"Остановлено: проверено "_q
				+ QString::number(state->next - (state->requestId ? 1 : 0))
				+ u" из "_q + QString::number(state->total)
				+ u", каналов убрано: "_q
				+ QString::number(state->drop.size())
				+ u". Остальные можно проверить позже."_q);
		});
		box->boxClosing() | rpl::on_next([=] {
			FinishChannelsCheck(baseId, state, u"Проверка прервана."_q);
		}, box->lifetime());
	}), Ui::LayerOption::KeepOther);
	Core::GgtgLog::Info(
		u"Базы"_q,
		u"Убрать каналы: проверяем "_q + QString::number(state->total));
	ChannelsCheckNext(controller, baseId, state);
}

void RememberUserMeta(const QString &username, const UserMeta &meta) {
	const auto key = username.trimmed().remove('@').toLower();
	if (key.isEmpty()) {
		return;
	}
	auto &cache = UsersMetaCache();
	cache[key] = meta;
	if (!meta.name.isEmpty()) {
		RememberName(key, meta.name);
	}
	static auto timer = base::Timer([] { SaveUsersMetaNow(); });
	timer.callOnce(2000);
}

void RememberPeerName(not_null<PeerData*> peer) {
	RememberName(peer->username(), peer->name());
	if (const auto user = peer->asUser()) {
		const auto now = base::unixtime::now();
		RememberUserMeta(user->username(), UserMeta{
			.name = user->name(),
			.id = user->id.value,
			.lastseen = Data::SortByOnlineValue(user, now),
			.lastseenText = Data::OnlineText(user, now),
		});
	}
}

void RememberNameIfUnknown(const QString &username, const QString &name) {
	const auto key = username.trimmed().remove('@').toLower();
	if (!key.isEmpty() && !NamesCache().contains(key)) {
		RememberName(key, name);
	}
}

QString KnownPeerName(const QString &username) {
	const auto key = username.trimmed().remove('@').toLower();
	if (key.isEmpty()) {
		return QString();
	}
	const auto &cache = NamesCache();
	if (const auto i = cache.find(key); i != cache.end()) {
		return i->second;
	}
	if (const auto session = Core::App().activeAccount().maybeSession()) {
		if (const auto peer = session->data().peerByUsername(key)) {
			RememberPeerName(peer);
			return peer->name();
		}
	}
	return QString();
}

UserMeta LookupUserMeta(const QString &username) {
	const auto key = username.trimmed().remove('@').toLower();
	if (key.isEmpty()) {
		return {};
	}
	if (const auto session = Core::App().activeAccount().maybeSession()) {
		if (const auto peer = session->data().peerByUsername(key)) {
			RememberPeerName(peer);
			if (const auto user = peer->asUser()) {
				const auto now = base::unixtime::now();
				const auto meta = UserMeta{
					.name = peer->name(),
					.id = peer->id.value,
					.lastseen = Data::SortByOnlineValue(user, now),
					.lastseenText = Data::OnlineText(user, now),
				};
				RememberUserMeta(key, meta);
				return meta;
			}
		}
	}
	const auto &cache = UsersMetaCache();
	if (const auto i = cache.find(key); i != cache.end()) {
		return i->second;
	}
	const auto name = KnownPeerName(key);
	return UserMeta{ .name = name };
}

// ---- group search ----

namespace {

[[nodiscard]] QString NormalizeForMatch(const QString &text) {
	// NFKC folds decorative unicode lookalikes (bold/italic math letters
	// that crypto-group names love) back to plain letters, so a keyword is
	// still found in a title that only *looks* like it contains it.
	return text.normalized(QString::NormalizationForm_KC).toLower();
}

// The keyword's meaningful words (3+ characters) - a derived search or
// found group is only kept when at least one of them shows up in its
// title or username, so chasing similar groups can't drift to unrelated
// ones.
[[nodiscard]] QStringList KeywordTokens(const QString &keyword) {
	static const auto splitRe = QRegularExpression(u"[^\\p{L}\\p{N}_]+"_q);
	auto result = QStringList();
	const auto normalized = NormalizeForMatch(keyword.trimmed());
	for (const auto &token : normalized.split(splitRe, Qt::SkipEmptyParts)) {
		if (token.size() >= 3) {
			result.push_back(token);
		}
	}
	if (result.isEmpty() && !normalized.isEmpty()) {
		result.push_back(normalized);
	}
	return result;
}

[[nodiscard]] bool IsRelevantToKeyword(
		const QStringList &tokens,
		const QString &title,
		const QString &username) {
	if (tokens.isEmpty()) {
		return true;
	}
	const auto text = NormalizeForMatch(title + ' ' + username);
	return ranges::any_of(tokens, [&](const QString &token) {
		return text.contains(token);
	});
}

void RunGroupSearch(
		not_null<Window::SessionController*> controller,
		const QString &keyword,
		bool snowball,
		int minMembers) {
	const auto session = &controller->session();
	const auto tokens = KeywordTokens(keyword);
	// Ceilings, so a broad keyword can't run away: groups kept, requests
	// made, and how many derived searches broadcast channels may seed.
	constexpr auto kMaxFound = 500;
	constexpr auto kMaxSteps = 80;
	constexpr auto kChannelSeedCap = 30;
	constexpr auto kMaxFloodWaitSeconds = 300;
	constexpr auto kMaxFloodRetries = 5;
	constexpr auto kMaxErrorsInRow = 5;

	struct Found {
		QString username;
		QString title;
		int members = 0;
	};
	struct State {
		base::weak_qptr<Ui::BoxContent> box;
		QPointer<Ui::FlatLabel> summary;
		QPointer<Ui::VerticalLayout> log;
		std::shared_ptr<bool> cancelled = std::make_shared<bool>(false);
		std::shared_ptr<Core::ProcessHandle> process;
		std::map<QString, Found> found; // by lowercase username
		std::deque<QString> keywords;
		base::flat_set<QString> queried;
		std::deque<ChannelId> similar;
		int steps = 0;
		int channelSeeds = 0;
		int floodRetries = 0;
		int errorsInRow = 0;
		bool finished = false;
	};
	const auto state = std::make_shared<State>();
	state->process = Core::App().processes().start(
		u"Поиск групп: "_q + keyword);

	const auto refreshSummary = [=] {
		if (const auto label = state->summary.data()) {
			label->setText(u"Запросов: "_q + QString::number(state->steps)
				+ u". Найдено групп: "_q
				+ QString::number(state->found.size())
				+ u". В очереди: "_q
				+ QString::number(
					state->keywords.size() + state->similar.size()) + '.');
		}
		state->process->progress(state->steps, kMaxSteps);
	};
	const auto appendLogLine = [=](const QString &text) {
		if (const auto log = state->log.data()) {
			log->add(object_ptr<Ui::FlatLabel>(log, text, st::boxLabel));
		}
		state->process->log(text);
	};
	const auto finish = [=](const QString &reason) {
		if (std::exchange(state->finished, true)) {
			return;
		}
		*state->cancelled = true;
		if (const auto box = state->box.get()) {
			box->closeBox();
		}
		auto entries = std::vector<ScrapedGroupEntry>();
		for (const auto &[key, found] : state->found) {
			if (found.members > 0 && found.members < minMembers) {
				continue;
			}
			entries.push_back({
				.username = found.username,
				.title = found.title,
				.membersCount = found.members,
			});
		}
		ranges::sort(
			entries,
			std::greater<>(),
			&ScrapedGroupEntry::membersCount);
		const auto summary = u"Найдено групп: "_q
			+ QString::number(entries.size())
			+ (reason.isEmpty() ? QString() : (u". Остановлено: "_q + reason));
		if (entries.empty()) {
			state->process->fail(
				reason.isEmpty() ? u"Ничего не найдено"_q : summary);
			controller->uiShow()->showToast(
				u"Поиск: групп не найдено - база не создана"_q);
			return;
		}
		const auto title = u"Поиск: "_q + keyword + u" ("_q
			+ QDateTime::currentDateTime().toString(u"dd.MM"_q) + ')';
		SaveScrapedGroupEntries(GenerateManualBaseId(), title, entries);
		state->process->succeed(summary);
		controller->uiShow()->showToast(
			u"Сохранена база групп \""_q + title + u"\": "_q
				+ QString::number(entries.size()));
	};

	const auto shownBox = controller->show(
		Box([=](not_null<Ui::GenericBox*> box) {
			box->setWidth(st::boxWideWidth);
			box->setTitle(rpl::single(u"Поиск групп: "_q + keyword));
			state->summary = box->addRow(object_ptr<Ui::FlatLabel>(
				box, u"Запросов: 0."_q, st::boxLabel));
			Ui::AddSkip(box->verticalLayout());
			state->log = box->addRow(object_ptr<Ui::VerticalLayout>(box));
			box->addButton(rpl::single(u"Остановить и сохранить"_q), [=] {
				finish(u"остановлено вручную"_q);
			});
			// Only hides the window; the search goes on ("Процессы" shows it).
			Ggtg::AddHideButton(box);
		}));
	state->box = shownBox;

	// A derived search term: a found group's title or @username, searched
	// again to reach neighbouring groups (the "snowball").
	const auto seed = [=](const QString &term) {
		const auto trimmed = term.trimmed();
		if (trimmed.isEmpty()
			|| !state->queried.emplace(trimmed.toLower()).second) {
			return;
		}
		state->keywords.push_back(trimmed);
	};
	seed(keyword);

	// `derived`: came from a snowball step, so the keyword-relevance check
	// applies (the original search's own hits are trusted as they are).
	const auto onChannel = [=](
			not_null<ChannelData*> channel,
			bool derived) {
		const auto username = channel->username();
		if (username.isEmpty()) {
			return;
		}
		const auto title = channel->name();
		if (derived && !IsRelevantToKeyword(tokens, title, username)) {
			return;
		}
		if (!channel->isMegagroup()) {
			// A broadcast channel isn't a group - only a seed for more
			// searches, and capped so a popular keyword's flood of channels
			// can't fill the queue with derived searches.
			if (snowball && state->channelSeeds < kChannelSeedCap) {
				++state->channelSeeds;
				seed(title);
				seed('@' + username);
			}
			return;
		}
		const auto key = username.toLower();
		// An unknown size reads as 1: keep it unknown (0) instead.
		const auto members = channel->membersCountKnown()
			? channel->membersCount()
			: 0;
		const auto it = state->found.find(key);
		if (it != state->found.end()) {
			it->second.members = std::max(it->second.members, members);
			return;
		}
		state->found[key] = {
			.username = username,
			.title = title,
			.members = members,
		};
		if (snowball && int(state->found.size()) < kMaxFound) {
			state->similar.push_back(peerToChannel(channel->id));
			seed(title);
			seed('@' + username);
		}
	};

	const auto tick = std::make_shared<Fn<void()>>();
	const auto schedule = [=](crl::time delay) {
		base::call_delayed(delay, session, [=] {
			if (!*state->cancelled) {
				(*tick)();
			}
		});
	};
	const auto gap = [] {
		return crl::time(1500 + base::RandomIndex(1500));
	};
	// `requeue` puts the failed work item back so a FLOOD_WAIT retry (or the
	// next tick) doesn't silently lose it.
	const auto onError = [=](const MTP::Error &error, Fn<void()> requeue) {
		if (*state->cancelled) {
			return;
		}
		const auto type = error.type();
		const auto wait = FloodWaitSeconds(type);
		if (wait > 0) {
			if (wait > kMaxFloodWaitSeconds
				|| ++state->floodRetries > kMaxFloodRetries) {
				finish(u"Telegram просит подождать "_q
					+ QString::number(wait) + u" сек (FLOOD_WAIT)"_q);
				return;
			}
			appendLogLine(u"FLOOD_WAIT: ждём "_q + QString::number(wait)
				+ u" сек и продолжаем..."_q);
			requeue();
			schedule(crl::time(wait + 1) * 1000);
			return;
		}
		appendLogLine(u"Ошибка: "_q + type);
		if (++state->errorsInRow >= kMaxErrorsInRow) {
			finish(u"слишком много ошибок подряд ("_q + type + ')');
			return;
		}
		schedule(gap());
	};

	*tick = [=] {
		if (*state->cancelled) {
			return;
		}
		if (int(state->found.size()) >= kMaxFound
			|| state->steps >= kMaxSteps) {
			finish(QString());
			return;
		}
		if (!state->keywords.empty()) {
			const auto term = state->keywords.front();
			state->keywords.pop_front();
			++state->steps;
			session->api().request(MTPcontacts_Search(
				MTP_flags(0),
				MTP_string(term),
				MTP_int(100)
			)).done([=](const MTPcontacts_Found &result) {
				if (*state->cancelled) {
					return;
				}
				state->errorsInRow = 0;
				const auto &data = result.data();
				session->data().processUsers(data.vusers());
				session->data().processChats(data.vchats());
				const auto before = state->found.size();
				for (const auto &mtpPeer : data.vresults().v) {
					const auto peer = session->data().peer(
						peerFromMTP(mtpPeer));
					if (const auto channel = peer->asChannel()) {
						onChannel(channel, term != keyword);
					}
				}
				appendLogLine(u"Поиск \""_q + term + u"\": +"_q
					+ QString::number(state->found.size() - before)
					+ u" (всего "_q + QString::number(state->found.size())
					+ ')');
				refreshSummary();
				schedule(gap());
			}).fail([=](const MTP::Error &error) {
				onError(error, [=] {
					state->keywords.push_front(term);
					--state->steps;
				});
			}).send();
			return;
		}
		if (!state->similar.empty()) {
			const auto id = state->similar.front();
			state->similar.pop_front();
			const auto channel = session->data().channelLoaded(id);
			if (!channel) {
				schedule(100);
				return;
			}
			++state->steps;
			using Flag = MTPchannels_GetChannelRecommendations::Flag;
			session->api().request(MTPchannels_GetChannelRecommendations(
				MTP_flags(Flag::f_channel),
				channel->inputChannel()
			)).done([=](const MTPmessages_Chats &result) {
				if (*state->cancelled) {
					return;
				}
				state->errorsInRow = 0;
				const auto before = state->found.size();
				result.match([&](const auto &data) {
					session->data().processChats(data.vchats());
					for (const auto &chat : data.vchats().v) {
						chat.match([&](const MTPDchannel &d) {
							const auto similar
								= session->data().channelLoaded(
									ChannelId(d.vid()));
							if (similar) {
								onChannel(similar, true);
							}
						}, [](const auto &) {});
					}
				});
				appendLogLine(u"Похожие на @"_q + channel->username()
					+ u": +"_q
					+ QString::number(state->found.size() - before)
					+ u" (всего "_q + QString::number(state->found.size())
					+ ')');
				refreshSummary();
				schedule(gap());
			}).fail([=](const MTP::Error &error) {
				onError(error, [=] {
					state->similar.push_front(id);
					--state->steps;
				});
			}).send();
			return;
		}
		finish(QString());
	};
	refreshSummary();
	(*tick)();
}

}

void SearchGroups(not_null<Window::SessionController*> controller) {
	struct State {
		QPointer<Ui::InputField> keyword;
		QPointer<Ui::InputField> minMembers;
		QPointer<Ui::Checkbox> snowball;
	};
	const auto state = std::make_shared<State>();
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setWidth(st::boxWideWidth);
		box->setTitle(rpl::single(u"Поиск групп"_q));
		Ui::AddDividerText(
			box->verticalLayout(),
			rpl::single(
				u"Поиск идёт с текущего аккаунта ("_q
					+ controller->session().user()->name()
					+ u") по всему Telegram. Находит публичные "
						"группы (супергруппы) с юзернеймом и сохраняет их "
						"как новую базу групп, самые большие сверху."_q));
		state->keyword = box->addRow(object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			rpl::single(u"Ключевое слово"_q)));
		state->minMembers = box->addRow(object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			rpl::single(u"Минимум участников (0 - любые)"_q),
			u"0"_q));
		state->snowball = box->addRow(object_ptr<Ui::Checkbox>(
			box,
			u"Расширять поиск (похожие группы и названия найденных)"_q,
			true));
		state->snowball->setAllowTextLines(0); // wraps, never "..."
		Ui::AddDividerText(
			box->verticalLayout(),
			rpl::single(
				u"Расширение находит заметно больше связанных групп, но "
					"занимает дольше (запросы идут с паузами, не более "
					"80 штук)."_q));
		box->setFocusCallback([=] { state->keyword->setFocusFast(); });
		box->addButton(rpl::single(u"Искать"_q), [=] {
			const auto keyword = state->keyword->getLastText().trimmed();
			if (keyword.isEmpty()) {
				box->uiShow()->showToast(u"Введите ключевое слово"_q);
				return;
			}
			auto ok = false;
			const auto minMembers = std::max(
				0,
				state->minMembers->getLastText().trimmed().toInt(&ok));
			const auto snowball = state->snowball->checked();
			box->closeBox();
			RunGroupSearch(controller, keyword, snowball, ok ? minMembers : 0);
		});
		box->addButton(rpl::single(u"Отмена"_q), [=] { box->closeBox(); });
	}));
}

// ---- collecting members from every group of a groups base ----

namespace {

void ResolveChannelByUsername(
		not_null<Main::Session*> session,
		const QString &username,
		Fn<void(ChannelData *channel, QString error)> done) {
	if (const auto peer = session->data().peerByUsername(username)) {
		done(peer->asChannel(), QString());
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
			done(
				peerId ? session->data().peer(peerId)->asChannel() : nullptr,
				QString());
		});
	}).fail([=](const MTP::Error &error) {
		done(nullptr, error.type());
	}).send();
}

void CollectGroupsBatch(
		not_null<Window::SessionController*> controller,
		const QString &title,
		const QStringList &usernames,
		const ScrapeOptions &options) {
	struct State {
		QStringList merged;
		base::flat_set<QString> seen;
		int index = 0;
		bool finished = false;
		std::shared_ptr<Core::ProcessHandle> process;
	};
	const auto session = &controller->session();
	const auto state = std::make_shared<State>();
	state->process = Core::App().processes().start(
		u"Сбор из базы групп: "_q + title,
		nullptr,
		Core::ProcessKind::Scrape);
	// Gap between two groups: each one is its own long run of requests,
	// and back-to-back they'd invite a FLOOD_WAIT.
	const auto gap = [] {
		return crl::time(6000 + base::RandomIndex(6000));
	};
	const auto finish = [=](const QString &reason) {
		if (std::exchange(state->finished, true)) {
			return;
		}
		const auto summary = u"Собрано уникальных: "_q
			+ QString::number(state->merged.size())
			+ (reason.isEmpty() ? QString() : (u". Остановлено: "_q + reason));
		if (state->merged.isEmpty()) {
			state->process->fail(summary);
			controller->uiShow()->showToast(
				u"Сбор из базы групп: никого не найдено - база не создана"_q);
			return;
		}
		const auto newTitle = u"Участники: "_q + title;
		SaveScrapedGroupUsernames(
			GenerateManualBaseId(),
			newTitle,
			state->merged);
		state->process->succeed(summary);
		controller->uiShow()->showToast(
			u"Сохранена база \""_q + newTitle + u"\": "_q
				+ QString::number(state->merged.size()));
	};
	const auto next = std::make_shared<Fn<void()>>();
	const auto pauseThenNext = [=] {
		base::call_delayed(gap(), session, [=] {
			(*next)();
		});
	};
	*next = [=] {
		if (state->finished) {
			return;
		}
		if (state->index >= int(usernames.size())) {
			finish(QString());
			return;
		}
		const auto username = usernames[state->index++];
		state->process->progress(state->index - 1, int(usernames.size()));
		ResolveChannelByUsername(session, username, [=](
				ChannelData *channel,
				QString error) {
			if (!channel) {
				state->process->log(u"@"_q + username
					+ u": не найдена или это не группа/канал"_q
					+ (error.isEmpty() ? QString() : (u" ("_q + error + ')')));
				const auto wait = FloodWaitSeconds(error);
				if (wait > 0) {
					finish(u"FLOOD_WAIT "_q + QString::number(wait)
						+ u" сек при поиске группы"_q);
					return;
				}
				pauseThenNext();
				return;
			}
			ScrapeChannelMembers(controller, channel, options, [=](
					QStringList list,
					QString reason) {
				auto added = 0;
				for (const auto &member : list) {
					if (state->seen.emplace(member.toLower()).second) {
						state->merged.push_back(member);
						++added;
					}
				}
				state->process->log(u"@"_q + username + u": +"_q
					+ QString::number(added) + u" новых (всего "_q
					+ QString::number(state->merged.size()) + ')'
					+ (reason.isEmpty()
						? QString()
						: (u" - "_q + reason)));
				// Manual stop or a rate limit ends the whole batch (what was
				// gathered so far is still saved); any other per-group
				// failure just moves on to the next group.
				if (reason == kScrapeStoppedManually
					|| reason.contains(u"FLOOD_WAIT"_q)) {
					finish(reason);
					return;
				}
				pauseThenNext();
			});
		});
	};
	(*next)();
}

}

void CollectFromGroupsBase(
		not_null<Window::SessionController*> controller,
		uint64 baseId) {
	auto usernames = QStringList();
	for (const auto &entry : LoadScrapedGroupEntries(baseId)) {
		if (!entry.username.isEmpty()) {
			usernames.push_back(entry.username);
		}
	}
	if (usernames.isEmpty()) {
		controller->uiShow()->showToast(
			u"В этой базе нет групп с юзернеймом"_q);
		return;
	}
	auto title = QString();
	for (const auto &meta : LoadScrapedGroupsIndex()) {
		if (meta.id == baseId) {
			title = meta.title;
			break;
		}
	}
	ShowScrapeOptions(controller, title, true, [=](ScrapeOptions options) {
		CollectGroupsBatch(controller, title, usernames, options);
	});
}

void TrackManualSends(not_null<Main::Session*> session) {
	// An answer from someone this program wrote to is noticed the moment it
	// arrives (not only when the chat happens to be open): its first words
	// are remembered for "Рассылка → Ответы" and the result screens.
	session->data().newItemAdded(
	) | rpl::on_next([=](not_null<HistoryItem*> item) {
		if (item->out() || item->isService()) {
			return;
		}
		const auto user = item->history()->peer->asUser();
		if (!user
			|| user->isSelf()
			|| user->isBot()
			|| user->username().isEmpty()) {
			return;
		}
		const auto record = FindSmsSendRecord(user->username());
		if (!record || record->status != SmsSendStatus::Sent) {
			return;
		}
		const auto text = item->originalText().text.simplified();
		Core::GgtgAi::Feedback::RememberReply(
			Core::GgtgAi::Feedback::ReplyKey(
				session->userId().bare,
				user->username()),
			text.isEmpty() ? u"[вложение или стикер]"_q : text);
		ChangesStream().fire({});
	}, session->lifetime());
	// itemIdChanged fires when a locally created message gets its real
	// server id, i.e. once it was actually delivered - a message that
	// failed to send never gets here.
	session->data().itemIdChanged(
	) | rpl::on_next([=](const Session::IdChange &change) {
		if (!IsClientMsgId(change.oldId)) {
			return;
		}
		const auto item = session->data().message(change.newId);
		if (!item
			|| !item->out()
			|| item->isService()
			|| (item->originalText().text.isEmpty() && !item->media())) {
			return;
		}
		const auto user = item->history()->peer->asUser();
		if (!user || user->isSelf() || user->isBot()) {
			return;
		}
		// Any delivered message (text, sticker, media) to a stranger counts
		// in the account manager's "СМС" column, once per recipient per day,
		// like the campaign's own sends.
		if (!user->isContact()) {
			RegisterManualSmsSent(
				session->userId().bare,
				user->id.value);
		}
		// Delivered by hand: a person marked before with an error (could not
		// write, no such user...) is written to after all - the mark is
		// replaced by "written"; one that is "written" already stays.
		const auto previous = user->username().isEmpty()
			? std::nullopt
			: FindSmsSendRecord(user->username());
		if (user->username().isEmpty()
			|| (previous && previous->status == SmsSendStatus::Sent)) {
			return;
		}
		const auto self = session->user();
		const auto label = self->name() + u" ("_q + self->phone() + ')';
		// A message written by hand to someone from a base: the base is
		// where the contact came from.
		auto baseId = uint64(0);
		const auto wanted = NormalizeUsername(user->username());
		for (const auto &meta : LoadScrapedGroupsIndex()) {
			if (meta.kind != BaseKind::Users) {
				continue;
			}
			for (const auto &name : LoadScrapedGroupUsernames(meta.id)) {
				if (NormalizeUsername(name) == wanted) {
					baseId = meta.id;
					break;
				}
			}
			if (baseId) {
				break;
			}
		}
		MarkSmsSent(user->username(), label, baseId);
		Core::GgtgLog::Info(
			u"Реестр отправок"_q,
			u"Ручное сообщение из чата: @"_q + user->username()
				+ (previous
					? u" написан вручную, прежняя отметка заменена на OK"_q
					: u" отмечен как написанный"_q));
	}, session->lifetime());
}

void TrackManualFailure(
		not_null<PeerData*> peer,
		const QString &error) {
	const auto user = peer->asUser();
	if (!user
		|| user->isSelf()
		|| user->isBot()
		|| user->isContact()
		|| user->username().isEmpty()) {
		return;
	}
	const auto previous = FindSmsSendRecord(user->username());
	if (previous && previous->status == SmsSendStatus::Sent) {
		return;
	}
	// The same split as a campaign makes of its errors.
	static const auto restricted = QRegularExpression(
		u"^(?:USER_PRIVACY_RESTRICTED|USER_IS_BLOCKED|YOU_BLOCKED_USER|"
		"INPUT_USER_DEACTIVATED|PEER_ID_INVALID|USER_NOT_MUTUAL_CONTACT|"
		"PRIVACY_PREMIUM_REQUIRED|ALLOW_PAYMENT_REQUIRED.*)$"_q);
	static const auto accountFault = QRegularExpression(
		u"^(?:(?:FLOOD|FLOOD_PREMIUM)_WAIT_\\d+|AUTH_KEY_UNREGISTERED|"
		"SESSION_REVOKED|SESSION_EXPIRED|USER_DEACTIVATED|"
		"USER_DEACTIVATED_BAN|PHONE_NUMBER_BANNED)$"_q);
	auto status = std::optional<SmsSendStatus>();
	if (error == u"PEER_FLOOD"_q) {
		status = SmsSendStatus::PeerLimit;
	} else if (restricted.match(error).hasMatch()) {
		status = SmsSendStatus::Restricted;
	} else if (accountFault.match(error).hasMatch()) {
		status = SmsSendStatus::AccountFail;
	}
	if (!status) {
		return; // The text, the network...: nothing to say about the person.
	}
	const auto self = peer->session().user();
	const auto label = self->name() + u" ("_q + self->phone() + ')';
	MarkSmsStatus(user->username(), *status, label);
	SetSmsNote(user->username(), u"ошибка при ручной отправке: "_q + error);
}

QString GroupBaseTitle(const QString &name, const QString &username) {
	const auto bare = username.trimmed().remove('@');
	const auto trimmed = name.trimmed();
	return bare.isEmpty()
		? trimmed
		: (trimmed.isEmpty()
			? ('@' + bare)
			: (trimmed + u" (@"_q + bare + ')'));
}

bool BaseTitleMatchesGroup(
		const QString &baseTitle,
		const QString &username,
		const QString &groupTitle) {
	const auto bare = [](const QString &name) {
		return name.trimmed().remove('@').toLower();
	};
	static const auto bracket = QRegularExpression(
		u"\\(@?([A-Za-z0-9_]{4,32})\\)\\s*$"_q);
	auto name = baseTitle.trimmed();
	auto baseUsername = QString();
	if (const auto match = bracket.match(name); match.hasMatch()) {
		baseUsername = match.captured(1).toLower();
		name = name.left(match.capturedStart()).trimmed();
	} else {
		const auto parsed = ParseUsernames({ name });
		if (!parsed.valid.isEmpty()) {
			baseUsername = bare(parsed.valid.front());
		}
	}
	if (!username.isEmpty() && baseUsername == bare(username)) {
		return true;
	}
	const auto lowerName = name.toLower();
	return !lowerName.isEmpty()
		&& (lowerName == groupTitle.trimmed().toLower()
			|| (!username.isEmpty() && lowerName == bare(username)));
}

} // namespace Data
