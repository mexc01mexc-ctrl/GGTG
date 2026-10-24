package app.ggtg.ai

import app.ggtg.GgtgDb
import org.json.JSONArray
import org.json.JSONObject

// ggtg: "Спросить свой второй мозг": one chat about everything. The agent sees the user's profile, the
// materials, the feed of answers and what was learned from the chats, and the mailing numbers; it
// calls these as tools when it needs them. Cloud models only: the cleverest one available answers.
object GgtgAiBrain {

    class Turn(val role: String, val text: String)

    private fun tool(name: String, description: String, argument: String? = null, argumentDescription: String = ""): JSONObject {
        val properties = JSONObject()
        val required = JSONArray()
        if (argument != null) {
            properties.put(argument, JSONObject().put("type", "string").put("description", argumentDescription))
            required.put(argument)
        }
        return JSONObject().put("type", "function").put("function", JSONObject().put("name", name).put("description", description)
            .put("parameters", JSONObject().put("type", "object").put("properties", properties).put("required", required)))
    }

    private val tools: JSONArray = JSONArray()
        .put(tool("my_materials", "Найти в материалах пользователя (прайсы, презентации, сайты, видео, документы, а также выученные из его чатов " +
            "вопросы, возражения и его ответы) то, что относится к вопросу.", "query", "О чём искать, своими словами"))
        .put(tool("set_option", "Включить или выключить настройку GGTG, когда пользователь просит: «ai on», «ai off» (весь ИИ), " +
            "«night_training on/off» (учиться на чатах по ночам).", "change", "Например «night_training on»"))
        .put(tool("what_i_learned", "Что ИИ выучил о пользователе: профиль, лучшие (сработавшие) ответы на вопросы и возражения, итоги обучения на чатах."))
        .put(tool("mailing_stats", "Сколько людей в базах пользователя, скольким уже написали и сколько ответили."))
        .put(tool("forget", "Забыть факт о пользователе: убирает из профиля все записи, в которых есть эти слова. Вызывай, когда пользователь " +
            "просит забыть, убрать или исправить что-то о нём. Сначала вызови what_i_learned, чтобы увидеть точные формулировки.", "text",
            "Слова или кусок фразы из факта, который надо забыть"))
        .put(tool("add_material", "Добавить в материалы ИИ ссылку (сайт, PDF, YouTube) или текст (прайс, условия, FAQ, описание продукта), чтобы ИИ " +
            "опирался на это в ответах людям.", "source", "Ссылка или сам текст"))
        .put(tool("list_materials", "Какие материалы уже добавлены: названия, виды, сколько кусков."))
        .put(tool("training", "Обучение ИИ на личных чатах пользователя: узнать, идёт ли оно и как далеко, запустить или остановить. " +
            "Запускай только если пользователь прямо просит.", "action", "status, start или stop"))
        .put(tool("remember", "Запомнить надолго важный факт о пользователе или его деле, который он сам сказал. Одна короткая фраза, начинается с " +
            "тега и двоеточия: «дело:», «цель:», «аудитория:», «правило:», «работает:» или «не работает:».", "fact", "Тег и факт, например «аудитория: Корея, трейдинг»"))

    private fun interviewPrompt(): String =
        "Ты берёшь у пользователя интервью, чтобы узнать его дело и цель: ты - ИИ внутри GGTG (программа для Telegram: рассылки и ответы людям). " +
            "Задавай по ОДНОМУ короткому вопросу по-русски. Узнай по очереди: чем занимается, что и на каких условиях предлагает (цены, ссылки), кому " +
            "пишет (страны, языки, ниша), к чему вести собеседника, как общаться (ты/вы, тон), что нельзя говорить. После каждого ответа вызови " +
            "remember с фактом (в формате «тег: факт»), потом задай следующий вопрос. Когда узнал достаточно, коротко подведи итог. Без markdown.\n\n" +
            "Что уже известно:\n" + GgtgAiProfile.block(2500).ifEmpty { "(пока ничего)" }

    private fun systemPrompt(interview: Boolean): String {
        if (interview) return interviewPrompt()
        val s = GgtgAi.settings()
        return "Ты - ИИ внутри GGTG, второй мозг пользователя: GGTG - программа для Telegram (много аккаунтов, базы людей, рассылка личных " +
            "сообщений, ответы людям). Твоя задача - помочь пользователю получать больше ответов и продаж.\n" +
            "Сегодня " + java.text.SimpleDateFormat("dd.MM.yyyy", java.util.Locale.getDefault()).format(java.util.Date()) + ".\n\n" +
            "Как работать:\n" +
            "• Про продукт, цены и условия пользователя сначала смотри my_materials; что работает " +
            "в его переписках - what_i_learned; цифры рассылок - mailing_stats. Не выдумывай цифры и факты.\n" +
            "• Лучшие ответы (помечены «сработал») предлагай в первую очередь.\n" +
            "• Узнал о пользователе что-то важное и долгое - вызови remember.\n" +
            "• Ты - единственный помощник пользователя в этом окне: он говорит с тобой обо всём. Просит забыть или исправить что-то о нём - " +
            "вызови forget. Даёт ссылку, прайс или текст - add_material. Спрашивает, что уже добавлено - list_materials. Про обучение на его " +
            "чатах - training. Просит рассказать, что ты знаешь о нём - what_i_learned, и перескажи по-человечески, по пунктам.\n" +
            "• Если пользователь просит расспросить его: задавай ОДИН короткий вопрос за раз (дело, цель, аудитория, стиль, правила, что " +
            "работает), после каждого ответа вызывай remember и переходи к следующему вопросу; когда всё важное узнал - скажи об этом.\n" +
            "• Если о деле пользователя почти ничего не известно, сначала одним коротким вопросом узнай нишу и цель.\n" +
            "• Предлагая тексты сообщений, пиши каждый так: «ВАРИАНТ 1: текст».\n" +
            "• Отвечай по-русски, конкретно: что сделать, какой текст. Без markdown: без **, без #, списки через «•».\n" +
            "• Береги аккаунты: советуй то, что не ведёт к спам-блокам и банам.\n\n" +
            "О пользователе (написано им самим):\n" + s.chatPrompt.trim().ifEmpty { "(пока ничего)" }.take(1500) +
            (GgtgAiProfile.block(2500).takeIf { it.isNotEmpty() }?.let { "\n\nЧто ты о нём знаешь:\n$it" } ?: "")
    }

    private suspend fun runTool(name: String, args: JSONObject, step: (String) -> Unit): String {
        return when (name) {
            "my_materials" -> {
                val q = args.optString("query").trim()
                step("ищет в материалах: «$q»")
                GgtgAiKnowledge.warm(q)
                GgtgAiKnowledge.block(5000, q).ifEmpty { "В материалах пользователя ничего нет." }
            }
            "set_option" -> {
                val parts = args.optString("change").trim().lowercase().split(Regex("\\s+"))
                if (parts.size != 2 || parts[1] !in listOf("on", "off")) return "Не понял настройку."
                val on = parts[1] == "on"
                val s = GgtgAi.settings()
                val title = when (parts[0]) {
                    "ai" -> { s.enabled = on; "ИИ" }
                    "night_training" -> { s.trainNightly = on; "обучение по ночам" }
                    else -> return "Такой настройки нет."
                }
                GgtgAi.save()
                step("$title: " + if (on) "включено" else "выключено")
                "Готово: $title " + if (on) "включено." else "выключено."
            }
            "answers_feed" -> {
                step("смотрит ленту ответов")
                val open = GgtgAiInbox.all().filter { it.status == GgtgAiInbox.Status.Pending || it.status == GgtgAiInbox.Status.Hot }
                if (open.isEmpty()) "Сейчас никто не ждёт ответа." else
                    "Ждут ответа: ${open.count { it.status == GgtgAiInbox.Status.Pending }}, горячих: ${open.count { it.status == GgtgAiInbox.Status.Hot }}\n" +
                        open.joinToString("\n") {
                            (if (it.status == GgtgAiInbox.Status.Hot) "горячий лид " else "") + "@${it.username} (${GgtgAiInbox.stageTitle(it.stage)}): «" +
                                it.theirText.take(200) + "»" + (if (it.draft.isEmpty()) "" else "\n   ИИ предлагает: " + it.draft.take(300))
                        }.take(6000)
            }
            "what_i_learned" -> {
                step("смотрит, что выучено")
                "Профиль:\n" + GgtgAiProfile.block(3000) + "\n\nОбучение на чатах: " + GgtgAiTrain.summary() + "\n\nЛучшие ответы:\n" +
                    GgtgAiPlaybook.entries().sortedByDescending { GgtgAiPlaybook.weight(it) }.take(15).joinToString("\n\n") { GgtgAiPlaybook.chunk(it) }.ifEmpty { "пока нет" }.take(5000)
            }
            "mailing_stats" -> {
                step("смотрит цифры рассылок")
                val index = GgtgDb.loadIndex()
                "Баз: ${index.size}, людей в них: ${index.sumOf { it.count }}. Ответили на рассылку: ${GgtgDb.countReplies()}."
            }
            "remember" -> {
                val raw = args.optString("fact").trim().take(300)
                if (raw.isEmpty()) return "Нечего запоминать."
                var section = GgtgAiProfile.Section.Business
                var fact = raw
                val colon = raw.indexOf(':')
                if (colon > 0) GgtgAiProfile.sectionByTag(raw.substring(0, colon))?.let { section = it; fact = raw.substring(colon + 1).trim() }
                if (!GgtgAiProfile.add(section, fact)) return "Это уже записано в профиле."
                step("запомнил: ${section.tag}: $fact")
                "Запомнено."
            }
            "forget" -> {
                val needle = args.optString("text").trim()
                if (needle.length < 3) return "Слишком коротко: назови слова из факта."
                val removed = ArrayList<String>()
                for (section in GgtgAiProfile.Section.values()) {
                    for (e in GgtgAiProfile.entries(section)) {
                        if (e.text.contains(needle, ignoreCase = true)) { removed.add(e.text); GgtgAiProfile.remove(section, e.text) }
                    }
                }
                if (removed.isEmpty()) { step("в профиле нет «$needle»"); return "Такого факта в профиле нет." }
                step("забыл: ${removed.size} факт(ов)")
                "Забыл: " + removed.joinToString("; ")
            }
            "add_material" -> {
                val source = args.optString("source").trim()
                if (source.isEmpty()) return "Нечего добавлять."
                val link = Regex("^(https?://\\S+|www\\.\\S+|\\S+\\.[a-z]{2,}/\\S*)$").matches(source)
                val title = source.take(40).replace(Regex("\\s+"), " ").let { if (source.length > 40) "$it…" else it }
                step(if (link) "читает ссылку: ${source.take(60)}" else "добавляет текст: $title")
                val error = if (link) GgtgAiKnowledge.addUrl(source) { } else GgtgAiKnowledge.addText(title, source) { }
                if (error == null) { step("добавил материал: $title"); "Добавлено в материалы: $title" }
                else { step("материал не добавлен: $error"); "Не вышло: $error" }
            }
            "list_materials" -> {
                val all = GgtgAiKnowledge.list()
                step("посмотрел материалы: ${all.size}")
                if (all.isEmpty()) "Материалов пока нет." else all.joinToString("\n") { "• ${it.name} (${GgtgAiKnowledge.kindTitle(it.kind)}, кусков: ${it.chunks.size})" }
            }
            "training" -> when (args.optString("action").trim().lowercase()) {
                "start" -> GgtgAiTrain.start().let { err ->
                    if (err == null) { step("запустил обучение на чатах"); "Обучение запущено." } else { step("обучение не запущено: $err"); err }
                }
                "stop" -> { GgtgAiTrain.stopRun(); step("остановил обучение"); "Остановлено." }
                else -> { step("посмотрел, как идёт обучение"); GgtgAiTrain.summary() }
            }
            else -> "Такого инструмента нет."
        }
    }

    /** One user turn: the answer of the model, with the tools it called on the way (reported to [step]). */
    suspend fun ask(history: List<Turn>, interview: Boolean, step: (String) -> Unit): Pair<String?, String?> {
        val messages = JSONArray().put(JSONObject().put("role", "system").put("content", systemPrompt(interview)))
        for (t in history) messages.put(JSONObject().put("role", t.role).put("content", t.text))
        for (round in 0 until 8) {
            val r = GgtgAi.askWithTools(messages, tools)
            if (r.error != null) return null to r.error
            val message = r.message ?: return null to "модель вернула пустой ответ"
            val calls = message.optJSONArray("tool_calls")
            if (calls == null || calls.length() == 0) return r.text.replace('—', '–') to null
            messages.put(JSONObject().put("role", "assistant").put("content", message.optString("content")).put("tool_calls", calls))
            for (i in 0 until calls.length()) {
                val call = calls.getJSONObject(i)
                val function = call.optJSONObject("function") ?: continue
                val args = try { JSONObject(function.optString("arguments").ifEmpty { "{}" }) } catch (t: Throwable) { JSONObject() }
                val result = runTool(function.optString("name"), args, step)
                messages.put(JSONObject().put("role", "tool").put("tool_call_id", call.optString("id")).put("content", result))
            }
        }
        return null to "агент слишком долго думал: спросите короче"
    }
}
