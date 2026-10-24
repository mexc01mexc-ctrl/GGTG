package app.ggtg

import org.json.JSONArray
import org.json.JSONObject
import org.telegram.messenger.ApplicationLoader
import java.io.File
import java.util.concurrent.CopyOnWriteArrayList

// ng: Kotlin port of ggtg-macos' data/scraped_members_db.cpp - the "База" store shared by the
// SMS campaign, the member/group scraper and Squidward. One JSON file per base under
// filesDir/ggtg_bulk_db/, an index.json listing them, and sms_sent.json tracking who was already
// written to. Same on-disk shape as the desktop app so a base exported there can be dropped in
// here (and vice versa). All access is synchronized; callers do disk work off the main thread.
object GgtgDb {

    enum class BaseKind { Users, Groups }

    data class GroupMeta(
        val id: Long,
        val title: String,
        val count: Int,
        val savedAt: Long,
        val kind: BaseKind
    )

    // One row of a groups base: a public group (username) or a closed one (invite/c-link).
    data class GroupEntry(
        val username: String = "",
        val title: String = "",
        val membersCount: Int = 0,
        val link: String = "",
        val checked: Boolean = false
    )

    // PeerLimit: PEER_FLOOD while the account itself is not limited (@spambot says it is free) -
    // Telegram refused this particular pair; another account may still try the recipient.
    enum class SmsStatus { Sent, SpamAcc, NotFound, Restricted, PeerLimit, SuspectedFake }

    data class SmsRecord(
        val username: String, // normalized: no '@', lowercase
        val status: SmsStatus,
        val account: String,
        val at: Long,
        val baseId: Long,
        // the base's name at the time of the attempt: it stays when the base is renamed or deleted, so
        // "where did you get my contact?" can still be answered
        val source: String = ""
    )

    data class UserMeta(
        val username: String,
        val id: Long = 0,
        val name: String = "",
        val lastSeenTime: Long = 0,
        val lastSeenText: String = ""
    )

    private val lock = Any()
    private val listeners = CopyOnWriteArrayList<() -> Unit>()

    fun dir(): File = File(ApplicationLoader.getFilesDirFixed(), "ggtg_bulk_db").also { it.mkdirs() }
    private fun indexPath() = File(dir(), "index.json")
    private fun groupPath(id: Long) = File(dir(), "$id.json")
    private fun smsPath() = File(dir(), "sms_sent.json")
    private fun usersMetaPath() = File(dir(), "users_meta.json")

    private var usersMetaCache: HashMap<String, UserMeta>? = null

    private fun loadUsersMeta(): HashMap<String, UserMeta> = synchronized(lock) {
        usersMetaCache?.let { return it }
        val map = HashMap<String, UserMeta>()
        val text = readJson(usersMetaPath())
        if (!text.isNullOrEmpty()) {
            try {
                val obj = JSONObject(text)
                val keys = obj.keys()
                while (keys.hasNext()) {
                    val k = keys.next()
                    val u = obj.optJSONObject(k) ?: continue
                    map[k] = UserMeta(
                        username = k,
                        id = u.optLong("id", 0L),
                        name = u.optString("name", ""),
                        lastSeenTime = u.optLong("lastseen", 0L),
                        lastSeenText = u.optString("lastseenText", "")
                    )
                }
            } catch (_: Throwable) {}
        }
        usersMetaCache = map
        map
    }

    private fun saveUsersMeta(map: HashMap<String, UserMeta>) {
        val obj = JSONObject()
        for ((k, v) in map) {
            val u = JSONObject().apply {
                put("name", v.name)
                put("id", v.id)
                put("lastseen", v.lastSeenTime)
                put("lastseenText", v.lastSeenText)
            }
            obj.put(k, u)
        }
        writeJson(usersMetaPath(), obj.toString())
    }

    fun lookupUser(username: String): UserMeta? = synchronized(lock) {
        val clean = username.trim().removePrefix("@").lowercase()
        loadUsersMeta()[clean]
    }

    fun rememberUser(user: org.telegram.tgnet.TLRPC.User) = synchronized(lock) {
        val u = user.username?.lowercase() ?: return
        if (u.isEmpty()) return
        val map = loadUsersMeta()
        val name = org.telegram.messenger.ContactsController.formatName(user.first_name, user.last_name)
        val now = System.currentTimeMillis() / 1000
        val lastSeen = when (val s = user.status) {
            is org.telegram.tgnet.TLRPC.TL_userStatusOnline -> now + 86400
            is org.telegram.tgnet.TLRPC.TL_userStatusRecently -> now - 3600
            is org.telegram.tgnet.TLRPC.TL_userStatusLastWeek -> now - 86400 * 3
            is org.telegram.tgnet.TLRPC.TL_userStatusLastMonth -> now - 86400 * 15
            is org.telegram.tgnet.TLRPC.TL_userStatusOffline -> s.expires.toLong()
            else -> 0L
        }
        val text = org.telegram.messenger.LocaleController.formatUserStatus(org.telegram.messenger.UserConfig.selectedAccount, user)
        map[u] = UserMeta(
            username = u,
            id = user.id,
            name = name,
            lastSeenTime = lastSeen,
            lastSeenText = text
        )
        saveUsersMeta(map)
    }

    fun addListener(l: () -> Unit) { listeners.add(l) }
    fun removeListener(l: () -> Unit) { listeners.remove(l) }
    private fun fireChanged() { listeners.forEach { runCatching { it() } } }

    private fun readJson(file: File): String? = try {
        if (file.exists()) file.readText() else null
    } catch (t: Throwable) { null }

    private fun writeJson(file: File, text: String) {
        val tmp = File(file.parentFile, file.name + ".tmp")
        tmp.writeText(text)
        if (!tmp.renameTo(file)) {
            file.writeText(text)
            tmp.delete()
        }
    }

    // ---- index ----

    // Base ids are unsigned 64-bit numbers on the desktop (a hand-made base has the top bit set), which
    // are negative as a Long here: the files and the sync carry them as unsigned decimal text.
    fun idToString(id: Long): String = java.lang.Long.toUnsignedString(id)
    fun idFromString(raw: String?): Long? = raw?.toULongOrNull()?.toLong() ?: raw?.toLongOrNull()

    private fun kindToString(kind: BaseKind) = if (kind == BaseKind.Groups) "groups" else "users"
    private fun kindFromString(raw: String?) = if (raw == "groups") BaseKind.Groups else BaseKind.Users

    fun loadIndex(): List<GroupMeta> = synchronized(lock) {
        val text = readJson(indexPath()) ?: return emptyList()
        return try {
            val array = JSONArray(text)
            (0 until array.length()).mapNotNull { i ->
                val o = array.getJSONObject(i)
                val id = idFromString(o.optString("id")) ?: return@mapNotNull null
                GroupMeta(
                    id = id,
                    title = o.optString("title"),
                    count = o.optInt("count"),
                    savedAt = o.optString("savedAt").toLongOrNull() ?: 0,
                    kind = kindFromString(o.optString("kind"))
                )
            }
        } catch (t: Throwable) { emptyList() }
    }

    private fun writeIndex(index: List<GroupMeta>) {
        titleCache = null
        val array = JSONArray()
        for (e in index) {
            array.put(JSONObject().apply {
                put("id", idToString(e.id))
                put("title", e.title)
                put("count", e.count)
                put("savedAt", e.savedAt.toString())
                put("kind", kindToString(e.kind))
            })
        }
        writeJson(indexPath(), array.toString())
        fireChanged()
    }

    private fun appendToIndex(id: Long, title: String, count: Int, kind: BaseKind) {
        val index = loadIndex().toMutableList()
        val now = System.currentTimeMillis() / 1000
        val at = index.indexOfFirst { it.id == id }
        if (at >= 0) {
            index[at] = index[at].copy(title = title, count = count, savedAt = now)
        } else {
            index.add(GroupMeta(id, title, count, now, kind))
        }
        writeIndex(index)
    }

    // ---- username bases ----

    fun loadUsernames(id: Long): List<String> = synchronized(lock) {
        val text = readJson(groupPath(id)) ?: return emptyList()
        return try {
            val array = JSONArray(text)
            (0 until array.length()).mapNotNull { array.optString(it).takeIf { s -> s.isNotEmpty() } }
        } catch (t: Throwable) { emptyList() }
    }

    fun saveUsernames(id: Long, title: String, usernames: List<String>, crossDedupe: Boolean = true) = synchronized(lock) {
        val seen = HashSet<String>()
        val normalized = ArrayList<String>()
        for (raw in usernames) {
            val name = raw.trim().removePrefix("@")
            if (name.isEmpty() || !seen.add(name.lowercase())) continue
            normalized.add(name)
        }
        if (crossDedupe) {
            val others = namesInOtherBases(id, BaseKind.Users)
            normalized.removeAll { others.contains(it.lowercase()) }
        }
        val array = JSONArray()
        normalized.forEach { array.put(it) }
        writeJson(groupPath(id), array.toString())
        statsGeneration++
        appendToIndex(id, title, normalized.size, BaseKind.Users)
    }

    // ---- group-entry bases ----

    fun loadGroupEntries(id: Long): List<GroupEntry> = synchronized(lock) {
        val text = readJson(groupPath(id)) ?: return emptyList()
        return try {
            val array = JSONArray(text)
            (0 until array.length()).mapNotNull { i ->
                val o = array.optJSONObject(i) ?: return@mapNotNull null
                val entry = GroupEntry(
                    username = o.optString("username"),
                    title = o.optString("title"),
                    membersCount = o.optInt("membersCount"),
                    link = o.optString("link"),
                    checked = o.optBoolean("checked")
                )
                if (entry.username.isEmpty() && entry.title.isEmpty() && entry.link.isEmpty()) null else entry
            }
        } catch (t: Throwable) { emptyList() }
    }

    fun saveGroupEntries(id: Long, title: String, entries: List<GroupEntry>) = synchronized(lock) {
        val seen = HashSet<String>()
        val normalized = ArrayList<GroupEntry>()
        for (raw in entries) {
            val entry = raw.copy(username = raw.username.removePrefix("@"))
            if (entry.username.isEmpty() && entry.title.isEmpty() && entry.link.isEmpty()) continue
            if (entry.username.isNotEmpty() && !seen.add(entry.username.lowercase())) continue
            if (entry.username.isEmpty() && entry.link.isNotEmpty() && !seen.add(entry.link.lowercase())) continue
            normalized.add(entry)
        }
        val others = namesInOtherBases(id, BaseKind.Groups)
        normalized.removeAll { it.username.isNotEmpty() && others.contains(it.username.lowercase()) }
        val array = JSONArray()
        for (e in normalized) {
            array.put(JSONObject().apply {
                put("username", e.username)
                put("title", e.title)
                put("membersCount", e.membersCount)
                put("link", e.link)
                put("checked", e.checked)
            })
        }
        writeJson(groupPath(id), array.toString())
        appendToIndex(id, title, normalized.size, BaseKind.Groups)
    }

    private fun namesInOtherBases(exceptId: Long, kind: BaseKind): Set<String> {
        val result = HashSet<String>()
        for (meta in loadIndex()) {
            if (meta.id == exceptId || meta.kind != kind) continue
            if (kind == BaseKind.Users) {
                loadUsernames(meta.id).forEach { result.add(it.lowercase()) }
            } else {
                loadGroupEntries(meta.id).forEach { if (it.username.isNotEmpty()) result.add(it.username.lowercase()) }
            }
        }
        return result
    }

    fun deleteBase(id: Long) = synchronized(lock) {
        noteDeleted("base:" + idToString(id))
        removeBaseNow(id)
    }

    private fun removeBaseNow(id: Long) {
        statsGeneration++
        groupPath(id).delete()
        writeIndex(loadIndex().filter { it.id != id })
    }

    fun generateManualBaseId(): Long = (0..Long.MAX_VALUE).random() or (1L shl 63)

    fun groupBaseTitle(name: String, username: String): String {
        val bare = username.trim().removePrefix("@")
        val trimmed = name.trim()
        return if (bare.isEmpty()) trimmed
        else if (trimmed.isEmpty()) "@$bare"
        else "$trimmed (@$bare)"
    }

    // ---- sms_sent registry ----

    private var smsCache: MutableMap<String, SmsRecord>? = null

    private fun statusToString(s: SmsStatus) = when (s) {
        SmsStatus.SpamAcc -> "spam_acc"
        SmsStatus.NotFound -> "not_found"
        SmsStatus.Restricted -> "restricted"
        SmsStatus.PeerLimit -> "peer_limit"
        SmsStatus.SuspectedFake -> "suspected_fake"
        SmsStatus.Sent -> "ok"
    }

    private fun statusFromString(raw: String?) = when (raw) {
        "spam_acc" -> SmsStatus.SpamAcc
        "not_found" -> SmsStatus.NotFound
        "restricted" -> SmsStatus.Restricted
        "peer_limit" -> SmsStatus.PeerLimit
        "suspected_fake" -> SmsStatus.SuspectedFake
        else -> SmsStatus.Sent
    }

    private fun smsSentCache(): MutableMap<String, SmsRecord> {
        smsCache?.let { return it }
        val map = HashMap<String, SmsRecord>()
        readJson(smsPath())?.let { text ->
            runCatching {
                val o = JSONObject(text)
                for (key in o.keys()) {
                    val e = o.getJSONObject(key)
                    map[key] = SmsRecord(
                        username = key,
                        status = statusFromString(e.optString("status")),
                        account = e.optString("account"),
                        at = e.optString("at").toLongOrNull() ?: 0,
                        baseId = idFromString(e.optString("base")) ?: 0,
                        source = e.optString("source")
                    )
                }
            }
        }
        smsCache = map
        return map
    }

    // The registry is written at most once every 2 s (a send used to rewrite the whole file, which on
    // tens of thousands of records is megabytes per message) and when a campaign ends / the app goes
    // to the background; the screens are told once per write.
    private const val SAVE_DELAY_MS = 2_000L
    private val saver = java.util.concurrent.Executors.newSingleThreadScheduledExecutor { r ->
        Thread(r, "ggtg-sms-save").apply { isDaemon = true }
    }
    private var saveScheduled = false

    private fun saveSmsCache() {
        statsGeneration++
        if (saveScheduled) return
        saveScheduled = true
        saver.schedule({ flushSmsCache() }, SAVE_DELAY_MS, java.util.concurrent.TimeUnit.MILLISECONDS)
    }

    /** Writes the registry now, if there is anything unsaved. */
    @JvmStatic fun flushSmsCache() {
        synchronized(lock) {
            if (!saveScheduled) return
            saveScheduled = false
            writeSmsCacheNow()
        }
    }

    private fun writeSmsCacheNow() {
        val o = JSONObject()
        for ((username, r) in smsSentCache()) {
            o.put(username, JSONObject().apply {
                put("status", statusToString(r.status))
                put("account", r.account)
                put("at", r.at.toString())
                put("base", idToString(r.baseId))
                if (r.source.isNotEmpty()) put("source", r.source)
            })
        }
        writeJson(smsPath(), o.toString())
        fireChanged()
    }

    /** Removes every record with one of [statuses]; returns how many went. */
    fun clearSmsByStatus(statuses: Set<SmsStatus>): Int = synchronized(lock) {
        val cache = smsSentCache()
        val before = cache.size
        cache.values.removeAll {
            val gone = it.status in statuses
            if (gone) noteDeleted("sent:" + it.username)
            gone
        }
        val removed = before - cache.size
        if (removed > 0) saveSmsCache()
        removed
    }

    fun countSmsByStatus(status: SmsStatus): Int = synchronized(lock) {
        smsSentCache().values.count { it.status == status }
    }

    private fun normalizeUsername(raw: String): String = raw.trim().removePrefix("@").lowercase()

    fun findSmsRecord(username: String): SmsRecord? = synchronized(lock) {
        smsSentCache()[normalizeUsername(username)]
    }

    fun loadAllSmsRecords(): List<SmsRecord> = synchronized(lock) { smsSentCache().values.sortedByDescending { it.at } }

    fun deleteSmsRecord(username: String) = synchronized(lock) {
        val normalized = normalizeUsername(username)
        if (smsSentCache().remove(normalized) != null) {
            noteDeleted("sent:$normalized")
            saveSmsCache()
        }
    }

    fun clearSmsRecords() = synchronized(lock) {
        smsSentCache().keys.forEach { noteDeleted("sent:$it") }
        smsSentCache().clear()
        saveSmsCache()
    }

    fun markSmsStatus(username: String, status: SmsStatus, account: String, baseId: Long = 0) = synchronized(lock) {
        val normalized = normalizeUsername(username)
        if (normalized.isEmpty()) return
        val previous = smsSentCache()[normalized]
        var source = if (baseId != 0L) baseTitle(baseId) else ""
        var base = baseId
        if (source.isEmpty()) {
            // a later update without a base (a late answer, a status change) keeps what was known before
            source = previous?.source.orEmpty()
            if (base == 0L) base = previous?.baseId ?: 0L
        }
        smsSentCache()[normalized] = SmsRecord(normalized, status, account, System.currentTimeMillis() / 1000, base, source)
        saveSmsCache()
    }

    fun markSmsSent(username: String, account: String, baseId: Long = 0) =
        markSmsStatus(username, SmsStatus.Sent, account, baseId)

    fun markSmsSpamAcc(username: String, account: String, baseId: Long = 0) =
        markSmsStatus(username, SmsStatus.SpamAcc, account, baseId)

    // ---- for the sync between GGTG devices (GgtgSync) ----

    private fun tombPath() = File(dir(), "sync_tombstones.json")
    private var tombCache: MutableMap<String, Long>? = null
    private val tombSaver = java.util.concurrent.Executors.newSingleThreadScheduledExecutor { r ->
        Thread(r, "ggtg-tomb-save").apply { isDaemon = true }
    }
    private var tombSaveScheduled = false

    // What was deleted on purpose and when ("base:<id>", "sent:<username>"): a sync carries it along so a
    // deletion on one device is not undone by another device's older copy. Kept for 90 days.
    private fun tombs(): MutableMap<String, Long> {
        tombCache?.let { return it }
        val map = HashMap<String, Long>()
        val limit = System.currentTimeMillis() / 1000 - 90 * 86400
        readJson(tombPath())?.let { text ->
            runCatching {
                val o = JSONObject(text)
                for (key in o.keys()) {
                    val at = o.optString(key).toLongOrNull() ?: continue
                    if (at > limit) map[key] = at
                }
            }
        }
        tombCache = map
        return map
    }

    private fun saveTombs() {
        if (tombSaveScheduled) return
        tombSaveScheduled = true
        tombSaver.schedule({
            synchronized(lock) {
                tombSaveScheduled = false
                val o = JSONObject()
                for ((k, v) in tombs()) o.put(k, v.toString())
                writeJson(tombPath(), o.toString())
            }
        }, 2, java.util.concurrent.TimeUnit.SECONDS)
    }

    fun noteDeleted(key: String) = synchronized(lock) {
        tombs()[key] = System.currentTimeMillis() / 1000
        saveTombs()
    }

    fun loadTombstones(): Map<String, Long> = synchronized(lock) { HashMap(tombs()) }

    fun mergeTombstones(other: Map<String, Long>) = synchronized(lock) {
        val mine = tombs()
        var changed = false
        for ((k, v) in other) {
            if (v > (mine[k] ?: 0L)) { mine[k] = v; changed = true }
        }
        if (changed) saveTombs()
    }

    /** Puts a base in as it is: keeps [savedAt], replaces the contents, no cross-base dedupe. */
    fun importBase(id: Long, kind: BaseKind, title: String, savedAt: Long, usernames: List<String>, entries: List<GroupEntry>) =
        synchronized(lock) {
            val array = JSONArray()
            val count: Int
            if (kind == BaseKind.Groups) {
                for (e in entries) {
                    array.put(JSONObject().apply {
                        put("username", e.username)
                        put("title", e.title)
                        put("membersCount", e.membersCount)
                        put("link", e.link)
                        put("checked", e.checked)
                    })
                }
                count = entries.size
            } else {
                usernames.forEach { array.put(it) }
                count = usernames.size
            }
            writeJson(groupPath(id), array.toString())
            statsGeneration++
            val index = loadIndex().toMutableList()
            val meta = GroupMeta(id, title, count, savedAt, kind)
            val at = index.indexOfFirst { it.id == id }
            if (at >= 0) index[at] = meta else index.add(meta)
            writeIndex(index)
        }

    /** Removes a base without a tombstone (the deletion came from another device). */
    fun removeBaseForSync(id: Long) = synchronized(lock) { removeBaseNow(id) }

    /** Writes registry records as they are and drops [remove], all at once and without tombstones. */
    fun importSmsRecords(upsert: List<SmsRecord>, remove: List<String>) = synchronized(lock) {
        val cache = smsSentCache()
        upsert.forEach { cache[it.username] = it }
        remove.forEach { cache.remove(it) }
        if (upsert.isNotEmpty() || remove.isNotEmpty()) {
            saveSmsCache()
            flushSmsCache()
        }
    }

    /** Takes the people already written to out of every recipients base. */
    fun pruneDeliveredFromBases(): Int = synchronized(lock) {
        var removed = 0
        val cache = smsSentCache()
        val index = loadIndex().toMutableList()
        var changed = false
        for ((i, meta) in index.withIndex()) {
            if (meta.kind != BaseKind.Users) continue
            val names = loadUsernames(meta.id)
            val kept = names.filter { cache[normalizeUsername(it)]?.status != SmsStatus.Sent }
            if (kept.size == names.size) continue
            removed += names.size - kept.size
            val array = JSONArray()
            kept.forEach { array.put(it) }
            writeJson(groupPath(meta.id), array.toString())
            index[i] = meta.copy(count = kept.size)
            changed = true
        }
        if (changed) {
            statsGeneration++
            writeIndex(index)
        }
        removed
    }

    // ---- replies: who answered a sent message (username -> last answer), sms_replies.json ----

    data class SmsReply(val text: String, val at: Long)

    private var repliesCache: MutableMap<String, SmsReply>? = null
    private fun repliesPath() = File(dir(), "sms_replies.json")

    private fun replies(): MutableMap<String, SmsReply> {
        repliesCache?.let { return it }
        val map = HashMap<String, SmsReply>()
        readJson(repliesPath())?.let { text ->
            runCatching {
                val o = JSONObject(text)
                for (key in o.keys()) {
                    val e = o.getJSONObject(key)
                    map[key] = SmsReply(e.optString("text"), e.optLong("at"))
                }
            }
        }
        repliesCache = map
        return map
    }

    fun findReply(username: String): SmsReply? = synchronized(lock) { replies()[normalizeUsername(username)] }

    fun countReplies(): Int = synchronized(lock) { replies().size }

    fun saveReply(username: String, text: String, at: Long) = synchronized(lock) {
        val map = replies()
        map[normalizeUsername(username)] = SmsReply(text.take(500), at)
        val o = JSONObject()
        for ((k, r) in map) o.put(k, JSONObject().put("text", r.text).put("at", r.at))
        writeJson(repliesPath(), o.toString())
        fireChanged()
    }

    // ---- sources and remaining ----

    /** The user base a username belongs to (0 = none): where a hand-written message came from. */
    fun findBaseOf(username: String): Long = synchronized(lock) {
        val name = normalizeUsername(username)
        for (meta in loadIndex()) {
            if (meta.kind != BaseKind.Users) continue
            if (loadUsernames(meta.id).any { it.trim().removePrefix("@").lowercase() == name }) return meta.id
        }
        0L
    }

    private var titleCache: Map<Long, String>? = null

    fun baseTitle(id: Long): String = if (id == 0L) "" else synchronized(lock) {
        val map = titleCache ?: loadIndex().associate { it.id to it.title }.also { titleCache = it }
        map[id] ?: ""
    }

    /** How many of a base's people can still be written to (no record yet, not a bot). */
    fun remainingIn(id: Long): Int = baseStats(id).remaining

    /** The name of the base a record's person came from: the one saved with the record, else the base's own. */
    fun sourceOf(r: SmsRecord): String = r.source.ifEmpty { baseTitle(r.baseId) }

    /** The source of a person who was written to (empty if never). */
    fun sourceOf(username: String): String = findSmsRecord(username)?.let { sourceOf(it) }.orEmpty()

    // Counted again only when the registry or a base changed, not on every redraw of the list of bases.
    @Volatile private var statsGeneration = 1L
    private val statsMemo = HashMap<Long, Pair<Long, BaseStats>>()

    fun baseStats(id: Long): BaseStats = synchronized(lock) {
        val memo = statsMemo[id]
        if (memo != null && memo.first == statsGeneration) return memo.second
        val stats = computeBaseStats(loadUsernames(id))
        statsMemo[id] = statsGeneration to stats
        stats
    }

    // ---- stats (mirrors ComputeBaseStats) ----

    data class BaseStats(
        var total: Int = 0, var sent: Int = 0, var spamAcc: Int = 0,
        var notFound: Int = 0, var restricted: Int = 0, var peerLimit: Int = 0, var suspectedFake: Int = 0, var bots: Int = 0, var remaining: Int = 0
    )

    fun computeBaseStats(usernames: List<String>): BaseStats = synchronized(lock) {
        val stats = BaseStats()
        val records = smsSentCache()
        for (raw in usernames) {
            stats.total++
            val name = raw.trim().removePrefix("@")
            val lower = name.lowercase()
            if (lower.endsWith("bot")) { stats.bots++; continue }
            when (records[lower]?.status) {
                SmsStatus.Sent -> stats.sent++
                SmsStatus.SpamAcc -> stats.spamAcc++
                SmsStatus.NotFound -> stats.notFound++
                SmsStatus.Restricted -> stats.restricted++
                SmsStatus.PeerLimit -> stats.peerLimit++
                SmsStatus.SuspectedFake -> stats.suspectedFake++
                null -> stats.remaining++
            }
        }
        stats
    }

    fun isBotUsername(username: String): Boolean = username.trim().removePrefix("@").lowercase().endsWith("bot")

    // ---- username parsing (mirrors ParseUsernames) ----

    data class ParsedUsernames(val valid: List<String>, val rejected: List<String>, val duplicates: Int)

    private val separators = Regex("[\\s,;]+")
    private val linkRe = Regex("^(?:https?://)?(?:www\\.)?(?:t|telegram)\\.me/(.+)$", RegexOption.IGNORE_CASE)
    private val validRe = Regex("^[A-Za-z][A-Za-z0-9_]{3,31}$")
    private val reservedPaths = setOf(
        "joinchat", "addstickers", "addemoji", "addlist", "c", "s", "proxy",
        "socks", "share", "boost", "invoice", "login", "setlanguage", "bg"
    )

    fun parseUsernames(lines: List<String>): ParsedUsernames {
        val valid = ArrayList<String>()
        val rejected = ArrayList<String>()
        var duplicates = 0
        val seen = HashSet<String>()
        for (line in lines) {
            for (token in line.split(separators).filter { it.isNotEmpty() }) {
                var name = token
                val link = linkRe.matchEntire(name)
                if (link != null) {
                    val path = link.groupValues[1]
                    val cut = path.indexOfFirst { it == '/' || it == '?' || it == '#' }
                    val first = if (cut < 0) path else path.substring(0, cut)
                    if (reservedPaths.contains(first.lowercase()) || first.startsWith("+")) {
                        rejected.add(token); continue
                    }
                    name = first
                }
                name = name.removePrefix("@")
                if (!validRe.matches(name)) { rejected.add(token); continue }
                if (!seen.add(name.lowercase())) { duplicates++; continue }
                valid.add(name)
            }
        }
        return ParsedUsernames(valid, rejected, duplicates)
    }

    // ---- group list parsing: one entry per line, "@username Title" or "link Title" or plain link
    // (private groups have no username, only an invite link - unlike parseUsernames this keeps those) ----

    fun parseGroupLines(lines: List<String>): List<GroupEntry> {
        val result = ArrayList<GroupEntry>()
        val seen = HashSet<String>()
        for (raw in lines) {
            val line = raw.trim()
            if (line.isEmpty()) continue
            val splitAt = listOf("\t", " — ", " - ").firstNotNullOfOrNull { sep ->
                line.indexOf(sep).takeIf { it > 0 }?.let { it to sep.length }
            }
            val address = (if (splitAt != null) line.substring(0, splitAt.first) else line).trim()
            val title = if (splitAt != null) line.substring(splitAt.first + splitAt.second).trim() else ""
            if (address.isEmpty()) continue
            val link = linkRe.matchEntire(address)
            var username = ""
            var fullLink = ""
            if (link != null) {
                val path = link.groupValues[1]
                val cut = path.indexOfFirst { it == '/' || it == '?' || it == '#' }
                val first = if (cut < 0) path else path.substring(0, cut)
                if (first.startsWith("+") || reservedPaths.contains(first.lowercase())) {
                    fullLink = address
                } else if (validRe.matches(first)) {
                    username = first
                } else {
                    fullLink = address
                }
            } else {
                val bare = address.removePrefix("@")
                if (validRe.matches(bare)) username = bare else fullLink = address
            }
            val key = username.ifEmpty { fullLink }.lowercase()
            if (key.isEmpty() || !seen.add(key)) continue
            result.add(GroupEntry(username = username, title = title, membersCount = 0, link = fullLink, checked = false))
        }
        return result
    }
}
