package app.ggtg.ai

import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.Intent
import android.os.Build
import androidx.core.app.NotificationCompat
import app.ggtg.GgtgAccounts
import app.ggtg.GgtgDb
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import org.json.JSONArray
import org.json.JSONObject
import org.telegram.messenger.AndroidUtilities
import org.telegram.messenger.ApplicationLoader
import org.telegram.messenger.MessagesController
import org.telegram.messenger.R
import org.telegram.messenger.SendMessagesHelper
import org.telegram.messenger.UserConfig
import org.telegram.messenger.UserObject
import org.telegram.ui.LaunchActivity
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

// ggtg: the feed of answers with approval, "Лента ответов" (core/ggtg_ai_inbox.h). Everyone who answered a
// mailing is an item here, with the stage the AI sees them at (a question, an objection, interest,
// a refusal, ready to buy) and a draft in the user's style. Nothing is sent without the user's
// "Отправить". A person ready to buy is a hot lead: the AI stays silent and calls the user (a
// notification, and a message to Saved Messages - so any device rings). The autopilot answers by
// itself the stages the user trusts. The store is ggtg_bulk_db/ai_inbox.json.
object GgtgAiInbox {

    enum class Status(val id: String) { Pending("pending"), Sent("sent"), Skipped("skipped"), Hot("hot") }

    class Item(
        val key: String,
        val accountUserId: Long,
        var accountName: String,
        val username: String,
        var name: String,
        val dialogId: Long
    ) {
        var lastId = 0L
        var lastAt = 0L
        var theirText = ""
        var stage = ""
        var draft = ""
        var warning = ""
        var status = Status.Pending
        @Volatile var drafting = false
        var updated = 0L
        var sentText = ""
        var sentStage = ""
        var sentTheirs = ""
        var autoAt = 0L
    }

    class Memory {
        val streaks = HashMap<String, Int>()
        val autopilot = ArrayList<String>()
        var lastBrief = ""
        var lastReview = 0L
    }

    const val AUTOPILOT_STREAK = 10
    private const val AUTOPILOT_DELAY_MS = 3 * 60 * 1000L
    private const val MAX_ITEMS = 300

    private val lock = Any()
    private val items = ArrayList<Item>()
    private val memory = Memory()
    private var loaded = false
    @Volatile var version = 0
        private set
    private val listeners = java.util.concurrent.CopyOnWriteArrayList<() -> Unit>()
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)

    fun addListener(l: () -> Unit) { listeners.add(l) }
    fun removeListener(l: () -> Unit) { listeners.remove(l) }
    private fun changed() { version++; listeners.forEach { runCatching { AndroidUtilities.runOnUIThread { it() } } } }

    private fun file() = File(GgtgAi.dir(), "ai_inbox.json")

    private fun ensureLoaded() {
        if (loaded) return
        loaded = true
        GgtgAi.readText(file())?.let { text ->
            try {
                val root = JSONObject(text)
                root.optJSONObject("memory")?.let { m ->
                    m.optJSONObject("streaks")?.let { s -> for (k in s.keys()) memory.streaks[k] = s.optInt(k) }
                    m.optJSONArray("autopilot")?.let { a -> for (i in 0 until a.length()) memory.autopilot.add(a.getString(i)) }
                    memory.lastBrief = m.optString("lastBrief")
                    memory.lastReview = m.optLong("lastReview")
                }
                val arr = root.optJSONArray("items") ?: JSONArray()
                for (i in 0 until arr.length()) {
                    val o = arr.getJSONObject(i)
                    val it = Item(o.optString("key"), o.optString("accountUserId").toLongOrNull() ?: 0, o.optString("accountName"),
                        o.optString("username"), o.optString("name"), o.optString("dialogId").toLongOrNull() ?: 0)
                    it.lastId = o.optLong("lastId"); it.lastAt = o.optLong("lastAt"); it.theirText = o.optString("theirText")
                    it.stage = o.optString("stage"); it.draft = o.optString("draft"); it.warning = o.optString("warning")
                    it.status = Status.values().firstOrNull { s -> s.id == o.optString("status") } ?: Status.Pending
                    it.updated = o.optLong("updated"); it.sentText = o.optString("sentText"); it.sentStage = o.optString("sentStage")
                    it.sentTheirs = o.optString("sentTheirs"); it.autoAt = o.optLong("autoAt")
                    if (it.key.isNotEmpty()) items.add(it)
                }
            } catch (t: Throwable) { }
        }
    }

    private fun save() {
        if (items.size > MAX_ITEMS) {
            items.sortWith(compareBy({ !(it.status == Status.Pending || it.status == Status.Hot) }, { -it.updated }))
            while (items.size > MAX_ITEMS) items.removeAt(items.size - 1)
        }
        val arr = JSONArray()
        for (it in items) arr.put(JSONObject().put("key", it.key).put("accountUserId", it.accountUserId.toString())
            .put("accountName", it.accountName).put("username", it.username).put("name", it.name).put("dialogId", it.dialogId.toString())
            .put("lastId", it.lastId).put("lastAt", it.lastAt).put("theirText", it.theirText).put("stage", it.stage)
            .put("draft", it.draft).put("warning", it.warning).put("status", it.status.id).put("updated", it.updated)
            .put("sentText", it.sentText).put("sentStage", it.sentStage).put("sentTheirs", it.sentTheirs).put("autoAt", it.autoAt))
        val m = JSONObject()
        m.put("streaks", JSONObject(memory.streaks as Map<*, *>))
        m.put("autopilot", JSONArray(memory.autopilot))
        m.put("lastBrief", memory.lastBrief).put("lastReview", memory.lastReview)
        GgtgAi.writeText(file(), JSONObject().put("items", arr).put("memory", m).toString())
    }

    private fun persist() { synchronized(lock) { save() }; changed() }

    fun all(): List<Item> = synchronized(lock) { ensureLoaded(); items.toList() }

    fun find(key: String): Item? = synchronized(lock) { ensureLoaded(); items.firstOrNull { it.key == key } }

    fun mem(): Memory = synchronized(lock) { ensureLoaded(); memory }

    fun pendingCount() = all().count { it.status == Status.Pending }
    fun hotCount() = all().count { it.status == Status.Hot }

    fun stageTitle(stage: String) = when (stage) {
        "question" -> "вопрос"; "objection" -> "возражение"; "interest" -> "интерес"; "refusal" -> "отказ"
        "hot" -> "готов купить"; "other" -> "другое"; else -> "определяю..."
    }

    fun stageFromWord(wordRaw: String): String {
        val w = wordRaw.lowercase()
        return when {
            w.contains("готов") || w.contains("купит") || w.contains("горяч") -> "hot"
            w.contains("отказ") || w.contains("негатив") -> "refusal"
            w.contains("возраж") || w.contains("сомнен") -> "objection"
            w.contains("вопрос") -> "question"
            w.contains("интерес") -> "interest"
            else -> "other"
        }
    }

    /** The offer of the autopilot: a stage whose drafts went out unchanged [AUTOPILOT_STREAK] times in a row. */
    fun autopilotOffer(): String? {
        val m = mem()
        return m.streaks.entries.firstOrNull { it.value >= AUTOPILOT_STREAK && it.key != "hot" && it.key != "refusal" && it.key !in m.autopilot }?.key
    }

    fun setAutopilot(stage: String, on: Boolean) {
        synchronized(lock) {
            ensureLoaded()
            memory.autopilot.remove(stage)
            if (on) memory.autopilot.add(stage) else memory.streaks[stage] = 0
        }
        persist()
    }

    fun declineAutopilot(stage: String) { synchronized(lock) { ensureLoaded(); memory.streaks[stage] = 0 }; persist() }

    fun markReviewed() { synchronized(lock) { ensureLoaded(); memory.lastReview = System.currentTimeMillis() / 1000 }; persist() }

    // ---- who writes ----

    /** A message of a person the mailings wrote to: an item of the feed (and a draft when the user wants). */
    fun onIncoming(slot: Int, dialogId: Long, username: String, name: String, text: String, messageId: Long, date: Long) {
        val uc = UserConfig.getInstance(slot)
        val key = "${uc.clientUserId}:${username.lowercase()}"
        var created = false
        synchronized(lock) {
            ensureLoaded()
            val now = System.currentTimeMillis() / 1000
            var item = items.firstOrNull { it.key == key }
            if (item == null) {
                item = Item(key, uc.clientUserId, GgtgAccounts.label(slot), username.lowercase(), name, dialogId)
                items.add(item)
            } else if (item.lastId >= messageId && messageId > 0) {
                return
            }
            item.lastId = messageId; item.lastAt = date; item.theirText = text.take(600); item.updated = now
            item.autoAt = 0
            if (item.status != Status.Hot) {
                item.status = Status.Pending; item.stage = ""; item.draft = ""; item.warning = ""
            }
            created = true
        }
        persist()
        if (created && GgtgAi.settings().inboxAuto && GgtgAi.ready()) scope.launch { delay(4_000); draftNext() }
    }

    /** Looks for the people who wait for an answer in the chats of every account; returns how many new. */
    fun scan(): Int {
        var added = 0
        for (slot in 0 until UserConfig.MAX_ACCOUNT_COUNT) {
            val uc = UserConfig.getInstance(slot)
            if (!uc.isClientActivated) continue
            val controller = MessagesController.getInstance(slot)
            for (d in ArrayList(controller.getAllDialogs())) {
                val dialogId = d.id
                if (dialogId <= 0) continue
                val user = controller.getUser(dialogId) ?: continue
                if (user.bot || user.self) continue
                val username = UserObject.getPublicUsername(user)
                if (username.isNullOrEmpty()) continue
                if (GgtgDb.findSmsRecord(username)?.status != GgtgDb.SmsStatus.Sent) continue
                val last = controller.dialogMessage.get(dialogId)?.firstOrNull() ?: continue
                val owner = last.messageOwner ?: continue
                if (owner.out || owner.action != null) continue
                val key = "${uc.clientUserId}:${username.lowercase()}"
                if (find(key)?.let { it.lastId >= owner.id } == true) continue
                val text = owner.message.orEmpty().ifEmpty { "[вложение или стикер]" }
                onIncomingQuiet(slot, dialogId, username, UserObject.getUserName(user), text, owner.id.toLong(), owner.date.toLong())
                added++
            }
        }
        if (added > 0) changed()
        return added
    }

    private fun onIncomingQuiet(slot: Int, dialogId: Long, username: String, name: String, text: String, id: Long, date: Long) {
        val uc = UserConfig.getInstance(slot)
        val key = "${uc.clientUserId}:${username.lowercase()}"
        synchronized(lock) {
            ensureLoaded()
            var item = items.firstOrNull { it.key == key }
            if (item == null) { item = Item(key, uc.clientUserId, GgtgAccounts.label(slot), username.lowercase(), name, dialogId); items.add(item) }
            item.lastId = id; item.lastAt = date; item.theirText = text.take(600); item.updated = System.currentTimeMillis() / 1000
            item.autoAt = 0
            if (item.status != Status.Hot) { item.status = Status.Pending; item.stage = ""; item.draft = ""; item.warning = "" }
            save()
        }
    }

    // ---- the draft ----

    private fun systemPrompt(lines: List<GgtgAiText.Line>, person: String, query: String): String {
        val s = GgtgAi.settings()
        val sb = StringBuilder()
        sb.append("О пользователе, его цели и манере общения (написано им самим):\n")
            .append(s.chatPrompt.trim().ifEmpty { "Помоги вести вежливую, живую переписку. Пиши коротко и естественно, как человек в мессенджере." }.take(2000))
            .append("\n\n")
        sb.append("Ты помощник в мессенджере: читаешь переписку и пишешь ответы ОТ ПЕРВОГО ЛИЦА - от имени пользователя (в переписке он ")
            .append("подписан \"Я\"), его словами, как будто их пишет он сам. Ведёшь разговор к цели из его описания: каждый ответ должен ")
            .append("приближать к ней, но естественно, без давления и навязчивости. Пиши коротко, по делу, как в мессенджере, на языке собеседника. ")
            .append("Не выдумывай фактов, цен, ссылок и обещаний, которых нет в описании пользователя, профиле, материалах и переписке; если ")
            .append("чего-то не знаешь - скажи, что уточнишь. Если собеседник спрашивает, откуда у вас его контакт, ответь спокойно и честно: ")
            .append("нашли в открытых группах. Никогда не используй длинное тире (—): вместо него запятая, точка или короткое тире (–). ")
            .append("Не оставляй в тексте заготовок в квадратных скобках вроде [Имя].\n")
        sb.append(GgtgAiText.languageNote(lines, person))
        GgtgAiProfile.block(3000, query).takeIf { it.isNotEmpty() }?.let {
            sb.append("\nЧто ты уже знаешь о пользователе, его деле и о том, что у него работает (это важно, соблюдай):\n").append(it)
        }
        GgtgAiProfile.examplesBlock().takeIf { it.isNotEmpty() }?.let {
            sb.append("\nТак пользователь на самом деле пишет (его недавние сообщения; подражай этому стилю, длине и тону):\n").append(it)
        }
        GgtgAiKnowledge.block(3500, query).takeIf { it.isNotEmpty() }?.let {
            sb.append("\nИз материалов пользователя (презентации, прайсы, описания, сайты, видео, документы; выбрано то, что подходит к ")
                .append("разговору): опирайся на это как на факты, ничего не выдумывай сверх написанного; упоминай, только когда это к месту:\n").append(it).append('\n')
        }
        sb.append("\nФормат ответа (строго):\nЭТАП: <вопрос | возражение | интерес | отказ | готов купить | другое> - на каком этапе собеседник по его ")
            .append("последнему сообщению.\nОТВЕТ: <текст сообщения для отправки, только он, без пояснений и кавычек>\n")
            .append("Этап «готов купить»: человек сам хочет купить, оплатить, записаться, просит реквизиты, ссылку на оплату или цену, чтобы ")
            .append("заплатить сейчас. Для этапов «готов купить» и «отказ» в ОТВЕТ напиши одно слово: НЕТ (отвечать за пользователя нельзя, он ")
            .append("ответит сам или не будет отвечать).")
        return sb.toString()
    }

    class Parsed(val stage: String, val text: String)

    fun parseDraft(result: String): Parsed {
        val stage = Regex("(?:^|\\n)\\s*\\**ЭТАП\\**\\s*[:：]\\s*([^\\n]+)").find(result)?.let { stageFromWord(it.groupValues[1]) }
        var text = result.trim()
        val answer = Regex("(?:^|\\n)\\s*\\**ОТВЕТ\\**\\s*[:：]\\s*(.*)$", RegexOption.DOT_MATCHES_ALL).find(result)
        if (answer != null) text = answer.groupValues[1].trim() else if (stage != null) text = ""
        if (text.length > 2 && ((text.startsWith("\"") && text.endsWith("\"")) || (text.startsWith("«") && text.endsWith("»")))) {
            text = text.substring(1, text.length - 1).trim()
        }
        if (text.length < 8 && text.uppercase().startsWith("НЕТ")) text = ""
        return Parsed(stage ?: "other", text)
    }

    /** Reads the conversation and prepares the reply for item [key]; with [force] a reply is written even
     *  for a refusal. Returns an error text, null when done. */
    suspend fun draft(key: String, force: Boolean = false): String? {
        val item = find(key) ?: return null
        if (item.drafting || (item.status != Status.Pending && item.status != Status.Hot)) return null
        val slot = GgtgAiText.slotOf(item.accountUserId)
        if (slot < 0) return "аккаунт недоступен"
        item.drafting = true
        changed()
        try {
            val lines = GgtgAiText.history(slot, item.dialogId, 25) ?: return "чат не прочитался"
            if (lines.isEmpty()) return "в чате нет сообщений"
            val me = UserConfig.getInstance(slot).currentUser
            val myName = me?.first_name?.trim().orEmpty().ifEmpty { me?.let { UserObject.getUserName(it) }.orEmpty() }
            val person = item.name.ifEmpty { item.username }
            val sb = StringBuilder()
            sb.append("Собеседник: ").append(person).append('\n').append("Username: @").append(item.username).append('\n')
            if (myName.isNotEmpty()) sb.append("Моё имя (имя аккаунта пользователя): ").append(myName).append('\n')
            GgtgDb.findReply(item.username)?.let { sb.append("Что известно о собеседнике: ответил на рассылку: «").append(it.text.take(160)).append("»\n") }
            sb.append("\nПереписка (старые сверху):\n").append(GgtgAiText.timeContext(lines)).append('\n').append(GgtgAiText.transcript(lines))
            val user = sb.toString()
            val query = user.takeLast(800)
            GgtgAiKnowledge.warm(query)
            var system = systemPrompt(lines, person, query)
            if (force) system += " Пользователь попросил подготовить ответ в любом случае, кроме этапа «готов купить»: для отказа напиши вежливый короткий ответ без давления."
            val r = GgtgAi.ask(system, user, 0.6, 600, hard = true)
            if (r.error != null) return r.error
            val parsed = parseDraft(r.text)
            synchronized(lock) {
                item.stage = parsed.stage
                item.warning = ""
                item.updated = System.currentTimeMillis() / 1000
            }
            // The user's answer before this message worked if the person went on (a question, interest, ready to buy), and did not if they refused.
            if (parsed.stage == "refusal") item.sentText = "" else if (parsed.stage != "hot") recordWin(item, 1)
            if (parsed.stage == "hot") {
                val already = item.status == Status.Hot
                item.status = Status.Hot
                item.draft = ""
                item.autoAt = 0
                recordWin(item, 3)
                if (!already) callUser(item)
            } else {
                item.status = Status.Pending
                item.draft = if (parsed.stage == "refusal" && !force) "" else parsed.text
                if (item.draft.isNotEmpty()) {
                    val invented = GgtgAiText.ungroundedFacts(item.draft, user + GgtgAiProfile.block(4000) + GgtgAiKnowledge.allText())
                    if (invented.isNotEmpty()) item.warning = "Проверьте: этого нет в вашем профиле, материалах и переписке: " + invented.joinToString(", ")
                }
                item.autoAt = if (item.draft.isNotEmpty() && item.warning.isEmpty() && parsed.stage in mem().autopilot)
                    System.currentTimeMillis() + AUTOPILOT_DELAY_MS else 0
            }
            persist()
            return null
        } finally {
            item.drafting = false
            changed()
        }
    }

    private suspend fun recordWin(item: Item, wins: Int) {
        if (item.sentText.isEmpty()) return
        val what = GgtgAi.plainMarks(GgtgAi.mask(item.sentTheirs).text).take(220)
        val kind = when (item.sentStage) { "objection" -> "objection"; "question" -> "question"; else -> "message" }
        val answer = item.sentText
        item.sentText = ""
        if (what.length >= 6) GgtgAiPlaybook.addWin(kind, what, answer, wins)
    }

    @Volatile private var busy = false

    /** Drafts for the ones that have no stage yet (at most [limit] at a time, so that the free limits are not drained). */
    suspend fun draftNext(limit: Int = 12): String? {
        if (busy) return null
        busy = true
        try {
            var n = 0
            for (item in all()) {
                if (n >= limit) break
                if (item.status == Status.Pending && item.stage.isEmpty() && !item.drafting) {
                    val error = draft(item.key)
                    if (error != null && (error.contains("отдыхают") || error.contains("не ответила") || error.contains("нет ни одного ключа") || error.contains("выключен"))) return error
                    n++
                }
            }
            return null
        } finally { busy = false }
    }

    /** Scans, then drafts. Returns an error text, null when it went well. */
    suspend fun refresh(): String? {
        scan()
        return draftNext()
    }

    // ---- what the user does ----

    /** After what the program did in a chat the chat is marked "unread" again (sending reads it), so the
     *  person is not lost in the account's own list of chats. */
    private fun keepUnread(slot: Int, dialogId: Long, force: Boolean) {
        if (!GgtgAi.settings().keepUnread) return
        AndroidUtilities.runOnUIThread {
            val dialog = org.telegram.messenger.MessagesController.getInstance(slot).dialogs_dict.get(dialogId)
            if (!force && (dialog == null || (dialog.unread_count <= 0 && !dialog.unread_mark))) return@runOnUIThread
            AndroidUtilities.runOnUIThread({ org.telegram.messenger.MessagesController.getInstance(slot).markDialogAsUnread(dialogId, null, 0L) }, 1500)
        }
    }

    private fun sendTo(slot: Int, dialogId: Long, text: String) {
        keepUnread(slot, dialogId, false)
        AndroidUtilities.runOnUIThread {
            SendMessagesHelper.getInstance(slot).sendMessage(SendMessagesHelper.SendMessageParams.of(text, dialogId))
        }
    }

    /** Puts [textRaw] into the chat from the account that wrote to the person; the user's change of a
     *  draft ([edited]) is a lesson for the AI. Returns an error text, null when sent. */
    fun send(key: String, textRaw: String, edited: Boolean): String? {
        val text = textRaw.trim()
        val item = find(key) ?: return "запись не найдена"
        if (text.isEmpty()) return "ответ пустой"
        val slot = GgtgAiText.slotOf(item.accountUserId)
        if (slot < 0) return "аккаунт недоступен"
        sendTo(slot, item.dialogId, text)
        if (edited && item.draft.isNotEmpty()) {
            GgtgAiLearn.recordEdit(item.draft, text)
            GgtgAiProfile.addExample(text)
            scope.launch { GgtgAiLearn.maybeLearnFromEdits() }
        }
        synchronized(lock) {
            val streak = mem().streaks[item.stage] ?: 0
            memory.streaks[item.stage] = if (edited) 0 else streak + 1
            item.sentText = text; item.sentStage = item.stage; item.sentTheirs = item.theirText
            item.autoAt = 0; item.status = Status.Sent; item.updated = System.currentTimeMillis() / 1000
        }
        persist()
        return null
    }

    fun skip(key: String) {
        find(key)?.let {
            it.status = Status.Skipped; it.autoAt = 0; it.updated = System.currentTimeMillis() / 1000; persist()
            // the chat stays marked "unread" in its account: a skipped person can be found there
            val slot = GgtgAiText.slotOf(it.accountUserId)
            if (slot >= 0) keepUnread(slot, it.dialogId, true)
        }
    }

    /** A skipped person comes back to the list of those waiting for an answer. */
    fun restore(key: String) { find(key)?.let { it.status = Status.Pending; it.updated = System.currentTimeMillis() / 1000; persist() } }

    /** A hot lead goes back to the AI: it reads the person's last message anew. */
    fun giveBack(key: String) { find(key)?.let { it.status = Status.Pending; it.stage = ""; it.draft = ""; persist() } }

    fun cancelAuto(key: String) {
        find(key)?.let { synchronized(lock) { it.autoAt = 0; memory.streaks[it.stage] = 0 }; persist() }
    }

    /** The drafts the autopilot planned go out when their time comes. */
    fun sendDue() {
        val now = System.currentTimeMillis()
        for (item in all()) {
            if (item.autoAt in 1..now && item.status == Status.Pending && item.draft.isNotEmpty()) {
                if (send(item.key, item.draft, false) != null) item.autoAt = 0
            }
        }
    }

    // ---- a lead turned hot: the user is called everywhere at once ----

    private fun callUser(item: Item) {
        val who = "@" + item.username + (if (item.name.isEmpty()) "" else " (${item.name})")
        try {
            val context = ApplicationLoader.applicationContext
            val manager = context.getSystemService(NotificationManager::class.java)
            if (Build.VERSION.SDK_INT >= 26) manager.createNotificationChannel(
                NotificationChannel("ggtg_hot", "Горячие лиды GGTG", NotificationManager.IMPORTANCE_HIGH))
            val intent = Intent(context, LaunchActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_CLEAR_TOP)
            val pending = PendingIntent.getActivity(context, 0, intent, PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT)
            val n = NotificationCompat.Builder(context, "ggtg_hot")
                .setSmallIcon(R.drawable.notification)
                .setContentTitle("Горячий лид")
                .setContentText("$who готов купить: " + item.theirText.take(100))
                .setStyle(NotificationCompat.BigTextStyle().bigText("$who готов купить:\n" + item.theirText.take(300)))
                .setPriority(NotificationCompat.PRIORITY_HIGH)
                .setAutoCancel(true)
                .setContentIntent(pending)
                .build()
            manager.notify(("hot" + item.key).hashCode(), n)
        } catch (t: Throwable) { }
        val slot = GgtgAiText.slotOf(item.accountUserId).let { if (it >= 0) it else UserConfig.selectedAccount }
        sendTo(slot, UserConfig.getInstance(slot).clientUserId,
            "Горячий лид (GGTG)\n$who написал аккаунту ${item.accountName}:\n«${item.theirText.take(400)}»\n\nИИ ему не отвечает: ответьте сами, пока горячо.")
    }

    // ---- the morning brief ----

    fun briefText(): String {
        val hot = all().filter { it.status == Status.Hot }
        val pending = all().filter { it.status == Status.Pending }
        val sb = StringBuilder("Доброе утро! Сводка GGTG на ").append(SimpleDateFormat("dd.MM", Locale.getDefault()).format(Date())).append("\n\n")
        if (hot.isNotEmpty()) sb.append("Горячие лиды (ответьте сами):\n").append(hot.take(5).joinToString("\n") { "• @${it.username}: «${it.theirText.take(80)}»" }).append("\n\n")
        if (pending.isEmpty()) sb.append("Ждущих ответа нет.\n")
        else sb.append("Ждут ответа: ${pending.size}\n").append(pending.take(5).joinToString("\n") { "• @${it.username}: «${it.theirText.take(80)}» (${stageTitle(it.stage)})" }).append('\n')
        autopilotOffer()?.let { sb.append("\nНа «${stageTitle(it)}» вы $AUTOPILOT_STREAK раз подряд отправили ответ ИИ без правок: можно включить автопилот в «Ленте ответов».\n") }
        val learned = GgtgAiPlaybook.since(mem().lastReview).size + GgtgAiProfile.newSince(mem().lastReview).size
        if (learned > 0) sb.append("\nИИ выучил новое: $learned. Проверьте в разделе «ИИ» → «2. О бизнесе и цель» → «Проверить, что ИИ выучил».\n")
        sb.append("\n").append(GgtgAi.statusText())
        return sb.toString()
    }

    // ---- what works by itself while the app is open ----

    @Volatile private var started = false

    /** Catches new answers as they come (see GgtgManualSends), sends what the autopilot planned, the morning brief. */
    fun startBackground() {
        // the feed of answers and the morning brief are gone (the user did not want them)
        return
        @Suppress("UNREACHABLE_CODE")
        if (started) return
        started = true
        scope.launch {
            var minute = 0
            while (true) {
                delay(10_000)
                try {
                    sendDue()
                    minute++
                    if (minute % 6 == 0) {
                        val s = GgtgAi.settings()
                        if (s.enabled && s.inboxAuto && GgtgAi.ready() && minute % 30 == 0) refresh()
                        maybeBrief()
                    }
                } catch (t: Throwable) { }
            }
        }
    }

    private fun maybeBrief() {
        val s = GgtgAi.settings()
        val cal = java.util.Calendar.getInstance()
        val today = SimpleDateFormat("yyyy-MM-dd", Locale.US).format(cal.time)
        if (!s.enabled || !s.briefDaily || cal.get(java.util.Calendar.HOUR_OF_DAY) < 8 || mem().lastBrief == today) return
        synchronized(lock) { mem().lastBrief = today }
        persist()
        val slot = UserConfig.selectedAccount
        if (!UserConfig.getInstance(slot).isClientActivated) return
        sendTo(slot, UserConfig.getInstance(slot).clientUserId, briefText())
    }
}
