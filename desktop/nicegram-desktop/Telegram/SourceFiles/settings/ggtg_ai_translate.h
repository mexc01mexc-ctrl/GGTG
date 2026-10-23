/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and exceptions, see https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "core/ggtg_ai.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"
#include "base/weak_ptr.h"
#include "data/data_session.h"
#include "history/history_item.h"
#include "main/main_session.h"
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_widgets.h"
#include "styles/style_window_main_menu.h" // ggtgTokenButton

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QUrlQuery>
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

namespace Settings {

// Free online translation of a chat message: no key, no account, no model to
// download. Only the text being translated is sent. Several public services
// are tried one after another - a single one refuses requests now and then
// (typically from a VPN's address): Google's own address, the same engine
// through the Chrome extension's address, and Lingva.
// `done(result, detectedLanguage, error)`.
inline void TryOnlineTranslate(
		int provider,
		const QString &text,
		const QString &target,
		const QString &lastError,
		Fn<void(QString result, QString detected, QString error)> done) {
	if (provider > 2) {
		done(
			QString(),
			QString(),
			u"сервисы перевода сейчас недоступны ("_q + lastError
				+ u"). Если включён VPN, попробуйте выключить его или "
					"повторить позже"_q);
		return;
	}
	auto request = QNetworkRequest();
	request.setHeader(
		QNetworkRequest::UserAgentHeader,
		u"Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 "
			"(KHTML, like Gecko) Chrome/124.0 Safari/537.36"_q);
	request.setTransferTimeout(20'000);
	QNetworkReply *reply = nullptr;
	auto &manager = Core::GgtgAi::details::Manager();
	if (provider == 0) {
		auto url = QUrl(u"https://translate.googleapis.com/translate_a/single"_q);
		auto query = QUrlQuery();
		query.addQueryItem(u"client"_q, u"gtx"_q);
		query.addQueryItem(u"sl"_q, u"auto"_q);
		query.addQueryItem(u"tl"_q, target);
		query.addQueryItem(u"dt"_q, u"t"_q);
		url.setQuery(query);
		request.setUrl(url);
		request.setHeader(
			QNetworkRequest::ContentTypeHeader,
			u"application/x-www-form-urlencoded"_q);
		auto body = QUrlQuery();
		body.addQueryItem(u"q"_q, text);
		reply = manager.post(
			request,
			body.query(QUrl::FullyEncoded).toUtf8());
	} else if (provider == 1) {
		auto url = QUrl(u"https://clients5.google.com/translate_a/t"_q);
		auto query = QUrlQuery();
		query.addQueryItem(u"client"_q, u"dict-chrome-ex"_q);
		query.addQueryItem(u"sl"_q, u"auto"_q);
		query.addQueryItem(u"tl"_q, target);
		query.addQueryItem(u"q"_q, text.left(1800));
		url.setQuery(query);
		request.setUrl(url);
		reply = manager.get(request);
	} else {
		request.setUrl(QUrl::fromEncoded(
			(u"https://lingva.ml/api/v1/auto/"_q + target + '/').toUtf8()
				+ QUrl::toPercentEncoding(text.left(1500))));
		reply = manager.get(request);
	}
	QObject::connect(reply, &QNetworkReply::finished, reply, [=] {
		const auto data = reply->readAll();
		const auto status = reply->attribute(
			QNetworkRequest::HttpStatusCodeAttribute).toInt();
		const auto failed = (reply->error() != QNetworkReply::NoError);
		reply->deleteLater();
		auto result = QString();
		auto detected = QString();
		if (!failed) {
			const auto document = QJsonDocument::fromJson(data);
			if (provider == 0) {
				const auto root = document.array();
				for (const auto &part : root.at(0).toArray()) {
					result += part.toArray().at(0).toString();
				}
				detected = root.at(2).toString();
			} else if (provider == 1) {
				const auto first = document.array().at(0);
				if (first.isArray()) {
					result = first.toArray().at(0).toString();
					detected = first.toArray().at(1).toString();
				} else {
					result = first.toString();
				}
			} else {
				result = document.object().value("translation").toString();
			}
		}
		if (result.isEmpty()) {
			TryOnlineTranslate(
				provider + 1,
				text,
				target,
				failed ? (u"http "_q + QString::number(status)) : u"пусто"_q,
				done);
		} else {
			done(result, detected, QString());
		}
	});
}

inline void TranslateOnline(
		const QString &text,
		const QString &target,
		Fn<void(QString result, QString detected, QString error)> done) {
	TryOnlineTranslate(0, text, target, QString(), std::move(done));
}

// Google's code for a language of the "ИИ" settings.
[[nodiscard]] inline QString OnlineLanguageCode(const QString &code) {
	if (code == u"zh"_q || code == u"zh-CN"_q) return u"zh-CN"_q;
	if (code == u"zh-TW"_q) return u"zh-TW"_q;
	if (code == u"lat"_q) return u"en"_q;
	return code.isEmpty() ? u"en"_q : code;
}

// Translates a message template using free online Google/Lingva translator (no AI keys).
// Preserves [NAME], [SURNAME], [MYNAME], [MYSURNAME], [GROUP], [SPLIT] and {a|b|c} tokens intact.
inline void TranslateTemplateFree(
		const QString &templateText,
		const QString &targetCode,
		Fn<void(QString result, QString error)> done) {
	static auto cache = std::map<QString, QString>();
	static auto pending = std::map<QString, std::vector<Fn<void(QString, QString)>>>();

	const auto normTarget = OnlineLanguageCode(targetCode);
	const auto key = normTarget + '\n' + templateText;
	if (const auto it = cache.find(key); it != cache.end()) {
		done(it->second, QString());
		return;
	}
	auto &waiting = pending[key];
	waiting.push_back(std::move(done));
	if (waiting.size() > 1) {
		return;
	}

	struct Piece {
		QString text;
		bool translate = false;
	};
	auto pieces = std::make_shared<std::vector<Piece>>();
	auto run = QString();
	const auto flush = [&] {
		if (!run.isEmpty()) {
			static const auto letters = QRegularExpression(u"\\p{L}"_q);
			pieces->push_back({ run, run.contains(letters) });
			run.clear();
		}
	};
	for (auto i = 0; i != templateText.size(); ++i) {
		const auto ch = templateText[i];
		if (ch == '[') {
			const auto end = templateText.indexOf(']', i);
			if (end > i) {
				flush();
				const auto token = templateText.mid(i, end - i + 1);
				static const auto fallback = QRegularExpression(
					u"^(\\[(?:NAME|SURNAME|MYNAME|MYSURNAME|GROUP)\\|)(.*)(\\])$"_q);
				const auto match = fallback.match(token);
				if (match.hasMatch()) {
					static const auto letters = QRegularExpression(u"\\p{L}"_q);
					pieces->push_back({ match.captured(1), false });
					pieces->push_back({
						match.captured(2),
						match.captured(2).contains(letters),
					});
					pieces->push_back({ match.captured(3), false });
				} else {
					pieces->push_back({ token, false });
				}
				i = end;
				continue;
			}
		}
		if (ch == '{' || ch == '}' || ch == '|') {
			flush();
			pieces->push_back({ QString(ch), false });
			continue;
		}
		run += ch;
	}
	flush();

	const auto finishAll = [=](QString result, QString error) {
		if (error.isEmpty() && !result.isEmpty()) {
			cache[key] = result;
		}
		auto callbacks = std::move(pending[key]);
		pending.erase(key);
		for (const auto &cb : callbacks) {
			cb(result, error);
		}
	};

	const auto index = std::make_shared<int>(0);
	const auto step = std::make_shared<Fn<void()>>();
	*step = [=] {
		while (*index < int(pieces->size()) && !(*pieces)[*index].translate) {
			++*index;
		}
		if (*index >= int(pieces->size())) {
			auto full = QString();
			for (const auto &p : *pieces) {
				full += p.text;
			}
			finishAll(full, QString());
			return;
		}
		const auto current = *index;
		const auto original = (*pieces)[current].text;
		const auto words = original.trimmed();
		const auto start = original.indexOf(words);
		const auto lead = original.left(start);
		const auto tail = original.mid(start + words.size());
		TranslateOnline(words, normTarget, [=](QString translated, QString detected, QString error) {
			if (!error.isEmpty()) {
				finishAll(QString(), error);
				return;
			}
			(*pieces)[current].text = lead + translated + tail;
			++*index;
			(*step)();
		});
	};
	(*step)();
}

// "Перевести" in a message's right-click menu: the text is translated by the
// free online translator and the message itself shows the translation (the
// language is the one set in "ИИ"); "Показать оригинал" brings the text back.
// The translation is kept, so switching back and forth costs nothing, and
// "Copy Text" gives whatever is shown.
inline void ShowItemTranslation(
		not_null<Window::SessionController*> controller,
		not_null<HistoryItem*> item) {
	if (item->hasFreeTranslation()) {
		item->showFreeTranslation(true);
		return;
	}
	// A very long message is translated by its beginning; the rest stays as
	// it is, so nothing is lost.
	constexpr auto kMaxLength = 4500;
	const auto full = item->originalText().text;
	const auto source = full.left(kMaxLength);
	const auto rest = full.mid(source.size());
	if (source.trimmed().isEmpty()) {
		return;
	}
	const auto target = Core::GgtgAi::Load().translateTo;
	const auto id = item->fullId();
	const auto weak = base::make_weak(controller);
	TranslateOnline(
		source,
		OnlineLanguageCode(target),
		[=](QString result, QString detected, QString error) {
			const auto strong = weak.get();
			if (!strong) {
				return;
			}
			if (!error.isEmpty()) {
				strong->showToast(u"Не удалось перевести: "_q + error);
				return;
			}
			if (detected.left(2) == target.left(2)) {
				strong->showToast(u"Сообщение уже на выбранном языке "
					"(его меняют в разделе «ИИ»)"_q);
				return;
			}
			if (const auto message = strong->session().data().message(id)) {
				message->setFreeTranslation(
					TextWithEntities{ result + rest },
					true);
			}
		});
}


// "Перевести выделенное" for the text selected in the input field: a small
// window with the translation, the field's text is not touched. The language
// is the one of the conversation (`conversation`, a code, may be empty); text
// that already is in it goes to the language set in "ИИ" instead.
inline void ShowTextTranslation(
		not_null<Window::SessionController*> controller,
		const QString &text,
		const QString &conversation) {
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"Перевод"_q));
		box->setWidth(st::boxWideWidth);
		const auto source = text.trimmed().left(3000);
		auto from = Core::GgtgAi::DetectLanguage(source);
		if (from == u"en"_q) {
			from = Core::GgtgAi::DetectLatinLanguage(source);
		}
		auto target = conversation.isEmpty() ? u"en"_q : conversation;
		if (from == target) {
			target = Core::GgtgAi::Load().translateTo;
		}
		if (from == target) {
			target = (from == u"en"_q) ? u"ru"_q : u"en"_q;
		}
		const auto &language = Core::GgtgAi::LanguageByCode(target);
		struct State {
			QString result;
		};
		const auto state = box->lifetime().make_state<State>();
		const auto original = box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			source.size() > 500 ? (source.left(500) + u"…"_q) : source,
			st::boxDividerLabel));
		original->setSelectable(true);
		const auto note = box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			u"Перевожу на "_q + language.russian + u"..."_q,
			st::boxDividerLabel));
		const auto output = box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			QString(),
			st::boxLabel));
		output->setSelectable(true);
		TranslateOnline(
			source,
			OnlineLanguageCode(target),
			crl::guard(box, [=](
					QString result,
					QString detected,
					QString error) {
				if (!error.isEmpty()) {
					note->setText(u"Не вышло: "_q + error);
					return;
				}
				state->result = result;
				note->setText(u"На "_q + language.russian + ':');
				output->setText(result);
			}));
		box->addButton(rpl::single(u"Закрыть"_q), [=] {
			box->closeBox();
		});
		box->addLeftButton(rpl::single(u"Копировать перевод"_q), [=] {
			if (!state->result.isEmpty()) {
				QGuiApplication::clipboard()->setText(state->result);
				box->uiShow()->showToast(u"Скопировано"_q);
			}
		});
	}));
}

} // namespace Settings
