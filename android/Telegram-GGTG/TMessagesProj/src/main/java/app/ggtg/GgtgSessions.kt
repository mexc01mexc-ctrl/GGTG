package app.ggtg

import androidx.fragment.app.FragmentActivity
import app.ggtg.tdata.TdataAccount
import app.ggtg.tdata.TdataWriter
import app.ggtg.ui.TdataImportProgressDialog
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.MainScope
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONArray
import org.json.JSONObject
import org.telegram.messenger.ApplicationLoader
import org.telegram.messenger.FileLog
import org.telegram.messenger.UserConfig
import org.telegram.tgnet.ConnectionsManager
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

// GGTG: Session Vault for Android - mirrors desktop's Core::GgtgSessions (core/ggtg_sessions.h)
// Manages stored backup sessions, duplicates and session swap operations.
object GgtgSessions {

    data class StoredSession(
        val id: String,
        val userId: Long,
        var role: String, // "backup", "duplicate", "previous", "active"
        var source: String,
        var note: String,
        var state: String = "ok", // "ok", "revoked"
        val createdAt: Long = System.currentTimeMillis() / 1000,
        val hash: Long = 0L,
        val phone: String = "",
        val name: String = "",
        val username: String = "",
        val folder: String = "",
        val twoFactor: String = "",
        val mainDcId: Int = 2,
        val keys: List<Pair<Int, ByteArray>> = emptyList(),
        val device: JSONObject? = null,
        val proxy: JSONObject? = null
    )

    private val lock = Any()

    fun vaultRoot(): File {
        return File(File(ApplicationLoader.getFilesDirFixed(), "ggtg_bulk_db"), "session_vault").also { it.mkdirs() }
    }

    private fun indexPath(): File = File(vaultRoot(), "index.json")

    fun listAll(): List<StoredSession> {
        synchronized(lock) {
            val file = indexPath()
            if (!file.exists()) return emptyList()
            return try {
                val raw = file.readText().trim()
                if (raw.isEmpty()) return emptyList()
                val array = if (raw.startsWith("{")) {
                    JSONObject(raw).optJSONArray("sessions") ?: JSONArray()
                } else {
                    JSONArray(raw)
                }
                val list = ArrayList<StoredSession>()
                for (i in 0 until array.length()) {
                    val obj = array.getJSONObject(i)
                    val id = obj.optString("id")
                    val userId = obj.optString("userId").toLongOrNull() ?: 0L
                    if (id.isEmpty() || userId == 0L) continue
                    val keysArr = obj.optJSONArray("keys")
                    val keys = ArrayList<Pair<Int, ByteArray>>()
                    if (keysArr != null) {
                        for (k in 0 until keysArr.length()) {
                            val kObj = keysArr.getJSONObject(k)
                            val dcId = kObj.optInt("dcId")
                            val hex = kObj.optString("key")
                            val b = hexToBytes(hex)
                            if (dcId > 0 && b != null) keys.add(dcId to b)
                        }
                    }
                    list.add(
                        StoredSession(
                            id = id,
                            userId = userId,
                            role = obj.optString("role", "backup"),
                            source = obj.optString("source", "duplicate"),
                            note = obj.optString("note"),
                            state = obj.optString("state", "ok"),
                            createdAt = obj.optLong("createdAt", System.currentTimeMillis() / 1000),
                            hash = obj.optString("hash").toLongOrNull() ?: 0L,
                            phone = obj.optString("phone"),
                            name = obj.optString("name"),
                            username = obj.optString("username"),
                            folder = obj.optString("folder"),
                            twoFactor = obj.optString("twoFactor"),
                            mainDcId = obj.optInt("mainDcId", 2),
                            keys = keys,
                            device = obj.optJSONObject("device"),
                            proxy = obj.optJSONObject("proxy")
                        )
                    )
                }
                list
            } catch (t: Throwable) {
                FileLog.e("GgtgSessions listAll failed", t)
                emptyList()
            }
        }
    }

    fun listForUser(userId: Long): List<StoredSession> {
        return listAll().filter { it.userId == userId }
    }

    fun getBackups(userId: Long): List<StoredSession> {
        return getSwitchableSessions(userId)
    }

    fun getSwitchableSessions(userId: Long): List<StoredSession> {
        return listForUser(userId).filter { s ->
            s.keys.isNotEmpty() ||
            File(vaultRoot(), "${s.userId}/${s.id}/session.json").exists() ||
            File(vaultRoot(), "${s.userId}/${s.id}/tdata").exists() ||
            s.role in listOf("backup", "duplicate", "imported", "exported", "desktop", "previous", "android")
        }
    }

    fun roleTitle(role: String): String {
        return when (role) {
            "backup" -> "Резервная"
            "duplicate" -> "Дублированная"
            "imported" -> "Импортированная"
            "exported" -> "Экспортированная"
            "desktop" -> "Десктоп"
            "android" -> "Android"
            "previous" -> "Бывшая рабочая"
            "active" -> "Активная"
            else -> "Сохранённая"
        }
    }

    fun switchWarning(session: StoredSession): String? {
        val isDesktop = session.role == "desktop" || session.source.contains("desktop") ||
            session.device?.optString("deviceModel", "")?.contains("Mac", ignoreCase = true) == true ||
            session.device?.optString("deviceModel", "")?.contains("Windows", ignoreCase = true) == true ||
            session.device?.optString("deviceModel", "")?.contains("PC", ignoreCase = true) == true
        if (isDesktop) {
            return "Эта сессия создана на Desktop (компьютере). Если аккаунт сейчас активен на компьютере, переключение на телефоне вызовет ошибку AUTH_KEY_DUPLICATED и разрыв сессии в обоих местах. Перед переключением убедитесь, что приложение на компьютере закрыто."
        }
        if (session.role == "exported" || session.source.contains("export")) {
            return "Эта сессия была экспортирована. Если она сейчас активна на другом устройстве, одновременная работа вызовет разрыв сессии (AUTH_KEY_DUPLICATED)."
        }
        if (session.role == "duplicate") {
            return "Это дублированная сессия. Если она где-то уже работает, после переключения Telegram разорвёт её в обоих местах."
        }
        return null
    }

    fun hasBackup(userId: Long): Boolean {
        return getSwitchableSessions(userId).isNotEmpty()
    }

    fun addStored(entry: StoredSession) {
        synchronized(lock) {
            val all = listAll().toMutableList()
            all.removeAll { it.id == entry.id }
            all.add(0, entry)
            saveIndex(all)
        }
    }

    fun updateRole(id: String, newRole: String, note: String = "") {
        synchronized(lock) {
            val all = listAll().toMutableList()
            val item = all.firstOrNull { it.id == id } ?: return
            item.role = newRole
            if (note.isNotEmpty()) item.note = note
            saveIndex(all)
        }
    }

    fun updateHash(id: String, newHash: Long) {
        synchronized(lock) {
            val all = listAll().toMutableList()
            val item = all.firstOrNull { it.id == id } ?: return
            if (item.hash != newHash) {
                val updated = item.copy(hash = newHash)
                val idx = all.indexOf(item)
                all[idx] = updated
                saveIndex(all)
            }
        }
    }

    fun findMatching(userId: Long, hash: Long, deviceModel: String?): StoredSession? {
        val list = listForUser(userId)
        if (list.isEmpty()) return null
        if (hash != 0L) {
            val byHash = list.firstOrNull { it.hash == hash }
            if (byHash != null) return byHash
        }
        val cleanModel = deviceModel?.trim().orEmpty()
        if (cleanModel.isNotEmpty()) {
            val byDevice = list.firstOrNull { s ->
                val sModel = s.device?.optString("deviceModel")?.trim().orEmpty()
                sModel.isNotEmpty() && (sModel.equals(cleanModel, ignoreCase = true) ||
                    cleanModel.contains(sModel, ignoreCase = true) ||
                    sModel.contains(cleanModel, ignoreCase = true))
            }
            if (byDevice != null) {
                if (byDevice.hash == 0L && hash != 0L) {
                    updateHash(byDevice.id, hash)
                }
                return byDevice
            }
        }
        return null
    }

    fun removeStored(id: String) {
        synchronized(lock) {
            val all = listAll().toMutableList()
            val item = all.firstOrNull { it.id == id } ?: return
            all.remove(item)
            saveIndex(all)
            try {
                File(vaultRoot(), "${item.userId}/${item.id}").deleteRecursively()
            } catch (_: Throwable) { }
        }
    }

    private fun saveIndex(list: List<StoredSession>) {
        val array = JSONArray()
        for (item in list) {
            val obj = JSONObject().apply {
                put("id", item.id)
                put("userId", item.userId.toString())
                put("role", item.role)
                put("source", item.source)
                put("note", item.note)
                put("state", item.state)
                put("createdAt", item.createdAt)
                put("hash", item.hash.toString())
                put("phone", item.phone)
                put("name", item.name)
                put("username", item.username)
                put("folder", item.folder)
                if (item.twoFactor.isNotEmpty()) put("twoFactor", item.twoFactor)
                put("mainDcId", item.mainDcId)
                val keysArr = JSONArray()
                for ((dcId, key) in item.keys) {
                    keysArr.put(JSONObject().apply {
                        put("dcId", dcId)
                        put("key", key.joinToString("") { "%02x".format(it) })
                    })
                }
                put("keys", keysArr)
                item.device?.let { put("device", it) }
                item.proxy?.let { put("proxy", it) }
            }
            array.put(obj)
        }
        val root = JSONObject().apply {
            put("version", 1)
            put("sessions", array)
        }
        indexPath().writeText(root.toString(2))
    }

    fun saveToVault(stored: StoredSession, account: TdataAccount) {
        val vaultDir = File(vaultRoot(), "${stored.userId}/${stored.id}").also { it.mkdirs() }

        // 1. Write tdata folder
        val tdataDir = File(vaultDir, "tdata")
        try {
            TdataWriter.write(account, tdataDir)
        } catch (t: Throwable) {
            FileLog.e("saveToVault write tdata error", t)
        }

        // 2. Write ggtg.json
        val ggtg = JSONObject().apply {
            put("id", stored.id)
            put("userId", stored.userId.toString())
            put("role", stored.role)
            put("phone", stored.phone)
            put("name", stored.name)
            put("username", stored.username)
            put("folder", stored.folder)
            if (stored.twoFactor.isNotEmpty()) put("twoFactor", stored.twoFactor)
            put("hash", stored.hash.toString())
            put("createdAt", stored.createdAt)
            stored.device?.let { put("device", it) }
            stored.proxy?.let { put("proxy", it) }
        }
        File(vaultDir, "ggtg.json").writeText(ggtg.toString(2))

        // 3. Write session.json
        val sessJson = JSONObject().apply {
            put("userId", stored.userId.toString())
            put("mainDcId", stored.mainDcId)
            val keysArr = JSONArray()
            for ((dcId, key) in stored.keys) {
                keysArr.put(JSONObject().apply {
                    put("dcId", dcId)
                    put("key", key.joinToString("") { "%02x".format(it) })
                })
            }
            put("keys", keysArr)
        }
        File(vaultDir, "session.json").writeText(sessJson.toString(2))

        // 4. Write 2fa.txt
        if (stored.twoFactor.isNotEmpty()) {
            File(vaultDir, "2fa.txt").writeText(stored.twoFactor)
        }
    }

    /** Creates an independent backup session for [slot] */
    fun createBackupSession(
        activity: FragmentActivity,
        slot: Int,
        progress: TdataImportProgressDialog? = null,
        onDone: (Result<StoredSession>) -> Unit
    ) {
        val uc = UserConfig.getInstance(slot)
        val userId = uc.clientUserId
        if (userId == 0L) {
            onDone(Result.failure(Exception("Аккаунт не авторизован")))
            return
        }
        val label = GgtgAccounts.label(slot)
        val proxy = ownProxy(slot)
        if (proxy == null) {
            onDone(Result.failure(Exception("У аккаунта $label не задан прокси — создание резервной сессии требует прокси")))
            return
        }
        val tempSlot = (0 until UserConfig.MAX_ACCOUNT_COUNT)
            .firstOrNull { it != UserConfig.selectedAccount && !UserConfig.getInstance(it).isClientActivated && it != slot }
        if (tempSlot == null) {
            onDone(Result.failure(Exception("Нет свободного слота аккаунта для дублирования — освободите один слот")))
            return
        }

        MainScope().launch {
            try {
                var acceptedHash = 0L
                val user = uc.currentUser
                val phone = user?.phone.orEmpty()
                val phoneGeo = GgtgGeo.matchForPhone(phone)
                val profile = GgtgDesktopProfiles.random(phone)
                val dupResult = TdataExportHelper.duplicateSession(
                    sourceSlot = slot,
                    tempSlot = tempSlot,
                    proxy = proxy,
                    progress = progress,
                    label = label,
                    desktopProfile = profile,
                    onAccepted = { hash -> acceptedHash = hash }
                )
                ConnectionsManager.getInstance(tempSlot).cleanup(true)
                ConnectionsManager.setProxySettingsForAccount(tempSlot, false, "", 0, "", "", "")

                dupResult.onSuccess { account ->
                    val id = "${System.currentTimeMillis()}_${(1000..9999).random()}"
                    val name = listOfNotNull(user?.first_name, user?.last_name).joinToString(" ").trim()
                    val username = user?.username.orEmpty()
                    val folder = uc.ncgFolder.orEmpty()
                    val twoFactor = uc.ncgTwoFactor.orEmpty()

                    // If acceptedHash is 0, query active authorizations to locate the new session by device model
                    if (acceptedHash == 0L) {
                        try {
                            val sourceCm = ConnectionsManager.getInstance(slot)
                            repeat(3) { attempt ->
                                if (acceptedHash != 0L) return@repeat
                                if (attempt > 0) delay(400L * attempt)
                                val (authsResp, _) = with(TdataImportHelper) {
                                    sourceCm.sendSuspend(org.telegram.tgnet.tl.TL_account.getAuthorizations(), 0, ConnectionsManager.DEFAULT_DATACENTER_ID, "dup getAuthorizations")
                                } ?: (null to null)
                                if (authsResp is org.telegram.tgnet.tl.TL_account.authorizations) {
                                    for (auth in authsResp.authorizations) {
                                        if (auth.device_model == profile.deviceModel) {
                                            acceptedHash = auth.hash
                                            break
                                        }
                                    }
                                }
                            }
                        } catch (_: Throwable) { }
                    }

                    val device = JSONObject().apply {
                        put("deviceModel", profile.deviceModel)
                        put("systemVersion", profile.systemVersion)
                        put("appVersion", profile.appVersion)
                        put("systemLangCode", phoneGeo.systemLang)
                        put("cloudLangCode", phoneGeo.cloudLang)
                        put("tzOffset", phoneGeo.tzOffset)
                        put("langPackName", GgtgDesktopProfiles.LANG_PACK)
                        put("apiId", GgtgDesktopProfiles.APP_ID)
                        put("apiHash", GgtgDesktopProfiles.APP_HASH)
                    }
                    val proxyJson = JSONObject().apply {
                        put("type", "socks5")
                        put("host", proxy.address)
                        put("port", proxy.port)
                        put("user", proxy.username)
                        put("password", proxy.password)
                        if (proxy.secret.isNotEmpty()) put("secret", proxy.secret)
                    }

                    val stored = StoredSession(
                        id = id,
                        userId = userId,
                        role = "backup",
                        source = "duplicate",
                        note = "Резервная (${profile.deviceModel}) от " + SimpleDateFormat("dd.MM.yyyy HH:mm", Locale.getDefault()).format(Date()),
                        state = "ok",
                        createdAt = System.currentTimeMillis() / 1000,
                        hash = acceptedHash,
                        phone = phone,
                        name = name,
                        username = username,
                        folder = folder,
                        twoFactor = twoFactor,
                        mainDcId = account.mainDcId,
                        keys = account.keys,
                        device = device,
                        proxy = proxyJson
                    )
                    withContext(Dispatchers.IO) {
                        saveToVault(stored, account)
                    }
                    addStored(stored)
                    onDone(Result.success(stored))
                }.onFailure {
                    onDone(Result.failure(it))
                }
            } catch (t: Throwable) {
                ConnectionsManager.getInstance(tempSlot).cleanup(true)
                ConnectionsManager.setProxySettingsForAccount(tempSlot, false, "", 0, "", "", "")
                onDone(Result.failure(t))
            }
        }
    }

    @JvmStatic
    fun swapToBackupAsync(slot: Int, targetId: String?, callback: (Boolean, String?) -> Unit) {
        MainScope().launch {
            val res = swapToBackup(slot, targetId)
            res.onSuccess { msg -> callback(true, msg) }
                .onFailure { err -> callback(false, err.message) }
        }
    }

    /** Swaps the working session on [slot] with its stored backup session */
    suspend fun swapToBackup(slot: Int, targetId: String? = null): Result<String> {
        val uc = UserConfig.getInstance(slot)
        val userId = uc.clientUserId
        if (userId == 0L) return Result.failure(Exception("Аккаунт не авторизован"))
        val sessions = getSwitchableSessions(userId)
        if (sessions.isEmpty()) return Result.failure(Exception("Для аккаунта +${uc.currentUser?.phone} нет доступных сессий"))
        val target = if (targetId != null) sessions.firstOrNull { it.id == targetId } else sessions.firstOrNull()
        if (target == null) return Result.failure(Exception("Сессия не найдена"))

        try {
            val currentAcc = TdataExportHelper.currentSession(slot)
            val oldId = "${System.currentTimeMillis()}_${(1000..9999).random()}"
            val user = uc.currentUser
            val currentProxy = ownProxy(slot)
            val currentProxyJson = currentProxy?.let { p ->
                JSONObject().apply {
                    put("type", "socks5")
                    put("host", p.address)
                    put("port", p.port)
                    put("user", p.username)
                    put("password", p.password)
                    if (p.secret.isNotEmpty()) put("secret", p.secret)
                }
            }
            val oldStored = StoredSession(
                id = oldId,
                userId = userId,
                role = "previous",
                source = "android",
                note = "Бывшая рабочая от " + SimpleDateFormat("dd.MM.yyyy HH:mm", Locale.getDefault()).format(Date()),
                state = "ok",
                createdAt = System.currentTimeMillis() / 1000,
                hash = 0L,
                phone = user?.phone.orEmpty(),
                name = listOfNotNull(user?.first_name, user?.last_name).joinToString(" ").trim(),
                username = user?.username.orEmpty(),
                folder = uc.ncgFolder.orEmpty(),
                twoFactor = uc.ncgTwoFactor.orEmpty(),
                mainDcId = currentAcc.mainDcId,
                keys = currentAcc.keys,
                device = GgtgDevice.getDeviceJson(slot),
                proxy = currentProxyJson
            )
            withContext(Dispatchers.IO) {
                saveToVault(oldStored, currentAcc)
            }
            addStored(oldStored)
        } catch (_: Throwable) { }

        val cm = ConnectionsManager.getInstance(slot)
        cm.cleanup(true)

        if (target.device != null) {
            GgtgDevice.assignNew(slot, target.device)
        }

        if (target.proxy != null) {
            val host = target.proxy.optString("host")
            val port = target.proxy.optInt("port")
            val user = target.proxy.optString("user")
            val pass = target.proxy.optString("password")
            val secret = target.proxy.optString("secret")
            if (host.isNotEmpty() && port > 0) {
                ConnectionsManager.setProxySettingsForAccount(slot, true, host, port, user, pass, secret)
                uc.ncgProxyEnabled = true
                uc.ncgProxyAddress = host
                uc.ncgProxyPort = port
                uc.ncgProxyUsername = user
                uc.ncgProxyPassword = pass
                uc.ncgProxySecret = secret
                uc.saveConfig(false)
            }
        }

        var keys = target.keys
        var mainDcId = target.mainDcId
        if (keys.isEmpty()) {
            val sessFile = File(vaultRoot(), "${target.userId}/${target.id}/session.json")
            if (sessFile.exists()) {
                try {
                    val sObj = JSONObject(sessFile.readText())
                    mainDcId = sObj.optInt("mainDcId", mainDcId)
                    val kArr = sObj.optJSONArray("keys")
                    if (kArr != null) {
                        val parsed = ArrayList<Pair<Int, ByteArray>>()
                        for (k in 0 until kArr.length()) {
                            val ko = kArr.getJSONObject(k)
                            val d = ko.optInt("dcId")
                            val b = hexToBytes(ko.optString("key"))
                            if (d > 0 && b != null) parsed.add(d to b)
                        }
                        keys = parsed
                    }
                } catch (_: Throwable) { }
            }
        }
        if (keys.isEmpty()) {
            val tdataDir = File(vaultRoot(), "${target.userId}/${target.id}/tdata")
            if (tdataDir.exists()) {
                try {
                    val accs = app.ggtg.tdata.TdataReader.readOneTdataFolder(tdataDir)
                    val acc = accs.firstOrNull()
                    if (acc != null && acc.keys.isNotEmpty()) {
                        keys = acc.keys
                        mainDcId = acc.mainDcId
                    }
                } catch (_: Throwable) { }
            }
        }
        if (keys.isEmpty()) {
            return Result.failure(Exception("Ключи авторизации для сессии не найдены"))
        }

        for ((dcId, key) in keys) {
            cm.importAuthKey(dcId, key, GgtgImportSupport.authKeyIdFor(key), dcId == mainDcId)
        }
        cm.setUserId(target.userId)

        delay(300)
        val res = GgtgImportSupport.activateSlot(slot, null)
        if (res.isFailure) {
            return Result.failure(res.exceptionOrNull() ?: Exception("Не удалось активировать слот"))
        }

        updateRole(target.id, target.role, "Активирована в приложении")
        return Result.success("Аккаунт успешно переключён на выбранную сессию!")
    }

    /** Restores an account from its stored backup session into a free slot */
    suspend fun restoreBackup(userId: Long, targetId: String? = null): Result<Int> {
        val backups = getBackups(userId)
        if (backups.isEmpty()) return Result.failure(Exception("Для этого аккаунта нет резервной сессии"))
        val target = if (targetId != null) backups.firstOrNull { it.id == targetId } else backups.firstOrNull()
        if (target == null) return Result.failure(Exception("Резервная сессия не найдена"))

        val freeSlot = (0 until UserConfig.MAX_ACCOUNT_COUNT)
            .firstOrNull { !UserConfig.getInstance(it).isClientActivated }
            ?: return Result.failure(Exception("Нет свободного слота аккаунта (максимум ${UserConfig.MAX_ACCOUNT_COUNT})"))

        val cm = ConnectionsManager.getInstance(freeSlot)
        cm.cleanup(true)

        if (target.device != null) {
            GgtgDevice.assignNew(freeSlot, target.device)
        }

        if (target.proxy != null) {
            val host = target.proxy.optString("host")
            val port = target.proxy.optInt("port")
            val user = target.proxy.optString("user")
            val pass = target.proxy.optString("password")
            val secret = target.proxy.optString("secret")
            if (host.isNotEmpty() && port > 0) {
                ConnectionsManager.setProxySettingsForAccount(freeSlot, true, host, port, user, pass, secret)
                val uc = UserConfig.getInstance(freeSlot)
                uc.ncgProxyEnabled = true
                uc.ncgProxyAddress = host
                uc.ncgProxyPort = port
                uc.ncgProxyUsername = user
                uc.ncgProxyPassword = pass
                uc.ncgProxySecret = secret
            }
        }

        var keys = target.keys
        var mainDcId = target.mainDcId
        if (keys.isEmpty()) {
            val sessFile = File(vaultRoot(), "${target.userId}/${target.id}/session.json")
            if (sessFile.exists()) {
                try {
                    val sObj = JSONObject(sessFile.readText())
                    mainDcId = sObj.optInt("mainDcId", mainDcId)
                    val kArr = sObj.optJSONArray("keys")
                    if (kArr != null) {
                        val parsed = ArrayList<Pair<Int, ByteArray>>()
                        for (k in 0 until kArr.length()) {
                            val ko = kArr.getJSONObject(k)
                            val d = ko.optInt("dcId")
                            val b = hexToBytes(ko.optString("key"))
                            if (d > 0 && b != null) parsed.add(d to b)
                        }
                        keys = parsed
                    }
                } catch (_: Throwable) { }
            }
        }
        if (keys.isEmpty()) {
            val tdataDir = File(vaultRoot(), "${target.userId}/${target.id}/tdata")
            if (tdataDir.exists()) {
                try {
                    val accs = app.ggtg.tdata.TdataReader.readOneTdataFolder(tdataDir)
                    val acc = accs.firstOrNull()
                    if (acc != null && acc.keys.isNotEmpty()) {
                        keys = acc.keys
                        mainDcId = acc.mainDcId
                    }
                } catch (_: Throwable) { }
            }
        }
        if (keys.isEmpty()) {
            return Result.failure(Exception("Ключи авторизации для резервной сессии не найдены"))
        }

        for ((dcId, key) in keys) {
            cm.importAuthKey(dcId, key, GgtgImportSupport.authKeyIdFor(key), dcId == mainDcId)
        }
        cm.setUserId(target.userId)

        delay(300)
        val res = GgtgImportSupport.activateSlot(freeSlot, null)
        if (res.isFailure) {
            return Result.failure(res.exceptionOrNull() ?: Exception("Не удалось активировать слот"))
        }

        val uc = UserConfig.getInstance(freeSlot)
        if (target.folder.isNotEmpty()) {
            uc.ncgFolder = target.folder
        }
        if (target.twoFactor.isNotEmpty()) {
            uc.ncgTwoFactor = target.twoFactor
        }
        uc.saveConfig(false)
        GgtgImportSupport.selectImportedSlotIfIdle(freeSlot)

        updateRole(target.id, "active", "Активирована в приложении")
        GgtgSessionLog.removeUser(userId)
        return Result.success(freeSlot)
    }

    private fun ownProxy(slot: Int): GgtgProxyBits? {
        val uc = UserConfig.getInstance(slot)
        if (!uc.ncgProxyEnabled || uc.ncgProxyAddress.isEmpty() || uc.ncgProxyPort <= 0) return null
        return GgtgProxyBits(uc.ncgProxyAddress, uc.ncgProxyPort, uc.ncgProxyUsername, uc.ncgProxyPassword, uc.ncgProxySecret)
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

