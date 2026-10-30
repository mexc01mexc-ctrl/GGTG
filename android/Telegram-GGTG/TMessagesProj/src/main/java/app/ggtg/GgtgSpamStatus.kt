package app.ggtg

import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import org.json.JSONObject
import org.telegram.messenger.ApplicationLoader
import org.telegram.messenger.MessagesController
import org.telegram.messenger.UserConfig
import org.telegram.tgnet.TLRPC
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.TimeZone
import java.util.concurrent.CopyOnWriteArrayList

// ng: port of the desktop's per-account SpamBot verdict (data/account_spam_status.h and
// CheckAccountSpamStatus in settings/sms_campaign.cpp): "/start" goes to @SpamBot from the account,
// its reply is read into Clear / Temporary(until) / Permanent and kept per user id in
// filesDir/ggtg_bulk_db/account_spam.json. The account manager shows it as each row's status.
object GgtgSpamStatus {

    enum class State { Clear, Temporary, Permanent }
    data class Status(val state: State, val until: Long = 0, val checkedAt: Long = 0)

    private const val REPLY_TIMEOUT_MS = 20_000L
    private const val POLL_MS = 1_500L

    private val lock = Any()
    private var loaded = false
    private val statuses = HashMap<Long, Status>()
    private val checking = HashSet<Long>()
    private val listeners = CopyOnWriteArrayList<() -> Unit>()

    fun addListener(l: () -> Unit) { listeners.add(l) }
    fun removeListener(l: () -> Unit) { listeners.remove(l) }
    private fun fire() { org.telegram.messenger.AndroidUtilities.runOnUIThread { listeners.forEach { runCatching { it() } } } }

    private fun file() = File(File(ApplicationLoader.getFilesDirFixed(), "ggtg_bulk_db").also { it.mkdirs() }, "account_spam.json")

    fun find(userId: Long): Status? = synchronized(lock) { ensureLoaded(); statuses[userId] }
    fun isChecking(userId: Long): Boolean = synchronized(lock) { userId in checking }

    fun isBlocked(userId: Long): Boolean {
        val s = find(userId) ?: return false
        return s.state == State.Permanent || (s.state == State.Temporary && s.until > System.currentTimeMillis() / 1000)
    }

    fun all(): Map<Long, Status> = synchronized(lock) { ensureLoaded(); HashMap(statuses) }

    fun set(userId: Long, status: Status) {
        synchronized(lock) {
            ensureLoaded()
            statuses[userId] = status
            runCatching {
                val root = JSONObject()
                for ((id, s) in statuses) root.put(id.toString(), JSONObject().put("state", s.state.name).put("until", s.until).put("at", s.checkedAt))
                val f = file()
                val tmp = File(f.parentFile, f.name + ".tmp")
                tmp.writeText(root.toString())
                if (!tmp.renameTo(f)) { f.delete(); tmp.renameTo(f) }
            }
        }
        fire()
    }

    fun merge(userId: Long, status: Status): Boolean {
        val local = find(userId)
        if (local == null || status.checkedAt > local.checkedAt) {
            set(userId, status)
            return true
        }
        return false
    }

    private fun ensureLoaded() {
        if (loaded) return
        loaded = true
        runCatching {
            val f = file()
            if (!f.exists()) return
            val root = JSONObject(f.readText())
            for (k in root.keys()) {
                val o = root.getJSONObject(k)
                val state = runCatching { State.valueOf(o.optString("state")) }.getOrNull() ?: continue
                statuses[k.toLong()] = Status(state, o.optLong("until"), o.optLong("at"))
            }
        }
    }

    /** Reads @SpamBot's reply (Russian or English) the same way the desktop does. */
    fun parseReply(raw: String): Status {
        val now = System.currentTimeMillis() / 1000
        val lower = raw.lowercase()
        val clear = listOf("no limits", "free as a bird", "you're free", "you are free", "not limited",
            "нет ограничени", "ограничения не применяются", "свободны как птица", "свободен от", "не ограничен")
        if (clear.any { lower.contains(it) }) return Status(State.Clear, checkedAt = now)
        Regex("""(\d{4}-\d{2}-\d{2})[ T](\d{1,2}:\d{2})""").find(raw)?.let { m ->
            runCatching {
                val f = SimpleDateFormat("yyyy-MM-dd H:mm", Locale.US).apply { timeZone = TimeZone.getTimeZone("UTC") }
                return Status(State.Temporary, f.parse(m.groupValues[1] + " " + m.groupValues[2])!!.time / 1000, now)
            }
        }
        Regex("""(\d{1,2})\s+([A-Za-z]{3,9})\s+(\d{4})[^\d]{0,10}(\d{1,2}:\d{2})""").find(raw)?.let { m ->
            val composed = "${m.groupValues[1]} ${m.groupValues[2]} ${m.groupValues[3]} ${m.groupValues[4]}"
            for (pattern in listOf("d MMM yyyy H:mm", "d MMMM yyyy H:mm")) {
                val date = runCatching {
                    SimpleDateFormat(pattern, Locale.US).apply { timeZone = TimeZone.getTimeZone("UTC") }.parse(composed)
                }.getOrNull()
                if (date != null) return Status(State.Temporary, date.time / 1000, now)
            }
        }
        return Status(State.Permanent, checkedAt = now)
    }

    /** Asks @SpamBot from [slot]; returns an error text, or null when the verdict was saved. */
    suspend fun check(slot: Int): String? {
        GgtgAccounts.wake(listOf(slot))
        return try { checkAwake(slot) } finally { GgtgAccounts.sleepIdle() }
    }

    private suspend fun checkAwake(slot: Int): String? {
        val uc = UserConfig.getInstance(slot)
        if (!uc.isClientActivated) return "нет активной сессии"
        val userId = uc.clientUserId
        synchronized(lock) { if (!checking.add(userId)) return "проверка уже идёт" }
        fire()
        try {
            val controller = MessagesController.getInstance(slot)
            var bot: TLRPC.User? = controller.getUser("spambot")
            if (bot == null) {
                val pair = GgtgAccounts.request(slot, TLRPC.TL_contacts_resolveUsername().apply { username = "spambot" })
                    ?: return "таймаут"
                pair.second?.let { return "не удалось найти @SpamBot: " + it.text }
                val resp = pair.first as? TLRPC.TL_contacts_resolvedPeer ?: return "не удалось найти @SpamBot"
                controller.putUsers(resp.users, false)
                bot = resp.users.firstOrNull()
            }
            if (bot == null) return "не удалось найти @SpamBot"
            val peer = controller.getInputPeer(bot.id)
            val floor = history(slot, peer, 1).maxOfOrNull { it.id } ?: 0
            val send = TLRPC.TL_messages_sendMessage().apply {
                this.peer = peer
                message = "/start"
                random_id = java.util.Random().nextLong()
            }
            var sent = GgtgAccounts.request(slot, send) ?: return "таймаут при отправке /start"
            // The account once blocked the bot: unblock it (what "Restart bot" does) and retry.
            if (sent.second?.text?.contains("BLOCKED") == true) {
                GgtgAccounts.request(slot, TLRPC.TL_contacts_unblock().apply { id = peer })
                send.random_id = java.util.Random().nextLong()
                sent = GgtgAccounts.request(slot, send) ?: return "таймаут при отправке /start"
            }
            sent.second?.let { return "не удалось отправить /start: " + it.text }
            val deadline = System.currentTimeMillis() + REPLY_TIMEOUT_MS
            while (System.currentTimeMillis() < deadline) {
                delay(POLL_MS)
                val reply = history(slot, peer, 3).firstOrNull { !it.out && it.id > floor && !it.message.isNullOrEmpty() }
                if (reply != null) {
                    set(userId, parseReply(reply.message))
                    return null
                }
            }
            return "@SpamBot не ответил"
        } finally {
            synchronized(lock) { checking.remove(userId) }
            fire()
        }
    }

    private suspend fun history(slot: Int, peer: TLRPC.InputPeer, limit: Int): List<TLRPC.Message> {
        val req = TLRPC.TL_messages_getHistory().apply { this.peer = peer; this.limit = limit }
        val resp = GgtgAccounts.request(slot, req)?.first
        return (resp as? TLRPC.messages_Messages)?.messages ?: emptyList()
    }

    /**
     * Checks [slots] in the background: 2-3 accounts at a time, 3-5 s between the groups - never
     * all in the same instant. Logged to "Процессы". Returns how many were started.
     */
    fun checkMany(slots: List<Int>): Int {
        val live = slots.filter { UserConfig.getInstance(it).isClientActivated }
        if (live.isEmpty()) return 0
        val process = GgtgProcess.start("Проверка SpamBot (${live.size})", GgtgProcess.Kind.Other)
        CoroutineScope(SupervisorJob() + Dispatchers.IO).launch {
            var done = 0
            var blocked = 0
            val jobs = ArrayList<kotlinx.coroutines.Job>()
            var index = 0
            while (index < live.size) {
                val group = live.subList(index, minOf(live.size, index + (2..3).random()))
                index += group.size
                for (slot in group) {
                    jobs += launch {
                        val label = GgtgAccounts.label(slot)
                        val error = runCatching { check(slot) }.getOrElse { it.message ?: "ошибка" }
                        synchronized(jobs) {
                            done++
                            if (error != null) process.log("[$label] ошибка: $error")
                            else {
                                val text = statusText(UserConfig.getInstance(slot).clientUserId)
                                if (isBlocked(UserConfig.getInstance(slot).clientUserId)) blocked++
                                process.log("[$label] $text")
                            }
                            process.progress(done, live.size)
                        }
                    }
                }
                if (index < live.size) delay((3000L..5000L).random())
            }
            jobs.forEach { it.join() }
            process.succeed("Проверено: ${live.size}, со спам-блоком: $blocked")
        }
        return live.size
    }

    /**
     * Auto-appeals to @SpamBot for accounts with spam blocks.
     * Staggered 3-5 seconds between accounts. Logged to "Процессы". Returns count of accounts started.
     */
    fun autoAppealMany(slots: List<Int>): Int {
        val live = slots.filter { UserConfig.getInstance(it).isClientActivated }
        if (live.isEmpty()) return 0
        val process = GgtgProcess.start("Авто-апелляция @SpamBot (${live.size})", GgtgProcess.Kind.Other)
        CoroutineScope(SupervisorJob() + Dispatchers.IO).launch {
            var done = 0
            var appealed = 0
            for ((index, slot) in live.withIndex()) {
                val label = GgtgAccounts.label(slot)
                val err = runCatching { autoAppeal(slot) }.getOrElse { it.message ?: "ошибка" }
                done++
                if (err != null) {
                    process.log("[$label] ошибка: $err")
                } else {
                    appealed++
                    process.log("[$label] апелляция успешно отправлена")
                }
                process.progress(done, live.size)
                if (index < live.size - 1) delay((3000L..5000L).random())
            }
            process.succeed("Завершено. Отправлено апелляций: $appealed из ${live.size}")
        }
        return live.size
    }

    fun statusText(userId: Long): String {
        if (isChecking(userId)) return "Проверяется…"
        val s = find(userId) ?: return "Не проверен"
        return when (s.state) {
            State.Clear -> "Свободен"
            State.Permanent -> "Спам-блок навсегда"
            State.Temporary -> if (s.until > System.currentTimeMillis() / 1000)
                "Спам-блок до " + SimpleDateFormat("dd.MM HH:mm", Locale.getDefault()).format(Date(s.until * 1000))
            else "Свободен"
        }
    }

    /** Traffic-light colour for [statusText]. */
    fun statusColor(userId: Long): Int {
        if (isChecking(userId)) return 0xFFFFC107.toInt()
        val s = find(userId) ?: return 0xFF8E8E93.toInt()
        return when {
            s.state == State.Permanent -> 0xFFFF453A.toInt()
            s.state == State.Temporary && s.until > System.currentTimeMillis() / 1000 -> 0xFFFF9F0A.toInt()
            else -> 0xFF4CD964.toInt()
        }
    }

    data class GgrInfo(val score: Float, val tier: String)

    /** Calculates account trust rating (1.0 to 10.0) from age, spambot status, proxy and activity. */
    fun calculateGgr(slot: Int): GgrInfo {
        val uc = UserConfig.getInstance(slot)
        if (!uc.isClientActivated) return GgrInfo(1.0f, "Не активен")
        val userId = uc.clientUserId
        var score = 1.0f

        // 1. Account age by User ID
        score += when {
            userId < 1_000_000_000L -> 4.0f
            userId < 3_000_000_000L -> 3.5f
            userId < 5_500_000_000L -> 2.8f
            userId < 7_500_000_000L -> 1.8f
            else -> 0.8f
        }

        // 2. Proxy isolation
        if (uc.ncgProxyEnabled && uc.ncgProxyAddress.isNotEmpty()) {
            score += 1.5f
        } else {
            score += 0.4f
        }

        // 3. Activity / Sent today
        val sent: Int = runCatching { GgtgSms.sentToday(slot) }.getOrDefault(0)
        if (sent > 0) {
            score += 1.0f
        } else {
            score += 0.5f
        }

        // 4. SpamBot status
        val spam = find(userId)
        if (spam != null) {
            when (spam.state) {
                State.Clear -> score += 2.5f
                State.Temporary -> score = minOf(score, 2.5f)
                State.Permanent -> score = 1.0f
            }
        } else {
            score += 1.5f
        }

        score = score.coerceIn(1.0f, 10.0f)
        val tier = when {
            score >= 7.5f -> "Высокий"
            score >= 5.0f -> "Средний"
            else -> "Низкий"
        }
        return GgrInfo(score, tier)
    }

    /**
     * Auto-appeals to @SpamBot for accounts with temporary or accidental limits.
     * Follows the dialog sequence: "Это ошибка" -> "Да" -> "Нет, ничего подобного не было" -> appeal text.
     */
    suspend fun autoAppeal(slot: Int): String? {
        val uc = UserConfig.getInstance(slot)
        if (!uc.isClientActivated) return "нет активной сессии"
        GgtgAccounts.wake(listOf(slot))
        return try {
            val controller = MessagesController.getInstance(slot)
            var bot: TLRPC.User? = controller.getUser("spambot")
            if (bot == null) {
                val pair = GgtgAccounts.request(slot, TLRPC.TL_contacts_resolveUsername().apply { username = "spambot" })
                    ?: return "таймаут"
                val resp = pair.first as? TLRPC.TL_contacts_resolvedPeer ?: return "не удалось найти @SpamBot"
                controller.putUsers(resp.users, false)
                bot = resp.users.firstOrNull()
            }
            if (bot == null) return "не удалось найти @SpamBot"
            val peer = controller.getInputPeer(bot.id)

            val sendStart = TLRPC.TL_messages_sendMessage().apply {
                this.peer = peer
                message = "/start"
                random_id = java.util.Random().nextLong()
            }
            GgtgAccounts.request(slot, sendStart) ?: return "таймаут при отправке /start"
            delay(2500)

            val sendStep1 = TLRPC.TL_messages_sendMessage().apply {
                this.peer = peer
                message = "Это ошибка"
                random_id = java.util.Random().nextLong()
            }
            GgtgAccounts.request(slot, sendStep1) ?: return "таймаут при шаге 1"
            delay(2500)

            val sendStep2 = TLRPC.TL_messages_sendMessage().apply {
                this.peer = peer
                message = "Да"
                random_id = java.util.Random().nextLong()
            }
            GgtgAccounts.request(slot, sendStep2) ?: return "таймаут при шаге 2"
            delay(2500)

            val sendStep3 = TLRPC.TL_messages_sendMessage().apply {
                this.peer = peer
                message = "Нет, ничего подобного не было"
                random_id = java.util.Random().nextLong()
            }
            GgtgAccounts.request(slot, sendStep3) ?: return "таймаут при шаге 3"
            delay(2500)

            val appealText = "Здравствуйте! Мой аккаунт был ограничен по ошибке. Я не отправлял спам и не нарушал правил Telegram. Пожалуйста, снимите ограничения. Спасибо!"
            val sendAppeal = TLRPC.TL_messages_sendMessage().apply {
                this.peer = peer
                message = appealText
                random_id = java.util.Random().nextLong()
            }
            val res = GgtgAccounts.request(slot, sendAppeal) ?: return "таймаут при отправке апелляции"
            if (res.second != null) return res.second?.text
            null
        } finally {
            GgtgAccounts.sleepIdle()
        }
    }
}
