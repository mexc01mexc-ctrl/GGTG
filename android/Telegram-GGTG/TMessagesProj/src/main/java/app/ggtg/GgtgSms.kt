package app.ggtg

import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import org.telegram.messenger.MessagesController
import org.telegram.messenger.UserConfig
import org.telegram.tgnet.ConnectionsManager
import org.telegram.tgnet.TLRPC
import java.util.concurrent.atomic.AtomicInteger
import kotlin.random.Random

// ng: Kotlin port of the SMS campaign engine (settings/sms_campaign.cpp) - sends a rendered
// message to every recipient of the chosen bases, spreading them across accounts in parallel
// lanes, one recipient at a time per lane, with the desktop app's pauses, per-account quota,
// daily limit, spam/flood handling and template rendering. All progress goes to a GgtgProcess.
object GgtgSms {

    data class Config(
        val slots: List<Int>,
        val usernames: List<String>,
        val baseOf: Map<String, Long>,
        val templates: List<String>,
        val perAccountQuota: Int,
        val threads: Int,
        val dailyLimit: Int,
        val pauseMinMs: Long,
        val pauseMaxMs: Long,
        // ng: "Пауза между аккаунтами" - the cooldown before a lane picks up the
        // next account once the current one has sent its per-account quota.
        val pauseAccountMinMs: Long = 60_000L,
        val pauseAccountMaxMs: Long = 90_000L,
        // ng: "Сначала приветственный стикер" - a 👋 sticker goes out first and the text follows
        // 10-15 seconds later; with an empty template the sticker alone is the whole message.
        val greetingSticker: Boolean = false,
        // ggtg: ИИ-Защита аккаунтов (GramGPT-style protection modes)
        // 0 = Консервативный, 1 = Оптимальный, 2 = Скоростной
        val protectionMode: Int = 1,
        val nightSleep: Boolean = true,
        val simulateTypos: Boolean = true,
        val filterByGgr: Boolean = false,
        val skipSuspectedFakes: Boolean = true,
        val translate: Boolean = false,
        val translateLang: String = "auto"
    )

    private const val NOT_FOUND_STREAK = 5
    private const val ERROR_STREAK = 5
    // refusals in a row (PEER_FLOOD with no success between) after which an account is set aside
    // even though @spambot did not call it limited
    private const val PEER_REFUSAL_STREAK = 6
    // an @spambot verdict younger than this is not asked for again
    private const val VERDICT_FRESH_SEC = 300L
    private const val MAX_FLOOD_WAIT = 600
    private const val SKIP_PAUSE = 2_000L
    // a resolve that timed out: the recipient is put back in the queue, not written off
    private const val TIMEOUT_RESOLVE = "таймаут поиска"
    // ng: "[SPLIT]" - the short gap between two parts of one recipient's message
    // when the text is split into several separate messages from one account.
    private const val SPLIT_PAUSE_MIN = 2_000L
    private const val SPLIT_PAUSE_MAX = 5_000L
    // ng: the gap between the greeting sticker and the text that follows it
    private const val GREETING_TEXT_PAUSE_MIN = 10_000L
    private const val GREETING_TEXT_PAUSE_MAX = 15_000L

    @Volatile var cancelled = false

    private enum class Outcome { Sent, SpamBlock, Skip, NotFound, FloodWait, AccountDead, Error }

    fun floodWaitSeconds(type: String?): Int {
        if (type == null) return 0
        val m = Regex("^(?:FLOOD|FLOOD_PREMIUM|SLOWMODE)_WAIT_(\\d+)$").find(type) ?: return 0
        return maxOf(1, m.groupValues[1].toInt())
    }

    private val deadErrors = setOf(
        "AUTH_KEY_UNREGISTERED", "AUTH_KEY_INVALID", "AUTH_KEY_DUPLICATED", "SESSION_REVOKED",
        "SESSION_EXPIRED", "USER_DEACTIVATED", "USER_DEACTIVATED_BAN", "PHONE_NUMBER_BANNED"
    )
    private val recipientOnly = setOf(
        "USER_PRIVACY_RESTRICTED", "USER_IS_BLOCKED", "YOU_BLOCKED_USER", "INPUT_USER_DEACTIVATED",
        "PEER_ID_INVALID", "USER_NOT_MUTUAL_CONTACT", "PRIVACY_PREMIUM_REQUIRED"
    )

    // ---- template rendering: {a|b} spintax + [NAME]/[SURNAME]/[MYNAME]/[MYSURNAME]/[GROUP] ----

    /** The group a base was collected from, for [GROUP]: the base's title without the "(@username)" of the
     *  group; empty for a list made by hand (use [GROUP|друзья]). */
    fun groupNameFromTitle(title: String): String =
        title.replace(Regex("\\s*\\(@[A-Za-z0-9_]{4,32}\\)"), "").trim()

    fun groupNameOfBase(baseId: Long): String {
        if (baseId == 0L) return ""
        val title = GgtgDb.baseTitle(baseId)
        if (title.isEmpty()) return ""
        val handle = Regex("\\s*\\(@[A-Za-z0-9_]{4,32}\\)")
        val manual = (baseId ushr 63) != 0L
        if (manual && !handle.containsMatchIn(title)) return ""
        return title.replace(handle, "").trim()
    }

    fun render(template: String, recipientFirst: String, recipientLast: String, senderFirst: String, senderLast: String,
               groupName: String = ""): String {
        var result = template
        val group = Regex("\\{([^{}]*)\\}")
        while (true) {
            val m = group.find(result) ?: break
            val options = splitSpintax(m.groupValues[1])
            result = result.replaceRange(m.range, options[Random.nextInt(options.size)])
        }
        val token = Regex("\\[(NAME|SURNAME|MYNAME|MYSURNAME|GROUP)(?:\\|([^\\]]*))?\\]")
        // ng: [NAME] is a name fit for a greeting - no emoji/symbols from the profile name, and for an
        // obviously not-a-name ("qqqq", one letter) "друг" in the language of the text, unless the
        // template gave its own [NAME|fallback]
        val greetingName = GgtgNames.cleanPersonName(recipientFirst)
        val out = StringBuilder()
        var last = 0
        var blank = false
        for (mt in token.findAll(result)) {
            out.append(result, last, mt.range.first)
            val value = when (mt.groupValues[1]) {
                "NAME" -> greetingName; "SURNAME" -> recipientLast
                "MYNAME" -> senderFirst; "GROUP" -> groupName; else -> senderLast
            }
            var fallback = mt.groupValues[2]
            if (mt.groupValues[1] == "NAME" && value.isEmpty() && fallback.isEmpty()) {
                fallback = GgtgNames.friendWord(result)
            }
            if (value.isEmpty() && fallback.isEmpty()) blank = true
            out.append(value.ifEmpty { fallback })
            last = mt.range.last + 1
        }
        out.append(result, last, result.length)
        var text = out.toString().replace(Regex(" {2,}"), " ")
        if (blank) text = text.replace(Regex(" +([,.!?;:])"), "$1")
        return text.trim()
    }

    private fun splitSpintax(group: String): List<String> {
        val result = ArrayList<String>()
        val current = StringBuilder()
        var depth = 0
        for (ch in group) {
            if (ch == '[') depth++ else if (ch == ']' && depth > 0) depth--
            if (ch == '|' && depth == 0) { result.add(current.toString()); current.clear() } else current.append(ch)
        }
        result.add(current.toString())
        return result
    }

    private suspend fun resolveInputPeer(slot: Int, username: String): Pair<TLRPC.InputPeer?, String> {
        val controller = MessagesController.getInstance(slot)
        val cached = controller.getUser(username)
        if (cached != null) return controller.getInputPeer(cached.id) to ""
        val req = TLRPC.TL_contacts_resolveUsername().apply { this.username = username }
        // a lookup that never got an answer is NOT "user not found" - report it as a timeout
        val pair = GgtgAccounts.request(slot, req) ?: return null to TIMEOUT_RESOLVE
        val err = pair.second
        if (err != null) return null to err.text
        val resp = pair.first
        if (resp is TLRPC.TL_contacts_resolvedPeer) {
            controller.putUsers(resp.users, false)
            controller.putChats(resp.chats, false)
            val user = resp.users.firstOrNull()
            if (user != null) return controller.getInputPeer(user.id) to ""
        }
        return null to ""
    }

    // ng: a failed send as the log shows it: what Telegram's error means in plain words, with the
    // raw code kept in brackets (mirrors HumanSmsError in settings/sms_campaign.cpp).
    fun humanError(type: String): String {
        if (type.isEmpty()) return "неизвестная"
        val reason = when {
            type == "USER_PRIVACY_RESTRICTED" -> "получатель закрыл личные сообщения от незнакомых"
            type == "USER_NOT_MUTUAL_CONTACT" -> "писать можно только общим контактам"
            type == "PRIVACY_PREMIUM_REQUIRED" -> "получатель принимает сообщения только от Premium"
            type == "USER_IS_BLOCKED" || type == "YOU_BLOCKED_USER" -> "получатель заблокировал аккаунт (или он его)"
            type == "PEER_FLOOD" -> "спам-ограничение аккаунта (PEER_FLOOD)"
            floodWaitSeconds(type) > 0 -> "слишком частые запросы, ждать ${floodWaitSeconds(type)} сек"
            else -> return type
        }
        return "$reason [$type]"
    }

    // ng: how long the "печатает..." indicator is shown for a text - grows with its length, like a
    // person composing it.
    private fun typingPause(text: String): Long =
        (text.length * 50L).coerceIn(1_500L, 7_000L) + Random.nextLong(1_000L)

    private fun pause(minMs: Long, maxMs: Long): Long = minMs + Random.nextLong(maxMs - minMs + 1)

    fun isAccountSleeping(slot: Int, protectionMode: Int, nightSleep: Boolean): Pair<Boolean, Int> {
        if (!nightSleep || protectionMode == 2) return false to 0
        val phone = UserConfig.getInstance(slot).currentUser?.phone ?: ""
        val tz = GgtgGeo.matchForPhone(phone).tzOffset
        val utcMs = System.currentTimeMillis()
        val localMs = utcMs + tz * 1000L
        val cal = java.util.Calendar.getInstance(java.util.TimeZone.getTimeZone("UTC"))
        cal.timeInMillis = localMs
        val hour = cal.get(java.util.Calendar.HOUR_OF_DAY)
        val sleepStart = if (protectionMode == 0) 1 else 2
        val sleepEnd = if (protectionMode == 0) 7 else 6
        val sleeping = hour in sleepStart until sleepEnd
        return sleeping to sleepEnd
    }

    // ng: "Лёгкий прогрев" as in the desktop app: what a person does before writing to a stranger,
    // each step a real request followed by a pause as if looking at it - opens the profile, looks at
    // it, looks at the photo (when there is one), opens the chat. Results are not needed, only the
    // requests (a failed one never stops the send).
    private suspend fun warmUp(slot: Int, clean: String, peer: TLRPC.InputPeer, protectionMode: Int, step: (String) -> Unit) {
        val controller = MessagesController.getInstance(slot)
        val user = controller.getUser(clean)
        val profileChance = if (protectionMode == 0) 90 else if (protectionMode == 2) 30 else 70
        val photoChance = if (protectionMode == 0) 60 else if (protectionMode == 2) 15 else 35
        val paceMultiplier = if (protectionMode == 0) 1.35 else if (protectionMode == 2) 0.7 else 1.0

        if (Random.nextInt(100) < profileChance) {
            step("открывает профиль")
            if (user != null) GgtgAccounts.request(slot, TLRPC.TL_users_getFullUser().apply { id = controller.getInputUser(user) })
            delay((pause(1_000L, 2_000L) * paceMultiplier).toLong())
            step("смотрит профиль")
            delay((pause(2_000L, 4_000L) * paceMultiplier).toLong())
            if (Random.nextInt(100) < photoChance && user != null && user.photo != null && user.photo !is TLRPC.TL_userProfilePhotoEmpty) {
                step("смотрит фото")
                GgtgAccounts.request(slot, TLRPC.TL_photos_getUserPhotos().apply {
                    user_id = controller.getInputUser(user); offset = 0; max_id = 0; limit = 1
                })
                delay((pause(1_500L, 3_500L) * paceMultiplier).toLong())
            }
        }
        step("открывает переписку")
        GgtgAccounts.request(slot, TLRPC.TL_messages_getHistory().apply { this.peer = peer; limit = 20 })
        delay((pause(1_000L, 2_500L) * paceMultiplier).toLong())
    }

    // ng: the 👋 stickers (same lookup the official client uses for its "suggest a sticker" greeting:
    // a plain stickers-by-emoji request), cached per account after the first answer.
    private val greetingStickers = HashMap<Int, List<TLRPC.Document>>()

    private suspend fun fetchGreetingSticker(slot: Int): TLRPC.Document? {
        val cached = synchronized(greetingStickers) { greetingStickers[slot] }
        val list = cached ?: run {
            val req = TLRPC.TL_messages_getStickers().apply { emoticon = "👋"; hash = 0 }
            val answer = GgtgAccounts.request(slot, req)?.first as? TLRPC.TL_messages_stickers
            val loaded = answer?.stickers?.toList().orEmpty()
            if (loaded.isNotEmpty()) synchronized(greetingStickers) { greetingStickers[slot] = loaded }
            loaded
        }
        return list.randomOrNull()
    }

    private suspend fun sendSticker(slot: Int, peer: TLRPC.InputPeer, document: TLRPC.Document): Pair<Outcome, String> {
        val req = TLRPC.TL_messages_sendMedia().apply {
            this.peer = peer
            message = ""
            random_id = Random.nextLong()
            media = TLRPC.TL_inputMediaDocument().apply {
                id = TLRPC.TL_inputDocument().apply {
                    id = document.id
                    access_hash = document.access_hash
                    file_reference = document.file_reference
                }
            }
        }
        val pair = GgtgAccounts.request(slot, req, ConnectionsManager.RequestFlagCanCompress) ?: return Outcome.Error to "таймаут"
        val err = pair.second ?: return Outcome.Sent to ""
        return when {
            err.text == "PEER_FLOOD" -> Outcome.SpamBlock to err.text
            floodWaitSeconds(err.text) > 0 -> Outcome.FloodWait to err.text
            err.text in deadErrors -> Outcome.AccountDead to err.text
            else -> Outcome.Error to err.text
        }
    }

    private suspend fun sendOne(
        slot: Int,
        username: String,
        template: String,
        greetingSticker: Boolean,
        groupName: String = "",
        protectionMode: Int = 1,
        simulateTypos: Boolean = true,
        skipSuspectedFakes: Boolean = true,
        translate: Boolean = false,
        translateLang: String = "auto",
        step: (String) -> Unit
    ): Pair<Outcome, String> {
        val clean = username.trim().removePrefix("@")
        if (clean.isEmpty()) return Outcome.Skip to "пустой username"
        if (GgtgDb.isBotUsername(clean)) return Outcome.Skip to "это бот"
        step("ищет @$clean")
        val (peer, error) = resolveInputPeer(slot, clean)
        if (peer == null) {
            floodWaitSeconds(error).let { if (it > 0) return Outcome.FloodWait to error }
            if (error in deadErrors) return Outcome.AccountDead to error
            if (error == TIMEOUT_RESOLVE) return Outcome.Error to TIMEOUT_RESOLVE
            val notFound = error.isEmpty() || error == "USERNAME_NOT_OCCUPIED" || error == "USERNAME_INVALID"
            return (if (notFound) Outcome.NotFound else Outcome.Skip) to error.ifEmpty { "не найден" }
        }
        val user = MessagesController.getInstance(slot).getUser(clean)
        if (user != null && GgtgAnalysis.seenBucket(slot, user) == 5) {
            step("был(а) давно, пропуск")
            return Outcome.Skip to "был(а) в сети давно"
        }
        if (skipSuspectedFakes && user != null) {
            val fakeCheck = GgtgFakeDetector.analyzeUser(user, null)
            if (fakeCheck.fakeProbability >= 65) {
                step("предположительный фейк (${fakeCheck.fakeProbability}%)")
                return Outcome.Skip to "предположительный фейк (${fakeCheck.fakeProbability}%): ${fakeCheck.reasons.joinToString(", ")}"
            }
        }
        warmUp(slot, clean, peer, protectionMode, step)
        val self = GgtgAccounts.user(slot)

        var templateToSend = template
        if (translate) {
            val u = MessagesController.getInstance(slot).getUser(clean)
            val fn = u?.first_name ?: ""
            val ln = u?.last_name ?: ""
            val combined = "$fn $ln"
            val target = if (translateLang != "auto") translateLang else {
                if (Regex("[\\uac00-\\ud7af\\u1100-\\u11ff]").containsMatchIn(combined) || groupName.contains("коре", ignoreCase = true) || groupName.contains("korea", ignoreCase = true) || groupName.contains("kr", ignoreCase = true)) "ko"
                else if (Regex("[\\u4e00-\\u9fff]").containsMatchIn(combined) || groupName.contains("кита", ignoreCase = true) || groupName.contains("china", ignoreCase = true) || groupName.contains("cn", ignoreCase = true)) "zh-CN"
                else if (Regex("[\\u0400-\\u04ff]").containsMatchIn(combined)) "ru"
                else if (Regex("[\\u3040-\\u30ff]").containsMatchIn(combined) || groupName.contains("япон", ignoreCase = true) || groupName.contains("japan", ignoreCase = true)) "ja"
                else if (Regex("[\\u0600-\\u06ff]").containsMatchIn(combined)) "ar"
                else ""
            }
            if (target.isNotEmpty()) {
                step("переводит на $target")
                val latch = java.util.concurrent.CountDownLatch(1)
                var translatedResult: String? = null
                GgtgTranslate.translateTemplate(template, target) { res, _ ->
                    translatedResult = res
                    latch.countDown()
                }
                latch.await(5, java.util.concurrent.TimeUnit.SECONDS)
                if (!translatedResult.isNullOrEmpty()) {
                    templateToSend = translatedResult!!
                    step("перевёл на $target")
                }
            }
        }

        val text = render(templateToSend,
            MessagesController.getInstance(slot).getUser(clean)?.first_name ?: "",
            MessagesController.getInstance(slot).getUser(clean)?.last_name ?: "",
            self?.first_name ?: "", self?.last_name ?: "", groupName)
        // ng: "[SPLIT]" - one recipient may get several messages in a row.
        val parts = splitMessage(text)
        val textEmpty = parts.size == 1 && parts[0].isEmpty()
        if (greetingSticker) {
            step("выбирает приветственный стикер")
            val sticker = fetchGreetingSticker(slot)
            if (sticker == null) {
                // nothing to send at all when the text is empty too
                if (textEmpty) return Outcome.Error to "приветственный стикер не найден"
                step("стикер не нашёл")
            } else {
                // the real "choosing a sticker" indicator for a moment, like leafing through the panel
                GgtgAccounts.request(slot, TLRPC.TL_messages_setTyping().apply {
                    this.peer = peer; action = TLRPC.TL_sendMessageChooseStickerAction()
                })
                delay(pause(1_000L, 2_500L))
                val sent = sendSticker(slot, peer, sticker)
                if (sent.first != Outcome.Sent) return sent
                step("отправил привет-стикер")
                if (textEmpty) return Outcome.Sent to ""
                step("ждёт перед текстом")
                delay(pause(GREETING_TEXT_PAUSE_MIN, GREETING_TEXT_PAUSE_MAX))
            }
        } else if (textEmpty) {
            return Outcome.Skip to "введите текст сообщения"
        }
        for ((i, part) in parts.withIndex()) {
            // the real "печатает..." indicator, for as long as a person would take to type it
            val label = if (parts.size > 1) " ${i + 1} из ${parts.size}" else ""
            step("печатает смс$label")
            GgtgAccounts.request(slot, TLRPC.TL_messages_setTyping().apply {
                this.peer = peer; action = TLRPC.TL_sendMessageTypingAction()
            })
            val typoChance = if (protectionMode == 0) 8 else if (protectionMode == 2) 2 else 5
            val simulateTypo = simulateTypos && (Random.nextInt(100) < typoChance)
            if (simulateTypo) {
                delay(1500L + Random.nextLong(1000L))
                step("исправляет опечатку...")
                delay(1200L + Random.nextLong(1000L))
                step("печатает смс$label")
                GgtgAccounts.request(slot, TLRPC.TL_messages_setTyping().apply {
                    this.peer = peer; action = TLRPC.TL_sendMessageTypingAction()
                })
                delay(1500L + Random.nextLong(1200L))
            } else {
                val typing = typingPause(part)
                if (typing > 4_500L) {
                    // the indicator lives about 5 seconds on the server's side
                    delay(4_000L)
                    GgtgAccounts.request(slot, TLRPC.TL_messages_setTyping().apply {
                        this.peer = peer; action = TLRPC.TL_sendMessageTypingAction()
                    })
                    delay(typing - 4_000L)
                } else {
                    delay(typing)
                }
            }
            val outcome = sendText(slot, peer, part)
            if (outcome.first != Outcome.Sent) return outcome
            step("отправил смс$label")
            if (i < parts.size - 1) delay(SPLIT_PAUSE_MIN + Random.nextLong(SPLIT_PAUSE_MAX - SPLIT_PAUSE_MIN + 1))
        }
        return Outcome.Sent to ""
    }

    // ng: splits already-rendered text on [SPLIT], trims each piece and drops the
    // empty ones; no [SPLIT] (or empty text) yields a single-element list.
    private fun splitMessage(text: String): List<String> {
        val parts = text.split("[SPLIT]").map { it.trim() }.filter { it.isNotEmpty() }
        return if (parts.isEmpty()) listOf(text.trim()) else parts
    }

    private suspend fun sendText(slot: Int, peer: TLRPC.InputPeer, text: String): Pair<Outcome, String> {
        val req = TLRPC.TL_messages_sendMessage().apply {
            this.peer = peer
            this.message = text
            random_id = Random.nextLong()
        }
        val pair = GgtgAccounts.request(slot, req, ConnectionsManager.RequestFlagCanCompress) ?: return Outcome.Error to "таймаут"
        val err = pair.second ?: return Outcome.Sent to ""
        return when {
            err.text == "PEER_FLOOD" -> Outcome.SpamBlock to err.text
            floodWaitSeconds(err.text) > 0 -> Outcome.FloodWait to err.text
            err.text in deadErrors -> Outcome.AccountDead to err.text
            else -> Outcome.Error to err.text
        }
    }

    /** How many messages the run can really send and about how long it takes: the people left, the
     *  accounts' quota and daily limit, the pauses, the lanes (the same sum as the desktop preflight). */
    fun estimate(config: Config): Pair<Int, Long> {
        val accounts = config.slots.count { !GgtgSpamStatus.isBlocked(UserConfig.getInstance(it).clientUserId) }
        if (accounts == 0) return 0 to 0L
        val left = config.usernames.count { GgtgDb.findSmsRecord(it) == null && !GgtgDb.isBotUsername(it) }
        var planned = left
        if (config.perAccountQuota > 0) planned = minOf(planned, accounts * config.perAccountQuota)
        if (config.dailyLimit > 0) planned = minOf(planned, accounts * config.dailyLimit)
        val lanes = config.threads.coerceIn(1, accounts)
        val avgPause = (config.pauseMinMs + config.pauseMaxMs) / 2000.0
        val avgAccountPause = (config.pauseAccountMinMs + config.pauseAccountMaxMs) / 2000.0
        val accountsPerLane = (accounts + lanes - 1) / lanes
        val seconds = planned * avgPause / lanes + (accountsPerLane - 1).coerceAtLeast(0) * avgAccountPause
        return planned to seconds.toLong()
    }

    fun durationText(seconds: Long): String = when {
        seconds < 90 -> "$seconds сек"
        seconds < 5400 -> "${(seconds + 30) / 60} мин"
        else -> "%.1f ч".format(seconds / 3600.0)
    }

    private fun smsAccount(slot: Int): SmsAccount {
        val user = GgtgAccounts.user(slot)
        val name = user?.let { org.telegram.messenger.UserObject.getUserName(it).trim() }.orEmpty()
        val parts = ArrayList<String>()
        user?.phone?.takeIf { it.isNotEmpty() }?.let { parts.add("+$it") }
        user?.username?.takeIf { it.isNotEmpty() }?.let { parts.add("@$it") }
        return SmsAccount(slot, name.ifEmpty { GgtgAccounts.label(slot) }, parts.joinToString(" · "))
    }

    /** Opens the live window of the running (or last) campaign from wherever the app is. */
    fun openRunWindow() {
        org.telegram.ui.LaunchActivity.getLastFragment()?.presentFragment(GgtgSmsRunActivity())
    }

    suspend fun run(config: Config) = coroutineScope {
        cancelled = false
        // whom GGTG Desktop already wrote to: taken in first, so they are not written to from here as well
        GgtgSyncTransport.syncBeforeMailing()
        // Accounts @spambot already called limited are not used: each would spend a recipient on its
        // first PEER_FLOOD.
        val limited = config.slots.filter { GgtgSpamStatus.isBlocked(UserConfig.getInstance(it).clientUserId) }
        val slots = config.slots - limited.toSet()
        GgtgAccounts.wake(slots)
        val process = GgtgProcess.start("Рассылка SMS (${config.usernames.size})", GgtgProcess.Kind.Sms) { openRunWindow() }
        if (limited.isNotEmpty()) {
            process.log("Пропущены аккаунты со спам-блоком: " + limited.joinToString { GgtgAccounts.label(it) })
        }
        if (slots.isEmpty()) {
            process.fail("Все выбранные аккаунты со спам-блоком: писать некому")
            return@coroutineScope
        }
        val total = config.usernames.size
        // ng: "цель" (the denominator) is the campaign's realistic capacity -
        // chosen accounts times messages per account - not the raw recipient
        // list size. Quota 0 = unlimited, so the whole list is the goal then;
        // otherwise it is capped by the list (never send more than recipients).
        val queue = ArrayDeque(config.usernames.filter { GgtgDb.findSmsRecord(it) == null && !GgtgDb.isBotUsername(it) })
        val goal = if (config.perAccountQuota > 0)
            minOf(slots.size * config.perAccountQuota, queue.size) else queue.size
        val lanes = config.threads.coerceIn(1, maxOf(1, slots.size))
        val run = SmsRun(config.usernames, slots.map { smsAccount(it) }, lanes, goal)
        run.earlier = total - queue.size
        SmsRun.current = run
        // the live window opens over the form the campaign was started from
        org.telegram.messenger.AndroidUtilities.runOnUIThread { openRunWindow() }
        var templateIndex = 0
        val nextAccount = AtomicInteger(0)

        val paceMultiplier = if (config.protectionMode == 0) 1.5 else if (config.protectionMode == 2) 0.7 else 1.0
        val effectivePauseMin = (config.pauseMinMs * paceMultiplier).toLong()
        val effectivePauseMax = (config.pauseMaxMs * paceMultiplier).toLong()
        val effectiveAccountPauseMin = (config.pauseAccountMinMs * paceMultiplier).toLong()
        val effectiveAccountPauseMax = (config.pauseAccountMaxMs * paceMultiplier).toLong()

        val jobs = (0 until lanes).map { lane ->
            launch {
                var account = -1
                var laneSuccesses = 0
                var errorStreak = 0
                var notFoundStreak = 0
                var floodCount = 0
                // PEER_FLOOD refusals of the current account that may turn out to be its own spam block
                var peerStreak = 0
                val peerLimited = ArrayList<Pair<String, Long>>()
                val notFoundPending = ArrayList<Pair<String, Long>>()
                fun confirmNotFound() {
                    for ((name, base) in notFoundPending) {
                        GgtgDb.markSmsStatus(name, GgtgDb.SmsStatus.NotFound, GgtgAccounts.label(account), base)
                    }
                    notFoundPending.clear()
                }
                // takes the account out of this campaign: [state] and [reason] stay on its row
                fun retire(state: SmsAccountState, reason: String) {
                    if (state == SmsAccountState.Done) {
                        confirmNotFound()
                    }
                    notFoundPending.clear()
                    run.setAccount(account, state, reason)
                    run.setLive(lane, -1, "")
                    process.log("${GgtgAccounts.label(account)} ${if (state == SmsAccountState.Done) "отработал" else "снят"}: $reason")
                    account = -1
                }
                while (!cancelled) {
                    if (account < 0) {
                        val idx = nextAccount.getAndIncrement()
                        if (idx >= slots.size) break
                        val candidate = slots[idx]
                        if (config.filterByGgr) {
                            val ggr = GgtgSpamStatus.calculateGgr(candidate)
                            if (ggr.score < 7.0f) {
                                run.setAccount(candidate, SmsAccountState.Retired, "GGR ${String.format(java.util.Locale.US, "%.1f", ggr.score)} < 7.0: отсеян")
                                process.log("${GgtgAccounts.label(candidate)}: отсеян фильтром траста GGR (${String.format(java.util.Locale.US, "%.1f", ggr.score)})")
                                continue
                            }
                        }
                        val (sleeping, wakeHour) = isAccountSleeping(candidate, config.protectionMode, config.nightSleep)
                        if (sleeping) {
                            run.setAccount(candidate, SmsAccountState.Waiting, "спит до $wakeHour:00 (по гео)")
                            process.log("${GgtgAccounts.label(candidate)}: спит до $wakeHour:00 (ночной сон по гео)")
                            continue
                        }
                        account = candidate
                        laneSuccesses = 0; errorStreak = 0; notFoundStreak = 0; floodCount = 0
                        peerStreak = 0; peerLimited.clear()
                        notFoundPending.clear()
                        if (config.dailyLimit > 0 && daySent(account) >= config.dailyLimit) {
                            run.setAccount(account, SmsAccountState.Done, "дневной лимит уже исчерпан")
                            account = -1; continue
                        }
                        run.setAccount(account, SmsAccountState.Working, "")
                        process.log("Поток ${lane + 1}: ${GgtgAccounts.label(account)}")
                    } else {
                        val (sleeping, wakeHour) = isAccountSleeping(account, config.protectionMode, config.nightSleep)
                        if (sleeping) {
                            run.setAccount(account, SmsAccountState.Waiting, "спит до $wakeHour:00 (по гео)")
                            process.log("${GgtgAccounts.label(account)}: спит до $wakeHour:00 (ночной сон по гео)")
                            account = -1
                            continue
                        }
                    }
                    val username = synchronized(queue) { queue.removeFirstOrNull() } ?: break
                    val i = run.indexOf(username)
                    run.setInProgress(i, account)
                    val template = synchronized(this@GgtgSms) { config.templates[templateIndex++ % config.templates.size] }
                    var quotaReached = false
                    // "Поток 1: Аккаунт: ищет @user → открывает профиль → ...": the steps taken with
                    // this recipient so far, ending with what the account is doing right now.
                    val trail = ArrayList<String>()
                    val laneLabel = GgtgAccounts.label(account)
                    val laneSlot = account
                    val baseId = config.baseOf[username.lowercase()] ?: 0
                    val (outcome, message) = sendOne(
                        account,
                        username,
                        template,
                        config.greetingSticker,
                        groupNameOfBase(baseId),
                        config.protectionMode,
                        config.simulateTypos,
                        config.skipSuspectedFakes,
                        config.translate,
                        config.translateLang
                    ) { stepName ->
                        trail.add(stepName)
                        run.setLive(lane, laneSlot, stepName)
                        process.setLive(lane, "Поток ${lane + 1}: $laneLabel: ${trail.joinToString(" → ")}")
                    }
                    val slot = account
                    var reason = ""
                    when (outcome) {
                        Outcome.Sent -> {
                            peerStreak = 0; peerLimited.clear()
                            confirmNotFound()
                            GgtgDb.markSmsSent(username, GgtgAccounts.label(account), baseId)
                            run.settle(i, lane, SmsOutcome.Sent, "", slot)
                            laneSuccesses++; errorStreak = 0; notFoundStreak = 0
                            registerDaySent(account)
                            if (config.perAccountQuota in 1..laneSuccesses) { retire(SmsAccountState.Done, "лимит ${config.perAccountQuota} сообщений"); quotaReached = true }
                            else if (config.dailyLimit > 0 && daySent(account) >= config.dailyLimit) retire(SmsAccountState.Done, "дневной лимит ${config.dailyLimit}")
                        }
                        Outcome.SpamBlock -> {
                            notFoundStreak = 0
                            // PEER_FLOOD: whose fault? If @spambot already called the account limited, the
                            // account's; if not, Telegram just refused this pair and the account stays in the game.
                            val userId = UserConfig.getInstance(account).clientUserId
                            val ownLabel = GgtgAccounts.label(account)
                            var accountBlocked = GgtgSpamStatus.isBlocked(userId)
                            GgtgDb.markSmsStatus(username, if (accountBlocked) GgtgDb.SmsStatus.SpamAcc else GgtgDb.SmsStatus.PeerLimit, ownLabel, baseId)
                            run.settle(i, lane, if (accountBlocked) SmsOutcome.Spam else SmsOutcome.Peer, "", slot)
                            if (!accountBlocked) {
                                peerLimited.add(username to baseId)
                                peerStreak++
                                val known = GgtgSpamStatus.find(userId)
                                val fresh = known != null && System.currentTimeMillis() / 1000 - known.checkedAt < VERDICT_FRESH_SEC
                                var asked = false
                                if (!fresh) {
                                    process.setLive(lane, "Поток ${lane + 1}: $ownLabel: спрашивает @spambot")
                                    run.setLive(lane, laneSlot, "спрашивает @spambot")
                                    asked = GgtgSpamStatus.check(account) == null
                                    accountBlocked = asked && GgtgSpamStatus.isBlocked(userId)
                                }
                                if (accountBlocked) {
                                    // the refusals were the account's own limit
                                    for ((name, base) in peerLimited) GgtgDb.markSmsStatus(name, GgtgDb.SmsStatus.SpamAcc, ownLabel, base)
                                    run.peerToSpam(peerLimited.map { run.indexOf(it.first) }.filter { it >= 0 }, slot)
                                } else if (peerStreak >= PEER_REFUSAL_STREAK) {
                                    retire(SmsAccountState.Retired, "Telegram $peerStreak раз подряд не пустил к получателям")
                                }
                            }
                            if (accountBlocked) {
                                peerLimited.clear()
                                retire(SmsAccountState.SpamBlocked, "@spambot: аккаунт ограничен")
                            }
                        }
                        Outcome.AccountDead -> {
                            notFoundPending.clear()
                            synchronized(queue) { queue.addLast(username) }
                            run.requeue(i)
                            retire(SmsAccountState.Retired, "сессия недействительна ($message)")
                        }
                        Outcome.FloodWait -> {
                            notFoundPending.clear()
                            synchronized(queue) { queue.addLast(username) }
                            run.requeue(i)
                            val secs = floodWaitSeconds(message)
                            if (++floodCount >= 2 || secs > MAX_FLOOD_WAIT) retire(SmsAccountState.Retired, "FLOOD_WAIT $secs сек")
                            else delay((secs + 5) * 1000L)
                        }
                        Outcome.NotFound -> {
                            notFoundPending.add(username to baseId)
                            if (++notFoundStreak >= NOT_FOUND_STREAK) {
                                val req = TLRPC.TL_contacts_resolveUsername().apply { this.username = "telegram" }
                                val pair = GgtgAccounts.request(account, req)
                                val canaryOk = pair?.first is TLRPC.TL_contacts_resolvedPeer
                                if (canaryOk) {
                                    confirmNotFound()
                                    notFoundStreak = 0
                                    process.log("${GgtgAccounts.label(account)}: $NOT_FOUND_STREAK раз «не найден», проверка поиска (@telegram) успешна — поиск работает, юзернеймов действительно нет")
                                    run.settle(i, lane, SmsOutcome.NotFound, "", slot)
                                } else {
                                    notFoundPending.clear()
                                    run.settle(i, lane, SmsOutcome.Fail, "поиск ограничен у ${GgtgAccounts.label(account)}", slot)
                                    retire(SmsAccountState.Retired, "$NOT_FOUND_STREAK раз подряд «не найден»: Telegram ограничил поиск")
                                }
                            } else {
                                run.settle(i, lane, SmsOutcome.NotFound, "", slot)
                            }
                        }
                        Outcome.Skip -> {
                            confirmNotFound()
                            notFoundStreak = 0
                            if (message.contains("фейк")) {
                                GgtgDb.markSmsStatus(username, GgtgDb.SmsStatus.SuspectedFake, GgtgAccounts.label(account), baseId)
                            } else if (message.contains("давно")) {
                                GgtgDb.markSmsStatus(username, GgtgDb.SmsStatus.LongAgo, GgtgAccounts.label(account), baseId)
                                if (baseId != 0L) {
                                    GgtgDb.pruneFromBase(baseId, username)
                                }
                                GgtgDb.pruneFromAllBases(username)
                            }
                            run.settle(i, lane, SmsOutcome.Fail, message, slot)
                        }
                        Outcome.Error -> if (message == TIMEOUT_RESOLVE) {
                            // nothing was sent and nothing is known about the recipient: try again later
                            synchronized(queue) { queue.addLast(username) }
                            run.requeue(i)
                            if (++errorStreak >= ERROR_STREAK) retire(SmsAccountState.Retired, "нет ответа от сервера")
                        } else {
                            confirmNotFound()
                            notFoundStreak = 0
                            // the sticker is already out when the text then fails: say so, the recipient did get something
                            val stickerOnly = trail.contains("отправил привет-стикер")
                            reason = (if (stickerOnly) "стикер ушёл, текст нет: " else "") + humanError(message)
                            if (message in recipientOnly) {
                                GgtgDb.markSmsStatus(username, GgtgDb.SmsStatus.Restricted, GgtgAccounts.label(account), baseId)
                                run.settle(i, lane, SmsOutcome.Closed, reason, slot)
                            } else {
                                run.settle(i, lane, SmsOutcome.Fail, reason, slot)
                                if (++errorStreak >= ERROR_STREAK) retire(SmsAccountState.Retired, "$ERROR_STREAK сбоев подряд")
                            }
                        }
                    }
                    process.progress(run.processed(), goal)
                    val last = run.recipients[i]
                    process.log("@$username — ${last.outcome?.title ?: "снова в очереди"}${if (last.reason.isEmpty()) "" else " (${last.reason})"}")
                    // Same account again -> inter-message pause; account just
                    // finished its quota -> the longer "Пауза между аккаунтами"
                    // before the next account picks up; anything else -> move on.
                    val pause = when {
                        outcome == Outcome.Sent && account >= 0 ->
                            effectivePauseMin + Random.nextLong(maxOf(1L, effectivePauseMax - effectivePauseMin + 1))
                        quotaReached ->
                            effectiveAccountPauseMin + Random.nextLong(maxOf(1L, effectiveAccountPauseMax - effectiveAccountPauseMin + 1))
                        else -> SKIP_PAUSE
                    }
                    process.setLive(lane, "Поток ${lane + 1}: $laneLabel: пауза ${pause / 1000} сек")
                    run.setLive(lane, if (account >= 0) account else -1, "пауза ${pause / 1000} сек")
                    delay(pause)
                }
                // the lane is done: the account it still holds finished its share (or was stopped with it)
                if (account >= 0) {
                    confirmNotFound()
                    run.setAccount(account, SmsAccountState.Done, if (cancelled) "рассылка остановлена" else "получатели кончились")
                }
                run.setLive(lane, -1, "")
                process.clearLive(lane)
            }
        }
        jobs.forEach { it.join() }
        GgtgDb.flushSmsCache()
        GgtgSyncTransport.syncSoon()
        val left = synchronized(queue) { queue.size }
        run.finish(cancelled, left)
        process.succeed(run.phase.title + ". " + run.finishLine())
    }

    // ---- per-day sent counter (survives restart), keyed by userId + date ----

    // A rolling 24 hours: the send times of each account (user id -> epoch millis, comma separated), kept
    // in preferences so it survives a restart. Only the last 24 hours are kept.
    private val dayPrefs get() = org.telegram.messenger.ApplicationLoader.applicationContext
        .getSharedPreferences("ggtg_sms_daily", android.content.Context.MODE_PRIVATE)
    private const val DAY_MS = 24L * 60 * 60 * 1000
    private fun userKey(slot: Int) = (GgtgAccounts.user(slot)?.id ?: slot.toLong()).toString()

    private fun recentSends(slot: Int): List<Long> {
        val saved = dayPrefs.getString(userKey(slot), null) ?: return emptyList()
        val since = System.currentTimeMillis() - DAY_MS
        // an older "day:count" value (from before the rolling counter) has no millis list: ignore it
        return saved.split(",").mapNotNull { it.toLongOrNull() }.filter { it > since }
    }

    /** How many messages this account has sent in the last 24 hours ("SMS 24h" in the account lists). */
    fun sentToday(slot: Int): Int = synchronized(this) { recentSends(slot).size }

    private fun daySent(slot: Int): Int = sentToday(slot)

    fun registerDaySent(slot: Int) {
        synchronized(this) {
            val list = recentSends(slot) + System.currentTimeMillis()
            dayPrefs.edit().putString(userKey(slot), list.joinToString(",")).apply()
        }
    }
}
