package app.ggtg

import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import org.telegram.messenger.MessagesController
import org.telegram.tgnet.TLRPC
import java.util.concurrent.atomic.AtomicInteger

// ng: Kotlin port of settings/views_campaign.cpp - boosts post views (messages.getMessagesViews
// with increment=true) and joins channels/groups, spread across the chosen accounts in parallel
// lanes. Progress goes to a GgtgProcess.
object GgtgViews {

    private const val CHUNK = 100
    private const val MAX_RANGE = 500
    private const val ACCOUNT_PAUSE_MIN = 1_000L
    private const val ACCOUNT_PAUSE_MAX = 3_000L

    @Volatile var cancelled = false

    data class Target(
        val username: String = "",
        val channelId: Long = 0,
        val ids: List<Int> = emptyList(),
        val inviteHash: String = "",
        val label: String = ""
    )

    private val viewRe = Regex("(?:https?://)?(?:t|telegram)\\.me/(?:c/(\\d+)|([A-Za-z][A-Za-z0-9_]{3,}))/(\\d+)(?:/(\\d+))?(?:\\s*-\\s*(\\d+))?")

    fun parseViewLinks(text: String): List<Target> {
        val result = ArrayList<Target>()
        for (m in viewRe.findAll(text)) {
            val first = (m.groupValues[4].ifEmpty { m.groupValues[3] }).toInt()
            val last = m.groupValues[5].ifEmpty { first.toString() }.toInt()
            if (first <= 0 || last < first || last - first >= MAX_RANGE) continue
            val username = m.groupValues[2]
            val channelId = m.groupValues[1].toLongOrNull() ?: 0
            result.add(Target(
                username = username, channelId = channelId, ids = (first..last).toList(),
                label = (if (username.isEmpty()) "c/${m.groupValues[1]}" else username) + "/$first" + if (last > first) "-$last" else ""
            ))
        }
        return result
    }

    private val inviteRe = Regex("^(?:https?://)?(?:t|telegram)\\.me/(?:\\+|joinchat/)([A-Za-z0-9_-]+)/?$")
    private val pubRe = Regex("^(?:(?:https?://)?(?:t|telegram)\\.me/|@)([A-Za-z][A-Za-z0-9_]{3,})/?$")

    fun parseJoinLinks(text: String): List<Target> {
        val result = ArrayList<Target>()
        val seen = HashSet<String>()
        for (token in text.split(Regex("[\\s,;]+")).filter { it.isNotEmpty() }) {
            val invite = inviteRe.matchEntire(token)
            val target = when {
                invite != null -> Target(inviteHash = invite.groupValues[1], label = "+${invite.groupValues[1]}")
                else -> {
                    val pub = pubRe.matchEntire(token) ?: continue
                    Target(username = pub.groupValues[1], label = pub.groupValues[1])
                }
            }
            if (seen.add(target.label.lowercase())) result.add(target)
        }
        return result
    }

    private suspend fun resolvePeer(slot: Int, target: Target): TLRPC.InputPeer? {
        val controller = MessagesController.getInstance(slot)
        if (target.username.isEmpty()) {
            if (target.channelId != 0L) return controller.getInputPeer(-target.channelId)
            return null
        }
        controller.getUserOrChat(target.username)?.let {
            if (it is TLRPC.Chat) return controller.getInputPeer(-it.id)
            if (it is TLRPC.User) return controller.getInputPeer(it.id)
        }
        val req = TLRPC.TL_contacts_resolveUsername().apply { username = target.username }
        val pair = GgtgAccounts.request(slot, req) ?: return null
        val resp = pair.first
        if (resp is TLRPC.TL_contacts_resolvedPeer) {
            controller.putUsers(resp.users, false)
            controller.putChats(resp.chats, false)
            resp.chats.firstOrNull()?.let { return controller.getInputPeer(-it.id) }
            resp.users.firstOrNull()?.let { return controller.getInputPeer(it.id) }
        }
        return null
    }

    private suspend fun viewTarget(slot: Int, target: Target): Pair<Boolean, String> {
        val peer = resolvePeer(slot, target) ?: return false to "не найдено"
        var offset = 0
        while (offset < target.ids.size) {
            val chunk = target.ids.subList(offset, minOf(offset + CHUNK, target.ids.size))
            val req = TLRPC.TL_messages_getMessagesViews().apply {
                this.peer = peer
                id = ArrayList(chunk)
                increment = true
            }
            val pair = GgtgAccounts.request(slot, req) ?: return false to "таймаут"
            pair.second?.let { return false to it.text }
            offset += chunk.size
        }
        return true to ""
    }

    private suspend fun joinTarget(slot: Int, target: Target): Pair<Boolean, String> {
        if (target.inviteHash.isNotEmpty()) {
            val req = TLRPC.TL_messages_importChatInvite().apply { hash = target.inviteHash }
            val pair = GgtgAccounts.request(slot, req) ?: return false to "таймаут"
            return joinResult(pair.second)
        }
        val peer = resolvePeer(slot, target) ?: return false to "не найдено"
        val controller = MessagesController.getInstance(slot)
        val channelId = when (peer) {
            is TLRPC.TL_inputPeerChannel -> peer.channel_id
            else -> return false to "это не канал/группа"
        }
        val req = TLRPC.TL_channels_joinChannel().apply { channel = controller.getInputChannel(channelId) }
        val pair = GgtgAccounts.request(slot, req) ?: return false to "таймаут"
        return joinResult(pair.second)
    }

    private fun joinResult(err: TLRPC.TL_error?): Pair<Boolean, String> = when {
        err == null -> true to ""
        err.text == "USER_ALREADY_PARTICIPANT" -> true to "уже в группе"
        err.text == "INVITE_REQUEST_SENT" -> true to "заявка отправлена"
        else -> false to err.text
    }

    suspend fun run(slots: List<Int>, targets: List<Target>, threads: Int, join: Boolean) = coroutineScope {
        cancelled = false
        GgtgAccounts.wake(slots)
        val process = GgtgProcess.start(
            if (join) "Вступление (${slots.size} акк., ${targets.size} ссыл.)"
            else "Просмотры (${slots.size} акк., ${targets.size} пост.)",
            GgtgProcess.Kind.Views
        )
        val next = AtomicInteger(0)
        val done = AtomicInteger(0)
        val failed = AtomicInteger(0)
        val lanes = threads.coerceIn(1, maxOf(1, slots.size))
        (0 until lanes).map {
            launch {
                while (!cancelled) {
                    val i = next.getAndIncrement()
                    if (i >= slots.size) break
                    val slot = slots[i]
                    var ok = 0
                    val errs = ArrayList<String>()
                    for (target in targets) {
                        if (cancelled) break
                        val (success, note) = if (join) joinTarget(slot, target) else viewTarget(slot, target)
                        if (success) ok++ else errs.add("${target.label}: $note")
                    }
                    done.incrementAndGet()
                    if (errs.isNotEmpty()) failed.incrementAndGet()
                    process.progress(done.get(), slots.size)
                    process.log("${GgtgAccounts.label(slot)} — " + if (errs.isEmpty())
                        (if (join) "вступил: $ok" else "просмотрено: $ok")
                    else "ошибок ${errs.size} (${errs.first()})")
                    delay(ACCOUNT_PAUSE_MIN + (0 until (ACCOUNT_PAUSE_MAX - ACCOUNT_PAUSE_MIN)).random())
                }
            }
        }.forEach { it.join() }
        process.succeed("Готово: ${done.get()} / ${slots.size}" + if (failed.get() > 0) ", с ошибками ${failed.get()}" else "")
    }
}
