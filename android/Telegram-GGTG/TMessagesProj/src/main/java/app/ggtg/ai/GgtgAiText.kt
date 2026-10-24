package app.ggtg.ai

import app.ggtg.GgtgAccounts
import org.telegram.messenger.MessagesController
import org.telegram.messenger.UserConfig
import org.telegram.tgnet.TLRPC
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

// ggtg: what the AI helpers share: a line of a conversation, reading a chat's latest messages, the
// language of a talk, the time context of a prompt.
object GgtgAiText {

    class Line(val mine: Boolean, val text: String, val date: Long, val id: Int = 0)

    /** The slot of the account with this user id, or -1. */
    fun slotOf(userId: Long): Int {
        for (slot in 0 until UserConfig.MAX_ACCOUNT_COUNT) {
            val uc = UserConfig.getInstance(slot)
            if (uc.isClientActivated && uc.clientUserId == userId) return slot
        }
        return -1
    }

    /** The last [limit] messages of a private chat, the oldest first; null when they could not be read. */
    suspend fun history(slot: Int, dialogId: Long, limit: Int, offsetId: Int = 0): List<Line>? {
        val peer = MessagesController.getInstance(slot).getInputPeer(dialogId) ?: return null
        val req = TLRPC.TL_messages_getHistory().apply {
            this.peer = peer
            offset_id = offsetId
            offset_date = 0
            add_offset = 0
            this.limit = limit
            max_id = 0
            min_id = 0
            hash = 0
        }
        val pair = GgtgAccounts.request(slot, req) ?: return null
        val result = pair.first as? TLRPC.messages_Messages ?: return null
        val lines = ArrayList<Line>()
        for (m in result.messages) {
            if (m is TLRPC.TL_messageEmpty || m.action != null) continue
            var text = m.message.orEmpty().trim()
            if (text.isEmpty()) {
                if (m.media == null) continue
                text = "[вложение]"
            }
            if (text.length > 400) text = text.take(400) + "…"
            lines.add(Line(m.out, text, m.date.toLong(), m.id))
        }
        lines.sortBy { it.date }
        return lines
    }

    private val months = listOf("янв", "фев", "мар", "апр", "мая", "июн", "июл", "авг", "сен", "окт", "ноя", "дек")

    fun stamp(date: Long): String {
        if (date <= 0) return ""
        val c = java.util.Calendar.getInstance().apply { time = Date(date * 1000) }
        val now = java.util.Calendar.getInstance()
        return "${c.get(java.util.Calendar.DAY_OF_MONTH)} ${months[c.get(java.util.Calendar.MONTH)]} " +
            (if (c.get(java.util.Calendar.YEAR) != now.get(java.util.Calendar.YEAR)) "${c.get(java.util.Calendar.YEAR)} " else "") +
            SimpleDateFormat("HH:mm", Locale.getDefault()).format(c.time)
    }

    fun transcript(lines: List<Line>, otherName: String = "Собеседник"): String {
        val sb = StringBuilder()
        for (l in lines) sb.append('[').append(stamp(l.date)).append("] ").append(if (l.mine) "Я" else otherName).append(": ").append(l.text).append('\n')
        return sb.toString()
    }

    private fun plural(n: Long, one: String, few: String, many: String): String {
        val m100 = n % 100
        val m10 = n % 10
        val word = if (m100 in 11..14) many else if (m10 == 1L) one else if (m10 in 2..4) few else many
        return "$n $word"
    }

    fun ago(date: Long): String {
        val s = maxOf(0L, System.currentTimeMillis() / 1000 - date)
        return when {
            s < 3600 -> plural(maxOf(s / 60, 1), "минуту", "минуты", "минут")
            s < 86400 -> plural(s / 3600, "час", "часа", "часов")
            s < 86400 * 60 -> plural(s / 86400, "день", "дня", "дней")
            s < 86400 * 365 -> plural(s / (86400 * 30), "месяц", "месяца", "месяцев")
            else -> plural(s / (86400 * 365), "год", "года", "лет")
        }
    }

    /** What the assistant is told about time: now, when the last message was, how long ago. */
    fun timeContext(lines: List<Line>): String {
        val sb = StringBuilder("Сейчас: ").append(stamp(System.currentTimeMillis() / 1000)).append(".\n")
        val last = lines.lastOrNull()
        if (last != null && last.date > 0) {
            sb.append("Последнее сообщение в переписке: ").append(stamp(last.date)).append(" (")
                .append(if (last.mine) "моё" else "собеседника").append("), прошло: ").append(ago(last.date)).append(".\n")
            val gap = System.currentTimeMillis() / 1000 - last.date
            if (gap > 86400 * 2) sb.append("ВАЖНО: разговор давно остановился. Ответ ДОЛЖЕН начинаться с приветствия и показывать, что вы " +
                "давно не общались (без упрёков), затем мягко вернуть к теме или спросить, актуально ли это. НЕ реагируй на последнее " +
                "сообщение так, будто оно только что пришло.\n")
            else if (gap > 86400) sb.append("ВАЖНО: с последнего сообщения прошли сутки или больше: начни с короткого приветствия и верни разговор к теме.\n")
        }
        return sb.toString()
    }

    /** The language of a text by its letters ("ko", "ru", "uk", "zh", "ja", "ar", "th", "hi", "en" for Latin), or "". */
    fun detectLanguage(text: String): String {
        var hangul = 0; var cyrillic = 0; var han = 0; var kana = 0; var arabic = 0; var thai = 0
        var devanagari = 0; var latin = 0; var ukrainian = 0
        for (ch in text) {
            val code = ch.code
            when {
                code in 0xAC00..0xD7AF || code in 0x1100..0x11FF -> hangul++
                code in 0x0400..0x04FF -> { cyrillic++; if (ch == 'і' || ch == 'ї' || ch == 'є' || ch == 'ґ') ukrainian++ }
                code in 0x4E00..0x9FFF -> han++
                code in 0x3040..0x30FF -> kana++
                code in 0x0600..0x06FF -> arabic++
                code in 0x0E00..0x0E7F -> thai++
                code in 0x0900..0x097F -> devanagari++
                ch.isLetter() && code < 0x250 -> latin++
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

    private val languageNames = mapOf("en" to "английский", "ru" to "русский", "ko" to "корейский", "zh" to "китайский",
        "ja" to "японский", "uk" to "украинский", "ar" to "арабский", "th" to "тайский", "hi" to "хинди")

    /** Which language the reply must be in: judged by the other side's latest messages. */
    fun languageNote(lines: List<Line>, personName: String): String {
        val theirs = lines.filter { !it.mine }.joinToString(" ") { it.text }.takeLast(1500)
        var code = detectLanguage(theirs)
        var fromName = false
        if (code.isEmpty()) {
            val byName = detectLanguage(personName)
            if (byName.isNotEmpty() && byName != "en") { code = byName; fromName = true }
        }
        if (code.isEmpty()) code = detectLanguage(lines.filter { it.mine }.joinToString(" ") { it.text }.takeLast(1500))
        if (code.isEmpty()) return ""
        if (code == "en") return "Язык переписки: язык, на котором пишет собеседник (латиница: английский, испанский, турецкий...). " +
            "Пиши ответ ТОЛЬКО на этом языке, даже если эти инструкции написаны по-русски. Имя собеседника пиши так, как оно записано в его профиле.\n"
        return "Язык переписки: ${languageNames[code] ?: code}" +
            (if (fromName) " (собеседник ещё не писал - язык выбран по его имени)" else "") +
            ". Пиши ответ ТОЛЬКО на этом языке, даже если эти инструкции написаны по-русски: отвечать по-русски собеседнику, который " +
            "пишет на другом языке, нельзя. Имя собеседника пиши так, как оно записано в его профиле, без смайликов и символов; если оно " +
            "явно ненастоящее - обращайся «друг» на языке переписки или без имени.\n"
    }

    /** Facts a draft must not invent: links, @names and numbers that are in neither the conversation
     *  nor what the user told the assistant. They are returned, so the user is warned before sending. */
    fun ungroundedFacts(draft: String, known: String): List<String> {
        val re = Regex("https?://\\S+|www\\.\\S+|@[A-Za-z0-9_]{4,}|\\d[\\d\\s.,]*\\d|\\d{2,}")
        val hay = known.lowercase()
        val compact = hay.replace(Regex("[\\s.,]"), "")
        val result = ArrayList<String>()
        for (m in re.findAll(draft)) {
            var value = m.value.trim()
            while (value.endsWith(".") || value.endsWith(",")) value = value.dropLast(1)
            if (value.length < 2) continue
            val plain = value.lowercase().replace(Regex("[\\s.,]"), "")
            if (hay.contains(value.lowercase()) || compact.contains(plain)) continue
            if (value !in result) result.add(value)
        }
        return result
    }
}
