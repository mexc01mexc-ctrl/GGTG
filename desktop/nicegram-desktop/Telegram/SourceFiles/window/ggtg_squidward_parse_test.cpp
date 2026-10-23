/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and exceptions, see https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
// Standalone checks for ggtg_squidward_parse.cpp - not part of the app build.
// Run: Telegram/build/ggtg/test_squidward_parse.sh
#include "window/ggtg_squidward_parse.h"

#include <QtCore/QLocale>

#include <cstdio>

using namespace Window::Squidward;
using namespace Qt::StringLiterals;

namespace {

int Failures = 0;

void Check(bool ok, const char *what, int line) {
	if (!ok) {
		++Failures;
		std::printf("FAIL line %d: %s\n", line, what);
	}
}

void CheckString(
		const QString &actual,
		const QString &expected,
		const char *what,
		int line) {
	if (actual != expected) {
		++Failures;
		std::printf(
			"FAIL line %d: %s\n  actual:   \"%s\"\n  expected: \"%s\"\n",
			line,
			what,
			actual.toUtf8().constData(),
			expected.toUtf8().constData());
	}
}

#define CHECK(x) Check((x), #x, __LINE__)
#define CHECK_EQ(a, b) Check((a) == (b), #a " == " #b, __LINE__)
#define CHECK_STR(a, b) CheckString((a), (b), #a, __LINE__)

[[nodiscard]] Entity Ent(
		Entity::Type type,
		const QString &text,
		const QString &shown,
		const QString &data = QString()) {
	return { type, int(text.indexOf(shown)), int(shown.size()), data };
}

void TestFoundation() {
	CHECK_STR(Fold(u"İD"_s), u"id"_s);
	CHECK_STR(Fold(u"ɪᴅ"_s), u"id"_s);
	CHECK_STR(Fold(u"Кαнαл"_s), u"канал"_s);
	CHECK_EQ(Fold(u"userŋames 🍀"_s).size(), u"userŋames 🍀"_s.size());
	CHECK_STR(ButtonLastWord(u"👁 Группы"_s), Fold(u"группы"_s));
	CHECK_STR(ButtonLastWord(u"🎖 Тoп, люди"_s), Fold(u"люди"_s));

	const auto page = ParsePageInfo(u"Всего 1262, страница 1 из 85"_s);
	CHECK(page.has_value());
	CHECK_EQ(page->page, 1);
	CHECK_EQ(page->pages, 85);
	CHECK_EQ(page->total, 1262);
	CHECK(!ParsePageInfo(u"Это FAMBUN"_s).has_value());

	CHECK_STR(UsernameFromLink(u"https://t.me/fambun_chat"_s), u"fambun_chat"_s);
	CHECK(UsernameFromLink(u"https://t.me/c/2361171296/896316"_s).isEmpty());
	CHECK_STR(
		PrivateLink(u"https://t.me/c/2361171296/896316"_s),
		u"https://t.me/c/2361171296/896316"_s);
	CHECK_STR(PrivateLink(u"t.me/+AbCd_ef"_s), u"t.me/+AbCd_ef"_s);
	CHECK(PrivateLink(u"https://t.me/fambun_chat"_s).isEmpty());

	CHECK_STR(UsernameFromQuery(u"@Qiuvvinn"_s), u"Qiuvvinn"_s);
	CHECK_STR(UsernameFromQuery(u" https://t.me/fambun_chat "_s), u"fambun_chat"_s);
	CHECK(UsernameFromQuery(u"https://t.me/c/2361171296/896316"_s).isEmpty());
	CHECK(UsernameFromQuery(u"149474479"_s).isEmpty());

	CHECK(IsPlaceholder(u"⏳ Вычисляю..."_s));
	CHECK(!IsPlaceholder(u"Это FAMBUN"_s));
	// Live sample: Latin "B" in "Вычисляю", plus an id suffix.
	CHECK(IsPlaceholder(u"ㅤ⏳ Bычисляю...[id149474479]"_s));
	CHECK(IsPlaceholder(u"⏳ ..."_s));
	CHECK_STR(CleanText(u"  a  \n\n\n\nb\n"_s), u"a\n\nb"_s);
	CHECK_STR(FormatCount(30707), QLocale(QLocale::Russian).toString(30707));

	CHECK_EQ(
		PushRecent({ u"@a"_s, u"@b"_s }, u"@B"_s, 8),
		(QStringList{ u"@B"_s, u"@a"_s }));
	auto many = QStringList();
	for (auto i = 0; i != 10; ++i) {
		many.push_back(QString::number(i));
	}
	CHECK_EQ(PushRecent(many, u"x"_s, 8).size(), 8);
}

[[nodiscard]] Message GroupsPage() {
	auto m = Message();
	m.text = u"Чаты аккаунта 149474479.\n"
		u"👮-админ, 🔒-приватный, ✖️-вышел\n"
		u"Посл. сообщ - чат (всего сообщений)\n\n"
		u"12:06 FAMBUN 🐇 (1503)\n"
		u"11:57 Teo Iŋfłuencers™.. (81)\n"
		u"09:29 pemuluŋ jalanan (922)\n\n"
		u"Всего 1262, страница 1 из 85"_s;
	m.entities = {
		Ent(Entity::Type::CustomUrl, m.text, u"149474479"_s, u"tg://user?id=149474479"_s),
		Ent(Entity::Type::CustomUrl, m.text, u"FAMBUN 🐇"_s, u"https://t.me/fambun_chat"_s),
		Ent(Entity::Type::CustomUrl, m.text, u"Teo Iŋfłuencers™.."_s, u"https://t.me/c/2361171296/896316"_s),
		Ent(Entity::Type::CustomUrl, m.text, u"pemuluŋ jalanan"_s, u"https://t.me/FAMBUN_chat"_s),
		Ent(Entity::Type::Bold, m.text, u"1262"_s),
	};
	return m;
}

void TestRows() {
	const auto rows = ExtractGroupRows(GroupsPage());
	CHECK_EQ(int(rows.size()), 3); // the account-id link is not a group
	CHECK_STR(rows[0].username, u"fambun_chat"_s);
	CHECK_STR(rows[0].title, u"FAMBUN 🐇"_s);
	CHECK(rows[1].username.isEmpty());
	CHECK_STR(rows[1].link, u"https://t.me/c/2361171296/896316"_s);
	CHECK_STR(rows[1].title, u"Teo Iŋfłuencers™.."_s);

	CHECK_EQ(int(DedupeGroupRows(rows).size()), 2);

	// Review #4: "(всего сообщений)" in the header isn't the total.
	const auto fullPage = ParsePageInfo(GroupsPage().text);
	CHECK(fullPage.has_value() && fullPage->total == 1262);

	// Review #12: one closed chat seen with different message ids.
	CHECK_EQ(int(DedupeGroupRows({
		GroupRow{ .link = u"https://t.me/c/2361171296/1"_s },
		GroupRow{ .link = u"https://t.me/c/2361171296/2"_s },
		GroupRow{ .link = u"https://t.me/c/999/2"_s },
	}).size()), 2);
	auto twice = rows;
	twice.insert(twice.end(), rows.begin(), rows.end());
	CHECK_EQ(int(DedupeGroupRows(twice).size()), 2);

	auto members = Message();
	members.text = u"Участники чата\n"
		u"01. ✓ sheAy fourtis - 449\n"
		u"02. ✓ RayRed - 243\n"
		u"03. ✓ HelperBot - 3\n\n"
		u"Всего 8067, страница 1 из 404"_s;
	members.entities = {
		Ent(Entity::Type::MentionName, members.text, u"sheAy fourtis"_s, u"12345.678"_s),
		Ent(Entity::Type::CustomUrl, members.text, u"RayRed"_s, u"https://t.me/RayRed"_s),
		Ent(Entity::Type::CustomUrl, members.text, u"HelperBot"_s, u"https://t.me/HelperBot"_s),
	};
	const auto users = ExtractUserRows(members);
	CHECK_EQ(int(users.size()), 3);
	CHECK_STR(users[0].mentionData, u"12345.678"_s);
	CHECK_STR(users[1].username, u"RayRed"_s);
	CHECK_STR(users[0].name, u"sheAy fourtis"_s);
	CHECK_STR(users[1].name, u"RayRed"_s);

	CHECK_EQ(
		DedupeUsernames({ u"@RayRed"_s, u"rayred"_s, u"HelperBot"_s, u""_s, u"@Mogu"_s }),
		(QStringList{ u"RayRed"_s, u"Mogu"_s }));
}

[[nodiscard]] Message UserCardMessage() {
	auto m = Message();
	m.text = u"Это Qiuvviɳɳ\n"
		u"Разнообразие сообщ. 67,15%\n"
		u"С 13.06.2018 по сегодня 12:06\n"
		u"30707 сообщений в 446 чатах\n"
		u"96,16% реплаи 85,85% медиа\n"
		u"Кружки: 0, голос: 0\n"
		u"любимый чат: pemuluŋ jałanaŋ\n"
		u"Искали: 2\n"
		u"Кαнал: Proofs\n\n"
		u"ɪᴅ: 149474479\n"
		u"userŋames: (3 oƒ 10)\n"
		u"| @Qiuvvinn | @RandyPlenger | @WangiWon\n"
		u"Имена: (3 из 10)\n"
		u"├2026-09-17 ➔ 🍀 Qiuvviɳɳ 🍀\n"
		u"├2026-09-17 ➔ 😎 Qiuvviɳɳ 😎\n"
		u"├2026-09-14 ➔ Qiuvviɳɳ@stakecruncher *FREPP"_s;
	return m;
}

void TestUserCard() {
	const auto card = ParseUserCard(UserCardMessage());
	CHECK(card.has_value());
	if (!card) {
		return;
	}
	CHECK_STR(card->name, u"Qiuvviɳɳ"_s);
	CHECK_STR(card->id, u"149474479"_s);
	CHECK_STR(card->diversity, u"67,15%"_s);
	CHECK_STR(card->activeSince, u"13.06.2018"_s);
	CHECK_STR(card->activeUntil, u"сегодня 12:06"_s);
	CHECK_EQ(card->messages, 30707);
	CHECK_EQ(card->chats, 446);
	CHECK_STR(card->replies, u"96,16%"_s);
	CHECK_STR(card->media, u"85,85%"_s);
	CHECK_EQ(card->roundVideos, 0);
	CHECK_EQ(card->voices, 0);
	CHECK_STR(card->favoriteChat, u"pemuluŋ jałanaŋ"_s);
	CHECK_EQ(card->searched, 2);
	CHECK_STR(card->channel, u"Proofs"_s);
	CHECK_EQ(
		card->usernames,
		(QStringList{ u"Qiuvvinn"_s, u"RandyPlenger"_s, u"WangiWon"_s }));
	CHECK_EQ(int(card->names.size()), 3);
	if (card->names.size() == 3) {
		CHECK_STR(card->names[0].date, u"2026-09-17"_s);
		CHECK_STR(card->names[0].name, u"🍀 Qiuvviɳɳ 🍀"_s);
		CHECK_STR(card->names[2].name, u"Qiuvviɳɳ@stakecruncher *FREPP"_s);
	}

	const auto lines = FormatUserDetails(*card);
	CHECK_EQ(lines.size(), 7);
	if (lines.size() == 7) {
		CHECK_STR(lines[0], u"Активен с 13.06.2018 по сегодня 12:06"_s);
		CHECK_STR(lines[1], FormatCount(30707) + u" сообщ. в "_s + FormatCount(446) + u" чатах"_s);
		CHECK_STR(lines[3], u"Реплаи 96,16% · медиа 85,85% · кружки 0 · голос 0"_s);
		CHECK_STR(lines[5], u"Канал: Proofs · искали: 2"_s);
		CHECK_STR(lines[6], u"Юзернеймы: @Qiuvvinn @RandyPlenger @WangiWon"_s);
	}

	// Review focus 1: a card with most lines missing.
	auto partial = Message();
	partial.text = u"Это Someone\nɪᴅ: 42\n30 сообщений в 3 чатах"_s;
	const auto small = ParseUserCard(partial);
	CHECK(small.has_value());
	if (small) {
		CHECK_EQ(
			FormatUserDetails(*small),
			(QStringList{ FormatCount(30) + u" сообщ. в "_s + FormatCount(3) + u" чатах"_s }));
	}
	auto notACard = Message();
	notACard.text = u"Ничего не найдено"_s;
	CHECK(!ParseUserCard(notACard).has_value());
}

[[nodiscard]] Message GroupCardMessage(bool withDescription) {
	auto m = Message();
	m.text = u"Это ƑAMBuN 🐇🐰👯\n"
		u"В топе # 17851 из 756778\n"
		u"Разнообразие сообщ. 9,80%\n"
		u"349 человek за сутки\n"
		u"написали 579 сообщений\n"
		u"из них 572 (98,99%) медиа\n"
		u"0 кружков и 0 голосовых\n"
		u"İD: 2303994538\n"
		u"Описание:\n"
		u"--- --- --- --- --- --- ---\n"_s
		+ (withDescription
			? u"Лучший чат о розыгрышах\nЗаходите!\n"_s
			: u"\n\n"_s)
		+ u"--- --- --- --- --- --- ---\n"
		u"Топ за сутки:\n"
		u"1. Ay🐰 - 25\n"
		u"2. ołaaaa - 18\n"
		u"3. S R - 15"_s;
	return m;
}

void TestGroupCard() {
	const auto card = ParseGroupCard(GroupCardMessage(true));
	CHECK(card.has_value());
	if (!card) {
		return;
	}
	CHECK_STR(card->name, u"ƑAMBuN 🐇🐰👯"_s);
	CHECK_STR(card->id, u"2303994538"_s);
	CHECK_EQ(card->topPlace, 17851);
	CHECK_EQ(card->topOf, 756778);
	CHECK_STR(card->diversity, u"9,80%"_s);
	CHECK_EQ(card->peopleDay, 349);
	CHECK_EQ(card->messagesDay, 579);
	CHECK_EQ(card->mediaDay, 572);
	CHECK_STR(card->mediaDayPercent, u"98,99%"_s);
	CHECK_EQ(card->roundVideosDay, 0);
	CHECK_EQ(card->voicesDay, 0);
	CHECK_STR(card->description, u"Лучший чат о розыгрышах\nЗаходите!"_s);
	CHECK_EQ(int(card->topDay.size()), 3);
	if (card->topDay.size() == 3) {
		CHECK_STR(card->topDay[0].name, u"Ay🐰"_s);
		CHECK_EQ(card->topDay[0].messages, 25);
		CHECK_STR(card->topDay[2].name, u"S R"_s);
	}
	const auto lines = FormatGroupDetails(*card);
	CHECK_EQ(lines.size(), 4);
	if (lines.size() == 4) {
		CHECK_STR(lines[0], u"В топе #"_s + FormatCount(17851) + u" из "_s + FormatCount(756778));
		CHECK_STR(lines[1], u"За сутки: "_s + FormatCount(349) + u" человек · "_s
			+ FormatCount(579) + u" сообщ. · медиа "_s + FormatCount(572) + u" (98,99%)"_s);
		CHECK_STR(lines[2], u"Кружки 0 · голосовые 0"_s);
	}

	const auto empty = ParseGroupCard(GroupCardMessage(false));
	CHECK(empty.has_value() && empty->description.isEmpty());

	// A card is one kind or the other, never both.
	CHECK(!ParseUserCard(GroupCardMessage(true)).has_value());
	CHECK(!ParseGroupCard(UserCardMessage()).has_value());
}

} // namespace


void TestLiveUserCard() {
	// Live reply of 2026-09-29 16:02: fullwidth "Ｃ", Greek "Ι" in "ID".
	const auto card = ParseUserCard(Message{ .text = u"ㅤЭтo    Qiᴜvviηn    \nΡαзнoобрαзиe cоοбщ. 67,13%  \nＣ 13.06.2018 πо сегοдня 13:36  \n30709 cоoбщ℮ний в 446 чαтαх  \n96,16% ρ℮плαи 85,85% медиа  \nKργжқи: 0, голоc: 0  \nЛюбимый чαт: ρemᴜluηց jαlαnαη\nИckали: 2\nKαнал: Ρrоοƒѕ\n  \nΙＤ: 149474479  \nᴜѕ℮rnαmes: (3 оf 10)\n| @Qiuvvinn | @RandyPlenger | @WangiWon  \nИмена: (3 из 10)\n├ 2026-09-17  ➜  🍀 Qiᴜvviηη 🍀  \n├ 2026-09-17  ➜  😎 Qiᴜvvinη 😎  \n├ 2026-09-14  ➜  Qiᴜvviηη@stakecruncher *FREΡP"_s });
	CHECK(card.has_value());
	if (card) {
		CHECK_STR(card->id, u"149474479"_s);
		CHECK(card->messages == 30709);
		CHECK(card->chats == 446);
		CHECK(card->searched == 2);
		CHECK(card->usernames.size() == 3);
		CHECK(card->names.size() == 3);
	}
}


void TestLiveUserCard2() {
	// Live reply of 2026-09-29 16:04: "℮", "ᴧ", "İＤ".
	const auto card = ParseUserCard(Message{ .text = u"ㅤЭто    Qiᴜvviηη    \nΡαзнοобрази℮ соoбщ. 67,13%  \nＣ 13.06.2018 πo c℮гoдня 13:36  \n30709 сoобщений в 446 чαтαx  \n96,16% ρeπлαи 85,85% м℮диа  \nKργжқи: 0, гoлοc: 0  \nᴧюбимый чαт: ρеmᴜlᴜηg jαⅼαηαη\nИcқαли: 2\nKαнал: Proofs\n  \nİＤ: 149474479  \nᴜѕerηαm℮ѕ: (3 оƒ 10)\n| @Qiuvvinn | @RandyPlenger | @WangiWon  \nИм℮на: (3 из 10)\n├ 2026-09-17  ➜  🍀 Qiᴜvviηη 🍀  \n├ 2026-09-17  ➜  😎 Qiᴜvvinη 😎  \n├ 2026-09-14  ➜  Qiuvviηη@stakecruncher *ƑREPΡ"_s });
	CHECK(card.has_value());
	if (card) {
		CHECK(!card->name.isEmpty());
		CHECK_STR(card->id, u"149474479"_s);
		CHECK(!card->diversity.isEmpty());
		CHECK(!card->activeSince.isEmpty());
		CHECK(!card->replies.isEmpty());
		CHECK(!card->favoriteChat.isEmpty());
		CHECK(card->searched == 2);
		CHECK(card->roundVideos == 0);
		CHECK(card->usernames.size() == 3);
		CHECK(card->names.size() == 3);
	}
}

void TestEnglish() {
	CHECK_STR(ButtonWords(u"15💎 Groups"_s), Fold(u"groups"_s));
	CHECK_STR(ButtonWords(u"👁 Группы"_s), Fold(u"группы"_s));
	CHECK(ButtonWords(u"1💎 Similar groups"_s) != Fold(u"groups"_s));
	CHECK_STR(ButtonLastWord(u"🎖 Ţop, members"_s), Fold(u"members"_s));
	CHECK_STR(Fold(u"Ţop"_s), Fold(u"top"_s));
	CHECK_STR(Fold(u"peopℓe"_s), Fold(u"people"_s));
	CHECK_STR(Fold(u"ɢʀᴏᴜᴘꜱ"_s), Fold(u"groups"_s));

	const auto text = u"Ţhis is Simoŋ S\n"
		"Μessage diversity 93.88%\n"
		"Ƒrom 4/29/2019 τo 8/18/2026\n"
		"2562 messages in 16 groups\n"
		"70.06% replies 62.33% media\n"
		"Circles: 0, voice: 0\n"
		"Favorite group:\n"
		"Admin in groups: 6\n"
		"Were ℓooking for: 4\n"
		"\n"
		"ID: 242034638\n"
		"usernames:\n"
		"| @seaevery | @sinbidam\n"
		"first ŋame / last ŋame: (3 of 5)\n"
		"├2026-10-04 ➔ Simon S\n"
		"├2026-05-14 ➔ Simon"_s;
	const auto card = ParseUserCard({ .text = text });
	CHECK(card.has_value());
	if (!card) {
		return;
	}
	CHECK_STR(card->name, u"Simoŋ S"_s);
	CHECK_STR(card->id, u"242034638"_s);
	CHECK_EQ(card->messages, 2562);
	CHECK_EQ(card->chats, 16);
	CHECK_STR(card->replies, u"70.06%"_s);
	CHECK_STR(card->media, u"62.33%"_s);
	CHECK_STR(card->diversity, u"93.88%"_s);
	CHECK_STR(card->activeSince, u"4/29/2019"_s);
	CHECK_STR(card->activeUntil, u"8/18/2026"_s);
	CHECK_EQ(card->roundVideos, 0);
	CHECK_EQ(card->voices, 0);
	CHECK_EQ(card->searched, 4);
	CHECK_EQ(int(card->usernames.size()), 2);
	CHECK_EQ(int(card->names.size()), 2);
	CHECK(!ParseGroupCard({ .text = text }).has_value());
}

int main() {
	TestFoundation();
	TestRows();
	TestUserCard();
	TestLiveUserCard();
	TestLiveUserCard2();
	TestGroupCard();
	TestEnglish();
	if (Failures) {
		std::printf("%d check(s) failed\n", Failures);
		return 1;
	}
	std::printf("All checks passed\n");
	return 0;
}
