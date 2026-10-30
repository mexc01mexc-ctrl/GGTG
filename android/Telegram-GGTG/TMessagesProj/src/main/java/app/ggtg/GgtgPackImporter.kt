package app.ggtg

import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.MainScope
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONObject
import org.telegram.messenger.UserConfig
import org.telegram.tgnet.ConnectionsManager
import org.telegram.tgnet.TLRPC
import org.telegram.messenger.FileLog
import java.io.File
import java.util.zip.ZipFile

// ng: imports a .ggtg pack written by GGTG Desktop's "Экспорт" section (settings/ggtg_export.cpp).
// Every account in it carries a brand-new session minted on the Mac for this transfer. With
// "handoff": true (current Macs) that session is an ordinary Desktop one: this phone makes its own
// Android session from it the way the tdata import does (QR handoff: this app asks for a login token
// as the genuine Android client, the Mac's session approves it), then ends the Mac's session. A Mac
// minting a session as Android itself gets API_ID_PUBLISHED_FLOOD from Telegram. When the handoff
// fails, or for older packs, the keys are installed as-is. Format: docs/ggtg-formats.md in ggtg-macos.
object GgtgPackImporter {

    private const val MANIFEST = "manifest.json"
    private const val KIND = "ggtg-android-export"
    private const val MAX_FORMAT = 1
    private const val AUTH_KEY_SIZE = 256

    private data class PackProxy(
        val type: String,
        val host: String,
        val port: Int,
        val user: String,
        val password: String,
        val secret: String
    )

    private data class PackAccount(
        val userId: Long,
        val mainDcId: Int,
        val keys: List<Pair<Int, ByteArray>>,
        val phone: String,
        val firstName: String = "",
        val lastName: String = "",
        val username: String = "",
        val folder: String?,
        val pinned: Boolean,
        val twoFactor: String,
        val proxy: PackProxy?,
        val device: JSONObject?,
        val handoff: Boolean,
        val sessionNote: String = "",
        val sessionRole: String = "",
        val spamStatus: JSONObject? = null
    )

    /** True when [file] is a zip holding a GGTG Desktop export manifest. */
    fun isPack(file: File): Boolean = try {
        ZipFile(file).use { zip ->
            val entry = zip.getEntry(MANIFEST) ?: return false
            val json = JSONObject(zip.getInputStream(entry).bufferedReader().readText())
            json.optString("kind") == KIND
        }
    } catch (t: Throwable) {
        false
    }

    /** Returns how many accounts in the pack need a proxy (have no usable proxy and are not yet logged in). */
    fun countWithoutProxy(file: File): Int {
        val accounts = try { readPack(file) } catch (t: Throwable) { return 0 }
        return accounts.count { account ->
            val loggedIn = (0 until UserConfig.MAX_ACCOUNT_COUNT).any { UserConfig.getInstance(it).clientUserId == account.userId }
            !loggedIn && (account.proxy == null || (account.proxy.type != "socks5" && account.proxy.type != "mtproto"))
        }
    }

    /** True when some not-yet-logged-in account in the pack has no proxy Android can use itself, and pools exist. */
    fun needsProxyPool(file: File): Boolean {
        val count = countWithoutProxy(file)
        if (count == 0) return false
        val names = org.telegram.ui.Components.GgtgProxyPoolStore.getListNames()
        return names.any { org.telegram.ui.Components.GgtgProxyPoolStore.getCount(it) > 0 }
    }

    fun interface ImportCallback {
        fun onResult(success: Boolean, error: String?)
    }

    @JvmStatic
    fun importFileAsync(activity: androidx.fragment.app.FragmentActivity, file: File, callback: ImportCallback) {
        val progress = app.ggtg.ui.TdataImportProgressDialog(activity, "Синхронизация сессий")
        progress.setWorkingText("Применение сессий с Desktop…")
        MainScope().launch {
            val res = import(file, null, progress, false)
            try { progress.dismiss() } catch (ignored: Throwable) {}
            if (res.imported.isNotEmpty() || res.alreadyLoggedIn > 0) {
                callback.onResult(true, null)
            } else {
                callback.onResult(false, res.failReason ?: "Не удалось импортировать сессию")
            }
        }
    }

    suspend fun import(
        file: File,
        proxySource: GgtgProxySource?,
        progressDialog: app.ggtg.ui.TdataImportProgressDialog,
        createBackup: Boolean = false
    ): GgtgImportSupport.ImportResult = GgtgWakeLock.withLock("pack_import") {
        val accounts = withContext(Dispatchers.IO) { readPack(file) }
        if (accounts.isEmpty()) {
            return GgtgImportSupport.ImportResult(emptyList(), 0, 0, 0, "В файле .ggtg нет аккаунтов")
        }

        val toImport = mutableListOf<PackAccount>()
        var alreadyLoggedIn = 0
        for (account in accounts) {
            val exists = (0 until UserConfig.MAX_ACCOUNT_COUNT).any { UserConfig.getInstance(it).clientUserId == account.userId }
            if (exists) {
                alreadyLoggedIn++
                val sNote = if (account.sessionNote.isNotEmpty()) account.sessionNote else "Импортированная сессия (.ggtg)"
                val id = "${System.currentTimeMillis()}_${(1000..9999).random()}"
                val backup = GgtgSessions.StoredSession(
                    id = id,
                    userId = account.userId,
                    role = if (account.sessionRole.isNotEmpty()) account.sessionRole else "imported",
                    source = "import_ggtg",
                    note = sNote,
                    state = "ok",
                    createdAt = System.currentTimeMillis() / 1000,
                    phone = account.phone,
                    name = listOfNotNull(account.firstName, account.lastName).joinToString(" ").trim(),
                    folder = account.folder.orEmpty(),
                    twoFactor = account.twoFactor,
                    mainDcId = account.mainDcId,
                    keys = account.keys,
                    device = account.device,
                    proxy = null
                )
                GgtgSessions.addStored(backup)
                val tacc = app.ggtg.tdata.TdataAccount(
                    userId = account.userId,
                    mainDcId = account.mainDcId,
                    keys = account.keys
                )
                GgtgSessions.saveToVault(backup, tacc)
            } else toImport += account
        }
        // same exclusion as the tdata path: never write into the slot the live login screen is bound to
        val freeSlots = (0 until UserConfig.MAX_ACCOUNT_COUNT)
            .filter { it != UserConfig.selectedAccount && !UserConfig.getInstance(it).isClientActivated }
            .toMutableList()

        // one never-activated slot that holds the Mac's session while it approves this phone's login,
        // reused across accounts (as in the tdata import); without a spare one the keys go in as-is
        val tempSlot = if (toImport.any { it.handoff } && freeSlots.size > 1) freeSlots.removeAt(freeSlots.size - 1) else null

        val imported = mutableListOf<Long>()
        var firstImportedSlot: Int? = null
        var noFreeSlot = 0
        var noFreeProxy = 0
        var failed = 0
        var lastFailReason: String? = null
        val total = toImport.size
        for ((i, account) in toImport.withIndex()) {
            if (i > 0) {
                kotlinx.coroutines.delay(2000)
            }
            val slot = freeSlots.removeFirstOrNull()
            if (slot == null) {
                noFreeSlot += total - i
                break
            }
            // the account's own proxy from the Mac if Android can use it, else a fresh one from the
            // chosen pool; if no pool was chosen, direct/global connection is allowed.
            val proxy = account.proxy?.takeIf { it.type == "socks5" || it.type == "mtproto" }
                ?: proxySource?.popNext()?.let { PackProxy("socks5", it.host, it.port, it.username ?: "", it.password ?: "", "") }
            if (proxy == null && proxySource != null) {
                noFreeProxy++
                freeSlots.add(0, slot)
                continue
            }
            progressDialog.setWorking(account.phone.takeIf { it.isNotEmpty() }?.let { "+$it" }, i + 1, total)
            // If handoff is requested (export from Desktop for Android), execute handoff to mint authentic Android session.
            // Otherwise, install auth keys directly into slot.
            var result = try {
                if (account.handoff && tempSlot != null) {
                    handoff(account, slot, tempSlot, proxy, i + 1, total, progressDialog, keepTransitSession = createBackup)
                } else if (account.keys.isNotEmpty()) {
                    install(account, slot, proxy)
                } else {
                    install(account, slot, proxy)
                }
            } catch (t: Throwable) {
                Result.failure(t)
            }
            if (result.isFailure && account.handoff && tempSlot != null && account.keys.isNotEmpty()
                && result.exceptionOrNull() !is TdataImportHelper.PasswordSkippedException) {
                // If handoff failed, fall back to direct install so the account isn't lost
                FileLog.e("ggtg pack: handoff failed for userId=${account.userId}, falling back to direct install", result.exceptionOrNull())
                try { ConnectionsManager.getInstance(tempSlot).cleanup(true) } catch (_: Throwable) {}
                try { ConnectionsManager.getInstance(slot).cleanup(true) } catch (_: Throwable) {}
                result = try {
                    install(account, slot, proxy)
                } catch (t: Throwable) {
                    Result.failure(t)
                }
            }
            result.onSuccess { userId ->
                imported += userId
                if (firstImportedSlot == null) firstImportedSlot = slot
                FileLog.d("ggtg pack: activated slot=$slot userId=$userId")

                if (createBackup) {
                    val id = "${System.currentTimeMillis()}_${(1000..9999).random()}"
                    val proxyJson = proxy?.let {
                        JSONObject().apply {
                            put("type", it.type)
                            put("host", it.host)
                            put("port", it.port)
                            put("user", it.user)
                            put("password", it.password)
                            put("secret", it.secret)
                        }
                    } ?: JSONObject()
                    val uc = UserConfig.getInstance(slot)
                    val transitDeviceJson = if (account.handoff) {
                        JSONObject().apply {
                            put("deviceModel", "GGTG Transfer")
                            put("systemVersion", "Telegram Desktop")
                            put("appVersion", "GGTG")
                        }
                    } else account.device
                    val sNote = if (account.sessionNote.isNotEmpty()) account.sessionNote
                        else ("Резервная сессия (импорт .ggtg) от " + java.text.SimpleDateFormat("dd.MM.yyyy HH:mm", java.util.Locale.getDefault()).format(java.util.Date()))
                    val backup = GgtgSessions.StoredSession(
                        id = id,
                        userId = account.userId,
                        role = if (account.sessionRole.isNotEmpty()) account.sessionRole else "backup",
                        source = "import_ggtg",
                        note = sNote,
                        state = "ok",
                        createdAt = System.currentTimeMillis() / 1000,
                        phone = uc.currentUser?.phone.orEmpty().ifEmpty { account.phone },
                        name = uc.currentUser?.let { org.telegram.messenger.UserObject.getUserName(it) }.orEmpty(),
                        folder = account.folder.orEmpty(),
                        twoFactor = account.twoFactor,
                        mainDcId = account.mainDcId,
                        keys = account.keys,
                        device = transitDeviceJson,
                        proxy = proxyJson
                    )
                    GgtgSessions.addStored(backup)
                    val tacc = app.ggtg.tdata.TdataAccount(
                        userId = account.userId,
                        mainDcId = account.mainDcId,
                        keys = account.keys
                    )
                    GgtgSessions.saveToVault(backup, tacc)
                }
            }.onFailure { t ->
                failed++
                lastFailReason = "+${account.phone}: ${t.message}"
                FileLog.e("ggtg pack: failed userId=${account.userId}", t)
                ConnectionsManager.getInstance(slot).cleanup(true)
                ConnectionsManager.setProxySettingsForAccount(slot, false, "", 0, "", "", "")
                freeSlots.add(0, slot)
            }
        }

        if (tempSlot != null) {
            ConnectionsManager.getInstance(tempSlot).cleanup(true)
            ConnectionsManager.setProxySettingsForAccount(tempSlot, false, "", 0, "", "", "")
        }
        if (imported.isNotEmpty()) {
            GgtgImportSupport.selectImportedSlotIfIdle(firstImportedSlot)
        }
        GgtgImportSupport.ImportResult(imported, alreadyLoggedIn, noFreeSlot, failed, lastFailReason, noFreeProxy)
    }

    /** This phone makes its own session from the Mac's (see the top of the file), then ends the Mac's one. */
    private suspend fun handoff(
        account: PackAccount,
        slot: Int,
        tempSlot: Int,
        proxy: PackProxy?,
        index: Int,
        total: Int,
        progressDialog: app.ggtg.ui.TdataImportProgressDialog,
        keepTransitSession: Boolean
    ): Result<Long> {
        // the 2FA password from the Mac is what the handoff tries first
        if (account.twoFactor.isNotEmpty()) GgtgTwoFaStore.save(account.userId, account.twoFactor)
        // both slots go through the account's own proxy (if any): the Mac's session was made from that address too
        val bits = if (proxy != null && proxy.host.isNotEmpty()) {
            GgtgProxyBits(proxy.host, proxy.port, proxy.user, proxy.password, proxy.secret)
        } else {
            GgtgProxyBits("", 0, "", "", "")
        }
        val result = TdataImportHelper.duplicateAccountViaHandoff(
            app.ggtg.tdata.TdataAccount(account.userId, account.mainDcId, account.keys),
            tempSlot, slot, bits, bits, index, total, progressDialog, account.device)
        if (result.isSuccess) {
            if (!keepTransitSession) {
                // the transit session was only the way here and user didn't request a backup session: end it
                val (_, error) = with(TdataImportHelper) {
                    ConnectionsManager.getInstance(tempSlot).sendSuspend(TLRPC.TL_auth_logOut(), 0,
                        ConnectionsManager.DEFAULT_DATACENTER_ID, "logOut(transit)")
                } ?: (null to null)
                FileLog.d("ggtg pack: transit session of userId=${account.userId} ended, error=${error?.text}")
            } else {
                FileLog.d("ggtg pack: keeping transit session of userId=${account.userId} as backup session in vault")
            }
            applyMeta(account, slot)
        }
        ConnectionsManager.getInstance(tempSlot).cleanup(true)
        return result
    }

    private fun applyMeta(account: PackAccount, slot: Int) {
        val uc = UserConfig.getInstance(slot)
        account.folder?.let { uc.ncgFolder = it }
        uc.ncgPinned = account.pinned
        if (account.twoFactor.isNotEmpty()) {
            uc.ncgTwoFactor = account.twoFactor
            GgtgTwoFaStore.save(account.userId, account.twoFactor)
        }
        account.spamStatus?.let { sObj ->
            val stateStr = sObj.optString("state")
            val until = sObj.optLong("until", 0L)
            val checkedAt = sObj.optLong("checkedAt", 0L)
            val state = when (stateStr) {
                "temporary" -> GgtgSpamStatus.State.Temporary
                "permanent" -> GgtgSpamStatus.State.Permanent
                else -> GgtgSpamStatus.State.Clear
            }
            if (checkedAt > 0) {
                GgtgSpamStatus.merge(account.userId, GgtgSpamStatus.Status(state, until, checkedAt))
            }
        }
        uc.saveConfig(false)
    }

    private suspend fun install(account: PackAccount, slot: Int, proxy: PackProxy?): Result<Long> = runCatching {
        // proxy first: per-account proxy if provided, else reset to defaults
        if (proxy != null) {
            ConnectionsManager.setProxySettingsForAccount(slot, true, proxy.host, proxy.port, proxy.user, proxy.password, proxy.secret)
        } else {
            ConnectionsManager.setProxySettingsForAccount(slot, false, "", 0, "", "", "")
        }
        // device identity: if the keys come from Telegram Desktop (or tdata fallback), running them as mobile (api_id 6)
        // causes Telegram server to detect an API_ID mismatch on the session, leading to instant spam block.
        // Therefore, keys born on Desktop MUST use isDesktop = true so they connect with api_id 2040 (tdesktop).
        val isDesktop = account.handoff || account.device?.optInt("apiId") == 2040 || account.device?.optString("langPackName") == "tdesktop"
        GgtgDevice.assignNew(slot, account.device, isDesktop = isDesktop)
        val cm = ConnectionsManager.getInstance(slot)
        if (account.mainDcId != 0 && account.mainDcId != ConnectionsManager.DEFAULT_DATACENTER_ID) {
            cm.setDefaultDatacenterId(account.mainDcId)
        }
        for ((dcId, key) in account.keys) {
            cm.importAuthKey(dcId, key, GgtgImportSupport.authKeyIdFor(key), dcId == account.mainDcId)
        }
        cm.setUserId(account.userId)
        // let the imported main-DC key settle before the first request goes out on it
        delay(300)
        val fallback = if (account.phone.isNotEmpty() || account.firstName.isNotEmpty()) {
            TLRPC.TL_user().apply {
                id = account.userId
                first_name = account.firstName.ifEmpty { "GGTG User" }
                last_name = account.lastName
                username = account.username
                phone = account.phone
                status = TLRPC.TL_userStatusEmpty()
                self = true
                flags = 1024 // FLAG_10 = self
                if (first_name.isNotEmpty()) flags = flags or 1
                if (!last_name.isNullOrEmpty()) flags = flags or 2
                if (!username.isNullOrEmpty()) flags = flags or 4
                if (!phone.isNullOrEmpty()) flags = flags or 16
            }
        } else null
        val result = GgtgImportSupport.activateSlot(slot, fallback, account.mainDcId)
        result.onSuccess { applyMeta(account, slot) }
        result.getOrThrow()
    }

    private fun readPack(file: File): List<PackAccount> {
        val manifest = ZipFile(file).use { zip ->
            val entry = zip.getEntry(MANIFEST) ?: throw IllegalArgumentException("нет $MANIFEST")
            JSONObject(zip.getInputStream(entry).bufferedReader().readText())
        }
        require(manifest.optString("kind") == KIND) { "это не экспорт GGTG" }
        val format = manifest.optInt("format", 0)
        require(format in 1..MAX_FORMAT) { "формат $format новее этой версии приложения - обновите GGTG Android" }
        val list = manifest.optJSONArray("accounts") ?: return emptyList()
        val result = mutableListOf<PackAccount>()
        for (i in 0 until list.length()) {
            val json = list.getJSONObject(i)
            val keysJson = json.optJSONArray("keys") ?: continue
            val keys = (0 until keysJson.length()).mapNotNull { k ->
                val item = keysJson.getJSONObject(k)
                val bytes = hexToBytes(item.optString("key"))
                if (bytes?.size == AUTH_KEY_SIZE) item.getInt("dcId") to bytes else null
            }
            val userId = json.optString("userId").toLongOrNull() ?: continue
            if (keys.isEmpty() || userId == 0L) continue
            val proxy = json.optJSONObject("proxy")?.let { p ->
                PackProxy(
                    p.optString("type"),
                    p.optString("host"),
                    p.optInt("port"),
                    p.optString("user"),
                    p.optString("password"),
                    p.optString("secret")
                ).takeIf { it.host.isNotEmpty() && it.port > 0 }
            }
            result += PackAccount(
                userId = userId,
                mainDcId = json.optInt("mainDcId", keys.first().first),
                keys = keys,
                phone = json.optString("phone"),
                firstName = json.optString("firstName"),
                lastName = json.optString("lastName"),
                username = json.optString("username"),
                folder = if (json.has("folder")) json.optString("folder") else null,
                pinned = json.optBoolean("pinned"),
                twoFactor = json.optString("twoFactor"),
                proxy = proxy,
                device = json.optJSONObject("device"),
                handoff = json.optBoolean("handoff"),
                sessionNote = json.optString("sessionNote").ifEmpty { json.optString("note") },
                sessionRole = json.optString("sessionRole"),
                spamStatus = json.optJSONObject("spamStatus")
            )
        }
        return result
    }

    private fun hexToBytes(hex: String): ByteArray? {
        if (hex.length % 2 != 0) return null
        return try {
            ByteArray(hex.length / 2) { hex.substring(it * 2, it * 2 + 2).toInt(16).toByte() }
        } catch (e: NumberFormatException) {
            null
        }
    }
}
