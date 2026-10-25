package app.ggtg

import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
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
        val handoff: Boolean
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

    /** True when some not-yet-logged-in account in the pack has no proxy Android can use itself, and pools exist. */
    fun needsProxyPool(file: File): Boolean {
        val accounts = try { readPack(file) } catch (t: Throwable) { return false }
        val names = org.telegram.ui.Components.GgtgProxyPoolStore.getListNames()
        val hasPools = names.any { org.telegram.ui.Components.GgtgProxyPoolStore.getCount(it) > 0 }
        if (!hasPools) return false
        return accounts.any { account ->
            val loggedIn = (0 until UserConfig.MAX_ACCOUNT_COUNT).any { UserConfig.getInstance(it).clientUserId == account.userId }
            !loggedIn && (account.proxy == null || (account.proxy.type != "socks5" && account.proxy.type != "mtproto"))
        }
    }

    suspend fun import(
        file: File,
        proxySource: GgtgProxySource?,
        progressDialog: app.ggtg.ui.TdataImportProgressDialog,
        createBackup: Boolean = false
    ): GgtgImportSupport.ImportResult {
        val accounts = withContext(Dispatchers.IO) { readPack(file) }
        if (accounts.isEmpty()) {
            return GgtgImportSupport.ImportResult(emptyList(), 0, 0, 0, "В файле .ggtg нет аккаунтов")
        }

        val toImport = mutableListOf<PackAccount>()
        var alreadyLoggedIn = 0
        for (account in accounts) {
            val exists = (0 until UserConfig.MAX_ACCOUNT_COUNT).any { UserConfig.getInstance(it).clientUserId == account.userId }
            if (exists) alreadyLoggedIn++ else toImport += account
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
            // If the pack contains auth keys (standard .ggtg export / sync), install them directly.
            // Only legacy packs without keys fall back to handoff.
            var result = try {
                if (account.keys.isNotEmpty()) {
                    install(account, slot, proxy)
                } else if (account.handoff && tempSlot != null) {
                    if (proxy == null) {
                        Result.failure(Exception("Требуется прокси для передачи сессии"))
                    } else {
                        handoff(account, slot, tempSlot, proxy, i + 1, total, progressDialog, keepTransitSession = createBackup)
                    }
                } else {
                    install(account, slot, proxy)
                }
            } catch (t: Throwable) {
                Result.failure(t)
            }
            if (result.isFailure && account.handoff && tempSlot != null && account.keys.isNotEmpty() && proxy != null
                && result.exceptionOrNull() !is TdataImportHelper.PasswordSkippedException) {
                // If direct install failed, try handoff as fallback
                FileLog.e("ggtg pack: direct install failed for userId=${account.userId}, trying handoff", result.exceptionOrNull())
                try { ConnectionsManager.getInstance(slot).cleanup(true) } catch (_: Throwable) {}
                result = try {
                    handoff(account, slot, tempSlot, proxy, i + 1, total, progressDialog, keepTransitSession = createBackup)
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
                    val backup = GgtgSessions.StoredSession(
                        id = id,
                        userId = account.userId,
                        role = "backup",
                        source = "import_ggtg",
                        note = "Резервная сессия (импорт .ggtg) от " + java.text.SimpleDateFormat("dd.MM.yyyy HH:mm", java.util.Locale.getDefault()).format(java.util.Date()),
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
        return GgtgImportSupport.ImportResult(imported, alreadyLoggedIn, noFreeSlot, failed, lastFailReason, noFreeProxy)
    }

    /** This phone makes its own session from the Mac's (see the top of the file), then ends the Mac's one. */
    private suspend fun handoff(
        account: PackAccount,
        slot: Int,
        tempSlot: Int,
        proxy: PackProxy,
        index: Int,
        total: Int,
        progressDialog: app.ggtg.ui.TdataImportProgressDialog,
        keepTransitSession: Boolean
    ): Result<Long> {
        // the 2FA password from the Mac is what the handoff tries first
        if (account.twoFactor.isNotEmpty()) GgtgTwoFaStore.save(account.userId, account.twoFactor)
        // both slots go through the account's own proxy: the Mac's session was made from that address too
        val bits = GgtgProxyBits(proxy.host, proxy.port, proxy.user, proxy.password, proxy.secret)
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
        uc.saveConfig(false)
    }

    private suspend fun install(account: PackAccount, slot: Int, proxy: PackProxy?): Result<Long> = runCatching {
        // proxy first: per-account proxy if provided, else reset to defaults
        if (proxy != null) {
            ConnectionsManager.setProxySettingsForAccount(slot, true, proxy.host, proxy.port, proxy.user, proxy.password, proxy.secret)
        } else {
            ConnectionsManager.setProxySettingsForAccount(slot, false, "", 0, "", "", "")
        }
        // device identity: the export's own, else a random phone (set before the ConnectionsManager exists)
        GgtgDevice.assignNew(slot, account.device)
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
                handoff = json.optBoolean("handoff")
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
