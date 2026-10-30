package app.ggtg

import android.app.Activity
import android.content.Context
import android.content.Intent
import android.database.sqlite.SQLiteDatabase
import android.net.Uri
import android.view.View
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.Toast
import androidx.documentfile.provider.DocumentFile
import app.ggtg.ui.TdataImportProgressDialog
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONArray
import org.json.JSONObject
import org.telegram.messenger.AndroidUtilities
import org.telegram.messenger.ApplicationLoader
import org.telegram.messenger.FileLog
import org.telegram.messenger.R
import org.telegram.messenger.UserConfig
import org.telegram.tgnet.ConnectionsManager
import org.telegram.ui.ActionBar.ActionBar.ActionBarMenuOnItemClick
import org.telegram.ui.ActionBar.AlertDialog
import org.telegram.ui.ActionBar.BaseFragment
import org.telegram.ui.ActionBar.Theme
import org.telegram.ui.Components.GgtgProxyChooser
import java.io.File
import java.io.FileInputStream
import java.io.FileOutputStream
import java.util.zip.ZipInputStream

// GGTG: Support for importing "Сессия + JSON" (Telethon, Pyrogram, and TG tools).
// Parses SQLite .session databases and/or .json files with credentials, proxy, device params, and 2FA.
object GgtgSessionJsonSupport {

    private const val AUTH_KEY_SIZE = 256

    data class SessionAccount(
        val userId: Long,
        val mainDcId: Int,
        val keys: List<Pair<Int, ByteArray>>,
        val phone: String,
        val firstName: String,
        val lastName: String,
        val username: String,
        val twoFactor: String,
        val folder: String?,
        val pinned: Boolean,
        val proxy: GgtgProxyBits?,
        val device: JSONObject?,
        val label: String
    )

    fun hexToBytes(hex: String): ByteArray? {
        val clean = hex.trim()
        if (clean.length % 2 != 0) return null
        return try {
            ByteArray(clean.length / 2) { clean.substring(it * 2, it * 2 + 2).toInt(16).toByte() }
        } catch (e: Throwable) {
            null
        }
    }

    fun parseProxyJson(json: JSONObject?): GgtgProxyBits? {
        if (json == null) return null
        val host = json.optString("host", json.optString("address", json.optString("ip"))).trim()
        val port = json.optInt("port", 0)
        if (host.isEmpty() || port <= 0) return null
        val user = json.optString("user", json.optString("username")).trim()
        val pass = json.optString("password").trim()
        val secret = json.optString("secret").trim()
        return GgtgProxyBits(host, port, user, pass, secret)
    }

    fun parseDeviceJson(json: JSONObject?): JSONObject? {
        if (json == null) return null
        if (json.has("device") && json.optJSONObject("device") != null) {
            return json.optJSONObject("device")
        }
        val dev = JSONObject()
        val model = json.optString("device_model", json.optString("deviceModel", json.optString("device"))).trim()
        val sysVer = json.optString("system_version", json.optString("systemVersion", json.optString("sdk"))).trim()
        val appVer = json.optString("app_version", json.optString("appVersion")).trim()
        val sysLang = json.optString("system_lang_code", json.optString("systemLangCode", json.optString("systemLang"))).trim()
        val cloudLang = json.optString("cloud_lang_code", json.optString("cloudLangCode", json.optString("cloudLang"))).trim()
        val langPack = json.optString("lang_pack", json.optString("langPack", json.optString("langPackName"))).trim()
        val apiId = json.optInt("app_id", json.optInt("api_id", json.optInt("apiId", 2040)))
        val apiHash = json.optString("app_hash", json.optString("api_hash", json.optString("apiHash", "b18441a1ff607e10a989891a5462e627"))).trim()

        if (model.isNotEmpty()) dev.put("deviceModel", model)
        if (sysVer.isNotEmpty()) dev.put("systemVersion", sysVer)
        if (appVer.isNotEmpty()) dev.put("appVersion", appVer)
        if (sysLang.isNotEmpty()) dev.put("systemLangCode", sysLang)
        if (cloudLang.isNotEmpty()) dev.put("cloudLangCode", cloudLang)
        if (langPack.isNotEmpty()) dev.put("langPackName", langPack)
        dev.put("apiId", apiId)
        dev.put("apiHash", apiHash)
        return if (dev.length() > 0) dev else null
    }

    fun readSqliteSession(sessionFile: File): Triple<Int, ByteArray?, Long?> {
        var dcId = 2
        var authKey: ByteArray? = null
        var userId: Long? = null

        var db: SQLiteDatabase? = null
        try {
            db = SQLiteDatabase.openDatabase(sessionFile.absolutePath, null, SQLiteDatabase.OPEN_READONLY)
            val cursor = db.rawQuery("SELECT * FROM sessions LIMIT 1", null)
            if (cursor.moveToFirst()) {
                val dcCol = cursor.getColumnIndex("dc_id")
                val keyCol = cursor.getColumnIndex("auth_key")
                val userCol = cursor.getColumnIndex("user_id")

                if (dcCol != -1) dcId = cursor.getInt(dcCol)
                if (keyCol != -1) {
                    val blob = cursor.getBlob(keyCol)
                    if (blob != null && blob.size == AUTH_KEY_SIZE) {
                        authKey = blob
                    }
                }
                if (userCol != -1) {
                    val u = cursor.getLong(userCol)
                    if (u != 0L) userId = u
                }
            }
            cursor.close()

            if (userId == null) {
                try {
                    val entCursor = db.rawQuery("SELECT id FROM entities LIMIT 1", null)
                    if (entCursor.moveToFirst()) {
                        val u = entCursor.getLong(0)
                        if (u != 0L) userId = u
                    }
                    entCursor.close()
                } catch (t: Throwable) {
                    // entities table optional
                }
            }
        } catch (t: Throwable) {
            FileLog.e(t)
        } finally {
            try { db?.close() } catch (t: Throwable) {}
        }
        return Triple(dcId, authKey, userId)
    }

    fun scanDirectory(root: File): List<SessionAccount> {
        val result = mutableListOf<SessionAccount>()
        val allFiles = root.walkTopDown().maxDepth(4).toList()
        val handledBases = mutableSetOf<String>()

        val sessionFiles = allFiles.filter { it.isFile && it.extension.equals("session", ignoreCase = true) }
        val jsonFiles = allFiles.filter { it.isFile && it.extension.equals("json", ignoreCase = true) }

        for (sessFile in sessionFiles) {
            val dir = sessFile.parentFile ?: continue
            val baseName = sessFile.nameWithoutExtension
            val baseKey = File(dir, baseName).absolutePath
            if (handledBases.contains(baseKey)) continue
            handledBases.add(baseKey)

            val (dcId, authKey, sqliteUser) = readSqliteSession(sessFile)
            if (authKey == null || authKey.size != AUTH_KEY_SIZE) continue

            // Look for matching json
            val jsonCandidate = File(dir, "$baseName.json").takeIf { it.exists() }
                ?: File(dir, "session.json").takeIf { it.exists() }
                ?: File(dir, "ggtg.json").takeIf { it.exists() }
                ?: jsonFiles.firstOrNull { it.parentFile == dir }

            var jsonObject: JSONObject? = null
            if (jsonCandidate != null) {
                try {
                    jsonObject = JSONObject(jsonCandidate.readText())
                } catch (t: Throwable) {
                    FileLog.e(t)
                }
            }

            var userId = sqliteUser ?: jsonObject?.optLong("user_id", 0L) ?: 0L
            if (userId == 0L) userId = jsonObject?.optLong("userId", 0L) ?: 0L
            if (userId == 0L) userId = jsonObject?.optLong("id", 0L) ?: 0L

            val phone = jsonObject?.optString("phone", "").orEmpty()
            if (userId == 0L && phone.isNotEmpty()) {
                val digits = phone.filter { it.isDigit() }
                userId = digits.toLongOrNull() ?: 0L
            }
            if (userId == 0L) {
                userId = 1000000000L + (authKey.contentHashCode().toLong() and 0x7FFFFFFFL)
            }

            val twoFactor = jsonObject?.optString("twoFA",
                jsonObject.optString("two_factor",
                    jsonObject.optString("twoFactor",
                        jsonObject.optString("password", "")))).orEmpty()
            val finalTwoFactor = if (twoFactor.isNotEmpty()) twoFactor else {
                File(dir, "2fa.txt").takeIf { it.exists() }?.readText()?.trim().orEmpty()
            }

            val folder = jsonObject?.optString("folder")?.takeIf { it.isNotEmpty() }
                ?: if (dir != root && dir.name != baseName) dir.name else null

            val pinned = jsonObject?.optJSONObject("settings")?.optBoolean("pinned")
                ?: jsonObject?.optBoolean("pinned", false) ?: false

            val proxy = parseProxyJson(jsonObject?.optJSONObject("proxy"))
            val device = parseDeviceJson(jsonObject)

            val label = if (phone.isNotEmpty()) phone else baseName

            result.add(
                SessionAccount(
                    userId = userId,
                    mainDcId = dcId,
                    keys = listOf(dcId to authKey),
                    phone = phone,
                    firstName = jsonObject?.optString("first_name", "").orEmpty(),
                    lastName = jsonObject?.optString("last_name", "").orEmpty(),
                    username = jsonObject?.optString("username", "").orEmpty(),
                    twoFactor = finalTwoFactor,
                    folder = folder,
                    pinned = pinned,
                    proxy = proxy,
                    device = device,
                    label = label
                )
            )
        }

        // Also check standalone JSON files with auth_key where no .session exists
        for (jFile in jsonFiles) {
            val dir = jFile.parentFile ?: continue
            val baseName = jFile.nameWithoutExtension
            val baseKey = File(dir, baseName).absolutePath
            if (handledBases.contains(baseKey)) continue

            var jsonObject: JSONObject? = null
            try {
                jsonObject = JSONObject(jFile.readText())
            } catch (t: Throwable) {
                continue
            }
            if (jsonObject == null) continue

            val hexKey = jsonObject.optString("auth_key", jsonObject.optString("session_key", ""))
            val keyBytes = hexToBytes(hexKey)
            if (keyBytes == null || keyBytes.size != AUTH_KEY_SIZE) continue

            handledBases.add(baseKey)
            val dcId = jsonObject.optInt("main_dc_id", jsonObject.optInt("dc_id", 2))
            var userId = jsonObject.optLong("user_id", jsonObject.optLong("userId", 0L))
            val phone = jsonObject.optString("phone", "")
            if (userId == 0L && phone.isNotEmpty()) {
                val digits = phone.filter { it.isDigit() }
                userId = digits.toLongOrNull() ?: 0L
            }
            if (userId == 0L) {
                userId = 1000000000L + (keyBytes.contentHashCode().toLong() and 0x7FFFFFFFL)
            }

            val twoFactor = jsonObject.optString("twoFA",
                jsonObject.optString("two_factor",
                    jsonObject.optString("twoFactor",
                        jsonObject.optString("password", ""))))
            val finalTwoFactor = if (twoFactor.isNotEmpty()) twoFactor else {
                File(dir, "2fa.txt").takeIf { it.exists() }?.readText()?.trim().orEmpty()
            }

            val folder = jsonObject.optString("folder").takeIf { it.isNotEmpty() }
            val pinned = jsonObject.optJSONObject("settings")?.optBoolean("pinned") ?: jsonObject.optBoolean("pinned", false)
            val proxy = parseProxyJson(jsonObject.optJSONObject("proxy"))
            val device = parseDeviceJson(jsonObject)

            result.add(
                SessionAccount(
                    userId = userId,
                    mainDcId = dcId,
                    keys = listOf(dcId to keyBytes),
                    phone = phone,
                    firstName = jsonObject.optString("first_name", ""),
                    lastName = jsonObject.optString("last_name", ""),
                    username = jsonObject.optString("username", ""),
                    twoFactor = finalTwoFactor,
                    folder = folder,
                    pinned = pinned,
                    proxy = proxy,
                    device = device,
                    label = if (phone.isNotEmpty()) phone else baseName
                )
            )
        }

        return result
    }

    fun unpackZip(zipFile: File, destDir: File): Boolean {
        destDir.deleteRecursively()
        destDir.mkdirs()
        return try {
            ZipInputStream(FileInputStream(zipFile)).use { zis ->
                var entry = zis.nextEntry
                while (entry != null) {
                    val outFile = File(destDir, entry.name)
                    if (!outFile.canonicalPath.startsWith(destDir.canonicalPath)) {
                        entry = zis.nextEntry
                        continue
                    }
                    if (entry.isDirectory) {
                        outFile.mkdirs()
                    } else {
                        outFile.parentFile?.mkdirs()
                        FileOutputStream(outFile).use { fos ->
                            zis.copyTo(fos)
                        }
                    }
                    zis.closeEntry()
                    entry = zis.nextEntry
                }
            }
            true
        } catch (t: Throwable) {
            FileLog.e(t)
            false
        }
    }

    suspend fun importAccounts(
        accounts: List<SessionAccount>,
        proxySource: GgtgProxySource?,
        progressDialog: TdataImportProgressDialog,
        createBackup: Boolean = false
    ): GgtgImportSupport.ImportResult {
        var alreadyLoggedIn = 0
        val toImport = mutableListOf<SessionAccount>()
        for (acc in accounts) {
            val exists = (0 until UserConfig.MAX_ACCOUNT_COUNT).any {
                UserConfig.getInstance(it).clientUserId == acc.userId
            }
            if (exists) alreadyLoggedIn++ else toImport.add(acc)
        }

        val freeSlots = (0 until UserConfig.MAX_ACCOUNT_COUNT)
            .filter { it != UserConfig.selectedAccount && !UserConfig.getInstance(it).isClientActivated }
            .toMutableList()

        val imported = mutableListOf<Long>()
        var firstImportedSlot: Int? = null
        var noFreeSlot = 0
        var noFreeProxy = 0
        var failed = 0
        var lastFailReason: String? = null

        for ((index, acc) in toImport.withIndex()) {
            if (freeSlots.isEmpty()) {
                noFreeSlot++
                continue
            }
            val slot = freeSlots.removeAt(0)
            if (firstImportedSlot == null) firstImportedSlot = slot

            progressDialog.setWorkingText("Импорт ${acc.label} (${index + 1} из ${toImport.size})…")

            val proxy = acc.proxy ?: proxySource?.popNext()?.toProxyBits()
            if (proxy == null) {
                noFreeProxy++
                continue
            }

            try {
                ConnectionsManager.setProxySettingsForAccount(slot, true, proxy.address, proxy.port, proxy.username, proxy.password, proxy.secret)
                GgtgDevice.assignNew(slot, acc.device, isDesktop = false)
                val cm = ConnectionsManager.getInstance(slot)
                for ((dcId, key) in acc.keys) {
                    cm.importAuthKey(dcId, key, GgtgImportSupport.authKeyIdFor(key), dcId == acc.mainDcId)
                }
                cm.setUserId(acc.userId)
                delay(300)

                val result = GgtgImportSupport.activateSlot(slot, null)
                result.onSuccess {
                    val uc = UserConfig.getInstance(slot)
                    acc.folder?.let { uc.ncgFolder = it }
                    uc.ncgPinned = acc.pinned
                    if (acc.twoFactor.isNotEmpty()) {
                        uc.ncgTwoFactor = acc.twoFactor
                        GgtgTwoFaStore.save(acc.userId, acc.twoFactor)
                    }
                    uc.saveConfig(false)
                    imported.add(acc.userId)

                    if (createBackup) {
                        val id = "${System.currentTimeMillis()}_${(1000..9999).random()}"
                        val proxyJson = proxy.let {
                            org.json.JSONObject().apply {
                                put("type", "socks5")
                                put("host", it.address)
                                put("port", it.port)
                                put("user", it.username)
                                put("password", it.password)
                            }
                        }
                        val backup = GgtgSessions.StoredSession(
                            id = id,
                            userId = acc.userId,
                            role = "backup",
                            source = "import_json",
                            note = "Резервная сессия от " + java.text.SimpleDateFormat("dd.MM.yyyy HH:mm", java.util.Locale.getDefault()).format(java.util.Date()),
                            state = "ok",
                            createdAt = System.currentTimeMillis() / 1000,
                            phone = uc.currentUser?.phone.orEmpty(),
                            name = acc.label,
                            folder = acc.folder.orEmpty(),
                            twoFactor = acc.twoFactor,
                            mainDcId = acc.mainDcId,
                            keys = acc.keys,
                            device = acc.device,
                            proxy = proxyJson
                        )
                        GgtgSessions.addStored(backup)
                        val tacc = app.ggtg.tdata.TdataAccount(
                            userId = acc.userId,
                            mainDcId = acc.mainDcId,
                            keys = acc.keys
                        )
                        GgtgSessions.saveToVault(backup, tacc)
                    }
                }.onFailure { err ->
                    failed++
                    lastFailReason = err.message ?: err.javaClass.simpleName
                    ConnectionsManager.getInstance(slot).cleanup(true)
                    ConnectionsManager.setProxySettingsForAccount(slot, false, "", 0, "", "", "")
                }
            } catch (t: Throwable) {
                failed++
                lastFailReason = t.message ?: t.javaClass.simpleName
                ConnectionsManager.getInstance(slot).cleanup(true)
                ConnectionsManager.setProxySettingsForAccount(slot, false, "", 0, "", "", "")
            }
        }

        if (imported.isNotEmpty()) {
            GgtgImportSupport.selectImportedSlotIfIdle(firstImportedSlot)
        }
        return GgtgImportSupport.ImportResult(imported, alreadyLoggedIn, noFreeSlot, failed, lastFailReason, noFreeProxy)
    }
}

// Activity for importing accounts from Session + JSON archives or folders.
class GgtgSessionJsonImportActivity : BaseFragment() {

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main)

    override fun createView(context: Context): View {
        actionBar.setBackButtonImage(R.drawable.ic_ab_back)
        actionBar.setAllowOverlayTitle(true)
        actionBar.setTitle("Импорт Сессия + JSON")
        actionBar.setActionBarMenuOnItemClick(object : ActionBarMenuOnItemClick() {
            override fun onItemClick(id: Int) { if (id == -1) finishFragment() }
        })

        val frame = FrameLayout(context)
        frame.setBackgroundColor(Theme.getColor(Theme.key_windowBackgroundGray))
        fragmentView = frame
        val root = GgtgUi.page(context, frame)

        // 1. Info card
        GgtgUi.addCard(
            root,
            GgtgUi.infoCard(
                context,
                org.telegram.messenger.R.drawable.msg_info,
                "Импорт сессий с JSON",
                "Импорт аккаунтов Telethon, Pyrogram и любого стороннего софта. Поддерживаются архивы zip и папки с файлами .session и .json. Пароли 2FA, параметры устройств, прокси и папки аккаунтов определяются и применяются автоматически."
            ),
            top = 10f
        )

        // 2. Action: choose ZIP archive
        GgtgUi.addCard(
            root,
            GgtgUi.actionCard(
                context,
                org.telegram.messenger.R.drawable.msg_archive,
                "Выбрать архив (.zip)",
                "Zip-архив с папками аккаунтов или файлами .session и .json"
            ) {
                val intent = Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
                    addCategory(Intent.CATEGORY_OPENABLE)
                    type = "*/*"
                }
                startActivityForResult(intent, PICK_ZIP_REQUEST)
            }
        )

        // 3. Action: choose folder
        GgtgUi.addCard(
            root,
            GgtgUi.actionCard(
                context,
                org.telegram.messenger.R.drawable.msg_folders,
                "Выбрать папку",
                "Папка на телефоне, содержащая файлы .session и .json"
            ) {
                val intent = Intent(Intent.ACTION_OPEN_DOCUMENT_TREE).apply {
                    addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
                }
                startActivityForResult(intent, PICK_FOLDER_REQUEST)
            }
        )

        return frame
    }

    override fun onActivityResultFragment(requestCode: Int, resultCode: Int, data: Intent?) {
        if (resultCode != Activity.RESULT_OK) return
        val uri = data?.data ?: return
        val context = context ?: return

        if (requestCode == PICK_ZIP_REQUEST) {
            handleZipImport(context, uri)
        } else if (requestCode == PICK_FOLDER_REQUEST) {
            handleTreeImport(context, uri)
        }
    }

    private fun handleZipImport(context: Context, uri: Uri) {
        val staging = File(ApplicationLoader.applicationContext.cacheDir, "session_json_import_staging")
        val zipCopy = File(ApplicationLoader.applicationContext.cacheDir, "session_json_temp.zip")

        val progress = TdataImportProgressDialog(parentActivity ?: return)
        progress.setWorkingText("Распаковка архива…")
        progress.show()

        scope.launch {
            try {
                withContext(Dispatchers.IO) {
                    context.contentResolver.openInputStream(uri)?.use { input ->
                        zipCopy.outputStream().use { input.copyTo(it) }
                    }
                    GgtgSessionJsonSupport.unpackZip(zipCopy, staging)
                }
                val accounts = withContext(Dispatchers.IO) {
                    GgtgSessionJsonSupport.scanDirectory(staging)
                }
                progress.dismiss()

                if (accounts.isEmpty()) {
                    AlertDialog.Builder(context)
                        .setTitle("Сессия + JSON")
                        .setMessage("В архиве не найдено файлов .session или .json с ключами авторизации.")
                        .setPositiveButton("Закрыть", null)
                        .show()
                    return@launch
                }

                checkProxyAndRun(accounts, staging)
            } catch (t: Throwable) {
                progress.dismiss()
                Toast.makeText(context, "Ошибка чтения архива: ${t.message ?: t.javaClass.simpleName}", Toast.LENGTH_LONG).show()
            } finally {
                zipCopy.delete()
            }
        }
    }

    private fun handleTreeImport(context: Context, treeUri: Uri) {
        val staging = File(ApplicationLoader.applicationContext.cacheDir, "session_json_tree_staging")
        staging.deleteRecursively()
        staging.mkdirs()

        val progress = TdataImportProgressDialog(parentActivity ?: return)
        progress.setWorkingText("Сканирование папки…")
        progress.show()

        scope.launch {
            try {
                withContext(Dispatchers.IO) {
                    val rootDoc = DocumentFile.fromTreeUri(context, treeUri)
                    if (rootDoc != null && rootDoc.isDirectory) {
                        copyTreeRecursive(context, rootDoc, staging, 0)
                    }
                }
                val accounts = withContext(Dispatchers.IO) {
                    GgtgSessionJsonSupport.scanDirectory(staging)
                }
                progress.dismiss()

                if (accounts.isEmpty()) {
                    AlertDialog.Builder(context)
                        .setTitle("Сессия + JSON")
                        .setMessage("В выбранной папке не найдено файлов .session или .json с ключами авторизации.")
                        .setPositiveButton("Закрыть", null)
                        .show()
                    return@launch
                }

                checkProxyAndRun(accounts, staging)
            } catch (t: Throwable) {
                progress.dismiss()
                Toast.makeText(context, "Ошибка чтения папки: ${t.message ?: t.javaClass.simpleName}", Toast.LENGTH_LONG).show()
            }
        }
    }

    private fun copyTreeRecursive(context: Context, doc: DocumentFile, targetDir: File, depth: Int) {
        if (depth > 3) return
        for (child in doc.listFiles()) {
            if (child.isDirectory) {
                val sub = File(targetDir, child.name ?: "dir").also { it.mkdirs() }
                copyTreeRecursive(context, child, sub, depth + 1)
            } else if (child.isFile) {
                val name = child.name ?: continue
                if (name.endsWith(".session", true) || name.endsWith(".json", true) || name.endsWith(".txt", true)) {
                    val outFile = File(targetDir, name)
                    context.contentResolver.openInputStream(child.uri)?.use { input ->
                        outFile.outputStream().use { input.copyTo(it) }
                    }
                }
            }
        }
    }

    private fun checkProxyAndRun(accounts: List<GgtgSessionJsonSupport.SessionAccount>, stagingDir: File) {
        val context = context ?: return
        GgtgImportSupport.askCreateBackupDialog(context) { createBackup ->
            val needsProxy = accounts.any { it.proxy == null }
            if (needsProxy) {
                GgtgProxyChooser.show(context) { source ->
                    runImport(accounts, source, stagingDir, createBackup)
                }
            } else {
                runImport(accounts, null, stagingDir, createBackup)
            }
        }
    }

    private fun runImport(
        accounts: List<GgtgSessionJsonSupport.SessionAccount>,
        source: GgtgProxySource?,
        stagingDir: File,
        createBackup: Boolean
    ) {
        val activity = parentActivity ?: return
        val progress = TdataImportProgressDialog(activity)
        progress.show()

        scope.launch {
            GgtgWakeLock.withLock("session_json_import") {
                try {
                    val result = withContext(Dispatchers.IO) {
                        GgtgSessionJsonSupport.importAccounts(accounts, source, progress, createBackup)
                    }
                    progress.dismiss()
                    AlertDialog.Builder(activity)
                        .setTitle("Импорт Сессия + JSON")
                        .setMessage(GgtgImportSupport.buildResultMessage(result))
                        .setPositiveButton("Закрыть", null)
                        .show()
                    if (result.imported.isNotEmpty()) {
                        finishFragment()
                    }
                } catch (t: Throwable) {
                    progress.dismiss()
                    Toast.makeText(activity, "Ошибка импорта: ${t.message ?: t.javaClass.simpleName}", Toast.LENGTH_LONG).show()
                } finally {
                    withContext(Dispatchers.IO) { stagingDir.deleteRecursively() }
                }
            }
        }
    }

    private companion object {
        const val PICK_ZIP_REQUEST = 5821
        const val PICK_FOLDER_REQUEST = 5822
    }
}
