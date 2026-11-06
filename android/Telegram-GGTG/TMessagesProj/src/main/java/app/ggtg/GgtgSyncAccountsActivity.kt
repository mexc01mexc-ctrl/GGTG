package app.ggtg

import android.content.Context
import android.view.View
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.Toast
import app.ggtg.ui.TdataImportProgressDialog
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.telegram.messenger.AndroidUtilities
import org.telegram.messenger.BuildVars
import org.telegram.messenger.R
import org.telegram.messenger.UserConfig
import org.telegram.tgnet.ConnectionsManager
import org.telegram.tgnet.TLRPC
import org.telegram.ui.ActionBar.ActionBar.ActionBarMenuOnItemClick
import org.telegram.ui.ActionBar.AlertDialog
import org.telegram.ui.ActionBar.BaseFragment
import org.telegram.ui.ActionBar.Theme
import org.telegram.ui.Components.LayoutHelper
import kotlin.coroutines.resume

// ng: on-demand session transfer for accounts from GGTG Desktop: the Mac mints a separate Desktop session
// per account and this phone runs it as it is (no Android sessions - see GgtgPackImporter).
// The catalog of available accounts is exchanged automatically through the main account's sync.
class GgtgSyncAccountsActivity : BaseFragment() {

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main)
    private var peerPicker: GgtgPeerAccountPicker? = null
    private var btnMint: View? = null

    override fun createView(context: Context): View {
        actionBar.setBackButtonImage(R.drawable.ic_ab_back)
        actionBar.setAllowOverlayTitle(true)
        actionBar.setTitle("Синхронизация аккаунтов")
        actionBar.setActionBarMenuOnItemClick(object : ActionBarMenuOnItemClick() {
            override fun onItemClick(id: Int) { if (id == -1) finishFragment() }
        })
        val frame = FrameLayout(context)
        frame.setBackgroundColor(Theme.getColor(Theme.key_windowBackgroundGray))
        fragmentView = frame
        val root = GgtgUi.page(context, frame)

        GgtgUi.addCard(root, GgtgUi.infoCard(context, R.drawable.msg_info, "Аккаунты с Desktop",
            "На Desktop выберите аккаунты и нажмите «Отправить на телефон» (Синхронизация). Для каждого Desktop создаёт отдельную сессию и кладёт её в «Избранное» главного аккаунта: она будет ждать здесь, даже если Desktop выключен. Телефон ставит её как есть."), top = 10f)

        val readyRow = GgtgUi.actionCard(context, R.drawable.msg_download, "Принять аккаунты с Desktop",
            "Установить на этот телефон все аккаунты, которые Desktop уже отправил и которые ещё никто не забрал") {
            installReady()
        }
        GgtgUi.addCard(root, readyRow, top = 10f)

        GgtgUi.addCard(root, GgtgUi.infoCard(context, R.drawable.msg_channel, "Аккаунты этого телефона на Desktop",
            "На Desktop: «Синхронизация» → «Аккаунты с телефона Android» → «Войти по QR». Телефон сам подтвердит вход этим аккаунтом, прокси, язык и время берутся с аккаунта."), top = 14f)

        root.addView(GgtgUi.spacer(context))

        updateMintButton()
        return frame
    }

    private fun updateMintButton() {
        val selected = peerPicker?.selectedAccounts().orEmpty()
        btnMint?.let { btn ->
            if (btn is android.widget.TextView) {
                btn.text = if (selected.isNotEmpty()) "Получить сессии с Desktop (${selected.size})" else "Получить сессии с Desktop"
            }
            btn.visibility = if (selected.isNotEmpty()) View.VISIBLE else View.GONE
        }
    }

    private fun refreshPeerAccounts() {
        toast("Обновление списка аккаунтов...")
        GgtgSyncTransport.runSync { report ->
            AndroidUtilities.runOnUIThread {
                peerPicker?.rebuild()
                updateMintButton()
                toast(if (report.ok) "Список аккаунтов обновлён" else "Синхронизация: ${report.text}")
            }
        }
    }

    /** Installs every session the Mac has already put up (the "waiting" count of the sync status) and clears it from the queue. */
    private fun installReady() {
        val activity = parentActivity ?: return
        GgtgSyncTransport.findIncoming { list, error ->
            if (error.isNotEmpty()) { toast(error); return@findIncoming }
            if (list.isEmpty()) { toast("От Desktop пока ничего не отправлено"); return@findIncoming }
            AlertDialog.Builder(activity)
                .setTitle("Принять аккаунты с Desktop")
                .setMessage("Будет принято отправок от Desktop: ${list.size}. Аккаунты установятся на этом телефоне и уйдут из очереди. Одна сессия работает только на одном устройстве: не принимайте, если их ждёт другой телефон.")
                .setPositiveButton("Принять") { _, _ -> runInstallReady(list) }
                .setNegativeButton("Отмена", null)
                .show()
        }
    }

    private fun runInstallReady(list: List<GgtgSyncTransport.Remote>) {
        val activity = parentActivity ?: return
        val progress = TdataImportProgressDialog(activity, "Установка готовых сессий")
        progress.show()
        val process = GgtgProcess.start("Установка готовых сессий (${list.size})", GgtgProcess.Kind.Other)
        process.progress(0, list.size)
        scope.launch {
            var installed = 0
            val problems = ArrayList<String>()
            for ((index, remote) in list.withIndex()) {
                progress.setWorking("сессия ${index + 1}", index + 1, list.size)
                val bytes = kotlin.coroutines.suspendCoroutine<ByteArray?> { cont ->
                    GgtgSyncTransport.fetchIncoming(remote) { data, _ -> cont.resume(data) }
                }
                if (bytes == null) { problems.add("не скачалась или не расшифровалась"); continue }
                val file = java.io.File(org.telegram.messenger.ApplicationLoader.applicationContext.cacheDir, "ggtg_sync_ready.ggtg")
                try {
                    file.writeBytes(bytes)
                    if (!GgtgPackImporter.isPack(file)) { problems.add("это не сессия"); continue }
                    val res = withContext(Dispatchers.IO) { GgtgPackImporter.import(file, null, progress, createBackup = true) }
                    if (res.imported.isNotEmpty()) {
                        installed += res.imported.size
                        GgtgSyncTransport.deleteIncoming(remote)
                        res.imported.forEach { id ->
                            val slot = (0 until UserConfig.MAX_ACCOUNT_COUNT).firstOrNull {
                                UserConfig.getInstance(it).isClientActivated && UserConfig.getInstance(it).clientUserId == id
                            }
                            val acc = GgtgSync.getPeerAccounts().firstOrNull { it.userId == id }
                            if (slot != null && acc != null) {
                                val uc = UserConfig.getInstance(slot)
                                if (acc.folder.isNotEmpty()) uc.ncgFolder = acc.folder
                                if (acc.twoFactor.isNotEmpty()) uc.ncgTwoFactor = acc.twoFactor
                                uc.saveConfig(false)
                                if (acc.trashed) GgtgAccounts.setTrashed(listOf(slot), true)
                            }
                        }
                    } else problems.add(res.failReason ?: "не установилась")
                } catch (t: Throwable) {
                    org.telegram.messenger.FileLog.e(t)
                    problems.add("сбой: ${t.message ?: t.javaClass.simpleName}")
                } finally {
                    try { file.delete() } catch (_: Throwable) {}
                }
                process.progress(index + 1, list.size)
            }
            try { progress.dismiss() } catch (_: Throwable) {}
            if (problems.isEmpty()) process.succeed("Установлено: $installed") else process.fail("Установлено $installed, проблем: ${problems.size}")
            withContext(Dispatchers.Main) {
                peerPicker?.rebuild()
                toast("Установлено: $installed" + if (problems.isNotEmpty()) "; проблем: ${problems.size} (${problems.first()})" else "")
            }
        }
    }

    private fun mintSelected() {
        val activity = parentActivity ?: return
        val selected = peerPicker?.selectedAccounts().orEmpty()
        if (selected.isEmpty()) {
            toast("Выберите хотя бы один аккаунт")
            return
        }

        val availableSlots = (0 until UserConfig.MAX_ACCOUNT_COUNT).count {
            !UserConfig.getInstance(it).isClientActivated
        }
        if (availableSlots < selected.size) {
            inform("Лимит аккаунтов", "Недостаточно свободных слотов на телефоне (свободно: $availableSlots, выбрано: ${selected.size}).")
            return
        }

        val progress = TdataImportProgressDialog(activity, "Получение сессий с Desktop")
        progress.show()
        val process = GgtgProcess.start("Получение сессий с Desktop (${selected.size})", GgtgProcess.Kind.Other)
        process.progress(0, selected.size)

        scope.launch {
            val successful = ArrayList<GgtgSync.PeerAccount>()
            val failed = ArrayList<Pair<GgtgSync.PeerAccount, String>>()
            var firstImportedSlot: Int? = null

            for ((index, acc) in selected.withIndex()) {
                progress.setWorking(acc.phone, index + 1, selected.size)
                process.log("+${acc.phone}: начало")
                // ggtg: nothing thrown while minting may take the whole app down - it becomes a failed row
                val (ok, resOrSlot, reason) = try {
                    mintSingleAccount(acc, progress, process)
                } catch (t: Throwable) {
                    org.telegram.messenger.FileLog.e(t)
                    Triple(false, null, "Сбой: ${t.message ?: t.javaClass.simpleName}")
                }
                process.progress(index + 1, selected.size)
                process.log("+${acc.phone}: " + if (ok) "готово, слот #$resOrSlot" else (reason ?: "ошибка"))
                if (ok) {
                    successful.add(acc)
                    if (firstImportedSlot == null && resOrSlot is Int) {
                        firstImportedSlot = resOrSlot
                    }
                } else {
                    failed.add(acc to (reason ?: "Ошибка авторизации"))
                }
            }

            try { progress.dismiss() } catch (_: Throwable) {}
            if (failed.isEmpty()) process.succeed("Создано сессий: ${successful.size}")
            else process.fail("Не создано: ${failed.size} из ${selected.size}")

            withContext(Dispatchers.Main) {
                peerPicker?.rebuild()
                updateMintButton()

                val act = parentActivity
                val reportMsg = buildString {
                    if (successful.isNotEmpty()) {
                        append("Получено сессий: ${successful.size}\n")
                        successful.forEach { append("• +${it.phone} (${it.firstName})\n") }
                    }
                    if (failed.isNotEmpty()) {
                        if (isNotEmpty()) append("\n")
                        append("Не удалось получить: ${failed.size}\n")
                        failed.forEach { append("• +${it.first.phone}: ${it.second}\n") }
                    }
                }

                val onDone = {
                    val launchAct = org.telegram.ui.LaunchActivity.instance
                    if (launchAct != null && !UserConfig.getInstance(launchAct.currentAccount).isClientActivated && firstImportedSlot != null) {
                        GgtgImportSupport.switchToImportedAccount(firstImportedSlot)
                    }
                }

                if (act != null && !act.isFinishing && !act.isDestroyed) {
                    try {
                        var handled = false
                        val handleDone = { if (!handled) { handled = true; onDone() } }
                        AlertDialog.Builder(act)
                            .setTitle("Получение сессий завершено")
                            .setMessage(reportMsg)
                            .setPositiveButton("Готово") { _, _ -> handleDone() }
                            .setOnDismissListener { handleDone() }
                            .show()
                    } catch (_: Throwable) {
                        toast(reportMsg)
                        onDone()
                    }
                } else {
                    toast(reportMsg)
                    onDone()
                }
            }
        }
    }

    /**
     * Asks the Mac (control message) for a separate session of this account, takes the pack it puts into
     * "Saved messages" and installs its keys as they are (no SMS, no e-mail, no new login).
     * null = it did not work out (no spare slot, Mac silent).
     */
    private suspend fun mintViaMac(
        acc: GgtgSync.PeerAccount,
        progress: TdataImportProgressDialog,
        process: GgtgProcess.Handle
    ): Triple<Boolean, Int?, String?>? {
        val free = (0 until UserConfig.MAX_ACCOUNT_COUNT).count {
            it != UserConfig.selectedAccount && !UserConfig.getInstance(it).isClientActivated
        }
        if (free < 1) {
            process.log("Нет свободных слотов для переноса с Desktop")
            return null
        }
        suspend fun packs(): List<GgtgSyncTransport.Remote>? = kotlin.coroutines.suspendCoroutine { cont ->
            GgtgSyncTransport.findIncoming { list, error -> cont.resume(if (error.isEmpty()) list else null) }
        }
        val before = packs()?.maxOfOrNull { it.msgId } ?: 0
        progress.setWorkingText("+${acc.phone}: просим Desktop подготовить сессию…")
        process.log("Запрос временной сессии у Desktop")
        val sent = kotlinx.coroutines.CompletableDeferred<String>()
        GgtgSyncTransport.sendControl(org.json.JSONObject()
            .put("type", "pack_req")
            .put("userId", acc.userId.toString())) { ok, text -> sent.complete(if (ok) "" else text) }
        val problem = kotlinx.coroutines.withTimeoutOrNull(20_000) { sent.await() }
        if (!problem.isNullOrEmpty()) {
            process.log("Запрос на Desktop не ушёл: $problem")
            progress.setWorkingText("+${acc.phone}: $problem")
            return null
        }
        progress.setWorkingText("+${acc.phone}: Desktop готовит сессию…")
        // ggtg: other sessions may be waiting in the queue (for another phone, or left over from earlier runs):
        // only a pack that holds THIS account is taken, the others are left where they are
        var remote: GgtgSyncTransport.Remote? = null
        val rejected = HashSet<Int>()
        val file = java.io.File(org.telegram.messenger.ApplicationLoader.applicationContext.cacheDir, "ggtg_sync_mint.ggtg")
        val until = System.currentTimeMillis() + 180_000
        while (System.currentTimeMillis() < until && remote == null) {
            kotlinx.coroutines.delay(4_000)
            val fresh = packs()?.filter { it.msgId > before && it.msgId !in rejected }?.sortedByDescending { it.msgId }.orEmpty()
            for (candidate in fresh) {
                val bytes = kotlin.coroutines.suspendCoroutine<ByteArray?> { cont ->
                    GgtgSyncTransport.fetchIncoming(candidate) { data, _ -> cont.resume(data) }
                } ?: continue
                file.writeBytes(bytes)
                if (GgtgPackImporter.isPack(file) && acc.userId in GgtgPackImporter.packUserIds(file)) {
                    remote = candidate
                    break
                }
                rejected.add(candidate.msgId)
            }
        }
        if (remote == null) {
            try { file.delete() } catch (_: Throwable) {}
            process.log("Desktop не прислал сессию этого аккаунта за 3 минуты")
            return null
        }
        process.log("Сессия от Desktop получена")
        try {
            if (!GgtgPackImporter.isPack(file)) return Triple(false, null, "Desktop прислал не сессию")
            val res = GgtgPackImporter.import(file, null, progress, createBackup = true)
            GgtgSyncTransport.deleteIncoming(remote)
            if (acc.userId !in res.imported) {
                return Triple(false, null, res.failReason ?: if (res.alreadyLoggedIn > 0) "Аккаунт уже есть на этом телефоне" else if (res.noFreeSlot > 0) "Нет свободного слота на телефоне" else "Не удалось создать сессию из временной")
            }
            val slot = (0 until UserConfig.MAX_ACCOUNT_COUNT).firstOrNull {
                UserConfig.getInstance(it).isClientActivated && UserConfig.getInstance(it).clientUserId == acc.userId
            } ?: return Triple(false, null, "Сессия создана, но слот не найден")
            val uc = UserConfig.getInstance(slot)
            if (acc.folder.isNotEmpty()) uc.ncgFolder = acc.folder
            if (acc.twoFactor.isNotEmpty()) uc.ncgTwoFactor = acc.twoFactor
            uc.saveConfig(false)
            if (acc.trashed) GgtgAccounts.setTrashed(listOf(slot), true)
            return Triple(true, slot, null)
        } finally {
            try { file.delete() } catch (_: Throwable) {}
        }
    }

    private suspend fun mintSingleAccount(
        acc: GgtgSync.PeerAccount,
        progress: TdataImportProgressDialog,
        process: GgtgProcess.Handle
    ): Triple<Boolean, Int?, String?> = withContext(Dispatchers.IO) {
        // The Mac makes a separate session of its own for this phone (control message, no SMS, no e-mail);
        // the phone installs it as it is. Telegram asks a login e-mail on every new Android login, so
        // there is no "log in here by code" way any more.
        mintViaMac(acc, progress, process)
            ?: Triple(false, null, "Desktop не ответил: откройте GGTG на Desktop с этим аккаунтом и повторите")
    }

    private suspend fun ConnectionsManager.sendSuspend(
        request: org.telegram.tgnet.TLObject,
        flags: Int = ConnectionsManager.RequestFlagWithoutLogin or ConnectionsManager.RequestFlagFailOnServerErrors,
        dcId: Int = ConnectionsManager.DEFAULT_DATACENTER_ID,
        label: String = ""
    ): Pair<org.telegram.tgnet.TLObject?, TLRPC.TL_error?>? = kotlinx.coroutines.withTimeoutOrNull(45_000) {
        kotlinx.coroutines.suspendCancellableCoroutine { cont ->
            val reqId = sendRequest(request, { resp, err ->
                if (cont.isActive) cont.resume(resp to err)
            }, null, null, flags, dcId, ConnectionsManager.ConnectionTypeGeneric, true)
            cont.invokeOnCancellation {
                cancelRequest(reqId, true)
            }
        }
    }

    private fun toast(text: String) { context?.let { Toast.makeText(it, text, Toast.LENGTH_LONG).show() } }

    private fun inform(title: String, text: String) {
        val context = context ?: return
        AlertDialog.Builder(context)
            .setTitle(title)
            .setMessage(text)
            .setPositiveButton("OK", null)
            .show()
    }
}
