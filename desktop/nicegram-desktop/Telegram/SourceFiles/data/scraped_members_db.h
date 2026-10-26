/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtCore/QStringList>

#include <map>
#include <optional>
#include <vector>

class PeerData;

namespace Ui {
class Show;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

namespace Main {
class Session;
} // namespace Main

namespace Data {

// Written to cWorkingDir()+"tdata/ggtg_db/" as one small JSON
// index plus one JSON file per base, entirely separate from the account's
// own encrypted local storage (see BaseDbPath in the .cpp) so a
// many-thousand-row base never bloats/slows down unrelated saves.
//
// A "base" is either a named list of usernames (people - scraped from a
// group's member list, or typed/pasted in by hand) or a named list of
// found groups/channels (see BaseKind) - "База" in the side menu shows
// them as two separate sections, "Пользователи" and "Группы". `id` is the
// source group's peer id for a scraped-members base, or a synthetic id
// (see GenerateManualBaseId) for a hand-made or group-search-result one;
// either way it's only used as the JSON file's key.

enum class BaseKind {
	Users,
	Groups,
};

struct ScrapedGroupMeta {
	uint64 id = 0;
	QString title;
	int count = 0;
	qint64 savedAt = 0; // unix seconds
	BaseKind kind = BaseKind::Users;
};

[[nodiscard]] std::vector<ScrapedGroupMeta> LoadScrapedGroupsIndex();
[[nodiscard]] QStringList LoadScrapedGroupUsernames(uint64 id);

// Creates a new base or overwrites an existing one in place (same `id`,
// new title/usernames). Usernames are normalized first: '@' stripped,
// blank lines dropped, duplicates (case-insensitive) collapsed to the
// first occurrence. Always writes a BaseKind::Users entry.
void SaveScrapedGroupUsernames(
	uint64 id,
	const QString &title,
	const QStringList &usernames);

// What a hand-typed or pasted list of usernames turned into. Every line may
// hold several separated by whitespace, commas or semicolons, each as
// "@name", "name", or a t.me / telegram.me link (extra path, "?start=…" and
// message ids are cut off). A token that isn't a valid public username -
// letter first, then letters/digits/underscore, 4-32 characters overall -
// or is an invite/sticker/etc. link goes to `rejected` as typed;
// case-insensitive repeats are counted in `duplicates` and kept once.
struct ParsedUsernames {
	QStringList valid;
	QStringList rejected;
	int duplicates = 0;
};

[[nodiscard]] ParsedUsernames ParseUsernames(const QStringList &lines);

// One group/channel entry in a BaseKind::Groups base (see
// settings/bulk_accounts_actions.cpp's ShowEditGroupsBaseBox) - `username`
// is empty for a group with no public @username (kept only for display;
// not much else can be done with it without an invite link).
struct ScrapedGroupEntry {
	QString username;
	QString title;
	int membersCount = 0;
	// A closed group has no public @username - only a link like
	// "https://t.me/c/2361171296/896316" (or an invite link). It can't be
	// opened or read directly, only looked up through Сквидвард, and is
	// shown with a 🔒 in "База". Empty for a group with a public @username.
	QString link;
	// Already looked up and known not to be a broadcast channel, so
	// "Убрать каналы" skips it (the check is limited by Telegram per day).
	bool checked = false;
};

[[nodiscard]] std::vector<ScrapedGroupEntry> LoadScrapedGroupEntries(
	uint64 id);

// Same overwrite-in-place semantics as SaveScrapedGroupUsernames, but for
// a BaseKind::Groups entry - deduplicated by username (case-insensitive;
// entries with no username are never deduplicated against each other).
void SaveScrapedGroupEntries(
	uint64 id,
	const QString &title,
	const std::vector<ScrapedGroupEntry> &entries);

// A fresh id for a hand-made base, guaranteed to never collide with a
// real Telegram peer id (see the .cpp): peer ids never set the top bit.
[[nodiscard]] uint64 GenerateManualBaseId();

// Writes to disk, right now, the removals of delivered usernames from their
// bases that MarkSmsSent has so far only applied in memory (see there).
void FlushBasePrunes();

// Removes a single member from base `baseId` (if it contains them).
void PruneSentFromBase(uint64 baseId, const QString &username);

// Removes a member from any user base they appear in.
void PruneMemberFromAllBases(const QString &username);

// A username belongs to one base only (the same goes for a group's public
// username among the groups bases): the Save* functions above leave out
// names another base already holds, and this cleans up what was saved
// before that rule existed - the oldest base keeps the name. Returns how
// many entries were removed.
int RemoveDuplicatesAcrossBases();

void DeleteScrapedGroup(uint64 id);

// The name a recipients base gets when it is made from a group: the group's
// own name plus its username in brackets ("Название (@username)"), just the
// name when it has no public username.
[[nodiscard]] QString GroupBaseTitle(
	const QString &name,
	const QString &username);

// Whether a recipients base titled `baseTitle` was made from the group with
// this `username` / `groupTitle` - true if either one matches ("Название
// (@username)", a bare "@username" / t.me link, or just the name).
[[nodiscard]] bool BaseTitleMatchesGroup(
	const QString &baseTitle,
	const QString &username,
	const QString &groupTitle);

// Per-username record of the outcome of a bulk SMS attempt (see
// settings/bulk_accounts_actions.cpp), kept in the same
// "tdata/ggtg_db/" directory as the bases above. `username` may
// be passed with or without a leading '@'; matching is case-insensitive.
// Used to skip a username on a later send - whether it reappears in the
// same base, a different one, or was typed by hand - instead of
// messaging it again, and to remember which account the attempt was
// made from.
enum class SmsSendStatus {
	// Actually delivered - permanent, never retried automatically.
	Sent,
	// The sending account hit Telegram's "mutual contacts only" limit
	// (MTP error PEER_FLOOD) on this username, so nothing was actually
	// sent. Also skipped on later runs, but only until cleared via
	// ClearAllSmsResettable - the account, not the recipient, was at fault.
	// Used only while the account itself is under a spam block (@spambot).
	SpamAcc,
	// The username didn't resolve to anybody (deleted / never existed).
	// Recorded only once the same account has shown its lookups work again
	// (see settings/sms_campaign.cpp), so a throttled account can't write
	// off real people. Cleared by ClearAllSmsResettable.
	NotFound,
	// The recipient can't be messaged (privacy settings, blocked us,
	// deactivated). Cleared by ClearAllSmsResettable.
	Restricted,
	// Telegram refused to let THIS account write to this person (PEER_FLOOD:
	// e.g. a number of one country writing to a stranger of another) while
	// the account itself is free of limits (@spambot says so). Unlike
	// SpamAcc, the account is fine; another account may get through. Cleared
	// by ClearAllSmsResettable.
	PeerLimit,
	// The sending account could not get to this person at all (FLOOD_WAIT,
	// a dead session, username lookups throttled): nothing was sent and the
	// recipient isn't at fault. Kept out of later runs until the mark is
	// taken off. Cleared by ClearAllSmsResettable.
	AccountFail,
	// Identified as a probable fake/bot during pre-send profile verification.
	// Skipped from sending and marked for manual review.
	SuspectedFake,
	// Recipient last seen long ago ("был(а) давно"): skipped from campaign
	// and removed from bases.
	LongAgo,
};

struct SmsSendRecord {
	QString username; // normalized: no '@', lowercase
	SmsSendStatus status = SmsSendStatus::Sent;
	QString account; // human-readable label of the account that tried it
	qint64 at = 0; // unix seconds
	// The base (see ScrapedGroupMeta::id) this recipient was taken from
	// when the attempt was made; 0 when unknown (older records, or a name
	// that appears in several bases - the first selected one wins).
	uint64 baseId = 0;
	// The name of that base at the time (a group's title, a list name):
	// kept in the record, so "where did you get my contact?" can still be
	// answered after the base was renamed or removed.
	QString source;
	// Why the mark was put, when it is not obvious (e.g. "аккаунт снят
	// после 6 отказов подряд"): shown next to the status.
	QString note;
};

// Adds a note to an existing record (nothing if there is none).
void SetSmsNote(const QString &username, const QString &note);

// The marks that can be taken off one kind at a time ("вернуть в очередь"):
// "Telegram не дал написать", "аккаунт в спам-блоке", "не найден",
// "получатель закрыл ЛС" - with their names, how many there are, and the
// clearing of just that kind.
[[nodiscard]] std::vector<SmsSendStatus> SmsMarkKinds();
[[nodiscard]] QString SmsMarkTitle(SmsSendStatus status);
[[nodiscard]] int CountSmsStatus(SmsSendStatus status);
int ClearSmsStatus(SmsSendStatus status);

// Where this person's contact came from (the base we wrote them from), or
// an empty string when unknown.
[[nodiscard]] QString SmsSourceOf(const QString &username);

[[nodiscard]] std::optional<SmsSendRecord> FindSmsSendRecord(
	const QString &username);
void MarkSmsStatus(
	const QString &username,
	SmsSendStatus status,
	const QString &account,
	uint64 baseId = 0);
// Also removes the username from base `baseId` (when known), so the base
// always holds only the people still to be messaged; that removal is
// written to disk a moment later, see FlushBasePrunes.
void MarkSmsSent(
	const QString &username,
	const QString &account,
	uint64 baseId = 0);

// Watches this session for text messages the user sends by hand (in an
// ordinary chat, outside a campaign) to a person with a username, and
// marks that username in the registry as already written to - so the next
// campaign doesn't write to someone the user has just talked to. Never
// overwrites an existing record (a campaign's own richer one, an
// earlier status). Called once per session when it is created.
void TrackManualSends(not_null<Main::Session*> session);
// A message the user sent by hand failed with Telegram's error `error`
// (PEER_FLOOD, USER_PRIVACY_RESTRICTED...): the person is marked in the
// registry the way a campaign marks them, so they are not written to again
// by mistake. Never touches a person already delivered to.
void TrackManualFailure(
	not_null<PeerData*> peer,
	const QString &error);
void MarkSmsSpamAcc(
	const QString &username,
	const QString &account,
	uint64 baseId = 0);

// Records that aren't final: SpamAcc, NotFound and Restricted (everything
// except Sent) - what the one-button "reset" brings back into play.
[[nodiscard]] int CountSmsResettable();

// Clears every non-Sent record (see CountSmsResettable) so those usernames
// can be messaged again - the one-button reset in the "База" list box.
void ClearAllSmsResettable();

// Only the "не пустило" records (PeerLimit): how many, and clearing them so
// another account can try those people. Everything else stays marked.
[[nodiscard]] int CountSmsPeerLimit();
int ClearSmsPeerLimit();

// Every record in the registry, newest first - backs the "Отправленные"
// browser (see settings/bulk_accounts_actions.cpp's MakeSmsSentUsersBox),
// the local equivalent of tg-web-panel's /api/recipients/sent-users.
[[nodiscard]] std::vector<SmsSendRecord> LoadAllSmsSendRecords();
// Just its size - cheap enough to recompute on every BasesChanged().
[[nodiscard]] int CountSmsSendRecords();

// Drops one record (any status) so that single username can be messaged
// again - a mistaken or stale entry, without resetting anyone else's.
void DeleteSmsSendRecord(const QString &username);

// Wipes the whole registry, Sent records included (unlike
// ClearAllSmsResettable) - every past recipient becomes messageable again.
void ClearAllSmsSendRecords();

// Telegram requires every bot's username to end in "bot" (enforced by
// BotFather), and a real person's never does - so the suffix reliably
// tells them apart without a request. Recipients are meant to be people.
[[nodiscard]] bool IsBotUsername(const QString &username);

// How one list of usernames stands against the send registry - the one
// place this is computed, so "База", "Отправка SMS" and the campaign itself
// can't disagree. Every username lands in exactly one bucket; `remaining`
// is what a campaign would actually still try.
struct BaseStats {
	int total = 0;
	int sent = 0;
	int spamAcc = 0;
	int notFound = 0;
	int restricted = 0;
	int peerLimit = 0;
	int accountFail = 0;
	int suspectedFake = 0;
	int longAgo = 0;
	int bots = 0;
	int remaining = 0;

	BaseStats &operator+=(const BaseStats &other) {
		total += other.total;
		sent += other.sent;
		spamAcc += other.spamAcc;
		notFound += other.notFound;
		restricted += other.restricted;
		peerLimit += other.peerLimit;
		accountFail += other.accountFail;
		suspectedFake += other.suspectedFake;
		longAgo += other.longAgo;
		bots += other.bots;
		remaining += other.remaining;
		return *this;
	}
};

[[nodiscard]] BaseStats ComputeBaseStats(const QStringList &usernames);
// A saved Users base by id (loads its usernames); zeros for a Groups base
// or an unknown id.
[[nodiscard]] BaseStats ComputeBaseStats(uint64 baseId);

// Fires whenever any base (its list or its contents) or the send registry
// changes, so a visible window can refresh its numbers instead of only
// noticing after being closed and reopened.
[[nodiscard]] rpl::producer<> BasesChanged();

// ---- For the sync between GGTG devices (core/ggtg_sync.cpp) ----
//
// What was deleted on purpose and when: "base:<id>" and "sent:<username>"
// -> unix seconds. A sync carries it along so a deletion on one device is
// not undone by the other device's older copy.
[[nodiscard]] std::map<QString, qint64> LoadTombstones();
void MergeTombstones(const std::map<QString, qint64> &other);
// Puts a base in as it is: keeps `meta.savedAt`, replaces its contents, no
// cross-base dedupe (the other device already did it).
void ImportBase(
	const ScrapedGroupMeta &meta,
	const QStringList &usernames,
	const std::vector<ScrapedGroupEntry> &entries);
// Removes a base without leaving a tombstone (the deletion came from there).
void RemoveBaseForSync(uint64 id);
// Writes records of the registry as they are (status, time, source) and
// drops the listed usernames, all in one go and without tombstones.
void ImportSmsRecords(
	const std::vector<SmsSendRecord> &upsert,
	const std::vector<QString> &remove);
// Takes the people already written to out of every recipients base.
int PruneDeliveredFromBases();

// Fetches every member of `peer` (paginated for large supergroups),
// keeps only those with a public @username - also excluding the
// creator/admins, bots, and anyone whose last-seen looks long-ago - and
// saves that username list locally under the group's peer id (see
// above). Shows its own progress/result toasts via `controller`; safe to
// call from a plain UI click handler.
// "Поиск групп": asks for a keyword, then searches Telegram with the window's
// current account (contacts.search, optionally widened through similar-group
// recommendations and the found groups' own names) and saves the megagroups
// it finds as a new BaseKind::Groups base, biggest first.
void SearchGroups(
	Window::SessionController *controller,
	std::shared_ptr<Ui::Show> show = nullptr);

// Collects the members of every group in a groups base into ONE new users
// base ("Участники: <base title>"), group after group with pauses between
// them, using the same filters and walk options as a single-group scrape.
void CollectFromGroupsBase(
	not_null<Window::SessionController*> controller,
	uint64 baseId);

void ScrapeGroupMembers(
	Window::SessionController *controller,
	not_null<PeerData*> peer,
	std::shared_ptr<Ui::Show> show = nullptr);

// The same, for a group known only by its public @username (a row of a
// groups base): looks it up first, then goes through ScrapeGroupMembers.
void ScrapeGroupByUsername(
	Window::SessionController *controller,
	const QString &username,
	std::shared_ptr<Ui::Show> show = nullptr);

// Display names of people and groups by @username, kept apart from the
// bases (which store usernames only) in names.json: remembered whenever a
// peer is seen while collecting, and shown when a base is viewed.
void RememberPeerName(not_null<PeerData*> peer);
// A name from elsewhere (the Сквидвард bot's lists): kept only while no
// name is known yet, a real Telegram name always wins.
void RememberNameIfUnknown(const QString &username, const QString &name);
// The remembered name, else the name the active session knows (and then
// remembers it); empty when neither knows the username.
[[nodiscard]] QString KnownPeerName(const QString &username);

struct UserMeta {
	QString name;
	uint64 id = 0;
	qint64 lastseen = 0; // SortByOnlineValue or unix seconds
	QString lastseenText;
};

[[nodiscard]] UserMeta LookupUserMeta(const QString &username);
void RememberUserMeta(const QString &username, const UserMeta &meta);

// "Убрать каналы" for a groups base: looks up every group not checked yet,
// drops broadcast channels and people, marks the rest as checked. Stops
// by itself when Telegram limits the lookups; run again later to go on.
// `changed` is called after the base is rewritten.
void RemoveChannelsFromGroupsBase(
	not_null<Window::SessionController*> controller,
	std::shared_ptr<Ui::Show> show,
	uint64 baseId,
	Fn<void()> changed);

} // namespace Data
