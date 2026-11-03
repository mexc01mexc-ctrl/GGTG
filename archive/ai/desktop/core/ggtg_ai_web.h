/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and exceptions, see https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "core/ggtg_ai.h"

#include <QtCore/QUrlQuery>

// The internet for the "ИИ-агент": a web search and reading one page, free
// and without keys. The search asks the ordinary result pages of Brave
// Search, then Bing, then DuckDuckGo (one of them refuses a computer now and
// then, typically behind a VPN). Google is not asked: its result page needs a
// real browser. A page is read straight from its site; a page that is built
// by scripts and comes back empty is read through r.jina.ai instead.
namespace Core::GgtgAi::Web {

struct Hit {
	QString title;
	QString url;
	QString snippet;
};

namespace details {

constexpr auto kMaxResponse = qint64(3) * 1024 * 1024;
constexpr auto kHits = 6;

[[nodiscard]] inline QByteArray Agent() {
	return "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) "
		"AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130.0 Safari/537.36";
}

[[nodiscard]] inline QString DecodeEntities(QString text) {
	static const auto numeric = QRegularExpression(
		u"&#(x[0-9a-fA-F]+|[0-9]+);"_q);
	auto result = QString();
	auto last = 0;
	auto i = numeric.globalMatch(text);
	while (i.hasNext()) {
		const auto match = i.next();
		result += text.mid(last, match.capturedStart() - last);
		const auto raw = match.captured(1);
		auto ok = false;
		const auto code = raw.startsWith('x')
			? raw.mid(1).toUInt(&ok, 16)
			: raw.toUInt(&ok, 10);
		if (ok && code > 0 && code < 0x110000) {
			const auto value = char32_t(code);
			result += QString::fromUcs4(&value, 1);
		}
		last = match.capturedEnd();
	}
	result += text.mid(last);
	result.replace(u"&nbsp;"_q, u" "_q);
	result.replace(u"&lt;"_q, u"<"_q);
	result.replace(u"&gt;"_q, u">"_q);
	result.replace(u"&quot;"_q, u"\""_q);
	result.replace(u"&apos;"_q, u"'"_q);
	result.replace(u"&mdash;"_q, u"-"_q);
	result.replace(u"&ndash;"_q, u"-"_q);
	result.replace(u"&hellip;"_q, u"..."_q);
	result.replace(u"&laquo;"_q, u"«"_q);
	result.replace(u"&raquo;"_q, u"»"_q);
	result.replace(u"&amp;"_q, u"&"_q);
	return result;
}

// A piece of markup as one line of text.
[[nodiscard]] inline QString InlineText(QString html) {
	static const auto tags = QRegularExpression(u"<[^>]*>"_q);
	static const auto spaces = QRegularExpression(u"\\s+"_q);
	html.remove(tags);
	return DecodeEntities(html).replace(spaces, u" "_q).trimmed();
}

// A whole page as text: scripts, styles and menus out, one paragraph per
// line. Short lines (menus, buttons, footers) are dropped unless they end
// like a sentence or a heading.
[[nodiscard]] inline QString PageText(QString html) {
	static const auto blocks = QRegularExpression(
		u"<(script|style|noscript|svg|head|nav|footer|form|iframe)\\b"
			"[^>]*>.*?</\\1\\s*>"_q,
		QRegularExpression::DotMatchesEverythingOption
			| QRegularExpression::CaseInsensitiveOption);
	static const auto comments = QRegularExpression(
		u"<!--.*?-->"_q,
		QRegularExpression::DotMatchesEverythingOption);
	static const auto breaks = QRegularExpression(
		u"<(br|/p|/div|/li|/h[1-6]|/tr|/section|/article|/blockquote)"
			"\\b[^>]*>"_q,
		QRegularExpression::CaseInsensitiveOption);
	static const auto items = QRegularExpression(
		u"<li\\b[^>]*>"_q,
		QRegularExpression::CaseInsensitiveOption);
	static const auto tags = QRegularExpression(u"<[^>]*>"_q);
	static const auto spaces = QRegularExpression(u"[ \\t\\x{00A0}]+"_q);
	html.remove(comments);
	html.remove(blocks);
	html.replace(breaks, u"\n"_q);
	html.replace(items, u"\n• "_q);
	html.remove(tags);
	html = DecodeEntities(html);
	auto lines = QStringList();
	for (auto line : html.split('\n')) {
		line = line.replace(spaces, u" "_q).trimmed();
		if (line.isEmpty() || line == u"•"_q) {
			continue;
		}
		const auto ending = line.back();
		const auto sentence = (ending == '.' || ending == '!'
			|| ending == '?' || ending == ':');
		if (line.size() < 40 && !sentence) {
			continue;
		}
		if (!lines.isEmpty() && lines.back() == line) {
			continue;
		}
		lines.push_back(line);
	}
	return lines.join('\n');
}

[[nodiscard]] inline QString PageTitle(const QString &html) {
	static const auto title = QRegularExpression(
		u"<title[^>]*>(.*?)</title>"_q,
		QRegularExpression::DotMatchesEverythingOption
			| QRegularExpression::CaseInsensitiveOption);
	const auto match = title.match(html);
	return match.hasMatch() ? InlineText(match.captured(1)) : QString();
}

inline void Get(
		const QUrl &url,
		Fn<void(QByteArray data, QString contentType, QString error)> done,
		const QByteArray &post = QByteArray()) {
	auto request = QNetworkRequest(url);
	request.setRawHeader("User-Agent", Agent());
	request.setRawHeader(
		"Accept",
		"text/html,application/xhtml+xml,text/plain;q=0.9,*/*;q=0.8");
	request.setRawHeader("Accept-Language", "ru,en;q=0.8");
	request.setAttribute(
		QNetworkRequest::RedirectPolicyAttribute,
		QNetworkRequest::NoLessSafeRedirectPolicy);
	request.setTransferTimeout(20'000);
	auto &manager = Core::GgtgAi::details::Manager();
	QNetworkReply *reply = nullptr;
	if (post.isEmpty()) {
		reply = manager.get(request);
	} else {
		request.setHeader(
			QNetworkRequest::ContentTypeHeader,
			u"application/x-www-form-urlencoded"_q);
		reply = manager.post(request, post);
	}
	// A huge file (a video, an archive) is not read to the end.
	QObject::connect(reply, &QNetworkReply::downloadProgress, reply, [=](
			qint64 received,
			qint64) {
		if (received > kMaxResponse) {
			reply->abort();
		}
	});
	QObject::connect(reply, &QNetworkReply::finished, reply, [=] {
		reply->deleteLater();
		const auto status = reply->attribute(
			QNetworkRequest::HttpStatusCodeAttribute).toInt();
		const auto type = reply->header(
			QNetworkRequest::ContentTypeHeader).toString().toLower();
		if (status >= 400) {
			done({}, type, u"ответ "_q + QString::number(status));
		} else if (reply->error() != QNetworkReply::NoError) {
			done({}, type, reply->errorString());
		} else if (status != 200) {
			done({}, type, u"ответ "_q + QString::number(status));
		} else {
			done(reply->readAll(), type, QString());
		}
	});
}

[[nodiscard]] inline std::vector<Hit> ParseBrave(const QString &html) {
	static const auto link = QRegularExpression(
		u"<a href=\"(https?://[^\"]+)\""_q);
	static const auto title = QRegularExpression(
		u"search-snippet-title[^\"]*\"[^>]*title=\"([^\"]*)\""_q);
	static const auto snippet = QRegularExpression(
		u"class=\"content[^\"]*\">(.*?)</div>"_q,
		QRegularExpression::DotMatchesEverythingOption);
	auto result = std::vector<Hit>();
	const auto chunks = html.split(u"data-type=\"web\""_q);
	for (auto i = 1; i < chunks.size(); ++i) {
		const auto &chunk = chunks[i];
		const auto url = link.match(chunk);
		if (!url.hasMatch()) {
			continue;
		}
		result.push_back({
			.title = DecodeEntities(title.match(chunk).captured(1)),
			.url = DecodeEntities(url.captured(1)),
			.snippet = InlineText(snippet.match(chunk).captured(1)),
		});
	}
	return result;
}

// Bing's links go through its own counter: the real address sits in the
// `u` parameter as "a1" + base64.
[[nodiscard]] inline QString BingTarget(QString url) {
	url = DecodeEntities(url);
	static const auto param = QRegularExpression(u"[?&]u=a1([^&]+)"_q);
	const auto match = param.match(url);
	if (!match.hasMatch()) {
		return url;
	}
	const auto decoded = QString::fromUtf8(QByteArray::fromBase64(
		match.captured(1).toLatin1(),
		QByteArray::Base64UrlEncoding));
	return decoded.startsWith(u"http"_q) ? decoded : url;
}

[[nodiscard]] inline std::vector<Hit> ParseBing(const QString &html) {
	static const auto heading = QRegularExpression(
		u"<h2[^>]*>\\s*<a[^>]*href=\"([^\"]+)\"[^>]*>(.*?)</a>"_q,
		QRegularExpression::DotMatchesEverythingOption);
	static const auto paragraph = QRegularExpression(
		u"<p[^>]*>(.*?)</p>"_q,
		QRegularExpression::DotMatchesEverythingOption);
	auto result = std::vector<Hit>();
	const auto chunks = html.split(u"<li class=\"b_algo\""_q);
	for (auto i = 1; i < chunks.size(); ++i) {
		const auto chunk = chunks[i].left(chunks[i].indexOf(u"</li>"_q));
		const auto match = heading.match(chunk);
		if (!match.hasMatch()) {
			continue;
		}
		result.push_back({
			.title = InlineText(match.captured(2)),
			.url = BingTarget(match.captured(1)),
			.snippet = InlineText(paragraph.match(chunk).captured(1)),
		});
	}
	return result;
}

[[nodiscard]] inline std::vector<Hit> ParseDuck(const QString &html) {
	static const auto link = QRegularExpression(
		u"class=\"result__a\"[^>]*href=\"([^\"]+)\"[^>]*>(.*?)</a>"_q,
		QRegularExpression::DotMatchesEverythingOption);
	static const auto snippet = QRegularExpression(
		u"class=\"result__snippet\"[^>]*>(.*?)</a>"_q,
		QRegularExpression::DotMatchesEverythingOption);
	auto result = std::vector<Hit>();
	const auto chunks = html.split(u"class=\"result results_links"_q);
	for (auto i = 1; i < chunks.size(); ++i) {
		const auto &chunk = chunks[i];
		const auto match = link.match(chunk);
		if (!match.hasMatch()) {
			continue;
		}
		auto url = DecodeEntities(match.captured(1));
		const auto query = QUrlQuery(QUrl(url.startsWith(u"//"_q)
			? (u"https:"_q + url)
			: url));
		if (query.hasQueryItem(u"uddg"_q)) {
			url = query.queryItemValue(u"uddg"_q, QUrl::FullyDecoded);
		}
		result.push_back({
			.title = InlineText(match.captured(2)),
			.url = url,
			.snippet = InlineText(snippet.match(chunk).captured(1)),
		});
	}
	return result;
}

struct Provider {
	QString name;
	QUrl url;
	QByteArray post;
	std::vector<Hit>(*parse)(const QString&) = nullptr;
};

inline void SearchFrom(
		std::shared_ptr<std::vector<Provider>> providers,
		int index,
		std::shared_ptr<QStringList> errors,
		Fn<void(std::vector<Hit> hits, QString error)> done) {
	if (index >= int(providers->size())) {
		done({}, u"поиск не ответил ("_q + errors->join(u", "_q) + ')');
		return;
	}
	const auto &provider = (*providers)[index];
	Get(provider.url, [=](QByteArray data, QString, QString error) {
		const auto &provider = (*providers)[index];
		auto hits = error.isEmpty()
			? provider.parse(QString::fromUtf8(data))
			: std::vector<Hit>();
		hits.erase(ranges::remove_if(hits, [](const Hit &hit) {
			return !hit.url.startsWith(u"http"_q);
		}), end(hits));
		if (hits.empty()) {
			errors->push_back(provider.name + u": "_q
				+ (error.isEmpty() ? u"пусто"_q : error));
			SearchFrom(providers, index + 1, errors, done);
			return;
		}
		if (int(hits.size()) > kHits) {
			hits.resize(kHits);
		}
		done(std::move(hits), QString());
	}, provider.post);
}

} // namespace details

// Up to six results: title, address and a short piece of text. `done` runs
// on the main thread; `error` is set only when every search refused.
inline void Search(
		const QString &query,
		Fn<void(std::vector<Hit> hits, QString error)> done) {
	using details::Provider;
	const auto encoded = QString::fromLatin1(QUrl::toPercentEncoding(query));
	auto providers = std::make_shared<std::vector<Provider>>();
	providers->push_back({
		.name = u"Brave"_q,
		.url = QUrl(u"https://search.brave.com/search?source=web&q="_q
			+ encoded),
		.parse = &details::ParseBrave,
	});
	providers->push_back({
		.name = u"Bing"_q,
		.url = QUrl(u"https://www.bing.com/search?count=10&q="_q + encoded),
		.parse = &details::ParseBing,
	});
	providers->push_back({
		.name = u"DuckDuckGo"_q,
		.url = QUrl(u"https://html.duckduckgo.com/html/"_q),
		.post = "q=" + QUrl::toPercentEncoding(query),
		.parse = &details::ParseDuck,
	});
	details::SearchFrom(
		std::move(providers),
		0,
		std::make_shared<QStringList>(),
		std::move(done));
}

// The text of one page (up to `limit` characters) and its title.
inline void FetchPage(
		const QString &address,
		int limit,
		Fn<void(QString title, QString text, QString error)> done) {
	auto url = QUrl(address.trimmed());
	if (url.scheme().isEmpty()) {
		url = QUrl(u"https://"_q + address.trimmed());
	}
	if (!url.isValid()
		|| (url.scheme() != u"https"_q && url.scheme() != u"http"_q)) {
		done({}, {}, u"неверный адрес"_q);
		return;
	}
	const auto cut = [=](QString text) {
		if (text.size() > limit) {
			text = text.left(limit) + u"\n(дальше страница обрезана)"_q;
		}
		return text;
	};
	// Through r.jina.ai: it opens the page in a real browser and gives the
	// text back - for pages that are empty without their scripts.
	const auto viaReader = [=](QString title, QString reason) {
		details::Get(
			QUrl(u"https://r.jina.ai/"_q + url.toString()),
			[=](QByteArray data, QString, QString error) {
				const auto text = QString::fromUtf8(data).trimmed();
				if (!error.isEmpty() || text.size() < 200) {
					done(
						title,
						{},
						reason.isEmpty() ? u"страница пустая"_q : reason);
					return;
				}
				done(title, cut(text), QString());
			});
	};
	details::Get(url, [=](QByteArray data, QString type, QString error) {
		if (!error.isEmpty()) {
			viaReader(QString(), u"сайт не открылся ("_q + error + ')');
			return;
		}
		if (type.contains(u"pdf"_q)) {
			viaReader(QString(), u"это PDF, он не читается"_q);
			return;
		}
		const auto html = QString::fromUtf8(data);
		const auto title = details::PageTitle(html);
		const auto text = (type.contains(u"html"_q) || type.isEmpty())
			? details::PageText(html)
			: html.trimmed();
		if (text.size() < 400) {
			viaReader(title, QString());
			return;
		}
		done(title, cut(text), QString());
	});
}

} // namespace Core::GgtgAi::Web
