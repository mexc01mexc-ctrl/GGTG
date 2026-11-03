package app.ggtg.ai

import org.json.JSONArray
import org.json.JSONObject
import java.io.File

// ggtg: the playbook (core/ggtg_ai_train.h): what people ask and object to, and how the user answers. It
// is filled by training on the user's chats and by the answers that worked in the reply feed, and
// it is a source of the "Материалы" base - so the assistant gets the ones that fit the talk, the
// answers that sold first. File: ggtg_bulk_db/ai_playbook.json.
object GgtgAiPlaybook {

    class Entry(
        val kind: String, // question, objection, message
        val what: String,
        val answers: MutableList<String>,
        var count: Int = 1,
        var at: Long = 0,
        var wins: Int = 0
    )

    fun weight(e: Entry) = e.wins * 3 + e.count

    private const val MAX = 500
    private val lock = Any()
    private var cache: MutableList<Entry>? = null

    private fun file() = File(GgtgAi.dir(), "ai_playbook.json")

    private fun data(): MutableList<Entry> {
        cache?.let { return it }
        val list = ArrayList<Entry>()
        GgtgAi.readText(file())?.let { text ->
            try {
                val arr = JSONObject(text).optJSONArray("entries") ?: JSONArray()
                for (i in 0 until arr.length()) {
                    val o = arr.getJSONObject(i)
                    val answers = o.optJSONArray("answers") ?: JSONArray()
                    val e = Entry(o.optString("kind"), o.optString("what"), (0 until answers.length()).map { answers.getString(it) }.toMutableList(),
                        maxOf(o.optInt("count", 1), 1), o.optLong("at"), o.optInt("wins"))
                    if (e.what.isNotEmpty()) list.add(e)
                }
            } catch (t: Throwable) { }
        }
        cache = list
        return list
    }

    fun entries(): List<Entry> = synchronized(lock) { data().toList() }

    private fun save() {
        val arr = JSONArray()
        for (e in data()) arr.put(JSONObject().put("kind", e.kind).put("what", e.what).put("answers", JSONArray(e.answers))
            .put("count", e.count).put("at", e.at).put("wins", e.wins))
        GgtgAi.writeText(file(), JSONObject().put("v", 1).put("entries", arr).toString())
    }

    private fun clean(raw: String) = raw.trim().replace(Regex("\\s+"), " ").trimStart('-', '*', '•', ' ').take(220)

    /** A question or an objection met in a chat; the same one again only raises its count and may
     *  bring a second way to answer; an answer that worked goes first. */
    fun add(kind: String, whatRaw: String, answersRaw: List<String>, wins: Int = 0) {
        synchronized(lock) {
            val what = clean(whatRaw)
            val answers = answersRaw.map { it.trim().replace(Regex("\\s+"), " ").take(300) }.filter { it.isNotEmpty() }
            if (what.length < 6) return
            val list = data()
            val now = System.currentTimeMillis() / 1000
            val existing = list.firstOrNull { GgtgAiProfile.similar(it.what, what) }
            if (existing != null) {
                existing.count++
                existing.wins += wins
                existing.at = now
                for (a in answers) {
                    val known = existing.answers.indexOfFirst { GgtgAiProfile.similar(it, a) }
                    if (known < 0 && existing.answers.size < 2) existing.answers.add(a)
                    else if (known > 0 && wins > 0) { existing.answers.removeAt(known); existing.answers.add(0, a) }
                    else if (known < 0 && wins > 0) { existing.answers.add(0, a); while (existing.answers.size > 2) existing.answers.removeAt(existing.answers.size - 1) }
                }
            } else {
                list.add(Entry(kind, what, answers.toMutableList(), 1, now, wins))
                if (list.size > MAX) {
                    list.sortWith(compareBy({ -weight(it) }, { -it.at }))
                    while (list.size > MAX) list.removeAt(list.size - 1)
                }
            }
            save()
        }
    }

    fun remove(what: String) {
        synchronized(lock) { data().removeAll { it.what == what }; save() }
    }

    fun clear() {
        synchronized(lock) { data().clear(); save() }
    }

    fun since(at: Long): List<Entry> = synchronized(lock) { data().filter { it.at > at } }

    fun chunk(e: Entry): String {
        val sb = StringBuilder(when (e.kind) { "objection" -> "Возражение: "; "message" -> "Собеседник написал: "; else -> "Частый вопрос: " })
        sb.append(e.what)
        if (e.count > 1) sb.append(" (встречалось ${e.count} раз)")
        if (e.wins > 0) sb.append(" (этот ответ сработал ${e.wins} раз: отвечай так)")
        for ((i, a) in e.answers.withIndex()) sb.append(if (i == 0) "\nМой ответ: " else "\nДругой мой ответ: ").append(a)
        return sb.toString()
    }

    /** The playbook as a source of the "Материалы" base. */
    suspend fun syncKnowledge() {
        val chunks = entries().sortedByDescending { weight(it) }.filter { it.answers.isNotEmpty() }.map { chunk(it) }
        GgtgAiKnowledge.setLearned("Опыт из ваших чатов: вопросы, возражения и ваши ответы", chunks)
    }

    /** An answer of the user's that worked in the feed: it is offered first next time. */
    suspend fun addWin(kind: String, what: String, answer: String, wins: Int = 1) {
        add(kind, what, listOf(answer), wins)
        syncKnowledge()
    }

    // ---- what one conversation gives (the model's lines) ----

    class Insight {
        val styles = ArrayList<String>()
        val entries = ArrayList<Triple<String, String, String>>() // kind, what, answer
        var ending: String? = null // deal, refusal, silence, going
        var lesson = ""
    }

    private val head = Regex("^\\s*(?:[-*•]|\\d+[.)])?\\s*\\**\\s*(СТИЛЬ|ВОПРОС|ВОЗРАЖЕНИЕ|ИТОГ)\\s*\\**\\s*:\\s*(.+?)\\s*$", RegexOption.IGNORE_CASE)

    fun parse(text: String, insight: Insight) {
        for (row in text.split('\n')) {
            val m = head.find(row) ?: continue
            val tag = m.groupValues[1].uppercase()
            val body = m.groupValues[2].trim()
            when (tag) {
                "СТИЛЬ" -> if (insight.styles.size < 3 && body.length >= 8) insight.styles.add(body)
                "ИТОГ" -> {
                    val parts = body.split('|')
                    val word = parts[0].lowercase()
                    insight.ending = when {
                        word.contains("договор") -> "deal"
                        word.contains("отказ") -> "refusal"
                        word.contains("молч") -> "silence"
                        else -> "going"
                    }
                    insight.lesson = if (parts.size > 1) parts.drop(1).joinToString(" ").trim() else ""
                }
                else -> {
                    var cut = body.indexOf("=>")
                    var skip = 2
                    if (cut < 0) { cut = body.indexOf('→'); skip = 1 }
                    if (cut < 0) continue
                    val what = body.substring(0, cut).trim()
                    val answer = body.substring(cut + skip).trim()
                    if (what.length < 6 || answer.length < 2 || insight.entries.size >= 8) continue
                    insight.entries.add(Triple(if (tag == "ВОПРОС") "question" else "objection", what, answer))
                }
            }
        }
    }
}
