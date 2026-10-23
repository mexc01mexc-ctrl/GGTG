/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and exceptions, see https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "core/ggtg_ai.h"

#include <cmath>
#include <optional>
#include <set>

namespace Core::GgtgAi::Embed {

// Meaning search: Google's free embedding model (gemini-embedding-001, with
// any Gemini key) turns a text into numbers so that texts with the same
// meaning are close ("сколько стоит" and "какая цена" meet, which word
// matching cannot do). Vectors of texts already seen are kept in
// tdata/ggtg_db/ai_embed_cache_gemini.json, so each text is embedded once.
// The texts are other people's replies: they leave with names, numbers and
// usernames replaced by marks, like every bulk job.
//
// Everything that uses it works without it too (word matching, the model):
// without a Gemini key, or when the key rests, callers get "not ok" and
// carry on.

inline constexpr auto kModel = "gemini-embedding-001";
// The model is trained so that the first numbers of a vector carry the most
// meaning: 768 of its 3072 keep the search good and the cache small.
inline constexpr auto kDimensions = 768;

namespace details {

// The first Gemini key that does not rest.
[[nodiscard]] inline std::optional<CloudKey> Key() {
	for (const auto &key : Load().keys) {
		if (key.provider == u"gemini"_q
			&& !Core::GgtgAi::details::Resting(key.key, QString(kModel))) {
			return key;
		}
	}
	return std::nullopt;
}

} // namespace details

// A Gemini key is there to make vectors with.
[[nodiscard]] inline bool Available() {
	return Load().enabled && details::Key().has_value();
}

using Vector = std::vector<float>;

namespace details {

[[nodiscard]] inline QString CachePath() {
	return Core::GgtgPaths::DbDir() + u"ai_embed_cache_gemini.json"_q;
}

[[nodiscard]] inline QString KeyOf(const QString &text) {
	return QString::fromLatin1(QCryptographicHash::hash(
		text.toUtf8(),
		QCryptographicHash::Sha1).toHex());
}

struct Cache {
	std::map<QString, Vector> vectors;
	bool loaded = false;
	bool dirty = false;
};

[[nodiscard]] inline Cache &CacheState() {
	static auto result = Cache();
	if (!std::exchange(result.loaded, true)) {
		// The vectors of the local model GGTG used before mean nothing to
		// this one.
		QFile::remove(Core::GgtgPaths::DbDir() + u"ai_embed_cache.json"_q);
		auto file = QFile(CachePath());
		if (file.open(QIODevice::ReadOnly)) {
			const auto object = QJsonDocument::fromJson(
				file.readAll()).object();
			for (auto i = object.begin(); i != object.end(); ++i) {
				const auto bytes = QByteArray::fromBase64(
					i.value().toString().toLatin1());
				if (bytes.isEmpty() || bytes.size() % sizeof(float)) {
					continue;
				}
				auto vector = Vector(bytes.size() / sizeof(float));
				memcpy(vector.data(), bytes.constData(), bytes.size());
				result.vectors[i.key()] = std::move(vector);
			}
		}
	}
	return result;
}

inline void SaveCache() {
	auto &cache = CacheState();
	if (!cache.dirty) {
		return;
	}
	cache.dirty = false;
	// A runaway cache is simply dropped: vectors are made again on demand.
	if (cache.vectors.size() > 6000) {
		cache.vectors.clear();
	}
	auto object = QJsonObject();
	for (const auto &[key, vector] : cache.vectors) {
		object.insert(key, QString::fromLatin1(QByteArray(
			reinterpret_cast<const char*>(vector.data()),
			int(vector.size() * sizeof(float))).toBase64()));
	}
	auto file = QSaveFile(CachePath());
	if (file.open(QIODevice::WriteOnly)) {
		file.write(QJsonDocument(object).toJson(QJsonDocument::Compact));
		file.commit();
	}
}

inline void Normalize(Vector &vector) {
	auto sum = 0.;
	for (const auto value : vector) {
		sum += double(value) * value;
	}
	if (sum <= 0.) {
		return;
	}
	const auto scale = float(1. / std::sqrt(sum));
	for (auto &value : vector) {
		value *= scale;
	}
}

} // namespace details

// The vector of a text if it has been made already, otherwise null.
[[nodiscard]] inline const Vector *Cached(const QString &text) {
	auto &cache = details::CacheState();
	const auto i = cache.vectors.find(
		details::KeyOf(text.simplified().left(600)));
	return (i == cache.vectors.end()) ? nullptr : &i->second;
}

// Cosine of two normalized vectors (their dot product).
[[nodiscard]] inline double Similarity(const Vector &a, const Vector &b) {
	if (a.size() != b.size() || a.empty()) {
		return 0.;
	}
	auto sum = 0.;
	for (auto i = size_t(0); i != a.size(); ++i) {
		sum += double(a[i]) * b[i];
	}
	return sum;
}

// Makes sure every text has its vector in the cache (only the missing ones
// are asked for, up to fifty at a time). `done(ok)`: false without a Gemini
// key or when it answered badly; whatever was made stays cached.
inline void Prepare(QStringList texts, Fn<void(bool ok)> done) {
	auto missing = QStringList();
	auto seen = std::set<QString>();
	for (auto text : texts) {
		text = text.simplified().left(600);
		if (text.isEmpty() || !seen.insert(text).second || Cached(text)) {
			continue;
		}
		missing.push_back(text);
	}
	if (missing.isEmpty()) {
		done(true);
		return;
	}
	const auto queue = std::make_shared<QStringList>(missing);
	const auto step = std::make_shared<Fn<void()>>();
	*step = [=] {
		if (queue->isEmpty()) {
			details::SaveCache();
			done(true);
			return;
		}
		const auto key = details::Key();
		if (!key) {
			details::SaveCache();
			done(false);
			return;
		}
		const auto batch = queue->mid(0, 50);
		queue->erase(queue->begin(), queue->begin() + batch.size());
		auto input = QJsonArray();
		for (const auto &text : batch) {
			input.push_back(Core::GgtgAi::details::Mask(text).text);
		}
		auto request = QNetworkRequest(QUrl(
			CloudProviderById(u"gemini"_q)->baseUrl + u"/embeddings"_q));
		request.setHeader(
			QNetworkRequest::ContentTypeHeader,
			u"application/json"_q);
		request.setRawHeader(
			"Authorization",
			("Bearer " + key->key.trimmed()).toUtf8());
		request.setTransferTimeout(60'000);
		const auto reply = Core::GgtgAi::details::Manager().post(
			request,
			QJsonDocument(QJsonObject{
				{ "input", input },
				{ "model", kModel },
			}).toJson(QJsonDocument::Compact));
		QObject::connect(reply, &QNetworkReply::finished, reply, [=] {
			const auto data = reply->readAll();
			const auto failed = (reply->error() != QNetworkReply::NoError);
			const auto status = reply->attribute(
				QNetworkRequest::HttpStatusCodeAttribute).toInt();
			const auto retryAfter = reply->rawHeader("Retry-After");
			reply->deleteLater();
			const auto rows = QJsonDocument::fromJson(data).object()
				.value("data").toArray();
			if (failed || rows.size() != batch.size()) {
				Core::GgtgAi::details::Failed(
					{ *key, QString(kModel) },
					failed ? status : 0,
					data,
					retryAfter);
				details::SaveCache();
				done(false);
				return;
			}
			auto &cache = details::CacheState();
			for (auto i = 0; i != batch.size(); ++i) {
				const auto numbers = rows.at(i).toObject()
					.value("embedding").toArray();
				if (numbers.isEmpty()) {
					continue;
				}
				auto vector = Vector();
				vector.reserve(kDimensions);
				for (const auto &number : numbers) {
					vector.push_back(float(number.toDouble()));
					if (int(vector.size()) == kDimensions) {
						break;
					}
				}
				details::Normalize(vector);
				cache.vectors[details::KeyOf(batch[i])] = std::move(vector);
				cache.dirty = true;
			}
			(*step)();
		});
	};
	(*step)();
}

// Forgets every vector made so far.
inline void ClearCache() {
	QFile::remove(details::CachePath());
	details::CacheState().vectors.clear();
}

// Nearest of `corpus` (texts that must have been Prepare()d too) to `query`,
// best first: pairs of similarity and index. Texts without a vector are
// skipped.
[[nodiscard]] inline std::vector<std::pair<double, int>> Nearest(
		const QString &query,
		const QStringList &corpus,
		int count) {
	auto result = std::vector<std::pair<double, int>>();
	const auto q = Cached(query);
	if (!q) {
		return result;
	}
	for (auto i = 0; i != corpus.size(); ++i) {
		if (const auto v = Cached(corpus[i])) {
			result.push_back({ Similarity(*q, *v), i });
		}
	}
	std::sort(result.begin(), result.end(), [](auto &a, auto &b) {
		return a.first > b.first;
	});
	if (int(result.size()) > count) {
		result.resize(count);
	}
	return result;
}

} // namespace Core::GgtgAi::Embed
