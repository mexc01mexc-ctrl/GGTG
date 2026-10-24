package app.ggtg

import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.CopyOnWriteArrayList

// ng: what a mailing's numbers mean, apart from how they are drawn - the same six outcomes, the same
// names and colors as the desktop app (settings/sms_campaign_model.h). One SmsRun lives for one
// campaign and is read by GgtgSmsRunActivity while the lanes of GgtgSms write into it.

enum class SmsOutcome(val title: String, val tile: String, val inline: String, val color: Int) {
    Sent("Отправлено", "Отправлено", "отправлено", 0xFF4CD964.toInt()),
    // Telegram refused this person (PEER_FLOOD) while the account itself is free
    Peer("Отказ Telegram", "Отказ Telegram", "отказ Telegram", 0xFFFF9F0A.toInt()),
    // privacy, blocked us, deleted: nobody can write there
    Closed("Закрыт для ЛС", "Закрыты для ЛС", "закрыты для ЛС", 0xFF8E8E93.toInt()),
    NotFound("Не найден", "Не найдены", "не найдены", 0xFF8E8E93.toInt()),
    // the account itself is under a spam block (@spambot)
    Spam("Спам-блок аккаунта", "Спам-блок акк.", "спам-блок аккаунта", 0xFFFF453A.toInt()),
    // network, timeout, FLOOD_WAIT, a dead session, unknown
    Fail("Сбой", "Сбой", "сбой", 0xFFFF453A.toInt())
}

enum class SmsFilter(val title: String) {
    All("Все"), Sent("Отправлено"), Peer("Отказ Telegram"), Closed("Закрыты"), Problems("Проблемы");

    fun matches(o: SmsOutcome) = this == All || of(o) == this

    companion object {
        fun of(o: SmsOutcome) = when (o) {
            SmsOutcome.Sent -> Sent
            SmsOutcome.Peer -> Peer
            SmsOutcome.Closed -> Closed
            SmsOutcome.NotFound, SmsOutcome.Spam, SmsOutcome.Fail -> Problems
        }
    }
}

enum class SmsAccountState(val title: String) {
    Working("В работе"), Waiting("Ждёт"),
    Done("Отработал"), // did its share: quota, daily limit, recipients ran out
    Retired("Снят"), // taken out: refusals / failures / FLOOD_WAIT in a row
    SpamBlocked("Спам-блок")
}

enum class SmsPhase(val title: String) {
    Running("Идёт рассылка"), Stopped("Остановлено вручную"), Done("Готово"), AccountsOut("Аккаунты закончились")
}

class SmsRecipient(val username: String) {
    @Volatile var account = ""
    @Volatile var reason = ""
    @Volatile var outcome: SmsOutcome? = null
    @Volatile var inProgress = false
}

/** [info] is the phone and @username: it tells apart accounts that share a name. */
class SmsAccount(val slot: Int, val label: String, val info: String) {
    @Volatile var state = SmsAccountState.Waiting
    @Volatile var note = ""
    @Volatile var sent = 0
    @Volatile var refused = 0
    @Volatile var failed = 0
    /** The whole log of the account: what it did, line by line with the time (oldest first). */
    val log = ArrayList<String>()
    @Volatile var current = "" // @username it is working with now

    fun counters(): String {
        val sb = StringBuilder("$sent")
        if (refused > 0) sb.append(" · отказ $refused")
        if (failed > 0) sb.append(" · сбой $failed")
        return sb.toString()
    }
}

/** One line of the recipient list: a recipient, or equal failures of one account in a row folded. */
class SmsRow(val indices: List<Int>, val outcome: SmsOutcome?, val account: String, val inProgress: Boolean)

class SmsRun(val usernames: List<String>, val accounts: List<SmsAccount>, val lanes: Int, val goal: Int) {

    val recipients = usernames.map { SmsRecipient(it) }
    private val indexOf = HashMap<String, Int>().also { m -> usernames.forEachIndexed { i, n -> m[n.lowercase()] = i } }
    private val accountOf = accounts.associateBy { it.slot }
    val counts = IntArray(SmsOutcome.values().size)
    // recipients worked on, newest first; the list in the window shows these
    private val order = ArrayList<Int>()
    private val laneLive = Array(lanes) { "" }
    private val laneSlot = ConcurrentHashMap<Int, Int>()
    private val laneLast = LongArray(lanes)
    private val recent = ArrayList<Int>()
    @Volatile var earlier = 0
    @Volatile var unprocessed = 0
    @Volatile var stopped = false
    @Volatile var finished = false
    private val listeners = CopyOnWriteArrayList<() -> Unit>()

    fun addListener(l: () -> Unit) { listeners.add(l) }
    fun removeListener(l: () -> Unit) { listeners.remove(l) }
    private fun changed() { listeners.forEach { runCatching { it() } } }

    val phase: SmsPhase get() = when {
        !finished -> SmsPhase.Running
        stopped -> SmsPhase.Stopped
        unprocessed > 0 -> SmsPhase.AccountsOut
        else -> SmsPhase.Done
    }

    @Synchronized fun processed() = counts.sum()

    private val clock = java.text.SimpleDateFormat("HH:mm:ss", java.util.Locale.getDefault())

    private fun addLog(slot: Int, text: String) {
        val a = accountOf[slot] ?: return
        val line = clock.format(java.util.Date()) + "  " + text
        if (a.log.lastOrNull()?.substringAfter("  ") == text) return
        a.log.add(line)
        if (a.log.size > 800) a.log.subList(0, 200).clear()
    }

    /** A copy of the account's log, newest first, for its window. */
    @Synchronized fun logOf(slot: Int): List<String> = accountOf[slot]?.log?.asReversed()?.toList() ?: emptyList()

    @Synchronized fun indexOf(username: String): Int = indexOf[username.lowercase()] ?: -1

    @Synchronized fun setInProgress(i: Int, slot: Int) {
        val r = recipients[i]
        r.inProgress = true; r.outcome = null; r.account = accountOf[slot]?.label ?: ""
        accountOf[slot]?.current = "@" + r.username
        addLog(slot, "→ взял @" + r.username)
        order.remove(i); order.add(0, i)
        if (order.size > MAX_ORDER) order.removeAt(order.size - 1)
        changed()
    }

    @Synchronized fun settle(i: Int, lane: Int, outcome: SmsOutcome, reason: String, slot: Int) {
        val r = recipients[i]
        r.inProgress = false; r.outcome = outcome; r.reason = reason
        counts[outcome.ordinal]++
        addLog(slot, (if (outcome == SmsOutcome.Sent) "✓ @" else "✗ @") + r.username + " - итог: " + outcome.title +
            (if (reason.isEmpty()) "" else ": $reason"))
        accountOf[slot]?.let { a ->
            r.account = a.label
            when (outcome) {
                SmsOutcome.Sent -> a.sent++
                SmsOutcome.Peer -> a.refused++
                SmsOutcome.Fail -> a.failed++
                else -> {}
            }
        }
        if (lane in laneLast.indices) {
            val now = System.currentTimeMillis()
            if (laneLast[lane] != 0L) {
                recent.add(((now - laneLast[lane]) / 1000).toInt())
                if (recent.size > 20) recent.removeAt(0)
            }
            laneLast[lane] = now
        }
        changed()
    }

    /** A recipient handed back to the queue untouched (dead session, flood wait, no answer). */
    @Synchronized fun requeue(i: Int) {
        val r = recipients[i]
        accounts.firstOrNull { it.label == r.account }?.let { addLog(it.slot, "↩ @${r.username} возвращён в очередь (повторит другой аккаунт или позже)") }
        r.inProgress = false; r.outcome = null; r.account = ""
        order.remove(i)
        changed()
    }

    /** @spambot said "blocked" after the refusals were already shown: they were the account's own limit. */
    @Synchronized fun peerToSpam(indices: List<Int>, slot: Int) {
        for (i in indices) {
            val r = recipients[i]
            if (r.outcome == SmsOutcome.Peer) {
                r.outcome = SmsOutcome.Spam
                counts[SmsOutcome.Peer.ordinal]--
                counts[SmsOutcome.Spam.ordinal]++
            }
        }
        accountOf[slot]?.refused = 0
        changed()
    }

    @Synchronized fun setAccount(slot: Int, state: SmsAccountState, note: String) {
        accountOf[slot]?.let { it.state = state; it.note = note }
        addLog(slot, "Состояние: " + state.title + (if (note.isEmpty()) "" else " - $note"))
        changed()
    }

    /** What lane [lane] is doing now ("пауза 42 сек", "ищет @x"); [slot] is the account it works with. */
    @Synchronized fun setLive(lane: Int, slot: Int, text: String) {
        if (lane in laneLive.indices) {
            laneLive[lane] = text
            if (slot >= 0) {
                laneSlot[lane] = slot
                if (text.isNotEmpty()) {
                    val who = accountOf[slot]?.current.orEmpty()
                    addLog(slot, if (text.startsWith("пауза") || who.isEmpty()) text else "$who: $text")
                }
            } else laneSlot.remove(lane)
        }
        changed()
    }

    /** The current step of the lane working this account, for its row. */
    @Synchronized fun liveFor(slot: Int): String {
        if (accountOf[slot]?.state != SmsAccountState.Working) return ""
        for ((lane, s) in laneSlot) if (s == slot) return laneLive.getOrNull(lane) ?: ""
        return ""
    }

    @Synchronized fun finish(stopped: Boolean, unprocessed: Int) {
        this.stopped = stopped; this.unprocessed = unprocessed; finished = true
        for (r in recipients) if (r.inProgress) r.inProgress = false
        changed()
    }

    /** Seconds left, or -1 while there are too few timings to say anything. */
    @Synchronized fun etaSeconds(): Int {
        if (recent.size < 3) return -1
        val remaining = (goal - processed()).coerceAtLeast(0)
        return (recent.average() * remaining / lanes.coerceAtLeast(1) + 0.5).toInt()
    }

    /** Working first, then the ones taken out, then those that did their share, then the ones waiting. */
    fun sortedAccounts(): List<SmsAccount> {
        fun rank(s: SmsAccountState) = when (s) {
            SmsAccountState.Working -> 0
            SmsAccountState.Retired, SmsAccountState.SpamBlocked -> 1
            SmsAccountState.Done -> 2
            SmsAccountState.Waiting -> 3
        }
        return accounts.sortedBy { rank(it.state) }
    }

    fun accountsHeadline(): String {
        fun count(vararg s: SmsAccountState) = accounts.count { it.state in s }
        val working = count(SmsAccountState.Working)
        val done = count(SmsAccountState.Done)
        val out = count(SmsAccountState.Retired, SmsAccountState.SpamBlocked)
        val waiting = count(SmsAccountState.Waiting)
        val parts = arrayListOf("Аккаунты", "${accounts.size} выбрано")
        if (working > 0) parts.add("$working в работе")
        if (done > 0) parts.add("$done " + (if (singular(done)) "отработал" else "отработали"))
        if (out > 0) parts.add("$out " + (if (singular(out)) "снят" else "сняты"))
        if (waiting > 0) parts.add("$waiting " + (if (singular(waiting)) "ждёт" else "ждут"))
        return parts.joinToString(" · ")
    }

    @Synchronized fun finishLine(): String {
        val parts = arrayListOf("Отправлено ${counts[0]}")
        for (o in SmsOutcome.values()) if (o != SmsOutcome.Sent && counts[o.ordinal] > 0) parts.add("${o.inline} ${counts[o.ordinal]}")
        if (unprocessed > 0) parts.add("ждут следующей рассылки $unprocessed (остались в базе)")
        return parts.joinToString(" · ")
    }

    /**
     * The list in the window. With [SmsFilter.All], runs of at least three settled recipients with one
     * non-Sent outcome and one account become a single row; a filter shows each recipient on its own
     * line. At most [limit] rows.
     */
    @Synchronized fun rows(filter: SmsFilter, limit: Int = 200): List<SmsRow> {
        val rows = ArrayList<SmsRow>()
        var run = ArrayList<Int>()
        fun single(i: Int) { val r = recipients[i]; rows.add(SmsRow(listOf(i), r.outcome, r.account, false)) }
        fun flush() {
            if (run.size >= FOLD_MIN) {
                val first = recipients[run[0]]
                rows.add(SmsRow(run.toList(), first.outcome, first.account, false))
            } else run.forEach { single(it) }
            run = ArrayList()
        }
        for (i in order) {
            val r = recipients[i]
            if (r.inProgress) {
                flush()
                if (filter == SmsFilter.All) rows.add(SmsRow(listOf(i), null, r.account, true))
                continue
            }
            val o = r.outcome ?: continue
            if (!filter.matches(o)) continue
            val foldable = filter == SmsFilter.All && o != SmsOutcome.Sent
            if (!foldable) { flush(); single(i); continue }
            if (run.isNotEmpty()) {
                val last = recipients[run.last()]
                if (last.outcome != o || last.account != r.account) flush()
            }
            run.add(i)
        }
        flush()
        return if (rows.size > limit) rows.subList(0, limit).toList() else rows
    }

    companion object {
        private const val MAX_ORDER = 1000
        private const val FOLD_MIN = 3

        /** The campaign shown by the "Рассылка" window: the running one, or the last one that ended. */
        @Volatile var current: SmsRun? = null

        fun singular(n: Int) = n % 10 == 1 && n % 100 != 11

        fun durationText(seconds: Int): String {
            if (seconds < 45) return "< 1 мин"
            val minutes = (seconds + 30) / 60
            if (minutes < 60) return "≈ $minutes мин"
            val h = minutes / 60
            val rest = minutes % 60
            return "≈ $h ч" + if (rest > 0) " $rest мин" else ""
        }
    }
}
