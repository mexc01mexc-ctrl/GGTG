/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and exceptions, see https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "core/ggtg_name_langs_data.h"
#include "core/ggtg_paths.h"
#include "base/basic_types.h"

#include <algorithm>
#include <vector>

#include <QtCore/QFile>
#include <QtCore/QHash>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QRegularExpression>
#include <QtCore/QSaveFile>
#include <QtNetwork/QNetworkAccessManager>
#include <QtWidgets/QApplication>

// ggtg: what is left of the former "ИИ" section after it was taken out of the program (the code
// of the models, the helper, the feed and the agent is kept in archive/ai/): the languages of
// the texts (by letters), the clean first name for a greeting, and the settings of the free
// translation of messages and of a mailing template. Nothing here talks to a model.
namespace Core::GgtgLang {

namespace details {

[[nodiscard]] inline QNetworkAccessManager &Manager() {
	static auto result = new QNetworkAccessManager(qApp);
	return *result;
}

} // namespace details

struct Settings {
	// "Перевести" in a message's right-click menu, and what to translate into (a code from Languages()).
	bool messageTranslate = true;
	QString translateTo = u"ru"_q;
	// "Отправка SMS": every recipient gets the template in their own language.
	bool smsTranslate = false;
	QString smsTranslateLang = u"auto"_q;
};

[[nodiscard]] inline QString SettingsPath() {
	return Core::GgtgPaths::DbDir() + u"translate_settings.json"_q;
}

// The settings of the former ai_settings.json are read once when there is no own file yet.
[[nodiscard]] inline Settings Load() {
	auto result = Settings();
	auto file = QFile(SettingsPath());
	if (!file.open(QIODevice::ReadOnly)) {
		file.setFileName(Core::GgtgPaths::DbDir() + u"ai_settings.json"_q);
		if (!file.open(QIODevice::ReadOnly)) {
			return result;
		}
	}
	const auto object = QJsonDocument::fromJson(file.readAll()).object();
	result.messageTranslate = object.value("messageTranslate").toBool(result.messageTranslate);
	result.translateTo = object.value("translateTo").toString(result.translateTo);
	result.smsTranslate = object.value("smsTranslate").toBool(result.smsTranslate);
	result.smsTranslateLang = object.value("smsTranslateLang").toString(result.smsTranslateLang);
	if (result.smsTranslateLang.isEmpty()) {
		result.smsTranslateLang = u"auto"_q;
	}
	return result;
}

[[nodiscard]] inline Settings &CachedSettings() {
	static auto result = Load();
	return result;
}

inline void Save(const Settings &settings) {
	CachedSettings() = settings;
	auto file = QSaveFile(SettingsPath());
	if (!file.open(QIODevice::WriteOnly)) {
		return;
	}
	file.write(QJsonDocument(QJsonObject{
		{ "messageTranslate", settings.messageTranslate },
		{ "translateTo", settings.translateTo },
		{ "smsTranslate", settings.smsTranslate },
		{ "smsTranslateLang", settings.smsTranslateLang.isEmpty() ? u"auto"_q : settings.smsTranslateLang },
	}).toJson(QJsonDocument::Indented));
	file.commit();
}

struct Language {
	QString code;
	QString english;
	QString russian;
	QString chinese;
};

[[nodiscard]] inline const std::vector<Language> &Languages() {
	static const auto result = std::vector<Language>{
		{ u"en"_q, u"English"_q, u"английский"_q, u"英语"_q },
		{ u"ru"_q, u"Russian"_q, u"русский"_q, u"俄语"_q },
		{ u"ko"_q, u"Korean"_q, u"корейский"_q, u"韩语"_q },
		{ u"zh"_q, u"Chinese"_q, u"китайский"_q, u"中文"_q },
		{ u"ja"_q, u"Japanese"_q, u"японский"_q, u"日语"_q },
		{ u"es"_q, u"Spanish"_q, u"испанский"_q, u"西班牙语"_q },
		{ u"de"_q, u"German"_q, u"немецкий"_q, u"德语"_q },
		{ u"fr"_q, u"French"_q, u"французский"_q, u"法语"_q },
		{ u"pt"_q, u"Portuguese"_q, u"португальский"_q, u"葡萄牙语"_q },
		{ u"tr"_q, u"Turkish"_q, u"турецкий"_q, u"土耳其语"_q },
		{ u"ar"_q, u"Arabic"_q, u"арабский"_q, u"阿拉伯语"_q },
		{ u"uk"_q, u"Ukrainian"_q, u"украинский"_q, u"乌克兰语"_q },
		{ u"it"_q, u"Italian"_q, u"итальянский"_q, u"意大利语"_q },
		{ u"hi"_q, u"Hindi"_q, u"хинди"_q, u"印地语"_q },
		{ u"id"_q, u"Indonesian"_q, u"индонезийский"_q, u"印度尼西亚语"_q },
		{ u"vi"_q, u"Vietnamese"_q, u"вьетнамский"_q, u"越南语"_q },
		{ u"th"_q, u"Thai"_q, u"тайский"_q, u"泰语"_q },
		{ u"pl"_q, u"Polish"_q, u"польский"_q, u"波兰语"_q },
	};
	return result;
}

[[nodiscard]] inline const Language &LanguageByCode(const QString &code) {
	for (const auto &language : Languages()) {
		if (language.code == code) {
			return language;
		}
	}
	return Languages().front();
}

// The language of a text by its letters (the script is enough to tell a
// recipient's name or a template apart: Hangul, Cyrillic, Han...).
[[nodiscard]] inline QString DetectLanguage(const QString &text) {
	auto hangul = 0, cyrillic = 0, han = 0, kana = 0, arabic = 0, thai = 0;
	auto devanagari = 0, latin = 0, ukrainian = 0;
	for (const auto ch : text) {
		const auto code = ch.unicode();
		if ((code >= 0xAC00 && code <= 0xD7AF) || (code >= 0x1100 && code <= 0x11FF)) {
			++hangul;
		} else if (code >= 0x0400 && code <= 0x04FF) {
			++cyrillic;
			if (ch == QChar(0x0456) || ch == QChar(0x0457)
				|| ch == QChar(0x0454) || ch == QChar(0x0491)) {
				++ukrainian;
			}
		} else if (code >= 0x4E00 && code <= 0x9FFF) {
			++han;
		} else if (code >= 0x3040 && code <= 0x30FF) {
			++kana;
		} else if (code >= 0x0600 && code <= 0x06FF) {
			++arabic;
		} else if (code >= 0x0E00 && code <= 0x0E7F) {
			++thai;
		} else if (code >= 0x0900 && code <= 0x097F) {
			++devanagari;
		} else if (ch.isLetter() && code < 0x250) {
			++latin;
		}
	}
	const auto most = std::max({
		hangul, cyrillic, han, kana, arabic, thai, devanagari, latin });
	if (most == 0) {
		return QString();
	} else if (kana > 0 && kana * 4 >= han) {
		return u"ja"_q;
	} else if (most == hangul) {
		return u"ko"_q;
	} else if (most == cyrillic) {
		return (ukrainian > 0) ? u"uk"_q : u"ru"_q;
	} else if (most == han) {
		return u"zh"_q;
	} else if (most == arabic) {
		return u"ar"_q;
	} else if (most == thai) {
		return u"th"_q;
	} else if (most == devanagari) {
		return u"hi"_q;
	}
	return u"en"_q;
}

// Which Latin-script language a text is: the common words of English,
// Spanish, Turkish, French, German, Portuguese, Italian and Indonesian are
// counted; a text with no clear winner (a short "Super", "Ok") is English -
// by far the likeliest. Never empty.
[[nodiscard]] inline QString DetectLatinLanguage(const QString &text) {
	struct Words {
		QString code;
		QStringList list;
	};
	static const auto kWords = std::vector<Words>{
		{ u"en"_q, { u"the"_q, u"you"_q, u"and"_q, u"is"_q, u"are"_q,
			u"what"_q, u"how"_q, u"hello"_q, u"hi"_q, u"hey"_q, u"thanks"_q,
			u"please"_q, u"ok"_q, u"okay"_q, u"yes"_q, u"not"_q, u"this"_q,
			u"that"_q, u"for"_q, u"with"_q, u"can"_q, u"your"_q, u"have"_q,
			u"do"_q, u"i"_q, u"my"_q, u"me"_q, u"good"_q } },
		{ u"es"_q, { u"el"_q, u"la"_q, u"de"_q, u"que"_q, u"y"_q, u"en"_q,
			u"hola"_q, u"gracias"_q, u"por"_q, u"para"_q, u"con"_q,
			u"cómo"_q, u"como"_q, u"está"_q, u"estás"_q, u"muy"_q,
			u"bien"_q, u"sí"_q, u"no"_q, u"un"_q, u"una"_q, u"es"_q } },
		{ u"tr"_q, { u"merhaba"_q, u"selam"_q, u"evet"_q, u"hayır"_q,
			u"teşekkür"_q, u"tamam"_q, u"nasılsın"_q, u"bir"_q, u"bu"_q,
			u"ve"_q, u"ben"_q, u"sen"_q, u"için"_q, u"çok"_q, u"iyi"_q,
			u"güzel"_q, u"mi"_q, u"mı"_q } },
		{ u"fr"_q, { u"le"_q, u"les"_q, u"des"_q, u"est"_q, u"et"_q,
			u"bonjour"_q, u"salut"_q, u"merci"_q, u"oui"_q, u"non"_q,
			u"je"_q, u"tu"_q, u"vous"_q, u"pour"_q, u"avec"_q, u"très"_q,
			u"comment"_q } },
		{ u"de"_q, { u"der"_q, u"die"_q, u"das"_q, u"und"_q, u"ist"_q,
			u"nicht"_q, u"ich"_q, u"du"_q, u"hallo"_q, u"danke"_q,
			u"bitte"_q, u"ja"_q, u"nein"_q, u"wie"_q, u"mit"_q } },
		{ u"pt"_q, { u"olá"_q, u"obrigado"_q, u"obrigada"_q, u"você"_q,
			u"não"_q, u"sim"_q, u"uma"_q, u"para"_q, u"com"_q, u"muito"_q,
			u"está"_q, u"tudo"_q, u"bem"_q } },
		{ u"it"_q, { u"ciao"_q, u"grazie"_q, u"sono"_q, u"come"_q,
			u"stai"_q, u"che"_q, u"non"_q, u"per"_q, u"molto"_q, u"bene"_q,
			u"sì"_q, u"il"_q, u"della"_q } },
		{ u"id"_q, { u"halo"_q, u"terima"_q, u"kasih"_q, u"apa"_q,
			u"kabar"_q, u"saya"_q, u"kamu"_q, u"ya"_q, u"tidak"_q,
			u"yang"_q, u"dan"_q, u"untuk"_q, u"dengan"_q } },
	};
	static const auto split = QRegularExpression(u"[^\\p{L}]+"_q);
	const auto words = text.toLower().split(split, Qt::SkipEmptyParts);
	auto best = u"en"_q;
	auto bestScore = 0;
	for (const auto &entry : kWords) {
		auto score = 0;
		for (const auto &word : words) {
			score += entry.list.contains(word) ? 1 : 0;
		}
		// English wins ties: it is the usual one.
		if (score > bestScore) {
			best = entry.code;
			bestScore = score;
		}
	}
	return best;
}

// A name fit for "Привет, <name>!": letters only (emoji, symbols and digits
// of a profile name are dropped), and empty when what is left is clearly not
// a name - one letter, "qqqq", a keyboard run like "asdf", a word without a
// vowel, "user", "admin"...
[[nodiscard]] inline QString CleanPersonName(const QString &raw) {
	auto kept = QString();
	auto lower = QString();
	auto alphabetic = true; // Latin / Cyrillic only: these can be judged.
	auto lastWasLetter = false;
	for (const auto cp : raw.toUcs4()) {
		if (QChar::isLetter(cp)) {
			kept += QString::fromUcs4(&cp, 1);
			lower += QString::fromUcs4(&cp, 1).toLower();
			lastWasLetter = true;
			if (!((cp >= 0x41 && cp <= 0x24F)
				|| (cp >= 0x400 && cp <= 0x4FF))) {
				alphabetic = false;
			}
		} else if (QChar::isMark(cp) && lastWasLetter) {
			kept += QString::fromUcs4(&cp, 1);
		} else if ((cp == '\'' || cp == 0x2019 || cp == '-' || cp == ' ')
				&& lastWasLetter) {
			kept += QChar(cp == ' ' ? ' ' : QChar(cp));
			lastWasLetter = false;
		} else {
			lastWasLetter = false;
		}
	}
	kept = kept.simplified();
	while (kept.endsWith('-') || kept.endsWith('\'')
		|| kept.endsWith(QChar(0x2019))) {
		kept.chop(1);
	}
	if (lower.isEmpty()) {
		return QString();
	}
	if (alphabetic) {
		if (lower.size() < 2) {
			return QString();
		}
		auto same = true;
		for (const auto ch : lower) {
			same = same && (ch == lower[0]);
		}
		if (same) {
			return QString(); // "qqqq", "ааа"
		}
		static const auto vowels = QString(
			u"aeiouyаеёиоуыэюяіїєáéíóúàèìòùäöüâêîôûãõ"_q);
		auto hasVowel = false;
		for (const auto ch : lower) {
			hasVowel = hasVowel || vowels.contains(ch);
		}
		if (lower.size() >= 4 && !hasVowel) {
			return QString(); // "qwrt", "хзбж"
		}
		if (lower.size() >= 3) {
			static const auto rows = QStringList{
				u"qwertyuiop"_q, u"asdfghjkl"_q, u"zxcvbnm"_q,
				u"йцукенгшщзхъ"_q, u"фывапролджэ"_q, u"ячсмитьбю"_q,
			};
			for (const auto &row : rows) {
				auto reversed = row;
				std::reverse(reversed.begin(), reversed.end());
				if (row.contains(lower) || reversed.contains(lower)) {
					return QString(); // "asdf", "йцук"
				}
			}
		}
		static const auto notNames = QStringList{
			u"user"_q, u"admin"_q, u"test"_q, u"unknown"_q, u"noname"_q,
			u"none"_q, u"null"_q, u"telegram"_q, u"deleted"_q, u"anon"_q,
			u"anonymous"_q, u"name"_q, u"nickname"_q, u"nick"_q, u"bot"_q,
			u"account"_q, u"no name"_q, u"имя"_q, u"аноним"_q,
			u"аккаунт"_q, u"пользователь"_q, u"user name"_q,
		};
		if (notNames.contains(kept.toLower())) {
			return QString();
		}
	}
	return kept;
}

// What to call a person whose name is not usable, in the language of the
// text being sent ("друг"): empty when the language is not sure (a Latin
// text that does not look English) - the name is then simply left out.
[[nodiscard]] inline QString FriendWord(const QString &text) {
	const auto code = DetectLanguage(text);
	if (code == u"ru"_q) {
		return u"друг"_q;
	} else if (code == u"uk"_q) {
		return u"друже"_q;
	} else if (code == u"zh"_q) {
		return u"朋友"_q;
	} else if (code == u"ja"_q) {
		return u"友達"_q;
	} else if (code == u"ko"_q) {
		return u"친구"_q;
	} else if (code == u"ar"_q) {
		return u"صديقي"_q;
	} else if (code == u"th"_q) {
		return u"เพื่อน"_q;
	} else if (code == u"hi"_q) {
		return u"दोस्त"_q;
	} else if (code == u"en"_q) {
		static const auto english = QRegularExpression(
			u"\\b(the|you|your|and|hello|hi|hey|please|thanks|for|with|"
			"this|that|have|are|can)\\b"_q,
			QRegularExpression::CaseInsensitiveOption);
		return english.match(text).hasMatch() ? u"friend"_q : QString();
	}
	return QString();
}



// ---- the language of a person by the name (and what the group says) ----

struct LanguageGuess {
	QString code; // a code from Languages(); empty when the name says nothing
	int confidence = 0; // 0..100: 65 and more is safe to act on (translate, split a base)
	[[nodiscard]] bool sure() const {
		return !code.isEmpty() && confidence >= 65;
	}
};

namespace details {

struct TokenWeight {
	QString code;
	double weight = 0.;
};

// lower case, no diacritics: "Nguyễn" -> "nguyen"
[[nodiscard]] inline QString Plain(const QString &text) {
	auto result = QString();
	auto lower = text.toLower();
	lower.replace(QChar(0x0111), u"d"_q).replace(QChar(0x0142), u"l"_q).replace(QChar(0x0131), u"i"_q)
		.replace(QChar(0x00F8), u"o"_q).replace(QChar(0x00DF), u"ss"_q).replace(QChar(0x00E6), u"ae"_q);
	for (const auto ch : lower.normalized(QString::NormalizationForm_D)) {
		if (ch.category() != QChar::Mark_NonSpacing) {
			result += ch;
		}
	}
	return result;
}

[[nodiscard]] inline QStringList NameTokens(const QString &text) {
	static const auto split = QRegularExpression(u"[^a-z]+"_q);
	return Plain(text).split(split, Qt::SkipEmptyParts);
}

[[nodiscard]] inline const QHash<QString, std::vector<TokenWeight>> &TokenIndex() {
	static const auto result = [] {
		auto index = QHash<QString, std::vector<TokenWeight>>();
		const auto add = [&](const QString &code, const char *list, double weight) {
			for (const auto &token : QString::fromLatin1(list).split(' ', Qt::SkipEmptyParts)) {
				auto &entry = index[token];
				const auto same = std::find_if(entry.begin(), entry.end(), [&](const TokenWeight &w) {
					return w.code == code;
				});
				if (same == entry.end()) {
					entry.push_back({ code, weight });
				} else {
					same->weight = std::max(same->weight, weight);
				}
			}
		};
		for (const auto &dictionary : NameDictionaries()) {
			const auto code = QString::fromLatin1(dictionary.code);
			add(code, dictionary.strong, 1.0);
			add(code, dictionary.medium, 0.6);
			add(code, dictionary.weak, 0.3);
		}
		return index;
	}();
	return result;
}

// A letter that only one language has (Vietnamese tones, Turkish dotless i, Polish l with a stroke...).
[[nodiscard]] inline QString LatinMarkerLanguage(const QString &text) {
	const auto lower = text.toLower();
	const auto has = [&](const QString &letters) {
		for (const auto ch : lower) {
			if (letters.contains(ch)) {
				return true;
			}
		}
		return false;
	};
	if (has(u"ăơưđạảấầẩẫậắằẳẵặẹẻẽếềểễệỉĩịọỏốồổỗộớờởỡợụủứừửữựỳỵỷỹ"_q)) {
		return u"vi"_q;
	} else if (has(u"şğı"_q)) {
		return u"tr"_q;
	} else if (has(u"ąęłśćźżń"_q)) {
		return u"pl"_q;
	} else if (has(u"ñ"_q)) {
		return u"es"_q;
	} else if (has(u"ß"_q)) {
		return u"de"_q;
	} else if (has(u"ãõ"_q)) {
		return u"pt"_q;
	}
	return QString();
}

} // namespace details

// The language of a recipient: by the script of the name (Hangul, kana, Cyrillic...), and for a name in
// Latin letters by the letters only one language has and by the name dictionaries (surnames and given
// names of Vietnam, Korea, Japan, China, Indonesia, Thailand, Turkey, the Arab world, India, Spain,
// Brazil, Germany, France, Italy, Poland, Russia). The username counts a little (a part of it is often a
// name); `hint` is the language the group or the base is about (see LanguageFromTitle), it only adds
// weight to a language that already has some. A name nothing is known about gives an empty code.
[[nodiscard]] inline LanguageGuess GuessNameLanguage(
		const QString &first,
		const QString &last,
		const QString &username = QString(),
		const QString &hint = QString()) {
	const auto name = (first + ' ' + last).simplified();
	const auto script = DetectLanguage(name);
	if (!script.isEmpty() && script != u"en"_q) {
		if (script == u"zh"_q && hint == u"ja"_q) {
			return { u"ja"_q, 80 }; // kanji only: the group says Japanese
		}
		const auto confidence = (script == u"zh"_q)
			? 80
			: (script == u"ru"_q || script == u"uk"_q || script == u"ar"_q)
			? 85
			: 95;
		return { script, confidence };
	} else if (script.isEmpty() && username.isEmpty()) {
		return {};
	}
	if (const auto marker = details::LatinMarkerLanguage(name); !marker.isEmpty()) {
		return { marker, (marker == u"pt"_q) ? 75 : 88 };
	}
	auto scores = QHash<QString, double>();
	const auto &index = details::TokenIndex();
	const auto count = [&](const QStringList &tokens, double factor, int minLength) {
		for (const auto &token : tokens) {
			if (token.size() < minLength) {
				continue;
			}
			const auto i = index.constFind(token);
			if (i != index.cend()) {
				for (const auto &entry : i.value()) {
					scores[entry.code] += entry.weight * factor;
				}
			} else if (token.size() >= 5) {
				for (const auto &list : details::NameSuffixList()) {
					const auto code = QString::fromLatin1(list.code);
					for (const auto &suffix : QString::fromLatin1(list.suffixes).split(' ', Qt::SkipEmptyParts)) {
						if (token.endsWith(suffix) && token.size() > suffix.size() + 1) {
							scores[code] += 0.8 * factor;
							break;
						}
					}
				}
			}
		}
	};
	count(details::NameTokens(name), 1., 2);
	// A username is a weak sign: "nguyen_van_an" or "kimminjun92".
	count(details::NameTokens(username), 0.4, 4);
	if (!hint.isEmpty() && scores.value(hint) >= 0.3) {
		scores[hint] += 0.5;
	}
	auto best = QString();
	auto bestScore = 0., secondScore = 0.;
	for (auto i = scores.cbegin(); i != scores.cend(); ++i) {
		if (i.value() > bestScore) {
			secondScore = bestScore;
			bestScore = i.value();
			best = i.key();
		} else if (i.value() > secondScore) {
			secondScore = i.value();
		}
	}
	if (bestScore >= 1.0 && bestScore >= secondScore + 0.6) {
		return { best, std::min(90, 60 + int(bestScore * 15)) };
	}
	return {};
}

// The language a group or a base is about, by its title: "Корея чат", "Vietnam business", "KR traders".
// Empty when the title says nothing or says several languages at once.
[[nodiscard]] inline QString LanguageFromTitle(const QString &title) {
	const auto lower = title.toLower();
	auto found = QString();
	for (const auto &hint : details::TitleHintList()) {
		const auto code = QString::fromLatin1(hint.code);
		auto matches = QRegularExpression(
			QString::fromUtf8(hint.pattern),
			QRegularExpression::CaseInsensitiveOption | QRegularExpression::UseUnicodePropertiesOption
		).match(lower).hasMatch();
		const auto word = QString::fromLatin1(hint.word);
		if (!matches && !word.isEmpty()) {
			matches = QRegularExpression(u"(^|[^a-z])"_q + word + u"([^a-z]|$)"_q).match(lower).hasMatch();
		}
		if (matches) {
			if (!found.isEmpty()) {
				return QString();
			}
			found = code;
		}
	}
	return found;
}

} // namespace Core::GgtgLang
