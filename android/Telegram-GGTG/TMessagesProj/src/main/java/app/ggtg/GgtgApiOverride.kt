package app.ggtg

import android.content.Context
import org.telegram.messenger.LocaleController
import org.telegram.messenger.R
import org.telegram.ui.ActionBar.AlertDialog

// ng: a login token (auth.exportLoginToken) is requested with Telegram Desktop's api_id, not this app's own:
// Telegram refuses the Android one outright (API_ID_INVALID / API_ID_PUBLISHED_FLOOD). The new session is then
// recorded as Telegram Desktop's. Regression fixed 2026-11-06 (e1a524a11 had switched this to BuildVars.APP_ID).
object GgtgApiOverride {

    fun id(): Int = DESKTOP_API_ID
    fun hash(): String = DESKTOP_API_HASH

    private const val DESKTOP_API_ID = 17349
    private const val DESKTOP_API_HASH = "344583e45741c457fe1862106095a5eb"

    fun inform(context: Context, title: String, text: String) {
        val b = AlertDialog.Builder(context).setTitle(title).setMessage(text)
        b.setPositiveButton(LocaleController.getString(R.string.OK), null)
        b.show()
    }
}
