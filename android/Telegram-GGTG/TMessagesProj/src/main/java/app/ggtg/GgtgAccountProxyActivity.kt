package app.ggtg

import android.content.Context
import android.view.View
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.TextView
import android.widget.Toast
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.telegram.messenger.AndroidUtilities
import org.telegram.messenger.NotificationCenter
import org.telegram.messenger.R
import org.telegram.messenger.UserConfig
import org.telegram.tgnet.ConnectionsManager
import org.telegram.ui.ActionBar.ActionBar.ActionBarMenuOnItemClick
import org.telegram.ui.ActionBar.BaseFragment
import org.telegram.ui.ActionBar.Theme
import org.telegram.ui.Components.GgtgProxyPoolStore
import org.telegram.utils.proxy.ProxySettings

// ggtg: the "Прокси" screen of an account, in place of Telegram's global proxy list (opened from the shield of the
// chats list, the side menu, Data settings, the sign-in screen). GGTG connects each account only through its own
// proxy, so it shows that one proxy of the open account with its live state and ping, and edits it right here:
// a field "host:port:login:password", the saved lists to take one from, "Проверить" (the exit IP through it) and
// "Сохранить". The same window as the desktop's MakeAccountProxySettingsBox (boxes/connection_box.cpp).
class GgtgAccountProxyActivity : BaseFragment(), NotificationCenter.NotificationCenterDelegate {

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main)
    private var title: TextView? = null
    private var state: TextView? = null
    private var ping: TextView? = null
    private var checkResult: TextView? = null
    private var field: android.widget.EditText? = null
    private var pingToken = 0

    override fun onFragmentCreate(): Boolean {
        NotificationCenter.getInstance(currentAccount).addObserver(this, NotificationCenter.didUpdateConnectionState)
        return super.onFragmentCreate()
    }

    override fun onFragmentDestroy() {
        NotificationCenter.getInstance(currentAccount).removeObserver(this, NotificationCenter.didUpdateConnectionState)
        scope.cancel()
        super.onFragmentDestroy()
    }

    override fun didReceivedNotification(id: Int, account: Int, vararg args: Any?) {
        if (id == NotificationCenter.didUpdateConnectionState && account == currentAccount) refreshState()
    }

    override fun createView(context: Context): View {
        actionBar.setBackButtonImage(R.drawable.ic_ab_back)
        actionBar.setAllowOverlayTitle(true)
        actionBar.setTitle("Прокси")
        actionBar.setActionBarMenuOnItemClick(object : ActionBarMenuOnItemClick() {
            override fun onItemClick(id: Int) {
                if (id == -1) finishFragment()
            }
        })
        val frame = FrameLayout(context)
        frame.setBackgroundColor(Theme.getColor(Theme.key_windowBackgroundGray))
        fragmentView = frame
        val column = GgtgUi.page(context, frame)

        // ---- the account's proxy and its state, as a row of Telegram's list
        val current = GgtgUi.card(context)
        val head = LinearLayout(context).apply { orientation = LinearLayout.HORIZONTAL; gravity = android.view.Gravity.CENTER_VERTICAL }
        head.addView(GgtgUi.badge(context, "🌐"), LinearLayout.LayoutParams(GgtgUi.dp(40f), GgtgUi.dp(40f)).apply { rightMargin = GgtgUi.dp(12f) })
        val texts = LinearLayout(context).apply { orientation = LinearLayout.VERTICAL }
        title = GgtgUi.title(context, "", 16f).also { texts.addView(it) }
        state = GgtgUi.body(context, "", 13f).also { texts.addView(it) }
        head.addView(texts, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
        ping = GgtgUi.body(context, "", 13f).also { head.addView(it) }
        current.addView(head)
        GgtgUi.addCard(column, current, top = 12f)

        // ---- edit in place
        column.addView(GgtgUi.sectionLabel(context, "Изменить"))
        val edit = GgtgUi.card(context)
        val input = GgtgUi.input(context, "хост:порт:логин:пароль", quickText())
        field = input
        edit.addView(input)
        for (name in GgtgProxyPoolStore.getListNames()) {
            if (GgtgProxyPoolStore.getCount(name) == 0) continue
            edit.addView(GgtgUi.body(context, "Взять из списка «$name»", 14f).apply {
                setTextColor(GgtgUi.accent)
                setPadding(0, GgtgUi.dp(10f), 0, GgtgUi.dp(2f))
                setOnClickListener {
                    GgtgProxyPoolStore.peekFirst(name)?.let { e ->
                        input.setText(listOf(e.host, e.port.toString(), e.username, e.password).joinToString(":"))
                        input.setSelection(input.text.length)
                    }
                }
            })
        }
        checkResult = GgtgUi.body(context, "", 13f).apply {
            visibility = View.GONE
            setPadding(0, GgtgUi.dp(10f), 0, 0)
            setTextIsSelectable(true)
        }
        edit.addView(checkResult)
        GgtgUi.addCard(column, edit)

        val buttons = LinearLayout(context).apply { orientation = LinearLayout.HORIZONTAL }
        buttons.addView(GgtgUi.button(context, "Проверить") { check() }.apply {
            background = GgtgUi.rounded(Theme.getColor(Theme.key_windowBackgroundWhite), GgtgUi.dp(12f).toFloat())
            setTextColor(GgtgUi.accent)
        }, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f).apply { rightMargin = GgtgUi.dp(8f) })
        buttons.addView(GgtgUi.button(context, "Сохранить") { save() },
            LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
        GgtgUi.addButton(column, buttons, top = 12f)
        column.addView(GgtgUi.body(context,
            "Каждый аккаунт подключается только через свой прокси. Здесь прокси открытого аккаунта: изменение сразу " +
                "переподключает его и сохраняется в GGTG.", 12f).apply { setPadding(GgtgUi.dp(16f), GgtgUi.dp(12f), GgtgUi.dp(16f), 0) })
        column.addView(GgtgUi.spacer(context, 24f))

        refreshState()
        return frame
    }

    private fun quickText(): String {
        val uc = UserConfig.getInstance(currentAccount)
        if (!uc.ncgProxyEnabled || uc.ncgProxyAddress.isNullOrEmpty()) return ""
        return listOf(uc.ncgProxyAddress, uc.ncgProxyPort.toString(), uc.ncgProxyUsername.orEmpty(), uc.ncgProxyPassword.orEmpty())
            .joinToString(":").trimEnd(':')
    }

    private fun refreshState() {
        val uc = UserConfig.getInstance(currentAccount)
        val has = uc.ncgProxyEnabled && !uc.ncgProxyAddress.isNullOrEmpty()
        val titleView = title ?: return
        val stateView = state ?: return
        if (!has) {
            titleView.text = "Прокси не задан"
            stateView.text = "без прокси аккаунт не подключается"
            stateView.setTextColor(Theme.getColor(Theme.key_text_RedRegular))
            ping?.text = ""
            return
        }
        val type = if (uc.ncgProxySecret.isNullOrEmpty()) "SOCKS5" else "MTPROTO"
        titleView.text = "$type  ${uc.ncgProxyAddress}:${uc.ncgProxyPort}"
        val connected = ConnectionsManager.getInstance(currentAccount).connectionState == ConnectionsManager.ConnectionStateConnected
        stateView.text = if (connected) "подключено" else "подключение…"
        stateView.setTextColor(if (connected) GgtgUi.accent else Theme.getColor(Theme.key_windowBackgroundWhiteGrayText))
        measurePing()
    }

    /** Telegram's own check of the proxy (the time of a round trip to a datacenter through it). */
    private fun measurePing() {
        val uc = UserConfig.getInstance(currentAccount)
        val settings = ProxySettings.builder()
            .setType(if (uc.ncgProxySecret.isNullOrEmpty()) ProxySettings.Type.SOCKS5 else ProxySettings.Type.MTPROTO)
            .setAddress(uc.ncgProxyAddress)
            .setPort(uc.ncgProxyPort)
            .setUser(uc.ncgProxyUsername)
            .setPassword(uc.ncgProxyPassword)
            .setSecret(uc.ncgProxySecret)
            .build()
        val token = ++pingToken
        ping?.text = "пинг…"
        ConnectionsManager.getInstance(currentAccount).checkProxy(settings) { time ->
            AndroidUtilities.runOnUIThread {
                if (token != pingToken) return@runOnUIThread
                ping?.text = if (time == -1L) "недоступен" else "пинг $time мс"
                ping?.setTextColor(if (time == -1L) Theme.getColor(Theme.key_text_RedRegular) else Theme.getColor(Theme.key_windowBackgroundWhiteGrayText))
            }
        }
    }

    private class Entered(val host: String, val port: Int, val user: String, val password: String)

    private fun entered(): Entered? {
        val parts = field?.text?.toString()?.trim().orEmpty().split(":", limit = 4)
        val port = parts.getOrNull(1)?.trim()?.toIntOrNull()
        if (parts.isEmpty() || parts[0].isBlank() || port == null || port !in 1..65535) {
            Toast.makeText(parentActivity, "Формат: хост:порт:логин:пароль", Toast.LENGTH_SHORT).show()
            return null
        }
        return Entered(parts[0].trim(), port, parts.getOrElse(2) { "" }.trim(), parts.getOrElse(3) { "" }.trim())
    }

    private fun check() {
        val e = entered() ?: return
        val view = checkResult ?: return
        view.visibility = View.VISIBLE
        view.text = "Проверяю…"
        view.setTextColor(Theme.getColor(Theme.key_windowBackgroundWhiteGrayText))
        scope.launch {
            val result = withContext(Dispatchers.IO) { GgtgProxyCheck.check(e.host, e.port, e.user, e.password) }
            view.text = result.text
            view.setTextColor(if (result.ok) GgtgUi.accent else Theme.getColor(Theme.key_text_RedRegular))
        }
    }

    private fun save() {
        val e = entered() ?: return
        val uc = UserConfig.getInstance(currentAccount)
        val same = uc.ncgProxyEnabled && uc.ncgProxyAddress == e.host && uc.ncgProxyPort == e.port &&
            uc.ncgProxyUsername.orEmpty() == e.user && uc.ncgProxyPassword.orEmpty() == e.password
        if (!same) {
            ConnectionsManager.setProxySettingsForAccount(currentAccount, true, e.host, e.port, e.user, e.password, "")
            GgtgDevice.autoGeoAfterProxy(currentAccount)
            Toast.makeText(parentActivity, "Прокси сохранён", Toast.LENGTH_SHORT).show()
        }
        refreshState()
    }
}
