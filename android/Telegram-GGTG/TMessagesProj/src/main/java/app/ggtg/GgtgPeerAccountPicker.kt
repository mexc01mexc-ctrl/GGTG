package app.ggtg

import android.content.Context
import android.graphics.Color
import android.graphics.drawable.GradientDrawable
import android.view.Gravity
import android.view.View
import android.widget.LinearLayout
import android.widget.TextView
import org.telegram.messenger.AndroidUtilities
import org.telegram.messenger.UserConfig
import org.telegram.ui.ActionBar.Theme
import android.widget.CheckBox
import org.telegram.ui.Cells.HeaderCell
import org.telegram.ui.Components.LayoutHelper

class GgtgPeerAccountPicker(context: Context) : LinearLayout(context) {

    private val accountsByFolder = LinkedHashMap<String, MutableList<GgtgSync.PeerAccount>>()
    private val rowByUserId = HashMap<Long, PeerRow>()
    private val folderMaster = HashMap<String, CheckBox>()
    private val folderSub = HashMap<String, TextView>()
    private val folderBody = HashMap<String, LinearLayout>()
    private val expanded = HashSet<String>()
    var onSelectionChanged: (() -> Unit)? = null

    init {
        orientation = VERTICAL
        rebuild()
    }

    fun selectedAccounts(): List<GgtgSync.PeerAccount> {
        val list = ArrayList<GgtgSync.PeerAccount>()
        accountsByFolder.values.forEach { accounts ->
            accounts.forEach { acc ->
                val row = rowByUserId[acc.userId]
                if (row != null && row.isChecked && !row.isAlreadyLoggedIn) {
                    list.add(acc)
                }
            }
        }
        return list
    }

    fun rebuild() {
        removeAllViews()
        accountsByFolder.clear()
        rowByUserId.clear()
        folderMaster.clear()
        folderSub.clear()
        folderBody.clear()

        val allAccounts = GgtgSync.getPeerAccounts()
        if (allAccounts.isEmpty()) {
            addView(HeaderCell(context).apply {
                setText("С Desktop пока не получено аккаунтов")
            })
            return
        }

        for (acc in allAccounts) {
            val folder = acc.folder.trim().ifEmpty { "Без папки" }
            accountsByFolder.getOrPut(folder) { ArrayList() }.add(acc)
        }

        for ((folder, accs) in accountsByFolder) {
            val body = LinearLayout(context).apply {
                orientation = VERTICAL
                visibility = if (folder in expanded) VISIBLE else GONE
            }
            folderBody[folder] = body

            val master = GgtgUi.checkBox(context)
            folderMaster[folder] = master
            val sub = TextView(context).apply {
                textSize = 13f
                setTextColor(Theme.getColor(Theme.key_windowBackgroundWhiteGrayText))
            }
            folderSub[folder] = sub
            val chevron = TextView(context).apply {
                text = if (folder in expanded) "▾" else "▸"
                textSize = 18f
                setTextColor(Theme.getColor(Theme.key_windowBackgroundWhiteGrayIcon))
            }

            val header = LinearLayout(context).apply {
                orientation = HORIZONTAL
                gravity = Gravity.CENTER_VERTICAL
                setBackgroundColor(Theme.getColor(Theme.key_windowBackgroundWhite))
                setPadding(AndroidUtilities.dp(16f), AndroidUtilities.dp(8f), AndroidUtilities.dp(12f), AndroidUtilities.dp(8f))
                addView(folderIcon(context, folder), LayoutParams(AndroidUtilities.dp(36f), AndroidUtilities.dp(36f)))
                addView(LinearLayout(context).apply {
                    orientation = VERTICAL
                    addView(TextView(context).apply {
                        text = folder
                        textSize = 16f
                        setTextColor(Theme.getColor(Theme.key_windowBackgroundWhiteBlackText))
                    })
                    addView(sub)
                }, LayoutParams(0, LayoutParams.WRAP_CONTENT, 1f).apply { leftMargin = AndroidUtilities.dp(12f) })
                addView(master)
                addView(chevron, LayoutParams(AndroidUtilities.dp(28f), LayoutParams.WRAP_CONTENT))
                setOnClickListener {
                    val open = body.visibility != VISIBLE
                    body.visibility = if (open) VISIBLE else GONE
                    chevron.text = if (open) "▾" else "▸"
                    if (open) expanded.add(folder) else expanded.remove(folder)
                }
            }
            addView(header, LayoutHelper.createLinear(LayoutHelper.MATCH_PARENT, LayoutHelper.WRAP_CONTENT))
            addView(body, LayoutHelper.createLinear(LayoutHelper.MATCH_PARENT, LayoutHelper.WRAP_CONTENT))

            for (acc in accs) {
                val already = (0 until UserConfig.MAX_ACCOUNT_COUNT).any {
                    val uc = UserConfig.getInstance(it)
                    uc.isClientActivated && uc.clientUserId == acc.userId
                }
                val row = PeerRow(context, acc, already)
                row.setOnClickListener {
                    if (!already) {
                        row.setChecked(!row.isChecked)
                        syncFolder(folder)
                        onSelectionChanged?.invoke()
                    }
                }
                body.addView(row, LayoutHelper.createLinear(LayoutHelper.MATCH_PARENT, LayoutHelper.WRAP_CONTENT))
                rowByUserId[acc.userId] = row
            }

            master.setOnClickListener {
                val newState = master.isChecked
                accs.forEach { acc ->
                    val r = rowByUserId[acc.userId]
                    if (r != null && !r.isAlreadyLoggedIn) {
                        r.setChecked(newState)
                    }
                }
                syncFolder(folder)
                onSelectionChanged?.invoke()
            }
            syncFolder(folder)
        }
    }

    private fun syncFolder(folder: String) {
        val accs = accountsByFolder[folder] ?: return
        val selectable = accs.filter { !(rowByUserId[it.userId]?.isAlreadyLoggedIn ?: false) }
        val checkedCount = selectable.count { rowByUserId[it.userId]?.isChecked == true }
        folderMaster[folder]?.isChecked = selectable.isNotEmpty() && checkedCount == selectable.size
        val total = accs.size
        folderSub[folder]?.text = when {
            selectable.isEmpty() -> "$total акк. (все уже на телефоне)"
            checkedCount > 0 -> "Выбрано $checkedCount из $total"
            else -> "$total акк."
        }
    }

    private fun folderIcon(context: Context, name: String): View {
        val colors = intArrayOf(
            0xFFE17076.toInt(), 0xFFFAA774.toInt(), 0xFFA695E7.toInt(),
            0xFF7BC862.toInt(), 0xFF6EC9CB.toInt(), 0xFF65AADD.toInt(), 0xFFEE7AAE.toInt()
        )
        var hash = 0
        for (ch in name) hash = hash * 31 + ch.code
        return TextView(context).apply {
            text = name.trim().take(1).uppercase()
            gravity = Gravity.CENTER
            textSize = 16f
            setTextColor(Color.WHITE)
            background = GradientDrawable().apply {
                shape = GradientDrawable.OVAL
                setColor(colors[Math.abs(hash) % colors.size])
            }
        }
    }

    private class PeerRow(
        context: Context,
        val account: GgtgSync.PeerAccount,
        val isAlreadyLoggedIn: Boolean
    ) : LinearLayout(context) {

        private val box = GgtgUi.checkBox(context)

        val isChecked: Boolean get() = box.isChecked

        fun setChecked(value: Boolean) {
            if (!isAlreadyLoggedIn) {
                box.isChecked = value
            }
        }

        init {
            orientation = HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setBackgroundColor(Theme.getColor(Theme.key_windowBackgroundWhite))
            setPadding(AndroidUtilities.dp(16f), AndroidUtilities.dp(8f), AndroidUtilities.dp(16f), AndroidUtilities.dp(8f))
            box.isClickable = false
            box.isFocusable = false
            if (isAlreadyLoggedIn) {
                box.isChecked = true
                box.alpha = 0.4f
            }
            addView(box, LayoutParams(LayoutParams.WRAP_CONTENT, LayoutParams.WRAP_CONTENT))

            val fullName = listOf(account.firstName, account.lastName).filter { it.isNotEmpty() }.joinToString(" ")
            val displayName = fullName.ifEmpty { if (account.phone.isNotEmpty()) "+${account.phone}" else "Аккаунт" }

            val detailsList = mutableListOf<String>()
            if (account.username.isNotEmpty()) detailsList.add("@${account.username}")
            if (account.phone.isNotEmpty()) detailsList.add("+${account.phone}")
            if (account.proxy != null) {
                val host = account.proxy.optString("host")
                if (host.isNotEmpty()) detailsList.add("Прокси: $host")
            }
            if (isAlreadyLoggedIn) detailsList.add("Уже на телефоне")
            val details = detailsList.joinToString("  ·  ")

            val texts = LinearLayout(context).apply { orientation = VERTICAL }
            texts.addView(TextView(context).apply {
                text = displayName
                textSize = 16f
                setTextColor(Theme.getColor(if (isAlreadyLoggedIn) Theme.key_windowBackgroundWhiteGrayText else Theme.key_windowBackgroundWhiteBlackText))
            })
            if (details.isNotEmpty()) {
                texts.addView(TextView(context).apply {
                    text = details
                    textSize = 13f
                    setTextColor(Theme.getColor(Theme.key_windowBackgroundWhiteGrayText))
                })
            }
            addView(texts, LayoutParams(0, LayoutParams.WRAP_CONTENT, 1f).apply { leftMargin = AndroidUtilities.dp(12f) })
        }
    }
}
