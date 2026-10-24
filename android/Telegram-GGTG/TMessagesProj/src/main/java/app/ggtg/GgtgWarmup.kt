package app.ggtg

import android.content.Context
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import org.telegram.messenger.ApplicationLoader
import org.telegram.messenger.MessagesController
import org.telegram.messenger.UserConfig
import org.telegram.tgnet.TLRPC
import org.telegram.tgnet.tl.TL_update
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.concurrent.atomic.AtomicBoolean
import kotlin.random.Random

typealias WarmupPreset = GgtgWarmup.WarmupMode

// GGTG: Smart Account Warmup (Прогрев аккаунтов) for Android
// 2 Modes:
// 1. Browsing International Channels & Groups (human pacing, reading posts, reactions, joining)
// 2. Inter-Account Mutual Conversation (natural English dialogue, typing simulation, typos, reactions)
object GgtgWarmup {

    enum class WarmupMode {
        ChannelsAndGroups, // Mode 1: Просмотр зарубежных каналов и групп
        MutualDialogue     // Mode 2: Взаимная переписка между аккаунтами (English)
    }

    data class WarmupConfig(
        var mode: WarmupMode = WarmupMode.ChannelsAndGroups,
        var preset: WarmupMode = WarmupMode.ChannelsAndGroups,
        var threads: Int = 2,
        var delayMinSec: Int = 4,
        var delayMaxSec: Int = 8,
        var joinChannels: Boolean = true,
        var maxJoinsPerAccount: Int = 2,
        var readPostsPerChannel: Int = 15,
        var enableReactions: Boolean = true,
        var reactionChance: Int = 70, // %
        var useSafeCatalog: Boolean = true,
        var customChannels: String = "",
        var dialogueTurns: Int = 2,
        var enableDialogueReactions: Boolean = true
    )

    data class WarmupStats(
        val trustScore: Int = 25,
        val lastWarmup: String = "",
        val postsRead: Int = 0,
        val reactionsGiven: Int = 0,
        val pollsVoted: Int = 0,
        val p2pDialogues: Int = 0,
        val channelsJoined: Int = 0
    )

    fun configForMode(mode: WarmupMode): WarmupConfig {
        return WarmupConfig(mode = mode, preset = mode)
    }

    fun configForPreset(preset: WarmupPreset): WarmupConfig = configForMode(preset)

    // -------------------------------------------------------------------------
    // Safe Whitelist Catalog: Strictly International (NO CIS / NO Russian)
    // -------------------------------------------------------------------------

    val safeChannelsEn = listOf(
        "techcrunch",
        "nature",
        "bbcearth",
        "theeconomist",
        "bloomberg",
        "discovery",
        "sciencedaily",
        "worldnews",
        "wired",
        "nationalgeographic",
        "theverge",
        "nasa",
        "tedtalks"
    )

    // -------------------------------------------------------------------------
    // Natural English Dialogue Scenarios (Mode 2)
    // -------------------------------------------------------------------------

    data class DialogueTurn(
        val textA: String,
        val textB: String,
        val reactionA: String? = null,
        val reactionB: String? = null
    )

    data class DialogueScript(
        val turns: List<DialogueTurn>
    )

    val naturalDialogueScripts = listOf(
        // Script 1: Work & Coffee Break
        DialogueScript(listOf(
            DialogueTurn(
                "Hey! How is your day going? 😊",
                "Hey there! Pretty busy, just finishing up a few tasks. How about you?",
                "☕", "👍"
            ),
            DialogueTurn(
                "Same here, had quite a few meetings today! Taking a quick coffee break now ☕",
                "Haha sounds familiar! Enjoy your coffee, talk to you in a bit 👌",
                null, "👍"
            )
        )),
        // Script 2: Weekend & Outdoor Plans
        DialogueScript(listOf(
            DialogueTurn(
                "Hello! Any fun plans for this weekend?",
                "Hey! Thinking of heading out for a short road trip if the weather stays nice 🚗 You?",
                "❤️", "✨"
            ),
            DialogueTurn(
                "That sounds awesome! Just planning to chill at home and catch up on some reading 📚",
                "Nice, relaxing weekends are the best! Have a great one! 😊",
                null, "🎉"
            )
        )),
        // Script 3: Tech & News Updates
        DialogueScript(listOf(
            DialogueTurn(
                "Hey mate, did you see the latest tech news today?",
                "Hi! Not yet, was heads down in work. Anything interesting happen?",
                "🔥", "👍"
            ),
            DialogueTurn(
                "Yeah, some pretty cool AI updates and hardware announcements released this morning 🚀",
                "Oh nice, thanks for the heads up! Will definitely check it out tonight 👌",
                null, "🔥"
            )
        )),
        // Script 4: Project Collaboration & Check-in
        DialogueScript(listOf(
            DialogueTurn(
                "Good afternoon! Hope you're having a productive week 💪",
                "Thanks! Going well so far, trying to stay on top of everything. Hope all is good on your end too!",
                "👏", "👍"
            ),
            DialogueTurn(
                "All good here, thank you! Let me know if you need any help with the shared docs.",
                "Appreciate it a lot! Will ping you if anything comes up 😊",
                null, "👌"
            )
        )),
        // Script 5: Hobbies & Photography
        DialogueScript(listOf(
            DialogueTurn(
                "Hey! Have you had a chance to try out that new camera feature yet?",
                "Hey! Yes, took a few test shots yesterday. The quality is honestly impressive 📸",
                "🔥", "✨"
            ),
            DialogueTurn(
                "Awesome, can't wait to see the shots! Catch you later 👍",
                "Will share some soon! Have a great afternoon 😊",
                null, "👍"
            )
        ))
    )

    val allowedReactions = listOf("👍", "🔥", "❤️", "👏", "🎉")

    // -------------------------------------------------------------------------
    // Trust Score & History Persistence
    // -------------------------------------------------------------------------

    private fun prefs() = ApplicationLoader.applicationContext
        .getSharedPreferences("ggtg_warmup_history", Context.MODE_PRIVATE)

    fun getStats(slot: Int): WarmupStats {
        val user = UserConfig.getInstance(slot)?.currentUser
        val phone = user?.phone?.ifEmpty { null } ?: user?.id?.toString() ?: slot.toString()
        val p = prefs()
        val score = p.getInt("score_$phone", 25)
        val last = p.getString("last_$phone", "") ?: ""
        val posts = p.getInt("posts_$phone", 0)
        val reacts = p.getInt("reacts_$phone", 0)
        val polls = p.getInt("polls_$phone", 0)
        val p2p = p.getInt("p2p_$phone", 0)
        val joins = p.getInt("joins_$phone", 0)
        return WarmupStats(score, last, posts, reacts, polls, p2p, joins)
    }

    fun updateStats(
        slot: Int,
        postsDelta: Int,
        reactsDelta: Int,
        pollsDelta: Int,
        p2pDelta: Int,
        joinsDelta: Int
    ): WarmupStats {
        val user = UserConfig.getInstance(slot)?.currentUser
        val phone = user?.phone?.ifEmpty { null } ?: user?.id?.toString() ?: slot.toString()
        val current = getStats(slot)
        val newPosts = current.postsRead + postsDelta
        val newReacts = current.reactionsGiven + reactsDelta
        val newPolls = current.pollsVoted + pollsDelta
        val newP2p = current.p2pDialogues + p2pDelta
        val newJoins = current.channelsJoined + joinsDelta
        val dateStr = SimpleDateFormat("dd.MM.yyyy HH:mm", Locale.getDefault()).format(Date())

        val calculated = 25 +
            minOf(30, newPosts / 2) +
            minOf(15, newReacts * 2) +
            minOf(10, newPolls * 3) +
            minOf(20, newP2p * 5) +
            minOf(10, newJoins * 3)
        val newScore = calculated.coerceIn(10, 100)

        prefs().edit().apply {
            putInt("score_$phone", newScore)
            putString("last_$phone", dateStr)
            putInt("posts_$phone", newPosts)
            putInt("reacts_$phone", newReacts)
            putInt("polls_$phone", newPolls)
            putInt("p2p_$phone", newP2p)
            putInt("joins_$phone", newJoins)
            apply()
        }
        return WarmupStats(newScore, dateStr, newPosts, newReacts, newPolls, newP2p, newJoins)
    }

    // -------------------------------------------------------------------------
    // Runner Engine (Strictly 2 Modes, Step-by-Step Pacing, Safe Cancel)
    // -------------------------------------------------------------------------

    val cancelled = AtomicBoolean(false)

    suspend fun runWarmup(
        slots: List<Int>,
        config: WarmupConfig,
        onLog: (String) -> Unit = {}
    ) = coroutineScope {
        cancelled.set(false)
        val total = slots.size
        val modeName = if (config.mode == WarmupMode.MutualDialogue)
            "Взаимная переписка (English)"
        else
            "Просмотр каналов"

        val process = GgtgProcess.start(
            "Прогрев аккаунтов ($total акк., $modeName)",
            GgtgProcess.Kind.Warmup
        )
        process.progressTotal = total
        process.progressCurrent = 0

        fun log(msg: String) {
            val line = SimpleDateFormat("HH:mm:ss", Locale.getDefault()).format(Date()) + " | " + msg
            onLog(line)
            process.log(line)
        }

        log("=== Старт прогрева GGTG ($total аккаунтов, режим: $modeName) ===")

        if (config.mode == WarmupMode.MutualDialogue) {
            // Mode 2: Mutual Dialogue
            if (slots.size < 2) {
                log("Для взаимной переписки требуется как минимум 2 аккаунта.")
                process.fail("Недостаточно аккаунтов")
                return@coroutineScope
            }
            log("=== Запуск взаимных P2P диалогов на английском языке ===")
            for (i in 0 until slots.size - 1 step 2) {
                if (cancelled.get()) break
                val s1 = slots[i]
                val s2 = slots[i + 1]
                runP2pPair(s1, s2, config, ::log)
                val pairPause = Random.nextLong(4000L, 8000L)
                delay(pairPause)
            }
            if (slots.size % 2 == 1 && !cancelled.get()) {
                runP2pPair(slots.last(), slots.first(), config, ::log)
            }
        } else {
            // Mode 1: Channels and Groups
            for ((idx, slot) in slots.withIndex()) {
                if (cancelled.get()) {
                    log("Прогрев остановлен.")
                    process.fail("Остановлено пользователем")
                    return@coroutineScope
                }

                val user = UserConfig.getInstance(slot)?.currentUser
                val name = user?.let { "${it.first_name ?: ""} ${it.last_name ?: ""}".trim() }.orEmpty().ifEmpty { "Аккаунт $slot" }
                log("--- [$idx/$total] Прогрев аккаунта: $name ---")
                process.progressCurrent = idx + 1

                warmupSingleAccount(slot, config, ::log)

                val pause = Random.nextLong(config.delayMinSec * 1000L, config.delayMaxSec * 1000L + 1)
                delay(pause)
            }
        }

        if (cancelled.get()) {
            log("Прогрев остановлен пользователем.")
            process.fail("Остановлено пользователем")
        } else {
            log("✓ Прогрев успешно завершён. Траст всех аккаунтов повышен!")
            process.succeed("Успешно завершено")
        }
    }

    private suspend fun warmupSingleAccount(
        slot: Int,
        config: WarmupConfig,
        log: (String) -> Unit
    ) {
        val targets = ArrayList<String>()
        if (config.useSafeCatalog) {
            targets.addAll(safeChannelsEn)
        }
        if (config.customChannels.isNotBlank()) {
            targets.addAll(config.customChannels.split(Regex("[\\s,;\\n]+")).map { it.trim().removePrefix("@") }.filter { it.isNotEmpty() })
        }
        if (targets.isEmpty()) {
            targets.addAll(safeChannelsEn)
        }
        targets.shuffle()
        val countToVisit = minOf(targets.size, 2)

        var postsCount = 0
        var reactsCount = 0
        var joinsCount = 0

        for (i in 0 until countToVisit) {
            if (cancelled.get()) break
            val chUsername = targets[i]

            // Human pause before opening each channel (3-6s)
            val openDelay = Random.nextLong(3000L, 6000L)
            delay(openDelay)
            if (cancelled.get()) break

            log("Открытие канала @$chUsername...")

            val controller = MessagesController.getInstance(slot)
            val reqResolve = TLRPC.TL_contacts_resolveUsername().apply { username = chUsername }
            val pairResolve = GgtgAccounts.request(slot, reqResolve)
            if (pairResolve == null || pairResolve.first == null) {
                log("Канал @$chUsername недоступен, пауза...")
                delay(4000L)
                continue
            }
            val resp = pairResolve.first
            val peer: TLRPC.InputPeer? = if (resp is TLRPC.TL_contacts_resolvedPeer) {
                controller.putUsers(resp.users, false)
                controller.putChats(resp.chats, false)
                resp.chats.firstOrNull()?.let { controller.getInputPeer(-it.id) }
            } else null

            if (peer == null) {
                log("Канал @$chUsername не найден, переход к следующему...")
                delay(3000L)
                continue
            }

            // Inspect channel header (2-4s)
            delay(Random.nextLong(2000L, 4000L))

            // Join channel if enabled
            if (config.joinChannels && joinsCount < config.maxJoinsPerAccount && peer is TLRPC.TL_inputPeerChannel) {
                val joinReq = TLRPC.TL_channels_joinChannel().apply {
                    channel = controller.getInputChannel(peer.channel_id)
                }
                val joinRes = GgtgAccounts.request(slot, joinReq)
                if (joinRes?.second == null) {
                    log("✓ Подписка на канал @$chUsername")
                    joinsCount++
                    delay(Random.nextLong(2000L, 4000L))
                }
            }

            // Human reading pause (3-6s)
            delay(Random.nextLong(3000L, 6000L))

            // Fetch history
            val histReq = TLRPC.TL_messages_getHistory().apply {
                this.peer = peer
                offset_id = 0
                offset_date = 0
                add_offset = 0
                limit = config.readPostsPerChannel
                max_id = 0
                min_id = 0
                hash = 0
            }
            val histPair = GgtgAccounts.request(slot, histReq)
            val histResp = histPair?.first as? TLRPC.messages_Messages ?: continue
            val msgIds = histResp.messages.map { it.id }.filter { it > 0 }
            val maxMsgId = msgIds.maxOrNull() ?: 0

            // Read history
            if (maxMsgId > 0) {
                val readReq = TLRPC.TL_messages_readHistory().apply {
                    this.peer = peer
                    max_id = maxMsgId
                }
                GgtgAccounts.request(slot, readReq)
            }

            if (msgIds.isNotEmpty()) {
                val viewsReq = TLRPC.TL_messages_getMessagesViews().apply {
                    this.peer = peer
                    id.addAll(msgIds)
                    increment = true
                }
                GgtgAccounts.request(slot, viewsReq)
                postsCount += msgIds.size
                log("Просмотрено ${msgIds.size} постов в @$chUsername")
            }

            // Reaction with realistic pause
            if (config.enableReactions && maxMsgId > 0 && Random.nextInt(100) < config.reactionChance) {
                val reactPause = Random.nextLong(2000L, 4000L)
                delay(reactPause)

                val emoji = allowedReactions.random()
                val reactReq = TLRPC.TL_messages_sendReaction().apply {
                    this.peer = peer
                    msg_id = maxMsgId
                    val reactionEmoji = TLRPC.TL_reactionEmoji().apply { emoticon = emoji }
                    reaction.add(reactionEmoji)
                }
                GgtgAccounts.request(slot, reactReq)
                reactsCount++
                log("✓ Поставлена реакция $emoji в @$chUsername")
            }

            delay(Random.nextLong(config.delayMinSec * 1000L, config.delayMaxSec * 1000L + 1))
        }

        val updated = updateStats(slot, postsCount, reactsCount, 0, 0, joinsCount)
        log("✓ Аккаунт прогрет. Траст: ${updated.trustScore}% (Постов: ${updated.postsRead}, Реакций: ${updated.reactionsGiven}, Подписок: ${updated.channelsJoined})")
    }

    private suspend fun runP2pPair(slot1: Int, slot2: Int, config: WarmupConfig, log: (String) -> Unit) {
        val u1 = UserConfig.getInstance(slot1)?.currentUser ?: return
        val u2 = UserConfig.getInstance(slot2)?.currentUser ?: return
        val name1 = "${u1.first_name ?: ""} ${u1.last_name ?: ""}".trim().ifEmpty { "A1" }
        val name2 = "${u2.first_name ?: ""} ${u2.last_name ?: ""}".trim().ifEmpty { "A2" }
        log("--- Диалог: [$name1] ↔ [$name2] ---")

        val ctrl1 = MessagesController.getInstance(slot1)
        val ctrl2 = MessagesController.getInstance(slot2)
        val peer2For1 = ctrl1.getInputPeer(u2.id) ?: return
        val peer1For2 = ctrl2.getInputPeer(u1.id) ?: return

        // Step 1: Account 1 views Account 2 profile photo & bio
        log("[$name1] Просмотр профиля [$name2]...")
        val fullUserReq = TLRPC.TL_users_getFullUser().apply {
            id = ctrl1.getInputUser(u2.id)
        }
        GgtgAccounts.request(slot1, fullUserReq)
        delay(Random.nextLong(3000L, 5000L))
        if (cancelled.get()) return

        val script = naturalDialogueScripts.random()
        val turnsToRun = minOf(script.turns.size, config.dialogueTurns.coerceIn(1, 4))

        for (t in 0 until turnsToRun) {
            if (cancelled.get()) break
            val turn = script.turns[t]

            // 1. Account 1 simulates typing with human delays
            log("[$name1] набирает сообщение...")
            val typing1 = TLRPC.TL_messages_setTyping().apply {
                peer = peer2For1
                action = TLRPC.TL_sendMessageTypingAction()
            }
            GgtgAccounts.request(slot1, typing1)
            val typingDuration1 = Random.nextLong(3500L, 6000L)
            delay(typingDuration1)
            if (cancelled.get()) break

            // Send message from 1 to 2
            val sendReq1 = TLRPC.TL_messages_sendMessage().apply {
                peer = peer2For1
                message = turn.textA
                random_id = Random.nextLong()
            }
            val res1 = GgtgAccounts.request(slot1, sendReq1)
            var msg1Id = (res1?.first as? TLRPC.TL_updateShortSentMessage)?.id ?: 0
            if (msg1Id == 0 && res1?.first is TLRPC.Updates) {
                val upd = res1.first as TLRPC.Updates
                for (u in upd.updates) {
                    if (u is TL_update.TL_updateNewMessage) {
                        msg1Id = u.message.id
                    }
                }
            }
            log("[$name1] -> [$name2]: «${turn.textA}»")

            // 2. Account 2 reading delay (3-6s)
            val readPause = Random.nextLong(3000L, 6000L)
            delay(readPause)
            if (cancelled.get()) break

            // Optional reaction from B on A's message
            if (config.enableDialogueReactions && turn.reactionB != null && msg1Id > 0) {
                val reactReq = TLRPC.TL_messages_sendReaction().apply {
                    peer = peer1For2
                    msg_id = msg1Id
                    reaction.add(TLRPC.TL_reactionEmoji().apply { emoticon = turn.reactionB })
                }
                GgtgAccounts.request(slot2, reactReq)
                log("[$name2] поставил реакцию ${turn.reactionB} на сообщение")
            }

            // 3. Account 2 simulates typing reply (3-5s)
            log("[$name2] набирает ответ...")
            val typing2 = TLRPC.TL_messages_setTyping().apply {
                peer = peer1For2
                action = TLRPC.TL_sendMessageTypingAction()
            }
            GgtgAccounts.request(slot2, typing2)
            val typingDuration2 = Random.nextLong(3000L, 5500L)
            delay(typingDuration2)
            if (cancelled.get()) break

            // Send reply from 2 to 1
            val sendReq2 = TLRPC.TL_messages_sendMessage().apply {
                peer = peer1For2
                message = turn.textB
                random_id = Random.nextLong()
            }
            val res2 = GgtgAccounts.request(slot2, sendReq2)
            var msg2Id = (res2?.first as? TLRPC.TL_updateShortSentMessage)?.id ?: 0
            if (msg2Id == 0 && res2?.first is TLRPC.Updates) {
                val upd = res2.first as TLRPC.Updates
                for (u in upd.updates) {
                    if (u is TL_update.TL_updateNewMessage) {
                        msg2Id = u.message.id
                    }
                }
            }
            log("[$name2] -> [$name1]: «${turn.textB}»")

            // 4. Reading delay by A and optional reaction from A on B's reply
            val readPause2 = Random.nextLong(3000L, 5000L)
            delay(readPause2)
            if (cancelled.get()) break

            if (config.enableDialogueReactions && turn.reactionA != null && msg2Id > 0) {
                val reactReq2 = TLRPC.TL_messages_sendReaction().apply {
                    peer = peer2For1
                    msg_id = msg2Id
                    reaction.add(TLRPC.TL_reactionEmoji().apply { emoticon = turn.reactionA })
                }
                GgtgAccounts.request(slot1, reactReq2)
                log("[$name1] поставил реакцию ${turn.reactionA} на ответ")
            }

            updateStats(slot1, 0, 1, 0, 1, 0)
            updateStats(slot2, 0, 1, 0, 1, 0)

            val turnPause = Random.nextLong(4000L, 7000L)
            delay(turnPause)
        }
    }
}
