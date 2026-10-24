package app.ggtg

// ng: Kotlin port of ggtg-macos' window/ggtg_squidward_parse.cpp - reading @skvidvarde_bot's
// replies into plain data. Pure Kotlin (no Android/tgnet), so it is covered by unit tests on the
// same bot-reply samples the desktop test uses (SquidwardParseTest).
object SquidwardParse {

    enum class EntityType { Bold, Url, CustomUrl, Mention, MentionName, Other }
    data class Entity(val type: EntityType, val offset: Int, val length: Int, val data: String = "")
    data class Message(val text: String, val entities: List<Entity>)

    data class NameChange(val date: String, val name: String)

    data class UserCard(
        var name: String = "", var id: String = "",
        var activeSince: String = "", var activeUntil: String = "",
        var messages: Int = -1, var chats: Int = -1,
        var diversity: String = "", var replies: String = "", var media: String = "",
        var roundVideos: Int = -1, var voices: Int = -1,
        var favoriteChat: String = "", var channel: String = "", var searched: Int = -1,
        var usernames: MutableList<String> = ArrayList(),
        var names: MutableList<NameChange> = ArrayList()
    )

    data class TopMember(val name: String, val messages: Int)

    data class GroupCard(
        var name: String = "", var id: String = "",
        var topPlace: Int = -1, var topOf: Int = -1,
        var peopleDay: Int = -1, var messagesDay: Int = -1,
        var mediaDay: Int = -1, var mediaDayPercent: String = "",
        var roundVideosDay: Int = -1, var voicesDay: Int = -1,
        var diversity: String = "", var description: String = "",
        var topDay: MutableList<TopMember> = ArrayList()
    )

    data class PageInfo(val page: Int, val pages: Int, val total: Int = -1)

    data class GroupRow(val username: String = "", val title: String = "", val link: String = "")
    data class UserRow(val username: String = "", val mentionData: String = "", val name: String = "")

    // Look-alikes the bot mixes into Russian words + odd "ID" glyphs. One char to one char.
    private val foldTable: Map<Char, Char> = buildMap {
        put('a', 'а'); put('c', 'с'); put('e', 'е'); put('o', 'о')
        put('p', 'р'); put('x', 'х'); put('y', 'у'); put('r', 'г'); put('k', 'к')
        put('ο', 'о'); put('γ', 'у'); put('ρ', 'р'); put('α', 'а')
        put('π', 'п'); put('η', 'н'); put('ν', 'н'); put('ᴏ', 'о')
        put('ℓ', 'л'); put('ɑ', 'а'); put('ҡ', 'к'); put('ł', 'л'); put('ǫ', 'о')
        put('ɪ', 'i'); put('ı', 'i'); put('İ', 'i'); put('ᴅ', 'd')
        put('ŋ', 'н'); put('ɳ', 'н'); put('n', 'н'); put('ƒ', 'f')
        put('b', 'в'); put('h', 'н'); put('m', 'м'); put('t', 'т')
        put('℮', 'е'); put('ᴜ', 'u'); put('ѕ', 's'); put('і', 'i')
        put('ј', 'j'); put('қ', 'к'); put('ι', 'i'); put('κ', 'к')
        put('τ', 'т'); put('υ', 'u'); put('ε', 'е'); put('χ', 'х')
        put('β', 'в'); put('μ', 'м'); put('σ', 'о'); put('ᴀ', 'а')
        put('ʙ', 'в'); put('ᴄ', 'с'); put('ᴇ', 'е'); put('ʜ', 'н')
        put('ᴋ', 'к'); put('ᴍ', 'м'); put('ɴ', 'н'); put('ᴧ', 'л')
        put('ᴘ', 'р'); put('ᴛ', 'т'); put('ʏ', 'у'); put('ʀ', 'г')
        put('ㅤ', ' '); put(' ', ' ')
        // English replies: look-alikes go to the Latin letter, which then folds like a plain one
        // ("Ţop" == "top", "peopℓe" == "people").
        put('l', 'л'); put('ʟ', 'l'); put('ɡ', 'g'); put('ɢ', 'g')
        put('ꜱ', 's'); put('ᴠ', 'v'); put('ᴡ', 'w'); put('ᴢ', 'z')
        put('ᴊ', 'j'); put('ғ', 'f'); put('ս', 'u'); put('օ', 'o')
        put('ո', 'n'); put('հ', 'h'); put('ց', 'g'); put('զ', 'q')
        put('ԁ', 'd'); put('ԛ', 'q'); put('ԝ', 'w'); put('ӏ', 'l')
        put('ɩ', 'i'); put('ɨ', 'i'); put('ɗ', 'd'); put('ɖ', 'd')
        put('ƫ', 't'); put('ƭ', 't'); put('ʈ', 't'); put('ƀ', 'b')
        put('ɓ', 'b'); put('ɲ', 'n'); put('ɱ', 'm'); put('ʂ', 's')
        put('ʐ', 'z'); put('ɣ', 'y'); put('ƴ', 'y'); put('ʋ', 'v')
        put('ɔ', 'o'); put('ɐ', 'a'); put('ɒ', 'a')
    }

    private fun foldChar(ch: Char): Char {
        var code = ch.code
        if (code in 0xFF01..0xFF5E) code -= 0xFEE0 // fullwidth -> ASCII
        var folded = code.toChar()
        // Accented Latin ("Ţ") -> its base letter; Cyrillic "й", "ё" and other scripts stay.
        if (folded.code >= 0x80) {
            val decomposed = java.text.Normalizer.normalize(folded.toString(), java.text.Normalizer.Form.NFKD)
            if (decomposed.isNotEmpty() && decomposed != folded.toString()) {
                val base = decomposed[0]
                if (base.code < 0x0250 && base.isLetter()) folded = base
            }
        }
        val lower = folded.lowercaseChar()
        // Two steps: a look-alike goes to its Latin letter, which then folds like a plain one.
        val once = foldTable[lower] ?: lower
        return foldTable[once] ?: once
    }

    fun fold(text: String): String = buildString(text.length) { for (ch in text) append(foldChar(ch)) }

    private fun escaped(label: String): String = Regex.escape(fold(label))

    // The bot answers in Russian or English: any of the labels.
    private fun escaped(vararg labels: String): String =
        labels.joinToString("|", prefix = "(?:", postfix = ")") { Regex.escape(fold(it)) }

    private fun capitalized(text: String): String =
        if (text.isEmpty()) text else text[0].uppercaseChar() + text.substring(1)

    private fun diversityPattern(): String =
        "(?:" + escaped("разнообразие сообщ") + """\S*|""" + escaped("message diversity") + """)\s*([\d.,]+\s*%)"""

    fun buttonLastWord(text: String): String {
        val parts = text.trim().split(Regex("\\s+")).filter { it.isNotEmpty() }
        return if (parts.isEmpty()) "" else fold(parts.last())
    }

    // The button's words, letters only, folded: "15💎 Groups" -> "groups" (folded).
    fun buttonWords(text: String): String =
        fold(text).map { if (it.isLetter()) it else ' ' }.joinToString("")
            .split(' ').filter { it.isNotEmpty() }.joinToString(" ")

    // Line-by-line matching on folded text; captures cut from the original at aligned positions.
    private class Reader(text: String) {
        private data class Line(val original: String, val folded: String)
        private val lines = text.split('\n').map { Line(it, fold(it)) }
        val size get() = lines.size
        fun original(index: Int) = lines[index].original

        fun find(pattern: String, from: Int = 0): FindResult? {
            val re = Regex(pattern)
            for (i in from until size) {
                val m = re.find(lines[i].folded) ?: continue
                val caps = ArrayList<String>()
                for (n in 1 until m.groupValues.size) {
                    val g = m.groups[n]
                    if (g != null) caps.add(lines[i].original.substring(g.range.first, g.range.last + 1).trim())
                    else caps.add("")
                }
                return FindResult(i, caps)
            }
            return null
        }

        fun has(pattern: String) = find(pattern) != null
    }

    private class FindResult(val index: Int, val caps: List<String>) {
        fun value(i: Int): String = caps.getOrElse(i) { "" }
    }

    private fun toInt(text: String): Int {
        val digits = text.filter { it.isDigit() }
        return if (digits.isEmpty()) -1 else digits.toInt()
    }

    private val NAME_PATTERN get() = "^\\s*" + escaped("это", "this is") + "\\s+(.+)$"
    private const val ID_PATTERN = "^\\s*id\\s*:\\s*(\\d+)"

    fun parsePageInfo(text: String): PageInfo? {
        val folded = fold(text)
        val pages = Regex(escaped("страниц", "page") + """\D*(\d+)\D+(\d+)""").find(folded) ?: return null
        val total = Regex(escaped("всего", "total") + """\s*:?\s*(\d+)""").find(folded)
        return PageInfo(
            page = pages.groupValues[1].toInt(),
            pages = pages.groupValues[2].toInt(),
            total = total?.groupValues?.get(1)?.toInt() ?: -1
        )
    }

    private val linkRe = Regex("""^(?:https?://)?t(?:elegram)?\.me/([A-Za-z0-9_]{4,32})/?$""")
    private val privateRe = Regex("""^(?:https?://)?t(?:elegram)?\.me/(?:c/\d+(?:/\d+)?|\+[A-Za-z0-9_-]+|joinchat/[A-Za-z0-9_-]+)/?$""")
    private val queryRe = Regex("""^@?([A-Za-z][A-Za-z0-9_]{3,31})$""")

    fun usernameFromLink(link: String): String =
        linkRe.matchEntire(link.trim())?.groupValues?.get(1) ?: ""

    fun privateLink(link: String): String {
        val trimmed = link.trim()
        return if (privateRe.matches(trimmed)) trimmed else ""
    }

    fun usernameFromQuery(query: String): String {
        val trimmed = query.trim()
        if (privateLink(trimmed).isNotEmpty()) return ""
        usernameFromLink(trimmed).let { if (it.isNotEmpty()) return it }
        return queryRe.matchEntire(trimmed)?.groupValues?.get(1) ?: ""
    }

    // Base title for a card saved from [query] whose subject name is [name] (mirrors the desktop
    // session's baseTitleFor + Data::GroupBaseTitle).
    fun groupBaseTitleFor(query: String, name: String): String {
        val username = usernameFromQuery(query)
        if (name.isEmpty()) return if (username.isEmpty()) query.trim() else "@$username"
        val bare = username.trim().removePrefix("@")
        val trimmed = name.trim()
        return if (bare.isEmpty()) trimmed else "$trimmed (@$bare)"
    }

    fun isPlaceholder(text: String): Boolean = text.contains("⏳") || fold(text).contains(fold("ычисля"))

    fun cleanText(text: String): String {
        val lines = ArrayList<String>()
        for (raw in text.split('\n')) {
            val line = raw.trimEnd()
            if (line.trim().isEmpty() && (lines.isEmpty() || lines.last().isEmpty())) continue
            lines.add(if (line.trim().isEmpty()) "" else line)
        }
        return lines.joinToString("\n").trim()
    }

    fun formatCount(value: Int): String {
        // 30707 -> "30 707" (non-breaking-ish thin space kept simple)
        val s = value.toString()
        val sb = StringBuilder()
        val neg = s.startsWith("-")
        val digits = if (neg) s.substring(1) else s
        for ((i, ch) in digits.withIndex()) {
            if (i > 0 && (digits.length - i) % 3 == 0) sb.append(' ')
            sb.append(ch)
        }
        return if (neg) "-$sb" else sb.toString()
    }

    fun pushRecent(list: List<String>, query: String, limit: Int): List<String> {
        val trimmed = query.trim()
        val result = ArrayList<String>()
        result.add(trimmed)
        for (entry in list) {
            if (entry.compareTo(trimmed, ignoreCase = true) != 0) result.add(entry)
        }
        return result.take(limit)
    }

    fun extractGroupRows(message: Message): List<GroupRow> {
        val result = ArrayList<GroupRow>()
        fun addLink(shown: String, link: String) {
            val title = if (shown == link) "" else shown.trim()
            val username = usernameFromLink(link)
            if (username.isNotEmpty()) result.add(GroupRow(username = username, title = title))
            else {
                val closed = privateLink(link)
                if (closed.isNotEmpty()) result.add(GroupRow(title = title, link = closed))
            }
        }
        for (entity in message.entities) {
            val shown = message.text.substring(entity.offset, entity.offset + entity.length)
            when (entity.type) {
                EntityType.Mention -> result.add(GroupRow(username = shown.removePrefix("@")))
                EntityType.Url -> addLink(shown, shown)
                EntityType.CustomUrl -> addLink(shown, entity.data)
                else -> {}
            }
        }
        return result
    }

    fun extractUserRows(message: Message): List<UserRow> {
        val result = ArrayList<UserRow>()
        for (entity in message.entities) {
            val shown = message.text.substring(entity.offset, entity.offset + entity.length)
            val name = if (shown.startsWith("@") || shown.contains("t.me/")) "" else shown.trim()
            if (entity.type == EntityType.MentionName) {
                result.add(UserRow(mentionData = entity.data, name = name))
            } else if (entity.type == EntityType.Url || entity.type == EntityType.CustomUrl) {
                val username = usernameFromLink(if (entity.type == EntityType.Url) shown else entity.data)
                if (username.isNotEmpty()) result.add(UserRow(username = username, name = name))
            }
        }
        return result
    }

    private val closedRe = Regex("""/c/(\d+)""")

    fun dedupeGroupRows(rows: List<GroupRow>): List<GroupRow> {
        val seen = HashSet<String>()
        val result = ArrayList<GroupRow>()
        for (row in rows) {
            val chat = closedRe.find(row.link)
            val key = when {
                row.username.isNotEmpty() -> "@" + row.username.lowercase()
                chat != null -> "c/" + chat.groupValues[1]
                else -> row.link.lowercase()
            }
            if (key.isEmpty() || !seen.add(key)) continue
            result.add(row)
        }
        return result
    }

    fun dedupeUsernames(usernames: List<String>): List<String> {
        val seen = HashSet<String>()
        val result = ArrayList<String>()
        for (raw in usernames) {
            val name = raw.trim().removePrefix("@")
            val key = name.lowercase()
            if (name.isEmpty() || key.endsWith("bot") || !seen.add(key)) continue
            result.add(name)
        }
        return result
    }

    fun parseUserCard(message: Message): UserCard? {
        val reader = Reader(message.text)
        val isGroup = reader.has(escaped("в топе"))
        val activity = reader.find("""(\d+)\s+""" + escaped("сообщени", "message") + """\S*\s+""" + escaped("в", "in") + """\s+(\d+)\s+""" + escaped("чат", "group", "chat"))
        val style = reader.find("""([\d.,]+\s*%)\s*""" + escaped("реплаи", "replies") + """\s*([\d.,]+\s*%)\s*""" + escaped("медиа", "media"))
        if (isGroup || (activity == null && style == null)) return null
        val card = UserCard()
        reader.find(NAME_PATTERN)?.let { card.name = it.value(0) }
        reader.find(ID_PATTERN)?.let { card.id = it.value(0) }
        activity?.let { card.messages = toInt(it.value(0)); card.chats = toInt(it.value(1)) }
        style?.let { card.replies = it.value(0); card.media = it.value(1) }
        reader.find(diversityPattern())?.let { card.diversity = it.value(0) }
        reader.find("""^\s*""" + escaped("с", "from") + """\s+(\d{1,2}[./]\d{1,2}[./]\d{4})\s+""" + escaped("по", "to") + """\s+(.+)$""")?.let {
            card.activeSince = it.value(0); card.activeUntil = it.value(1)
        }
        reader.find(escaped("кружки", "circles") + """\s*:\s*(\d+)\s*,\s*""" + escaped("голос", "voice") + """\S*\s*:\s*(\d+)""")?.let {
            card.roundVideos = toInt(it.value(0)); card.voices = toInt(it.value(1))
        }
        reader.find(escaped("любимый чат", "favorite group", "favorite chat") + """\s*:\s*(.+)$""")?.let { card.favoriteChat = it.value(0) }
        reader.find(escaped("искали", "were looking for") + """\s*:\s*(\d+)""")?.let { card.searched = toInt(it.value(0)) }
        reader.find(escaped("канал", "channel") + """\s*:\s*(.+)$""")?.let { card.channel = it.value(0) }

        val names = reader.find("""^\s*""" + escaped("имена", "first name"))
        val namesAt = names?.index ?: -1
        val usernamesFound = reader.find(escaped("usernames"))
        if (usernamesFound != null) {
            val usernamesAt = usernamesFound.index
            val mention = Regex("""@([A-Za-z0-9_]{4,32})""")
            val last = minOf(usernamesAt + 1, if (namesAt > usernamesAt) namesAt - 1 else reader.size - 1)
            for (i in usernamesAt..last) {
                mention.findAll(reader.original(i)).forEach { card.usernames.add(it.groupValues[1]) }
            }
        }
        if (names != null) {
            val change = Regex("""^[\s├└│|]*(\d{4}-\d{2}-\d{2})\s*[➔→➜⮕>\-–—]*️?\s*(.+?)\s*$""")
            for (i in namesAt + 1 until reader.size) {
                val line = reader.original(i)
                val m = change.find(line)
                if (m == null) {
                    if (line.trim().isEmpty()) continue else break
                }
                card.names.add(NameChange(m.groupValues[1], m.groupValues[2]))
            }
        }
        return card
    }

    fun parseGroupCard(message: Message): GroupCard? {
        val reader = Reader(message.text)
        val top = reader.find(escaped("в топе") + """\s*#\s*(\d+)\s*""" + escaped("из") + """\s*(\d+)""")
        if (top == null && !reader.has(escaped("за сутки"))) return null
        val card = GroupCard()
        top?.let { card.topPlace = toInt(it.value(0)); card.topOf = toInt(it.value(1)) }
        reader.find(NAME_PATTERN)?.let { card.name = it.value(0) }
        reader.find(ID_PATTERN)?.let { card.id = it.value(0) }
        reader.find(diversityPattern())?.let { card.diversity = it.value(0) }
        reader.find("""(\d+)\s*""" + escaped("челов") + """\S*\s*""" + escaped("за сутки"))?.let { card.peopleDay = toInt(it.value(0)) }
        reader.find(escaped("написали") + """\s*(\d+)\s*""" + escaped("сообщ"))?.let { card.messagesDay = toInt(it.value(0)) }
        reader.find(escaped("из них") + """\s*(\d+)\s*\(([\d.,]+\s*%)\)\s*""" + escaped("медиа"))?.let {
            card.mediaDay = toInt(it.value(0)); card.mediaDayPercent = it.value(1)
        }
        reader.find("""(\d+)\s*""" + escaped("кружк") + """\S*\s*""" + escaped("и") + """\s*(\d+)\s*""" + escaped("голос"))?.let {
            card.roundVideosDay = toInt(it.value(0)); card.voicesDay = toInt(it.value(1))
        }

        val topDay = reader.find("""^\s*""" + escaped("топ за сутки"))
        val topAt = topDay?.index ?: -1
        val description = reader.find("""^\s*""" + escaped("описание") + """\s*:(.*)$""")
        if (description != null) {
            val descriptionAt = description.index
            val filler = Regex("""^[\s\-–—]*$""")
            val lines = ArrayList<String>()
            if (description.value(0).isNotEmpty()) lines.add(description.value(0))
            val end = if (topAt > descriptionAt) topAt else reader.size
            for (i in descriptionAt + 1 until end) {
                val line = reader.original(i).trim()
                if (!filler.matches(line)) lines.add(line)
            }
            card.description = lines.joinToString("\n").trim()
        }
        if (topDay != null) {
            val row = Regex("""^\s*(\d+)\.\s*(.+?)\s*[-–—]\s*(\d+)\s*$""")
            for (i in topAt + 1 until reader.size) {
                val line = reader.original(i)
                val m = row.find(line)
                if (m == null) {
                    if (line.trim().isEmpty()) continue else break
                }
                card.topDay.add(TopMember(m.groupValues[2], m.groupValues[3].toInt()))
            }
        }
        return card
    }

    fun formatUserDetails(card: UserCard): List<String> {
        val result = ArrayList<String>()
        if (card.activeSince.isNotEmpty()) {
            result.add("Активен с " + card.activeSince + if (card.activeUntil.isEmpty()) "" else " по " + card.activeUntil)
        }
        if (card.messages >= 0 && card.chats >= 0) {
            result.add(formatCount(card.messages) + " сообщ. в " + formatCount(card.chats) + " чатах")
        }
        if (card.favoriteChat.isNotEmpty()) result.add("Любимый чат: " + card.favoriteChat)
        val style = ArrayList<String>()
        if (card.replies.isNotEmpty()) style.add("реплаи " + card.replies)
        if (card.media.isNotEmpty()) style.add("медиа " + card.media)
        if (card.roundVideos >= 0) style.add("кружки " + card.roundVideos)
        if (card.voices >= 0) style.add("голос " + card.voices)
        if (style.isNotEmpty()) result.add(capitalized(style.joinToString(" · ")))
        if (card.diversity.isNotEmpty()) result.add("Разнообразие сообщений " + card.diversity)
        val extra = ArrayList<String>()
        if (card.channel.isNotEmpty()) extra.add("канал: " + card.channel)
        if (card.searched >= 0) extra.add("искали: " + card.searched)
        if (extra.isNotEmpty()) result.add(capitalized(extra.joinToString(" · ")))
        if (card.usernames.isNotEmpty()) {
            result.add("Юзернеймы: " + card.usernames.joinToString(" ") { "@$it" })
        }
        return result
    }

    fun formatGroupDetails(card: GroupCard): List<String> {
        val result = ArrayList<String>()
        if (card.topPlace >= 0) {
            result.add("В топе #" + formatCount(card.topPlace) + if (card.topOf >= 0) " из " + formatCount(card.topOf) else "")
        }
        val day = ArrayList<String>()
        if (card.peopleDay >= 0) day.add(formatCount(card.peopleDay) + " человек")
        if (card.messagesDay >= 0) day.add(formatCount(card.messagesDay) + " сообщ.")
        if (card.mediaDay >= 0) day.add("медиа " + formatCount(card.mediaDay) + if (card.mediaDayPercent.isEmpty()) "" else " (" + card.mediaDayPercent + ")")
        if (day.isNotEmpty()) result.add("За сутки: " + day.joinToString(" · "))
        val voice = ArrayList<String>()
        if (card.roundVideosDay >= 0) voice.add("кружки " + card.roundVideosDay)
        if (card.voicesDay >= 0) voice.add("голосовые " + card.voicesDay)
        if (voice.isNotEmpty()) result.add(capitalized(voice.joinToString(" · ")))
        if (card.diversity.isNotEmpty()) result.add("Разнообразие сообщений " + card.diversity)
        return result
    }
}
