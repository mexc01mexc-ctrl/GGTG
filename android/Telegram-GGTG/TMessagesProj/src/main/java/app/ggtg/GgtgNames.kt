package app.ggtg

import java.util.Locale

// ng: a recipient's name made fit for "Привет, <name>!" (mirrors Core::GgtgAi::CleanPersonName /
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
        "th" to "тайский", "hi" to "хинди", "en" to "английский"
    )

    fun languageLabel(code: String): String = russianNames[code] ?: code
}
