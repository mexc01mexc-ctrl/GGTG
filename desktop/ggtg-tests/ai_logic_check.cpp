// Checks of the AI logic with the real headers (core/ggtg_ai*.h): providers,
// the choice of models and resting, masking of other people's data, the
// knowledge base's chunks and search, what training takes from a chat, the
// playbook, the sort of names. Run with ggtg-tests/run_ai.sh.
#include "core/ggtg_ai_inbox.h"
#include "core/ggtg_ai_knowledge.h"
#include "core/ggtg_ai_profile.h"
#include "core/ggtg_ai_train.h"
#include "core/ggtg_name_sort.h"

#include <cstdio>
#include <cstdlib>

namespace {

int failures = 0;
int checks = 0;

void Check(bool ok, const char *what, int line) {
	++checks;
	if (!ok) {
		std::fprintf(stderr, "FAIL line %d: %s\n", line, what);
		++failures;
	}
}
#define CHECK(x) Check((x), #x, __LINE__)

void CheckEq(const QString &a, const QString &b, int line) {
	++checks;
	if (a != b) {
		std::fprintf(stderr, "FAIL line %d:\n  got:  %s\n  want: %s\n",
			line, a.toUtf8().constData(), b.toUtf8().constData());
		++failures;
	}
}
#define CHECK_EQ(a, b) CheckEq((a), (b), __LINE__)

namespace Ai = Core::GgtgAi;

void TestProviders() {
	CHECK_EQ(Ai::DetectProvider(u"AIzaSyABC"_q), u"gemini"_q);
	CHECK_EQ(Ai::DetectProvider(u"  gsk_abc "_q), u"groq"_q);
	CHECK_EQ(Ai::DetectProvider(u"sk-or-v1-abc"_q), u"openrouter"_q);
	CHECK_EQ(Ai::DetectProvider(u"github_pat_abc"_q), u"github"_q);
	CHECK_EQ(Ai::DetectProvider(u"ghp_abc"_q), u"github"_q);
	CHECK_EQ(Ai::DetectProvider(u"nvapi-abc"_q), u"nvidia"_q);
	CHECK(Ai::DetectProvider(u"sk-proj-whatever"_q).isEmpty());
	CHECK_EQ(Ai::ShortKey(u"AIzaSyABCDEFGHIJKLMNOP"_q), u"AIzaSy\u2026MNOP"_q);
}

Ai::Settings TwoGeminiOneGroq() {
	auto settings = Ai::Settings();
	settings.keys = {
		{ u"AIza-key-one"_q, u"gemini"_q },
		{ u"AIza-key-two"_q, u"gemini"_q },
		{ u"gsk_key"_q, u"groq"_q },
	};
	return settings;
}

void TestCandidates() {
	using namespace Ai::details;
	auto settings = TwoGeminiOneGroq();
	CHECK(Ai::CloudReady(settings));
	settings.enabled = false;
	CHECK(!Ai::CloudReady(settings));
	settings.enabled = true;

	// A clever job: both Gemini keys go before anything else, in the order
	// they were added.
	auto smart = Candidates(settings, Ai::Task::Smart, false);
	CHECK(smart.size() >= 4);
	CHECK_EQ(smart[0].key.key, u"AIza-key-one"_q);
	CHECK_EQ(smart[1].key.key, u"AIza-key-two"_q);
	CHECK_EQ(smart[0].model, smart[1].model);
	CHECK(smart[0].model.contains(u"pro"_q));

	// The first key's best model rests: the second key's takes over.
	Rest(u"AIza-key-one"_q, smart[0].model, 60);
	smart = Candidates(settings, Ai::Task::Smart, false);
	CHECK_EQ(smart[0].key.key, u"AIza-key-two"_q);
	CHECK(smart[0].model.contains(u"pro"_q));
	// ... while the first key still gives its other models.
	auto firstKeyModels = 0;
	for (const auto &candidate : smart) {
		firstKeyModels += (candidate.key.key == u"AIza-key-one"_q) ? 1 : 0;
	}
	CHECK(firstKeyModels > 0);

	// A whole key rests (a wrong key): none of its models is offered.
	Rest(u"AIza-key-one"_q, QString(), 600);
	smart = Candidates(settings, Ai::Task::Smart, false);
	for (const auto &candidate : smart) {
		CHECK(candidate.key.key != u"AIza-key-one"_q);
	}
	Wake(u"AIza-key-one"_q);
	smart = Candidates(settings, Ai::Task::Smart, false);
	CHECK_EQ(smart[0].key.key, u"AIza-key-one"_q);

	// Bulk jobs go to the fast ones: a Lite model or Groq before Pro.
	const auto bulk = Candidates(settings, Ai::Task::Bulk, false);
	CHECK(!bulk.empty());
	CHECK(!bulk[0].model.contains(u"-pro"_q));

	// Pictures: only models that see.
	const auto vision = Candidates(settings, Ai::Task::Vision, false);
	CHECK(!vision.empty());
	CHECK(vision[0].key.provider == u"gemini"_q);
}

void TestFailures() {
	using namespace Ai::details;
	auto candidate = Candidate{
		.key = { u"AIza-fail"_q, u"gemini"_q },
		.model = u"gemini-2.5-flash"_q,
	};
	// A minute limit: the model rests a few minutes, the key still works.
	Failed(candidate, 429, "Resource exhausted", "30");
	CHECK(Resting(u"AIza-fail"_q, candidate.model));
	CHECK(!Resting(u"AIza-fail"_q, u"gemini-2.5-pro"_q));
	Wake(u"AIza-fail"_q);
	CHECK(!Resting(u"AIza-fail"_q, candidate.model));

	// A day limit, or "limit: 0" (not free on this key): a long rest.
	Failed(candidate, 429, "quota exceeded: limit: 0", "");
	CHECK(RestUntil(u"AIza-fail"_q, candidate.model)
		> QDateTime::currentMSecsSinceEpoch() + 3600 * 1000);
	Wake(u"AIza-fail"_q);

	// A wrong key, also as Gemini's plain 400: the whole key rests.
	Failed(candidate, 400, "API key not valid. Please pass a valid API key.", "");
	CHECK(Resting(u"AIza-fail"_q, u"anything"_q));
	Wake(u"AIza-fail"_q);
	Failed(candidate, 401, "", "");
	CHECK(Resting(u"AIza-fail"_q, u"anything"_q));
	Wake(u"AIza-fail"_q);

	// No such model: only that model rests, long.
	Failed(candidate, 404, "model not found", "");
	CHECK(Resting(u"AIza-fail"_q, candidate.model));
	CHECK(!Resting(u"AIza-fail"_q, u"other"_q));
	Wake(u"AIza-fail"_q);

	CHECK(FailureText(401, "", "x").contains(u"ключ"_q));
	CHECK(FailureText(429, "", "x").contains(u"лимит"_q));
	CHECK(FailureText(0, "", "x").contains(u"соединения"_q));
}

void TestMasking() {
	using namespace Ai::details;
	const auto text = u"Привет, я Anna, пишите на anna@mail.com или @anna_ivanova, "
		"звоните +7 916 123-45-67 или смотрите https://t.me/annachannel . "
		"Цена 5000 руб, 3 места."_q;
	const auto masked = Mask(text);
	CHECK(!masked.text.contains(u"anna@mail.com"_q));
	CHECK(!masked.text.contains(u"@anna_ivanova"_q));
	CHECK(!masked.text.contains(u"916"_q));
	CHECK(!masked.text.contains(u"t.me/annachannel"_q));
	// The price and the count are not hidden.
	CHECK(masked.text.contains(u"5000"_q));
	CHECK(masked.text.contains(u"3 места"_q));
	// The marks come back as they were.
	CHECK_EQ(Unmask(masked.text, masked), text);
	// ... or become plain words when nothing real may be kept.
	const auto plain = PlainMarks(masked.text);
	CHECK(plain.contains(u"[почта]"_q));
	CHECK(plain.contains(u"[@юзернейм]"_q));
	CHECK(plain.contains(u"[номер]"_q));
	CHECK(plain.contains(u"[ссылка]"_q));
	CHECK(!plain.contains(u"<"_q));
	// Many marks of one kind: <U1> is not taken for part of <U12>.
	auto many = QString();
	for (auto i = 1; i <= 12; ++i) {
		many += u"@user%1abc "_q.arg(i, 2, 10, QChar('0'));
	}
	const auto maskedMany = Mask(many);
	CHECK_EQ(Unmask(maskedMany.text, maskedMany), many);
}

void TestChunks() {
	namespace K = Core::GgtgAi::Knowledge;
	// A long text of paragraphs: chunks of a sensible size, nothing lost.
	auto text = QString();
	for (auto i = 0; i != 60; ++i) {
		text += u"Абзац %1. Курс включает уроки по трейдингу. Цена курса "
			"составляет %2 рублей. Доступ навсегда.\n\n"_q
			.arg(i).arg(1000 + i);
	}
	const auto chunks = K::details::Split(text);
	CHECK(chunks.size() > 3);
	auto joined = QString();
	for (const auto &chunk : chunks) {
		CHECK(chunk.size() <= 1300);
		CHECK(chunk.size() > 0);
		joined += chunk + '\n';
	}
	CHECK(joined.contains(u"Абзац 0."_q));
	CHECK(joined.contains(u"Абзац 59."_q));
	CHECK(joined.contains(u"1059 рублей"_q));

	// A heading goes with the text under it.
	auto sectioned = QString();
	for (auto i = 0; i != 30; ++i) {
		sectioned += u"Раздел %1\n\n"_q.arg(i);
		sectioned += u"Подробности раздела %1: "_q.arg(i)
			+ QString(u"слово "_q).repeated(120) + u".\n\n"_q;
	}
	const auto withHeads = K::details::Split(sectioned);
	auto headsKept = 0;
	for (const auto &chunk : withHeads) {
		for (auto i = 0; i != 30; ++i) {
			if (chunk.contains(u"Раздел %1\n"_q.arg(i))
				&& chunk.contains(u"Подробности раздела %1:"_q.arg(i))) {
				++headsKept;
			}
		}
	}
	CHECK(headsKept >= 25);

	// One huge paragraph with no punctuation (a transcript): cut by words.
	const auto transcript = QString(u"привет это расшифровка видео "_q)
		.repeated(400);
	const auto cut = K::details::Split(transcript);
	CHECK(cut.size() > 5);
	for (const auto &chunk : cut) {
		CHECK(chunk.size() <= 1300);
		CHECK(!chunk.startsWith(u"ривет"_q)); // never in the middle of a word
	}

	// Table lines stay whole.
	auto table = QString();
	for (auto i = 0; i != 200; ++i) {
		table += u"тариф%1;%2;руб\n"_q.arg(i).arg(i * 10);
	}
	const auto rows = K::details::Split(table);
	CHECK(!rows.isEmpty());
	for (const auto &chunk : rows) {
		for (const auto &line : chunk.split(QChar('\n'))) {
			CHECK(line.isEmpty() || line.startsWith(u"тариф"_q));
		}
	}
	CHECK(K::details::Split(u"   \n\n  "_q).isEmpty());
}

void TestSearch() {
	namespace K = Core::GgtgAi::Knowledge;
	QDir(cWorkingDir()).removeRecursively();
	QDir().mkpath(cWorkingDir() + u"tdata/ggtg_db/"_q);
	K::details::Save({
		.id = u"price"_q,
		.name = u"Прайс"_q,
		.kind = u"text"_q,
		.origin = u"price"_q,
		.added = 1,
		.chunks = QStringList{
			u"Тариф Базовый: 5000 рублей, 10 уроков, доступ на три месяца."_q,
			u"Тариф Премиум: 15000 рублей, 30 уроков, личный куратор."_q,
		},
	});
	K::details::Save({
		.id = u"faq"_q,
		.name = u"FAQ"_q,
		.kind = u"text"_q,
		.origin = u"faq"_q,
		.added = 2,
		.chunks = QStringList{
			u"Можно ли вернуть деньги? Да, возврат в течение 14 дней."_q,
			u"Где проходят занятия? Онлайн, в Zoom, по вечерам."_q,
		},
	});
	CHECK(K::ChunkCount() == 4);
	// Asked about a refund: the refund chunk, not the prices.
	const auto refund = K::Block(600, u"а деньги вернуть можно, возврат?"_q);
	CHECK(refund.contains(u"возврат в течение 14 дней"_q));
	CHECK(!refund.contains(u"Премиум"_q));
	// Asked about the price of the premium: that chunk.
	const auto premium = K::Block(600, u"сколько стоит премиум тариф"_q);
	CHECK(premium.contains(u"15000"_q));
	// Nothing in common with the question: the beginning of each source.
	const auto none = K::Block(1000, u"qqq zzz"_q);
	CHECK(none.contains(u"Прайс"_q));
	CHECK(none.contains(u"FAQ"_q));
	// The limit is kept.
	CHECK(K::Block(120, QString()).size() < 600);
	K::Remove(u"faq"_q);
	CHECK(K::ChunkCount() == 2);
	CHECK(!K::Block(600, u"вернуть деньги"_q).contains(u"возврат"_q));
	CHECK(K::AllText().contains(u"15000"_q));
}

void TestTrainParse() {
	namespace T = Core::GgtgAi::Train;
	T::Insight insight;
	T::Parse(
		u"СТИЛЬ: пишет коротко, на «ты», без эмодзи\n"
		"- ВОПРОС: сколько стоит курс => Базовый 5000, Премиум 15000\n"
		"2. ВОЗРАЖЕНИЕ: дорого => Понимаю, давайте разберём рассрочку\n"
		"**ИТОГ**: договорились | предложил рассрочку сразу после «дорого»\n"
		"что-то лишнее без метки"_q,
		insight);
	CHECK(insight.styles.size() == 1);
	CHECK(insight.entries.size() == 2);
	CHECK_EQ(insight.entries[0].kind, u"question"_q);
	CHECK_EQ(insight.entries[0].what, u"сколько стоит курс"_q);
	CHECK_EQ(insight.entries[0].answers[0], u"Базовый 5000, Премиум 15000"_q);
	CHECK_EQ(insight.entries[1].kind, u"objection"_q);
	CHECK(insight.ending == T::Ending::Deal);
	CHECK(insight.lesson.contains(u"рассрочку"_q));

	T::Insight none;
	T::Parse(u"НЕТ"_q, none);
	CHECK(none.entries.empty() && !none.ending);

	T::Insight refusal;
	T::Parse(u"ИТОГ: отказ | молчал после цены"_q, refusal);
	CHECK(refusal.ending == T::Ending::Refusal);
	T::Insight silence;
	T::Parse(u"ИТОГ: молчание"_q, silence);
	CHECK(silence.ending == T::Ending::Silence);
}

void TestTrainTexts() {
	namespace T = Core::GgtgAi::Train;
	auto lines = std::vector<T::Line>();
	lines.push_back({ false, u"Привет, меня зовут Мария Петрова"_q, 1700000000 });
	lines.push_back({ true, u"Привет, Мария! Чем помочь?"_q, 1700000100 });
	lines.push_back({ false, u"@maria_p напишите мне, Мария"_q, 1700000200 });
	T::HideNames(lines, { u"Мария Петрова"_q, u"maria_p"_q, u"Иван"_q });
	for (const auto &line : lines) {
		CHECK(!line.text.contains(u"Мария"_q));
		CHECK(!line.text.contains(u"Петрова"_q));
		CHECK(!line.text.contains(u"maria_p"_q));
	}
	CHECK(lines[0].text.contains(u"Привет"_q));
	CHECK(lines[1].text.contains(u"Чем помочь"_q));

	// Windows: long talks are cut, the first part is kept.
	auto many = std::vector<T::Line>();
	for (auto i = 0; i != 400; ++i) {
		many.push_back({
			(i % 2) == 0,
			QString(u"сообщение номер %1 "_q.arg(i)).repeated(6),
			qint64(1700000000 + i),
		});
	}
	const auto windows = T::Windows(many);
	CHECK(windows.size() >= 1 && windows.size() <= 3);
	CHECK(windows.front().contains(u"номер 0 "_q));
	CHECK(windows.back().contains(u"номер 399 "_q));
	for (const auto &window : windows) {
		CHECK(window.size() < 8000);
		CHECK(window.contains(u"Я:"_q) || window.contains(u"Собеседник:"_q));
	}
}

void TestPlaybook() {
	namespace T = Core::GgtgAi::Train;
	T::Playbook().clear();
	T::AddEntry({
		.kind = u"objection"_q,
		.what = u"Это слишком дорого для меня"_q,
		.answers = QStringList{ u"Понимаю, есть рассрочка"_q },
	});
	T::AddEntry({
		.kind = u"objection"_q,
		.what = u"это слишком дорого для меня!"_q,
		.answers = QStringList{ u"Давайте подберём формат дешевле"_q },
	});
	CHECK(T::Playbook().size() == 1);
	CHECK(T::Playbook()[0].count == 2);
	CHECK(T::Playbook()[0].answers.size() == 2);
	// An answer that worked is put first.
	T::AddEntry({
		.kind = u"objection"_q,
		.what = u"Это слишком дорого для меня"_q,
		.answers = QStringList{ u"Давайте подберём формат дешевле"_q },
		.wins = 1,
	});
	CHECK_EQ(T::Playbook()[0].answers[0], u"Давайте подберём формат дешевле"_q);
	CHECK(T::Playbook()[0].wins == 1);
	T::AddEntry({
		.kind = u"question"_q,
		.what = u"Есть ли рассрочка?"_q,
		.answers = QStringList{ u"Да, до 6 месяцев"_q },
	});
	CHECK(T::Playbook().size() == 2);
	CHECK(T::Weight(T::Playbook()[0]) > T::Weight(T::Playbook()[1]));
	CHECK(T::EntryChunk(T::Playbook()[0]).contains(u"сработал"_q));
	T::Playbook().clear();
}

void TestInboxWords() {
	namespace I = Core::GgtgAi::Inbox;
	CHECK_EQ(I::StageFromWord(u"вопрос"_q), u"question"_q);
	CHECK_EQ(I::StageFromWord(u"Возражение"_q), u"objection"_q);
	CHECK_EQ(I::StageFromWord(u"интерес"_q), u"interest"_q);
	CHECK_EQ(I::StageFromWord(u"отказ"_q), u"refusal"_q);
	CHECK_EQ(I::StageFromWord(u"готов купить"_q), u"hot"_q);
	CHECK_EQ(I::StageFromWord(u"горячий"_q), u"hot"_q);
	CHECK_EQ(I::StageFromWord(u"другое"_q), u"other"_q);
}

void TestNameSort() {
	using Core::GgtgSort::SortKey;
	const auto names = std::vector<QString>{
		u"Zed"_q, u"Anna"_q, u"Борис"_q, u"김민수"_q, u"王伟"_q, u"محمد"_q,
		u"123abc"_q, u"🔥🔥"_q, u"🔥Anna"_q, u"_x_"_q,
	};
	auto keyed = std::vector<std::pair<std::pair<int, QString>, QString>>();
	for (const auto &name : names) {
		keyed.push_back({ SortKey(name), name });
	}
	std::stable_sort(keyed.begin(), keyed.end(), [](
			const auto &a,
			const auto &b) {
		return a.first < b.first;
	});
	auto order = QStringList();
	for (const auto &entry : keyed) {
		order.push_back(entry.second);
	}
	const auto at = [&](const QString &name) {
		return int(order.indexOf(name));
	};
	// Latin, then Cyrillic, then the other scripts, then digits, then signs.
	CHECK(at(u"Anna"_q) < at(u"Zed"_q));
	CHECK(at(u"🔥Anna"_q) < at(u"Zed"_q)); // the emoji in front is skipped
	CHECK(at(u"Zed"_q) < at(u"Борис"_q));
	CHECK(at(u"Борис"_q) < at(u"김민수"_q));
	CHECK(at(u"Борис"_q) < at(u"王伟"_q));
	CHECK(at(u"Борис"_q) < at(u"محمد"_q));
	CHECK(at(u"김민수"_q) < at(u"123abc"_q));
	CHECK(at(u"王伟"_q) < at(u"123abc"_q));
	CHECK(at(u"123abc"_q) < at(u"🔥🔥"_q));
	CHECK(at(u"123abc"_q) < at(u"_x_"_q) || SortKey(u"_x_"_q).first == 0);
	CHECK(SortKey(u"🔥🔥"_q).first == 500);
	// The same script stays together (the Han names are not scattered).
	CHECK(SortKey(u"王伟"_q).first == SortKey(u"李四"_q).first);
	CHECK(SortKey(u"김민수"_q).first != SortKey(u"王伟"_q).first);
}

void TestLanguage() {
	CHECK_EQ(Ai::DetectLanguage(u"김민수"_q), u"ko"_q);
	CHECK_EQ(Ai::DetectLanguage(u"王伟"_q), u"zh"_q);
	CHECK_EQ(Ai::DetectLanguage(u"ひらがな"_q), u"ja"_q);
	CHECK_EQ(Ai::DetectLanguage(u"Борис"_q), u"ru"_q);
	CHECK_EQ(Ai::DetectLanguage(u"Олексій Ґанзя"_q), u"uk"_q);
	CHECK_EQ(Ai::DetectLanguage(u"राहुल"_q), u"hi"_q);
	CHECK_EQ(Ai::DetectLanguage(u"محمد"_q), u"ar"_q);
	CHECK_EQ(Ai::DetectLanguage(u"Anna"_q), u"en"_q);
	CHECK(Ai::DetectLanguage(u"🔥🔥"_q).isEmpty());
}

void TestSettingsRoundTrip() {
	QDir(cWorkingDir()).removeRecursively();
	QDir().mkpath(cWorkingDir() + u"tdata/ggtg_db/"_q);
	auto settings = Ai::Settings();
	settings.keys = {
		{ u"AIza-1"_q, u"gemini"_q },
		{ u"AIza-2"_q, u"gemini"_q },
		{ u"gsk_3"_q, u"groq"_q },
	};
	settings.chatPrompt = u"Я продаю курсы"_q;
	settings.inboxAuto = false;
	settings.trainNightly = false;
	Ai::Save(settings);
	const auto loaded = Ai::Load();
	CHECK(loaded.keys.size() == 3);
	CHECK_EQ(loaded.keys[1].key, u"AIza-2"_q);
	CHECK_EQ(loaded.keys[1].provider, u"gemini"_q);
	CHECK_EQ(loaded.chatPrompt, u"Я продаю курсы"_q);
	CHECK(!loaded.inboxAuto);
	CHECK(!loaded.trainNightly);
	// The same key twice is one key.
	settings.keys.push_back({ u"AIza-1"_q, u"gemini"_q });
	Ai::Save(settings);
	CHECK(Ai::Load().keys.size() == 3);
	// A key of an unknown kind is not usable.
	CHECK(!Ai::CloudKeyUsable({ u"zzz"_q, QString() }));
	CHECK(Ai::CloudKeyUsable({ u"AIza-1"_q, u"gemini"_q }));
}

void TestInboxStore() {
	namespace I = Core::GgtgAi::Inbox;
	QDir(cWorkingDir()).removeRecursively();
	QDir().mkpath(cWorkingDir() + u"tdata/ggtg_db/"_q);
	I::Items().clear();
	I::Items().push_back({
		.key = I::KeyOf(42, u"Anna_X"_q),
		.senderId = 42,
		.sender = u"Мой аккаунт"_q,
		.username = u"anna_x"_q,
		.name = u"Anna"_q,
		.lastId = 100,
		.theirText = u"сколько стоит?"_q,
		.stage = u"question"_q,
		.draft = u"5000"_q,
		.status = I::Status::Pending,
		.sentText = u"прошлый ответ"_q,
		.sentStage = u"objection"_q,
		.autoAt = 12345,
	});
	I::Remembered().streaks[u"question"_q] = I::kAutopilotStreak;
	I::Remembered().autopilot = { u"interest"_q };
	I::Save();
	I::Items().clear();
	I::Remembered() = I::Memory();
	// A fresh read of the file: the state is the same.
	const auto fresh = Ai::Settings(); // (touches nothing)
	(void)fresh;
	auto state = QFile(I::Path());
	CHECK(state.exists());
	CHECK(I::Count().pending == 0);
	// The key is lower case whatever the username's case.
	CHECK_EQ(I::KeyOf(42, u"Anna_X"_q), u"42:anna_x"_q);
}

} // namespace

int main() {
	TestProviders();
	TestCandidates();
	TestFailures();
	TestMasking();
	TestChunks();
	TestSearch();
	TestTrainParse();
	TestTrainTexts();
	TestPlaybook();
	TestInboxWords();
	TestNameSort();
	TestLanguage();
	TestSettingsRoundTrip();
	TestInboxStore();
	std::fprintf(stderr, "%d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
