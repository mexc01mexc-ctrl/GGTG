/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and exceptions, see https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtCore/QString>
#include <QtCore/QStringList>

#include <optional>
#include <vector>

// Reading @skvidvarde_bot's replies into plain data. Depends on Qt Core only
// (see Telegram/build/ggtg/test_squidward_parse.sh, which builds its test
// without the rest of the app).
namespace Window::Squidward {

struct Entity {
	enum class Type {
		Bold,
		Url,
		CustomUrl,
		Mention,
		MentionName,
		Other,
	};
	Type type = Type::Other;
	int offset = 0;
	int length = 0;
	QString data;
};

// A bot message's text and its formatting/link entities.
struct Message {
	QString text;
	std::vector<Entity> entities;
};

struct NameChange {
	QString date; // "2026-09-17"
	QString name;
};

// A person's card. Numbers are -1 and strings empty when the bot didn't say.
struct UserCard {
	QString name;
	QString id;
	QString activeSince; // "13.06.2018"
	QString activeUntil; // "сегодня 12:06"
	int messages = -1;
	int chats = -1;
	QString diversity; // "67,15%"
	QString replies; // "96,16%"
	QString media; // "85,85%"
	int roundVideos = -1;
	int voices = -1;
	QString favoriteChat;
	QString channel;
	int searched = -1;
	QStringList usernames; // without '@'
	std::vector<NameChange> names;
};

struct TopMember {
	QString name;
	int messages = 0;
};

// A group's card. Same "not said" convention as UserCard.
struct GroupCard {
	QString name;
	QString id;
	int topPlace = -1;
	int topOf = -1;
	int peopleDay = -1;
	int messagesDay = -1;
	int mediaDay = -1;
	QString mediaDayPercent; // "98,99%"
	int roundVideosDay = -1;
	int voicesDay = -1;
	QString diversity;
	QString description;
	std::vector<TopMember> topDay;
};

struct PageInfo {
	int page = 0;
	int pages = 0;
	int total = -1; // "Всего N", -1 when absent
};

// One line of a user's groups list: a public group (username), or a closed
// one (link to t.me/c/..., t.me/+..., t.me/joinchat/...).
struct GroupRow {
	QString username;
	QString title;
	QString link;
};

// One line of a group's members list: a username straight from a t.me link,
// or a text-mention's raw data to be resolved by the caller.
struct UserRow {
	QString username;
	QString mentionData;
	QString name; // as the bot shows it (the link's text), may be stylized
};

// Lower-cases and folds the bot's look-alike letters (Latin/Greek/small caps
// in Russian words, "İD", "ɪᴅ"...) one character to one character, so
// positions in the result match positions in `text`.
[[nodiscard]] QString Fold(const QString &text);
// A keyboard button's last word, folded ("🎖 Топ, люди" -> "люди").
[[nodiscard]] QString ButtonLastWord(const QString &text);
// A keyboard button's letters only, folded ("15💎 Groups" -> "groups").
[[nodiscard]] QString ButtonWords(const QString &text);
[[nodiscard]] std::optional<PageInfo> ParsePageInfo(const QString &text);
// "t.me/name" -> "name"; empty for anything else.
[[nodiscard]] QString UsernameFromLink(const QString &link);
// The link itself if it's a closed group's link; empty otherwise.
[[nodiscard]] QString PrivateLink(const QString &link);
// The public username a lookup query names ("@name", "name", t.me link);
// empty for ids, closed links and anything invalid.
[[nodiscard]] QString UsernameFromQuery(const QString &query);
// The bot's "⏳ Вычисляю..." stand-in before the real answer.
[[nodiscard]] bool IsPlaceholder(const QString &text);
// Trims lines, keeps at most one empty line in a row, trims the result.
[[nodiscard]] QString CleanText(const QString &text);
// 30707 -> "30 707".
[[nodiscard]] QString FormatCount(int value);
// `query` first, no case-insensitive repeats, at most `limit` entries.
[[nodiscard]] QStringList PushRecent(
	QStringList list,
	const QString &query,
	int limit);

[[nodiscard]] std::vector<GroupRow> ExtractGroupRows(const Message &message);
[[nodiscard]] std::vector<UserRow> ExtractUserRows(const Message &message);
// Keeps the first of each username / link (case-insensitive).
[[nodiscard]] std::vector<GroupRow> DedupeGroupRows(
	std::vector<GroupRow> rows);
// Strips '@', drops empty names and bots ("...bot"), keeps the first of each
// (case-insensitive).
[[nodiscard]] QStringList DedupeUsernames(const QStringList &usernames);

[[nodiscard]] std::optional<UserCard> ParseUserCard(const Message &message);
[[nodiscard]] std::optional<GroupCard> ParseGroupCard(const Message &message);
// Human-readable lines for a card; fields the bot didn't give are left out.
[[nodiscard]] QStringList FormatUserDetails(const UserCard &card);
[[nodiscard]] QStringList FormatGroupDetails(const GroupCard &card);

} // namespace Window::Squidward
