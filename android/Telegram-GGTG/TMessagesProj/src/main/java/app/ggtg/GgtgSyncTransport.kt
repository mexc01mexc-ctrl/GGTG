package app.ggtg

import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeoutOrNull
import org.telegram.messenger.AccountInstance
import org.telegram.messenger.AndroidUtilities
import org.telegram.messenger.ApplicationLoader
import org.telegram.messenger.FileLog
import org.telegram.messenger.SendMessagesHelper
import org.telegram.messenger.UserConfig
import org.telegram.tgnet.ConnectionsManager
import org.telegram.tgnet.TLObject
import org.telegram.tgnet.TLRPC
import java.io.File
import kotlin.coroutines.resume

// ng: the transport of GgtgSync: one round merges what the other devices put into the main account's
// "Saved messages" and then puts this device's own snapshot there if it changed. The same as
// core/ggtg_sync_transport.cpp on the desktop.
object GgtgSyncTransport {

    data class Report(val ok: Boolean, val text: String)

    /** A file of this transfer in the main account's "Saved messages": kind is "sync" (a data snapshot), "pack"
     *  (accounts for this phone, a .ggtg) or "keys" (accounts for GGTG Desktop). */
    class Remote(
        val msgId: Int,
        val kind: String,
        val device: String,
        val doc: TLRPC.Document,
        /** keyId() of the phrase the file was made with ("" in older files) */
        val key: String = "",
        val date: Int = 0
    )

    private const val PREFIX = "ggtg-sync-"
    private const val TAG = "#ggtgsync"
    private const val CHUNK = 512 * 1024
    private const val PERIOD_MS = 5 * 60 * 1000L
    private const val AFTER_CHANGE_MS = 90 * 1000L
    private const val FIRST_DELAY_MS = 25 * 1000L
    private const val SOON_MS = 5 * 1000L
    private const val BEFORE_MAILING_TIMEOUT_MS = 30 * 1000L
    /** A file of another device made with another phrase and not replaced for this long is of a device that is gone. */
    private const val FOREIGN_KEY_TTL_S = 7 * 86400

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)

    @Volatile var running = false
        private set

    /** Where a file to put into "Saved messages" is written first. Not the app's own cacheDir: Telegram refuses to
     *  send anything from /data/data/<package> (AndroidUtilities.isInternalUri - "unsupported file"), so the files
     *  never left the phone. The app's external cache (Android/data/<package>/cache) needs no permission. */
    private fun outbox(): File {
        val context = ApplicationLoader.applicationContext
        val base = context.externalCacheDir ?: context.getExternalFilesDir(null) ?: context.cacheDir
        return File(base, "ggtg-sync").also { it.mkdirs() }
    }

    /** The slot of the main account, if it is signed in on this phone. */
    fun mainSlot(): Int? {
        val wanted = GgtgSync.loadSettings().mainUserId.toLongOrNull() ?: return null
        for (slot in 0 until UserConfig.MAX_ACCOUNT_COUNT) {
            val config = UserConfig.getInstance(slot)
            if (config.isClientActivated && config.clientUserId == wanted) return slot
        }
        return null
    }

    /** The bytes of a downloaded part: copied inside the callback, because right after it the response's buffer is freed. */
    private class Chunk(val bytes: ByteArray)

    private suspend fun rpc(slot: Int, req: TLObject, dcId: Int = ConnectionsManager.DEFAULT_DATACENTER_ID, type: Int = ConnectionsManager.ConnectionTypeGeneric): Pair<Any?, TLRPC.TL_error?> =
        suspendCancellableCoroutine { cont ->
            val token = AccountInstance.getInstance(slot).connectionsManager.sendRequest(req, { resp, err ->
                val value: Any? = if (resp is TLRPC.TL_upload_file) {
                    val buffer = resp.bytes
                    if (buffer == null) Chunk(ByteArray(0)) else Chunk(ByteArray(buffer.limit()).also { buffer.readBytes(it, false) })
                } else resp
                if (cont.isActive) cont.resume(value to err)
            }, null, null, ConnectionsManager.RequestFlagFailOnServerErrors, dcId, type, true)
            cont.invokeOnCancellation { AccountInstance.getInstance(slot).connectionsManager.cancelRequest(token, false) }
        }

    private fun remotesOf(result: Any?): List<Remote> {
        val messages = (result as? TLRPC.messages_Messages)?.messages ?: return emptyList()
        val out = ArrayList<Remote>()
        for (m in messages) {
            val doc = (m.media as? TLRPC.TL_messageMediaDocument)?.document ?: continue
            val name = doc.attributes.filterIsInstance<TLRPC.TL_documentAttributeFilename>().firstOrNull()?.file_name ?: continue
            if (!name.endsWith(".bin")) continue
            val stem = name.substring(0, name.length - 4)
            when {
                stem.startsWith(PREFIX) -> {
                    // "<device>-<key>", or just "<device>" before
                    val rest = stem.substring(PREFIX.length)
                    val dash = rest.indexOf('-')
                    out.add(Remote(m.id, "sync", if (dash < 0) rest else rest.substring(0, dash), doc,
                        if (dash < 0) "" else rest.substring(dash + 1), m.date))
                }
                stem.startsWith("ggtg-accounts-pack-") -> out.add(Remote(m.id, "pack", stem.substring(19), doc))
                stem.startsWith("ggtg-accounts-keys-") -> out.add(Remote(m.id, "keys", stem.substring(19), doc))
            }
        }
        return out
    }

    private suspend fun download(slot: Int, doc: TLRPC.Document): ByteArray? {
        val out = java.io.ByteArrayOutputStream()
        var offset = 0L
        while (doc.size <= 0 || offset < doc.size) {
            val req = TLRPC.TL_upload_getFile()
            req.location = TLRPC.TL_inputDocumentFileLocation().also {
                it.id = doc.id
                it.access_hash = doc.access_hash
                it.file_reference = doc.file_reference
                it.thumb_size = ""
            }
            req.offset = offset
            req.limit = CHUNK
            val (resp, err) = rpc(slot, req, doc.dc_id, ConnectionsManager.ConnectionTypeDownload)
            val chunk = resp as? Chunk
            if (err != null || chunk == null) return null
            if (chunk.bytes.isEmpty()) break
            out.write(chunk.bytes)
            offset += chunk.bytes.size
        }
        return out.toByteArray()
    }

    /** One round. [done] is called on the main thread; the upload itself goes on in the background. */
    fun runSync(done: ((Report) -> Unit)? = null) {
        fun reply(report: Report) {
            if (done != null) AndroidUtilities.runOnUIThread { done(report) }
        }
        if (running) { reply(Report(false, "Синхронизация уже идёт")); return }
        val settings = GgtgSync.loadSettings()
        if (settings.passphrase.isEmpty()) { reply(Report(false, "Задайте фразу-пароль для шифрования")); return }
        val slot = mainSlot()
        if (slot == null) { reply(Report(false, "Главный аккаунт не подключён в этом GGTG")); return }
        running = true
        scope.launch {
            GgtgWakeLock.withLock("sync") {
                val report = try { round(slot, settings) } catch (t: Throwable) {
                    Report(false, "Ошибка: " + (t.message ?: t.javaClass.simpleName))
                }
                running = false
                val now = GgtgSync.loadSettings()
                GgtgSync.saveSettings(now.copy(lastSyncAt = System.currentTimeMillis() / 1000, lastResult = report.text))
                reply(report)
            }
        }
    }

    private suspend fun round(slot: Int, start: GgtgSync.Settings): Report {
        var settings = start
        suspend fun search(query: String): List<Remote>? {
            val req = TLRPC.TL_messages_search()
            req.peer = TLRPC.TL_inputPeerSelf()
            req.q = query
            req.from_id = null
            req.saved_peer_id = null
            req.saved_reaction = null
            req.filter = TLRPC.TL_inputMessagesFilterDocument()
            req.limit = 100
            val (resp, err) = rpc(slot, req)
            if (err != null) return null
            return remotesOf(resp)
        }
        var all = search(TAG)
        if (all != null && all.isEmpty()) all = search("")
        if (all == null) return Report(false, "Не удалось прочитать «Избранное»")
        val incoming = all.count { it.kind == "pack" && it.device != settings.deviceId }
        val found = all.filter { it.kind == "sync" }

        // The newest of each device made with this phrase (or an older file without the fingerprint - tried, may
        // not open). This device's own other files go: older ones and the ones made with a previous phrase (then
        // it puts its data up again); so do the files of a gone device made with another phrase.
        val key = GgtgSync.keyId(settings.passphrase)
        val now = (System.currentTimeMillis() / 1000).toInt()
        var ownNewest = 0
        val newest = HashMap<String, Remote>()
        val otherKey = HashMap<String, Remote>()
        for (r in found) {
            if (r.device == settings.deviceId) { if (r.key == key) ownNewest = maxOf(ownNewest, r.msgId); continue }
            val target = if (r.key.isNotEmpty() && r.key != key) otherKey else newest
            val mine = target[r.device]
            if (mine == null || r.msgId > mine.msgId) target[r.device] = r
        }
        val stale = found.filter {
            if (it.device == settings.deviceId) it.key != key || it.msgId < ownNewest
            else it.key.isNotEmpty() && it.key != key && it.date > 0 && now - it.date > FOREIGN_KEY_TTL_S
        }.map { it.msgId }
        val otherKeys = ArrayList(otherKey.filterKeys { it !in newest }.values.map { it.key })
        val sameKeyDevices = newest.values.count { it.key.isNotEmpty() }
        if (stale.isNotEmpty()) {
            val req = TLRPC.TL_messages_deleteMessages()
            req.revoke = true
            req.id.addAll(stale)
            rpc(slot, req)
        }

        var merged = 0
        var problem = ""
        val total = intArrayOf(0, 0, 0, 0)
        val seen = HashMap(settings.seen)
        for ((device, remote) in newest) {
            if ((seen[device] ?: 0L) >= remote.msgId) continue
            val data = download(slot, remote.doc)
            if (data == null) { problem = "Не скачался файл другого устройства"; continue }
            val json = GgtgSync.unpack(data, settings.passphrase)
            // an older file without the fingerprint, made with another phrase: not an error, just not ours
            if (json == null) { otherKeys.add("?"); continue }
            val result = withContext(Dispatchers.Default) { GgtgSync.mergeSnapshot(json) }
            if (!result.ok) { problem = result.error; continue }
            total[0] += result.bases; total[1] += result.sentAdded; total[2] += result.sentRemoved; total[3] += result.files
            merged++
            seen[device] = remote.msgId.toLong()
        }
        // this device's own data goes up even when another device's file failed; the problem is told in the result
        val note = (if (merged == 0 && sameKeyDevices == 0 && otherKeys.isNotEmpty())
            ". Другое устройство шифрует другим ключом (${otherKeys.joinToString(", ")}, здесь $key): отсканируйте QR-код заново"
        else "") + (if (problem.isNotEmpty()) ". $problem" else "")
        val ok = problem.isEmpty() || merged > 0
        val got = if (merged > 0) GgtgSync.resultText(GgtgSync.MergeResult(true, "", total[0], total[1], total[2], total[3])) else ""

        // our own data, if it changed
        settings = GgtgSync.loadSettings().copy(seen = seen)
        val snapshot = GgtgSync.buildSnapshot(settings)
        if (snapshot.hash == settings.lastUploadedHash && ownNewest != 0) {
            GgtgSync.saveSettings(settings)
            return Report(ok, (if (merged > 0) "Получено: $got" else "Всё актуально") + note + incomingNote(incoming))
        }
        val dir = outbox()
        val file = File(dir, PREFIX + settings.deviceId + "-" + key + ".bin")
        file.writeBytes(GgtgSync.pack(snapshot.json, settings.passphrase))
        val dialogId = UserConfig.getInstance(slot).clientUserId
        withContext(Dispatchers.Main) {
            SendMessagesHelper.prepareSendingDocument(AccountInstance.getInstance(slot), file.absolutePath, file.absolutePath, null, TAG,
                "application/octet-stream", dialogId, null, null, null, null, null, true, 0, null, null, false)
        }
        GgtgSync.saveSettings(settings.copy(lastUploadedHash = snapshot.hash))
        return Report(ok, (if (merged > 0) "Получено: $got. " else "") + "Свои данные отправлены" + note + incomingNote(incoming))
    }

    private fun incomingNote(count: Int) = if (count > 0) ". Ждут аккаунты с Desktop: $count" else ""

    // ---- accounts between the devices ----

    private suspend fun searchAll(slot: Int): List<Remote>? {
        suspend fun one(query: String): List<Remote>? {
            val req = TLRPC.TL_messages_search()
            req.peer = TLRPC.TL_inputPeerSelf()
            req.q = query
            req.from_id = null
            req.saved_peer_id = null
            req.saved_reaction = null
            req.filter = TLRPC.TL_inputMessagesFilterDocument()
            req.limit = 100
            val (resp, err) = rpc(slot, req)
            return if (err != null) null else remotesOf(resp)
        }
        var all = one(TAG)
        if (all != null && all.isEmpty()) all = one("")
        return all
    }

    /** The accounts GGTG Desktop put there for this phone (newest per device); [done] runs on the main thread. */
    fun findIncoming(done: (List<Remote>, String) -> Unit) {
        val slot = mainSlot()
        if (slot == null) { AndroidUtilities.runOnUIThread { done(emptyList(), "Главный аккаунт не подключён в этом GGTG") }; return }
        val mine = GgtgSync.loadSettings().deviceId
        scope.launch {
            val all = try { searchAll(slot) } catch (t: Throwable) { null }
            val list = all?.filter { it.kind == "pack" && it.device != mine }
                ?.sortedByDescending { it.msgId }.orEmpty()
            AndroidUtilities.runOnUIThread { done(list, if (all == null) "Не удалось прочитать «Избранное»" else "") }
        }
    }

    /** Downloads and decrypts one incoming file; does not delete it yet; [done] gets the payload or an error. */
    fun fetchIncoming(remote: Remote, done: (ByteArray?, String) -> Unit) {
        val slot = mainSlot()
        val passphrase = GgtgSync.loadSettings().passphrase
        if (slot == null) { AndroidUtilities.runOnUIThread { done(null, "Главный аккаунт не подключён в этом GGTG") }; return }
        scope.launch {
            val data = try { download(slot, remote.doc) } catch (t: Throwable) { null }
            val plain = data?.let { GgtgSync.unpack(it, passphrase) }
            AndroidUtilities.runOnUIThread {
                when {
                    data == null -> done(null, "Не скачался файл")
                    plain == null -> done(null, "Не удалось расшифровать: проверьте, что фраза одинаковая на обоих устройствах")
                    else -> done(plain, "")
                }
            }
        }
    }

    /** Deletes an incoming message once it has been successfully imported. */
    fun deleteIncoming(remote: Remote) {
        val slot = mainSlot() ?: return
        scope.launch {
            try {
                val req = TLRPC.TL_messages_deleteMessages()
                req.revoke = true
                req.id.add(remote.msgId)
                rpc(slot, req)
            } catch (t: Throwable) {
                FileLog.e(t)
            }
        }
    }

    /** Puts accounts for GGTG Desktop into the main account's "Saved messages" (kind "keys"). */
    fun sendAccounts(kind: String, payload: ByteArray, done: (Boolean, String) -> Unit) {
        val slot = mainSlot()
        val settings = GgtgSync.loadSettings()
        if (settings.passphrase.isEmpty()) { done(false, "Задайте фразу-пароль в «Синхронизации»"); return }
        if (slot == null) { done(false, "Главный аккаунт не подключён в этом GGTG"); return }
        scope.launch {
            GgtgWakeLock.withLock("sync_send_accounts") {
                val dir = outbox()
                val file = File(dir, "ggtg-accounts-$kind-${settings.deviceId}.bin")
                file.writeBytes(GgtgSync.pack(payload, settings.passphrase))
                val dialogId = UserConfig.getInstance(slot).clientUserId
                withContext(Dispatchers.Main) {
                    SendMessagesHelper.prepareSendingDocument(AccountInstance.getInstance(slot), file.absolutePath, file.absolutePath, null, TAG,
                        "application/octet-stream", dialogId, null, null, null, null, null, true, 0, null, null, false)
                }
                AndroidUtilities.runOnUIThread { done(true, "Файл отправляется в «Избранное» главного аккаунта. Когда он там появится, его можно принять на Desktop") }
            }
        }
    }

    // ---- the automatic part ----

    private var started = false
    private val periodic = object : Runnable {
        override fun run() {
            tick()
            AndroidUtilities.runOnUIThread(this, PERIOD_MS)
        }
    }
    private val afterChange = Runnable { tick() }

    private fun tick() {
        val settings = GgtgSync.loadSettings()
        if (settings.enabled && !running && mainSlot() != null) runSync()
    }

    /** Called when the app starts and whenever the settings change. */
    @JvmStatic fun restart() {
        AndroidUtilities.runOnUIThread {
            AndroidUtilities.cancelRunOnUIThread(periodic)
            AndroidUtilities.cancelRunOnUIThread(afterChange)
            val settings = GgtgSync.loadSettings()
            if (!settings.enabled || settings.passphrase.isEmpty()) return@runOnUIThread
            if (!started) {
                started = true
                GgtgDb.addListener {
                    val s = GgtgSync.loadSettings()
                    if (s.enabled && !running) {
                        AndroidUtilities.cancelRunOnUIThread(afterChange)
                        AndroidUtilities.runOnUIThread(afterChange, AFTER_CHANGE_MS)
                    }
                }
            }
            AndroidUtilities.runOnUIThread(afterChange, FIRST_DELAY_MS)
            AndroidUtilities.runOnUIThread(periodic, PERIOD_MS)
        }
    }

    /** Before a mailing: when the sync is on, first takes in what the other device sent, so nobody it already
     *  wrote to is written to again. Gives up after 30 s (the mailing goes on anyway). */
    suspend fun syncBeforeMailing() {
        val settings = GgtgSync.loadSettings()
        if (!settings.enabled || settings.passphrase.isEmpty() || mainSlot() == null) return
        withTimeoutOrNull(BEFORE_MAILING_TIMEOUT_MS) {
            // a round already going on may have downloaded before the other device put its last records up
            while (running) delay(500)
            suspendCancellableCoroutine<Unit> { cont -> runSync { if (cont.isActive) cont.resume(Unit) } }
        }
    }

    /** After a mailing: puts the new "уже написали" up a few seconds later. */
    @JvmStatic fun syncSoon() {
        val settings = GgtgSync.loadSettings()
        if (!settings.enabled || settings.passphrase.isEmpty()) return
        restart()
        AndroidUtilities.runOnUIThread {
            AndroidUtilities.cancelRunOnUIThread(afterChange)
            AndroidUtilities.runOnUIThread(afterChange, SOON_MS)
        }
    }
}
