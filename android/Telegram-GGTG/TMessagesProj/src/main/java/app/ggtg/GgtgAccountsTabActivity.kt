package app.ggtg

import android.content.Context
import android.view.View
import android.widget.FrameLayout
import org.telegram.messenger.AndroidUtilities
import org.telegram.messenger.NotificationCenter
import org.telegram.messenger.R
import org.telegram.messenger.UserConfig
import org.telegram.ui.ActionBar.ActionBar
import org.telegram.ui.ActionBar.BaseFragment
import org.telegram.ui.ActionBar.Theme
import org.telegram.ui.Components.AccountFolderLogic
import org.telegram.ui.Components.AccountSwitcherView
import org.telegram.ui.LaunchActivity
import org.telegram.ui.MainTabsActivity

// The "Аккаунты" page of the bottom bar: the same folder-aware list as the popup of the Profile tab (folders,
// notifications, pin, drag to reorder, long-press menus), as a page of its own, like the desktop's side panel.
class GgtgAccountsTabActivity : BaseFragment(), NotificationCenter.NotificationCenterDelegate, MainTabsActivity.TabFragmentDelegate {

    private var switcher: AccountSwitcherView? = null

    private fun accountSlots(): ArrayList<Int> {
        val list = ArrayList<Int>()
        for (a in 0 until UserConfig.MAX_ACCOUNT_COUNT) if (UserConfig.getInstance(a).isClientActivated) list.add(a)
        return list
    }

    override fun createView(context: Context): View {
        actionBar.setAllowOverlayTitle(true)
        actionBar.setTitle("Аккаунты")
        actionBar.setBackButtonImage(0)
        actionBar.createMenu().addItem(1, R.drawable.msg_add).contentDescription = "Добавить аккаунт"
        actionBar.setActionBarMenuOnItemClick(object : ActionBar.ActionBarMenuOnItemClick() {
            override fun onItemClick(id: Int) {
                if (id == 1) addAccount()
            }
        })

        val frame = FrameLayout(context)
        frame.setBackgroundColor(Theme.getColor(Theme.key_windowBackgroundWhite))
        fragmentView = frame
        // as a tab the action bar floats over the page: the list starts under it
        val top = ActionBar.getCurrentActionBarHeight() + AndroidUtilities.statusBarHeight
        val view = AccountSwitcherView(context, accountSlots(), currentAccount, { account ->
            if (account != currentAccount) LaunchActivity.instance?.switchToAccount(account, true)
        }, true)
        switcher = view
        frame.addView(view, FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT).apply { topMargin = top })
        return frame
    }

    private fun addAccount() {
        var free = 0
        var available: Int? = null
        for (a in UserConfig.MAX_ACCOUNT_COUNT - 1 downTo 0) {
            if (!UserConfig.getInstance(a).isClientActivated) {
                free++
                if (available == null) available = a
            }
        }
        if (!UserConfig.hasPremiumOnAccounts()) free -= (UserConfig.MAX_ACCOUNT_COUNT - UserConfig.MAX_ACCOUNT_DEFAULT_COUNT)
        if (free > 0 && available != null) GgtgAddAccount.start(this, available)
    }

    override fun onResume() {
        super.onResume()
        AccountFolderLogic.observeUnread(this, true)
        switcher?.refreshAccounts(accountSlots())
    }

    override fun onPause() {
        super.onPause()
        AccountFolderLogic.observeUnread(this, false)
    }

    override fun didReceivedNotification(id: Int, account: Int, vararg args: Any?) {
        if (id == NotificationCenter.dialogsUnreadCounterChanged) switcher?.refreshAccounts(accountSlots())
    }
}
