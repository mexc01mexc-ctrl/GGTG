package app.ggtg.ai

import org.json.JSONArray
import org.json.JSONObject
import java.io.File

// ggtg: everything the AI knows about the user and the way they promote their own thing, in one place that
// every helper reads (the reply feed, the "second brain" chat) and every learning path writes to (the
// talk with the AI, the edits of drafts, training on chats). Sections of their own with limits of
// their own, so the facts about the business are never pushed out by the rules of style.
// File: ggtg_bulk_db/ai_profile.json - the same shape as the desktop app.
object GgtgAiProfile {

    enum class Section(val key: String, val title: String, val tag: String, val maxItems: Int) {
        Business("business", "Дело и оффер", "дело", 10),
        Goal("goal", "Цель диалога", "цель", 5),
        Audience("audience", "Аудитория", "аудитория", 8),
        Style("style", "Стиль", "стиль", 12),
        Rules("rules", "Правила и запреты", "правило", 12),
        Works("works", "Что работает", "работает", 15),
        NotWorks("notworks", "Что не работает", "не работает", 15)
    }

    class Entry(val text: String, val at: Long)

    private val lock = Any()
    private var cache: MutableMap<Section, MutableList<Entry>>? = null

    private fun file() = File(GgtgAi.dir(), "ai_profile.json")

    private fun data(): MutableMap<Section, MutableList<Entry>> {
        cache?.let { return it }
        val map = HashMap<Section, MutableList<Entry>>()
        for (s in Section.values()) map[s] = ArrayList()
        GgtgAi.readText(file())?.let { text ->
            try {
                val sections = JSONObject(text).optJSONObject("sections")
                if (sections != null) for (s in Section.values()) {
                    val arr = sections.optJSONArray(s.key) ?: continue
                    for (i in 0 until arr.length()) {
                        val o = arr.getJSONObject(i)
                        val t = clean(o.optString("t"))
                        if (t.length >= 4) map[s]!!.add(Entry(t, o.optLong("at")))
                    }
                }
            } catch (t: Throwable) { }
        }
        cache = map
        return map
    }

    private fun write() {
        val sections = JSONObject()
        for (s in Section.values()) {
            val arr = JSONArray()
            for (e in data()[s]!!) arr.put(JSONObject().put("t", e.text).put("at", e.at))
            sections.put(s.key, arr)
        }
        GgtgAi.writeText(file(), JSONObject().put("v", 1).put("sections", sections).toString())
    }

    private fun clean(raw: String): String {
        var t = raw.trim().replace(Regex("\\s+"), " ")
        while (t.startsWith("-") || t.startsWith("*") || t.startsWith("•")) t = t.substring(1).trim()
        return if (t.length > 220) t.take(220).trim() else t
    }

    private fun simpleKey(t: String) = t.lowercase().replace(Regex("[^\\p{L}\\p{N}]"), "")

    fun similar(a: String, b: String): Boolean {
        val ka = simpleKey(a)
        val kb = simpleKey(b)
        if (ka == kb) return true
        return minOf(ka.length, kb.length) >= 14 && (ka.contains(kb) || kb.contains(ka))
    }

    fun sectionByTag(tagRaw: String): Section? {
        val tag = tagRaw.lowercase().trim().replace("*", "").replace("`", "").trim()
        return when (tag) {
            "не работает", "неработает" -> Section.NotWorks
            "работает" -> Section.Works
            "дело", "оффер", "продукт", "бизнес" -> Section.Business
            "цель" -> Section.Goal
            "аудитория" -> Section.Audience
            "стиль" -> Section.Style
            "правило", "правила", "запрет", "запреты" -> Section.Rules
            else -> null
        }
    }

    fun entries(section: Section): List<Entry> = synchronized(lock) { data()[section]!!.toList() }

    fun count(): Int = synchronized(lock) { data().values.sumOf { it.size } }

    /** Puts a fact into a section; the same fact again (by letters) replaces the older one. True if new. */
    fun add(section: Section, textRaw: String): Boolean = synchronized(lock) {
        val text = clean(textRaw)
        if (text.length < 4) return false
        val list = data()[section]!!
        val now = System.currentTimeMillis() / 1000
        val i = list.indexOfFirst { similar(it.text, text) }
        if (i >= 0) {
            val same = list[i].text == text
            list.removeAt(i)
            list.add(Entry(text, now))
            write()
            return !same
        }
        list.add(Entry(text, now))
        while (list.size > section.maxItems) list.removeAt(0)
        write()
        true
    }

    fun remove(section: Section, text: String) {
        synchronized(lock) {
            data()[section]!!.removeAll { it.text == text }
            write()
        }
    }

    fun newSince(since: Long): List<Pair<Section, Entry>> = synchronized(lock) {
        val r = ArrayList<Pair<Section, Entry>>()
        for (s in Section.values()) for (e in data()[s]!!) if (e.at > since) r.add(s to e)
        r
    }

    private fun stems(text: String): Set<String> =
        Regex("[\\p{L}\\p{N}]{4,}").findAll(text.lowercase()).map { it.value.take(5) }.toSet()

    /** What goes into a prompt: the sections with their facts, the newest first when they do not all
     *  fit into [limit] characters; with a [query] the facts that share words with it go first. */
    fun block(limit: Int, query: String = ""): String = synchronized(lock) {
        val d = data()
        if (d.values.all { it.isEmpty() }) return ""
        val wanted = stems(query)
        val priority = listOf(Section.Business, Section.Goal, Section.Audience, Section.Rules, Section.Works, Section.NotWorks, Section.Style)
        var remaining = limit
        val perSection = maxOf(300, limit / 3)
        val taken = HashMap<Section, List<Entry>>()
        for (s in priority) {
            val list = d[s]!!.asReversed()
            val ordered = if (wanted.isEmpty()) list else list.sortedByDescending { e -> stems(e.text).count { it in wanted } }
            var used = 0
            val picked = ArrayList<Entry>()
            for (e in ordered) {
                val cost = e.text.length + 3
                if (cost > remaining || used + cost > perSection) continue
                picked.add(e); remaining -= cost; used += cost
            }
            taken[s] = picked
        }
        val sb = StringBuilder()
        for (s in Section.values()) {
            val list = taken[s] ?: continue
            if (list.isEmpty()) continue
            sb.append(s.title).append(":\n")
            for (e in list.asReversed()) sb.append("• ").append(e.text).append('\n')
        }
        sb.toString()
    }

    // ---- the user's own messages, as samples of their style (ai_examples.json) ----

    private fun examplesFile() = File(GgtgAi.dir(), "ai_examples.json")

    fun examples(): List<String> = synchronized(lock) {
        val text = GgtgAi.readText(examplesFile()) ?: return emptyList()
        try {
            val arr = JSONObject(text).optJSONArray("items") ?: return emptyList()
            (0 until arr.length()).map { arr.getString(it) }
        } catch (t: Throwable) { emptyList() }
    }

    fun addExample(text: String) {
        synchronized(lock) {
            val clean = text.trim()
            if (clean.length < 8 || clean.length > 400) return
            val list = examples().toMutableList()
            if (list.any { it == clean }) return
            list.add(clean)
            while (list.size > 12) list.removeAt(0)
            val arr = JSONArray()
            list.forEach { arr.put(it) }
            GgtgAi.writeText(examplesFile(), JSONObject().put("items", arr).toString())
        }
    }

    fun examplesBlock(count: Int = 4): String = examples().takeLast(count).joinToString("") { "• $it\n" }
}
