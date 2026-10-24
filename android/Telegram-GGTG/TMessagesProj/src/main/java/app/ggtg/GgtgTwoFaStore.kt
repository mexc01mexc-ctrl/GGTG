package app.ggtg

import android.content.Context
import org.telegram.messenger.ApplicationLoader

// ng: plaintext 2FA (cloud password) values the user typed in while exporting tdata with
// duplication (mode 2) that the server just confirmed correct - saved per Telegram userId so a
// later export of the same account never has to ask again. Local-only (SharedPreferences, never
// sent anywhere but auth.checkPassword on that same account, never shown back in any UI) - same
// on-device-only trust level as the twoFA.txt sidecar files TdataPasswordFinder reads for import.
object GgtgTwoFaStore {
    private const val PREFS = "ggtg_tdata_2fa"

    private fun prefs() = ApplicationLoader.applicationContext.getSharedPreferences(PREFS, Context.MODE_PRIVATE)

    fun get(userId: Long): String? = prefs().getString(userId.toString(), null)?.takeIf { it.isNotEmpty() }

    fun save(userId: Long, password: String) {
        prefs().edit().putString(userId.toString(), password).apply()
    }

    fun forget(userId: Long) {
        prefs().edit().remove(userId.toString()).apply()
    }
}
