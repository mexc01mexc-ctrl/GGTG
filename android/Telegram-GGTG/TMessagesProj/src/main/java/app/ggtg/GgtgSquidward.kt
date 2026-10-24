package app.ggtg

import kotlinx.coroutines.delay
import kotlinx.coroutines.suspendCancellableCoroutine
import org.telegram.messenger.AndroidUtilities
import org.telegram.messenger.ChatObject
import org.telegram.messenger.MessagesController
import org.telegram.messenger.SendMessagesHelper
import org.telegram.messenger.UserConfig
import org.telegram.tgnet.TLRPC

// ng: Kotlin port of window/ggtg_squidward_session.cpp, driven by polling instead of the desktop's
// reactive observers: it messages @skvidvarde_bot from the current account, reads its reply from
// the dialog history, parses the card with the tested SquidwardParse, and (on collect) presses the
// bot's own inline buttons to page through a person's groups or a group's members, saving the
// result to a base. Paid buttons are never pressed.
object GgtgSquidward {

    private const val BOT = "skvidvarde_bot"
    private const val LOOKUP_TIMEOUT_MS = 30_000L
    private const val PAGE_TIMEOUT_MS = 25_000L
    private const val POLL_MS = 1_500L
    private const val MAX_PAGES = 400

    @Volatile var cancelled = false

    // contacts.resolveUsername is tightly limited: a short FLOOD_WAIT is sat out, a long one ends
    // the check (the rest is kept unchecked), and a request with no answer is given up on.
    private const val MAX_FLOOD_WAIT = 60
    private const val RESOLVE_TIMEOUT_MS = 20_000L

    data class LookupResult(
        val kind: String,               // "user" | "group" | "unknown"
        val userCard: SquidwardParse.UserCard?,
        val groupCard: SquidwardParse.GroupCard?,
        val rawText: String,
        val details: List<String>,
        val baseTitle: String,
        val canCollect: Boolean
    )

    /** The account the bot is talked to from; -1 = whichever account the app is on. Lets an account
     *  from the «Корзина» (hidden from Telegram's own list) be used without switching to it. */
    @Volatile var viaSlot = -1

    /** The Squidward screen is open: its chosen account must stay awake. */
    @Volatile var screenOpen = false

    fun currentSlot(): Int = slot()

    private fun slot(): Int {
        val chosen = viaSlot
        return if (chosen in 0 until UserConfig.MAX_ACCOUNT_COUNT && UserConfig.getInstance(chosen).isClientActivated) chosen
        else UserConfig.selectedAccount
    }
    private fun controller() = MessagesController.getInstance(slot())

    private suspend fun resolveBot(): Pair<TLRPC.InputPeer?, Long> {
        controller().getUser(BOT)?.let { return controller().getInputPeer(it.id) to it.id }
        val req = TLRPC.TL_contacts_resolveUsername().apply { username = BOT }
        val pair = GgtgAccounts.request(slot(), req) ?: return null to 0
        val resp = pair.first
        if (resp is TLRPC.TL_contacts_resolvedPeer) {
            controller().putUsers(resp.users, false)
            val bot = resp.users.firstOrNull() ?: return null to 0
            return controller().getInputPeer(bot.id) to bot.id
        }
        return null to 0
    }

    private suspend fun history(peer: TLRPC.InputPeer, limit: Int): List<TLRPC.Message> {
        val req = TLRPC.TL_messages_getHistory().apply { this.peer = peer; this.limit = limit }
        val pair = GgtgAccounts.request(slot(), req) ?: return emptyList()
        val resp = pair.first
        return if (resp is TLRPC.messages_Messages) resp.messages else emptyList()
    }

    private fun toMessage(m: TLRPC.Message): SquidwardParse.Message {
        val entities = ArrayList<SquidwardParse.Entity>()
        for (e in m.entities) {
            val type = when (e) {
                is TLRPC.TL_messageEntityBold -> SquidwardParse.EntityType.Bold
                is TLRPC.TL_messageEntityUrl -> SquidwardParse.EntityType.Url
                is TLRPC.TL_messageEntityTextUrl -> SquidwardParse.EntityType.CustomUrl
                is TLRPC.TL_messageEntityMention -> SquidwardParse.EntityType.Mention
                is TLRPC.TL_messageEntityMentionName -> SquidwardParse.EntityType.MentionName
                else -> SquidwardParse.EntityType.Other
            }
            val data = when (e) {
                is TLRPC.TL_messageEntityTextUrl -> e.url ?: ""
                is TLRPC.TL_messageEntityMentionName -> "${e.user_id}"
                else -> ""
            }
            entities.add(SquidwardParse.Entity(type, e.offset, e.length, data))
        }
        return SquidwardParse.Message(m.message ?: "", entities)
    }

    private fun findButton(m: TLRPC.Message, match: (String) -> Boolean): Pair<Int, ByteArray>? {
        val markup = m.reply_markup as? TLRPC.TL_replyInlineMarkup ?: return null
        for (row in markup.rows) {
            for (button in row.buttons) {
                val type = button.type as? org.telegram.tgnet.tl.TL_keyboard.TL_inlineButtonTypeCallback ?: continue
                if (match(button.text ?: "")) return m.id to (type.data ?: ByteArray(0))
            }
        }
        return null
    }

    // "👁 Группы", "15💎 Groups" - but not "1💎 Similar groups".
    private fun isGroupsButton(text: String): Boolean {
        val words = SquidwardParse.buttonWords(text)
        return words == SquidwardParse.fold("группы") || words == SquidwardParse.fold("groups")
    }

    // "🎖 Топ, люди", "🎖 Top, members".
    private fun isPeopleButton(text: String): Boolean {
        val last = SquidwardParse.buttonLastWord(text)
        return (last == SquidwardParse.fold("люди") || last == SquidwardParse.fold("members") ||
            last == SquidwardParse.fold("people")) && !text.contains("💎")
    }

    /** "Имя (@username)" of the account the bot is talked to from. */
    fun viaLabel(): String {
        val user = UserConfig.getInstance(slot()).currentUser ?: return "—"
        val name = listOfNotNull(user.first_name, user.last_name).joinToString(" ").trim()
        val handle = when {
            !user.username.isNullOrEmpty() -> "@" + user.username
            !user.phone.isNullOrEmpty() -> "+" + user.phone
            else -> "ID " + user.id
        }
        return if (name.isEmpty()) handle else "$name ($handle)"
    }
    private fun isNextButton(text: String) = text.trim().startsWith("➡") && !text.contains("💎")

    private suspend fun clickButton(peer: TLRPC.InputPeer, msgId: Int, data: ByteArray) {
        val req = TLRPC.TL_messages_getBotCallbackAnswer().apply {
            this.peer = peer; msg_id = msgId
            if (data.isNotEmpty()) { this.data = data; flags = flags or 1 }
        }
        GgtgAccounts.request(slot(), req, timeoutMs = 20_000)
    }

    /** Sends [query] to the bot and returns the parsed dossier, or null on timeout/no bot. */
    suspend fun lookup(query: String, prefer: String? = null): LookupResult? =
        lookupFromBot(query, prefer)?.also { GgtgSquidwardCache.putCard(query, it) }

    // "Найдено несколько результатов, выберете что интересует." with a "User: …" and a "Group: …" button.
    private fun isChoiceText(text: String): Boolean {
        val t = text.lowercase()
        return t.contains("несколько результатов") || t.contains("several results") || t.contains("multiple results")
    }

    private fun isChoiceButton(text: String, group: Boolean): Boolean {
        val t = text.lowercase()
        return if (group) t.contains("group:") || t.contains("группа:") else t.contains("user:") || t.contains("пользователь:")
    }

    private fun callbackButtons(m: TLRPC.Message): List<Triple<String, Int, ByteArray>> {
        val markup = m.reply_markup as? TLRPC.TL_replyInlineMarkup ?: return emptyList()
        val result = ArrayList<Triple<String, Int, ByteArray>>()
        for (row in markup.rows) for (button in row.buttons) {
            val type = button.type as? org.telegram.tgnet.tl.TL_keyboard.TL_inlineButtonTypeCallback ?: continue
            result.add(Triple(button.text ?: "", m.id, type.data ?: ByteArray(0)))
        }
        return result
    }

    /** Asks the user which match to take; the index, or null when the dialog is dismissed. */
    private suspend fun askChoice(options: List<String>): Int? = suspendCancellableCoroutine { cont ->
        AndroidUtilities.runOnUIThread {
            val activity = org.telegram.ui.LaunchActivity.getLastFragment()?.parentActivity
            if (activity == null) {
                if (cont.isActive) cont.resume(null) {}
                return@runOnUIThread
            }
            val dialog = org.telegram.ui.ActionBar.AlertDialog.Builder(activity)
                .setTitle("Сквидвард нашёл несколько результатов")
                .setItems(options.toTypedArray()) { _, which -> if (cont.isActive) cont.resume(which) {} }
                .setNegativeButton("Отмена") { _, _ -> if (cont.isActive) cont.resume(null) {} }
                .create()
            dialog.setOnDismissListener { if (cont.isActive) cont.resume(null) {} }
            dialog.show()
        }
    }

    // A link into a private chat / an invite link can only be a group; otherwise [prefer] decides, and
    // with no preference the user is asked.
    private fun choiceWantsGroup(query: String, prefer: String?): Boolean? {
        val q = query.lowercase()
        if (q.contains("t.me/c/") || q.contains("t.me/+") || q.contains("joinchat")) return true
        return when (prefer) { "group" -> true; "user" -> false; else -> null }
    }

    private suspend fun lookupFromBot(query: String, prefer: String? = null): LookupResult? {
        val (peer, botId) = resolveBot()
        if (peer == null) return null
        val floor = history(peer, 1).firstOrNull { !it.out }?.id ?: 0
        val slot = slot()
        AndroidUtilities.runOnUIThread {
            SendMessagesHelper.getInstance(slot).sendMessage(
                SendMessagesHelper.SendMessageParams.of(query.trim(), botId)
            )
        }
        var deadline = System.currentTimeMillis() + LOOKUP_TIMEOUT_MS
        var choiceId = -1
        while (System.currentTimeMillis() < deadline && !cancelled) {
            delay(POLL_MS)
            val msg = history(peer, 3).firstOrNull { !it.out && it.id > floor && !SquidwardParse.isPlaceholder(it.message ?: "") }
                ?: continue
            if (isChoiceText(msg.message ?: "")) {
                // several matches: press the wanted one and wait for the bot to show that card
                if (msg.id == choiceId) continue
                val wantsGroup = choiceWantsGroup(query, prefer)
                var button = if (wantsGroup != null) findButton(msg) { isChoiceButton(it, wantsGroup) } else null
                if (button == null) {
                    val options = callbackButtons(msg)
                    if (options.isNotEmpty()) {
                        val index = askChoice(options.map { it.first })
                        if (index == null) {
                            return LookupResult("unknown", null, null, "Выбор отменён", emptyList(),
                                SquidwardParse.groupBaseTitleFor(query, ""), false)
                        }
                        button = options[index].second to options[index].third
                    }
                }
                if (button != null) {
                    choiceId = msg.id
                    clickButton(peer, button.first, button.second)
                    // the time spent choosing is not the bot's silence
                    deadline = System.currentTimeMillis() + LOOKUP_TIMEOUT_MS
                    continue
                }
            }
            val parsed = toMessage(msg)
            val userCard = SquidwardParse.parseUserCard(parsed)
            if (userCard != null) {
                return LookupResult("user", userCard, null, SquidwardParse.cleanText(parsed.text),
                    SquidwardParse.formatUserDetails(userCard),
                    SquidwardParse.groupBaseTitleFor(query, userCard.name),
                    findButton(msg, ::isGroupsButton) != null)
            }
            val groupCard = SquidwardParse.parseGroupCard(parsed)
            if (groupCard != null) {
                return LookupResult("group", null, groupCard, SquidwardParse.cleanText(parsed.text),
                    SquidwardParse.formatGroupDetails(groupCard),
                    SquidwardParse.groupBaseTitleFor(query, groupCard.name),
                    findButton(msg, ::isPeopleButton) != null)
            }
            // Unknown reply (not found / a notice) - report raw, no collect.
            return LookupResult("unknown", null, null, SquidwardParse.cleanText(parsed.text), emptyList(),
                SquidwardParse.groupBaseTitleFor(query, ""), false)
        }
        return null
    }

    /** Collects the last card's list (person's groups / group's members) into a base. */
    suspend fun collect(query: String, isUser: Boolean, baseTitle: String) {
        cancelled = false
        val process = GgtgProcess.start(
            (if (isUser) "Сбор групп: " else "Сбор участников: ") + baseTitle, GgtgProcess.Kind.Scrape
        )
        val (peer, _) = resolveBot()
        if (peer == null) { process.fail("Бот не найден"); return }
        // The card is the newest incoming message; press its Groups/People button.
        var msg = history(peer, 3).firstOrNull { !it.out } ?: run { process.fail("Ответ бота не найден"); return }
        var button = findButton(msg, if (isUser) ::isGroupsButton else ::isPeopleButton)
            ?: run { process.fail("Нет кнопки сбора"); return }
        clickButton(peer, button.first, button.second)

        val groupRows = ArrayList<SquidwardParse.GroupRow>()
        val userRows = ArrayList<SquidwardParse.UserRow>()
        var lastPage = 0
        var listId = 0
        var pages = 0
        for (step in 0 until MAX_PAGES) {
            if (cancelled) break
            val deadline = System.currentTimeMillis() + PAGE_TIMEOUT_MS
            var page: TLRPC.Message? = null
            while (System.currentTimeMillis() < deadline && !cancelled) {
                delay(POLL_MS)
                val candidate = history(peer, 3).firstOrNull { !it.out && !SquidwardParse.isPlaceholder(it.message ?: "") }
                if (candidate != null) {
                    val info = SquidwardParse.parsePageInfo(candidate.message ?: "")
                    val fresh = candidate.id != listId || (info != null && info.page != lastPage)
                    if (fresh && (info != null || findButton(candidate, ::isNextButton) != null)) { page = candidate; break }
                }
            }
            if (page == null) { process.log("страница не пришла — сохраняем что есть"); break }
            listId = page.id
            val parsed = toMessage(page)
            val info = SquidwardParse.parsePageInfo(parsed.text)
            if (info != null) { lastPage = info.page; pages = info.pages }
            if (isUser) groupRows.addAll(SquidwardParse.extractGroupRows(parsed))
            else userRows.addAll(SquidwardParse.extractUserRows(parsed))
            val found = if (isUser) SquidwardParse.dedupeGroupRows(groupRows).size else userRows.size
            process.progress(lastPage, if (pages > 0) pages else lastPage + 1)
            process.log("страница ${lastPage.takeIf { it > 0 } ?: (step + 1)}${if (pages > 0) " из $pages" else ""} · найдено $found")
            val next = findButton(page, ::isNextButton)
            if (next == null || (info != null && info.page >= info.pages)) break
            clickButton(peer, next.first, next.second)
        }

        val stopped = cancelled
        if (isUser) {
            val rows = SquidwardParse.dedupeGroupRows(groupRows)
            val check = checkGroups(rows, process)
            val entries = check.kept
            if (entries.isEmpty()) { process.fail(if (stopped) "Сбор остановлен, групп не собрано" else "Групп не найдено"); return }
            if (!stopped) GgtgSquidwardCache.putGroups(query, check)
            GgtgDb.saveGroupEntries(GgtgDb.generateManualBaseId(), baseTitle, entries)
            process.succeed(groupsSummary(check))
        } else {
            val names = SquidwardParse.dedupeUsernames(userRows.mapNotNull { it.username.ifEmpty { null } })
            if (names.isEmpty()) { process.fail("Участников с юзернеймом не найдено"); return }
            if (!stopped) GgtgSquidwardCache.putUsers(query, names)
            GgtgDb.saveUsernames(GgtgDb.generateManualBaseId(), baseTitle, names)
            process.succeed("Сохранено пользователей: ${names.size}")
        }
    }

    data class GroupCheck(
        val kept: List<GgtgDb.GroupEntry>,
        val channelsRemoved: Int,
        val closed: Int,
        val unchecked: Int
    )

    fun groupsSummary(check: GroupCheck): String = buildString {
        append("Сохранено групп: ${check.kept.size}")
        if (check.channelsRemoved > 0) append(" · каналов убрано: ${check.channelsRemoved}")
        if (check.closed > 0) append(" · закрытых: ${check.closed}")
        if (check.unchecked > 0) append(" · не проверено на каналы: ${check.unchecked} (лимит Telegram)")
    }

    // Channels are of no use as recipients: every group with a username is looked up (cache first,
    // then contacts.resolveUsername with a pause), broadcast channels and people are dropped, and a
    // megagroup's real size is read (resolving alone doesn't tell it). A group that can't be checked
    // now is kept as is.
    private suspend fun checkGroups(rows: List<SquidwardParse.GroupRow>, process: GgtgProcess.Handle): GroupCheck {
        val slot = slot()
        val controller = controller()
        val kept = ArrayList<GgtgDb.GroupEntry>()
        var channels = 0
        var unchecked = 0
        val closed = rows.count { it.link.isNotEmpty() }
        var giveUp = false
        process.log("Отделяем группы от каналов: ${rows.size}")
        for ((index, row) in rows.withIndex()) {
            val entry = GgtgDb.GroupEntry(username = row.username, title = row.title, link = row.link)
            if (row.username.isEmpty()) { kept.add(entry); continue }
            if (giveUp || cancelled) { kept.add(entry); unchecked++; continue }

            var chat: TLRPC.Chat? = null
            var isUserPeer = false
            var pause = 0L
            val cached = controller.getUserOrChat(row.username)
            if (cached is TLRPC.Chat) chat = cached else if (cached is TLRPC.User) isUserPeer = true
            if (cached == null) {
                var settled = false
                while (!settled) {
                    val req = TLRPC.TL_contacts_resolveUsername().apply { username = row.username }
                    val pair = GgtgAccounts.request(slot, req, timeoutMs = RESOLVE_TIMEOUT_MS)
                    if (pair == null) { giveUp = true; break }
                    val (resp, err) = pair
                    if (err != null) {
                        val text = err.text ?: ""
                        if (text.startsWith("FLOOD_WAIT_")) {
                            val seconds = text.substringAfterLast('_').toIntOrNull() ?: 0
                            if (seconds in 1..MAX_FLOOD_WAIT && !cancelled) {
                                process.log("Telegram попросил паузу — ждём $seconds с…")
                                delay(seconds * 1000L + 500)
                                continue
                            }
                            giveUp = true
                            break
                        }
                        pause = 800 // not found or similar: kept, as on desktop
                        settled = true
                    } else {
                        if (resp is TLRPC.TL_contacts_resolvedPeer) {
                            controller.putUsers(resp.users, false)
                            controller.putChats(resp.chats, false)
                            chat = resp.chats.firstOrNull()
                            isUserPeer = chat == null && resp.users.isNotEmpty()
                        }
                        pause = 400
                        settled = true
                    }
                }
                if (giveUp) { kept.add(entry); unchecked++; continue }
            }

            when {
                chat != null && ChatObject.isChannel(chat) && !chat.megagroup -> channels++
                isUserPeer -> {} // a person or a bot mentioned on the page, not a group
                else -> {
                    var members = chat?.participants_count ?: 0
                    if (chat != null && chat.megagroup && members <= 1) {
                        val req = TLRPC.TL_channels_getFullChannel().apply { channel = MessagesController.getInputChannel(chat) }
                        val full = GgtgAccounts.request(slot, req, timeoutMs = RESOLVE_TIMEOUT_MS)?.first
                        if (full is TLRPC.TL_messages_chatFull) members = maxOf(members, full.full_chat?.participants_count ?: 0)
                        pause += 400
                    }
                    kept.add(entry.copy(
                        title = entry.title.ifEmpty { chat?.title ?: "" },
                        membersCount = if (members > 1) members else 0,
                        checked = true))
                }
            }
            process.progress(index + 1, rows.size)
            if ((index + 1) % 10 == 0) process.log("проверено ${index + 1} из ${rows.size} · групп ${kept.size}")
            if (pause > 0) delay(pause)
        }
        return GroupCheck(kept, channels, closed, unchecked)
    }
}
