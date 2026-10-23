/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and exceptions, see https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "core/ggtg_ai.h"
#include "core/ggtg_ai_embed.h"
#include "core/ggtg_ai_knowledge.h"

// The profile: everything the AI knows about the user and the way they
// promote their own thing, in one place that every helper reads (the chat
// assistant, the template assistant, the agent) and every learning path
// writes to (the interview, the remarks, "Научиться на переписке", the
// agent's talks). It is kept in tdata/ggtg_db/ai_profile.json in sections of
// their own with limits of their own, so the facts about the business are
// never pushed out by the rules of style (and the other way round):
//
//   {"v":1,"sections":{"business":[{"t":"Курс по трейдингу, 300$","at":1791000000}],...}}
//
// The first read turns the old files (ai_learned.json, the agent's notes in
// ai_agent.json) into a profile; they are left where they are, but not read
// any more.
namespace Core::GgtgAi::Profile {

enum class Section {
	Business,
	Goal,
	Audience,
	Style,
	Rules,
	Works,
	NotWorks,
};
constexpr auto kSections = 7;

struct SectionInfo {
	QString key;
	QString title; // in the editor and in the prompts
	QString tag; // what the model writes before a fact
	QString hint; // told to the model
	int maxItems = 10;
};

[[nodiscard]] inline const SectionInfo &Info(Section section) {
	static const auto list = std::vector<SectionInfo>{
		{
			u"business"_q,
			u"Дело и оффер"_q,
			u"дело"_q,
			u"чем занимается пользователь, что и на каких условиях "
				"предлагает (цены, ссылки, результат для клиента)"_q,
			10,
		},
		{
			u"goal"_q,
			u"Цель диалога"_q,
			u"цель"_q,
			u"к чему вести собеседника и что считается успехом"_q,
			5,
		},
		{
			u"audience"_q,
			u"Аудитория"_q,
			u"аудитория"_q,
			u"кому пишет: страны, языки, ниша, интересы, возраст"_q,
			8,
		},
		{
			u"style"_q,
			u"Стиль"_q,
			u"стиль"_q,
			u"как пишет пользователь: тон, «ты» или «вы», длина, эмодзи, "
				"обороты"_q,
			12,
		},
		{
			u"rules"_q,
			u"Правила и запреты"_q,
			u"правило"_q,
			u"что можно и нельзя говорить, пожелания и замечания"_q,
			12,
		},
		{
			u"works"_q,
			u"Что работает"_q,
			u"работает"_q,
			u"что подтверждённо давало ответы и результат (формулировка, "
				"время, длина, ход разговора)"_q,
			15,
		},
		{
			u"notworks"_q,
			u"Что не работает"_q,
			u"не работает"_q,
			u"после чего молчали, отказывали или банили"_q,
			15,
		},
	};
	return list[int(section)];
}

struct Entry {
	QString text;
	qint64 at = 0;
};

struct Data {
	std::vector<Entry> sections[kSections];

	[[nodiscard]] int count() const {
		auto result = 0;
		for (const auto &list : sections) {
			result += int(list.size());
		}
		return result;
	}
	[[nodiscard]] bool empty() const {
		return count() == 0;
	}
};

[[nodiscard]] inline QString Path() {
	return Core::GgtgPaths::DbDir() + u"ai_profile.json"_q;
}

namespace details {

// One fact: a line of text, no list marks, at most 220 characters.
[[nodiscard]] inline QString Clean(QString text) {
	text = text.simplified();
	while (text.startsWith('-') || text.startsWith('*')
		|| text.startsWith(QChar(0x2022))) {
		text = text.mid(1).trimmed();
	}
	if (text.size() > 220) {
		text = text.left(220).trimmed();
	}
	return text;
}

// Letters and digits only: "Цена: 300$." and "цена 300" are one fact.
[[nodiscard]] inline QString SimpleKey(const QString &text) {
	static const auto re = QRegularExpression(u"[^\\p{L}\\p{N}]"_q);
	auto result = text.toLower();
	result.remove(re);
	return result;
}

[[nodiscard]] inline bool Similar(const QString &a, const QString &b) {
	const auto ka = SimpleKey(a);
	const auto kb = SimpleKey(b);
	if (ka == kb) {
		return true;
	}
	return std::min(ka.size(), kb.size()) >= 14
		&& (ka.contains(kb) || kb.contains(ka));
}

[[nodiscard]] inline std::optional<Section> SectionByTag(QString tag) {
	tag = tag.toLower().simplified();
	tag.remove('*');
	tag.remove('`');
	tag = tag.trimmed();
	if (tag == u"не работает"_q || tag == u"неработает"_q) {
		return Section::NotWorks;
	}
	if (tag == u"работает"_q) {
		return Section::Works;
	}
	if (tag == u"дело"_q || tag == u"оффер"_q || tag == u"продукт"_q
		|| tag == u"бизнес"_q) {
		return Section::Business;
	}
	if (tag == u"цель"_q) {
		return Section::Goal;
	}
	if (tag == u"аудитория"_q) {
		return Section::Audience;
	}
	if (tag == u"стиль"_q) {
		return Section::Style;
	}
	if (tag == u"правило"_q || tag == u"правила"_q || tag == u"запрет"_q
		|| tag == u"запреты"_q) {
		return Section::Rules;
	}
	return std::nullopt;
}

[[nodiscard]] inline std::optional<Section> SectionByTitle(QString title) {
	title = title.toLower().simplified();
	for (auto i = 0; i != kSections; ++i) {
		const auto section = Section(i);
		if (title == Info(section).title.toLower()) {
			return section;
		}
	}
	return SectionByTag(title);
}

} // namespace details

// Puts a fact into a section. A fact that says the same as one already there
// replaces it (the newer wording wins and becomes the newest); past the
// section's limit the oldest facts go. Returns true if something new (or
// reworded) was added.
inline bool Add(Data &data, Section section, QString text, qint64 now) {
	text = details::Clean(text);
	if (text.size() < 4) {
		return false;
	}
	auto &list = data.sections[int(section)];
	for (auto i = 0; i != int(list.size()); ++i) {
		if (details::Similar(list[i].text, text)) {
			const auto same = (list[i].text == text);
			list.erase(list.begin() + i);
			list.push_back({ text, now });
			return !same;
		}
	}
	list.push_back({ text, now });
	while (int(list.size()) > Info(section).maxItems) {
		list.erase(list.begin());
	}
	return true;
}

namespace details {

// The old rules ("Стиль: ...", "Работает: ...", free ones) as profile entries.
inline void MigrateRule(Data &data, QString note, qint64 now) {
	note = note.trimmed();
	struct Prefix {
		QString text;
		Section section;
	};
	static const auto prefixes = std::vector<Prefix>{
		{ u"Стиль:"_q, Section::Style },
		{ u"Не работает:"_q, Section::NotWorks },
		{ u"Работает:"_q, Section::Works },
		{ u"Людям нравится:"_q, Section::Works },
	};
	for (const auto &prefix : prefixes) {
		if (note.startsWith(prefix.text, Qt::CaseInsensitive)) {
			Add(data, prefix.section, note.mid(prefix.text.size()), now);
			return;
		}
	}
	Add(data, Section::Rules, note, now);
}

[[nodiscard]] inline QStringList NotesOf(const QString &path) {
	auto result = QStringList();
	auto file = QFile(path);
	if (!file.open(QIODevice::ReadOnly)) {
		return result;
	}
	const auto array = QJsonDocument::fromJson(file.readAll()).object()
		.value("notes").toArray();
	for (const auto &value : array) {
		if (!value.toString().trimmed().isEmpty()) {
			result.push_back(value.toString().trimmed());
		}
	}
	return result;
}

[[nodiscard]] inline Data Migrated() {
	auto result = Data();
	const auto now = qint64(QDateTime::currentSecsSinceEpoch());
	for (const auto &note : NotesOf(
			Core::GgtgPaths::DbDir() + u"ai_learned.json"_q)) {
		MigrateRule(result, note, now);
	}
	for (const auto &note : NotesOf(
			Core::GgtgPaths::DbDir() + u"ai_agent.json"_q)) {
		Add(
			result,
			note.startsWith(u"Цель"_q, Qt::CaseInsensitive)
				? Section::Goal
				: Section::Business,
			note,
			now);
	}
	return result;
}

} // namespace details

inline void Write(const Data &data) {
	auto sections = QJsonObject();
	for (auto i = 0; i != kSections; ++i) {
		auto array = QJsonArray();
		for (const auto &entry : data.sections[i]) {
			array.push_back(QJsonObject{
				{ "t", entry.text },
				{ "at", double(entry.at) },
			});
		}
		sections.insert(Info(Section(i)).key, array);
	}
	QDir().mkpath(Core::GgtgPaths::DbDir());
	auto file = QSaveFile(Path());
	if (file.open(QIODevice::WriteOnly)) {
		file.write(QJsonDocument(QJsonObject{
			{ "v", 1 },
			{ "sections", sections },
		}).toJson(QJsonDocument::Compact));
		file.commit();
	}
}

[[nodiscard]] inline Data Read() {
	auto file = QFile(Path());
	if (!file.open(QIODevice::ReadOnly)) {
		auto result = details::Migrated();
		if (!result.empty()) {
			Write(result);
		}
		return result;
	}
	auto result = Data();
	const auto sections = QJsonDocument::fromJson(file.readAll()).object()
		.value("sections").toObject();
	for (auto i = 0; i != kSections; ++i) {
		const auto array = sections.value(Info(Section(i)).key).toArray();
		for (const auto &value : array) {
			const auto object = value.toObject();
			const auto text = details::Clean(object.value("t").toString());
			if (text.size() >= 4) {
				result.sections[i].push_back({
					text,
					qint64(object.value("at").toDouble()),
				});
			}
		}
	}
	return result;
}

// What goes into a prompt: the sections with their facts, the newest facts
// first when they do not all fit into `limit` characters. A section cannot
// take more than a third of the room, so none starves the others; the order
// of importance is the business, the goal, the audience, the rules, what
// works, what does not, the style.
// Words of a text reduced to their first five letters (so "цена", "цены",
// "ценой" meet), for telling which facts matter for a given question.
[[nodiscard]] inline std::set<QString> StemSet(const QString &text) {
	auto result = std::set<QString>();
	static const auto word = QRegularExpression(u"[\\p{L}\\p{N}]{4,}"_q);
	auto it = word.globalMatch(text.toLower());
	while (it.hasNext()) {
		result.insert(it.next().captured(0).left(5));
	}
	return result;
}

// With a `query` (what the person just wrote, the draft...), the facts of a
// section that share words with it are taken first when not all of them fit;
// without one, the newest first.
[[nodiscard]] inline QString Block(int limit, const QString &query = QString()) {
	const auto data = Read();
	if (data.empty()) {
		return QString();
	}
	const auto wanted = StemSet(query);
	// With the meaning search installed and the query's vector made (see
	// WarmFor), facts are ranked by meaning; otherwise by shared words.
	const auto *queryVector = query.trimmed().isEmpty()
		? nullptr
		: Embed::Cached(query);
	const auto priority = std::vector<Section>{
		Section::Business,
		Section::Goal,
		Section::Audience,
		Section::Rules,
		Section::Works,
		Section::NotWorks,
		Section::Style,
	};
	auto remaining = limit;
	const auto perSection = std::max(300, limit / 3);
	auto take = std::vector<std::vector<int>>(kSections);
	for (const auto section : priority) {
		const auto &list = data.sections[int(section)];
		auto used = 0;
		// Newest first; with a query, the ones that match it go before the
		// rest (the order inside each group stays newest first).
		auto order = std::vector<int>();
		for (auto i = int(list.size()); i != 0; --i) {
			order.push_back(i - 1);
		}
		if (!wanted.empty() || queryVector) {
			const auto score = [&](int index) {
				if (queryVector) {
					if (const auto vector = Embed::Cached(list[index].text)) {
						return Embed::Similarity(*queryVector, *vector);
					}
				}
				auto hits = 0;
				for (const auto &stem : StemSet(list[index].text)) {
					hits += wanted.contains(stem) ? 1 : 0;
				}
				return hits * 0.1;
			};
			std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
				return score(a) > score(b);
			});
		}
		for (const auto index : order) {
			const auto cost = int(list[index].text.size()) + 3;
			if (cost > remaining || used + cost > perSection) {
				continue;
			}
			take[int(section)].push_back(index);
			remaining -= cost;
			used += cost;
		}
	}
	auto result = QString();
	for (auto i = 0; i != kSections; ++i) {
		if (take[i].empty()) {
			continue;
		}
		result += Info(Section(i)).title + u":\n"_q;
		for (auto j = int(take[i].size()); j != 0; --j) {
			result += u"• "_q + data.sections[i][take[i][j - 1]].text + '\n';
		}
	}
	return result;
}

// Gets the meaning vectors of the query, of every fact and of every chunk of
// the "Материалы" base ready, so that Block(limit, query) can rank by
// meaning, then calls `done`. Never waits
// long (eight seconds at most: the vectors that are not ready yet simply
// are not used this time); without a Gemini key it calls `done` at once.
inline void WarmFor(const QString &query, Fn<void()> done) {
	if (!Embed::Available() || query.trimmed().isEmpty()) {
		done();
		return;
	}
	auto texts = QStringList{ query };
	texts.append(Knowledge::AllChunks()); // the base of "Материалы"
	const auto data = Read();
	for (const auto &section : data.sections) {
		for (const auto &entry : section) {
			texts.push_back(entry.text);
		}
	}
	const auto finished = std::make_shared<bool>(false);
	const auto finish = [=] {
		if (!std::exchange(*finished, true)) {
			done();
		}
	};
	QTimer::singleShot(8000, qApp, finish);
	Embed::Prepare(texts, [=](bool) { finish(); });
}

// The profile for the model to look at, each fact with its number in the
// section (the model may ask to remove a fact by it).
[[nodiscard]] inline QString Numbered(const Data &data) {
	if (data.empty()) {
		return u"(пока пусто)"_q;
	}
	auto result = QString();
	for (auto i = 0; i != kSections; ++i) {
		if (data.sections[i].empty()) {
			continue;
		}
		result += Info(Section(i)).tag + u":\n"_q;
		for (auto j = 0; j != int(data.sections[i].size()); ++j) {
			result += u"  "_q + QString::number(j + 1) + u". "_q
				+ data.sections[i][j].text + '\n';
		}
	}
	return result;
}

// The editor's text: a "## Title" line for each section, then "- fact" lines
// (older ones above).
[[nodiscard]] inline QString ToText(const Data &data) {
	auto result = QString();
	for (auto i = 0; i != kSections; ++i) {
		result += u"## "_q + Info(Section(i)).title + '\n';
		for (const auto &entry : data.sections[i]) {
			result += u"- "_q + entry.text + '\n';
		}
		result += '\n';
	}
	return result.trimmed();
}

// The editor's text back into a profile; a fact that stayed as it was keeps
// its date. Nothing when the text has no "## Title" line at all.
[[nodiscard]] inline std::optional<Data> FromText(
		const QString &text,
		const Data &old) {
	auto result = Data();
	auto current = std::optional<Section>();
	auto headers = 0;
	const auto now = qint64(QDateTime::currentSecsSinceEpoch());
	for (auto line : text.split('\n')) {
		line = line.trimmed();
		if (line.isEmpty()) {
			continue;
		}
		if (line.startsWith(u"##"_q)) {
			current = details::SectionByTitle(line.mid(2));
			++headers;
			continue;
		}
		if (!current) {
			continue;
		}
		const auto fact = details::Clean(line);
		if (fact.size() < 4) {
			continue;
		}
		auto at = now;
		for (const auto &entry : old.sections[int(*current)]) {
			if (entry.text == fact) {
				at = entry.at;
				break;
			}
		}
		auto &list = result.sections[int(*current)];
		auto duplicate = false;
		for (const auto &entry : list) {
			duplicate = duplicate || (entry.text == fact);
		}
		if (!duplicate) {
			list.push_back({ fact, at });
		}
	}
	if (!headers) {
		return std::nullopt;
	}
	for (auto i = 0; i != kSections; ++i) {
		auto &list = result.sections[i];
		const auto limit = Info(Section(i)).maxItems;
		if (int(list.size()) > limit) {
			list.erase(list.begin(), list.end() - limit);
		}
	}
	return result;
}

struct Change {
	int added = 0;
	int removed = 0;
	QStringList lines; // "тег: факт" for each fact that was added
};

// The answer of the model - "+ тег: факт" to add, "- тег N" to remove the
// fact number N of that section as it was listed in `snapshot` - applied to
// the profile as it is now. Only the sections in `allowed` are touched.
[[nodiscard]] inline Change Apply(
		const QString &output,
		const Data &snapshot,
		const std::vector<Section> &allowed) {
	static const auto lineRe = QRegularExpression(
		u"^\\s*([+~\\-\\x{2212}\\x{2013}])\\s*(.+?)\\s*$"_q);
	static const auto replaceRe = QRegularExpression(
		u"^(.+?)\\s*(?:№|#)?\\s*(\\d+)\\s*[:\\x{FF1A}]\\s*(.+)$"_q);
	static const auto removeRe = QRegularExpression(
		u"^(.+?)\\s*(?:№|#)?\\s*(\\d+)\\s*$"_q);
	const auto isAllowed = [&](Section section) {
		return std::find(allowed.begin(), allowed.end(), section)
			!= allowed.end();
	};
	struct Replace {
		Section section;
		int number = 0;
		QString text;
	};
	auto adds = std::vector<std::pair<Section, QString>>();
	auto removes = std::vector<std::pair<Section, int>>();
	auto replaces = std::vector<Replace>();
	for (auto line : output.split('\n')) {
		line.remove('*');
		line.remove('`');
		const auto match = lineRe.match(line);
		if (!match.hasMatch()) {
			continue;
		}
		const auto body = match.captured(2);
		if (match.captured(1) == u"~"_q) {
			const auto parts = replaceRe.match(body);
			if (!parts.hasMatch()) {
				continue;
			}
			const auto section = details::SectionByTag(parts.captured(1));
			const auto fact = details::Clean(parts.captured(3));
			if (section && isAllowed(*section) && fact.size() >= 4) {
				replaces.push_back({
					*section,
					parts.captured(2).toInt(),
					fact,
				});
			}
		} else if (match.captured(1) == u"+"_q) {
			const auto colon = body.indexOf(':');
			if (colon <= 0) {
				continue;
			}
			const auto section = details::SectionByTag(body.left(colon));
			const auto fact = details::Clean(body.mid(colon + 1));
			if (section && isAllowed(*section) && fact.size() >= 4
				&& !fact.startsWith(u"(нет"_q, Qt::CaseInsensitive)) {
				adds.push_back({ *section, fact });
			}
		} else {
			const auto number = removeRe.match(body);
			if (!number.hasMatch()) {
				continue;
			}
			const auto section = details::SectionByTag(number.captured(1));
			if (section && isAllowed(*section)) {
				removes.push_back({ *section, number.captured(2).toInt() });
			}
		}
	}
	auto result = Change();
	auto data = Read();
	const auto now = qint64(QDateTime::currentSecsSinceEpoch());
	// A fact that changed: the old one goes, the new one takes its place as
	// the newest.
	for (const auto &replace : replaces) {
		const auto &old = snapshot.sections[int(replace.section)];
		if (replace.number < 1 || replace.number > int(old.size())) {
			continue;
		}
		const auto &was = old[replace.number - 1].text;
		if (was == replace.text) {
			continue;
		}
		auto &list = data.sections[int(replace.section)];
		for (auto i = 0; i != int(list.size()); ++i) {
			if (list[i].text == was) {
				list.erase(list.begin() + i);
				break;
			}
		}
		if (Add(data, replace.section, replace.text, now)) {
			++result.added;
			result.lines.push_back(
				Info(replace.section).tag + u": "_q + replace.text);
		}
	}
	for (const auto &[section, number] : removes) {
		const auto &old = snapshot.sections[int(section)];
		if (number < 1 || number > int(old.size())) {
			continue;
		}
		auto &list = data.sections[int(section)];
		for (auto i = 0; i != int(list.size()); ++i) {
			if (list[i].text == old[number - 1].text) {
				list.erase(list.begin() + i);
				++result.removed;
				break;
			}
		}
	}
	for (const auto &[section, fact] : adds) {
		if (Add(data, section, fact, now)) {
			++result.added;
			result.lines.push_back(Info(section).tag + u": "_q + fact);
		}
	}
	if (result.added || result.removed) {
		Write(data);
	}
	return result;
}

[[nodiscard]] inline QString ExtractionSystem(
		const QString &task,
		const std::vector<Section> &allowed) {
	auto tags = QString();
	for (const auto section : allowed) {
		tags += u"• "_q + Info(section).tag + u" - "_q
			+ Info(section).hint + '\n';
	}
	return task
		+ u"\n\nОтвет - строки, по одной на пункт:\n"
			"+ <тег>: <новый факт, до 160 символов>   (добавить)\n"
			"~ <тег> <номер>: <новый текст пункта>   (пункт «Профиля "
			"сейчас» устарел или изменился: замени его новым текстом)\n"
			"- <тег> <номер>   (пункт неверен или повторяется: убрать)\n"
			"Теги:\n"_q + tags
		+ u"Только то, что прямо сказано или видно в материале: не "
			"выдумывай. Не записывай имена людей и номера телефонов (у "
			"пользователя несколько аккаунтов, а собеседники у каждого "
			"свои). Не повторяй то, что уже есть в профиле. Если материал "
			"меняет или опровергает пункт профиля (цена, условия, правило), "
			"исправь именно этот пункт по его номеру, а не добавляй новый "
			"рядом. Остальные пункты не трогай. Если нового нет, ответь "
			"одним словом НЕТ. Без вступлений и пояснений.\n\n"
			"Пример (другое дело, только для формата).\n"
			"Профиль сейчас:\nдело:\n  1. Ремонт квартир, от 100$ за м².\n"
			"Материал: «Пользователь: Теперь работаем в Дубае, цена от 120$ "
			"за м².»\n"
			"Ответ:\n~ дело 1: Ремонт квартир, от 120$ за м².\n"
			"+ аудитория: Дубай."_q;
}

// One round of learning: the model reads `material` next to the profile and
// says what to add and what to remove; the change is applied. `done` gets
// what changed (nothing is not an error).
inline void Extract(
		const QString &task,
		const std::vector<Section> &allowed,
		const QString &material,
		Fn<void(Change change, QString error)> done,
		int maxTokens = 500) {
	const auto snapshot = Read();
	Core::GgtgAi::Ask(
		ExtractionSystem(task, allowed),
		u"Профиль сейчас:\n"_q + Numbered(snapshot)
			+ u"\n\nМатериал:\n"_q + material,
		[=](QString result, QString error) {
			if (!error.isEmpty()) {
				done(Change(), error);
				return;
			}
			if (result.trimmed().size() < 12
				&& result.trimmed().startsWith(
					u"НЕТ"_q,
					Qt::CaseInsensitive)) {
				done(Change(), QString());
				return;
			}
			done(Apply(result, snapshot, allowed), QString());
		},
		0.1,
		maxTokens,
		Core::GgtgAi::Role::General,
		QStringList(),
		true); // What the AI knows about the user: the cleverest model.
}

// The user's own words (an interview, remarks) into the profile. `dialog`
// has lines "Пользователь: ..." and "ИИ: ...": only the user's lines are
// facts, the AI's lines are its questions.
inline void LearnFromDialog(
		const QString &dialog,
		Fn<void(Change change, QString error)> done) {
	if (!dialog.contains(u"Пользователь:"_q)) {
		done(Change(), QString());
		return;
	}
	Extract(
		u"Ты ведёшь профиль пользователя программы для рассылок и "
			"переписок в Telegram: чтобы помощник писал за него и помогал "
			"продвигать его дело. Тебе дают профиль и диалог. Факты бери "
			"ТОЛЬКО из реплик «Пользователь:»; реплики «ИИ:» - это "
			"вопросы, из них нельзя брать ничего."_q,
		{
			Section::Business,
			Section::Goal,
			Section::Audience,
			Section::Style,
			Section::Rules,
		},
		dialog,
		std::move(done));
}

// The user's message to the agent: what it says about their business.
inline void LearnFromMessage(
		const QString &message,
		Fn<void(Change change, QString error)> done) {
	if (message.trimmed().size() < 15) {
		done(Change(), QString());
		return;
	}
	Extract(
		u"Ты ведёшь профиль пользователя программы для рассылок в "
			"Telegram. Тебе дают профиль и новое сообщение пользователя. "
			"Выпиши долговременные факты о нём и его деле, которые он сам "
			"сказал."_q,
		{
			Section::Business,
			Section::Goal,
			Section::Audience,
			Section::Style,
			Section::Rules,
			Section::Works,
			Section::NotWorks,
		},
		message.left(1500),
		std::move(done),
		300);
}

// Merges repeats and settles contradictions with the model's help; the
// profile before is kept in ai_profile.json.backup. `done(before, after,
// error)`: how many facts there were and are.
inline void Consolidate(Fn<void(int before, int after, QString error)> done) {
	const auto old = Read();
	if (old.count() < 3) {
		done(old.count(), old.count(), QString());
		return;
	}
	Core::GgtgAi::Ask(
		u"Ты наводишь порядок в профиле пользователя. Объедини "
			"повторяющиеся и близкие по смыслу пункты в один, убери "
			"устаревшее: если пункты противоречат друг другу, оставь "
			"нижний (в каждом разделе новее те, что ниже). Ничего не "
			"добавляй от себя, не меняй смысл, не переноси пункты между "
			"разделами. Верни профиль в том же виде: строка «## Раздел», "
			"под ней пункты со знака «- ». Все разделы оставь, даже "
			"пустые. Без пояснений."_q,
		ToText(old),
		[=](QString result, QString error) {
			if (!error.isEmpty()) {
				done(old.count(), old.count(), error);
				return;
			}
			const auto merged = FromText(result, old);
			if (!merged || merged->empty()) {
				done(
					old.count(),
					old.count(),
					u"модель вернула профиль в неверном виде, он не "
					"изменён"_q);
				return;
			}
			const auto backup = Path() + u".backup"_q;
			QFile::remove(backup);
			QFile::copy(Path(), backup);
			Write(*merged);
			done(old.count(), merged->count(), QString());
		},
		0.1,
		1500,
		Core::GgtgAi::Role::General,
		QStringList(),
		true);
}

// Forgets the profile and the examples of the user's style. An empty profile
// file stops the migration from the old files from running again.
inline void Forget() {
	QFile::remove(Path() + u".backup"_q);
	QFile::remove(ExamplesPath());
	Write(Data());
}

} // namespace Core::GgtgAi::Profile
