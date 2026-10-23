/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and exceptions, see https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtCore/QChar>
#include <QtCore/QString>

#include <utility>

namespace Core::GgtgSort {

// Where a letter stands when names of many scripts are sorted together:
// Latin first, then Cyrillic, then every other script (Greek, Hebrew,
// Arabic, Hangul, Chinese...) one after another, then digits, and last
// everything that is not a letter (emoji, signs).
[[nodiscard]] inline int ScriptBucket(uint ucs4) {
	if (QChar::isLetter(ucs4)) {
		const auto script = QChar::script(ucs4);
		if (script == QChar::Script_Latin) {
			return 0;
		} else if (script == QChar::Script_Cyrillic) {
			return 1;
		}
		return 10 + int(script);
	} else if (QChar::isNumber(ucs4)) {
		return 400;
	}
	return 500;
}

// The sort key of a name: the bucket of its first letter or digit (signs
// and emoji before it are skipped, so "🔥Anna" stands with "Anna") and the
// text from there, case folded. A name with nothing but signs goes last.
[[nodiscard]] inline std::pair<int, QString> SortKey(const QString &text) {
	for (auto i = 0; i < text.size();) {
		auto ucs4 = uint(text[i].unicode());
		auto length = 1;
		if (QChar::isHighSurrogate(ucs4)
			&& i + 1 < text.size()
			&& QChar::isLowSurrogate(text[i + 1].unicode())) {
			ucs4 = QChar::surrogateToUcs4(ucs4, text[i + 1].unicode());
			length = 2;
		}
		const auto bucket = ScriptBucket(ucs4);
		if (bucket < 500) {
			return { bucket, text.mid(i).toCaseFolded() };
		}
		i += length;
	}
	return { 500, text.toCaseFolded() };
}

} // namespace Core::GgtgSort
