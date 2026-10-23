/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and exceptions, see https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "window/ggtg_squidward_parse.h"

#include <QtCore/QHash>
#include <QtCore/QLocale>
#include <QtCore/QRegularExpression>
#include <QtCore/QSet>

#include <algorithm>

namespace Window::Squidward {

namespace {

// Qt::StringLiterals is Qt 6 only, and the Windows build is on Qt 5.
[[nodiscard]] QString operator""_s(const char16_t *text, std::size_t size) {
	return QString::fromUtf16(text, int(size));
}

[[nodiscard]] QChar FoldChar(QChar ch) {
	// Look-alikes the bot mixes into Russian words, and the odd letters of
	// its "ID" labels; one character to one character, always.
	static const auto table = QHash<uint, char16_t>{
		{ u'a', u'а' }, { u'c', u'с' }, { u'e', u'е' }, { u'o', u'о' },
		{ u'p', u'р' }, { u'x', u'х' }, { u'y', u'у' }, { u'r', u'г' },
		{ u'k', u'к' },
		{ 0x03BF, u'о' }, { 0x03B3, u'у' }, { 0x03C1, u'р' },
		{ 0x03B1, u'а' }, { 0x03C0, u'п' }, { 0x03B7, u'н' },
		{ 0x03BD, u'н' }, { 0x1D0F, u'о' }, { 0x2113, u'л' },
		{ 0x0251, u'а' }, { 0x04A1, u'к' }, { 0x0142, u'л' },
		{ 0x01EB, u'о' },
		{ 0x026A, u'i' }, { 0x0131, u'i' }, { 0x0130, u'i' }, { 0x1D05, u'd' },
		{ 0x014B, u'н' }, { 0x0273, u'н' }, { u'n', u'н' }, { 0x0192, u'f' },
		// Seen live: the bot draws new look-alikes on every reply.
		{ u'b', u'в' }, { u'h', u'н' }, { u'm', u'м' }, { u't', u'т' },
		{ 0x212E, u'е' }, { 0x1D1C, u'u' }, { 0x0455, u's' },
		{ 0x0456, u'i' }, { 0x0458, u'j' }, { 0x049B, u'к' },
		{ 0x03B9, u'i' }, { 0x03BA, u'к' }, { 0x03C4, u'т' },
		{ 0x03C5, u'u' }, { 0x03B5, u'е' }, { 0x03C7, u'х' },
		{ 0x03B2, u'в' }, { 0x03BC, u'м' }, { 0x03C3, u'о' },
		{ 0x1D00, u'а' }, { 0x0299, u'в' }, { 0x1D04, u'с' },
		{ 0x1D07, u'е' }, { 0x029C, u'н' }, { 0x1D0B, u'к' },
		{ 0x1D0D, u'м' }, { 0x0274, u'н' }, { 0x1D27, u'л' }, { 0x1D18, u'р' },
		{ 0x1D1B, u'т' }, { 0x028F, u'у' }, { 0x0280, u'г' },
		{ 0x3164, u' ' }, { 0x00A0, u' ' },
		// English replies: look-alikes go to the Latin letter, which then
		// folds like a plain one ("Ţop" == "top", "peopℓe" == "people").
		{ u'l', u'л' }, { 0x029F, u'l' }, { 0x0261, u'g' }, { 0x0262, u'g' },
		{ 0xA731, u's' }, { 0x1D20, u'v' }, { 0x1D21, u'w' }, { 0x1D22, u'z' },
		{ 0x1D0A, u'j' }, { 0x0493, u'f' }, { 0x057D, u'u' }, { 0x0585, u'o' },
		{ 0x0578, u'n' }, { 0x0570, u'h' }, { 0x0581, u'g' }, { 0x0566, u'q' },
		{ 0x0501, u'd' }, { 0x051B, u'q' }, { 0x051D, u'w' }, { 0x04CF, u'l' },
		{ 0x0269, u'i' }, { 0x0268, u'i' }, { 0x0257, u'd' }, { 0x0256, u'd' },
		{ 0x01AB, u't' }, { 0x01AD, u't' }, { 0x0288, u't' }, { 0x0180, u'b' },
		{ 0x0253, u'b' }, { 0x0272, u'n' }, { 0x0271, u'm' }, { 0x0282, u's' },
		{ 0x0290, u'z' }, { 0x0263, u'y' }, { 0x01B4, u'y' }, { 0x028B, u'v' },
		{ 0x0254, u'o' }, { 0x0250, u'a' }, { 0x0252, u'a' },
	};
	auto code = uint(ch.unicode());
	if (code >= 0xFF01 && code <= 0xFF5E) {
		code -= 0xFEE0; // fullwidth "Ｃ", "Ｄ" -> ASCII
	}
	auto folded = QChar(char16_t(code));
	// Accented Latin ("Ţ", "ŋ" stays) -> its base letter; Cyrillic "й",
	// "ё" and other scripts are left as they are.
	if (folded.decompositionTag() == QChar::Canonical
		|| folded.decompositionTag() == QChar::Font) {
		const auto base = folded.decomposition().at(0);
		if (base.unicode() < 0x0250 && base.isLetter()) {
			folded = base;
		}
	}
	const auto lower = uint(folded.toLower().unicode());
	const auto once = uint(table.value(lower, char16_t(lower)));
	return QChar(table.value(once, char16_t(once)));
}

QString L(const QString &label) {
	return QRegularExpression::escape(Fold(label));
}

// The bot answers in Russian or English: any of the labels.
QString L(std::initializer_list<QString> labels) {
	auto list = QStringList();
	for (const auto &label : labels) {
		list.push_back(L(label));
	}
	return u"(?:"_s + list.join(u'|') + u")"_s;
}

[[nodiscard]] QString Capitalized(QString text) {
	if (!text.isEmpty()) {
		text[0] = text[0].toUpper();
	}
	return text;
}

[[nodiscard]] QString DiversityPattern() {
	return u"(?:"_s + L(u"разнообразие сообщ"_s) + uR"(\S*|)"_s
		+ L(u"message diversity"_s) + uR"()\s*([\d.,]+\s*%))"_s;
}

// Line-by-line matching on the folded text; captures are cut from the
// original line at the same positions (Fold keeps them aligned).
class Reader final {
public:
	explicit Reader(const QString &text) {
		for (const auto &line : text.split(u'\n')) {
			_lines.push_back({ line, Fold(line) });
		}
	}

	[[nodiscard]] int size() const {
		return int(_lines.size());
	}
	[[nodiscard]] const QString &original(int index) const {
		return _lines[index].original;
	}

	// Captures of the first line at or after `from` matching `pattern`.
	[[nodiscard]] std::optional<QStringList> find(
			const QString &pattern,
			int *index = nullptr,
			int from = 0) const {
		const auto re = QRegularExpression(pattern);
		for (auto i = from; i < size(); ++i) {
			const auto match = re.match(_lines[i].folded);
			if (!match.hasMatch()) {
				continue;
			}
			if (index) {
				*index = i;
			}
			auto result = QStringList();
			for (auto n = 1; n <= match.lastCapturedIndex(); ++n) {
				result.push_back(_lines[i].original.mid(
					match.capturedStart(n),
					match.capturedLength(n)).trimmed());
			}
			return result;
		}
		return std::nullopt;
	}

	[[nodiscard]] bool has(const QString &pattern) const {
		return find(pattern).has_value();
	}

private:
	struct Line {
		QString original;
		QString folded;
	};
	std::vector<Line> _lines;

};

[[nodiscard]] int ToInt(const QString &text) {
	auto digits = QString();
	for (const auto ch : text) {
		if (ch.isDigit()) {
			digits.push_back(ch);
		}
	}
	return digits.isEmpty() ? -1 : digits.toInt();
}

// Shared by both cards.
[[nodiscard]] QString NamePattern() {
	return uR"(^\s*)"_s + L({ u"это"_s, u"this is"_s }) + uR"(\s+(.+)$)"_s;
}
constexpr auto kIdPattern = u"^\\s*id\\s*:\\s*(\\d+)";

} // namespace

QString Fold(const QString &text) {
	auto result = text;
	for (auto i = 0; i != result.size(); ++i) {
		result[i] = FoldChar(text[i]);
	}
	return result;
}

QString ButtonLastWord(const QString &text) {
	const auto parts = text.simplified().split(u' ', Qt::SkipEmptyParts);
	return parts.isEmpty() ? QString() : Fold(parts.back());
}

QString ButtonWords(const QString &text) {
	auto letters = QString();
	for (const auto ch : Fold(text)) {
		letters.push_back(ch.isLetter() ? ch : QChar(u' '));
	}
	return letters.simplified();
}

std::optional<PageInfo> ParsePageInfo(const QString &text) {
	const auto folded = Fold(text);
	static const auto pages = QRegularExpression(
		L({ u"страниц"_s, u"page"_s }) + uR"(\D*(\d+)\D+(\d+))"_s);
	static const auto total = QRegularExpression(
		L({ u"всего"_s, u"total"_s }) + uR"(\s*:?\s*(\d+))"_s);
	const auto match = pages.match(folded);
	if (!match.hasMatch()) {
		return std::nullopt;
	}
	auto result = PageInfo{
		.page = match.captured(1).toInt(),
		.pages = match.captured(2).toInt(),
	};
	if (const auto all = total.match(folded); all.hasMatch()) {
		result.total = all.captured(1).toInt();
	}
	return result;
}

QString UsernameFromLink(const QString &link) {
	static const auto expression = QRegularExpression(
		uR"(^(?:https?://)?t(?:elegram)?\.me/([A-Za-z0-9_]{4,32})/?$)"_s);
	const auto match = expression.match(link.trimmed());
	return match.hasMatch() ? match.captured(1) : QString();
}

QString PrivateLink(const QString &link) {
	static const auto expression = QRegularExpression(
		uR"(^(?:https?://)?t(?:elegram)?\.me/(?:c/\d+(?:/\d+)?|\+[A-Za-z0-9_-]+|joinchat/[A-Za-z0-9_-]+)/?$)"_s);
	const auto trimmed = link.trimmed();
	return expression.match(trimmed).hasMatch() ? trimmed : QString();
}

QString UsernameFromQuery(const QString &query) {
	const auto trimmed = query.trimmed();
	if (!PrivateLink(trimmed).isEmpty()) {
		return QString();
	}
	if (const auto fromLink = UsernameFromLink(trimmed); !fromLink.isEmpty()) {
		return fromLink;
	}
	static const auto valid = QRegularExpression(
		uR"(^@?([A-Za-z][A-Za-z0-9_]{3,31})$)"_s);
	const auto match = valid.match(trimmed);
	return match.hasMatch() ? match.captured(1) : QString();
}

bool IsPlaceholder(const QString &text) {
	// "⏳ Вычисляю…" - the first letter may come as a Latin look-alike.
	return text.contains(u"⏳"_s)
		|| Fold(text).contains(Fold(u"ычисля"_s));
}

QString CleanText(const QString &text) {
	auto lines = QStringList();
	for (auto line : text.split(u'\n')) {
		while (!line.isEmpty() && line.back().isSpace()) {
			line.chop(1);
		}
		if (line.trimmed().isEmpty()
			&& (lines.isEmpty() || lines.back().isEmpty())) {
			continue;
		}
		lines.push_back(line.trimmed().isEmpty() ? QString() : line);
	}
	return lines.join(u'\n').trimmed();
}

QString FormatCount(int value) {
	return QLocale(QLocale::Russian).toString(value);
}

QStringList PushRecent(QStringList list, const QString &query, int limit) {
	const auto trimmed = query.trimmed();
	list.erase(std::remove_if(list.begin(), list.end(), [&](
			const QString &entry) {
		return entry.compare(trimmed, Qt::CaseInsensitive) == 0;
	}), list.end());
	list.prepend(trimmed);
	return list.mid(0, limit);
}

std::vector<GroupRow> ExtractGroupRows(const Message &message) {
	auto result = std::vector<GroupRow>();
	const auto addLink = [&](const QString &shown, const QString &link) {
		const auto title = (shown == link) ? QString() : shown.trimmed();
		if (const auto username = UsernameFromLink(link); !username.isEmpty()) {
			result.push_back({ .username = username, .title = title });
		} else if (const auto closed = PrivateLink(link); !closed.isEmpty()) {
			result.push_back({ .title = title, .link = closed });
		}
		// Anything else (tg://user?id=... in the header, etc.) isn't a group.
	};
	for (const auto &entity : message.entities) {
		const auto shown = message.text.mid(entity.offset, entity.length);
		switch (entity.type) {
		case Entity::Type::Mention:
			result.push_back({ .username = shown.mid(1) });
			break;
		case Entity::Type::Url:
			addLink(shown, shown);
			break;
		case Entity::Type::CustomUrl:
			addLink(shown, entity.data);
			break;
		default:
			break;
		}
	}
	return result;
}

std::vector<UserRow> ExtractUserRows(const Message &message) {
	auto result = std::vector<UserRow>();
	for (const auto &entity : message.entities) {
		const auto shown = message.text.mid(entity.offset, entity.length);
		// The link's text is the person's name, unless it's a bare link.
		const auto name = (shown.startsWith(u'@')
			|| shown.contains(u"t.me/"_s))
			? QString()
			: shown.trimmed();
		if (entity.type == Entity::Type::MentionName) {
			result.push_back({ .mentionData = entity.data, .name = name });
		} else if (entity.type == Entity::Type::Url
			|| entity.type == Entity::Type::CustomUrl) {
			const auto username = UsernameFromLink(
				entity.type == Entity::Type::Url ? shown : entity.data);
			if (!username.isEmpty()) {
				result.push_back({ .username = username, .name = name });
			}
		}
	}
	return result;
}

std::vector<GroupRow> DedupeGroupRows(std::vector<GroupRow> rows) {
	auto seen = QSet<QString>();
	auto result = std::vector<GroupRow>();
	for (auto &row : rows) {
		// A closed chat's link carries a message id that changes as the
		// list shifts - the chat itself is t.me/c/<id>.
		static const auto closed = QRegularExpression(uR"(/c/(\d+))"_s);
		const auto chat = closed.match(row.link);
		const auto key = !row.username.isEmpty()
			? (u"@"_s + row.username.toLower())
			: chat.hasMatch()
			? (u"c/"_s + chat.captured(1))
			: row.link.toLower();
		if (key.isEmpty() || seen.contains(key)) {
			continue;
		}
		seen.insert(key);
		result.push_back(std::move(row));
	}
	return result;
}

QStringList DedupeUsernames(const QStringList &usernames) {
	auto seen = QSet<QString>();
	auto result = QStringList();
	for (const auto &raw : usernames) {
		auto name = raw.trimmed();
		if (name.startsWith(u'@')) {
			name.remove(0, 1);
		}
		const auto key = name.toLower();
		if (name.isEmpty()
			|| key.endsWith(u"bot"_s)
			|| seen.contains(key)) {
			continue;
		}
		seen.insert(key);
		result.push_back(name);
	}
	return result;
}

std::optional<UserCard> ParseUserCard(const Message &message) {
	const auto reader = Reader(message.text);
	const auto isGroup = reader.has(L(u"в топе"_s));
	const auto activity = reader.find(uR"((\d+)\s+)"_s
		+ L({ u"сообщени"_s, u"message"_s }) + uR"(\S*\s+)"_s
		+ L({ u"в"_s, u"in"_s }) + uR"(\s+(\d+)\s+)"_s
		+ L({ u"чат"_s, u"group"_s, u"chat"_s }));
	const auto style = reader.find(uR"(([\d.,]+\s*%)\s*)"_s
		+ L({ u"реплаи"_s, u"replies"_s }) + uR"(\s*([\d.,]+\s*%)\s*)"_s
		+ L({ u"медиа"_s, u"media"_s }));
	if (isGroup || (!activity && !style)) {
		return std::nullopt;
	}
	auto card = UserCard();
	if (const auto name = reader.find(NamePattern())) {
		card.name = name->value(0);
	}
	if (const auto id = reader.find(QString::fromUtf16(kIdPattern))) {
		card.id = id->value(0);
	}
	if (activity) {
		card.messages = ToInt(activity->value(0));
		card.chats = ToInt(activity->value(1));
	}
	if (style) {
		card.replies = style->value(0);
		card.media = style->value(1);
	}
	if (const auto diversity = reader.find(DiversityPattern())) {
		card.diversity = diversity->value(0);
	}
	if (const auto period = reader.find(uR"(^\s*)"_s
			+ L({ u"с"_s, u"from"_s })
			+ uR"(\s+(\d{1,2}[./]\d{1,2}[./]\d{4})\s+)"_s
			+ L({ u"по"_s, u"to"_s }) + uR"(\s+(.+)$)"_s)) {
		card.activeSince = period->value(0);
		card.activeUntil = period->value(1);
	}
	if (const auto rv = reader.find(L({ u"кружки"_s, u"circles"_s })
			+ uR"(\s*:\s*(\d+)\s*,\s*)"_s + L({ u"голос"_s, u"voice"_s })
			+ uR"(\S*\s*:\s*(\d+))"_s)) {
		card.roundVideos = ToInt(rv->value(0));
		card.voices = ToInt(rv->value(1));
	}
	if (const auto favorite = reader.find(
			L({ u"любимый чат"_s, u"favorite group"_s, u"favorite chat"_s })
				+ uR"(\s*:\s*(.+)$)"_s)) {
		card.favoriteChat = favorite->value(0);
	}
	if (const auto searched = reader.find(
			L({ u"искали"_s, u"were looking for"_s })
				+ uR"(\s*:\s*(\d+))"_s)) {
		card.searched = ToInt(searched->value(0));
	}
	if (const auto channel = reader.find(
			L({ u"канал"_s, u"channel"_s }) + uR"(\s*:\s*(.+)$)"_s)) {
		card.channel = channel->value(0);
	}

	auto namesAt = -1;
	const auto names = reader.find(
		uR"(^\s*)"_s + L({ u"имена"_s, u"first name"_s }),
		&namesAt);
	auto usernamesAt = -1;
	if (reader.find(L(u"usernames"_s), &usernamesAt)) {
		static const auto mention = QRegularExpression(
			uR"(@([A-Za-z0-9_]{4,32}))"_s);
		const auto last = std::min(
			usernamesAt + 1,
			(namesAt > usernamesAt) ? (namesAt - 1) : (reader.size() - 1));
		for (auto i = usernamesAt; i <= last; ++i) {
			auto it = mention.globalMatch(reader.original(i));
			while (it.hasNext()) {
				card.usernames.push_back(it.next().captured(1));
			}
		}
	}
	if (names) {
		static const auto change = QRegularExpression(
			uR"(^[\s├└│|]*(\d{4}-\d{2}-\d{2})\s*[➔→➜⮕>\-–—]*\x{FE0F}?\s*(.+?)\s*$)"_s);
		for (auto i = namesAt + 1; i < reader.size(); ++i) {
			const auto line = reader.original(i);
			const auto match = change.match(line);
			if (!match.hasMatch()) {
				if (line.trimmed().isEmpty()) {
					continue;
				}
				break;
			}
			card.names.push_back({ match.captured(1), match.captured(2) });
		}
	}
	return card;
}

QStringList FormatUserDetails(const UserCard &card) {
	auto result = QStringList();
	if (!card.activeSince.isEmpty()) {
		result.push_back(u"Активен с "_s + card.activeSince
			+ (card.activeUntil.isEmpty()
				? QString()
				: (u" по "_s + card.activeUntil)));
	}
	if (card.messages >= 0 && card.chats >= 0) {
		result.push_back(FormatCount(card.messages) + u" сообщ. в "_s
			+ FormatCount(card.chats) + u" чатах"_s);
	}
	if (!card.favoriteChat.isEmpty()) {
		result.push_back(u"Любимый чат: "_s + card.favoriteChat);
	}
	auto style = QStringList();
	if (!card.replies.isEmpty()) {
		style.push_back(u"реплаи "_s + card.replies);
	}
	if (!card.media.isEmpty()) {
		style.push_back(u"медиа "_s + card.media);
	}
	if (card.roundVideos >= 0) {
		style.push_back(u"кружки "_s + QString::number(card.roundVideos));
	}
	if (card.voices >= 0) {
		style.push_back(u"голос "_s + QString::number(card.voices));
	}
	if (!style.isEmpty()) {
		result.push_back(Capitalized(style.join(u" · "_s)));
	}
	if (!card.diversity.isEmpty()) {
		result.push_back(u"Разнообразие сообщений "_s + card.diversity);
	}
	auto extra = QStringList();
	if (!card.channel.isEmpty()) {
		extra.push_back(u"канал: "_s + card.channel);
	}
	if (card.searched >= 0) {
		extra.push_back(u"искали: "_s + QString::number(card.searched));
	}
	if (!extra.isEmpty()) {
		result.push_back(Capitalized(extra.join(u" · "_s)));
	}
	if (!card.usernames.isEmpty()) {
		auto handles = QStringList();
		for (const auto &name : card.usernames) {
			handles.push_back(u"@"_s + name);
		}
		result.push_back(u"Юзернеймы: "_s + handles.join(u' '));
	}
	return result;
}

std::optional<GroupCard> ParseGroupCard(const Message &message) {
	const auto reader = Reader(message.text);
	const auto top = reader.find(L(u"в топе"_s)
		+ uR"(\s*#\s*(\d+)\s*)"_s + L(u"из"_s) + uR"(\s*(\d+))"_s);
	if (!top && !reader.has(L(u"за сутки"_s))) {
		return std::nullopt;
	}
	auto card = GroupCard();
	if (top) {
		card.topPlace = ToInt(top->value(0));
		card.topOf = ToInt(top->value(1));
	}
	if (const auto name = reader.find(NamePattern())) {
		card.name = name->value(0);
	}
	if (const auto id = reader.find(QString::fromUtf16(kIdPattern))) {
		card.id = id->value(0);
	}
	if (const auto diversity = reader.find(DiversityPattern())) {
		card.diversity = diversity->value(0);
	}
	if (const auto people = reader.find(uR"((\d+)\s*)"_s
			+ L(u"челов"_s) + uR"(\S*\s*)"_s + L(u"за сутки"_s))) {
		card.peopleDay = ToInt(people->value(0));
	}
	if (const auto written = reader.find(L(u"написали"_s)
			+ uR"(\s*(\d+)\s*)"_s + L(u"сообщ"_s))) {
		card.messagesDay = ToInt(written->value(0));
	}
	if (const auto media = reader.find(L(u"из них"_s)
			+ uR"(\s*(\d+)\s*\(([\d.,]+\s*%)\)\s*)"_s + L(u"медиа"_s))) {
		card.mediaDay = ToInt(media->value(0));
		card.mediaDayPercent = media->value(1);
	}
	if (const auto rv = reader.find(uR"((\d+)\s*)"_s + L(u"кружк"_s)
			+ uR"(\S*\s*)"_s + L(u"и"_s) + uR"(\s*(\d+)\s*)"_s
			+ L(u"голос"_s))) {
		card.roundVideosDay = ToInt(rv->value(0));
		card.voicesDay = ToInt(rv->value(1));
	}

	auto topAt = -1;
	const auto topDay = reader.find(
		uR"(^\s*)"_s + L(u"топ за сутки"_s),
		&topAt);
	auto descriptionAt = -1;
	if (const auto description = reader.find(
			uR"(^\s*)"_s + L(u"описание"_s) + uR"(\s*:(.*)$)"_s,
			&descriptionAt)) {
		static const auto filler = QRegularExpression(uR"(^[\s\-–—]*$)"_s);
		auto lines = QStringList();
		if (!description->value(0).isEmpty()) {
			lines.push_back(description->value(0));
		}
		const auto end = (topAt > descriptionAt) ? topAt : reader.size();
		for (auto i = descriptionAt + 1; i < end; ++i) {
			const auto line = reader.original(i).trimmed();
			if (!filler.match(line).hasMatch()) {
				lines.push_back(line);
			}
		}
		card.description = lines.join(u'\n').trimmed();
	}
	if (topDay) {
		static const auto row = QRegularExpression(
			uR"(^\s*(\d+)\.\s*(.+?)\s*[-–—]\s*(\d+)\s*$)"_s);
		for (auto i = topAt + 1; i < reader.size(); ++i) {
			const auto line = reader.original(i);
			const auto match = row.match(line);
			if (!match.hasMatch()) {
				if (line.trimmed().isEmpty()) {
					continue;
				}
				break;
			}
			card.topDay.push_back({
				match.captured(2),
				match.captured(3).toInt(),
			});
		}
	}
	return card;
}

QStringList FormatGroupDetails(const GroupCard &card) {
	auto result = QStringList();
	if (card.topPlace >= 0) {
		result.push_back(u"В топе #"_s + FormatCount(card.topPlace)
			+ (card.topOf >= 0
				? (u" из "_s + FormatCount(card.topOf))
				: QString()));
	}
	auto day = QStringList();
	if (card.peopleDay >= 0) {
		day.push_back(FormatCount(card.peopleDay) + u" человек"_s);
	}
	if (card.messagesDay >= 0) {
		day.push_back(FormatCount(card.messagesDay) + u" сообщ."_s);
	}
	if (card.mediaDay >= 0) {
		day.push_back(u"медиа "_s + FormatCount(card.mediaDay)
			+ (card.mediaDayPercent.isEmpty()
				? QString()
				: (u" ("_s + card.mediaDayPercent + u')')));
	}
	if (!day.isEmpty()) {
		result.push_back(u"За сутки: "_s + day.join(u" · "_s));
	}
	auto voice = QStringList();
	if (card.roundVideosDay >= 0) {
		voice.push_back(u"кружки "_s + QString::number(card.roundVideosDay));
	}
	if (card.voicesDay >= 0) {
		voice.push_back(u"голосовые "_s + QString::number(card.voicesDay));
	}
	if (!voice.isEmpty()) {
		result.push_back(Capitalized(voice.join(u" · "_s)));
	}
	if (!card.diversity.isEmpty()) {
		result.push_back(u"Разнообразие сообщений "_s + card.diversity);
	}
	return result;
}

} // namespace Window::Squidward
