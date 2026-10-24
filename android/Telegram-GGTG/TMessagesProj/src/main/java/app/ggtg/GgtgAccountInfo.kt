package app.ggtg

import android.content.Context
import org.telegram.ui.LaunchActivity

// ggtg: the "i" of an account in "Менеджер аккаунтов" opens a screen of its own (GgtgAccountInfoActivity),
// themed like the rest of the app, instead of a white system dialog.
object GgtgAccountInfo {
    @JvmStatic
    fun show(context: Context, slot: Int, onChanged: () -> Unit) {
        val fragment = GgtgAccountInfoActivity(slot, onChanged)
        val parent = LaunchActivity.instance?.actionBarLayout
        if (parent != null) parent.presentFragment(fragment) else LaunchActivity.instance?.presentFragment(fragment)
    }
}
