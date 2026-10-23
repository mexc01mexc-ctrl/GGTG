/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and exceptions, see https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "core/ggtg_ai.h"
#include "core/ggtg_ai_embed.h"
#include "core/ggtg_ai_web.h"

#include <cmath>
#include <optional>
#include <set>

namespace Core::GgtgAi::Knowledge {

// One knowledge base, "Материалы": whatever the user wants the assistant to
// be in the know about. It takes PDF, Word, text files, pictures (the text
// on them is read), links to sites, and YouTube (the video is taken by its
// subtitles; when there are none, Gemini watches it itself).
//
// Everything is cut into chunks of about a paragraph; the ones that fit the
// conversation (by meaning when there is a Gemini key, by words otherwise)
// are put in front of the model with every reply, see Block. The base lives
// in tdata/ggtg_db/ai_knowledge/, one file per source.

struct Source {
	QString id;
	QString name;
	QString kind; // file, pdf, image, web, youtube, text
	QString origin; // a path, an address
	qint64 added = 0;
	QStringList chunks;
};

namespace details {

constexpr auto kChunk = 900; // characters in a chunk, about
constexpr auto kLongPiece = 1200;
constexpr auto kMaxChunks = 800; // per source
constexpr auto kMaxText = 400'000; // characters read from one source
constexpr auto kMaxInline = 18 * 1024 * 1024; // a PDF sent to Gemini

[[nodiscard]] inline QString Dir() {
	return Core::GgtgPaths::DbDir() + u"ai_knowledge/"_q;
}

[[nodiscard]] inline QString IdOf(const QString &origin) {
	return QString::fromLatin1(QCryptographicHash::hash(
		origin.toUtf8(),
		QCryptographicHash::Sha1).toHex().left(16));
}

// A text cut into chunks of about `kChunk` characters on paragraph and
// sentence borders. A short heading goes together with the text under it.
[[nodiscard]] inline QStringList Split(QString text) {
	text.replace(u"\r\n"_q, u"\n"_q);
	text.replace(QChar('\r'), QChar('\n'));
	text.replace(QChar(0x00a0), QChar(' '));
	auto pieces = QStringList();

	// A run of text too long for a chunk: by sentences, then by words.
	const auto addLong = [&](const QString &run) {
		static const auto sentence = QRegularExpression(
			u"(?<=[.!?…。])\\s+"_q);
		auto current = QString();
		const auto flush = [&] {
			if (!current.trimmed().isEmpty()) {
				pieces.push_back(current.trimmed());
			}
			current.clear();
		};
		for (const auto &part : run.split(sentence, Qt::SkipEmptyParts)) {
			if (part.size() > kLongPiece) {
				flush();
				auto rest = part;
				while (rest.size() > kLongPiece) {
					auto cut = int(rest.lastIndexOf(QChar(' '), kChunk));
					if (cut < kChunk / 2) {
						cut = kChunk;
					}
					pieces.push_back(rest.left(cut).trimmed());
					rest = rest.mid(cut);
				}
				if (!rest.trimmed().isEmpty()) {
					pieces.push_back(rest.trimmed());
				}
				continue;
			}
			if (!current.isEmpty() && current.size() + part.size() > kChunk) {
				flush();
			}
			current += (current.isEmpty() ? QString() : u" "_q) + part;
		}
		flush();
	};

	static const auto blank = QRegularExpression(u"\\n\\s*\\n"_q);
	for (auto paragraph : text.split(blank, Qt::SkipEmptyParts)) {
		paragraph = paragraph.trimmed();
		if (paragraph.isEmpty()) {
			continue;
		} else if (paragraph.size() <= kLongPiece) {
			pieces.push_back(paragraph);
		} else if (paragraph.contains(QChar('\n'))) {
			// Lines that mean something (a table, a list): kept whole.
			auto current = QString();
			for (const auto &line : paragraph.split(QChar('\n'))) {
				if (line.size() > kLongPiece) {
					if (!current.isEmpty()) {
						pieces.push_back(current);
						current.clear();
					}
					addLong(line);
					continue;
				}
				if (!current.isEmpty()
					&& current.size() + line.size() > kChunk) {
					pieces.push_back(current);
					current.clear();
				}
				current += (current.isEmpty() ? QString() : u"\n"_q) + line;
			}
			if (!current.trimmed().isEmpty()) {
				pieces.push_back(current);
			}
		} else {
			addLong(paragraph);
		}
	}

	auto result = QStringList();
	auto current = QString();
	auto last = QString();
	for (const auto &piece : pieces) {
		if (!current.isEmpty() && current.size() + piece.size() + 1 > kChunk) {
			result.push_back(current);
			const auto heading = last.size() < 120
				&& !last.endsWith(QChar('.'))
				&& !last.endsWith(QChar('!'))
				&& !last.endsWith(QChar('?'))
				&& !last.endsWith(QChar(':'));
			current = heading ? last : QString();
		}
		current += (current.isEmpty() ? QString() : u"\n"_q) + piece;
		last = piece;
	}
	if (!current.isEmpty()) {
		result.push_back(current);
	}
	if (result.size() > 1) {
		// Crumbs are not worth a chunk of their own.
		for (auto i = 0; i < result.size();) {
			if (result.size() > 1 && result[i].size() < 30) {
				result.removeAt(i);
			} else {
				++i;
			}
		}
	}
	while (result.size() > kMaxChunks) {
		result.removeLast();
	}
	return result;
}

struct Store {
	std::vector<Source> sources;
	bool loaded = false;
	bool listed = false;
};

[[nodiscard]] inline Store &State() {
	static auto result = Store();
	return result;
}

inline void Write(const Source &source) {
	QDir().mkpath(Dir());
	auto file = QSaveFile(Dir() + source.id + u".json"_q);
	if (!file.open(QIODevice::WriteOnly)) {
		return;
	}
	file.write(QJsonDocument(QJsonObject{
		{ "name", source.name },
		{ "kind", source.kind },
		{ "origin", source.origin },
		{ "added", double(source.added) },
		{ "chunks", QJsonArray::fromStringList(source.chunks) },
	}).toJson(QJsonDocument::Compact));
	file.commit();
}

inline void ReadAll(Store &store) {
	auto it = QDirIterator(Dir(), QStringList{ u"*.json"_q }, QDir::Files);
	while (it.hasNext()) {
		auto file = QFile(it.next());
		if (!file.open(QIODevice::ReadOnly)) {
			continue;
		}
		const auto object = QJsonDocument::fromJson(file.readAll()).object();
		auto source = Source{
			.id = QFileInfo(file).completeBaseName(),
			.name = object.value("name").toString(),
			.kind = object.value("kind").toString(),
			.origin = object.value("origin").toString(),
			.added = qint64(object.value("added").toDouble()),
		};
		for (const auto &chunk : object.value("chunks").toArray()) {
			source.chunks.push_back(chunk.toString());
		}
		if (!source.chunks.isEmpty()) {
			store.sources.push_back(std::move(source));
		}
	}

	// The materials of the earlier version (a digest each) become sources.
	const auto old = AiDir() + u"materials/"_q;
	auto oldIt = QDirIterator(old, QStringList{ u"*.json"_q }, QDir::Files);
	while (oldIt.hasNext()) {
		const auto path = oldIt.next();
		auto file = QFile(path);
		if (!file.open(QIODevice::ReadOnly)) {
			continue;
		}
		const auto object = QJsonDocument::fromJson(file.readAll()).object();
		file.close();
		auto source = Source{
			.id = IdOf(u"old:"_q + object.value("name").toString()),
			.name = object.value("name").toString(),
			.kind = u"text"_q,
			.origin = u"old:"_q + object.value("name").toString(),
			.added = QDateTime::currentSecsSinceEpoch(),
			.chunks = Split(object.value("text").toString()),
		};
		if (!source.chunks.isEmpty()) {
			Write(source);
			store.sources.push_back(std::move(source));
		}
		QFile::remove(path);
	}
	std::sort(store.sources.begin(), store.sources.end(), [](
			const Source &a,
			const Source &b) {
		return (a.added != b.added) ? (a.added < b.added) : (a.name < b.name);
	});
}

} // namespace details

// Every source, the oldest first.
[[nodiscard]] inline const std::vector<Source> &List() {
	auto &store = details::State();
	if (!std::exchange(store.loaded, true)) {
		details::ReadAll(store);
	}
	return store.sources;
}

[[nodiscard]] inline int ChunkCount() {
	auto result = 0;
	for (const auto &source : List()) {
		result += source.chunks.size();
	}
	return result;
}

[[nodiscard]] inline QStringList AllChunks() {
	auto result = QStringList();
	for (const auto &source : List()) {
		result.append(source.chunks);
	}
	return result;
}

// All the text of the base in one piece (to check a draft against), at most
// `limit` characters.
[[nodiscard]] inline QString AllText(int limit = 300'000) {
	auto result = QString();
	for (const auto &source : List()) {
		for (const auto &chunk : source.chunks) {
			if (result.size() >= limit) {
				return result;
			}
			result += chunk + '\n';
		}
	}
	return result;
}

inline void Remove(const QString &id) {
	(void)List();
	auto &sources = details::State().sources;
	sources.erase(
		std::remove_if(sources.begin(), sources.end(), [&](const Source &s) {
			return s.id == id;
		}),
		sources.end());
	QFile::remove(details::Dir() + id + u".json"_q);
}

inline void RemoveAll() {
	const auto ids = [&] {
		auto result = QStringList();
		for (const auto &source : List()) {
			result.push_back(source.id);
		}
		return result;
	}();
	for (const auto &id : ids) {
		Remove(id);
	}
}

namespace details {

inline void Save(Source source) {
	(void)List();
	auto &sources = State().sources;
	sources.erase(
		std::remove_if(sources.begin(), sources.end(), [&](const Source &s) {
			return s.id == source.id;
		}),
		sources.end());
	Write(source);
	sources.push_back(std::move(source));
}

// ---- finding what fits the conversation ----

[[nodiscard]] inline QStringList Stems(const QString &text) {
	static const auto word = QRegularExpression(u"[\\p{L}\\p{N}]{3,}"_q);
	static const auto stop = QSet<QString>{
		u"что"_q, u"как"_q, u"это"_q, u"для"_q, u"при"_q, u"или"_q,
		u"все"_q, u"его"_q, u"она"_q, u"они"_q, u"мне"_q, u"вас"_q,
		u"нас"_q, u"так"_q, u"уже"_q, u"еще"_q, u"ещё"_q, u"был"_q,
		u"the"_q, u"and"_q, u"for"_q, u"you"_q, u"are"_q, u"not"_q,
		u"but"_q, u"with"_q, u"that"_q, u"this"_q, u"have"_q,
	};
	auto result = QStringList();
	auto it = word.globalMatch(text.toLower());
	while (it.hasNext()) {
		const auto found = it.next().captured(0);
		if (!stop.contains(found)) {
			// Five letters are enough to tell the endings apart.
			result.push_back(found.left(5));
		}
	}
	return result;
}

} // namespace details

// Gets the meaning vector of the query (and of any chunk that has none yet),
// so that Block(limit, query) can rank by meaning, then calls `done`. Never
// waits long (eight seconds at most); without a Gemini key it calls `done`
// at once and Block ranks by words.
inline void Warm(const QString &query, Fn<void()> done) {
	if (!Embed::Available()
		|| query.trimmed().isEmpty()
		|| ChunkCount() == 0) {
		done();
		return;
	}
	auto texts = QStringList{ query };
	texts.append(AllChunks());
	const auto finished = std::make_shared<bool>(false);
	const auto finish = [=] {
		if (!std::exchange(*finished, true)) {
			done();
		}
	};
	QTimer::singleShot(8000, qApp, finish);
	Embed::Prepare(texts, [=](bool) { finish(); });
}

// What the assistant is told from the base, at most `limit` characters: the
// chunks closest to `query` (the end of the conversation), grouped by
// source. Without a query, or when nothing matches, the beginning of each
// source - what it is about.
[[nodiscard]] inline QString Block(int limit, const QString &query = {}) {
	const auto &sources = List();
	if (sources.empty()) {
		return QString();
	}
	struct Entry {
		int source = 0;
		int index = 0;
		double score = 0.;
	};
	auto entries = std::vector<Entry>();
	auto total = 0;
	for (auto s = 0; s != int(sources.size()); ++s) {
		for (auto i = 0; i != sources[s].chunks.size(); ++i) {
			entries.push_back({ s, i, 0. });
			++total;
		}
	}
	const auto queryStems = details::Stems(query);
	const auto wanted = QSet<QString>(queryStems.begin(), queryStems.end());
	if (!wanted.isEmpty()) {
		auto sets = std::vector<QSet<QString>>();
		sets.reserve(entries.size());
		auto df = std::map<QString, int>();
		for (const auto &entry : entries) {
			const auto stems = details::Stems(
				sources[entry.source].chunks[entry.index]);
			auto set = QSet<QString>(stems.begin(), stems.end());
			set.intersect(wanted);
			for (const auto &stem : set) {
				++df[stem];
			}
			sets.push_back(std::move(set));
		}
		auto idf = std::map<QString, double>();
		auto idfSum = 0.;
		for (const auto &[stem, count] : df) {
			idf[stem] = std::log(1. + double(total) / count);
			idfSum += idf[stem];
		}
		const auto queryVector = Embed::Cached(query);
		for (auto i = 0; i != int(entries.size()); ++i) {
			auto words = 0.;
			for (const auto &stem : sets[i]) {
				words += idf[stem];
			}
			if (idfSum > 0.) {
				words /= idfSum;
			}
			auto meaning = -1.;
			if (queryVector) {
				const auto &chunk = sources[entries[i].source]
					.chunks[entries[i].index];
				if (const auto vector = Embed::Cached(chunk)) {
					meaning = std::clamp(
						Embed::Similarity(*queryVector, *vector),
						0.,
						1.);
				}
			}
			entries[i].score = (meaning >= 0.)
				? (0.7 * meaning + 0.3 * words)
				: words;
		}
		std::stable_sort(entries.begin(), entries.end(), [](
				const Entry &a,
				const Entry &b) {
			return a.score > b.score;
		});
	}

	auto picked = std::vector<Entry>();
	if (!wanted.isEmpty() && entries.front().score > 0.) {
		const auto top = entries.front().score;
		auto used = 0;
		for (const auto &entry : entries) {
			const auto &chunk = sources[entry.source].chunks[entry.index];
			const auto size = int(chunk.size() + sources[entry.source].name
				.size() + 4);
			if (entry.score < top * 0.5
				|| (!picked.empty() && used + size > limit)) {
				break;
			}
			picked.push_back(entry);
			used += size;
		}
		std::sort(picked.begin(), picked.end(), [](
				const Entry &a,
				const Entry &b) {
			return (a.source != b.source)
				? (a.source < b.source)
				: (a.index < b.index);
		});
	} else {
		const auto each = std::max(limit / int(sources.size()), 400);
		auto result = QString();
		for (const auto &source : sources) {
			result += u"• "_q + source.name + u": "_q
				+ source.chunks.front().left(each).trimmed() + '\n';
		}
		return result;
	}
	auto result = QString();
	for (const auto &entry : picked) {
		result += u"• "_q + sources[entry.source].name + u": "_q
			+ sources[entry.source].chunks[entry.index].trimmed() + '\n';
	}
	return result;
}

namespace details {

// ---- reading the sources ----

[[nodiscard]] inline bool IsPicture(const QString &suffix) {
	return suffix == u"png"_q || suffix == u"jpg"_q || suffix == u"jpeg"_q
		|| suffix == u"webp"_q || suffix == u"gif"_q || suffix == u"heic"_q
		|| suffix == u"bmp"_q || suffix == u"tiff"_q;
}

// The text of a PDF that has a text layer, with what the computer has: on a
// Mac its own PDFKit, elsewhere pdftotext when it is installed. Empty for a
// scan, or when there is no reader. Blocks: call it off the main thread.
[[nodiscard]] inline QString PdfTextLocal(const QString &path) {
	auto process = QProcess();
	Core::GgtgAi::HideConsoleWindow(&process);
#ifdef Q_OS_MAC
	process.start(u"/usr/bin/osascript"_q, QStringList{
		u"-l"_q,
		u"JavaScript"_q,
		u"-e"_q,
		u"function run(argv){ObjC.import('PDFKit');"
		"var d=$.PDFDocument.alloc.initWithURL("
		"$.NSURL.fileURLWithPath(argv[0]));"
		"return d?ObjC.unwrap(d.string):'';}"_q,
		path,
	});
#else // Q_OS_MAC
	const auto program = QStandardPaths::findExecutable(u"pdftotext"_q);
	if (program.isEmpty()) {
		return QString();
	}
	process.start(program, QStringList{
		u"-layout"_q,
		u"-enc"_q,
		u"UTF-8"_q,
		path,
		u"-"_q,
	});
#endif // Q_OS_MAC
	if (!process.waitForFinished(120'000)) {
		process.kill();
		return QString();
	}
	return QString::fromUtf8(process.readAllStandardOutput());
}

// Gemini's own interface (not the OpenAI-like one): it takes a PDF as it is
// and watches a YouTube video by its address. `parts` are the "parts" of one
// user message. The best Gemini model that does not rest is used; a failure
// sends the model to rest and the next one tries.
inline void GeminiNative(
		QJsonArray parts,
		int maxTokens,
		int timeout,
		Fn<void(QString text, QString error)> done,
		int attempt = 0) {
	using Core::GgtgAi::details::Candidate;
	const auto settings = Load();
	Core::GgtgAi::details::Warm(settings);
	auto picked = std::optional<Candidate>();
	for (const auto &candidate : Core::GgtgAi::details::Candidates(
			settings,
			Task::Vision,
			false)) {
		if (candidate.key.provider == u"gemini"_q) {
			picked = candidate;
			break;
		}
	}
	if (!picked) {
		done(QString(), attempt
			? u"Gemini сейчас не отвечает (лимит или ошибка), попробуйте "
				"позже"_q
			: u"для этого нужен ключ Gemini: раздел «ИИ» → «Ключи»"_q);
		return;
	}
	const auto candidate = *picked;
	const auto lite = candidate.model.startsWith(u"gemini-2.5-flash"_q);
	auto generation = QJsonObject{
		{ "maxOutputTokens", lite ? maxTokens : (maxTokens + 4096) },
		{ "temperature", 0.1 },
	};
	if (lite) {
		generation.insert("thinkingConfig", QJsonObject{
			{ "thinkingBudget", 0 },
		});
	}
	const auto body = QJsonObject{
		{ "contents", QJsonArray{ QJsonObject{ { "parts", parts } } } },
		{ "generationConfig", generation },
	};
	auto request = QNetworkRequest(QUrl(
		u"https://generativelanguage.googleapis.com/v1beta/models/%1"
		":generateContent"_q.arg(candidate.model)));
	request.setHeader(
		QNetworkRequest::ContentTypeHeader,
		u"application/json"_q);
	request.setRawHeader("x-goog-api-key", candidate.key.key.trimmed().toUtf8());
	request.setTransferTimeout(timeout);
	const auto reply = Core::GgtgAi::details::Manager().post(
		request,
		QJsonDocument(body).toJson(QJsonDocument::Compact));
	QObject::connect(reply, &QNetworkReply::finished, reply, [=] {
		const auto data = reply->readAll();
		const auto failed = (reply->error() != QNetworkReply::NoError);
		const auto text = reply->errorString();
		const auto status = reply->attribute(
			QNetworkRequest::HttpStatusCodeAttribute).toInt();
		const auto retryAfter = reply->rawHeader("Retry-After");
		reply->deleteLater();
		auto answer = QString();
		if (!failed) {
			const auto candidates = QJsonDocument::fromJson(data).object()
				.value("candidates").toArray();
			if (!candidates.isEmpty()) {
				const auto content = candidates.at(0).toObject()
					.value("content").toObject().value("parts").toArray();
				for (const auto &part : content) {
					const auto object = part.toObject();
					if (!object.value("thought").toBool()) {
						answer += object.value("text").toString();
					}
				}
			}
		}
		if (failed || answer.trimmed().isEmpty()) {
			Core::GgtgAi::details::Failed(
				candidate,
				failed ? status : 0,
				data,
				retryAfter);
			if (attempt < 3) {
				GeminiNative(parts, maxTokens, timeout, done, attempt + 1);
				return;
			}
			done(QString(), failed
				? Core::GgtgAi::details::FailureText(status, data, text)
				: u"Gemini вернул пустой ответ"_q);
			return;
		}
		done(answer.trimmed(), QString());
	});
}

// A page or a file from the internet, at most `maxBytes` of it.
inline void Fetch(
		const QUrl &url,
		const QByteArray &cookie,
		qint64 maxBytes,
		Fn<void(QByteArray data, QString error)> done) {
	auto request = QNetworkRequest(url);
	request.setRawHeader("User-Agent", Web::details::Agent());
	request.setRawHeader("Accept-Language", "ru,en;q=0.8");
	if (!cookie.isEmpty()) {
		request.setRawHeader("Cookie", cookie);
	}
	request.setAttribute(
		QNetworkRequest::RedirectPolicyAttribute,
		QNetworkRequest::NoLessSafeRedirectPolicy);
	request.setTransferTimeout(60'000);
	const auto reply = Core::GgtgAi::details::Manager().get(request);
	const auto tooBig = std::make_shared<bool>(false);
	QObject::connect(reply, &QNetworkReply::downloadProgress, reply, [=](
			qint64 received,
			qint64) {
		if (received > maxBytes && !std::exchange(*tooBig, true)) {
			reply->abort();
		}
	});
	QObject::connect(reply, &QNetworkReply::finished, reply, [=] {
		reply->deleteLater();
		const auto status = reply->attribute(
			QNetworkRequest::HttpStatusCodeAttribute).toInt();
		if (*tooBig) {
			done({}, u"файл слишком большой"_q);
		} else if (reply->error() != QNetworkReply::NoError) {
			done({}, reply->errorString());
		} else if (status != 200) {
			done({}, u"ответ "_q + QString::number(status));
		} else {
			done(reply->readAll(), QString());
		}
	});
}

// ---- YouTube ----

[[nodiscard]] inline QString YoutubeId(const QString &address) {
	static const auto re = QRegularExpression(
		u"(?:youtu\\.be/|youtube(?:-nocookie)?\\.com/(?:watch\\?(?:[^#]*&)?v="
		"|shorts/|embed/|live/|v/))([A-Za-z0-9_-]{11})"_q);
	const auto match = re.match(address);
	return match.hasMatch() ? match.captured(1) : QString();
}

// The subtitles of a video (its own, else the automatic ones; Russian and
// English first) as one text, and its title. `error` says why there is no
// text; the title may still be there.
inline void YoutubeTranscript(
		const QString &id,
		Fn<void(QString title, QString text, QString error)> done) {
	Fetch(
		QUrl(u"https://www.youtube.com/watch?v="_q + id + u"&hl=ru"_q),
		"CONSENT=YES+cb; SOCS=CAI",
		6 * 1024 * 1024,
		[=](QByteArray data, QString error) {
			if (!error.isEmpty()) {
				done(QString(), QString(), u"YouTube не открылся ("_q
					+ error + ')');
				return;
			}
			const auto html = QString::fromUtf8(data);
			auto title = Web::details::PageTitle(html);
			if (title.endsWith(u" - YouTube"_q)) {
				title.chop(10);
			}
			const auto marker = html.indexOf(u"\"captionTracks\":["_q);
			if (marker < 0) {
				done(title, QString(), u"у видео нет субтитров"_q);
				return;
			}
			// The array of tracks: up to the bracket that closes it.
			const auto start = html.indexOf(QChar('['), marker);
			auto depth = 0;
			auto inString = false;
			auto end = -1;
			for (auto i = start; i < html.size(); ++i) {
				const auto ch = html[i];
				if (inString) {
					if (ch == QChar('\\')) {
						++i;
					} else if (ch == QChar('"')) {
						inString = false;
					}
				} else if (ch == QChar('"')) {
					inString = true;
				} else if (ch == QChar('[')) {
					++depth;
				} else if (ch == QChar(']') && --depth == 0) {
					end = i;
					break;
				}
			}
			const auto tracks = QJsonDocument::fromJson(
				html.mid(start, end - start + 1).toUtf8()).array();
			if (end < 0 || tracks.isEmpty()) {
				done(title, QString(), u"у видео нет субтитров"_q);
				return;
			}
			auto best = QJsonObject();
			auto bestRank = -1;
			for (const auto &value : tracks) {
				const auto track = value.toObject();
				const auto language = track.value("languageCode").toString();
				const auto automatic = track.value("kind").toString()
					== u"asr"_q;
				const auto rank = (language.startsWith(u"ru"_q) ? 4 : 0)
					+ (language.startsWith(u"en"_q) ? 2 : 0)
					+ (automatic ? 0 : 6) + 1;
				if (rank > bestRank) {
					bestRank = rank;
					best = track;
				}
			}
			auto address = best.value("baseUrl").toString();
			if (address.isEmpty()) {
				done(title, QString(), u"у видео нет субтитров"_q);
				return;
			}
			Fetch(
				QUrl(address + u"&fmt=json3"_q),
				"CONSENT=YES+cb; SOCS=CAI",
				6 * 1024 * 1024,
				[=](QByteArray data, QString error) {
					auto text = QString();
					const auto events = QJsonDocument::fromJson(data)
						.object().value("events").toArray();
					for (const auto &event : events) {
						auto line = QString();
						for (const auto &seg : event.toObject()
								.value("segs").toArray()) {
							line += seg.toObject().value("utf8").toString();
						}
						line = line.simplified();
						if (!line.isEmpty()) {
							text += line + ' ';
						}
					}
					text = text.trimmed();
					done(title, text, text.isEmpty()
						? (error.isEmpty()
							? u"субтитры пустые"_q
							: u"субтитры не скачались ("_q + error + ')')
						: QString());
				});
		});
}

// Cuts `text` into chunks, saves the source, builds its meaning index.
inline void Ingest(
		Source source,
		QString text,
		Fn<void(QString progress)> progress,
		Fn<void(QString error)> done) {
	text = text.trimmed().left(kMaxText);
	source.id = IdOf(source.origin);
	source.added = QDateTime::currentSecsSinceEpoch();
	source.chunks = Split(text);
	if (source.chunks.isEmpty()) {
		done(u"в «"_q + source.name + u"» не нашлось текста"_q);
		return;
	}
	const auto chunks = source.chunks;
	Save(std::move(source));
	if (!Embed::Available()) {
		done(QString());
		return;
	}
	progress(u"Строю смысловой индекс (%1 кусков)..."_q.arg(chunks.size()));
	Embed::Prepare(chunks, [=](bool) { done(QString()); });
}

inline void ReadPdfWithGemini(
		const QString &path,
		Fn<void(QString text, QString error)> done) {
	auto file = QFile(path);
	if (!file.open(QIODevice::ReadOnly)) {
		done(QString(), u"файл не открылся"_q);
		return;
	} else if (file.size() > kMaxInline) {
		done(QString(), u"PDF больше 18 МБ: разделите его на части"_q);
		return;
	}
	const auto bytes = file.readAll();
	GeminiNative(
		QJsonArray{
			QJsonObject{ { "inline_data", QJsonObject{
				{ "mime_type", "application/pdf" },
				{ "data", QString::fromLatin1(bytes.toBase64()) },
			} } },
			QJsonObject{ { "text", u"Извлеки весь текст этого документа "
				"дословно, по порядку. Таблицы передай строками. Текст на "
				"картинках и сканах тоже перепиши. Без комментариев и "
				"вступлений."_q } },
		},
		16000,
		300'000,
		std::move(done));
}

} // namespace details

// Adds a file: a document (txt, md, csv, docx, rtf, html, pptx), a PDF, or
// a picture. `progress` says what is going on, `done(error)` runs on the main
// thread when it is over (empty error: the file is in the base).
inline void AddFile(
		const QString &path,
		Fn<void(QString progress)> progress,
		Fn<void(QString error)> done) {
	const auto name = QFileInfo(path).fileName();
	const auto suffix = QFileInfo(path).suffix().toLower();
	auto source = Source{ .name = name, .origin = path };

	if (details::IsPicture(suffix)) {
		source.kind = u"image"_q;
		progress(u"Смотрю на «"_q + name + u"»..."_q);
		crl::async([=] {
			const auto url = Core::GgtgAi::details::PictureDataUrl(path);
			crl::on_main([=] {
				if (url.isEmpty()) {
					done(u"не удалось прочитать картинку"_q);
					return;
				}
				Ask(
					u"Ты переносишь содержимое картинки в базу знаний "
					"помощника. Ответ только по-русски, без вступлений."_q,
					u"Сначала перепиши весь видимый текст и цифры дословно, "
					"построчно. Потом коротко опиши, что изображено (слайд, "
					"схема, прайс, скриншот, документ, фото) и о чём оно. "
					"До 3000 символов."_q,
					[=](QString result, QString error) {
						if (!error.isEmpty()) {
							done(error);
							return;
						}
						details::Ingest(source, result, progress, done);
					},
					0.1,
					1800,
					Role::General,
					QStringList{ url });
			});
		});
		return;
	}

	if (suffix == u"pdf"_q) {
		source.kind = u"pdf"_q;
		progress(u"Читаю PDF «"_q + name + u"»..."_q);
		crl::async([=] {
			const auto text = details::PdfTextLocal(path).trimmed();
			crl::on_main([=] {
				if (text.size() >= 200) {
					details::Ingest(source, text, progress, done);
					return;
				}
				progress(u"В «"_q + name + u"» нет текстового слоя или нечем "
					"его прочитать: читает Gemini..."_q);
				details::ReadPdfWithGemini(path, [=](
						QString result,
						QString error) {
					if (!error.isEmpty()) {
						done(u"не получилось прочитать «"_q + name + u"»: "_q
							+ error);
						return;
					}
					details::Ingest(source, result, progress, done);
				});
			});
		});
		return;
	}

	source.kind = u"file"_q;
	progress(u"Читаю «"_q + name + u"»..."_q);
	crl::async([=] {
		const auto text = Core::GgtgAi::details::ExtractDocumentText(path);
		crl::on_main([=] {
			if (text.trimmed().isEmpty()) {
				done(u"не получилось прочитать «"_q + name
					+ u"» (подходят pdf, txt, md, csv, docx, rtf, html, "
						"pptx и картинки)"_q);
				return;
			}
			details::Ingest(source, text, progress, done);
		});
	});
}

// Adds a link: a YouTube video, a PDF on the internet or a page of a site.
inline void AddUrl(
		const QString &raw,
		Fn<void(QString progress)> progress,
		Fn<void(QString error)> done) {
	auto address = raw.trimmed();
	if (address.isEmpty()) {
		done(u"вставьте ссылку"_q);
		return;
	} else if (!address.contains(u"://"_q)) {
		address = u"https://"_q + address;
	}
	auto source = Source{ .origin = address };

	if (const auto id = details::YoutubeId(address); !id.isEmpty()) {
		source.kind = u"youtube"_q;
		source.origin = u"https://www.youtube.com/watch?v="_q + id;
		progress(u"Беру расшифровку видео..."_q);
		details::YoutubeTranscript(id, [=](
				QString title,
				QString text,
				QString error) {
			auto named = source;
			named.name = u"YouTube: "_q + (title.isEmpty() ? id : title);
			if (error.isEmpty() && text.size() >= 100) {
				details::Ingest(named, text, progress, done);
				return;
			}
			progress(u"Субтитров нет ("_q + error
				+ u"): видео смотрит Gemini..."_q);
			details::GeminiNative(
				QJsonArray{
					QJsonObject{ { "file_data", QJsonObject{
						{ "file_uri", named.origin },
					} } },
					QJsonObject{ { "text", u"Сделай подробную расшифровку "
						"этого видео по порядку: что в нём говорится, "
						"дословно там, где важны цифры, цены, условия, "
						"названия и имена. Если речи нет, опиши, что "
						"показано. Без вступлений и комментариев."_q } },
				},
				16000,
				600'000,
				[=](QString result, QString geminiError) {
					if (!geminiError.isEmpty()) {
						done(u"у видео нет доступных субтитров, а Gemini не "
							"смог его посмотреть: "_q + geminiError);
						return;
					}
					details::Ingest(named, result, progress, done);
				});
		});
		return;
	}

	if (QUrl(address).path().toLower().endsWith(u".pdf"_q)) {
		source.kind = u"pdf"_q;
		source.name = QUrl(address).fileName();
		progress(u"Скачиваю PDF..."_q);
		details::Fetch(
			QUrl(address),
			QByteArray(),
			details::kMaxInline,
			[=](QByteArray data, QString error) {
				if (!error.isEmpty()) {
					done(u"PDF не скачался ("_q + error + ')');
					return;
				}
				const auto temp = QStandardPaths::writableLocation(
					QStandardPaths::TempLocation)
					+ u"/ggtg_"_q + details::IdOf(address) + u".pdf"_q;
				auto file = QFile(temp);
				if (!file.open(QIODevice::WriteOnly)
					|| file.write(data) != data.size()) {
					done(u"не удалось сохранить скачанный PDF"_q);
					return;
				}
				file.close();
				AddFile(temp, progress, [=](QString error) {
					QFile::remove(temp);
					if (!error.isEmpty()) {
						done(error);
						return;
					}
					// The file went in under its temporary path: put it
					// under the link instead.
					auto &sources = details::State().sources;
					for (auto &added : sources) {
						if (added.origin == temp) {
							auto moved = added;
							QFile::remove(details::Dir() + added.id
								+ u".json"_q);
							moved.origin = address;
							moved.name = source.name;
							moved.kind = u"pdf"_q;
							moved.id = details::IdOf(address);
							added = moved;
							details::Write(moved);
							break;
						}
					}
					done(QString());
				});
			});
		return;
	}

	source.kind = u"web"_q;
	progress(u"Открываю страницу..."_q);
	Web::FetchPage(address, details::kMaxText, [=](
			QString title,
			QString text,
			QString error) {
		if (!error.isEmpty() || text.trimmed().isEmpty()) {
			done(u"страница не прочиталась ("_q
				+ (error.isEmpty() ? u"пустая"_q : error) + ')');
			return;
		}
		auto named = source;
		named.name = title.isEmpty() ? QUrl(address).host() : title.left(100);
		details::Ingest(named, text, progress, done);
	});
}

// Adds a piece of text written or pasted by hand.
inline void AddText(
		const QString &name,
		const QString &text,
		Fn<void(QString progress)> progress,
		Fn<void(QString error)> done) {
	details::Ingest(
		Source{
			.name = name,
			.kind = u"text"_q,
			.origin = u"text:"_q + name,
		},
		text,
		std::move(progress),
		std::move(done));
}

// The one source that is not added by hand: what GGTG learned from the
// user's own chats (see core/ggtg_ai_train.h). Empty `chunks` removes it.
inline void SetLearned(
		const QString &name,
		const QStringList &chunks,
		Fn<void(QString progress)> progress,
		Fn<void(QString error)> done) {
	const auto id = details::IdOf(u"learned:playbook"_q);
	if (chunks.isEmpty()) {
		Remove(id);
		done(QString());
		return;
	}
	auto source = Source{
		.id = id,
		.name = name,
		.kind = u"playbook"_q,
		.origin = u"learned:playbook"_q,
		.added = QDateTime::currentSecsSinceEpoch(),
		.chunks = chunks,
	};
	while (source.chunks.size() > details::kMaxChunks) {
		source.chunks.removeLast();
	}
	details::Save(source);
	if (!Embed::Available()) {
		done(QString());
		return;
	}
	progress(u"Строю смысловой индекс (%1 кусков)..."_q.arg(chunks.size()));
	Embed::Prepare(source.chunks, [=](bool) { done(QString()); });
}

[[nodiscard]] inline QString KindTitle(const QString &kind) {
	if (kind == u"playbook"_q) {
		return u"Из ваших чатов"_q;
	} else if (kind == u"pdf"_q) {
		return u"PDF"_q;
	} else if (kind == u"image"_q) {
		return u"Картинка"_q;
	} else if (kind == u"web"_q) {
		return u"Сайт"_q;
	} else if (kind == u"youtube"_q) {
		return u"YouTube"_q;
	} else if (kind == u"text"_q) {
		return u"Текст"_q;
	}
	return u"Файл"_q;
}

} // namespace Core::GgtgAi::Knowledge
