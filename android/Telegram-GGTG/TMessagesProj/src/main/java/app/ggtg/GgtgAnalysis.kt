package app.ggtg

import org.telegram.ui.ActionBar.AlertDialog
import android.content.Context
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import android.widget.Toast
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import org.json.JSONObject
import org.telegram.messenger.AndroidUtilities
import org.telegram.messenger.ApplicationLoader
import org.telegram.messenger.MessagesController
import org.telegram.tgnet.ConnectionsManager
import org.telegram.tgnet.TLRPC
import org.telegram.ui.ActionBar.BaseFragment
import java.io.File
import kotlin.random.Random

// ng: "Анализ и чистка базы" (mirrors the desktop app, without the AI advice): every username of a
// base is looked up through Telegram slowly, like a person would, and what it turned out to be is
// kept next to the base - a live person (when last online, Premium, language of the name), a bot,
// a deleted account, a group/channel, or nothing. The report then cleans the base in one tap,
// moves Premium / recently active people into a base of their own, or splits by language.
object GgtgAnalysis {
    enum class State { Ok, NotFound, Deleted, Bot, Chat, Error }

    data class Item(
        val state: State,
        val language: String = "",
        val seen: Int = 0, // 0 unknown, 1 today, 2 this week, 3 this month or an exact older time, 4 legacy, 5 "был(а) давно"
        val premium: Boolean = false
    )

    private const val ERROR_STREAK = 5
    private const val MAX_FLOOD_WAIT = 600
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)

    // ---- storage ----

    private fun path(baseId: Long) =
        File(File(ApplicationLoader.getFilesDirFixed(), "ggtg_bulk_db").also { it.mkdirs() }, "analysis_$baseId.json")

    private fun key(name: String) = name.trim().removePrefix("@").lowercase()

    private fun load(baseId: Long): MutableMap<String, Item> {
        val result = LinkedHashMap<String, Item>()
        try {
            val file = path(baseId)
            if (!file.exists()) return result
            val json = JSONObject(file.readText())
            for (name in json.keys()) {
                val o = json.getJSONObject(name)
                result[name] = Item(
                    State.values().getOrElse(o.optInt("s")) { State.Error },
                    o.optString("l"), o.optInt("seen"), o.optBoolean("p")
                )
            }
        } catch (_: Exception) {
        }
        return result
    }

    private fun save(baseId: Long, map: Map<String, Item>) {
        try {
            val json = JSONObject()
            for ((name, item) in map) {
                json.put(name, JSONObject().put("s", item.state.ordinal).put("l", item.language)
                    .put("seen", item.seen).put("p", item.premium))
            }
            path(baseId).writeText(json.toString())
        } catch (_: Exception) {
        }
    }

    // ---- classifying one user ----

    fun seenBucket(slot: Int, user: TLRPC.User): Int {
        val now = ConnectionsManager.getInstance(slot).currentTime
        return when (val status = user.status) {
            is TLRPC.TL_userStatusOnline -> 1
            is TLRPC.TL_userStatusRecently -> 2
            is TLRPC.TL_userStatusLastWeek -> 2
            is TLRPC.TL_userStatusLastMonth -> 3
            is TLRPC.TL_userStatusEmpty -> 5
            is TLRPC.TL_userStatusOffline -> {
                val age = now - status.expires
                when {
                    age < 86_400 -> 1
                    age < 7 * 86_400 -> 2
                    else -> 3
                }
            }
            else -> 0
        }
    }

    private fun classify(slot: Int, user: TLRPC.User): Item {
        if (user.deleted) return Item(State.Deleted)
        if (user.bot) return Item(State.Bot)
        val code = GgtgNames.detectLanguage((user.first_name ?: "") + " " + (user.last_name ?: ""))
        return Item(State.Ok, if (code == "en") "lat" else code, seenBucket(slot, user), user.premium)
    }

    // ---- the run ----

    private class Run(val map: MutableMap<String, Item>) {
        @Volatile var stop = false
        @Volatile var finished = false
    }

    fun open(fragment: BaseFragment, baseId: Long, title: String, onChanged: () -> Unit) {
        val context = fragment.parentActivity ?: return
        if (load(baseId).isEmpty()) start(fragment, context, baseId, title, onChanged) else report(context, fragment, baseId, title, onChanged)
    }

    private fun start(fragment: BaseFragment, context: Context, baseId: Long, title: String, onChanged: () -> Unit) {
        val map = load(baseId)
        val queue = GgtgDb.loadUsernames(baseId).map { key(it) }.filter { name ->
            val done = map[name]
            done == null || done.state == State.Error
        }
        if (queue.isEmpty()) { report(context, fragment, baseId, title, onChanged); return }
        val slot = fragment.currentAccount
        val run = Run(map)
        val handle = GgtgProcess.start("Проверка базы: $title (${queue.size})", GgtgProcess.Kind.Scrape)
        val note = "Каждое имя проверяется через Telegram не быстрее раза в секунду-две, чтобы аккаунт не получил ограничение. Окно можно скрыть: проверка идёт в фоне и видна в «Процессах»."
        val dialog = AlertDialog.Builder(context)
            .setTitle("Проверка базы: $title")
            .setMessage("Проверено: 0 из ${queue.size}\n\n$note")
            .setNeutralButton("Скрыть", null)
            .setNegativeButton("Остановить и показать итоги") { _, _ -> run.stop = true }
            .create().also { it.setCanCancel(false); it.setCanceledOnTouchOutside(false); it.show() }

        val finish = { reason: String ->
            if (!run.finished) {
                run.finished = true
                save(baseId, run.map)
                val checked = queue.count { run.map.containsKey(it) }
                val summary = "Проверено $checked из ${queue.size}" + if (reason.isEmpty()) "" else ". $reason"
                handle.succeed(summary)
                AndroidUtilities.runOnUIThread {
                    runCatching { dialog.dismiss() }
                    report(context, fragment, baseId, title, onChanged)
                }
            }
        }

        scope.launch {
            var errorStreak = 0
            var i = 0
            while (i < queue.size) {
                if (run.stop) { finish("Остановлено вручную"); return@launch }
                val name = queue[i]
                val found = lookup(slot, name)
                val user = found.user
                if (user == null && !found.chat) {
                    val wait = GgtgSms.floodWaitSeconds(found.error)
                    if (wait > 0) {
                        if (wait > MAX_FLOOD_WAIT) { finish("Telegram просит подождать $wait сек - продолжите позже"); return@launch }
                        handle.log("Пауза от Telegram: $wait сек")
                        delay((wait + 2) * 1000L)
                        continue
                    }
                    val missing = found.error.isEmpty() || found.error == "USERNAME_NOT_OCCUPIED" || found.error == "USERNAME_INVALID"
                    run.map[name] = Item(if (missing) State.NotFound else State.Error)
                    errorStreak = if (missing) 0 else errorStreak + 1
                    if (errorStreak >= ERROR_STREAK) { finish("подряд пять ошибок (${found.error})"); return@launch }
                } else {
                    errorStreak = 0
                    run.map[name] = if (user == null) Item(State.Chat) else classify(slot, user)
                }
                i++
                handle.progress(i, queue.size)
                val shown = i
                AndroidUtilities.runOnUIThread {
                    if (dialog.isShowing) dialog.setMessage("Проверено: $shown из ${queue.size}\n\n$note")
                }
                if (i % 25 == 0) save(baseId, run.map)
                delay(900L + Random.nextLong(900L))
            }
            finish("")
        }
    }

    private class Found(val user: TLRPC.User?, val chat: Boolean, val error: String)

    private suspend fun lookup(slot: Int, username: String): Found {
        val controller = MessagesController.getInstance(slot)
        val req = TLRPC.TL_contacts_resolveUsername().apply { this.username = username }
        val pair = GgtgAccounts.request(slot, req) ?: return Found(null, false, "таймаут поиска")
        val err = pair.second
        if (err != null) return Found(null, false, err.text ?: "")
        val resp = pair.first as? TLRPC.TL_contacts_resolvedPeer ?: return Found(null, false, "")
        controller.putUsers(resp.users, false)
        controller.putChats(resp.chats, false)
        val peer = resp.peer
        if (peer is TLRPC.TL_peerUser) {
            val user = resp.users.firstOrNull { it.id == peer.user_id } ?: resp.users.firstOrNull()
            return Found(user, false, "")
        }
        return Found(null, true, "")
    }

    // ---- the report ----

    private class Counts(val total: Int) {
        var checked = 0; var people = 0; var notFound = 0; var bots = 0; var chats = 0
        var errors = 0; var active = 0; var longAgo = 0; var premium = 0
        val languages = LinkedHashMap<String, Int>()
    }

    private fun count(usernames: List<String>, map: Map<String, Item>): Counts {
        val c = Counts(usernames.size)
        for (raw in usernames) {
            val item = map[key(raw)] ?: continue
            c.checked++
            when (item.state) {
                State.Ok -> {
                    c.people++
                    if (item.seen == 1 || item.seen == 2) c.active++
                    if (item.seen == 5) c.longAgo++
                    if (item.premium) c.premium++
                    c.languages[item.language] = (c.languages[item.language] ?: 0) + 1
                }
                State.NotFound, State.Deleted -> c.notFound++
                State.Bot -> c.bots++
                State.Chat -> c.chats++
                State.Error -> c.errors++
            }
        }
        return c
    }

    private fun report(context: Context, fragment: BaseFragment, baseId: Long, title: String, onChanged: () -> Unit) {
        val usernames = GgtgDb.loadUsernames(baseId)
        val map = load(baseId)
        val c = count(usernames, map)
        val dp = { v: Float -> AndroidUtilities.dp(v) }
        val root = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(20f), dp(8f), dp(20f), dp(4f))
        }
        val languages = c.languages.entries.joinToString(", ") { GgtgNames.languageLabel(it.key) + " " + it.value }
        root.addView(TextView(context).apply {
            textSize = 14f
            text = if (c.checked == 0) "База ещё не проверялась." else
                "В базе: ${c.total} · проверено: ${c.checked}\n" +
                "Живые люди: ${c.people} · активны за неделю: ${c.active} · «был(а) давно»: ${c.longAgo} · Premium: ${c.premium}\n" +
                "Не найдены или удалены: ${c.notFound} · боты: ${c.bots} · группы и каналы: ${c.chats} · не удалось проверить: ${c.errors}\n" +
                "Языки имён: ${languages.ifEmpty { "-" }}"
        })
        var dialog: AlertDialog? = null
        val reopen = {
            dialog?.dismiss()
            onChanged()
            report(context, fragment, baseId, title, onChanged)
        }
        fun row(text: String, onClick: () -> Unit) {
            root.addView(TextView(context).apply {
                this.text = text
                textSize = 15f
                setTextColor(GgtgUi.accent)
                setPadding(0, dp(12f), 0, dp(12f))
                setOnClickListener { onClick() }
            })
        }
        fun confirm(text: String, action: () -> Unit) {
            AlertDialog.Builder(context).setMessage(text)
                .setPositiveButton("Продолжить") { _, _ -> action() }
                .setNegativeButton("Отмена", null).show()
        }
        fun clean(text: String, count: Int, predicate: (Item) -> Boolean) {
            if (count <= 0) return
            row("$text ($count)") {
                confirm("$text: $count из базы «$title» будут удалены. Продолжить?") {
                    val kept = GgtgDb.loadUsernames(baseId).filter { raw -> map[key(raw)]?.let(predicate) != true }
                    val removed = usernames.size - kept.size
                    GgtgDb.saveUsernames(baseId, title, kept, crossDedupe = false)
                    Toast.makeText(context, "Удалено из базы: $removed", Toast.LENGTH_LONG).show()
                    reopen()
                }
            }
        }
        // moves the people matching the predicate into a base of their own
        fun move(text: String, suffix: String, count: Int, predicate: (Item) -> Boolean) {
            if (count <= 0) return
            row("$text ($count)") {
                confirm("$text: $count чел. переедут из базы «$title» в новую базу «$title · $suffix». Остальные останутся здесь. Продолжить?") {
                    val moved = ArrayList<String>()
                    val kept = ArrayList<String>()
                    for (raw in GgtgDb.loadUsernames(baseId)) {
                        val item = map[key(raw)]
                        if (item != null && item.state == State.Ok && predicate(item)) moved.add(raw) else kept.add(raw)
                    }
                    // a name lives in one base only: it leaves this one before the new one takes it
                    GgtgDb.saveUsernames(baseId, title, kept, crossDedupe = false)
                    GgtgDb.saveUsernames(GgtgDb.generateManualBaseId(), "$title · $suffix", moved, crossDedupe = false)
                    Toast.makeText(context, "Создана база «$title · $suffix»: ${moved.size} чел.", Toast.LENGTH_LONG).show()
                    reopen()
                }
            }
        }
        clean("Убрать не найденные и удалённые", c.notFound) { it.state == State.NotFound || it.state == State.Deleted }
        clean("Убрать ботов", c.bots) { it.state == State.Bot }
        clean("Убрать группы и каналы", c.chats) { it.state == State.Chat }
        clean("Убрать тех, у кого «был(а) давно»", c.longAgo) { it.state == State.Ok && it.seen == 5 }
        move("Выделить Premium в отдельную базу", "Premium", c.premium) { it.premium }

        val splittable = c.languages.filter { it.key.isNotEmpty() && it.value > 0 }.entries.toList()
        if (splittable.isNotEmpty()) {
            row("Разделить по языкам имён…") {
                val labels = splittable.map { GgtgNames.languageLabel(it.key) + " - " + it.value + " чел." }.toTypedArray()
                val checked = BooleanArray(splittable.size) { splittable[it].key != "lat" } // Latin is a mix, not a language
                app.ggtg.ai.GgtgAiUi.multiChoice(context, "Разделить по языкам", labels.toList(), checked, "Разделить") {
                    run {
                        var made = 0
                        for ((index, entry) in splittable.withIndex()) {
                            if (!checked[index]) continue
                            val moved = ArrayList<String>()
                            val kept = ArrayList<String>()
                            for (raw in GgtgDb.loadUsernames(baseId)) {
                                val item = map[key(raw)]
                                if (item != null && item.state == State.Ok && item.language == entry.key) moved.add(raw) else kept.add(raw)
                            }
                            if (moved.isEmpty()) continue
                            GgtgDb.saveUsernames(baseId, title, kept, crossDedupe = false)
                            GgtgDb.saveUsernames(GgtgDb.generateManualBaseId(), "$title · ${GgtgNames.languageLabel(entry.key)}", moved, crossDedupe = false)
                            made++
                        }
                        Toast.makeText(context, if (made == 0) "Ничего не отмечено" else "Создано баз: $made", Toast.LENGTH_LONG).show()
                        if (made > 0) reopen()
                    }
                }
            }
        }
        row(if (c.checked < c.total) "Проверить непроверенных" else "Проверить заново") {
            if (c.checked >= c.total) path(baseId).delete()
            dialog?.dismiss()
            start(fragment, context, baseId, title, onChanged)
        }

        dialog = AlertDialog.Builder(context)
            .setTitle("Анализ базы: $title")
            .setView(ScrollView(context).apply { addView(root) })
            .setPositiveButton("Закрыть", null)
            .show()
    }
}
