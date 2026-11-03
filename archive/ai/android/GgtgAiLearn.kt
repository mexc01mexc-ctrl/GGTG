package app.ggtg.ai

import org.json.JSONArray
import org.json.JSONObject
import java.io.File

// ggtg: how the AI learns about the user: it turns what it is shown (a talk, the user's edits of its drafts)
// into facts of the profile, and keeps the pairs "draft -> what was really sent". A draft that the
// user changed is the best lesson there is: after a few such pairs the AI writes down the habits.
object GgtgAiLearn {

    /** Asks the model for facts of [allowed] sections about the user, one per line "тег: факт", and puts
     *  them into the profile. Returns how many were added; null with the error text when the model failed. */
    suspend fun extract(task: String, allowed: List<GgtgAiProfile.Section>, material: String, maxTokens: Int = 500): Pair<Int, String?> {
        val tags = allowed.joinToString(", ") { "«${it.tag}»" }
        val sections = allowed.joinToString("\n") { "${it.tag}: ${hint(it)}" }
        val system = task + "\n\nВерни только новые факты, по одному на строке в виде «тег: факт» (теги: $tags), без нумерации и вступлений. " +
            "Факт - одна короткая фраза. Значения тегов:\n$sections\nУже известно (не повторяй):\n" +
            GgtgAiProfile.block(1500).ifEmpty { "(пока пусто)" } + "\nЕсли нового нет, ответь одним словом: НЕТ."
        val r = GgtgAi.ask(system, material, 0.1, maxTokens, hard = true)
        if (r.error != null) return 0 to r.error
        if (r.text.trim().length < 12 && r.text.trim().startsWith("НЕТ", true)) return 0 to null
        var added = 0
        for (row in r.text.split('\n')) {
            val line = row.trim().trimStart('-', '*', '•', ' ')
            val colon = line.indexOf(':')
            if (colon <= 0) continue
            val section = GgtgAiProfile.sectionByTag(line.substring(0, colon)) ?: continue
            if (section !in allowed) continue
            if (GgtgAiProfile.add(section, line.substring(colon + 1))) added++
        }
        return added to null
    }

    private fun hint(s: GgtgAiProfile.Section) = when (s) {
        GgtgAiProfile.Section.Business -> "чем занимается пользователь, что и на каких условиях предлагает"
        GgtgAiProfile.Section.Goal -> "к чему вести собеседника и что считается успехом"
        GgtgAiProfile.Section.Audience -> "кому пишет: страны, языки, ниша, интересы"
        GgtgAiProfile.Section.Style -> "как пишет пользователь: тон, «ты» или «вы», длина, эмодзи, обороты"
        GgtgAiProfile.Section.Rules -> "что можно и нельзя говорить"
        GgtgAiProfile.Section.Works -> "что подтверждённо давало ответы и результат"
        GgtgAiProfile.Section.NotWorks -> "после чего молчали, отказывали или банили"
    }

    // ---- the user's edits of drafts (ai_pairs.json) ----

    private class EditPair(val draft: String, val sent: String, val at: Long, var learned: Boolean)

    private fun file() = File(GgtgAi.dir(), "ai_pairs.json")

    private fun loadPairs(): MutableList<EditPair> {
        val text = GgtgAi.readText(file()) ?: return ArrayList()
        return try {
            val arr = JSONObject(text).optJSONArray("items") ?: JSONArray()
            (0 until arr.length()).map {
                val o = arr.getJSONObject(it)
                EditPair(o.optString("draft"), o.optString("sent"), o.optLong("at"), o.optBoolean("learned"))
            }.toMutableList()
        } catch (t: Throwable) { ArrayList() }
    }

    private fun savePairs(list: List<EditPair>) {
        val arr = JSONArray()
        for (p in list.takeLast(30)) arr.put(JSONObject().put("draft", p.draft).put("sent", p.sent).put("at", p.at).put("learned", p.learned))
        GgtgAi.writeText(file(), JSONObject().put("items", arr).toString())
    }

    @Synchronized fun recordEdit(draft: String, sent: String) {
        val a = draft.trim().replace(Regex("\\s+"), " ")
        val b = sent.trim().replace(Regex("\\s+"), " ")
        if (a.isEmpty() || b.isEmpty() || a == b || a.length > 800 || b.length > 800) return
        val list = loadPairs()
        list.add(EditPair(a, b, System.currentTimeMillis() / 1000, false))
        savePairs(list)
    }

    @Volatile private var learning = false

    /** When enough new edits have come together, the model compares the drafts with what was sent
     *  and writes down what the user changes (style, rules). */
    suspend fun maybeLearnFromEdits() {
        if (!GgtgAi.ready() || learning) return
        val pairs = synchronized(this) { loadPairs() }
        val fresh = pairs.withIndex().filter { !it.value.learned }
        if (fresh.size < 3) return
        learning = true
        try {
            val take = fresh.takeLast(8)
            val material = take.joinToString("") { "Черновик ИИ: ${it.value.draft}\nОтправил пользователь: ${it.value.sent}\n\n" }
            val (_, error) = extract(
                "Пользователь поправил черновики ИИ-помощника перед отправкой. По разнице видно, как он пишет на самом деле и что ему не " +
                    "нравится в ответах ИИ. Выпиши устойчивые привычки, которые повторяются или очевидны; случайные правки не записывай.",
                listOf(GgtgAiProfile.Section.Style, GgtgAiProfile.Section.Rules), material, 400)
            if (error == null) synchronized(this) {
                val now = loadPairs()
                for (t in take) if (t.index < now.size) now[t.index].learned = true
                savePairs(now)
            }
        } finally { learning = false }
    }
}
