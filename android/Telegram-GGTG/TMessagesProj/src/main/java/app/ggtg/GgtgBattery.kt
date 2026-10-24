package app.ggtg

import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.Build
import android.os.PowerManager
import android.provider.Settings

// A campaign runs for hours; Xiaomi/Samsung/etc. firmware kills a background app that is still under
// battery optimization. Once, before the first long run, offer to take the app out of it.
object GgtgBattery {
    private const val PREFS = "ggtg_battery"
    private const val ASKED = "asked"

    private fun optimized(context: Context): Boolean {
        if (Build.VERSION.SDK_INT < 23) return false
        val pm = context.getSystemService(Context.POWER_SERVICE) as? PowerManager ?: return false
        return !pm.isIgnoringBatteryOptimizations(context.packageName)
    }

    /** Shows the explanation once (never again after any answer). */
    fun askOnce(context: Context) {
        val prefs = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
        if (prefs.getBoolean(ASKED, false) || !optimized(context)) return
        prefs.edit().putBoolean(ASKED, true).apply()
        org.telegram.ui.ActionBar.AlertDialog.Builder(context)
            .setTitle("Работа в фоне")
            .setMessage("Рассылка идёт часами. Чтобы телефон не останавливал GGTG в фоне, разрешите приложению работать без ограничений батареи.")
            .setPositiveButton("Разрешить") { _, _ ->
                runCatching {
                    context.startActivity(
                        Intent(Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS, Uri.parse("package:" + context.packageName))
                    )
                }
            }
            .setNegativeButton("Не сейчас", null)
            .show()
    }
}
