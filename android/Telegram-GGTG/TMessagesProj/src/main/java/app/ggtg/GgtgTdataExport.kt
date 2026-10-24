package app.ggtg

import android.content.ContentValues
import android.database.sqlite.SQLiteDatabase
import android.net.Uri
import androidx.activity.result.ActivityResultLauncher
import androidx.activity.result.contract.ActivityResultContracts
import androidx.fragment.app.FragmentActivity
import app.ggtg.TdataImportHelper.sendSuspend
import app.ggtg.tdata.TdataAccount
import app.ggtg.tdata.TdataWriter
import app.ggtg.ui.TdataImportProgressDialog
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.MainScope
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.telegram.messenger.ApplicationLoader
import org.telegram.messenger.BuildVars
import org.telegram.messenger.FileLog
import org.telegram.messenger.UserConfig
import org.telegram.tgnet.ConnectionsManager
import org.telegram.tgnet.TLRPC
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream
import org.json.JSONArray
import org.json.JSONObject

// Exports logged-in Android accounts as Telegram Desktop "tdata" folders (one zip, a folder per account),
// the inverse of TdataImportHelper - same two variants as GGTG macOS:
//  1) "Обычный": the account's own session keys go into tdata as-is (the session is then shared with
//     whatever opens that tdata). No 2FA involved - the account is already authorized.
//  2) "С дубликацией": a brand-new independent session is minted first (auth.exportLoginToken on a
//     throwaway slot approved by the account via auth.acceptLoginToken), and only that new key is
//     exported - the phone's own session stays separate. Needs a proxy for the throwaway slot, and
//     if the account has a cloud password, that approval needs it too (see resolvePasswordAndAuthorize) -
//     a saved password (GgtgTwoFaStore, shared with tdata import) is tried first, then the same
//     progress window prompts for it (or lets the account be skipped) exactly like import does.
object TdataExportHelper {
    private const val STAGING_DIR = "tdata_export_staging"

    private lateinit var saveLauncher: ActivityResultLauncher<String>
    private var savedCallback: ((Uri?) -> Unit)? = null

    enum class ExportFormat {
        TData,
        SessionJson,
        GgtgPack
    }

    enum class SessionSource {
        NewDuplicate,
        WorkingSwap,
        Backup,
        WorkingCopy
    }

    data class ExportOptions(
        val format: ExportFormat = ExportFormat.TData,
        val source: SessionSource = SessionSource.NewDuplicate,
        val newWhenNoBackup: Boolean = true,
        val sidecars: Boolean = true
    )

    @JvmStatic
    fun registerResultCallbacks(activity: FragmentActivity) {
        saveLauncher = activity.registerForActivityResult(ActivityResultContracts.CreateDocument("*/*")) { uri: Uri? ->
            savedCallback?.invoke(uri)
        }
    }

    /** The account's own proxy: the throwaway "new device" of a duplication goes out the same way the account does
     *  (as on the Mac), never through another address. Null when the account has none. */
    private fun ownProxy(slot: Int): GgtgProxyBits? {
        val uc = UserConfig.getInstance(slot)
        if (!uc.ncgProxyEnabled || uc.ncgProxyAddress.isEmpty() || uc.ncgProxyPort <= 0) return null
        return GgtgProxyBits(uc.ncgProxyAddress, uc.ncgProxyPort, uc.ncgProxyUsername, uc.ncgProxyPassword, uc.ncgProxySecret)
    }

    private val NO_PROXY = Exception("у аккаунта не задан прокси - дублирование сессии никогда не идёт напрямую (задайте его в карточке аккаунта)")

    /** Thrown when the user chose to skip an account rather than provide its 2FA password. */
    private class PasswordSkippedException : Exception("2FA password not provided")

    /** [proxySource] is required (and only used) when [duplicate] is true. [onDone] gets a user-facing summary. */
    @JvmStatic
    fun export(activity: FragmentActivity, slots: List<Int>, duplicate: Boolean, proxySource: GgtgProxySource?, onDone: (String) -> Unit) {
        val options = ExportOptions(
            format = ExportFormat.TData,
            source = if (duplicate) SessionSource.NewDuplicate else SessionSource.WorkingCopy,
            newWhenNoBackup = true,
            sidecars = true
        )
        exportWithOptions(activity, slots, options, onDone)
    }

    @JvmStatic
    fun exportWithOptions(
        activity: FragmentActivity,
        slots: List<Int>,
        options: ExportOptions,
        onDone: (String) -> Unit
    ) {
        val progress = TdataImportProgressDialog(activity, "Экспорт аккаунтов")
        progress.setWorkingText("Подготовка…")
        MainScope().launch {
            val stage = File(ApplicationLoader.applicationContext.cacheDir, STAGING_DIR)
            try {
                withContext(Dispatchers.IO) { stage.deleteRecursively(); stage.mkdirs() }
                val failures = ArrayList<String>()
                var done = 0
                var passwordSkipped = 0
                val swapSlots = ArrayList<Int>()
                val manifestAccounts = JSONArray()

                val needTempSlot = (options.source == SessionSource.NewDuplicate || (options.source == SessionSource.Backup && options.newWhenNoBackup))
                val tempSlot = if (needTempSlot) {
                    (0 until UserConfig.MAX_ACCOUNT_COUNT)
                        .firstOrNull { it != UserConfig.selectedAccount && !UserConfig.getInstance(it).isClientActivated && it !in slots }
                        ?: run { progress.dismiss(); onDone("Нет свободного слота для дублирования сессии — освободите один слот аккаунта."); return@launch }
                } else -1

                for ((i, slot) in slots.withIndex()) {
                    val label = GgtgAccounts.label(slot)
                    val uc = UserConfig.getInstance(slot)
                    val userId = uc.clientUserId
                    progress.setWorkingText("Экспорт $label (${i + 1} из ${slots.size})…")

                    val result: Result<TdataAccount> = try {
                        when (options.source) {
                            SessionSource.NewDuplicate -> {
                                val proxy = ownProxy(slot)
                                if (proxy == null) Result.failure(NO_PROXY)
                                else duplicateSession(slot, tempSlot, proxy, progress, label)
                            }
                            SessionSource.Backup -> {
                                val backups = GgtgSessions.getBackups(userId)
                                if (backups.isNotEmpty()) {
                                    val b = backups.first()
                                    GgtgSessions.updateRole(b.id, "duplicate", "Выдана в экспорт")
                                    Result.success(TdataAccount(b.userId, b.mainDcId, b.keys))
                                } else if (options.newWhenNoBackup) {
                                    val proxy = ownProxy(slot)
                                    if (proxy == null) Result.failure(NO_PROXY)
                                    else duplicateSession(slot, tempSlot, proxy, progress, label)
                                } else {
                                    Result.failure(Exception("нет резервной сессии"))
                                }
                            }
                            SessionSource.WorkingSwap -> {
                                val hasBackup = GgtgSessions.hasBackup(userId)
                                if (!hasBackup && !options.newWhenNoBackup) {
                                    Result.failure(Exception("нет резервной сессии для переключения"))
                                } else {
                                    if (hasBackup) swapSlots.add(slot)
                                    Result.success(currentSession(slot))
                                }
                            }
                            SessionSource.WorkingCopy -> {
                                Result.success(currentSession(slot))
                            }
                        }
                    } catch (t: Throwable) {
                        Result.failure(t)
                    }

                    result.onSuccess { account ->
                        withContext(Dispatchers.IO) {
                            val safeName = "${i + 1}_" + label.filter { c -> c.isLetterOrDigit() || c == '+' || c == '_' }.take(40)
                            val accDir = File(stage, safeName).also { it.mkdirs() }

                            val phone = uc.currentUser?.phone.orEmpty()
                            val phoneGeo = GgtgGeo.matchForPhone(phone)
                            val isAccountIos = uc.ncgDeviceModel.contains("iPhone") || uc.ncgSystemVersion.contains("iOS")
                            val dev = if (isAccountIos) {
                                val iosProfile = GgtgIosProfiles.random(phone)
                                JSONObject().apply {
                                    put("deviceModel", if (uc.ncgDeviceModel.isNotEmpty()) uc.ncgDeviceModel else iosProfile.deviceModel)
                                    put("systemVersion", if (uc.ncgSystemVersion.isNotEmpty()) uc.ncgSystemVersion else iosProfile.systemVersion)
                                    put("appVersion", if (uc.ncgAppVersion.isNotEmpty()) uc.ncgAppVersion else iosProfile.appVersion)
                                    put("systemLangCode", uc.ncgSystemLang.ifEmpty { phoneGeo.systemLang.lowercase() })
                                    put("cloudLangCode", uc.ncgCloudLang.ifEmpty { phoneGeo.cloudLang })
                                    put("tzOffset", if (uc.ncgTzSet) uc.ncgTzOffset else phoneGeo.tzOffset)
                                    put("langPackName", GgtgIosProfiles.LANG_PACK)
                                    put("apiId", GgtgIosProfiles.APP_ID)
                                    put("apiHash", GgtgIosProfiles.APP_HASH)
                                }
                            } else {
                                val androidProfile = GgtgAndroidProfiles.random()
                                JSONObject().apply {
                                    put("deviceModel", if (uc.ncgDeviceModel.isNotEmpty()) uc.ncgDeviceModel else androidProfile.deviceModel)
                                    put("systemVersion", if (uc.ncgSystemVersion.isNotEmpty()) uc.ncgSystemVersion else androidProfile.systemVersion)
                                    put("appVersion", if (uc.ncgAppVersion.isNotEmpty()) uc.ncgAppVersion else androidProfile.appVersion)
                                    put("systemLangCode", uc.ncgSystemLang.ifEmpty { phoneGeo.systemLang.lowercase() })
                                    put("cloudLangCode", uc.ncgCloudLang.ifEmpty { phoneGeo.cloudLang })
                                    put("tzOffset", if (uc.ncgTzSet) uc.ncgTzOffset else phoneGeo.tzOffset)
                                    put("langPackName", "android")
                                    put("apiId", 6)
                                    put("apiHash", "eb06d4abfb49dc3eeb1aeb98ae0f581e")
                                }
                            }

                            val proxyBits = ownProxy(slot)
                            val proxyJson = proxyBits?.let { p ->
                                JSONObject().apply {
                                    put("type", "socks5")
                                    put("host", p.address)
                                    put("port", p.port)
                                    put("user", p.username)
                                    put("password", p.password)
                                    if (p.secret.isNotEmpty()) put("secret", p.secret)
                                }
                            }

                            if (options.format == ExportFormat.TData) {
                                val tdataDir = File(accDir, "tdata")
                                TdataWriter.write(account, tdataDir)
                            } else if (options.format == ExportFormat.SessionJson) {
                                val rawPhone = uc.currentUser?.phone.orEmpty().filter { it.isDigit() }
                                val sessionName = if (rawPhone.isNotEmpty()) rawPhone else "account_${account.userId}"

                                val primaryDc = account.mainDcId
                                val primaryKey = account.keys.firstOrNull { it.first == primaryDc }?.second
                                    ?: account.keys.firstOrNull()?.second
                                val primaryHex = primaryKey?.joinToString("") { "%02x".format(it) }.orEmpty()

                                val dcIp = when (primaryDc) {
                                    1 -> "149.154.175.53"
                                    2 -> "149.154.167.51"
                                    3 -> "149.154.175.100"
                                    4 -> "149.154.167.91"
                                    5 -> "91.108.56.130"
                                    else -> "149.154.167.51"
                                }

                                if (primaryKey != null) {
                                    val sessionFile = File(accDir, "$sessionName.session")
                                    try {
                                        val db = SQLiteDatabase.openOrCreateDatabase(sessionFile, null)
                                        db.execSQL("CREATE TABLE IF NOT EXISTS version (version INTEGER PRIMARY KEY);")
                                        db.execSQL("DELETE FROM version;")
                                        db.execSQL("INSERT INTO version VALUES (7);")
                                        db.execSQL("CREATE TABLE IF NOT EXISTS sessions (dc_id INTEGER PRIMARY KEY, server_address TEXT, port INTEGER, auth_key BLOB, takeout_id INTEGER);")
                                        db.execSQL("DELETE FROM sessions;")
                                        val cv = ContentValues().apply {
                                            put("dc_id", primaryDc)
                                            put("server_address", dcIp)
                                            put("port", 443)
                                            put("auth_key", primaryKey)
                                            put("takeout_id", 0)
                                        }
                                        db.insert("sessions", null, cv)

                                        db.execSQL("CREATE TABLE IF NOT EXISTS entities (id INTEGER PRIMARY KEY, hash INTEGER NOT NULL, username TEXT, phone INTEGER, name TEXT, date INTEGER);")
                                        val ent = ContentValues().apply {
                                            put("id", account.userId)
                                            put("hash", 0)
                                            put("username", uc.currentUser?.username.orEmpty())
                                            put("phone", rawPhone.toLongOrNull() ?: 0L)
                                            put("name", "${uc.currentUser?.first_name.orEmpty()} ${uc.currentUser?.last_name.orEmpty()}".trim())
                                            put("date", (System.currentTimeMillis() / 1000L).toInt())
                                        }
                                        db.insertWithOnConflict("entities", null, ent, SQLiteDatabase.CONFLICT_REPLACE)
                                        db.close()
                                    } catch (t: Throwable) {
                                        FileLog.e(t)
                                    }
                                }

                                val sess = JSONObject().apply {
                                    put("session_file", "$sessionName.session")
                                    put("phone", if (rawPhone.isNotEmpty()) "+$rawPhone" else "")
                                    put("user_id", account.userId)
                                    put("first_name", uc.currentUser?.first_name.orEmpty())
                                    put("last_name", uc.currentUser?.last_name.orEmpty())
                                    put("username", uc.currentUser?.username.orEmpty())
                                    put("main_dc_id", primaryDc)
                                    put("dc_id", primaryDc)
                                    put("auth_key", primaryHex)
                                    put("app_id", dev.optInt("apiId", if (isAccountIos) GgtgIosProfiles.APP_ID else 2040))
                                    put("app_hash", dev.optString("apiHash", if (isAccountIos) GgtgIosProfiles.APP_HASH else "b18441a1ff607e10a989891a5462e627"))
                                    put("device_model", dev.optString("deviceModel"))
                                    put("system_version", dev.optString("systemVersion"))
                                    put("app_version", dev.optString("appVersion"))
                                    put("system_lang_code", dev.optString("systemLangCode", "ru-ru"))
                                    put("lang_pack", dev.optString("langPackName", if (isAccountIos) GgtgIosProfiles.LANG_PACK else "android"))
                                    put("cloud_lang_code", dev.optString("cloudLangCode", "ru"))
                                    val twoFa = uc.ncgTwoFactor.orEmpty()
                                        .ifEmpty { GgtgTwoFaStore.get(account.userId).orEmpty() }
                                        .ifEmpty { GgtgSessions.listForUser(account.userId).firstOrNull { it.twoFactor.isNotEmpty() }?.twoFactor.orEmpty() }
                                    if (twoFa.isNotEmpty()) {
                                        put("twoFA", twoFa)
                                        put("two_factor", twoFa)
                                        put("password", twoFa)
                                    }
                                    uc.ncgFolder?.let { put("folder", it) }
                                    proxyJson?.let { put("proxy", it) }
                                    put("device", dev)
                                    put("settings", JSONObject().apply {
                                        put("pinned", uc.ncgPinned)
                                        uc.ncgFolder?.let { put("folder", it) }
                                        if (twoFa.isNotEmpty()) put("twoFactor", twoFa)
                                    })
                                    val keys = JSONArray()
                                    for ((dcId, key) in account.keys) {
                                        keys.put(JSONObject().apply {
                                            put("dcId", dcId)
                                            put("key", key.joinToString("") { "%02x".format(it) })
                                        })
                                    }
                                    put("keys", keys)
                                }
                                File(accDir, "$sessionName.json").writeText(sess.toString(2))
                                File(accDir, "session.json").writeText(sess.toString(2))
                            } else if (options.format == ExportFormat.GgtgPack) {
                                val twoFa = uc.ncgTwoFactor.orEmpty()
                                    .ifEmpty { GgtgTwoFaStore.get(account.userId).orEmpty() }
                                    .ifEmpty { GgtgSessions.listForUser(account.userId).firstOrNull { it.twoFactor.isNotEmpty() }?.twoFactor.orEmpty() }
                                val entry = JSONObject().apply {
                                    put("userId", account.userId.toString())
                                    put("mainDcId", account.mainDcId)
                                    val keys = JSONArray()
                                    for ((dcId, key) in account.keys) {
                                        keys.put(JSONObject().apply {
                                            put("dcId", dcId)
                                            put("key", key.joinToString("") { "%02x".format(it) })
                                        })
                                    }
                                    put("keys", keys)
                                    put("phone", uc.currentUser?.phone.orEmpty())
                                    uc.ncgFolder?.let { put("folder", it) }
                                    put("pinned", uc.ncgPinned)
                                    if (twoFa.isNotEmpty()) put("twoFactor", twoFa)
                                    put("device", dev)
                                    proxyJson?.let { put("proxy", it) }
                                }
                                manifestAccounts.put(entry)
                            }

                            if (options.sidecars) {
                                val twoFa = uc.ncgTwoFactor.orEmpty()
                                    .ifEmpty { GgtgTwoFaStore.get(account.userId).orEmpty() }
                                    .ifEmpty { GgtgSessions.listForUser(account.userId).firstOrNull { it.twoFactor.isNotEmpty() }?.twoFactor.orEmpty() }
                                val ggtgJson = JSONObject().apply {
                                    put("userId", account.userId.toString())
                                    put("phone", uc.currentUser?.phone.orEmpty())
                                    uc.ncgFolder?.let { put("folder", it) }
                                    if (twoFa.isNotEmpty()) put("twoFactor", twoFa)
                                    proxyJson?.let { put("proxy", it) }
                                    put("device", dev)
                                }
                                File(accDir, "ggtg.json").writeText(ggtgJson.toString(2))
                                if (twoFa.isNotEmpty()) {
                                    File(accDir, "2fa.txt").writeText(twoFa)
                                }
                            }
                        }
                        done++
                    }.onFailure { t ->
                        if (t is PasswordSkippedException) {
                            passwordSkipped++
                        } else {
                            FileLog.e("export failed for slot=$slot", t)
                            failures += "$label: ${t.message ?: t.javaClass.simpleName}"
                        }
                    }
                }

                if (tempSlot >= 0) {
                    ConnectionsManager.getInstance(tempSlot).cleanup(true)
                    ConnectionsManager.setProxySettingsForAccount(tempSlot, false, "", 0, "", "", "")
                }

                for (slot in swapSlots) {
                    try {
                        GgtgSessions.swapToBackup(slot)
                    } catch (t: Throwable) {
                        FileLog.e("swapToBackup after export failed for slot=$slot", t)
                    }
                }

                progress.dismiss()
                if (done == 0) {
                    val skippedLine = if (passwordSkipped > 0) "Пропущено (без пароля 2FA): $passwordSkipped\n" else ""
                    onDone("Ничего не экспортировано.\n$skippedLine" + failures.joinToString("\n"))
                    return@launch
                }

                if (options.format == ExportFormat.GgtgPack) {
                    val manifest = JSONObject().apply {
                        put("format", 1)
                        put("kind", "ggtg-android-export")
                        put("createdAt", System.currentTimeMillis() / 1000)
                        put("accounts", manifestAccounts)
                    }
                    File(stage, "manifest.json").writeText(manifest.toString(2))
                }

                val ext = if (options.format == ExportFormat.GgtgPack) "ggtg" else "zip"
                val outArchive = File(ApplicationLoader.applicationContext.cacheDir, "export_archive.$ext")
                withContext(Dispatchers.IO) { zipDir(stage, outArchive) }

                val summary = StringBuilder("Экспортировано аккаунтов: $done.")
                if (options.source == SessionSource.WorkingSwap && swapSlots.isNotEmpty()) {
                    summary.append("\nПереключено на резервную сессию: ${swapSlots.size}")
                }
                if (passwordSkipped > 0) summary.append("\nПропущено (без пароля 2FA): $passwordSkipped")
                if (failures.isNotEmpty()) summary.append("\nОшибки:\n").append(failures.joinToString("\n"))

                val stamp = SimpleDateFormat("yyyyMMdd_HHmm", Locale.US).format(Date())
                val outName = when (options.format) {
                    ExportFormat.TData -> "GGTG_tdata_$stamp.zip"
                    ExportFormat.SessionJson -> "GGTG_sessions_$stamp.zip"
                    ExportFormat.GgtgPack -> "GGTG_export_$stamp.ggtg"
                }

                savedCallback = { uri ->
                    MainScope().launch {
                        if (uri == null) {
                            onDone("Сохранение отменено.")
                        } else {
                            try {
                                withContext(Dispatchers.IO) {
                                    activity.contentResolver.openOutputStream(uri)?.use { out ->
                                        outArchive.inputStream().use { it.copyTo(out) }
                                    }
                                }
                                onDone(summary.toString())
                            } catch (t: Throwable) {
                                onDone("Не удалось сохранить файл: ${t.message}")
                            }
                        }
                        outArchive.delete()
                        stage.deleteRecursively()
                    }
                }
                saveLauncher.launch(outName)
            } catch (t: Throwable) {
                FileLog.e("export failed", t)
                progress.dismiss()
                stage.deleteRecursively()
                onDone("Ошибка экспорта: ${t.message ?: t.javaClass.simpleName}")
            }
        }
    }

    /** The accounts' brand-new sessions for GGTG Desktop, as a manifest (the shape of a .ggtg pack, kind
     *  "ggtg-desktop-import"); the sync carries it to the Mac. [onDone] gets the manifest (null on failure) and a
     *  user-facing summary. Always duplicates: the phone's own sessions stay separate. */
    @JvmStatic
    fun exportForDesktop(activity: FragmentActivity, slots: List<Int>, onDone: (ByteArray?, String) -> Unit) {
        val progress = TdataImportProgressDialog(activity, "Аккаунты на Mac")
        progress.setWorkingText("Подготовка…")
        MainScope().launch {
            try {
                val tempSlot = (0 until UserConfig.MAX_ACCOUNT_COUNT)
                    .firstOrNull { it != UserConfig.selectedAccount && !UserConfig.getInstance(it).isClientActivated && it !in slots }
                if (tempSlot == null) {
                    progress.dismiss(); onDone(null, "Нет свободного слота для дублирования сессии - освободите один слот аккаунта."); return@launch
                }
                val failures = ArrayList<String>()
                val accounts = org.json.JSONArray()
                var passwordSkipped = 0
                for ((i, slot) in slots.withIndex()) {
                    val label = GgtgAccounts.label(slot)
                    progress.setWorkingText("Дублируем $label (${i + 1} из ${slots.size})…")
                    val result = try {
                        val proxy = ownProxy(slot)
                        if (proxy == null) Result.failure(NO_PROXY) else duplicateSession(slot, tempSlot, proxy, progress, label)
                    } catch (t: Throwable) { Result.failure(t) }
                    result.onSuccess { account ->
                        val uc = UserConfig.getInstance(slot)
                        val entry = org.json.JSONObject()
                        entry.put("userId", account.userId.toString())
                        entry.put("mainDcId", account.mainDcId)
                        val keys = org.json.JSONArray()
                        for ((dcId, key) in account.keys) {
                            keys.put(org.json.JSONObject().put("dcId", dcId).put("key", key.joinToString("") { "%02x".format(it) }))
                        }
                        entry.put("keys", keys)
                        entry.put("phone", uc.currentUser?.phone.orEmpty())
                        uc.ncgFolder?.let { entry.put("folder", it) }
                        entry.put("pinned", uc.ncgPinned)
                        uc.ncgTwoFactor?.takeIf { it.isNotEmpty() }?.let { entry.put("twoFactor", it) }
                        val phone = uc.currentUser?.phone.orEmpty()
                        val phoneGeo = GgtgGeo.matchForPhone(phone)
                        val desktopProfile = GgtgDesktopProfiles.random(phone)
                        val dev = org.json.JSONObject().apply {
                            put("deviceModel", desktopProfile.deviceModel)
                            put("systemVersion", desktopProfile.systemVersion)
                            put("appVersion", desktopProfile.appVersion)
                            put("systemLangCode", phoneGeo.systemLang.lowercase())
                            put("cloudLangCode", phoneGeo.cloudLang)
                            put("tzOffset", phoneGeo.tzOffset)
                            put("langPackName", GgtgDesktopProfiles.LANG_PACK)
                            put("apiId", GgtgDesktopProfiles.APP_ID)
                            put("apiHash", GgtgDesktopProfiles.APP_HASH)
                        }
                        entry.put("device", dev)
                        accounts.put(entry)
                    }.onFailure { t ->
                        if (t is PasswordSkippedException) passwordSkipped++
                        else {
                            FileLog.e("desktop export failed for slot=$slot", t)
                            failures += "$label: ${t.message ?: t.javaClass.simpleName}"
                        }
                    }
                }
                ConnectionsManager.getInstance(tempSlot).cleanup(true)
                ConnectionsManager.setProxySettingsForAccount(tempSlot, false, "", 0, "", "", "")
                progress.dismiss()
                if (accounts.length() == 0) {
                    val skipped = if (passwordSkipped > 0) "Пропущено (без пароля 2FA): $passwordSkipped\n" else ""
                    onDone(null, "Ничего не подготовлено.\n$skipped" + failures.joinToString("\n"))
                    return@launch
                }
                val manifest = org.json.JSONObject()
                manifest.put("format", 1)
                manifest.put("kind", "ggtg-desktop-import")
                manifest.put("createdAt", System.currentTimeMillis() / 1000)
                manifest.put("accounts", accounts)
                val summary = StringBuilder("Подготовлено аккаунтов: ${accounts.length()}.")
                if (passwordSkipped > 0) summary.append("\nПропущено (без пароля 2FA): $passwordSkipped")
                if (failures.isNotEmpty()) summary.append("\nОшибки:\n").append(failures.joinToString("\n"))
                onDone(manifest.toString().toByteArray(Charsets.UTF_8), summary.toString())
            } catch (t: Throwable) {
                FileLog.e("desktop export failed", t)
                progress.dismiss()
                onDone(null, "Ошибка: ${t.message ?: t.javaClass.simpleName}")
            }
        }
    }

    private fun zipDir(dir: File, zip: File) {
        ZipOutputStream(zip.outputStream().buffered()).use { zos ->
            dir.walkTopDown().filter { it.isFile }.forEach { f ->
                zos.putNextEntry(ZipEntry(f.relativeTo(dir).path))
                f.inputStream().use { it.copyTo(zos) }
                zos.closeEntry()
            }
        }
    }

    /** Snapshot of the account's own live session keys (tgnet blob: mainDc, count, then dcId + 256 bytes each). */
    internal fun currentSession(slot: Int): TdataAccount {
        val blob = ConnectionsManager.getInstance(slot).exportAuthKeys()
        val userId = UserConfig.getInstance(slot).clientUserId
        return parseKeys(blob, userId, null)
    }

    private fun parseKeys(blob: ByteArray, userId: Long, forceMainDc: Int?): TdataAccount {
        fun i32(p: Int) = ((blob[p].toInt() and 0xFF) shl 24) or ((blob[p + 1].toInt() and 0xFF) shl 16) or ((blob[p + 2].toInt() and 0xFF) shl 8) or (blob[p + 3].toInt() and 0xFF)
        var pos = 0
        val mainDc = i32(pos).also { pos += 4 }
        val count = i32(pos).also { pos += 4 }
        val keys = ArrayList<Pair<Int, ByteArray>>()
        repeat(count) {
            val dc = i32(pos); pos += 4
            keys += dc to blob.copyOfRange(pos, pos + 256); pos += 256
        }
        val main = forceMainDc ?: mainDc
        val picked = keys.filter { it.first == main }
        if (picked.isEmpty()) throw IllegalStateException("нет ключа для DC$main (аккаунт ещё не подключался?)")
        return TdataAccount(userId, main, if (forceMainDc != null) picked else keys)
    }

    /** ng: SESSION_PASSWORD_NEEDED on the throwaway "new device" slot (tempCm) during duplication - same
     * shape as TdataImportHelper's own 2FA handling, reused via [TdataImportHelper.trySubmitPassword].
     * Tries a password saved earlier for this exact userId first (silently); falls back to the shared
     * progress dialog's password prompt (Skip allowed). A password that checks out is saved right away
     * so neither export nor import ever has to ask again for this account. */
    private suspend fun resolvePasswordAndAuthorize(
        tempCm: ConnectionsManager,
        dcId: Int,
        userId: Long,
        label: String,
        progress: TdataImportProgressDialog?
    ): Result<TLRPC.auth_Authorization> {
        // the password saved for import, then the one kept on the account itself ("Пароль 2FA")
        val accountPassword = (0 until UserConfig.MAX_ACCOUNT_COUNT)
            .map { UserConfig.getInstance(it) }
            .firstOrNull { it.isClientActivated && it.clientUserId == userId }
            ?.ncgTwoFactor?.takeIf { it.isNotEmpty() }
        for (saved in listOfNotNull(GgtgTwoFaStore.get(userId), accountPassword).distinct()) {
            val result = TdataImportHelper.trySubmitPassword(tempCm, dcId, saved)
            if (result.isSuccess) return result
            FileLog.d("tdata export: saved 2FA password for userId=$userId no longer works (${result.exceptionOrNull()?.message})")
        }
        // no window to ask in (a bulk action): the account is reported, the queue goes on
        if (progress == null) return Result.failure(Exception("нужен пароль 2FA - сохраните его в меню аккаунта («Пароль 2FA»)"))

        var wrongPassword = false
        var hint: String? = null
        while (true) {
            val typed = progress.askPasswordSuspend(label, wrongPassword, hint) ?: return Result.failure(PasswordSkippedException())
            val result = TdataImportHelper.trySubmitPassword(tempCm, dcId, typed)
            if (result.isSuccess) {
                GgtgTwoFaStore.save(userId, typed)
                return result
            }
            val reason = result.exceptionOrNull()?.message ?: "checkPassword failed"
            if (reason == "PASSWORD_HASH_INVALID") {
                wrongPassword = true
                hint = null
                continue
            }
            if (reason.startsWith("FLOOD_WAIT")) return Result.failure(Exception(reason))
            wrongPassword = false
            hint = "Ошибка проверки: $reason"
        }
    }

    /** New independent session for [sourceSlot]'s user: [tempSlot] plays the "new device" asking for a login token,
     * [sourceSlot] plays the phone that approves it - the same handshake as TdataImportHelper, mirrored. */
    /** [onAccepted] gets the new session's number (TL_authorization.hash) - how it is told apart
     *  in the account's session list later (GgtgSessions). */
    internal suspend fun duplicateSession(
        sourceSlot: Int,
        tempSlot: Int,
        proxy: GgtgProxyBits,
        progress: TdataImportProgressDialog?,
        label: String,
        desktopProfile: GgtgDesktopProfiles.Profile? = null,
        onAccepted: (Long) -> Unit = {}
    ): Result<TdataAccount> {
        val sourceUser = UserConfig.getInstance(sourceSlot).currentUser
        val sourcePhone = sourceUser?.phone.orEmpty()
        val phoneGeo = GgtgGeo.matchForPhone(sourcePhone)
        val profile = desktopProfile ?: GgtgDesktopProfiles.random(sourcePhone)
        ConnectionsManager.setProxySettingsForAccount(tempSlot, true, proxy.address, proxy.port, proxy.username, proxy.password, proxy.secret)
        GgtgDevice.save(
            tempSlot, profile.deviceModel, profile.systemVersion,
            phoneGeo.systemLang.lowercase(), phoneGeo.cloudLang, phoneGeo.tzOffset,
            profile.appVersion, GgtgDesktopProfiles.LANG_PACK,
            GgtgDesktopProfiles.APP_ID, GgtgDesktopProfiles.APP_HASH
        )
        val tempCm = ConnectionsManager.getInstance(tempSlot)
        tempCm.cleanup(true)
        val sourceCm = ConnectionsManager.getInstance(sourceSlot)
        val sourceUserId = UserConfig.getInstance(sourceSlot).clientUserId

        // 2FA on the SOURCE account surfaces here, on tempCm (the throwaway "new device" asking to log
        // in) - resolves it, then reads the just-authorized session key straight back off tempCm.
        suspend fun onPasswordNeeded(dcId: Int): Result<TdataAccount> {
            val progressResult = resolvePasswordAndAuthorize(tempCm, dcId, sourceUserId, label, progress)
            val auth = progressResult.getOrElse { return Result.failure(it) }
            if (auth !is TLRPC.TL_auth_authorization) return Result.failure(Exception("account requires sign-up - can't happen for an existing session"))
            val blob = tempCm.exportAuthKeys()
            return try { Result.success(parseKeys(blob, auth.user.id, dcId)) } catch (t: Throwable) { Result.failure(t) }
        }

        fun exportReq() = TLRPC.TL_auth_exportLoginToken().apply { api_id = GgtgApiOverride.id(); api_hash = GgtgApiOverride.hash() }
        val noLogin = ConnectionsManager.RequestFlagWithoutLogin
        val noLoginUnauth = noLogin or ConnectionsManager.RequestFlagEnableUnauthorized

        var dcId = ConnectionsManager.DEFAULT_DATACENTER_ID
        var (resp, err) = tempCm.sendSuspend(exportReq(), noLogin, dcId, "dup exportLoginToken") ?: return Result.failure(Exception("exportLoginToken: таймаут"))
        if (err != null) {
            if (err.text == "SESSION_PASSWORD_NEEDED") return onPasswordNeeded(dcId)
            return Result.failure(Exception("exportLoginToken: ${err.text}"))
        }

        val token: ByteArray = when (resp) {
            is TLRPC.TL_auth_loginTokenMigrateTo -> {
                dcId = resp.dc_id
                val (r2, e2) = tempCm.sendSuspend(TLRPC.TL_auth_importLoginToken().apply { token = resp.token }, noLoginUnauth, dcId, "dup importLoginToken")
                    ?: return Result.failure(Exception("importLoginToken: таймаут"))
                if (e2 != null) {
                    if (e2.text == "SESSION_PASSWORD_NEEDED") return onPasswordNeeded(dcId)
                    return Result.failure(Exception("importLoginToken: ${e2.text}"))
                }
                (r2 as? TLRPC.TL_auth_loginToken)?.token ?: return Result.failure(Exception("неожиданный ответ: $r2"))
            }
            is TLRPC.TL_auth_loginToken -> { dcId = 2; resp.token }
            else -> return Result.failure(Exception("неожиданный ответ: $resp"))
        }

        // the source account approves on ITS home DC (the server resolves the token across DCs itself)
        val (acc, accErr) = sourceCm.sendSuspend(TLRPC.TL_auth_acceptLoginToken().apply { this.token = token }, 0, ConnectionsManager.DEFAULT_DATACENTER_ID, "dup acceptLoginToken")
            ?: return Result.failure(Exception("acceptLoginToken: таймаут"))
        if (accErr != null) return Result.failure(Exception("acceptLoginToken: ${accErr.text}"))
        if (acc !is TLRPC.TL_authorization) return Result.failure(Exception("неожиданный ответ accept: $acc"))
        onAccepted(acc.hash)

        repeat(5) { attempt ->
            val (p, pe) = tempCm.sendSuspend(exportReq(), noLogin, dcId, "dup poll") ?: return Result.failure(Exception("poll: таймаут"))
            if (pe != null) {
                if (pe.text == "SESSION_PASSWORD_NEEDED") return onPasswordNeeded(dcId)
                return Result.failure(Exception("poll: ${pe.text}"))
            }
            var success: TLRPC.auth_Authorization? = null
            when (p) {
                is TLRPC.TL_auth_loginTokenSuccess -> success = p.authorization
                is TLRPC.TL_auth_loginTokenMigrateTo -> {
                    dcId = p.dc_id
                    val (r2, e2) = tempCm.sendSuspend(TLRPC.TL_auth_importLoginToken().apply { this.token = p.token }, noLoginUnauth, dcId, "dup importLoginToken2")
                        ?: return Result.failure(Exception("importLoginToken: таймаут"))
                    if (e2 != null) {
                        if (e2.text == "SESSION_PASSWORD_NEEDED") return onPasswordNeeded(dcId)
                        return Result.failure(Exception("importLoginToken: ${e2.text}"))
                    }
                    if (r2 is TLRPC.TL_auth_loginTokenSuccess) success = r2.authorization
                }
                is TLRPC.TL_auth_loginToken -> {}
                else -> return Result.failure(Exception("неожиданный ответ poll: $p"))
            }
            if (success is TLRPC.TL_auth_authorization) {
                // the new session's key lives on the DC the login finished on
                val blob = tempCm.exportAuthKeys()
                return try { Result.success(parseKeys(blob, success.user.id, dcId)) } catch (t: Throwable) { Result.failure(t) }
            }
            if (attempt < 4) delay(400)
        }
        return Result.failure(Exception("вход подтверждён, но сессия не завершилась"))
    }
}
