package app.ggtg

import java.util.Locale

// ng: a recipient's name made fit for "Привет, <name>!" (mirrors Core::GgtgLang::CleanPersonName /
// FriendWord / DetectLanguage in the desktop app): emoji, symbols and digits of a profile name are
// dropped, and an obviously-not-a-name ("qqqq", one letter, "asdf", "user") becomes "друг" in the
// language of the text being sent.
object GgtgNames {
    private val vowels = "aeiouyаеёиоуыэюяіїєáéíóúàèìòùäöüâêîôûãõ"
    private val rows = listOf("qwertyuiop", "asdfghjkl", "zxcvbnm", "йцукенгшщзхъ", "фывапролджэ", "ячсмитьбю")
    private val notNames = setOf(
        "user", "admin", "test", "unknown", "noname", "none", "null", "telegram", "deleted", "anon",
        "anonymous", "name", "nickname", "nick", "bot", "account", "no name", "имя", "аноним",
        "аккаунт", "пользователь", "user name"
    )
    private val english = Regex(
        "\\b(the|you|your|and|hello|hi|hey|please|thanks|for|with|this|that|have|are|can)\\b",
        RegexOption.IGNORE_CASE
    )

    /** The name as letters only, or "" when it is clearly not a real name. */
    fun cleanPersonName(raw: String): String {
        val kept = StringBuilder()
        val lower = StringBuilder()
        var alphabetic = true // Latin / Cyrillic only: these can be judged
        var lastWasLetter = false
        var i = 0
        while (i < raw.length) {
            val cp = raw.codePointAt(i)
            i += Character.charCount(cp)
            when {
                Character.isLetter(cp) -> {
                    kept.appendCodePoint(cp)
                    lower.appendCodePoint(Character.toLowerCase(cp))
                    lastWasLetter = true
                    if (!((cp in 0x41..0x24F) || (cp in 0x400..0x4FF))) alphabetic = false
                }
                lastWasLetter && isMark(cp) -> kept.appendCodePoint(cp)
                lastWasLetter && (cp == '\''.code || cp == 0x2019 || cp == '-'.code || cp == ' '.code) -> {
                    kept.appendCodePoint(cp)
                    lastWasLetter = false
                }
                else -> lastWasLetter = false
            }
        }
        var name = kept.toString().replace(Regex("\\s+"), " ").trim()
        while (name.endsWith("-") || name.endsWith("'") || name.endsWith("’")) name = name.dropLast(1)
        val letters = lower.toString()
        if (letters.isEmpty()) return ""
        if (alphabetic) {
            if (letters.length < 2) return ""
            if (letters.all { it == letters[0] }) return "" // "qqqq", "ааа"
            if (letters.length >= 4 && letters.none { vowels.contains(it) }) return "" // "qwrt", "хзбж"
            if (letters.length >= 3 && rows.any { it.contains(letters) || it.reversed().contains(letters) }) return ""
            if (name.lowercase(Locale.ROOT) in notNames) return ""
        }
        return name
    }

    private fun isMark(cp: Int): Boolean = when (Character.getType(cp).toByte()) {
        Character.NON_SPACING_MARK, Character.COMBINING_SPACING_MARK, Character.ENCLOSING_MARK -> true
        else -> false
    }

    /** The language of a text by its letters (script is enough for the cases that matter here). */
    fun detectLanguage(text: String): String {
        var hangul = 0; var cyrillic = 0; var han = 0; var kana = 0; var arabic = 0
        var thai = 0; var devanagari = 0; var latin = 0; var ukrainian = 0
        for (ch in text) {
            val c = ch.code
            when {
                c in 0xAC00..0xD7AF || c in 0x1100..0x11FF -> hangul++
                c in 0x400..0x4FF -> {
                    cyrillic++
                    if (c == 0x456 || c == 0x457 || c == 0x454 || c == 0x491) ukrainian++
                }
                c in 0x4E00..0x9FFF -> han++
                c in 0x3040..0x30FF -> kana++
                c in 0x600..0x6FF -> arabic++
                c in 0xE00..0xE7F -> thai++
                c in 0x900..0x97F -> devanagari++
                ch.isLetter() && c < 0x250 -> latin++
            }
        }
        val most = maxOf(hangul, cyrillic, han, kana, arabic, thai, devanagari, latin)
        return when {
            most == 0 -> ""
            kana > 0 && kana * 4 >= han -> "ja"
            most == hangul -> "ko"
            most == cyrillic -> if (ukrainian > 0) "uk" else "ru"
            most == han -> "zh"
            most == arabic -> "ar"
            most == thai -> "th"
            most == devanagari -> "hi"
            else -> "en"
        }
    }

    /** What to call a person with no usable name, in the language of [text]; "" when not sure. */
    fun friendWord(text: String): String = when (detectLanguage(text)) {
        "ru" -> "друг"
        "uk" -> "друже"
        "zh" -> "朋友"
        "ja" -> "友達"
        "ko" -> "친구"
        "ar" -> "صديقي"
        "th" -> "เพื่อน"
        "hi" -> "दोस्त"
        "en" -> if (english.containsMatchIn(text)) "friend" else ""
        else -> ""
    }

    private val russianNames = mapOf(
        "" to "не определён", "lat" to "латиница", "ru" to "русский", "uk" to "украинский",
        "zh" to "китайский", "ja" to "японский", "ko" to "корейский", "ar" to "арабский",
        "th" to "тайский", "hi" to "хинди", "en" to "английский", "vi" to "вьетнамский", "id" to "индонезийский",
        "tr" to "турецкий", "es" to "испанский", "pt" to "португальский", "de" to "немецкий", "fr" to "французский",
        "it" to "итальянский", "pl" to "польский"
    )

    fun languageLabel(code: String): String = russianNames[code] ?: code

    // ---- the language of a person by the name (mirrors Core::GgtgLang::GuessNameLanguage of the desktop app) ----

    class LanguageGuess(val code: String, val confidence: Int) {
        /** 65 and more is safe to act on (translate, split a base). */
        val sure: Boolean get() = code.isNotEmpty() && confidence >= 65
    }

    private val noGuess = LanguageGuess("", 0)

    private class TokenWeight(val code: String, val weight: Double)

    private val tokenIndex: Map<String, List<TokenWeight>> by lazy {
        val index = HashMap<String, MutableList<TokenWeight>>()
        fun add(code: String, list: String, weight: Double) {
            for (token in list.split(' ').filter { it.isNotEmpty() }) {
                val entry = index.getOrPut(token) { mutableListOf() }
                val at = entry.indexOfFirst { it.code == code }
                if (at < 0) entry.add(TokenWeight(code, weight))
                else if (entry[at].weight < weight) entry[at] = TokenWeight(code, weight)
            }
        }
        for ((code, d) in GgtgNameLangsData.dictionaries) {
            add(code, d.first, 1.0); add(code, d.second, 0.6); add(code, d.third, 0.3)
        }
        index
    }

    /** Lower case, no diacritics: "Nguyễn" -> "nguyen". */
    private fun plain(text: String): String {
        val lower = text.lowercase()
            .replace('\u0111', 'd').replace('\u0142', 'l').replace('\u0131', 'i')
            .replace("\u00F8", "o").replace("\u00DF", "ss").replace("\u00E6", "ae")
        val sb = StringBuilder()
        for (ch in java.text.Normalizer.normalize(lower, java.text.Normalizer.Form.NFD)) {
            if (!isMark(ch.code)) sb.append(ch)
        }
        return sb.toString()
    }

    private fun nameTokens(text: String): List<String> = plain(text).split(Regex("[^a-z]+")).filter { it.isNotEmpty() }

    /** A letter that only one language has (Vietnamese tones, Turkish dotless i, Polish l with a stroke...). */
    private fun latinMarkerLanguage(text: String): String {
        val lower = text.lowercase()
        fun has(letters: String) = lower.any { letters.indexOf(it) >= 0 }
        return when {
            has("ăơưđạảấầẩẫậắằẳẵặẹẻẽếềểễệỉĩịọỏốồổỗộớờởỡợụủứừửữựỳỵỷỹ") -> "vi"
            has("şğı") -> "tr"
            has("ąęłśćźżń") -> "pl"
            has("ñ") -> "es"
            has("ß") -> "de"
            has("ãõ") -> "pt"
            else -> ""
        }
    }

    /**
     * The language of a recipient: the script of the name, and for a name in Latin letters the letters only one
     * language has and the dictionaries of surnames and given names. The username counts a little; [hint] is the
     * language the group is about ([languageFromTitle]), it only adds weight to a language that already has some.
     */
    fun guessNameLanguage(first: String, last: String, username: String = "", hint: String = ""): LanguageGuess {
        val name = "$first $last".trim()
        val script = detectLanguage(name)
        if (script.isNotEmpty() && script != "en") {
            if (script == "zh" && hint == "ja") return LanguageGuess("ja", 80)
            val confidence = when (script) { "zh" -> 80; "ru", "uk", "ar" -> 85; else -> 95 }
            return LanguageGuess(script, confidence)
        } else if (script.isEmpty() && username.isEmpty()) {
            return noGuess
        }
        val marker = latinMarkerLanguage(name)
        if (marker.isNotEmpty()) return LanguageGuess(marker, if (marker == "pt") 75 else 88)
        val scores = HashMap<String, Double>()
        fun count(tokens: List<String>, factor: Double, minLength: Int) {
            for (token in tokens) {
                if (token.length < minLength) continue
                val entry = tokenIndex[token]
                if (entry != null) {
                    for (w in entry) scores[w.code] = (scores[w.code] ?: 0.0) + w.weight * factor
                } else if (token.length >= 5) {
                    for ((code, list) in GgtgNameLangsData.suffixes) {
                        if (list.split(' ').any { it.isNotEmpty() && token.endsWith(it) && token.length > it.length + 1 }) {
                            scores[code] = (scores[code] ?: 0.0) + 0.8 * factor
                        }
                    }
                }
            }
        }
        count(nameTokens(name), 1.0, 2)
        count(nameTokens(username), 0.4, 4)
        if (hint.isNotEmpty() && (scores[hint] ?: 0.0) >= 0.3) scores[hint] = scores[hint]!! + 0.5
        val ranked = scores.entries.sortedByDescending { it.value }
        val best = ranked.firstOrNull() ?: return noGuess
        val second = ranked.getOrNull(1)?.value ?: 0.0
        if (best.value >= 1.0 && best.value >= second + 0.6) {
            return LanguageGuess(best.key, minOf(90, 60 + (best.value * 15).toInt()))
        }
        return noGuess
    }

    /** The language a group or a base is about by its title; "" when it says nothing or says several. */
    fun languageFromTitle(title: String): String {
        val lower = title.lowercase()
        var found = ""
        for ((code, hint) in GgtgNameLangsData.titles) {
            var matches = Regex(hint.first, RegexOption.IGNORE_CASE).containsMatchIn(lower)
            if (!matches && hint.second.isNotEmpty()) matches = Regex("(^|[^a-z])${hint.second}([^a-z]|$)").containsMatchIn(lower)
            if (matches) {
                if (found.isNotEmpty()) return ""
                found = code
            }
        }
        return found
    }
}
