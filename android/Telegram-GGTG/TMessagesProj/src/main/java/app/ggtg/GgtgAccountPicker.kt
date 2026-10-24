package app.ggtg

import android.content.Context
import android.graphics.Color
import android.graphics.drawable.GradientDrawable
import android.view.Gravity
import android.widget.CheckBox
import android.widget.LinearLayout
import android.widget.TextView
import org.telegram.messenger.AndroidUtilities
import org.telegram.ui.ActionBar.Theme
import org.telegram.ui.Cells.HeaderCell
import org.telegram.ui.Components.LayoutHelper

// ng: reusable account picker used by Bulk, Views and the SMS screen - the logged-in accounts
// grouped by their ncgFolder tag. Like GGTG Desktop, every folder is one collapsed row (folder
// icon, name, "N аккаунт(ов)", a "вся папка" checkbox); tapping the row unfolds its accounts.
// Selection is exposed as the set of selected account slots.
class GgtgAccountPicker(context: Context) : LinearLayout(context) {

    private val rowBySlot = HashMap<Int, PickRow>()
    private val slotsByFolder = LinkedHashMap<String, MutableList<Int>>()
    private val folderMaster = HashMap<String, CheckBox>()
    private val folderSub = HashMap<String, TextView>()
    private val folderBody = HashMap<String, LinearLayout>()
    private val expanded = HashSet<String>()
    var onSelectionChanged: (() -> Unit)? = null

    /** When true, a rebuild (after an account's menu changed something) keeps the ticked accounts. */
    var keepSelection = false

    /** Ticks exactly these accounts (folders follow). */
    fun setSelected(slots: Collection<Int>) {
        rowBySlot.forEach { (slot, row) -> row.setChecked(slot in slots, false) }
        slotsByFolder.keys.forEach { syncFolder(it) }
    }

    /** An extra gray line under each account (e.g. "Сегодня отправлено: 5"); null/empty = none. */
    var todayProvider: ((Int) -> String?)? = null

    fun refreshToday() {
        rowBySlot.forEach { (slot, row) -> row.setToday(todayProvider?.invoke(slot)) }
    }

    init {
        orientation = VERTICAL
        rebuild()
    }

    fun rebuild() {
        val kept = if (keepSelection) rowBySlot.filter { it.value.isChecked }.keys.toList() else emptyList()
        removeAllViews()
        rowBySlot.clear(); slotsByFolder.clear(); folderMaster.clear(); folderSub.clear(); folderBody.clear()

        val groups = GgtgAccounts.byFolder()
        if (groups.isEmpty()) {
            addView(HeaderCell(context).apply { setText("Нет залогиненных аккаунтов") })
            return
        }
        for ((folder, refs) in groups) {
            val name = GgtgAccounts.folderTitle(folder)
            val slots = refs.map { it.slot }.toMutableList()
            slotsByFolder[folder] = slots

            val body = LinearLayout(context).apply { orientation = VERTICAL; visibility = if (folder in expanded) VISIBLE else GONE }
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
                addView(folderIcon(context, name, folder == GgtgAccounts.TRASH_FOLDER), LayoutParams(AndroidUtilities.dp(36f), AndroidUtilities.dp(36f)))
                addView(LinearLayout(context).apply {
                    orientation = VERTICAL
                    addView(TextView(context).apply {
                        text = name; textSize = 16f
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

            for (ref in refs) {
                val cell = PickRow(context, ref.slot)
                cell.setToday(todayProvider?.invoke(ref.slot))
                cell.setOnClickListener {
                    cell.setChecked(!cell.isChecked, true)
                    syncFolder(folder)
                    onSelectionChanged?.invoke()
                }
                cell.setOnLongClickListener {
                    GgtgAccountMenus.showAccountMenu(context, ref.slot, null) { rebuild(); onSelectionChanged?.invoke() }
                    true
                }
                body.addView(cell, LayoutHelper.createLinear(LayoutHelper.MATCH_PARENT, LayoutHelper.WRAP_CONTENT))
                rowBySlot[ref.slot] = cell
            }
            master.setOnClickListener {
                val newState = master.isChecked
                slots.forEach { rowBySlot[it]?.setChecked(newState, true) }
                syncFolder(folder)
                onSelectionChanged?.invoke()
            }
            header.setOnLongClickListener {
                if (folder == GgtgAccounts.TRASH_FOLDER) return@setOnLongClickListener false // not a real folder: nothing to rename/delete
                GgtgAccountMenus.showFolderMenu(context, folder) { rebuild(); onSelectionChanged?.invoke() }
                true
            }
            syncFolder(folder)
        }
        if (keepSelection && kept.isNotEmpty()) setSelected(kept)
    }

    private fun folderIcon(context: Context, name: String, trash: Boolean = false): android.view.View {
        val colors = intArrayOf(0xFFE17076.toInt(), 0xFFFAA774.toInt(), 0xFFA695E7.toInt(), 0xFF7BC862.toInt(), 0xFF6EC9CB.toInt(), 0xFF65AADD.toInt(), 0xFFEE7AAE.toInt())
        var hash = 0
        for (ch in name) hash = hash * 31 + ch.code
        if (trash) {
            // Telegram's own trash icon on a grey round chip instead of a letter
            return android.widget.FrameLayout(context).apply {
                background = GradientDrawable().apply { shape = GradientDrawable.OVAL; setColor(Theme.getColor(Theme.key_windowBackgroundWhiteGrayIcon)) }
                addView(android.widget.ImageView(context).apply {
                    setImageDrawable(GgtgUi.icon(context, org.telegram.messenger.R.drawable.msg_delete, Color.WHITE))
                }, android.widget.FrameLayout.LayoutParams(AndroidUtilities.dp(20f), AndroidUtilities.dp(20f), Gravity.CENTER))
            }
        }
        return TextView(context).apply {
            text = name.trim().take(1).uppercase()
            gravity = Gravity.CENTER
            textSize = 16f
            setTextColor(Color.WHITE)
            background = GradientDrawable().apply { shape = GradientDrawable.OVAL; setColor(colors[Math.abs(hash) % colors.size]) }
        }
    }

    private fun syncFolder(folder: String) {
        val slots = slotsByFolder[folder] ?: return
        val picked = slots.count { rowBySlot[it]?.isChecked == true }
        folderMaster[folder]?.isChecked = slots.isNotEmpty() && picked == slots.size
        folderSub[folder]?.text = if (picked > 0) "${slots.size} аккаунт(ов) · выбрано $picked" else "${slots.size} аккаунт(ов)"
    }

    fun selectedSlots(): List<Int> = rowBySlot.filter { it.value.isChecked }.keys.sorted()

    fun selectedCount(): Int = rowBySlot.count { it.value.isChecked }

    fun selectAll(checked: Boolean) {
        rowBySlot.values.forEach { it.setChecked(checked, true) }
        slotsByFolder.keys.forEach { syncFolder(it) }
        onSelectionChanged?.invoke()
    }

    /** One account row: a checkbox, the name on top and "@username · phone" under it. Both lines wrap
     *  (never "…"), so a long name or phone is always shown in full. */
    private class PickRow(context: Context, slot: Int) : LinearLayout(context) {
        private val box = GgtgUi.checkBox(context)

        val isChecked: Boolean get() = box.isChecked

        private val todayView = TextView(context).apply {
            textSize = 12f
            setTextColor(Theme.getColor(Theme.key_featuredStickers_addButton))
            visibility = GONE
        }

        fun setToday(text: String?) {
            todayView.text = text.orEmpty()
            todayView.visibility = if (text.isNullOrEmpty()) GONE else VISIBLE
        }

        fun setChecked(value: Boolean, @Suppress("UNUSED_PARAMETER") animated: Boolean) { box.isChecked = value }

        init {
            orientation = HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setBackgroundColor(Theme.getColor(Theme.key_windowBackgroundWhite))
            setPadding(AndroidUtilities.dp(16f), AndroidUtilities.dp(8f), AndroidUtilities.dp(16f), AndroidUtilities.dp(8f))
            box.isClickable = false
            box.isFocusable = false
            addView(box, LayoutParams(LayoutParams.WRAP_CONTENT, LayoutParams.WRAP_CONTENT))
            val user = GgtgAccounts.user(slot)
            val name = org.telegram.messenger.UserObject.getUserName(user).trim().ifEmpty { "Аккаунт ${slot + 1}" }
            val details = listOfNotNull(
                user?.username?.takeIf { it.isNotEmpty() }?.let { "@$it" },
                user?.phone?.takeIf { it.isNotEmpty() }?.let { "+$it" }
            ).joinToString("  ·  ")
            val texts = LinearLayout(context).apply { orientation = VERTICAL }
            texts.addView(AccountNameView(context).apply {
                setAccount(slot, name); textSize = 16f
                setTextColor(Theme.getColor(Theme.key_windowBackgroundWhiteBlackText))
            })
            if (details.isNotEmpty()) {
                texts.addView(TextView(context).apply {
                    text = details; textSize = 13f
                    setTextColor(Theme.getColor(Theme.key_windowBackgroundWhiteGrayText))
                })
            }
            texts.addView(todayView)
            addView(texts, LayoutParams(0, LayoutParams.WRAP_CONTENT, 1f).apply { leftMargin = AndroidUtilities.dp(12f) })
        }
    }
}
