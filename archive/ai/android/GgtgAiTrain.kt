package app.ggtg.ai

import app.ggtg.GgtgAccounts
import app.ggtg.GgtgProcess
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import org.json.JSONArray
import org.json.JSONObject
import org.telegram.messenger.MessagesController
import org.telegram.messenger.UserConfig
import org.telegram.messenger.UserObject
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

// ggtg: learning from the user's own chats (core/ggtg_ai_train.h and settings/ggtg_ai_train.h). A long
// run in the background: it reads the personal chats of the chosen accounts where the user wrote and
// teaches the AI from them - how the user writes (style), what people ask, what they object to and
// what the user answered, how the talk ended. The run is a row in "Процессы" and survives the phone
// going to the background (the foreground service of GgtgProcess); it waits out the limits of the free
// keys, stops on request and goes on from the same chat next time. The text leaves with the names
// of the person and of the user hidden and names, numbers, usernames replaced by marks.
object GgtgAiTrain {

    private const val WINDOW = 6500
    private const val MAX_WINDOWS = 3
    private const val MAX_MESSAGES = 600
    private const val MIN_MINE = 2
    private const val NOTES_BATCH = 40
    private val ENDINGS = listOf("deal", "refusal", "silence", "going")

    class State {
        val accounts = ArrayList<Long>() // user ids
        val done = HashSet<String>()
        var chats = 0
        var skipped = 0
        var messages = 0
        val endings = IntArray(4)
        val styleNotes = ArrayList<String>()
        val outcomeNotes = ArrayList<String>()
    }

    private val lock = Any()
    private var state: State? = null
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    @Volatile private var stop = false
    @Volatile var running = false
        private set
    @Volatile var status = ""
        private set

    private fun file() = File(GgtgAi.dir(), "ai_training.json")

    fun current(): State = synchronized(lock) {
        state?.let { return it }
        val s = State()
        GgtgAi.readText(file())?.let { text ->
            try {
                val o = JSONObject(text)
                o.optJSONArray("accounts")?.let { a -> for (i in 0 until a.length()) s.accounts.add(a.getString(i).toLong()) }
                o.optJSONArray("done")?.let { a -> for (i in 0 until a.length()) s.done.add(a.getString(i)) }
                s.chats = o.optInt("chats"); s.skipped = o.optInt("skipped"); s.messages = o.optInt("messages")
                o.optJSONArray("endings")?.let { a -> for (i in 0 until minOf(a.length(), 4)) s.endings[i] = a.getInt(i) }
                o.optJSONArray("styleNotes")?.let { a -> for (i in 0 until a.length()) s.styleNotes.add(a.getString(i)) }
                o.optJSONArray("outcomeNotes")?.let { a -> for (i in 0 until a.length()) s.outcomeNotes.add(a.getString(i)) }
            } catch (t: Throwable) { }
        }
        state = s
        s
    }

    fun saveState() {
        val s = current()
        synchronized(lock) {
            val o = JSONObject()
            o.put("accounts", JSONArray(s.accounts.map { it.toString() }))
            o.put("done", JSONArray(s.done.toList()))
            o.put("chats", s.chats).put("skipped", s.skipped).put("messages", s.messages)
            o.put("endings", JSONArray(s.endings.toList()))
            o.put("styleNotes", JSONArray(s.styleNotes)).put("outcomeNotes", JSONArray(s.outcomeNotes))
            GgtgAi.writeText(file(), o.toString())
        }
    }

    fun summary(): String {
        val s = current()
        if (s.chats == 0 && s.skipped == 0 && GgtgAiPlaybook.entries().isEmpty()) return "Ещё не запускалось."
        return "Прочитано чатов: ${s.chats} (пропущено без ваших слов: ${s.skipped}), сообщений: ${s.messages}. Концовки: " +
            "договорились ${s.endings[0]}, отказ ${s.endings[1]}, молчание ${s.endings[2]}, продолжается ${s.endings[3]}. " +
            "Вопросов и возражений с вашими ответами: ${GgtgAiPlaybook.entries().size}."
    }

    fun startOver() { val s = current(); s.done.clear(); s.chats = 0; s.skipped = 0; s.messages = 0; s.endings.fill(0); saveState() }

    suspend fun forget() {
        GgtgAiPlaybook.clear()
        val s = current(); s.styleNotes.clear(); s.outcomeNotes.clear()
        startOver()
        GgtgAiKnowledge.setLearned("", emptyList())
    }

    // ---- one conversation ----

    private fun windows(lines: List<GgtgAiText.Line>): List<String> {
        val fmt = SimpleDateFormat("dd.MM.yy HH:mm", Locale.getDefault())
        val pieces = lines.filter { it.text.isNotBlank() }.map {
            "[" + fmt.format(Date(it.date * 1000)) + "] " + (if (it.mine) "Я" else "Собеседник") + ": " + it.text.replace(Regex("\\s+"), " ").take(400)
        }
        val result = ArrayList<String>()
        var current = StringBuilder()
        for (p in pieces) {
            if (current.isNotEmpty() && current.length + p.length > WINDOW) { result.add(current.toString()); current = StringBuilder() }
            current.append(p).append('\n')
        }
        if (current.isNotEmpty()) result.add(current.toString())
        while (result.size > MAX_WINDOWS) result.removeAt(1) // the first and the newest stay
        return result
    }

    private const val EXTRACTION_SYSTEM =
        "Ты изучаешь личную переписку пользователя («Я») в Telegram с одним собеседником, чтобы помощник отвечал так же, как он. Имена, " +
            "номера и ники заменены метками вроде <N1>: так и задумано, не расшифровывай их и не пиши в ответе.\n" +
            "Выведи строки ТОЛЬКО в этих форматах, по одной на строку, без вступлений и нумерации:\n" +
            "СТИЛЬ: <одно наблюдение о том, как пишет «Я»: тон, «ты» или «вы», длина, эмодзи, приветствия, обороты>\n" +
            "ВОПРОС: <вопрос собеседника, сформулированный обобщённо> => <как ответил «Я», близко к его словам>\n" +
            "ВОЗРАЖЕНИЕ: <сомнение или отказ собеседника, сформулированный обобщённо> => <как ответил «Я», близко к его словам>\n" +
            "ИТОГ: <договорились | отказ | молчание | продолжается> | <что в этом диалоге сработало или не сработало, одной фразой>\n" +
            "Правила: вопросы и возражения пиши так, чтобы они подошли и другим людям: без имён, номеров и личных подробностей " +
            "собеседника. Вопрос или возражение берёшь только если «Я» на него ответил. Ничего не выдумывай, только из переписки. " +
            "Не больше 3 строк СТИЛЬ, не больше 6 строк ВОПРОС и ВОЗРАЖЕНИЕ вместе, одна строка ИТОГ. Если выводов нет, ответь одним словом: НЕТ."

    private suspend fun learn(windows: List<String>): Pair<GgtgAiPlaybook.Insight?, String?> {
        val insight = GgtgAiPlaybook.Insight()
        for (w in windows) {
            val r = GgtgAi.ask(EXTRACTION_SYSTEM, w, 0.2, 1200, hard = false, keepMarks = true)
            if (r.error != null) return null to r.error
            GgtgAiPlaybook.parse(r.text, insight)
        }
        return insight to null
    }

    private suspend fun keep(insight: GgtgAiPlaybook.Insight, messages: Int) {
        val s = current()
        s.chats++; s.messages += messages
        for (style in insight.styles) s.styleNotes.add(style.take(240))
        for ((kind, what, answer) in insight.entries) GgtgAiPlaybook.add(kind, what, listOf(answer), if (insight.ending == "deal") 1 else 0)
        insight.ending?.let { e ->
            s.endings[ENDINGS.indexOf(e)]++
            if (insight.lesson.isNotEmpty()) {
                val word = mapOf("deal" to "договорились", "refusal" to "отказ", "silence" to "молчание", "going" to "продолжается")[e]
                s.outcomeNotes.add("$word: " + insight.lesson.take(240))
            }
        }
        while (s.styleNotes.size > 400) s.styleNotes.removeAt(0)
        while (s.outcomeNotes.size > 400) s.outcomeNotes.removeAt(0)
        saveState()
    }

    private fun hasNotes() = current().let { it.styleNotes.isNotEmpty() || it.outcomeNotes.isNotEmpty() }

    /** The notes of many chats become a few rules in the profile's "Стиль", "Что работает" and "Что не работает". */
    private suspend fun consolidate(): String? {
        val s = current()
        val style = s.styleNotes.toList()
        val outcomes = s.outcomeNotes.toList()
        if (style.size >= 3) {
            val r = GgtgAi.ask("Ниже наблюдения о манере письма пользователя («Я»), собранные по многим его перепискам. Объедини их в 6-10 коротких " +
                "правил стиля, по одному в строке, каждая начинается с «- »: тон, «ты» или «вы», длина сообщений, эмодзи, приветствия, " +
                "характерные обороты. Бери только то, что повторяется в нескольких наблюдениях. Без имён и вступлений.",
                style.joinToString("\n"), 0.2, 900, hard = true)
            if (r.error != null) return r.error
            for (row in r.text.split('\n')) { val t = row.trim(); if (t.startsWith("-") || t.startsWith("*") || t.startsWith("•")) GgtgAiProfile.add(GgtgAiProfile.Section.Style, t) }
            s.styleNotes.clear(); saveState()
        }
        if (outcomes.size >= 3) {
            val r = GgtgAi.ask("Ниже итоги многих личных диалогов пользователя: чем закончился диалог и что в нём сработало или нет. Сделай до 8 " +
                "выводов, каждый одной строкой в формате «работает: ...» или «не работает: ...». Бери только то, что подтверждается " +
                "несколькими диалогами. Без имён и вступлений.", outcomes.joinToString("\n"), 0.2, 900, hard = true)
            if (r.error != null) return r.error
            for (row in r.text.split('\n')) {
                val line = row.trim(); val colon = line.indexOf(':')
                if (colon <= 0) continue
                val section = GgtgAiProfile.sectionByTag(line.substring(0, colon))
                if (section == GgtgAiProfile.Section.Works || section == GgtgAiProfile.Section.NotWorks) GgtgAiProfile.add(section, line.substring(colon + 1))
            }
            s.outcomeNotes.clear(); saveState()
        }
        return null
    }

    // ---- the run ----

    fun accountsToLearn(): List<Int> = (0 until UserConfig.MAX_ACCOUNT_COUNT).filter { UserConfig.getInstance(it).isClientActivated }

    fun stopRun() { stop = true }

    /** Starts (or goes on): returns an error text, null when it started. */
    fun start(): String? {
        if (running) return "Обучение уже идёт."
        if (!GgtgAi.ready()) return "Нужен бесплатный ключ: шаг «1. Ключи»."
        val s = current()
        val slots = accountsToLearn().filter { UserConfig.getInstance(it).clientUserId in s.accounts }
        if (slots.isEmpty()) return "Отметьте хотя бы один аккаунт."
        running = true; stop = false
        val handle = GgtgProcess.start("Обучение ИИ на ваших чатах", GgtgProcess.Kind.Other)
        handle.log(if (s.done.isEmpty()) "Начинаю: читаю личные чаты, где писали вы." else "Продолжаю с того же места: уже прочитано чатов: ${s.done.size}.")
        scope.launch { try { run(handle, slots) } catch (t: Throwable) { handle.fail("Ошибка: ${t.message}") } finally { running = false; status = "" } }
        return null
    }

    private suspend fun run(handle: GgtgProcess.Handle, slots: List<Int>) {
        val s = current()
        var total = 0
        var finished = 0
        var sinceConsolidation = 0
        var waits = 0
        GgtgAccounts.wake(slots)
        for (slot in slots) {
            val uc = UserConfig.getInstance(slot)
            val controller = MessagesController.getInstance(slot)
            val queue = ArrayList<Long>()
            for (d in ArrayList(controller.getAllDialogs())) {
                val id = d.id
                if (id <= 0 || id == 777000L) continue
                val user = controller.getUser(id) ?: continue
                if (user.bot || user.self || user.deleted || user.support) continue
                if ("${uc.clientUserId}:$id" in s.done) continue
                queue.add(id)
            }
            total += queue.size
            handle.log("Аккаунт ${GgtgAccounts.label(slot)}: личных чатов к чтению: ${queue.size}")
            for (id in queue) {
                if (stop) { finish(handle, "Остановлено. Нажмите «Продолжить», чтобы идти дальше с того же места."); return }
                val user = controller.getUser(id) ?: continue
                status = "Читаю чат ${finished + 1} из $total: ${UserObject.getUserName(user)}"
                val lines = ArrayList<GgtgAiText.Line>()
                var offset = 0
                var failed = false
                while (lines.size < MAX_MESSAGES) {
                    val page = GgtgAiText.history(slot, id, 100, offset)
                    if (page == null) { failed = true; break }
                    lines.addAll(0, page)
                    val oldest = page.minOfOrNull { it.id } ?: 0
                    if (page.size < 100 || oldest == 0) break
                    offset = oldest
                    delay(900)
                }
                if (failed && lines.isEmpty()) { handle.log("Чат «${UserObject.getUserName(user)}» не прочитался, пропускаю"); finished++; continue }
                val key = "${uc.clientUserId}:$id"
                if (lines.count { it.mine && it.text != "[вложение]" } < MIN_MINE) {
                    s.skipped++; s.done.add(key); finished++; saveState()
                    handle.progress(finished, maxOf(total, 1))
                    continue
                }
                val names = listOf(UserObject.getUserName(user), user.first_name.orEmpty(), user.last_name.orEmpty(), user.username.orEmpty(),
                    uc.currentUser?.first_name.orEmpty(), uc.currentUser?.last_name.orEmpty(), uc.currentUser?.username.orEmpty())
                val hidden = lines.map { GgtgAiText.Line(it.mine, GgtgAi.hideNames(it.text, names), it.date, it.id) }
                val parts = windows(hidden)
                var insight: GgtgAiPlaybook.Insight? = null
                var attempt = 0
                while (insight == null) {
                    if (stop) { finish(handle, "Остановлено. Нажмите «Продолжить», чтобы идти дальше с того же места."); return }
                    val (got, error) = learn(parts)
                    if (error == null) { insight = got; waits = 0; break }
                    if (error.contains("нет ни одного ключа") || error.contains("выключен")) { finish(handle, "Остановлено: $error", false); return }
                    if (error.contains("не ответила ни одна модель") && attempt >= 2) { handle.log("Чат пропущен: модели не смогли его разобрать"); break }
                    if (++waits > 36) { finish(handle, "Остановлено: бесплатные лимиты не вернулись за три часа. Нажмите «Продолжить» позже или добавьте ещё ключ.", false); return }
                    handle.log("Модели отдыхают после лимита, жду 5 минут (ключи можно добавить в шаге «1. Ключи»)...")
                    if (!GgtgAi.waitForKeys(300) { stop }) { finish(handle, "Остановлено. Нажмите «Продолжить», чтобы идти дальше с того же места."); return }
                    attempt++
                }
                if (insight != null) { keep(insight, lines.size); sinceConsolidation++ }
                s.done.add(key); finished++; saveState()
                handle.progress(finished, maxOf(total, 1))
                if (sinceConsolidation >= 25 && hasNotes()) {
                    sinceConsolidation = 0
                    status = "Свожу выводы в профиль..."
                    if (consolidate() == null) GgtgAiPlaybook.syncKnowledge()
                }
                delay(500L + (Math.random() * 1000).toLong())
            }
        }
        finish(handle, "Готово. Прочитано чатов за всё время: ${s.chats}. Вопросов и возражений с вашими ответами: ${GgtgAiPlaybook.entries().size}.")
    }

    private suspend fun finish(handle: GgtgProcess.Handle, message: String, ok: Boolean = true) {
        status = "Подвожу итоги..."
        if (hasNotes()) consolidate()
        GgtgAiPlaybook.syncKnowledge()
        if (ok) handle.succeed(message) else handle.fail(message)
    }
}
