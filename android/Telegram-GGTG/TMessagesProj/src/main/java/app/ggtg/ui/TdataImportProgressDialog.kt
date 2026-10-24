package app.ggtg.ui

import android.app.AlertDialog
import android.content.Context
import android.text.InputType
import android.view.Gravity
import android.view.View
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.ProgressBar
import android.widget.TextView
import kotlinx.coroutines.suspendCancellableCoroutine
import org.telegram.messenger.AndroidUtilities
import org.telegram.messenger.LocaleController
import org.telegram.messenger.R
import kotlin.coroutines.resume

// ng: non-cancelable progress window shown for the whole duration of a tdata import (or export)
// batch - a "working" state (spinner + which account, i of n) and a "password needed" state
// (phone number, password field, Skip/Continue) that the driving coroutine (GgtgTdataImport or
// TdataExportHelper) uses. [title] is the only thing distinguishing the two call sites.
class TdataImportProgressDialog(context: Context, title: String = LocaleController.getString(R.string.NicegramImportTdata)) {

    private val dialog: AlertDialog
    private val workingLayout: LinearLayout
    private val statusText: TextView
    private val passwordLayout: LinearLayout
    private val phoneLabel: TextView
    private val errorLabel: TextView
    private val passwordInput: EditText

    private var onSubmit: ((String) -> Unit)? = null
    private var onSkip: (() -> Unit)? = null

    init {
        val root = LinearLayout(context)
        root.orientation = LinearLayout.VERTICAL
        val pad = AndroidUtilities.dp(8f)
        root.setPadding(pad, pad, pad, pad)

        workingLayout = LinearLayout(context)
        workingLayout.orientation = LinearLayout.HORIZONTAL
        workingLayout.gravity = Gravity.CENTER_VERTICAL
        val spinner = ProgressBar(context)
        workingLayout.addView(spinner, LinearLayout.LayoutParams(AndroidUtilities.dp(24f), AndroidUtilities.dp(24f)))
        statusText = TextView(context)
        statusText.setPadding(AndroidUtilities.dp(16f), 0, 0, 0)
        val statusParams = LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT)
        workingLayout.addView(statusText, statusParams)
        root.addView(workingLayout)

        passwordLayout = LinearLayout(context)
        passwordLayout.orientation = LinearLayout.VERTICAL
        passwordLayout.visibility = View.GONE

        phoneLabel = TextView(context)
        phoneLabel.setPadding(0, 0, 0, AndroidUtilities.dp(8f))
        passwordLayout.addView(phoneLabel)

        errorLabel = TextView(context)
        errorLabel.setTextColor(0xFFCF3030.toInt())
        errorLabel.text = LocaleController.getString(R.string.NicegramTdataPasswordWrong)
        errorLabel.visibility = View.GONE
        errorLabel.setPadding(0, 0, 0, AndroidUtilities.dp(8f))
        passwordLayout.addView(errorLabel)

        passwordInput = EditText(context)
        passwordInput.inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_PASSWORD
        passwordLayout.addView(passwordInput, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT))
        root.addView(passwordLayout)

        dialog = AlertDialog.Builder(context)
            .setTitle(title)
            .setView(root)
            .setCancelable(false)
            .setNegativeButton(LocaleController.getString(R.string.NicegramSkip), null)
            .setPositiveButton(LocaleController.getString(R.string.Done), null)
            .create()
        // ng: the Skip/Continue buttons only make sense in the password state - hide them while
        // just showing progress, since a dialog with no visible action would otherwise still show them.
        dialog.setOnShowListener {
            // ng: click listeners set here (not via Builder) so the buttons don't auto-dismiss the
            // dialog - the same window has to stay up for "checking password" and any re-prompt.
            dialog.getButton(AlertDialog.BUTTON_NEGATIVE)?.setOnClickListener {
                showChecking(skipping = true)
                onSkip?.invoke()
            }
            dialog.getButton(AlertDialog.BUTTON_POSITIVE)?.setOnClickListener {
                val pwd = passwordInput.text.toString()
                if (pwd.isBlank()) return@setOnClickListener
                showChecking(skipping = false)
                onSubmit?.invoke(pwd)
            }
            updateButtonVisibility()
        }
    }

    private fun updateButtonVisibility() {
        val showButtons = passwordLayout.visibility == View.VISIBLE
        dialog.getButton(AlertDialog.BUTTON_NEGATIVE)?.visibility = if (showButtons) View.VISIBLE else View.GONE
        dialog.getButton(AlertDialog.BUTTON_POSITIVE)?.visibility = if (showButtons) View.VISIBLE else View.GONE
    }

    fun show() {
        AndroidUtilities.runOnUIThread { ensureShown(); updateButtonVisibility() }
    }

    private fun ensureShown() {
        if (!dialog.isShowing) dialog.show()
    }

    /** Back to the spinner state right after Done/Skip, keeping the window on screen. */
    private fun showChecking(skipping: Boolean) {
        workingLayout.visibility = View.VISIBLE
        passwordLayout.visibility = View.GONE
        if (!skipping) statusText.text = LocaleController.getString(R.string.NicegramTdataPasswordChecking)
        updateButtonVisibility()
    }

    fun dismiss() {
        AndroidUtilities.runOnUIThread { dialog.dismiss() }
    }

    fun setWorking(phone: String?, index: Int, total: Int) {
        setWorkingText(
            LocaleController.formatString(
                "NicegramTdataImportingAccount", R.string.NicegramTdataImportingAccount,
                phone ?: "…", index, total
            )
        )
    }

    /** Same spinner state as [setWorking], with an arbitrary caption - used by the export flow. */
    fun setWorkingText(text: String) {
        AndroidUtilities.runOnUIThread {
            ensureShown()
            workingLayout.visibility = View.VISIBLE
            passwordLayout.visibility = View.GONE
            statusText.text = text
            updateButtonVisibility()
        }
    }

    private fun askPassword(phone: String?, wrongPassword: Boolean, hint: String?, onSubmit: (String) -> Unit, onSkip: () -> Unit) {
        this.onSubmit = onSubmit
        this.onSkip = onSkip
        AndroidUtilities.runOnUIThread {
            ensureShown()
            workingLayout.visibility = View.GONE
            passwordLayout.visibility = View.VISIBLE
            phoneLabel.text = LocaleController.formatString(
                "NicegramTdataPasswordNeededText", R.string.NicegramTdataPasswordNeededText, phone ?: "?"
            )
            errorLabel.text = hint ?: LocaleController.getString(R.string.NicegramTdataPasswordWrong)
            errorLabel.visibility = if (wrongPassword || hint != null) View.VISIBLE else View.GONE
            passwordInput.setText("")
            updateButtonVisibility()
        }
    }

    /** Suspends until the user submits a password or taps Skip (null). */
    suspend fun askPasswordSuspend(phone: String?, wrongPassword: Boolean, hint: String? = null): String? = suspendCancellableCoroutine { cont ->
        askPassword(
            phone, wrongPassword, hint,
            onSubmit = { pwd -> if (cont.isActive) cont.resume(pwd.takeIf { it.isNotBlank() }) },
            onSkip = { if (cont.isActive) cont.resume(null) }
        )
    }
}
