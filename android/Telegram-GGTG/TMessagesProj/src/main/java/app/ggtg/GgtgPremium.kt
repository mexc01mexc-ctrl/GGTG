package app.ggtg

import android.content.Context
import android.graphics.drawable.Drawable
import android.widget.TextView
import org.telegram.messenger.AndroidUtilities
import org.telegram.messenger.R
import org.telegram.messenger.UserObject
import org.telegram.ui.ActionBar.Theme
import org.telegram.ui.Components.AnimatedEmojiDrawable

// ng: an account's name followed by Telegram's own mark - the emoji status, or the Premium star when
// there is none - exactly as the account list of Telegram shows it (see SettingsActivity.AccountCell).
// A TextView, so a long name still wraps; the mark rides at the end of the text.
class AccountNameView(context: Context) : TextView(context) {

    private val statusDrawable = AnimatedEmojiDrawable.SwapAnimatedEmojiDrawable(
        this, AndroidUtilities.dp(20f), AnimatedEmojiDrawable.CACHE_TYPE_EMOJI_STATUS)
    private var attached = false

    fun setAccount(slot: Int, name: CharSequence) {
        text = name
        val user = GgtgAccounts.user(slot)
        statusDrawable.setCurrentAccount(slot)
        statusDrawable.setColor(Theme.getColor(Theme.key_profile_verifiedBackground))
        val statusId = UserObject.getEmojiStatusDocumentId(user)
        if (statusId != null) {
            statusDrawable.set(statusId, false)
        } else if (user != null && user.premium) {
            statusDrawable.set(context.resources.getDrawable(R.drawable.msg_premium_liststar).mutate(), false)
        } else {
            statusDrawable.set(null as Drawable?, false)
        }
        if (statusDrawable.isEmpty) {
            setCompoundDrawablesRelative(null, null, null, null)
        } else {
            statusDrawable.setBounds(0, 0, AndroidUtilities.dp(20f), AndroidUtilities.dp(20f))
            setCompoundDrawablesRelative(null, null, statusDrawable, null)
            compoundDrawablePadding = AndroidUtilities.dp(6f)
        }
    }

    override fun onAttachedToWindow() {
        super.onAttachedToWindow()
        attached = true
        statusDrawable.attach()
    }

    override fun onDetachedFromWindow() {
        attached = false
        statusDrawable.detach()
        super.onDetachedFromWindow()
    }
}
