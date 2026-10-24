package app.ggtg

import android.content.Context
import android.widget.Toast
import org.telegram.tgnet.ConnectionsManager
import org.telegram.ui.ActionBar.BaseFragment
import org.telegram.ui.Components.GgtgProxyChooser
import org.telegram.ui.LoginActivity

// ng: adding an account by phone number. A number must never be entered from this phone's own
// address, so the proxy is asked for FIRST (a named list from "Прокси", or one typed in by hand;
// there is no "skip") and set on the account's slot before the login screen opens - the code
// request and the sign-in itself then go through it, and the account keeps it afterwards.
object GgtgAddAccount {

    /** Asks for a proxy, applies it to [slot], then runs [proceed] (the login screen). */
    @JvmStatic
    fun withProxy(context: Context, slot: Int, proceed: Runnable) {
        GgtgProxyChooser.show(context) { source ->
            val entry = source.popNext()
            if (entry == null) {
                Toast.makeText(context, "В списке не осталось прокси", Toast.LENGTH_LONG).show()
                return@show
            }
            ConnectionsManager.setProxySettingsForAccount(slot, true, entry.host, entry.port, entry.username ?: "", entry.password ?: "", "")
            // a new account gets its own random phone before its connection exists
            GgtgDevice.assignNew(slot)
            proceed.run()
        }
    }

    /** "Add account" from a menu: the proxy first, then the login screen for the free [slot]. */
    @JvmStatic
    fun start(fragment: BaseFragment, slot: Int) {
        val context = fragment.parentActivity ?: return
        withProxy(context, slot) { fragment.presentFragment(LoginActivity(slot)) }
    }
}
