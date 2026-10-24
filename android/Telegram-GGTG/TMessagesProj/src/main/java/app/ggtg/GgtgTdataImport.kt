package app.ggtg

import android.content.Context
import android.net.Uri
import android.provider.DocumentsContract
import android.provider.OpenableColumns
import android.util.Base64
import androidx.activity.result.ActivityResultLauncher
import androidx.activity.result.contract.ActivityResultContracts
import androidx.fragment.app.FragmentActivity
import app.ggtg.tdata.TdataAccount
import app.ggtg.tdata.TdataReader
import app.ggtg.ui.TdataImportProgressDialog
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.MainScope
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeoutOrNull
import org.telegram.messenger.AndroidUtilities
import org.telegram.messenger.ApplicationLoader
import org.telegram.messenger.BuildVars
import org.telegram.messenger.FileLog
import org.telegram.messenger.LocaleController
import org.telegram.messenger.MessagesController
import org.telegram.messenger.R
import org.telegram.messenger.SRPHelper
import org.telegram.messenger.UserConfig
import org.telegram.tgnet.ConnectionsManager
import org.telegram.tgnet.SerializedData
import org.telegram.tgnet.TLObject
import org.telegram.tgnet.TLRPC
import org.telegram.tgnet.Vector
import org.telegram.tgnet.tl.TL_account
import java.io.File
import java.security.MessageDigest
import java.util.zip.ZipInputStream
import kotlin.coroutines.resume

// Imports Telegram Desktop tdata session folders (picked via SAF, or extracted from a zip/rar
// archive containing several of them) as new, INDEPENDENT Android accounts - a real second login,
// not a copy of Desktop's own auth key. Desktop's key (from the tdata folder) is only ever used
// transiently, on a throwaway account slot, to approve a fresh login token for a brand-new Android
// session - exactly what happens when you scan a real QR code with an already-logged-in device,
// just done in software instead of with a camera. Desktop's own session is never touched, so it
// never gets logged out, and the two sessions never end up sharing one auth key (which would look
// like session theft to Telegram's own systems).
object TdataImportHelper {
    private const val STAGING_DIR = "tdata_import_staging"

    // ng: short step trail per account, appended to the failure text so a failed import explains
    // itself on screen (no adb needed)
    private val trace = ArrayList<String>()
    private fun tr(step: String) {
        FileLog.d("tdata trace: $step")
        synchronized(trace) { trace.add(step); if (trace.size > 14) trace.removeAt(0) }
    }
    private fun traceText(): String = synchronized(trace) { trace.joinToString(" > ") }
    private const val ARCHIVE_TEMP_NAME = "tdata_import_archive.tmp"

    private lateinit var pickTreeLauncher: ActivityResultLauncher<Uri?>
    private lateinit var pickFileLauncher: ActivityResultLauncher<Array<String>>
    private var pickedTreeCallback: ((Uri) -> Unit)? = null
    private var pickedFileCallback: ((Uri) -> Unit)? = null

    @JvmStatic
    fun registerResultCallbacks(activity: FragmentActivity) {
        pickTreeLauncher = activity.registerForActivityResult(
            ActivityResultContracts.OpenDocumentTree()
        ) { uri: Uri? ->
            uri?.let { pickedTreeCallback?.invoke(it) }
        }
        pickFileLauncher = activity.registerForActivityResult(
            ActivityResultContracts.OpenDocument()
        ) { uri: Uri? ->
            uri?.let { pickedFileCallback?.invoke(it) }
        }
    }

    @JvmStatic
    fun pickFolderAndImport(activity: FragmentActivity, proxySource: GgtgProxySource, createBackup: Boolean = false, onDone: (GgtgImportSupport.ImportResult?, Throwable?) -> Unit) {
        val progressDialog = TdataImportProgressDialog(activity)
        pickedTreeCallback = { treeUri ->
            progressDialog.show()
            MainScope().launch {
                try {
                    val result = importFromSafTree(activity, treeUri, proxySource, progressDialog, createBackup)
                    progressDialog.dismiss()
                    onDone(result, null)
                } catch (t: Throwable) {
                    FileLog.e("tdata import failed", t)
                    progressDialog.dismiss()
                    onDone(null, t)
                }
            }
        }
        pickTreeLauncher.launch(null)
    }

    @JvmStatic
    fun pickArchiveAndImport(activity: FragmentActivity, proxySource: GgtgProxySource, createBackup: Boolean = false, onDone: (GgtgImportSupport.ImportResult?, Throwable?) -> Unit) {
        val progressDialog = TdataImportProgressDialog(activity)
        pickedFileCallback = { fileUri ->
            progressDialog.show()
            MainScope().launch {
                try {
                    val result = importFromArchive(activity, fileUri, proxySource, progressDialog, createBackup)
                    progressDialog.dismiss()
                    onDone(result, null)
                } catch (t: Throwable) {
                    FileLog.e("tdata archive import failed", t)
                    progressDialog.dismiss()
                    onDone(null, t)
                }
            }
        }
        pickFileLauncher.launch(arrayOf("*/*"))
    }

    // ng: like pickArchiveAndImport, but asks for a proxy source only when the archive needs one:
    // a .ggtg pack whose accounts all carry their own proxy is installed straight away.
    @JvmStatic
    fun pickArchiveSmart(activity: FragmentActivity, chooseProxy: (onChosen: (GgtgProxySource) -> Unit) -> Unit, createBackup: Boolean = false, onDone: (GgtgImportSupport.ImportResult?, Throwable?) -> Unit) {
        pickedFileCallback = { fileUri ->
            MainScope().launch {
                val selfSufficient = withContext(Dispatchers.IO) {
                    try {
                        val probe = File(ApplicationLoader.applicationContext.cacheDir, ARCHIVE_TEMP_NAME)
                        activity.contentResolver.openInputStream(fileUri)?.use { input ->
                            probe.outputStream().use { out -> input.copyTo(out) }
                        }
                        try { GgtgPackImporter.isPack(probe) && !GgtgPackImporter.needsProxyPool(probe) } finally { probe.delete() }
                    } catch (t: Throwable) { false }
                }
                val run = { source: GgtgProxySource? ->
                    val progressDialog = TdataImportProgressDialog(activity)
                    progressDialog.show()
                    MainScope().launch {
                        try {
                            val result = importFromArchive(activity, fileUri, source, progressDialog, createBackup)
                            progressDialog.dismiss()
                            onDone(result, null)
                        } catch (t: Throwable) {
                            FileLog.e("archive import failed", t)
                            progressDialog.dismiss()
                            onDone(null, t)
                        }
                    }
                }
                if (selfSufficient) run(null) else chooseProxy { run(it) }
            }
        }
        pickFileLauncher.launch(arrayOf("*/*"))
    }

    private suspend fun importFromSafTree(context: Context, treeUri: Uri, proxySource: GgtgProxySource, progressDialog: TdataImportProgressDialog, createBackup: Boolean = false): GgtgImportSupport.ImportResult {
        val staging = File(ApplicationLoader.applicationContext.filesDir, STAGING_DIR)
        val accounts = withContext(Dispatchers.IO) {
            if (staging.exists()) staging.deleteRecursively()
            staging.mkdirs()
            copySafTreeToStaging(context, treeUri, staging)
            TdataReader.readForeignTdataAuthorizations(staging)
        }
        try {
            return importAccounts(accounts, proxySource, progressDialog, createBackup)
        } finally {
            // ng: only clean up staging AFTER importAccounts finishes - it reads each account's
            // sourceFolder (inside staging) to look for a sidecar 2FA password file.
            withContext(Dispatchers.IO) { staging.deleteRecursively() }
        }
    }

    private suspend fun importFromArchive(context: Context, fileUri: Uri, proxySource: GgtgProxySource?, progressDialog: TdataImportProgressDialog, createBackup: Boolean = false): GgtgImportSupport.ImportResult {
        val staging = File(ApplicationLoader.applicationContext.filesDir, STAGING_DIR)
        val archiveCopy = File(ApplicationLoader.applicationContext.cacheDir, ARCHIVE_TEMP_NAME)
        // ng: a .ggtg pack from GGTG Desktop's "Экспорт" already holds sessions minted for this
        // phone - install them directly instead of treating it as a tdata archive
        val isPack = withContext(Dispatchers.IO) {
            context.contentResolver.openInputStream(fileUri)?.use { input ->
                archiveCopy.outputStream().use { out -> input.copyTo(out) }
            }
            GgtgPackImporter.isPack(archiveCopy)
        }
        if (isPack) {
            try {
                return GgtgPackImporter.import(archiveCopy, proxySource, progressDialog, createBackup)
            } finally {
                withContext(Dispatchers.IO) { archiveCopy.delete() }
            }
        }
        val accounts = withContext(Dispatchers.IO) {
            if (staging.exists()) staging.deleteRecursively()
            staging.mkdirs()
            try {
                val name = queryDisplayName(context, fileUri)?.lowercase() ?: ""
                if (name.endsWith(".rar")) {
                    com.github.junrar.Junrar.extract(archiveCopy, staging)
                } else {
                    extractZip(archiveCopy, staging)
                }
            } finally {
                archiveCopy.delete()
            }
            TdataReader.readForeignTdataAuthorizations(staging)
        }
        try {
            return importAccounts(accounts, proxySource ?: error("нужен источник прокси"), progressDialog, createBackup)
        } finally {
            withContext(Dispatchers.IO) { staging.deleteRecursively() }
        }
    }

    private fun extractZip(src: File, destDir: File) {
        ZipInputStream(src.inputStream()).use { zis ->
            var entry = zis.nextEntry
            while (entry != null) {
                val outFile = File(destDir, entry.name)
                // Zip Slip: an entry such as "../../shared_prefs/x" must never leave destDir
                val root = destDir.canonicalPath + File.separator
                if (!outFile.canonicalPath.startsWith(root)) {
                    throw IllegalStateException("Небезопасный путь в архиве: ${entry.name}")
                }
                if (entry.isDirectory) {
                    outFile.mkdirs()
                } else {
                    outFile.parentFile?.mkdirs()
                    outFile.outputStream().use { out -> zis.copyTo(out) }
                }
                zis.closeEntry()
                entry = zis.nextEntry
            }
        }
    }

    private fun queryDisplayName(context: Context, uri: Uri): String? {
        return context.contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use { cursor ->
            if (cursor.moveToFirst()) cursor.getString(0) else null
        }
    }

    private suspend fun importAccounts(accounts: List<TdataAccount>, proxySource: GgtgProxySource, progressDialog: TdataImportProgressDialog, createBackup: Boolean = false): GgtgImportSupport.ImportResult {
        if (accounts.isEmpty()) {
            return GgtgImportSupport.ImportResult(emptyList(), 0, 0, 0)
        }

        val tempEntry = proxySource.peekFirst()
        if (tempEntry == null) {
            return GgtgImportSupport.ImportResult(
                emptyList(), 0, 0, accounts.size,
                "Не задан прокси - добавьте хотя бы один. Дублирование сессии Desktop никогда не идёт напрямую."
            )
        }

        val toImport = mutableListOf<TdataAccount>()
        var alreadyLoggedIn = 0
        for (account in accounts) {
            val exists = (0 until UserConfig.MAX_ACCOUNT_COUNT).any { UserConfig.getInstance(it).clientUserId == account.userId }
            if (exists) alreadyLoggedIn++ else toImport += account
        }

        // exclude the account LoginActivity/IntroActivity itself is bound to right now
        // (UserConfig.selectedAccount, e.g. slot 0 on a first run) - it shares that
        // ConnectionsManager instance with the live UI screen, which fights our manual
        // importAuthKey/sendRequest calls over the same request queue
        val freeSlots = (0 until UserConfig.MAX_ACCOUNT_COUNT)
            .filter { it != UserConfig.selectedAccount && !UserConfig.getInstance(it).isClientActivated }
            .toMutableList()
        if (freeSlots.isEmpty()) {
            return GgtgImportSupport.ImportResult(emptyList(), alreadyLoggedIn, toImport.size, 0)
        }
        // one shared, never-activated slot used transiently to relay Desktop's approval - reused across accounts
        val tempSlot = freeSlots.removeAt(0)

        val imported = mutableListOf<Long>()
        var firstImportedSlot: Int? = null
        var noFreeSlot = 0
        var noFreeProxy = 0
        var failed = 0
        var passwordSkipped = 0
        var lastFailReason: String? = null
        // ng: tempSlot is ephemeral (wiped and reused between accounts, never persisted) so reusing
        // the same proxy for it every time is fine - it's still always proxied, just not a "real"
        // allocation. targetSlot becomes a real, permanent account though, and each one needs its
        // own genuinely distinct proxy (matching the desktop app's one-proxy-per-account model). Once
        // the pool runs out of unused proxies, remaining accounts are deliberately left unconnected
        // (red "no proxy" indicator in the switcher) rather than silently sharing one - reusing a
        // proxy across multiple live, concurrently-connected accounts is exactly what "never mixes
        // proxies between accounts" was meant to prevent.
        val tempProxy = tempEntry.toProxyBits()

        val total = toImport.size
        var index = 0
        for (account in toImport) {
            index++
            val targetSlot = freeSlots.removeFirstOrNull()
            if (targetSlot == null) {
                noFreeSlot++
                continue
            }
            // only pop (permanently remove) a proxy once we know there's actually a slot to use it on
            val targetEntry = proxySource.popNext()
            if (targetEntry == null) {
                noFreeProxy++
                freeSlots.add(0, targetSlot)
                continue
            }
            val targetProxy = targetEntry.toProxyBits()

            progressDialog.setWorking(null, index, total)
            synchronized(trace) { trace.clear() }
            val result = duplicateAccountViaHandoff(account, tempSlot, targetSlot, tempProxy, targetProxy, index, total, progressDialog)
            result.onSuccess { userId ->
                imported += userId
                if (firstImportedSlot == null) firstImportedSlot = targetSlot
                FileLog.d("tdata duplicate: activated slot=$targetSlot userId=$userId mainDc=${account.mainDcId}")
                if (createBackup) {
                    val uc = UserConfig.getInstance(targetSlot)
                    val phone = uc.currentUser?.phone.orEmpty()
                    val id = "${System.currentTimeMillis()}_${(1000..9999).random()}"
                    val proxyJson = org.json.JSONObject().apply {
                        put("type", "socks5")
                        put("host", targetProxy.address)
                        put("port", targetProxy.port)
                        put("user", targetProxy.username)
                        put("password", targetProxy.password)
                    }
                    val deviceJson = org.json.JSONObject().apply {
                        if (!uc.ncgDeviceModel.isNullOrEmpty()) put("deviceModel", uc.ncgDeviceModel)
                        if (!uc.ncgSystemVersion.isNullOrEmpty()) put("systemVersion", uc.ncgSystemVersion)
                        if (!uc.ncgAppVersion.isNullOrEmpty()) put("appVersion", uc.ncgAppVersion)
                        if (!uc.ncgSystemLang.isNullOrEmpty()) put("systemLangCode", uc.ncgSystemLang)
                        if (!uc.ncgCloudLang.isNullOrEmpty()) put("cloudLangCode", uc.ncgCloudLang)
                        if (!uc.ncgLangPack.isNullOrEmpty()) put("langPackName", uc.ncgLangPack)
                    }
                    val backup = GgtgSessions.StoredSession(
                        id = id,
                        userId = userId,
                        role = "backup",
                        source = "tdata_duplicate",
                        note = "Резервная сессия tdata от " + java.text.SimpleDateFormat("dd.MM.yyyy HH:mm", java.util.Locale.getDefault()).format(java.util.Date()),
                        state = "ok",
                        createdAt = System.currentTimeMillis() / 1000,
                        phone = phone,
                        name = GgtgAccounts.label(targetSlot),
                        folder = uc.ncgFolder.orEmpty(),
                        twoFactor = uc.ncgTwoFactor.orEmpty(),
                        mainDcId = account.mainDcId,
                        keys = account.keys,
                        device = deviceJson,
                        proxy = proxyJson
                    )
                    GgtgSessions.addStored(backup)
                    GgtgSessions.saveToVault(backup, account)
                }
            }.onFailure { t ->
                if (t is PasswordSkippedException) {
                    passwordSkipped++
                    FileLog.d("tdata duplicate: skipped userId=${account.userId}, 2FA password not provided")
                } else {
                    failed++
                    lastFailReason = "${t.message} [${traceText()}]"
                    FileLog.e("tdata duplicate failed for userId=${account.userId}", t)
                }
                // slot never got setCurrentUser'd - just clear its transient state so it stays free
                ConnectionsManager.getInstance(targetSlot).cleanup(true)
                freeSlots.add(0, targetSlot)
            }
        }

        // temp slot never gets activated - wipe Desktop's borrowed key, don't leave it sitting around
        ConnectionsManager.getInstance(tempSlot).cleanup(true)
        ConnectionsManager.setProxySettingsForAccount(tempSlot, false, "", 0, "", "", "")

        if (imported.isNotEmpty()) {
            GgtgImportSupport.selectImportedSlotIfIdle(firstImportedSlot)
        }
        return GgtgImportSupport.ImportResult(imported, alreadyLoggedIn, noFreeSlot, failed, lastFailReason, noFreeProxy, passwordSkipped)
    }

    /** Thrown to signal "user chose to skip this account rather than provide its 2FA password" -
     * distinct from a genuine failure, counted separately in [GgtgImportSupport.ImportResult.passwordSkipped]. */
    internal class PasswordSkippedException : Exception("2FA password not provided")

    /**
     * Approves a brand-new Android login for [account] using Desktop's own already-valid session
     * (installed transiently on [tempSlot]) - the software equivalent of scanning a QR code.
     * [targetSlot] gets its own fresh, independent auth key via the normal handshake tgnet already
     * does for any new connection; Desktop's key is never written into it.
     */
    internal suspend fun duplicateAccountViaHandoff(
        account: TdataAccount,
        tempSlot: Int,
        targetSlot: Int,
        tempProxy: GgtgProxyBits,
        targetProxy: GgtgProxyBits,
        index: Int,
        total: Int,
        progressDialog: TdataImportProgressDialog,
        // the device the new session is born with (a .ggtg pack carries one); null = a random phone
        device: org.json.JSONObject? = null
    ): Result<Long> {
        // proxy both slots BEFORE either one's ConnectionsManager is ever touched (construction
        // applies whatever proxy state UserConfig already holds at that moment)
        ConnectionsManager.setProxySettingsForAccount(tempSlot, true, tempProxy.address, tempProxy.port, tempProxy.username, tempProxy.password, tempProxy.secret)
        ConnectionsManager.setProxySettingsForAccount(targetSlot, true, targetProxy.address, targetProxy.port, targetProxy.username, targetProxy.password, targetProxy.secret)
        GgtgDevice.assignNew(targetSlot, device, isDesktop = false)

        FileLog.d("tdata handoff: installing temp key on slot=$tempSlot userId=${account.userId}")
        val tempCm = ConnectionsManager.getInstance(tempSlot)
        if (account.mainDcId != 0 && account.mainDcId != ConnectionsManager.DEFAULT_DATACENTER_ID) {
            tempCm.setDefaultDatacenterId(account.mainDcId)
        }
        for ((dcId, keyBytes) in account.keys) {
            tempCm.importAuthKey(dcId, keyBytes, GgtgImportSupport.authKeyIdFor(keyBytes), dcId == account.mainDcId)
        }
        tempCm.setUserId(account.userId)

        // ng: tempSlot already holds Desktop's real, authorized session at this point, so its own
        // phone number is available immediately - fetch it now purely so the progress dialog (and
        // any later 2FA prompt) can say which account this is, before the target login even starts.
        val phone = fetchSelfPhone(tempCm)
        progressDialog.setWorking(phone, index, total)

        val targetCm = ConnectionsManager.getInstance(targetSlot)

        // 0) the way Telegram Android itself signs in: by the number, with the code Telegram sends to the
        // account's other sessions - read here from the temp session, which is one of them. The new session is
        // then Telegram Android's own (this app's api_id, the phone shown by its real name); a login token can't
        // be had with that id any more (API_ID_PUBLISHED_FLOOD), only with Telegram Desktop's. Null = the code
        // didn't come to the app (an SMS is never used up for this): the login token below is the way then.
        signInByCode(tempCm, targetCm, targetSlot, phone, account, progressDialog)?.let { return it }

        // 1) target (unauthenticated, fresh) asks for a login token. Use the default DC, not the
        // account's home DC directly - a virgin ConnectionsManager only proactively handshakes its
        // current/default datacenter, so targeting an arbitrary other DC before any connection
        // exists just leaves the request queued forever with nothing driving a handshake for it.
        // The server redirects us via loginTokenMigrateTo if the token actually needs to be issued
        // elsewhere - this is also what real clients do.
        var dcId = ConnectionsManager.DEFAULT_DATACENTER_ID
        val exportReq = TLRPC.TL_auth_exportLoginToken().apply {
            api_id = GgtgApiOverride.id()
            api_hash = GgtgApiOverride.hash()
        }
        FileLog.d("tdata handoff: exportLoginToken on slot=$targetSlot dc=$dcId")
        tr("export dc=$dcId")
        var (response, error) = targetCm.sendSuspend(exportReq, ConnectionsManager.RequestFlagWithoutLogin, dcId, "exportLoginToken")
            ?: return Result.failure(Exception("exportLoginToken timed out"))
        if (error != null) {
            if (error.text == "SESSION_PASSWORD_NEEDED") {
                return resolvePasswordAndFinalize(targetCm, targetSlot, dcId, account, phone, progressDialog)
            }
            return Result.failure(Exception("exportLoginToken: ${error.text}"))
        }
        FileLog.d("tdata handoff: exportLoginToken response=${response?.javaClass?.simpleName}")
        tr("export->${response?.javaClass?.simpleName}")

        var token: ByteArray
        when (response) {
            is TLRPC.TL_auth_loginTokenMigrateTo -> {
                dcId = response.dc_id
                val importReq = TLRPC.TL_auth_importLoginToken().apply { this.token = response.token }
                FileLog.d("tdata handoff: migrated to dc=$dcId, importLoginToken")
                // RequestFlagEnableUnauthorized required: targetCm has no userId set yet (it's not
                // logged in - that's the whole point of this call), so tgnet's local "is this DC
                // authorized" bookkeeping can't self-heal via exportAuthorization (that path only
                // runs when currentUserId != 0). Same class of call as ordinary pre-login auth.* RPCs.
                val (r2, e2) = targetCm.sendSuspend(importReq, ConnectionsManager.RequestFlagWithoutLogin or ConnectionsManager.RequestFlagEnableUnauthorized, dcId, "importLoginToken(migrate)")
                    ?: return Result.failure(Exception("importLoginToken (migrate) timed out"))
                if (e2 != null) {
                    // ng: with 2FA the server reports the password requirement here, not on exportLoginToken
                    if (e2.text == "SESSION_PASSWORD_NEEDED") {
                        return resolvePasswordAndFinalize(targetCm, targetSlot, dcId, account, phone, progressDialog)
                    }
                    return Result.failure(Exception("importLoginToken after migrate: ${e2.text}"))
                }
                token = when (r2) {
                    is TLRPC.TL_auth_loginToken -> r2.token
                    is TLRPC.TL_auth_loginTokenSuccess -> return finalizeTargetAccount(targetSlot, dcId, r2.authorization)
                    else -> return Result.failure(Exception("unexpected response after migrate: $r2"))
                }
            }
            is TLRPC.TL_auth_loginToken -> {
                // no migration - the token is scoped to whichever concrete DC the sentinel just
                // resolved to for this (fresh, default-DC-2) account; pin it explicitly from here
                // on, since tempSlot's own "current DC" is a different concrete value (its real
                // home DC) and the sentinel would silently resolve differently there.
                dcId = 2
                token = response.token
            }
            is TLRPC.TL_auth_loginTokenSuccess -> return finalizeTargetAccount(targetSlot, dcId, response.authorization)
            else -> return Result.failure(Exception("unexpected exportLoginToken response: $response"))
        }

        // 2) Desktop's session (temp slot) approves that exact token - this is the whole "no camera needed"
        // trick: normally a second device scans the QR and calls this; we already hold the key that device
        // would use, so we call it directly.
        //
        // Send this on tempSlot's OWN home DC (DEFAULT_DATACENTER_ID, which resolves to
        // account.mainDcId - tempSlot's forced currentDatacenterId), NOT the DC the token happens to
        // live on. Confirmed live: sending it explicitly to the token's DC (2) gets a real, fast
        // response - but it's rpc error 303 USER_MIGRATE_4, since this authenticated user's account
        // actually lives on DC4. tgnet's own 303 handler calls moveToDatacenter() for that internally
        // but discards the original response outright (ignoreResult=true) rather than retrying it -
        // so a request explicitly pinned to the wrong DC just orphans and hangs to the timeout. Real
        // clients always send auth.acceptLoginToken through their own home DC; the server resolves
        // the token across DCs on its own.
        FileLog.d("tdata handoff: acceptLoginToken on slot=$tempSlot dc=default(home)")
        val acceptReq = TLRPC.TL_auth_acceptLoginToken().apply { this.token = token }
        val (acceptResp, acceptErr) = tempCm.sendSuspend(acceptReq, 0, ConnectionsManager.DEFAULT_DATACENTER_ID, "acceptLoginToken")
            ?: return Result.failure(Exception("acceptLoginToken timed out"))
        if (acceptErr != null) return Result.failure(Exception("acceptLoginToken: ${acceptErr.text}"))
        if (acceptResp !is TLRPC.TL_authorization) return Result.failure(Exception("unexpected acceptLoginToken response: $acceptResp"))
        FileLog.d("tdata handoff: acceptLoginToken OK")
        tr("accepted")

        // 3) target re-polls its OWN token status by calling exportLoginToken again (same RPC as
        // step 1, not importLoginToken - that one's for importing a login link on an already-
        // authorized session, a different use case; confirmed live, it just returns AUTH_TOKEN_EXPIRED
        // here). Once accepted, the token's identity is resolved to the real user, whose home DC may
        // differ from the DC the token was originally exported on (confirmed live: migrates to dc4) -
        // handled the same way as step 1's own migrate case.
        repeat(5) { attempt ->
            val pollReq = TLRPC.TL_auth_exportLoginToken().apply {
                api_id = GgtgApiOverride.id()
                api_hash = GgtgApiOverride.hash()
            }
            FileLog.d("tdata handoff: poll exportLoginToken attempt=$attempt dc=$dcId")
            tr("poll#$attempt dc=$dcId")
            val (pollResp, pollErr) = targetCm.sendSuspend(pollReq, ConnectionsManager.RequestFlagWithoutLogin, dcId, "pollExportLoginToken")
                ?: return Result.failure(Exception("poll exportLoginToken timed out"))
            if (pollErr != null) {
                if (pollErr.text == "SESSION_PASSWORD_NEEDED") {
                    return resolvePasswordAndFinalize(targetCm, targetSlot, dcId, account, phone, progressDialog)
                }
                return Result.failure(Exception("poll exportLoginToken: ${pollErr.text}"))
            }
            when (pollResp) {
                is TLRPC.TL_auth_loginTokenSuccess -> return finalizeTargetAccount(targetSlot, dcId, pollResp.authorization)
                is TLRPC.TL_auth_loginTokenMigrateTo -> {
                    dcId = pollResp.dc_id
                    val importReq = TLRPC.TL_auth_importLoginToken().apply { this.token = pollResp.token }
                    FileLog.d("tdata handoff: poll migrated to dc=$dcId, importLoginToken")
                    val (r2, e2) = targetCm.sendSuspend(importReq, ConnectionsManager.RequestFlagWithoutLogin or ConnectionsManager.RequestFlagEnableUnauthorized, dcId, "importLoginToken(pollMigrate)")
                        ?: return Result.failure(Exception("importLoginToken (poll migrate) timed out"))
                    if (e2 != null) {
                        if (e2.text == "SESSION_PASSWORD_NEEDED") {
                            return resolvePasswordAndFinalize(targetCm, targetSlot, dcId, account, phone, progressDialog)
                        }
                        return Result.failure(Exception("importLoginToken after poll migrate: ${e2.text}"))
                    }
                    when (r2) {
                        is TLRPC.TL_auth_loginTokenSuccess -> return finalizeTargetAccount(targetSlot, dcId, r2.authorization)
                        is TLRPC.TL_auth_loginToken -> if (attempt < 4) delay(400)
                        else -> return Result.failure(Exception("unexpected response after poll migrate: $r2"))
                    }
                }
                is TLRPC.TL_auth_loginToken -> {
                    if (attempt < 4) delay(400)
                }
                else -> return Result.failure(Exception("unexpected poll response: $pollResp"))
            }
        }
        return Result.failure(Exception("login token was approved but never finalized"))
    }

    /** ng: 2FA on the source Desktop account surfaces here - auth.exportLoginToken (either the
     * initial call or the post-accept poll) returns SESSION_PASSWORD_NEEDED instead of a token.
     * Tries the sidecar password file next to the tdata folder first, silently; if that's missing
     * or wrong, prompts through the progress dialog (which also offers Skip). A successfully
     * checked password completes the login on its own (auth.checkPassword's response IS the final
     * authorization, same shape as loginTokenSuccess.authorization) so this finalizes directly. */
    private suspend fun resolvePasswordAndFinalize(
        targetCm: ConnectionsManager,
        targetSlot: Int,
        dcId: Int,
        account: TdataAccount,
        phone: String?,
        progressDialog: TdataImportProgressDialog
    ): Result<Long> {
        // ng: app.ggtg.tdata.TdataPasswordFinder locates the note beside the tdata folder (twoFA.txt
        // with "TelegramExpert.pro: 6xfhsi" and similar) and returns candidates best-first. Stops
        // immediately on success or on any error that isn't "wrong password" (e.g. FLOOD_WAIT)
        // rather than blindly working through the rest of the list.
        tr("2FA needed dc=$dcId")
        val candidates = app.ggtg.tdata.TdataPasswordFinder.findCandidates(account.sourceFolder, account.userId, phone, File(ApplicationLoader.applicationContext.filesDir, STAGING_DIR))
            .toMutableList()
        // a password saved earlier (export mode 2, or a previous import) for this exact userId -
        // tried first since it's the most likely to still be right
        GgtgTwoFaStore.get(account.userId)?.let { saved -> if (saved.isNotEmpty() && saved !in candidates) candidates.add(0, saved) }
        GgtgSessions.listForUser(account.userId).firstOrNull { it.twoFactor.isNotEmpty() }?.let { s ->
            if (s.twoFactor !in candidates) candidates.add(0, s.twoFactor)
        }
        // shown in the prompt so it's clear whether the note was found and why it didn't work
        var noteHint: String? = null
        for (candidate in candidates) {
            val result = trySubmitPassword(targetCm, dcId, candidate)
            tr("note pw->${if (result.isSuccess) "ok" else result.exceptionOrNull()?.message}")
            if (result.isSuccess) {
                GgtgTwoFaStore.save(account.userId, candidate)
                return finalizeTargetAccount(targetSlot, dcId, result.getOrThrow())
            }
            val reason = result.exceptionOrNull()?.message
            noteHint = LocaleController.formatString("NicegramTdataNoteFailed", R.string.NicegramTdataNoteFailed, reason ?: "?")
            if (reason != "PASSWORD_HASH_INVALID") {
                FileLog.d("tdata handoff: sidecar candidate check failed hard ($reason), falling back to prompt")
                break
            }
        }

        var wrongPassword = false
        var hint = noteHint
        while (true) {
            val typed = progressDialog.askPasswordSuspend(phone, wrongPassword, hint)
                ?: return Result.failure(PasswordSkippedException())
            val result = trySubmitPassword(targetCm, dcId, typed)
            tr("typed pw->${if (result.isSuccess) "ok" else result.exceptionOrNull()?.message}")
            if (result.isSuccess) {
                GgtgTwoFaStore.save(account.userId, typed)
                return finalizeTargetAccount(targetSlot, dcId, result.getOrThrow())
            }
            val reason = result.exceptionOrNull()?.message ?: "checkPassword failed"
            FileLog.d("tdata handoff: typed password check failed: $reason")
            if (reason == "PASSWORD_HASH_INVALID") {
                wrongPassword = true
                hint = null
                continue
            }
            // rate limit: retrying right away would only extend the wait
            if (reason.startsWith("FLOOD_WAIT")) return Result.failure(Exception(reason))
            // anything else (timeout, SRP_ID_INVALID, network...) - keep the window up, show why,
            // and let the user retry or skip instead of silently dropping the account
            wrongPassword = false
            hint = LocaleController.formatString("NicegramTdataCheckFailed", R.string.NicegramTdataCheckFailed, reason)
        }
    }

    /** ng: mirrors LoginActivity's own SESSION_PASSWORD_NEEDED handling (account.getPassword ->
     * SRPHelper.getX/startCheck -> auth.checkPassword with TL_inputCheckPasswordSRP) - reuses the
     * existing SRP math rather than reimplementing it. */
    internal suspend fun trySubmitPassword(cm: ConnectionsManager, dcId: Int, password: String): Result<TLRPC.auth_Authorization> {
        // ng: tempCm/targetCm is still unauthenticated on this dc at this point (that's the whole
        // reason SESSION_PASSWORD_NEEDED fired) - RequestFlagWithoutLogin alone isn't enough to get
        // a request past tgnet's own local gate (ConnectionsManager.cpp: "skip queue: dc is
        // unauthorized"), it just sits client-side until sendSuspend's timeout fires without ever
        // reaching the network. RequestFlagEnableUnauthorized is what exportLoginToken/
        // importLoginToken already carry for the same reason - needed here too.
        val unauthFlags = ConnectionsManager.RequestFlagWithoutLogin or ConnectionsManager.RequestFlagEnableUnauthorized
        val (pwResp, pwErr) = cm.sendSuspend(TL_account.getPassword(), unauthFlags, dcId, "account.getPassword")
            ?: return Result.failure(Exception("getPassword timed out"))
        if (pwErr != null) return Result.failure(Exception(pwErr.text))
        val currentPassword = pwResp as? TL_account.Password ?: return Result.failure(Exception("unexpected getPassword response"))
        val algo = currentPassword.current_algo as? TLRPC.TL_passwordKdfAlgoSHA256SHA256PBKDF2HMACSHA512iter100000SHA256ModPow
            ?: return Result.failure(Exception("unsupported password algo"))
        val xBytes = SRPHelper.getX(AndroidUtilities.getStringBytes(password), algo)
        val srp = SRPHelper.startCheck(xBytes, currentPassword.srp_id, currentPassword.srp_B, algo)
            ?: return Result.failure(Exception("PASSWORD_HASH_INVALID"))
        val checkReq = TLRPC.TL_auth_checkPassword().apply { this.password = srp }
        val (checkResp, checkErr) = cm.sendSuspend(checkReq, ConnectionsManager.RequestFlagFailOnServerErrors or unauthFlags, dcId, "auth.checkPassword")
            ?: return Result.failure(Exception("checkPassword timed out"))
        if (checkErr != null) return Result.failure(Exception(checkErr.text))
        return when (checkResp) {
            is TLRPC.auth_Authorization -> Result.success(checkResp)
            else -> Result.failure(Exception("unexpected checkPassword response: $checkResp"))
        }
    }

    private const val SERVICE_USER_ID = 777000L
    private const val CODE_WAIT_MS = 60_000L

    private suspend fun signInByCode(
        tempCm: ConnectionsManager,
        targetCm: ConnectionsManager,
        targetSlot: Int,
        phone: String?,
        account: TdataAccount,
        progressDialog: TdataImportProgressDialog
    ): Result<Long>? {
        val digits = phone?.filter(Char::isDigit).orEmpty()
        if (digits.isEmpty()) return null
        val flags = ConnectionsManager.RequestFlagWithoutLogin or ConnectionsManager.RequestFlagFailOnServerErrors or
            ConnectionsManager.RequestFlagTryDifferentDc or ConnectionsManager.RequestFlagEnableUnauthorized
        val since = tempCm.currentTime - 30
        tr("code: sendCode")
        val (sent, sendError) = targetCm.sendSuspend(TLRPC.TL_auth_sendCode().apply {
            phone_number = digits
            api_id = BuildVars.APP_ID
            api_hash = BuildVars.APP_HASH
            settings = TLRPC.TL_codeSettings()
        }, flags, ConnectionsManager.DEFAULT_DATACENTER_ID, "sendCode") ?: return null
        val sentCode = sent as? TLRPC.TL_auth_sentCode
        if (sendError != null || sentCode == null) {
            tr("code: sendCode failed ${sendError?.text}")
            return null
        }
        val cancel: suspend () -> Unit = {
            targetCm.sendSuspend(TLRPC.TL_auth_cancelCode().apply {
                phone_number = digits
                phone_code_hash = sentCode.phone_code_hash
            }, flags, ConnectionsManager.DEFAULT_DATACENTER_ID, "cancelCode")
        }
        if (sentCode.type !is TLRPC.TL_auth_sentCodeTypeApp) {
            tr("code: not to the app (${sentCode.type?.javaClass?.simpleName})")
            cancel()
            return null
        }
        val code = waitForLoginCode(tempCm, since)
        if (code == null) {
            tr("code: didn't come")
            cancel()
            return null
        }
        tr("code: signIn")
        val (signed, signError) = targetCm.sendSuspend(TLRPC.TL_auth_signIn().apply {
            this.flags = this.flags or 1
            phone_number = digits
            phone_code_hash = sentCode.phone_code_hash
            phone_code = code
        }, flags, ConnectionsManager.DEFAULT_DATACENTER_ID, "signIn") ?: return null
        // tgnet moved its own datacenter on PHONE_MIGRATE_X: the login lives there
        val dcId = targetCm.currentDatacenterId
        if (signError != null) {
            if (signError.text == "SESSION_PASSWORD_NEEDED") {
                return resolvePasswordAndFinalize(targetCm, targetSlot, dcId, account, phone, progressDialog)
            }
            tr("code: signIn failed ${signError.text}")
            return null
        }
        val authorization = signed as? TLRPC.auth_Authorization ?: return null
        return finalizeTargetAccount(targetSlot, dcId, authorization)
    }

    /** The login code from Telegram's service notifications, as the temp session (the same account) sees them. */
    private suspend fun waitForLoginCode(tempCm: ConnectionsManager, since: Int): String? {
        val pattern = Regex("(?<!\\d)(\\d{5,6})(?!\\d)")
        val deadline = System.currentTimeMillis() + CODE_WAIT_MS
        while (System.currentTimeMillis() < deadline) {
            kotlinx.coroutines.delay(2_000)
            val (resp, err) = tempCm.sendSuspend(TLRPC.TL_messages_getDialogs().apply {
                offset_peer = TLRPC.TL_inputPeerEmpty()
                limit = 10
            }, 0, ConnectionsManager.DEFAULT_DATACENTER_ID, "getDialogs(code)") ?: continue
            if (err != null) continue
            val messages = (resp as? TLRPC.messages_Dialogs)?.messages ?: continue
            for (m in messages) {
                if (m.peer_id?.user_id != SERVICE_USER_ID || m.date < since) continue
                pattern.find(m.message ?: "")?.let { return it.groupValues[1] }
            }
        }
        return null
    }

    private suspend fun fetchSelfPhone(cm: ConnectionsManager): String? {
        val (resp, err) = cm.sendSuspend(
            TLRPC.TL_users_getUsers().apply { id.add(TLRPC.TL_inputUserSelf()) },
            0, ConnectionsManager.DEFAULT_DATACENTER_ID, "getUsers(self,temp)"
        ) ?: return null
        if (err != null) return null
        val user = (resp as? Vector<*>)?.objects?.firstOrNull { it is TLRPC.User } as? TLRPC.User
        return user?.phone?.takeIf { it.isNotEmpty() }?.let { "+$it" }
    }

    private suspend fun finalizeTargetAccount(targetSlot: Int, dcId: Int, authorization: TLRPC.auth_Authorization): Result<Long> {
        tr("finalize ${authorization.javaClass.simpleName}")
        if (authorization !is TLRPC.TL_auth_authorization) {
            return Result.failure(Exception("account requires sign-up - can't happen for an existing Desktop session"))
        }
        var user = authorization.user
        val targetCm = ConnectionsManager.getInstance(targetSlot)
        // must match UserConfig's clientUserId below, so tgnet's own request-authorization
        // bookkeeping (ConnectionsManager.cpp ~2873/3118) treats this DC as belonging to a real,
        // logged-in user rather than an anonymous one.
        targetCm.setUserId(user.id)
        // The login itself can complete on a DC other than targetSlot's native currentDatacenterId
        // (confirmed live: migrates to dc4) - our own migrate handling above only updates the LOCAL
        // dcId variable, never tgnet's native bookkeeping, so DEFAULT_DATACENTER_ID-routed requests
        // (which is most of the app's normal post-login traffic: registerDevice, config fetch, etc.)
        // kept resolving back to the stale original DC, whose key was never actually signed in -
        // AUTH_KEY_UNREGISTERED there, which trips tgnet's own auto-logout (onLogout ->
        // UserConfig.clearConfig()) within ~100ms, silently wiping the whole account right after
        // activation. setDefaultDatacenterId is safe to call here specifically (unlike on tempSlot's
        // accept step earlier) because moveToDatacenter's own native code skips its blocking handshake
        // path entirely whenever the target DC already has a permanent key (Datacenter::hasAuthKey) -
        // true here, since the login that just succeeded is exactly what put that key there.
        if (dcId != ConnectionsManager.DEFAULT_DATACENTER_ID) {
            targetCm.setDefaultDatacenterId(dcId)
        }
        return GgtgImportSupport.activateSlot(targetSlot, user, dcId)
    }

    /** Bridges the callback-based ConnectionsManager.sendRequest into a suspend call, with a hard
     * timeout so a stuck connection fails loudly instead of hanging the whole import forever. */
    internal suspend fun ConnectionsManager.sendSuspend(
        request: TLObject,
        flags: Int,
        dcId: Int,
        label: String
    ): Pair<TLObject?, TLRPC.TL_error?>? = withTimeoutOrNull(60_000) {
        suspendCancellableCoroutine { cont ->
            FileLog.d("tdata handoff: sending $label (flags=$flags dc=$dcId)")
            val token = sendRequest(request, { resp, err ->
                FileLog.d("tdata handoff: $label responded resp=${resp?.javaClass?.simpleName} err=${err?.text}")
                if (cont.isActive) cont.resume(resp to err)
            }, null, null, flags, dcId, ConnectionsManager.ConnectionTypeGeneric, true)
            FileLog.d("tdata handoff: $label queued as token=$token")
        }
    }

    private fun copySafTreeToStaging(context: Context, treeUri: Uri, destDir: File) {
        val resolver = context.contentResolver
        val rootDocId = DocumentsContract.getTreeDocumentId(treeUri)
        val rootUri = DocumentsContract.buildDocumentUriUsingTree(treeUri, rootDocId)
        walkSafTree(resolver, treeUri, rootUri, destDir)
    }

    private fun walkSafTree(resolver: android.content.ContentResolver, treeUri: Uri, dirUri: Uri, destDir: File) {
        destDir.mkdirs()
        val childrenUri = DocumentsContract.buildChildDocumentsUriUsingTree(treeUri, DocumentsContract.getDocumentId(dirUri))
        resolver.query(
            childrenUri,
            arrayOf(
                DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                DocumentsContract.Document.COLUMN_MIME_TYPE
            ),
            null, null, null
        )?.use { cursor ->
            while (cursor.moveToNext()) {
                val docId = cursor.getString(0)
                val name = cursor.getString(1) ?: continue
                val mime = cursor.getString(2)
                val childUri = DocumentsContract.buildDocumentUriUsingTree(treeUri, docId)
                if (mime == DocumentsContract.Document.MIME_TYPE_DIR) {
                    walkSafTree(resolver, treeUri, childUri, File(destDir, name))
                } else {
                    resolver.openInputStream(childUri)?.use { input ->
                        File(destDir, name).outputStream().use { out -> input.copyTo(out) }
                    }
                }
            }
        }
    }
}
