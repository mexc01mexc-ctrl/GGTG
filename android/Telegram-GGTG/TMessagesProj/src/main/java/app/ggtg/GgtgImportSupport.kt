package app.ggtg

import android.content.Context
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlinx.coroutines.withTimeoutOrNull
import org.telegram.messenger.ApplicationLoader
import org.telegram.messenger.MessagesController
import org.telegram.messenger.UserConfig
import org.telegram.tgnet.ConnectionsManager
import org.telegram.tgnet.SerializedData
import org.telegram.tgnet.TLObject
import org.telegram.tgnet.TLRPC
import java.security.MessageDigest
import kotlin.coroutines.resume

// ng: the pieces of the account-import flow every import path (.ggtg pack, tdata handoff) needs:
// the result summary and activating a slot whose auth key was just installed. The progress window
// itself lives in app.ggtg.ui.TdataImportProgressDialog (shared - it also drives 2FA prompts).
object GgtgImportSupport {

    data class ImportResult(
        val imported: List<Long>,
        val alreadyLoggedIn: Int,
        val noFreeSlot: Int,
        val failed: Int,
        val failReason: String? = null,
        val noFreeProxy: Int = 0,
        val passwordSkipped: Int = 0
    )

    fun buildResultMessage(result: ImportResult): String {
        val msg = StringBuilder()
        fun line(text: String) { if (msg.isNotEmpty()) msg.append('\n'); msg.append(text) }
        if (result.imported.isNotEmpty()) line("Импортировано аккаунтов: ${result.imported.size}")
        if (result.alreadyLoggedIn > 0) line("Уже в приложении: ${result.alreadyLoggedIn}")
        if (result.noFreeSlot > 0) line("Нет свободных слотов: ${result.noFreeSlot}")
        if (result.noFreeProxy > 0) line("Не хватило прокси: ${result.noFreeProxy}")
        if (result.passwordSkipped > 0) line("Пропущено (без пароля 2FA): ${result.passwordSkipped}")
        if (result.failed > 0) line("Не удалось: ${result.failed}" + (result.failReason?.let { " ($it)" } ?: ""))
        if (msg.isEmpty()) msg.append("В файле не найдено аккаунтов")
        return msg.toString()
    }

    fun askCreateBackupDialog(context: Context, onChoice: (createBackup: Boolean) -> Unit) {
        org.telegram.ui.ActionBar.AlertDialog.Builder(context)
            .setTitle("Резервная копия сессии")
            .setMessage("Создать дубликат сессий перед добавлением?\n\nЕсли выбрано «Да», для каждого аккаунта будет сохранена и активная сессия в приложении, и исходная сессия в локальном хранилище (Session Vault) как резервная копия.")
            .setPositiveButton("Да (сохранить дубликат)") { _, _ -> onChoice(true) }
            .setNegativeButton("Нет (только импорт)") { _, _ -> onChoice(false) }
            .show()
    }

    /** MTProto convention: auth_key_id = last 8 bytes of SHA1(authKey), read little-endian. */
    fun authKeyIdFor(authKey: ByteArray): Long {
        val digest = MessageDigest.getInstance("SHA-1").digest(authKey)
        var v = 0L
        for (i in 7 downTo 0) v = (v shl 8) or (digest[12 + i].toLong() and 0xFF)
        return v
    }

    private suspend fun ConnectionsManager.sendSuspend(request: TLObject, dcId: Int = ConnectionsManager.DEFAULT_DATACENTER_ID): Pair<TLObject?, TLRPC.TL_error?>? =
        withTimeoutOrNull(20_000) {
            suspendCancellableCoroutine { cont ->
                sendRequest(request, { resp, err -> if (cont.isActive) cont.resume(resp to err) }, null, null, 0,
                    dcId, ConnectionsManager.ConnectionTypeGeneric, true)
            }
        }

    /**
     * Finishes an import for [targetSlot]: fetches the real self user over the just-installed key,
     * stores it as the slot's current user (written synchronously - the app is restarted right after
     * an import and UserConfig's own deferred write would lose the race) and returns its id.
     */
    suspend fun activateSlot(targetSlot: Int, fallbackUser: TLRPC.User?, mainDcId: Int = 0): Result<Long> {
        val cm = ConnectionsManager.getInstance(targetSlot)
        if (mainDcId != 0 && mainDcId != ConnectionsManager.DEFAULT_DATACENTER_ID) {
            cm.setDefaultDatacenterId(mainDcId)
        }
        val targetDc = if (mainDcId != 0) mainDcId else ConnectionsManager.DEFAULT_DATACENTER_ID
        val result = cm.sendSuspend(TLRPC.TL_users_getUsers().apply { id.add(TLRPC.TL_inputUserSelf()) }, targetDc)
        val resp = result?.first
        @Suppress("UNCHECKED_CAST")
        val fetched = if (result?.second == null && resp is org.telegram.tgnet.Vector<*>) {
            resp.objects.firstOrNull { it is TLRPC.User } as? TLRPC.User
        } else null
        val user: TLRPC.User = fetched ?: fallbackUser
            ?: return Result.failure(Exception("getUsers(self): ${result?.second?.text ?: "нет ответа"}"))
        user.self = true
        user.flags = user.flags or 1024
        val uc = UserConfig.getInstance(targetSlot)
        uc.setCurrentUser(user)
        uc.saveConfig(true)
        val prefsName = if (targetSlot == 0) "userconfing" else "userconfig$targetSlot"
        val data = SerializedData(user.getObjectSize())
        user.serializeToStream(data)
        ApplicationLoader.applicationContext.getSharedPreferences(prefsName, Context.MODE_PRIVATE).edit()
            .putString("user", android.util.Base64.encodeToString(data.toByteArray(), android.util.Base64.DEFAULT)).commit()
        data.cleanup()
        kotlinx.coroutines.withContext(kotlinx.coroutines.Dispatchers.Main) {
            try {
                MessagesController.getInstance(targetSlot).putUser(user, false)
                org.telegram.messenger.NotificationCenter.getInstance(targetSlot).postNotificationName(org.telegram.messenger.NotificationCenter.mainUserInfoChanged)
                org.telegram.messenger.NotificationCenter.getGlobalInstance().postNotificationName(org.telegram.messenger.NotificationCenter.mainUserInfoChanged)
                org.telegram.messenger.NotificationCenter.getInstance(targetSlot).postNotificationName(org.telegram.messenger.NotificationCenter.dialogsNeedReload)
            } catch (t: Throwable) {
                org.telegram.messenger.FileLog.e(t)
            }
        }
        return Result.success(user.id)
    }

    /** The slot the login screen is bound to stays empty after an import - move to the imported account instead. */
    fun selectImportedSlotIfIdle(firstImportedSlot: Int?) {
        if (!UserConfig.getInstance(UserConfig.selectedAccount).isClientActivated) {
            firstImportedSlot?.let { slot ->
                UserConfig.selectedAccount = slot
                UserConfig.getInstance(0).saveConfig(true)
                ApplicationLoader.applicationContext.getSharedPreferences("userconfing", Context.MODE_PRIVATE)
                    .edit().putInt("selectedAccount", slot).commit()
            }
        }
    }
}
