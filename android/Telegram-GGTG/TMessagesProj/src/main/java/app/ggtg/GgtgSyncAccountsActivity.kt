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

// ng: on-demand session minting for accounts from GGTG Desktop: each account gets a new,
// independent mobile session born directly on Android with authentic hardware parameters.
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

        GgtgUi.addCard(root, GgtgUi.infoCard(context, R.drawable.msg_info, "Безопасная синхронизация без риска бана",
            "Сессии создаются прямо на телефоне с генерацией уникальных характеристик устройства (модель, версия Android, язык, часовой пояс) и привязкой прокси. Telegram регистрирует сессию как чистое мобильное приложение. Сессии на Desktop продолжают работать параллельно."), top = 10f)

        // Section: Accounts on Desktop (Mint on Android)
        root.addView(GgtgUi.sectionLabel(context, "Доступные аккаунты с Desktop (Mac)"))
        val peerCard = GgtgUi.card(context)
        val pp = GgtgPeerAccountPicker(context)
        pp.setBackgroundColor(android.graphics.Color.TRANSPARENT)
        peerCard.addView(pp, LinearLayout.LayoutParams(LayoutHelper.MATCH_PARENT, LayoutHelper.WRAP_CONTENT))
        GgtgUi.addCard(root, peerCard)
        peerPicker = pp

        val mintButton = GgtgUi.button(context, "Создать мобильные сессии на Android") { mintSelected() }
        mintButton.visibility = if (pp.selectedAccounts().isNotEmpty()) View.VISIBLE else View.GONE
        GgtgUi.addButton(root, mintButton, top = 10f)
        btnMint = mintButton

        pp.onSelectionChanged = {
            updateMintButton()
        }

        val refreshRow = GgtgUi.actionCard(context, R.drawable.msg_retry, "Обновить список с Desktop",
            "Запросить свежий список доступных аккаунтов через главное синхро-облако") {
            refreshPeerAccounts()
        }
        GgtgUi.addCard(root, refreshRow, top = 8f)

        GgtgUi.addCard(root, GgtgUi.infoCard(context, R.drawable.msg_channel, "Аккаунты этого телефона на Mac",
            "На Mac: «Синхронизация» → «Аккаунты с телефона Android» → «Войти по QR». Телефон сам подтвердит вход этим аккаунтом, прокси, язык и время берутся с аккаунта. Для аккаунтов Mac на телефоне код тоже приходит сам."), top = 14f)

        root.addView(GgtgUi.spacer(context))

        updateMintButton()
        return frame
    }

    private fun updateMintButton() {
        val selected = peerPicker?.selectedAccounts().orEmpty()
        btnMint?.let { btn ->
            if (btn is android.widget.TextView) {
                btn.text = if (selected.isNotEmpty()) "Создать сессии на Android (${selected.size})" else "Создать сессии на Android"
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

        val progress = TdataImportProgressDialog(activity, "Создание сессий на Android")
        progress.show()
        val process = GgtgProcess.start("Создание сессий с Desktop (${selected.size})", GgtgProcess.Kind.Other)
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
                        append("Успешно создано мобильных сессий: ${successful.size}\n")
                        successful.forEach { append("• +${it.phone} (${it.firstName})\n") }
                    }
                    if (failed.isNotEmpty()) {
                        if (isNotEmpty()) append("\n")
                        append("Не удалось создать: ${failed.size}\n")
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
                            .setTitle("Создание сессий завершено")
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

    private suspend fun mintSingleAccount(
        acc: GgtgSync.PeerAccount,
        progress: TdataImportProgressDialog,
        process: GgtgProcess.Handle
    ): Triple<Boolean, Int?, String?> = withContext(Dispatchers.IO) {
        val digits = acc.phone.filter(Char::isDigit)
        if (digits.isEmpty()) return@withContext Triple(false, null, "Номер телефона пуст")

        val freeSlot = (0 until UserConfig.MAX_ACCOUNT_COUNT).firstOrNull {
            !UserConfig.getInstance(it).isClientActivated
        } ?: return@withContext Triple(false, null, "Достигнут лимит аккаунтов на телефоне")

        val cm = ConnectionsManager.getInstance(freeSlot)
        val uc = UserConfig.getInstance(freeSlot)

        fun cleanupSlot() {
            try {
                uc.clearConfig()
                ConnectionsManager.setProxySettingsForAccount(freeSlot, false, "", 0, "", "", "")
                cm.setAsleep(true)
            } catch (_: Throwable) {}
        }

        // 1. Setup proxy if account has one
        val proxy = acc.proxy
        if (proxy != null && proxy.has("host") && proxy.optString("host").isNotEmpty()) {
            val host = proxy.optString("host")
            val port = proxy.optInt("port", 1080)
            val user = proxy.optString("user")
            val pass = proxy.optString("password")
            val secret = proxy.optString("secret")
            // ggtg: a dead proxy would only show up as a long silent wait - check it first
            val reachable = try {
                java.net.Socket().use { it.connect(java.net.InetSocketAddress(host, port), 6000) }
                true
            } catch (_: Throwable) { false }
            if (!reachable) {
                process.log("Прокси $host:$port не отвечает")
                return@withContext Triple(false, null, "Прокси $host:$port не отвечает")
            }
            if (proxy.optString("type") == "http") {
                return@withContext Triple(false, null, "HTTP-прокси не поддерживается на Android (нужен SOCKS5 или MTProto)")
            }
            ConnectionsManager.setProxySettingsForAccount(freeSlot, true, host, port, user, pass, secret)
            // ggtg: keep the account's own proxy with the account (shown in its info, used after restart, exported again)
            uc.ncgProxyEnabled = true
            uc.ncgProxyAddress = host
            uc.ncgProxyPort = port
            uc.ncgProxyUsername = user
            uc.ncgProxyPassword = pass
            uc.ncgProxySecret = secret
            uc.saveConfig(false)
        } else {
            ConnectionsManager.setProxySettingsForAccount(freeSlot, false, "", 0, "", "", "")
        }

        // 2. Setup authentic Android device parameters (model, SDK, app build, tz, languages)
        GgtgDevice.randomizeForPhoneLogin(freeSlot, acc.phone)

        val targetAppId = BuildVars.APP_ID
        val targetAppHash = BuildVars.APP_HASH
        val targetDc = ConnectionsManager.DEFAULT_DATACENTER_ID

        val flags = ConnectionsManager.RequestFlagWithoutLogin or ConnectionsManager.RequestFlagFailOnServerErrors

        // ggtg: a free slot is kept asleep (no network) by earlier imports / cleanups; without
        // waking it the code request never leaves the phone and just times out
        cm.setAsleep(false)

        // 3. Send code
        process.log("Запрос кода в Telegram")
        progress.setWorkingText("+${acc.phone}: запрос кода…")
        val (sent, sendError) = cm.sendSuspend(TLRPC.TL_auth_sendCode().apply {
            phone_number = digits
            api_id = targetAppId
            api_hash = targetAppHash
            settings = TLRPC.TL_codeSettings()
        }, flags, targetDc, "sendCode") ?: run {
            cleanupSlot()
            return@withContext Triple(false, null, "Таймаут отправки кода")
        }

        if (sendError != null || sent !is TLRPC.TL_auth_sentCode) {
            cleanupSlot()
            val errText = sendError?.text ?: "Ошибка отправки кода"
            return@withContext Triple(false, null, "Ошибка Telegram: $errText")
        }

        val phoneCodeHash = sent.phone_code_hash

        // 4. The code lands in Telegram on the Mac: it passes it back by itself (the sync); by hand only if it does not
        process.log("Ждём код от Mac")
        progress.setWorkingText("+${acc.phone}: ждём код от Mac…")
        val auto = GgtgSyncTransport.requestLoginCode(acc.userId, 75_000)
        val code = if (!auto.isNullOrBlank()) auto else progress.askCodeSuspend(
            acc.phone,
            "Код подтверждения отправлен в Telegram на Mac\nдля +${acc.phone}"
        )
        if (code.isNullOrBlank()) {
            cm.sendSuspend(TLRPC.TL_auth_cancelCode().apply {
                phone_number = digits
                phone_code_hash = phoneCodeHash
            }, flags, targetDc, "cancelCode")
            cleanupSlot()
            return@withContext Triple(false, null, "Ввод кода отменён")
        }

        // 5. Sign in
        process.log("Вход по коду")
        progress.setWorkingText("+${acc.phone}: вход…")
        val (signed, signError) = cm.sendSuspend(TLRPC.TL_auth_signIn().apply {
            this.flags = this.flags or 1
            phone_number = digits
            phone_code_hash = phoneCodeHash
            phone_code = code
        }, flags, targetDc, "signIn") ?: run {
            cleanupSlot()
            return@withContext Triple(false, null, "Таймаут авторизации")
        }

        val dcId = if (cm.currentDatacenterId != 0 && cm.currentDatacenterId != ConnectionsManager.DEFAULT_DATACENTER_ID) {
            cm.currentDatacenterId
        } else if (acc.mainDcId != 0) {
            acc.mainDcId
        } else {
            ConnectionsManager.DEFAULT_DATACENTER_ID
        }

        var authorization: TLRPC.auth_Authorization? = null

        if (signError != null) {
            if (signError.text == "SESSION_PASSWORD_NEEDED") {
                // Check 2FA
                var passwordSuccess = false
                if (acc.twoFactor.isNotEmpty()) {
                    val pwRes = TdataImportHelper.trySubmitPassword(cm, dcId, acc.twoFactor)
                    if (pwRes.isSuccess) {
                        authorization = pwRes.getOrNull()
                        passwordSuccess = true
                        GgtgTwoFaStore.save(acc.userId, acc.twoFactor)
                    }
                }

                if (!passwordSuccess) {
                    var wrong = false
                    var hint: String? = null
                    while (true) {
                        val typed = progress.askPasswordSuspend(acc.phone, wrong, hint)
                        if (typed == null) {
                            cleanupSlot()
                            return@withContext Triple(false, null, "Ввод 2FA отменён")
                        }
                        val pwRes = TdataImportHelper.trySubmitPassword(cm, dcId, typed)
                        if (pwRes.isSuccess) {
                            authorization = pwRes.getOrNull()
                            GgtgTwoFaStore.save(acc.userId, typed)
                            break
                        }
                        val reason = pwRes.exceptionOrNull()?.message ?: "checkPassword failed"
                        if (reason == "PASSWORD_HASH_INVALID") {
                            wrong = true
                            hint = null
                            continue
                        }
                        if (reason.startsWith("FLOOD_WAIT")) {
                            cleanupSlot()
                            return@withContext Triple(false, null, "Лимит попыток: $reason")
                        }
                        wrong = false
                        hint = reason
                    }
                }
            } else {
                cleanupSlot()
                return@withContext Triple(false, null, "Ошибка входа: ${signError.text}")
            }
        } else {
            authorization = signed as? TLRPC.auth_Authorization
        }

        if (authorization !is TLRPC.TL_auth_authorization) {
            cleanupSlot()
            return@withContext Triple(false, null, "Аккаунт требует регистрации имени")
        }

        val user = authorization.user
        cm.setUserId(user.id)
        if (dcId != ConnectionsManager.DEFAULT_DATACENTER_ID) {
            cm.setDefaultDatacenterId(dcId)
        }

        process.log("Активация слота #$freeSlot")
        progress.setWorkingText("+${acc.phone}: настройка аккаунта…")
        val actRes = GgtgImportSupport.activateSlot(freeSlot, user, dcId)
        if (actRes.isFailure) {
            cleanupSlot()
            return@withContext Triple(false, null, "Ошибка активации: ${actRes.exceptionOrNull()?.message}")
        }

        if (acc.folder.isNotEmpty()) {
            uc.ncgFolder = acc.folder
            uc.saveConfig(false)
        }
        if (acc.twoFactor.isNotEmpty()) {
            uc.ncgTwoFactor = acc.twoFactor
            uc.saveConfig(false)
        }
        // ggtg: the trash is synced like folders
        if (acc.trashed) GgtgAccounts.setTrashed(listOf(freeSlot), true)

        return@withContext Triple(true, freeSlot, null)
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
