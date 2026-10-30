package app.ggtg

import android.content.Context
import android.view.View
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.TextView
import org.telegram.messenger.R
import org.telegram.messenger.UserConfig
import org.telegram.ui.ActionBar.ActionBar.ActionBarMenuOnItemClick
import org.telegram.ui.ActionBar.BaseFragment
import org.telegram.ui.ActionBar.Theme
import org.telegram.ui.Components.GgtgProxyPoolStore
import org.telegram.ui.LoginActivity

// ng: "Добавить аккаунты" - the proxy lists first (every new account takes its own proxy from them; without one
// it never connects), then the ways to add accounts. Opened from the account manager's "+" and from the GGTG link
// of the very first screen, before any account is logged in - so nothing here needs a logged-in account. The same
// box as the desktop's «Аккаунты» → «Добавить» (settings/ggtg_add_accounts.h).
class GgtgAddAccountsActivity : BaseFragment() {

    private var content: LinearLayout? = null
    private var proxyState: TextView? = null

    override fun createView(context: Context): View {
        actionBar.setBackButtonImage(R.drawable.ic_ab_back)
        actionBar.setAllowOverlayTitle(true)
        actionBar.setTitle("Добавить аккаунты")
        actionBar.setActionBarMenuOnItemClick(object : ActionBarMenuOnItemClick() {
            override fun onItemClick(id: Int) {
                if (id == -1) finishFragment()
            }
        })
        val frame = FrameLayout(context)
        frame.setBackgroundColor(Theme.getColor(Theme.key_windowBackgroundGray))
        fragmentView = frame
        content = GgtgUi.page(context, frame)
        build(context)
        return frame
    }

    override fun onResume() {
        super.onResume()
        // back from "Прокси": the counts may have changed
        refreshProxyState()
    }

    private fun build(context: Context) {
        val column = content ?: return
        column.removeAllViews()

        // ---- proxies ----
        column.addView(GgtgUi.sectionLabel(context, "1. Прокси"))
        val proxies = GgtgUi.card(context)
        val state = GgtgUi.body(context, "", 14f)
        proxyState = state
        proxies.addView(state)
        GgtgUi.addCard(column, proxies)
        refreshProxyState()
        GgtgUi.addCard(column, GgtgUi.actionCard(context, R.drawable.msg_language, "Списки прокси",
            "Добавьте прокси пачкой «хост:порт:логин:пароль». Каждому новому аккаунту выдаётся свой.") {
            presentFragment(GgtgProxyActivity())
        })

        // ---- accounts ----
        column.addView(GgtgUi.sectionLabel(context, "2. Аккаунты"))
        GgtgUi.addCard(column, GgtgUi.actionCard(context, R.drawable.msg_newphone, "Регистрация аккаунтов",
            "Автоматическая (по SMS) и ручная регистрация новых аккаунтов с 2FA, прокси, Push-токенами (FCM) и обходом защиты.") {
            presentFragment(GgtgRegistrationActivity())
        })
        GgtgUi.addCard(column, GgtgUi.actionCard(context, R.drawable.msg_retry, "Смена номера (миграция)",
            "Смена номера на существующих аккаунтах через SMS-сервисы с учётом отлёжки 24 часа.") {
            presentFragment(GgtgPhoneMigrationActivity())
        })
        GgtgUi.addCard(column, GgtgUi.actionCard(context, R.drawable.msg_list, "Импорт Сессия + JSON",
            "Файлы .session и .json (Telethon / Pyrogram / софт): zip-архив или папка с 2FA и прокси.") {
            presentFragment(GgtgSessionJsonImportActivity())
        })
        GgtgUi.addCard(column, GgtgUi.actionCard(context, R.drawable.msg_archive, "Импорт .ggtg",
            "Файл экспорта с GGTG Desktop: аккаунты с прокси, папками и паролями 2FA.") {
            presentFragment(GgtgImportActivity())
        })
        GgtgUi.addCard(column, GgtgUi.actionCard(context, R.drawable.msg_folders, "Импорт tdata",
            "Папка tdata с компьютера: телефон получает свою отдельную сессию.") {
            presentFragment(GgtgTdataImportActivity())
        })
        GgtgUi.addCard(column, GgtgUi.actionCard(context, R.drawable.msg_calls, "Войти по номеру телефона",
            "Сначала выбирается прокси: код никогда не запрашивается с адреса этого телефона.") {
            signIn(context)
        })
        if (GgtgSyncTransport.mainSlot() != null) {
            GgtgUi.addCard(column, GgtgUi.actionCard(context, R.drawable.msg_download, "Принять аккаунты с Desktop",
                "Через синхронизацию: новой отдельной сессией, без разрыва на Desktop.") {
                presentFragment(GgtgSyncAccountsActivity())
            })
        }
        column.addView(GgtgUi.spacer(context, 24f))
    }

    private fun signIn(context: Context) {
        val activated = UserConfig.getActivatedAccountsCount()
        if (activated == 0) {
            // the very first account: the login screen of the current (empty) slot, as the intro's own button does
            GgtgAddAccount.withProxy(context, UserConfig.selectedAccount) { presentFragment(LoginActivity()) }
            return
        }
        val free = (UserConfig.MAX_ACCOUNT_COUNT - 1 downTo 0).firstOrNull { !UserConfig.getInstance(it).isClientActivated }
        if (free == null) {
            android.widget.Toast.makeText(context, "Достигнут лимит аккаунтов", android.widget.Toast.LENGTH_LONG).show()
            return
        }
        GgtgAddAccount.start(this, free)
    }

    private fun refreshProxyState() {
        val view = proxyState ?: return
        val names = GgtgProxyPoolStore.getListNames()
        val free = names.sumOf { GgtgProxyPoolStore.getCount(it) }
        view.text = when {
            names.isEmpty() -> "Списков прокси ещё нет. Добавьте их до аккаунтов: без прокси аккаунт не подключается."
            free == 0 -> "Списков: ${names.size}, но свободных прокси не осталось. Добавьте новые."
            else -> "Списков: ${names.size}, свободных прокси: $free."
        }
    }
}
