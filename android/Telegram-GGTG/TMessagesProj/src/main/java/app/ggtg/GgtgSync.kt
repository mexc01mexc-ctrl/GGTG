package app.ggtg

import org.json.JSONArray
import org.json.JSONObject
import org.telegram.messenger.UserConfig
import java.io.ByteArrayOutputStream
import java.io.File
import java.security.MessageDigest
import java.security.SecureRandom
import java.util.zip.Deflater
import java.util.zip.Inflater
import javax.crypto.Cipher
import javax.crypto.Mac
import javax.crypto.spec.GCMParameterSpec
import javax.crypto.spec.SecretKeySpec

// ng: the sync of GGTG data between devices (this phone, GGTG Desktop on the Mac) through one "main" Telegram
// account: every device puts an encrypted snapshot of its data into that account's "Saved messages" and merges
// the snapshots of the other devices into its own. This file is the format and the merge (the same as
// core/ggtg_sync.cpp on the desktop); the transport is GgtgSyncTransport. Format: docs/ggtg-formats.md.
object GgtgSync {

    private const val FORMAT = 1
    private val MAGIC = byteArrayOf('G'.code.toByte(), 'G'.code.toByte(), 'S'.code.toByte(), 'Y'.code.toByte(), 1)
    private const val SALT_SIZE = 16
    private const val IV_SIZE = 12
    private const val TAG_BITS = 128
    private const val PBKDF_ROUNDS = 100_000
    private const val MAX_FILE = 30 * 1024 * 1024

    data class Settings(
        val enabled: Boolean = false,
        val mainUserId: String = "",
        val passphrase: String = "",
        val deviceId: String = "",
        val lastSyncAt: Long = 0,
        val lastResult: String = "",
        val lastUploadedHash: String = "",
        val seen: Map<String, Long> = emptyMap()
    )

    class Snapshot(val json: ByteArray, val hash: String)

    data class MergeResult(
        val ok: Boolean = false,
        val error: String = "",
        val bases: Int = 0,
        val sentAdded: Int = 0,
        val sentRemoved: Int = 0,
        val files: Int = 0
    )

    private val lock = Any()

    private fun settingsFile() = File(GgtgDb.dir(), "sync_settings.json")

    /** What the QR code of GGTG Desktop's "Подключить телефон" holds: the main account and the phrase,
     *  "ggtgsync:1:<user id>:<phrase as base64url of UTF-8>" (core/ggtg_sync.cpp's PairingText). */
    class Pairing(val mainUserId: String, val passphrase: String)

    /** A short fingerprint of the phrase ("A1B2C3"), the same as KeyId() on the desktop: it is in the snapshot's
     *  file name, so a device only opens the files made with its own phrase, and the user can compare it. */
    fun keyId(passphrase: String): String {
        if (passphrase.isEmpty()) return ""
        val hash = MessageDigest.getInstance("SHA-256").digest(("ggtgsync-key:" + passphrase).toByteArray(Charsets.UTF_8))
        return hash.joinToString("") { "%02x".format(it) }.substring(0, 6).uppercase()
    }

    fun parsePairing(text: String): Pairing? {
        val parts = text.trim().split(':')
        if (parts.size != 4 || parts[0] != "ggtgsync" || parts[1] != "1") return null
        val id = parts[2].takeIf { it.isNotEmpty() && it.all(Char::isDigit) } ?: return null
        val phrase = runCatching {
            String(android.util.Base64.decode(parts[3],
                android.util.Base64.URL_SAFE or android.util.Base64.NO_PADDING or android.util.Base64.NO_WRAP), Charsets.UTF_8)
        }.getOrNull()?.takeIf { it.isNotEmpty() } ?: return null
        return Pairing(id, phrase)
    }

    fun loadSettings(): Settings = synchronized(lock) {
        var s = Settings()
        runCatching {
            val f = settingsFile()
            if (f.exists()) {
                val o = JSONObject(f.readText())
                val seen = HashMap<String, Long>()
                o.optJSONObject("seen")?.let { so -> for (k in so.keys()) seen[k] = so.optString(k).toLongOrNull() ?: 0 }
                s = Settings(
                    enabled = o.optBoolean("enabled"),
                    mainUserId = o.optString("mainUserId"),
                    passphrase = o.optString("passphrase"),
                    deviceId = o.optString("deviceId"),
                    lastSyncAt = o.optString("lastSyncAt").toLongOrNull() ?: 0,
                    lastResult = o.optString("lastResult"),
                    lastUploadedHash = o.optString("lastUploadedHash"),
                    seen = seen
                )
            }
        }
        if (s.deviceId.isEmpty()) {
            val bytes = ByteArray(8).also { SecureRandom().nextBytes(it) }
            s = s.copy(deviceId = bytes.joinToString("") { "%02x".format(it) })
            saveSettings(s)
        }
        s
    }

    fun saveSettings(s: Settings) = synchronized(lock) {
        val o = JSONObject()
        val seen = JSONObject()
        s.seen.forEach { (k, v) -> seen.put(k, v.toString()) }
        o.put("seen", seen)
        o.put("enabled", s.enabled)
        o.put("mainUserId", s.mainUserId)
        o.put("passphrase", s.passphrase)
        o.put("deviceId", s.deviceId)
        o.put("lastSyncAt", s.lastSyncAt.toString())
        o.put("lastResult", s.lastResult)
        o.put("lastUploadedHash", s.lastUploadedHash)
        val f = settingsFile()
        val tmp = File(f.parentFile, f.name + ".tmp")
        tmp.writeText(o.toString())
        if (!tmp.renameTo(f)) { f.writeText(o.toString()); tmp.delete() }
    }

    /** Small files with the same shape on every platform: the one changed later wins as a whole. */
    private val plainFiles = listOf(
        "ai_profile.json", "ai_examples.json", "ai_playbook.json",
        "ai_training.json", "ai_settings.json", "chat_lists.json"
    )

    private fun safeFileName(name: String): Boolean {
        if (name.contains("..") || name.startsWith("/")) return false
        if (name in plainFiles) return true
        return name.startsWith("ai_knowledge/") && name.endsWith(".json") && name.count { it == '/' } == 1
    }

    private fun statusText(s: GgtgDb.SmsStatus) = when (s) {
        GgtgDb.SmsStatus.SpamAcc -> "spam_acc"
        GgtgDb.SmsStatus.NotFound -> "not_found"
        GgtgDb.SmsStatus.Restricted -> "restricted"
        GgtgDb.SmsStatus.PeerLimit -> "peer_limit"
        GgtgDb.SmsStatus.SuspectedFake -> "suspected_fake"
        GgtgDb.SmsStatus.LongAgo -> "long_ago"
        GgtgDb.SmsStatus.Sent -> "ok"
    }

    /** null for a status this app does not know (the desktop's "account_fail"): such marks are not taken. */
    private fun statusFrom(raw: String?): GgtgDb.SmsStatus? = when (raw) {
        "spam_acc" -> GgtgDb.SmsStatus.SpamAcc
        "not_found" -> GgtgDb.SmsStatus.NotFound
        "restricted" -> GgtgDb.SmsStatus.Restricted
        "peer_limit" -> GgtgDb.SmsStatus.PeerLimit
        "suspected_fake" -> GgtgDb.SmsStatus.SuspectedFake
        "long_ago" -> GgtgDb.SmsStatus.LongAgo
        "ok", "", null -> GgtgDb.SmsStatus.Sent
        else -> null
    }

    private fun long(o: JSONObject, key: String): Long = o.optString(key).toLongOrNull() ?: 0L

    // ---- the snapshot ----

    fun buildSnapshot(settings: Settings): Snapshot {
        val bases = JSONArray()
        for (meta in GgtgDb.loadIndex()) {
            val items = JSONArray()
            if (meta.kind == GgtgDb.BaseKind.Groups) {
                for (e in GgtgDb.loadGroupEntries(meta.id)) {
                    items.put(JSONObject().apply {
                        put("username", e.username)
                        put("title", e.title)
                        put("membersCount", e.membersCount)
                        put("link", e.link)
                        put("checked", e.checked)
                    })
                }
            } else {
                GgtgDb.loadUsernames(meta.id).forEach { items.put(it) }
            }
            bases.put(JSONObject().apply {
                put("id", GgtgDb.idToString(meta.id))
                put("kind", if (meta.kind == GgtgDb.BaseKind.Groups) "groups" else "users")
                put("title", meta.title)
                put("savedAt", meta.savedAt.toString())
                put("items", items)
            })
        }

        val sent = JSONObject()
        for (r in GgtgDb.loadAllSmsRecords()) {
            sent.put(r.username, JSONObject().apply {
                put("status", statusText(r.status))
                put("account", r.account)
                put("at", r.at.toString())
                put("base", GgtgDb.idToString(r.baseId))
                if (r.source.isNotEmpty()) put("source", r.source)
            })
        }

        val tomb = JSONObject()
        GgtgDb.loadTombstones().forEach { (k, v) -> tomb.put(k, v.toString()) }

        val files = JSONObject()
        val dir = GgtgDb.dir()
        val names = ArrayList(plainFiles)
        File(dir, "ai_knowledge").listFiles { f -> f.isFile && f.name.endsWith(".json") }
            ?.forEach { names.add("ai_knowledge/" + it.name) }
        for (name in names) {
            val f = File(dir, name)
            if (!f.exists() || f.length() > MAX_FILE) continue
            val text = runCatching { f.readText() }.getOrNull()?.takeIf { it.isNotEmpty() } ?: continue
            files.put(name, JSONObject().put("mtime", (f.lastModified() / 1000).toString()).put("text", text))
        }

        val body = JSONObject()
        body.put("format", FORMAT)
        body.put("bases", bases)
        body.put("sent", sent)
        body.put("tomb", tomb)
        body.put("files", files)
        val poolsStr = org.telegram.ui.Components.GgtgProxyPoolStore.toJson()
        if (poolsStr.isNotEmpty()) {
            runCatching {
                body.put("proxy_pools", JSONArray(poolsStr))
            }
        }
        val sessionNotes = JSONArray()
        for (s in GgtgSessions.listAll()) {
            if (s.userId == 0L && s.phone.isEmpty()) continue
            if (s.note.isEmpty() && s.role.isEmpty()) continue
            val obj = JSONObject().apply {
                put("id", s.id)
                put("userId", s.userId.toString())
                if (s.phone.isNotEmpty()) put("phone", s.phone)
                if (s.role.isNotEmpty()) put("role", s.role)
                if (s.note.isNotEmpty()) put("note", s.note)
                if (s.createdAt > 0L) put("createdAt", s.createdAt)
                if (s.hash != 0L) put("hash", s.hash.toString())
                if (s.device != null) put("device", s.device)
            }
            sessionNotes.put(obj)
        }
        if (sessionNotes.length() > 0) {
            body.put("session_notes", sessionNotes)
        }

        val accountStatuses = JSONObject()
        for ((uId, status) in GgtgSpamStatus.all()) {
            val key = uId.toString()
            val obj = JSONObject().apply {
                put("userId", key)
                put("state", when (status.state) {
                    GgtgSpamStatus.State.Temporary -> "temporary"
                    GgtgSpamStatus.State.Permanent -> "permanent"
                    else -> "clear"
                })
                put("until", status.until.toString())
                put("checkedAt", status.checkedAt.toString())
            }
            accountStatuses.put(key, obj)
        }
        for (slot in 0 until UserConfig.MAX_ACCOUNT_COUNT) {
            val uc = UserConfig.getInstance(slot)
            if (!uc.isClientActivated) continue
            val uId = uc.clientUserId
            val key = uId.toString()
            val obj = accountStatuses.optJSONObject(key) ?: JSONObject().apply {
                put("userId", key)
                accountStatuses.put(key, this)
            }
            uc.currentUser?.phone?.takeIf { it.isNotEmpty() }?.let { obj.put("phone", it) }
            if (uc.ncgFolder.isNotEmpty()) obj.put("folder", uc.ncgFolder)
            if (GgtgAccounts.isTrashed(slot)) obj.put("trashed", true)
            if (uc.ncgOffline) obj.put("offline", true)
            val sent = runCatching { GgtgSms.sentToday(slot) }.getOrDefault(0)
            if (sent > 0) obj.put("sentToday", sent)
        }
        if (accountStatuses.length() > 0) {
            body.put("account_statuses", accountStatuses)
        }
        // the hash covers the contents only, not who made it and when
        val hash = MessageDigest.getInstance("SHA-256").digest(body.toString().toByteArray(Charsets.UTF_8))
            .joinToString("") { "%02x".format(it) }
        body.put("device", settings.deviceId)
        body.put("deviceName", "Android")
        body.put("createdAt", (System.currentTimeMillis() / 1000).toString())
        return Snapshot(body.toString().toByteArray(Charsets.UTF_8), hash)
    }

    // ---- the merge ----

    fun mergeSnapshot(json: ByteArray): MergeResult {
        val root = try { JSONObject(String(json, Charsets.UTF_8)) } catch (t: Throwable) {
            return MergeResult(error = "Файл синхронизации повреждён")
        }
        if (root.optInt("format") > FORMAT) {
            return MergeResult(error = "Файл сделан более новой версией GGTG - обновите приложение")
        }

        // what was deleted on purpose, from both sides
        val remoteTomb = HashMap<String, Long>()
        root.optJSONObject("tomb")?.let { o -> for (k in o.keys()) remoteTomb[k] = long(o, k) }
        GgtgDb.mergeTombstones(remoteTomb)
        val tomb = GgtgDb.loadTombstones()
        fun deletedAfter(key: String, at: Long) = (tomb[key] ?: Long.MIN_VALUE) >= at

        // the registry of who was written to
        val local = HashMap<String, GgtgDb.SmsRecord>()
        GgtgDb.loadAllSmsRecords().forEach { local[it.username] = it }
        val upsert = ArrayList<GgtgDb.SmsRecord>()
        val remove = ArrayList<String>()
        var added = 0
        var removedCount = 0
        root.optJSONObject("sent")?.let { sent ->
            for (username in sent.keys()) {
                val o = sent.getJSONObject(username)
                val status = statusFrom(o.optString("status")) ?: continue
                var record = GgtgDb.SmsRecord(
                    username = username,
                    status = status,
                    account = o.optString("account"),
                    at = long(o, "at"),
                    baseId = GgtgDb.idFromString(o.optString("base")) ?: 0,
                    source = o.optString("source")
                )
                if (deletedAfter("sent:$username", record.at)) continue
                val was = local[username]
                if (was == null) {
                    upsert.add(record)
                    added++
                    continue
                }
                val wasSent = was.status == GgtgDb.SmsStatus.Sent
                val nowSent = status == GgtgDb.SmsStatus.Sent
                // "written to" is never lost; otherwise the later mark wins
                val take = if (wasSent != nowSent) nowSent else record.at > was.at
                if (take) {
                    if (record.source.isEmpty()) record = record.copy(source = was.source)
                    if (record.baseId == 0L) record = record.copy(baseId = was.baseId)
                    upsert.add(record)
                }
            }
        }
        for ((username, record) in local) {
            if (deletedAfter("sent:$username", record.at)) {
                remove.add(username)
                removedCount++
            }
        }
        GgtgDb.importSmsRecords(upsert, remove)

        // the bases: the newer save of a base wins as a whole
        var bases = 0
        val localBases = GgtgDb.loadIndex().associateBy { it.id }
        root.optJSONArray("bases")?.let { array ->
            for (i in 0 until array.length()) {
                val o = array.getJSONObject(i)
                val id = GgtgDb.idFromString(o.optString("id")) ?: continue
                if (id == 0L) continue
                val kind = if (o.optString("kind") == "groups") GgtgDb.BaseKind.Groups else GgtgDb.BaseKind.Users
                val savedAt = long(o, "savedAt")
                if (deletedAfter("base:" + GgtgDb.idToString(id), savedAt)) continue
                val mine = localBases[id]
                if (mine != null && mine.savedAt >= savedAt) continue
                val names = ArrayList<String>()
                val entries = ArrayList<GgtgDb.GroupEntry>()
                val items = o.optJSONArray("items") ?: JSONArray()
                for (j in 0 until items.length()) {
                    if (kind == GgtgDb.BaseKind.Groups) {
                        val e = items.optJSONObject(j) ?: continue
                        entries.add(GgtgDb.GroupEntry(
                            username = e.optString("username"),
                            title = e.optString("title"),
                            membersCount = e.optInt("membersCount"),
                            link = e.optString("link"),
                            checked = e.optBoolean("checked")
                        ))
                    } else {
                        items.optString(j).takeIf { it.isNotEmpty() }?.let { names.add(it) }
                    }
                }
                GgtgDb.importBase(id, kind, o.optString("title"), savedAt, names, entries)
                bases++
            }
        }
        for ((id, meta) in localBases) {
            if (deletedAfter("base:" + GgtgDb.idToString(id), meta.savedAt)) {
                GgtgDb.removeBaseForSync(id)
                bases++
            }
        }
        GgtgDb.pruneDeliveredFromBases()

        // merge proxy pools if present
        root.optJSONArray("proxy_pools")?.let { poolsArr ->
            org.telegram.ui.Components.GgtgProxyPoolStore.mergeJson(poolsArr.toString())
        }

        // merge session notes & roles across devices
        root.optJSONArray("session_notes")?.let { notesArr ->
            val all = GgtgSessions.listAll()
            for (i in 0 until notesArr.length()) {
                val obj = notesArr.optJSONObject(i) ?: continue
                val remoteId = obj.optString("id")
                val uId = obj.optString("userId").toLongOrNull() ?: 0L
                val phone = obj.optString("phone")
                val note = obj.optString("note")
                val role = obj.optString("role")
                val remoteHash = obj.optString("hash").toLongOrNull() ?: 0L
                if (note.isEmpty() && role.isEmpty()) continue
                var matched = false
                for (item in all) {
                    if ((remoteId.isNotEmpty() && item.id == remoteId) ||
                        (remoteHash != 0L && item.hash == remoteHash) ||
                        ((uId != 0L && item.userId == uId) || (phone.isNotEmpty() && item.phone == phone))) {
                        matched = true
                        if (note.isNotEmpty() && (item.note.isEmpty() || item.note != note)) {
                            GgtgSessions.updateNote(item.id, note)
                        }
                        if (role.isNotEmpty() && item.role.isEmpty()) {
                            GgtgSessions.updateRole(item.id, role)
                        }
                        if (remoteHash != 0L && item.hash == 0L) {
                            GgtgSessions.updateHash(item.id, remoteHash)
                        }
                    }
                }
                if (!matched && uId != 0L) {
                    val entry = GgtgSessions.StoredSession(
                        id = if (remoteId.isNotEmpty()) remoteId else "${System.currentTimeMillis()}_${(1000..9999).random()}",
                        userId = uId,
                        role = role.ifEmpty { "stored" },
                        source = "sync",
                        note = note,
                        state = "ok",
                        createdAt = obj.optLong("createdAt", System.currentTimeMillis() / 1000),
                        hash = remoteHash,
                        phone = phone,
                        device = obj.optJSONObject("device")
                    )
                    GgtgSessions.addStored(entry)
                }
            }
        }

        // merge account statuses across devices
        root.optJSONObject("account_statuses")?.let { statusesObj ->
            for (key in statusesObj.keys()) {
                val entry = statusesObj.optJSONObject(key) ?: continue
                val uId = key.toLongOrNull() ?: continue
                val remoteStateStr = entry.optString("state")
                val remoteUntil = entry.optLong("until", 0L)
                val remoteCheckedAt = entry.optLong("checkedAt", 0L)
                if (remoteCheckedAt > 0) {
                    val state = when (remoteStateStr) {
                        "temporary" -> GgtgSpamStatus.State.Temporary
                        "permanent" -> GgtgSpamStatus.State.Permanent
                        else -> GgtgSpamStatus.State.Clear
                    }
                    GgtgSpamStatus.merge(uId, GgtgSpamStatus.Status(state, remoteUntil, remoteCheckedAt))
                }
                for (slot in 0 until UserConfig.MAX_ACCOUNT_COUNT) {
                    val uc = UserConfig.getInstance(slot)
                    if (!uc.isClientActivated || uc.clientUserId != uId) continue
                    val folder = entry.optString("folder")
                    if (folder.isNotEmpty() && uc.ncgFolder.isEmpty()) {
                        uc.ncgFolder = folder
                        uc.saveConfig(false)
                    }
                    if (entry.optBoolean("trashed", false) && !GgtgAccounts.isTrashed(slot)) {
                        GgtgAccounts.setTrashed(listOf(slot), true)
                    }
                }
            }
        }

        // the AI side: whichever copy was changed later
        var filesCount = 0
        val dir = GgtgDb.dir()
        root.optJSONObject("files")?.let { files ->
            for (name in files.keys()) {
                if (!safeFileName(name)) continue
                val o = files.getJSONObject(name)
                val mtime = long(o, "mtime")
                val text = o.optString("text")
                if (name == "chat_lists.json") {
                    app.ggtg.ai.GgtgChatLists.mergeJson(text)
                    filesCount++
                    continue
                }
                val f = File(dir, name)
                if (f.exists() && f.lastModified() / 1000 >= mtime) continue
                f.parentFile?.mkdirs()
                val tmp = File(f.parentFile, f.name + ".tmp")
                tmp.writeText(text)
                if (!tmp.renameTo(f)) { f.writeText(text); tmp.delete() }
                f.setLastModified(mtime * 1000)
                filesCount++
            }
        }
        return MergeResult(true, "", bases, added, removedCount, filesCount)
    }

    fun resultText(r: MergeResult): String =
        if (!r.ok) r.error
        else "баз: ${r.bases}, новых отметок: ${r.sentAdded}, убрано: ${r.sentRemoved}, файлов ИИ: ${r.files}"

    // ---- compress + encrypt (AES-256-GCM, key from the passphrase) ----

    private fun pbkdf2(password: ByteArray, salt: ByteArray): ByteArray {
        // one 32-byte block of PBKDF2-HMAC-SHA256 (same as OpenSSL's on the desktop; written out here so the
        // result does not depend on how the phone's provider turns characters into bytes)
        val mac = Mac.getInstance("HmacSHA256")
        mac.init(SecretKeySpec(password, "HmacSHA256"))
        mac.update(salt)
        mac.update(byteArrayOf(0, 0, 0, 1))
        var u = mac.doFinal()
        val t = u.copyOf()
        for (i in 2..PBKDF_ROUNDS) {
            u = mac.doFinal(u)
            for (j in t.indices) t[j] = (t[j].toInt() xor u[j].toInt()).toByte()
        }
        return t
    }

    // Qt's qCompress: the length of the data as 4 big-endian bytes, then a zlib stream.
    private fun compress(data: ByteArray): ByteArray {
        val deflater = Deflater(6)
        deflater.setInput(data)
        deflater.finish()
        val out = ByteArrayOutputStream()
        out.write(byteArrayOf((data.size ushr 24).toByte(), (data.size ushr 16).toByte(), (data.size ushr 8).toByte(), data.size.toByte()))
        val buffer = ByteArray(64 * 1024)
        while (!deflater.finished()) out.write(buffer, 0, deflater.deflate(buffer))
        deflater.end()
        return out.toByteArray()
    }

    private fun uncompress(data: ByteArray): ByteArray? {
        if (data.size < 5) return null
        val inflater = Inflater()
        inflater.setInput(data, 4, data.size - 4)
        val out = ByteArrayOutputStream()
        val buffer = ByteArray(64 * 1024)
        return try {
            while (!inflater.finished()) {
                val n = inflater.inflate(buffer)
                if (n == 0 && (inflater.needsInput() || inflater.needsDictionary())) break
                out.write(buffer, 0, n)
            }
            out.toByteArray()
        } catch (t: Throwable) { null } finally { inflater.end() }
    }

    fun pack(json: ByteArray, passphrase: String): ByteArray {
        val random = SecureRandom()
        val salt = ByteArray(SALT_SIZE).also { random.nextBytes(it) }
        val iv = ByteArray(IV_SIZE).also { random.nextBytes(it) }
        val key = pbkdf2(passphrase.toByteArray(Charsets.UTF_8), salt)
        val cipher = Cipher.getInstance("AES/GCM/NoPadding")
        cipher.init(Cipher.ENCRYPT_MODE, SecretKeySpec(key, "AES"), GCMParameterSpec(TAG_BITS, iv))
        val encrypted = cipher.doFinal(compress(json)) // the ciphertext, then the 16-byte tag
        return MAGIC + salt + iv + encrypted
    }

    fun unpack(blob: ByteArray, passphrase: String): ByteArray? {
        val header = MAGIC.size + SALT_SIZE + IV_SIZE
        if (blob.size < header + 16) return null
        for (i in MAGIC.indices) if (blob[i] != MAGIC[i]) return null
        return try {
            val salt = blob.copyOfRange(MAGIC.size, MAGIC.size + SALT_SIZE)
            val iv = blob.copyOfRange(MAGIC.size + SALT_SIZE, header)
            val key = pbkdf2(passphrase.toByteArray(Charsets.UTF_8), salt)
            val cipher = Cipher.getInstance("AES/GCM/NoPadding")
            cipher.init(Cipher.DECRYPT_MODE, SecretKeySpec(key, "AES"), GCMParameterSpec(TAG_BITS, iv))
            uncompress(cipher.doFinal(blob, header, blob.size - header))
        } catch (t: Throwable) { null }
    }
}
