package app.ggtg

import kotlinx.coroutines.delay
import org.telegram.messenger.AccountInstance
import org.telegram.messenger.MessagesController
import org.telegram.messenger.UserConfig
import org.telegram.tgnet.ConnectionsManager
import org.telegram.tgnet.TLRPC

// ng: Kotlin port of the member/group-search parts of data/scraped_members_db.cpp, run on the
// current account. Scrapes a channel's participants (recent + optional letter search) into a
// users base, and searches public groups by keyword into a groups base. Progress and logs go to
// GgtgProcess; results are saved through GgtgDb.
object GgtgScraper {

    data class Options(
        val skipBots: Boolean = true,
        val skipAdmins: Boolean = true,
        val premiumOnly: Boolean = false,
        val searchByLetters: Boolean = false
    )

    private const val PER_PAGE = 200
    private const val PHASE_DEPTH = 10_000
    private const val HARD_CAP = 50_000

    private fun cm() = AccountInstance.getInstance(UserConfig.selectedAccount).connectionsManager
    private fun controller() = MessagesController.getInstance(UserConfig.selectedAccount)

    private fun floodWaitSeconds(type: String?): Int {
        if (type == null) return 0
        val m = Regex("^(?:FLOOD|FLOOD_PREMIUM|SLOWMODE)_WAIT_(\\d+)$").find(type) ?: return 0
        return maxOf(1, m.groupValues[1].toInt())
    }

    private suspend fun resolve(username: String): TLRPC.Chat? {
        val req = TLRPC.TL_contacts_resolveUsername().apply { this.username = username.removePrefix("@") }
        val pair = GgtgAccounts.request(UserConfig.selectedAccount, req,
            ConnectionsManager.RequestFlagFailOnServerErrors) ?: return null
        val resp = pair.first
        if (resp is TLRPC.TL_contacts_resolvedPeer) {
            controller().putUsers(resp.users, false)
            controller().putChats(resp.chats, false)
            return resp.chats.firstOrNull()
        }
        return null
    }

    private fun userAllowed(user: TLRPC.User?, options: Options): Boolean {
        if (user == null) return false
        if (user.self || user.username.isNullOrEmpty()) return false
        if (options.skipBots && (user.bot || GgtgDb.isBotUsername(user.username))) return false
        if (options.premiumOnly && !user.premium) return false
        return true
    }

    /** Scrapes participants of a public channel/group (by username) into a users base. */
    suspend fun scrapeMembers(username: String, options: Options) {
        val process = GgtgProcess.start("Сбор участников: @$username", GgtgProcess.Kind.Scrape)
        try {
            val chat = resolve(username)
            if (chat == null) {
                process.fail("Не найдено или это не группа")
                return
            }
            val inputChannel = TLRPC.TL_inputChannel().apply {
                channel_id = chat.id
                access_hash = chat.access_hash
            }
            val collected = LinkedHashSet<String>()
            val seen = HashSet<Long>()
            var offset = 0
            var floodRetries = 0
            while (collected.size < HARD_CAP) {
                val req = TLRPC.TL_channels_getParticipants().apply {
                    channel = inputChannel
                    filter = TLRPC.TL_channelParticipantsRecent()
                    this.offset = offset
                    limit = PER_PAGE
                    hash = 0
                }
                val pair = GgtgAccounts.request(UserConfig.selectedAccount, req)
                if (pair == null) { process.log("таймаут страницы"); break }
                val resp = pair.first
                val err = pair.second
                if (err != null) {
                    val wait = floodWaitSeconds(err.text)
                    if (wait in 1..300 && floodRetries++ < 5) {
                        process.log("FLOOD_WAIT $wait сек, ждём...")
                        delay((wait + 1) * 1000L); continue
                    }
                    process.log("Ошибка: ${err.text}")
                    break
                }
                if (resp !is TLRPC.TL_channels_channelParticipants) break
                controller().putUsers(resp.users, false)
                val userById = resp.users.associateBy { it.id }
                var added = 0
                for (p in resp.participants) {
                    val uid = participantUserId(p)
                    if (uid == 0L || !seen.add(uid)) continue
                    val user = userById[uid]
                    if (options.skipAdmins && isAdmin(p)) continue
                    if (userAllowed(user, options)) {
                        collected.add(user!!.username)
                        GgtgDb.rememberUser(user)
                        added++
                    }
                }
                offset += resp.participants.size
                process.progress(collected.size, maxOf(resp.count, collected.size))
                process.log("Участники: +$added (всего ${collected.size})")
                if (resp.participants.isEmpty() || offset >= PHASE_DEPTH) break
                delay(600 + (0..700).random().toLong())
            }
            if (collected.isEmpty()) {
                process.fail("Не найдено ни одного участника с юзернеймом")
                return
            }
            val title = "Участники: " + GgtgDb.groupBaseTitle(chat.title ?: username, username)
            GgtgDb.saveUsernames(GgtgDb.generateManualBaseId(), title, collected.toList())
            process.succeed("Собрано: ${collected.size}")
        } catch (t: Throwable) {
            process.fail("Ошибка: ${t.message}")
        }
    }

    private fun participantUserId(p: TLRPC.ChannelParticipant): Long =
        (p.peer as? TLRPC.TL_peerUser)?.user_id ?: p.user_id

    private fun isAdmin(p: TLRPC.ChannelParticipant): Boolean =
        p is TLRPC.TL_channelParticipantCreator || p is TLRPC.TL_channelParticipantAdmin

    /** Searches public groups by keyword into a groups base. */
    suspend fun searchGroups(keyword: String, minMembers: Int) {
        val process = GgtgProcess.start("Поиск групп: $keyword", GgtgProcess.Kind.Scrape)
        try {
            val req = TLRPC.TL_contacts_search().apply {
                q = keyword
                limit = 100
            }
            val (resp, err) = GgtgAccounts.request(UserConfig.selectedAccount, req) ?: (null to null)
            if (err != null || resp !is TLRPC.TL_contacts_found) {
                process.fail("Ошибка поиска: ${err?.text ?: "нет ответа"}")
                return
            }
            controller().putChats(resp.chats, false)
            val entries = ArrayList<GgtgDb.GroupEntry>()
            val seen = HashSet<String>()
            for (chat in resp.chats) {
                if (!chat.megagroup) continue
                val uname = chat.username ?: continue
                if (uname.isEmpty() || !seen.add(uname.lowercase())) continue
                if (minMembers > 0 && chat.participants_count in 1 until minMembers) continue
                entries.add(GgtgDb.GroupEntry(username = uname, title = chat.title ?: "", membersCount = chat.participants_count))
            }
            entries.sortByDescending { it.membersCount }
            if (entries.isEmpty()) {
                process.fail("Групп не найдено")
                return
            }
            GgtgDb.saveGroupEntries(GgtgDb.generateManualBaseId(), "Поиск: $keyword", entries)
            process.succeed("Найдено групп: ${entries.size}")
        } catch (t: Throwable) {
            process.fail("Ошибка: ${t.message}")
        }
    }
}
