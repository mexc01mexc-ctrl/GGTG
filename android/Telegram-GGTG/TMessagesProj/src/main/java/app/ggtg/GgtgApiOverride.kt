package app.ggtg

import android.content.Context
import org.telegram.messenger.BuildVars
import org.telegram.messenger.LocaleController
import org.telegram.messenger.R
import org.telegram.ui.ActionBar.AlertDialog

// ng: official Telegram app_id and app_hash
object GgtgApiOverride {

    fun id(): Int = BuildVars.APP_ID
    fun hash(): String = BuildVars.APP_HASH

    fun inform(context: Context, title: String, text: String) {
        val b = AlertDialog.Builder(context).setTitle(title).setMessage(text)
        b.setPositiveButton(LocaleController.getString(R.string.OK), null)
        b.show()
    }
}
