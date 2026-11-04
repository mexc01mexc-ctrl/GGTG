package app.ggtg

import android.content.Context
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.text.Editable
import android.text.InputType
import android.text.TextWatcher
import android.view.Gravity
import android.view.View
import android.widget.CheckBox
import android.widget.EditText
import android.widget.FrameLayout
import android.widget.HorizontalScrollView
import android.widget.LinearLayout
import android.widget.ScrollView
import androidx.recyclerview.widget.DiffUtil
import androidx.recyclerview.widget.LinearLayoutManager
import androidx.recyclerview.widget.RecyclerView
import androidx.recyclerview.widget.SimpleItemAnimator
import android.widget.TextView
import android.widget.Toast
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.telegram.messenger.AndroidUtilities
import org.telegram.messenger.R
import org.telegram.messenger.UserConfig
import org.telegram.ui.ActionBar.ActionBar.ActionBarMenuOnItemClick
import org.telegram.ui.ActionBar.BaseFragment
import org.telegram.ui.ActionBar.Theme
import org.telegram.ui.Components.AccountFolderLogic
import org.telegram.ui.Components.AvatarDrawable
import org.telegram.ui.Components.BackupImageView
import org.telegram.ui.Components.LayoutHelper
import org.telegram.ui.LaunchActivity

// ng: "Менеджер аккаунтов" - the list of accounts, same model as GGTG Desktop's manager
// (bulk_accounts_actions.cpp). A RecyclerView of rows (DiffUtil), so only what is on screen is built.
// On top: a search, the folder chips and the status chips (all / free / trouble / no proxy).
// A card: avatar with a status dot, name, phone and @username, and a line of tags (current, status,
// folder, proxy). Tap = the menu of the account (sign in, info, profile, proxy, SpamBot, folder, trash,
// all actions); long press = the selection mode (checkboxes, a bar with "Папка / В корзину / Действия").
// In the "Все" view the folders are collapsed groups that unfold on tap.
class GgtgAccountManagerActivity : BaseFragment() {

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main)
    private var chips: LinearLayout? = null
    private var statusChips: LinearLayout? = null
    private var recycler: RecyclerView? = null
    private val listAdapter = RowAdapter()
    private var countView: TextView? = null
    private var selectionBar: View? = null
    private var searchBox: View? = null
    private var moveBtn: TextView? = null
    private var trashBtn: TextView? = null

    private var filterFolder: String? = null // null = ALL
    // «Корзина»: accounts hidden from Telegram's own list but still usable (Squidward, bulk actions).
    private var viewTrash = false
    private var query = ""
    private var statusFilter = STATUS_ALL
    // 0 as in the list, 1 trouble first, 2 free first, 3 by name, 4 by folder, 5 by messages sent today
    private var sortMode = 0
    private var selectionMode = false
    private val selected = HashSet<Int>()
    private val expanded = HashSet<String>()
    private val bulk = GgtgBulkActions(this) { rebuildAll() }
    private val statusListener: () -> Unit = { AndroidUtilities.runOnUIThread { rebuildRows() } }

    private companion object {
        const val STATUS_ALL = 0
        const val STATUS_FREE = 1
        const val STATUS_PROBLEM = 2
        const val STATUS_NOPROXY = 3
        const val ID_ADD = 1
        const val ID_SORT = 2
        const val ID_SELECT_ALL = 3
        const val ID_MORE = 10
    }

    // ---- small style helpers (one place, so every screen can feel the same) ----

    private fun dp(v: Float) = AndroidUtilities.dp(v)
    private fun color(key: Int) = Theme.getColor(key)
    private fun rounded(fill: Int, radius: Float, stroke: Int = 0, strokeColor: Int = 0) =
        GradientDrawable().apply {
            cornerRadius = radius
            setColor(fill)
            if (stroke > 0) setStroke(stroke, strokeColor)
        }

    private fun pill(context: Context, text: String, textColor: Int, fill: Int): TextView =
        TextView(context).apply {
            this.text = text
            textSize = 10.5f
            setTypeface(null, Typeface.BOLD)
            setTextColor(textColor)
            gravity = Gravity.CENTER
            setSingleLine(true)
            maxLines = 1
            setPadding(dp(7f), dp(2.5f), dp(7f), dp(2.5f))
            background = rounded(fill, dp(6f).toFloat())
        }

    override fun createView(context: Context): View {
        actionBar.setBackButtonImage(R.drawable.ic_ab_back)
        actionBar.setAllowOverlayTitle(true)
        actionBar.setTitle("Менеджер аккаунтов")
        // "+": the proxy lists and every way to add accounts; "...": sort and select all
        val menu = actionBar.createMenu()
        menu.addItem(ID_ADD, R.drawable.msg_add)
        val more = menu.addItem(ID_MORE, R.drawable.ic_ab_other)
        more.addSubItem(ID_SORT, R.drawable.msg_list, "Сортировка")
        more.addSubItem(ID_SELECT_ALL, R.drawable.msg_select, "Выбрать все")
        actionBar.setActionBarMenuOnItemClick(object : ActionBarMenuOnItemClick() {
            override fun onItemClick(id: Int) {
                when (id) {
                    -1 -> if (selectionMode) exitSelection() else finishFragment()
                    ID_ADD -> presentFragment(GgtgAddAccountsActivity())
                    ID_SORT -> showSortDialog()
                    ID_SELECT_ALL -> selectAllShown()
                }
            }
        })

        val frame = FrameLayout(context)
        frame.setBackgroundColor(color(Theme.key_windowBackgroundGray))
        fragmentView = frame
        val root = LinearLayout(context).apply { orientation = LinearLayout.VERTICAL }
        frame.addView(root, LayoutHelper.createFrame(LayoutHelper.MATCH_PARENT, LayoutHelper.MATCH_PARENT.toFloat()))

        // search, in a rounded card
        val search = LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            background = rounded(color(Theme.key_windowBackgroundWhite), dp(12f).toFloat())
            setPadding(dp(12f), 0, dp(10f), 0)
        }
        search.addView(
            android.widget.ImageView(context).apply {
                setImageDrawable(GgtgUi.icon(context, R.drawable.msg_search, color(Theme.key_windowBackgroundWhiteGrayIcon)))
            },
            LinearLayout.LayoutParams(dp(20f), dp(20f)).apply { rightMargin = dp(8f) })
        val field = EditText(context).apply {
            hint = "Поиск: имя, @username, номер, ID, папка"
            setSingleLine()
            background = null
            textSize = 14f
            setTextColor(color(Theme.key_windowBackgroundWhiteBlackText))
            setHintTextColor(color(Theme.key_windowBackgroundWhiteHintText))
            setPadding(0, dp(11f), 0, dp(11f))
            addTextChangedListener(object : TextWatcher {
                override fun beforeTextChanged(s: CharSequence?, a: Int, b: Int, c: Int) {}
                override fun onTextChanged(s: CharSequence?, a: Int, b: Int, c: Int) {}
                override fun afterTextChanged(s: Editable?) { query = s?.toString().orEmpty().trim(); rebuildAll() }
            })
        }
        search.addView(field, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
        searchBox = search
        root.addView(search, LayoutHelper.createLinear(LayoutHelper.MATCH_PARENT, LayoutHelper.WRAP_CONTENT, 10f, 8f, 10f, 0f))

        // selection bar: takes the place of the search while accounts are being picked
        val bar = LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            background = rounded(color(Theme.key_windowBackgroundWhite), dp(12f).toFloat())
            setPadding(dp(8f), dp(2f), dp(6f), dp(2f))
            visibility = View.GONE
        }
        selectionBar = bar
        val count = TextView(context).apply {
            textSize = 14f
            setTypeface(null, Typeface.BOLD)
            setTextColor(color(Theme.key_windowBackgroundWhiteBlackText))
            setPadding(dp(6f), 0, 0, 0)
        }
        countView = count
        bar.addView(count, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
        moveBtn = barButton(context, "Папка", color(Theme.key_windowBackgroundWhiteBlueText4)) { moveSelected() }
        trashBtn = barButton(context, "В корзину", color(Theme.key_text_RedRegular)) { trashSelected() }
        bar.addView(moveBtn)
        bar.addView(trashBtn)
        bar.addView(barButton(context, "Действия", color(Theme.key_featuredStickers_addButton)) { bulk.show(selectedInOrder()) })
        root.addView(bar, LayoutHelper.createLinear(LayoutHelper.MATCH_PARENT, LayoutHelper.WRAP_CONTENT, 10f, 8f, 10f, 0f))

        // folder chips and status chips
        val chipScroll = HorizontalScrollView(context).apply { isHorizontalScrollBarEnabled = false }
        val chipRow = LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL
            setPadding(dp(10f), dp(8f), dp(10f), dp(2f))
        }
        chipScroll.addView(chipRow)
        chips = chipRow
        root.addView(chipScroll, LayoutHelper.createLinear(LayoutHelper.MATCH_PARENT, LayoutHelper.WRAP_CONTENT))
        val statusScroll = HorizontalScrollView(context).apply { isHorizontalScrollBarEnabled = false }
        val statusRow = LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL
            setPadding(dp(10f), dp(4f), dp(10f), dp(4f))
        }
        statusScroll.addView(statusRow)
        statusChips = statusRow
        root.addView(statusScroll, LayoutHelper.createLinear(LayoutHelper.MATCH_PARENT, LayoutHelper.WRAP_CONTENT))

        val list = RecyclerView(context).apply {
            layoutManager = LinearLayoutManager(context)
            adapter = listAdapter
            clipToPadding = false
            overScrollMode = View.OVER_SCROLL_NEVER
            setPadding(0, dp(2f), 0, dp(20f))
            (itemAnimator as? SimpleItemAnimator)?.supportsChangeAnimations = false
            addOnScrollListener(object : RecyclerView.OnScrollListener() {
                override fun onScrollStateChanged(recyclerView: RecyclerView, newState: Int) {
                    if (newState == RecyclerView.SCROLL_STATE_DRAGGING) AndroidUtilities.hideKeyboard(field)
                }
            })
        }
        recycler = list
        root.addView(list, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f))
        rebuildAll()
        return frame
    }

    override fun onResume() { super.onResume(); rebuildAll() }

    override fun onFragmentCreate(): Boolean {
        GgtgSpamStatus.addListener(statusListener)
        GgtgProcess.addListener(statusListener)
        return super.onFragmentCreate()
    }

    override fun onFragmentDestroy() {
        GgtgSpamStatus.removeListener(statusListener)
        GgtgProcess.removeListener(statusListener)
        super.onFragmentDestroy()
    }

    override fun onActivityResultFragment(requestCode: Int, resultCode: Int, data: android.content.Intent?) {
        bulk.onActivityResult(requestCode, resultCode, data)
    }

    // The ticked accounts in the list's order (bulk steps go one by one in this order).
    private fun selectedInOrder(): List<Int> = allSlots().filter { it in selected }

    // The first back leaves the selection mode, the next one leaves the screen.
    override fun onBackPressed(invoked: Boolean): Boolean {
        if (selectionMode) {
            if (invoked) exitSelection()
            return false
        }
        return super.onBackPressed(invoked)
    }

    // ---- selection mode ----

    private fun enterSelection(slot: Int) { selectionMode = true; selected.add(slot); rebuildRows() }

    private fun exitSelection() { selectionMode = false; selected.clear(); rebuildRows() }

    private fun toggleSelected(slot: Int) {
        if (!selected.remove(slot)) selected.add(slot)
        if (selected.isEmpty()) selectionMode = false
        rebuildRows()
    }

    private fun selectAllShown() { selectionMode = true; selected.addAll(shownSlots()); rebuildRows() }

    // ---- data ----

    private fun allSlots() = GgtgAccounts.activated().map { it.slot }

    private fun folderOf(slot: Int) = UserConfig.getInstance(slot).ncgFolder ?: ""

    private fun matches(slot: Int): Boolean {
        if (query.isEmpty()) return true
        val u = GgtgAccounts.user(slot)
        val hay = listOf(u?.first_name, u?.last_name, u?.username, u?.phone, UserConfig.getInstance(slot).clientUserId.toString(), folderOf(slot))
            .joinToString(" ").lowercase()
        return query.removePrefix("@").lowercase().let { hay.contains(it) }
    }

    private fun matchesLost(entry: GgtgSessionLog.Entry): Boolean {
        if (query.isEmpty()) return true
        val needle = query.removePrefix("@").lowercase().trim()
        val hay = "${entry.folder} ${entry.name} ${entry.username} ${entry.phone} ${entry.userId}".lowercase()
        return hay.contains(needle)
    }

    private fun shownLostAccounts(): List<GgtgSessionLog.Entry> {
        if (viewTrash || statusFilter != STATUS_ALL) return emptyList()
        val all = GgtgSessionLog.currentLostAccounts()
        return all.filter { entry ->
            (filterFolder == null || entry.folder == filterFolder) && matchesLost(entry)
        }
    }

    private fun trashedOf(slot: Int) = UserConfig.getInstance(slot).ncgTrashed

    // What an account is: free, in trouble (spam block / frozen), without a proxy, or something else.
    private fun statusKind(slot: Int): Int {
        val rank = GgtgAccountStatus.of(slot).rank
        return when {
            rank == 0 -> STATUS_FREE
            rank >= 4 -> STATUS_PROBLEM
            rank == 3 -> STATUS_NOPROXY
            else -> STATUS_ALL
        }
    }

    // The accounts of the chosen folder / trash that the search finds (before the status filter).
    private fun inScope(): List<Int> = allSlots().filter {
        trashedOf(it) == viewTrash && (viewTrash || filterFolder == null || folderOf(it) == filterFolder) && matches(it)
    }

    private fun shownSlots(): List<Int> {
        var list = inScope()
        if (statusFilter != STATUS_ALL) list = list.filter { statusKind(it) == statusFilter }
        return sortSlots(list)
    }

    private fun sortSlots(list: List<Int>): List<Int> {
        if (sortMode == 0) return list
        val name = { slot: Int -> GgtgAccounts.user(slot)?.let { u -> (u.first_name.orEmpty() + " " + u.last_name.orEmpty()).trim().lowercase() }.orEmpty() }
        val rank = { slot: Int -> GgtgAccountStatus.of(slot).rank }
        return when (sortMode) {
            1 -> list.sortedWith(compareBy<Int>({ -rank(it) }, { name(it) }))
            2 -> list.sortedWith(compareBy<Int>({ rank(it) }, { name(it) }))
            3 -> list.sortedBy { name(it) }
            4 -> list.sortedWith(compareBy<Int>({ folderOf(it).lowercase() }, { name(it) }))
            5 -> list.sortedByDescending { GgtgSms.sentToday(it) }
            else -> list
        }
    }

    private fun showSortDialog() {
        val context = context ?: return
        val titles = arrayOf("Как в списке", "Проблемные сверху", "Свободные сверху", "По имени", "По папке", "По рассылке сегодня")
        val icons = IntArray(titles.size) { if (it == sortMode) R.drawable.msg_select else 0 }
        org.telegram.ui.ActionBar.AlertDialog.Builder(context)
            .setTitle("Сортировка")
            .setItems(titles, icons) { _, which -> sortMode = which; rebuildRows() }
            .show()
    }

    // ---- UI ----

    private fun rebuildAll() { rebuildChips(); rebuildRows() }

    private fun rebuildChips() {
        val row = chips ?: return
        val context = row.context
        row.removeAllViews()
        val counts = LinkedHashMap<String, Int>()
        allSlots().filter { !trashedOf(it) }.forEach { counts[folderOf(it)] = (counts[folderOf(it)] ?: 0) + 1 }
        if (!viewTrash) {
            for (lost in GgtgSessionLog.currentLostAccounts()) {
                counts[lost.folder] = (counts[lost.folder] ?: 0) + 1
            }
        }
        val trashCount = allSlots().count { trashedOf(it) }
        if (trashCount == 0) viewTrash = false
        if (filterFolder != null && filterFolder !in counts) filterFolder = null

        val accent = color(Theme.key_featuredStickers_addButton)
        val currentSlot = UserConfig.selectedAccount
        val currentFolder = if (currentSlot >= 0 && UserConfig.getInstance(currentSlot).isClientActivated) {
            folderOf(currentSlot)
        } else ""

        fun chip(
            target: LinearLayout,
            title: String,
            n: Int,
            active: Boolean,
            isCurrentFolder: Boolean = false,
            tint: Int = 0,
            onClick: () -> Unit,
            onLong: (() -> Unit)? = null,
            trashIcon: Boolean = false
        ) {
            val v = TextView(context).apply {
                text = "$title  $n"
                textSize = 13.5f
                setTypeface(null, if (active || isCurrentFolder) Typeface.BOLD else Typeface.NORMAL)
                setTextColor(when {
                    active -> 0xFFFFFFFF.toInt()
                    tint != 0 -> tint
                    isCurrentFolder -> accent
                    else -> color(Theme.key_windowBackgroundWhiteBlackText)
                })
                setPadding(dp(12f), dp(6.5f), dp(12f), dp(6.5f))
                background = rounded(
                    fill = if (active) accent else color(Theme.key_windowBackgroundWhite),
                    radius = dp(14f).toFloat(),
                    stroke = if (!active && isCurrentFolder) dp(1.5f) else 0,
                    strokeColor = accent
                )
                setOnClickListener { onClick() }
                if (onLong != null) setOnLongClickListener { onLong(); true }
                if (trashIcon) {
                    // Telegram's trash icon, in the chip's own text color
                    GgtgUi.withTrashIcon(this, if (active) 0xFFFFFFFF.toInt() else color(Theme.key_windowBackgroundWhiteBlackText), 15f)
                    compoundDrawablePadding = dp(5f)
                }
            }
            target.addView(v, LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply { rightMargin = dp(6f) })
        }
        chip(row, "Все", counts.values.sum(), !viewTrash && filterFolder == null, false, onClick = { viewTrash = false; filterFolder = null; exitSelectionSilently(); rebuildAll() })
        for ((folder, n) in counts) {
            val isCurr = (folder == currentFolder)
            chip(
                row,
                title = if (folder.isEmpty()) "Без папки" else folder,
                n = n,
                active = !viewTrash && filterFolder == folder,
                isCurrentFolder = isCurr,
                onClick = { viewTrash = false; filterFolder = folder; rebuildAll() },
                onLong = if (folder.isEmpty()) null else { { GgtgAccountMenus.showFolderMenu(context, folder) { rebuildAll() } } }
            )
        }
        if (trashCount > 0) chip(row, "Корзина", trashCount, viewTrash, false, onClick = { viewTrash = true; filterFolder = null; exitSelectionSilently(); rebuildAll() }, trashIcon = true)

        // the status is written on every account's card; no status filter chips
        statusFilter = STATUS_ALL
        statusChips?.removeAllViews()
        (statusChips?.parent as? View)?.visibility = View.GONE
    }

    // A change of the view (another folder, trash) drops the selection.
    private fun exitSelectionSilently() { selectionMode = false; selected.clear() }

    private fun rebuildRows() {
        val shown = shownSlots()
        val lost = shownLostAccounts()
        val totalShown = shown.size + lost.size
        selected.retainAll(allSlots().toSet())
        if (selected.isEmpty()) selectionMode = false
        if (!selectionMode) selected.clear()
        selectionBar?.visibility = if (selectionMode) View.VISIBLE else View.GONE
        searchBox?.visibility = if (selectionMode) View.GONE else View.VISIBLE
        actionBar.setTitle(if (selectionMode) "Выбрано: ${selected.size}" else "Менеджер аккаунтов")
        countView?.text = "Выбрано ${selected.size} из $totalShown"
        moveBtn?.visibility = if (viewTrash) View.GONE else View.VISIBLE
        trashBtn?.apply {
            text = if (viewTrash) "Вернуть" else "В корзину"
            setTextColor(if (viewTrash) color(Theme.key_windowBackgroundWhiteBlueText4) else color(Theme.key_text_RedRegular))
        }
        val hasAnyAccounts = allSlots().isNotEmpty() || GgtgSessionLog.currentLostAccounts().isNotEmpty()

        val out = ArrayList<Row>()
        if (totalShown == 0) {
            out.add(Row.Empty(when {
                !hasAnyAccounts -> "Нет добавленных аккаунтов"
                viewTrash -> "Корзина пуста"
                statusFilter != STATUS_ALL -> "Нет аккаунтов с таким статусом"
                else -> "Ничего не найдено"
            }))
            listAdapter.submit(out)
            return
        }
        // ALL view: folders are collapsed groups; a chosen folder / search / status shows the plain list
        val grouped = !viewTrash && filterFolder == null && query.isEmpty() && statusFilter == STATUS_ALL
        if (!grouped) {
            shown.forEachIndexed { i, slot -> out.add(Row.Account(slot, i + 1)) }
            if (lost.isNotEmpty()) {
                out.add(Row.LostHeader(lost.size))
                lost.forEach { entry -> out.add(Row.Lost(entry)) }
            }
            listAdapter.submit(out)
            return
        }
        var index = 0
        val byFolder = shown.groupBy { folderOf(it) }
        val lostByFolder = lost.groupBy { it.folder }
        for (slot in byFolder[""].orEmpty()) out.add(Row.Account(slot, ++index))
        for (entry in lostByFolder[""].orEmpty()) out.add(Row.Lost(entry))

        val allFolderNames = (byFolder.keys + lostByFolder.keys).filter { it.isNotEmpty() }.toSortedSet()
        for (folder in allFolderNames) {
            val slots = byFolder[folder].orEmpty()
            val lostInFolder = lostByFolder[folder].orEmpty()
            val open = folder in expanded
            out.add(Row.Folder(folder, slots, lostInFolder.size, open))
            if (open) {
                for (slot in slots) out.add(Row.Account(slot, ++index))
                for (entry in lostInFolder) out.add(Row.Lost(entry))
            }
        }
        listAdapter.submit(out)
    }

    // ---- the rows of the list ----

    private sealed class Row(val key: String) {
        class Account(val slot: Int, val number: Int) : Row("a$slot")
        class Lost(val entry: GgtgSessionLog.Entry) : Row("l${entry.userId}")
        class Folder(val name: String, val slots: List<Int>, val lostCount: Int, val open: Boolean) : Row("f$name")
        class LostHeader(val count: Int) : Row("lh")
        class Empty(val text: String) : Row("empty")
    }

    private inner class RowAdapter : RecyclerView.Adapter<RowAdapter.Holder>() {
        private var items: List<Row> = emptyList()

        inner class Holder(val box: LinearLayout) : RecyclerView.ViewHolder(box)

        fun submit(new: List<Row>) {
            val old = items
            val diff = DiffUtil.calculateDiff(object : DiffUtil.Callback() {
                override fun getOldListSize() = old.size
                override fun getNewListSize() = new.size
                override fun areItemsTheSame(o: Int, n: Int) = old[o].key == new[n].key
                // a card shows live state (status, selection, proxy), so every kept row is bound again
                override fun areContentsTheSame(o: Int, n: Int) = false
            })
            items = new
            diff.dispatchUpdatesTo(this)
        }

        override fun getItemCount() = items.size

        override fun onCreateViewHolder(parent: android.view.ViewGroup, viewType: Int): Holder =
            Holder(LinearLayout(parent.context).apply {
                orientation = LinearLayout.VERTICAL
                layoutParams = RecyclerView.LayoutParams(RecyclerView.LayoutParams.MATCH_PARENT, RecyclerView.LayoutParams.WRAP_CONTENT)
            })

        override fun onBindViewHolder(holder: Holder, position: Int) {
            val context = holder.box.context
            holder.box.removeAllViews()
            val view = when (val row = items[position]) {
                is Row.Account -> accountCard(context, row.slot, row.number)
                is Row.Lost -> lostAccountCard(context, row.entry)
                is Row.Folder -> folderHeader(context, row.name, row.slots, row.lostCount, row.open)
                is Row.LostHeader -> lostSectionHeader(context, row.count)
                is Row.Empty -> emptyView(context, row.text)
            }
            holder.box.addView(view)
        }
    }

    private fun emptyView(context: Context, text: String): View = LinearLayout(context).apply {
        orientation = LinearLayout.VERTICAL
        gravity = Gravity.CENTER_HORIZONTAL
        setPadding(dp(24f), dp(56f), dp(24f), dp(16f))
        addView(android.widget.ImageView(context).apply {
            setImageDrawable(GgtgUi.icon(context, R.drawable.msg_contacts, color(Theme.key_windowBackgroundWhiteGrayIcon)))
        }, LinearLayout.LayoutParams(dp(56f), dp(56f)))
        addView(TextView(context).apply {
            this.text = text
            textSize = 15f
            gravity = Gravity.CENTER
            setTextColor(color(Theme.key_windowBackgroundWhiteGrayText))
            setPadding(0, dp(12f), 0, 0)
        }, LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT))
    }

    private fun lostSectionHeader(context: Context, count: Int) = TextView(context).apply {
        text = "Нет в программе (сессия разорвана): $count"
        textSize = 14f
        setTypeface(null, Typeface.BOLD)
        setTextColor(color(Theme.key_text_RedRegular))
        setPadding(dp(16f), dp(16f), dp(16f), dp(6f))
    }

    private fun folderHeader(context: Context, folder: String, slots: List<Int>, lostCount: Int = 0, open: Boolean = false) = LinearLayout(context).apply {
        orientation = LinearLayout.HORIZONTAL
        gravity = Gravity.CENTER_VERTICAL
        val currentSlot = UserConfig.selectedAccount
        val hasActiveAccount = currentSlot >= 0 && slots.contains(currentSlot)
        val strokeWidth = if (hasActiveAccount) dp(1.5f) else 0
        val strokeColor = color(Theme.key_featuredStickers_addButton)

        background = rounded(
            color(Theme.key_windowBackgroundWhite),
            dp(10f).toFloat(),
            strokeWidth,
            strokeColor
        )
        setPadding(dp(10f), dp(8f), dp(10f), dp(8f))
        layoutParams = LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply {
            leftMargin = dp(10f); rightMargin = dp(10f); topMargin = dp(4f)
        }
        val picked = slots.count { it in selected }
        val box = GgtgUi.checkBox(context, slots.isNotEmpty() && picked == slots.size).apply {
            setOnClickListener {
                if (isChecked) selected.addAll(slots) else selected.removeAll(slots.toSet())
                rebuildRows()
            }
        }
        if (selectionMode) {
            addView(box, LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply {
                rightMargin = dp(4f)
            })
        }

        val iconColor = if (hasActiveAccount) color(Theme.key_featuredStickers_addButton) else color(Theme.key_windowBackgroundWhiteGrayIcon)
        addView(LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            addView(GgtgUi.withLeadingIcon(TextView(context), R.drawable.msg_folders, iconColor, 16f, 6f).apply {
                text = folder; textSize = 14.5f
                setTypeface(null, Typeface.BOLD)
                setTextColor(color(Theme.key_windowBackgroundWhiteBlackText))
            })
            val totalCount = slots.size + lostCount
            var sub = "$totalCount аккаунт(ов)" + (if (picked > 0) " · выбрано $picked" else "") + (if (lostCount > 0) " · разрывов: $lostCount" else "")
            if (hasActiveAccount) {
                sub += " · активный аккаунт"
            }
            addView(TextView(context).apply {
                text = sub
                textSize = 12f
                setTextColor(if (hasActiveAccount) color(Theme.key_featuredStickers_addButton) else color(Theme.key_windowBackgroundWhiteGrayText))
            })
        }, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))

        addView(GgtgUi.chevron(
            context, open,
            if (hasActiveAccount) color(Theme.key_featuredStickers_addButton) else color(Theme.key_windowBackgroundWhiteGrayIcon)
        ).apply { setPadding(dp(4f), 0, dp(4f), 0) })
        setOnClickListener { if (open) expanded.remove(folder) else expanded.add(folder); rebuildRows() }
        setOnLongClickListener { GgtgAccountMenus.showFolderMenu(context, folder) { rebuildAll() }; true }
    }

    private fun lostAccountCard(context: Context, entry: GgtgSessionLog.Entry): View {
        val hasBackup = GgtgSessions.hasBackup(entry.userId)
        val card = LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            background = rounded(
                color(Theme.key_windowBackgroundWhite),
                dp(10f).toFloat(),
                dp(1.5f),
                color(Theme.key_text_RedRegular)
            )
            setPadding(dp(8f), dp(8f), dp(10f), dp(8f))
            layoutParams = LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply {
                leftMargin = dp(10f); rightMargin = dp(10f); topMargin = dp(4f)
            }
        }

        val avatar = FrameLayout(context).apply {
            background = rounded(0x26FF3B30.toInt(), dp(19f).toFloat())
            val icon = TextView(context).apply {
                text = "!"
                textSize = 17f
                gravity = Gravity.CENTER
                setTypeface(null, Typeface.BOLD)
                setTextColor(color(Theme.key_text_RedRegular))
            }
            addView(icon, LayoutHelper.createFrame(LayoutHelper.MATCH_PARENT, LayoutHelper.MATCH_PARENT, Gravity.CENTER))
        }
        card.addView(avatar, LinearLayout.LayoutParams(dp(38f), dp(38f)).apply {
            gravity = Gravity.CENTER_VERTICAL; rightMargin = dp(10f); leftMargin = dp(4f)
        })

        val texts = LinearLayout(context).apply { orientation = LinearLayout.VERTICAL }
        val name = entry.name.ifEmpty { "Без имени" }

        val headerRow = LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
        }
        headerRow.addView(TextView(context).apply {
            text = name; textSize = 15f
            setTypeface(null, Typeface.BOLD)
            setTextColor(color(Theme.key_windowBackgroundWhiteBlackText))
            setSingleLine(true)
            ellipsize = android.text.TextUtils.TruncateAt.END
        }, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))

        headerRow.addView(pill(context, "Разорвана", 0xFFFFFFFF.toInt(), color(Theme.key_text_RedRegular)),
            LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply {
                leftMargin = dp(6f)
            })

        headerRow.addView(TextView(context).apply {
            text = "i"; textSize = 13.5f; gravity = Gravity.CENTER
            setTypeface(null, Typeface.BOLD)
            setTextColor(color(Theme.key_text_RedRegular))
            background = rounded(0, dp(11f).toFloat(), dp(1.5f), color(Theme.key_text_RedRegular))
            setOnClickListener { showLostAccountDialog(context, entry) }
        }, LinearLayout.LayoutParams(dp(22f), dp(22f)).apply { gravity = Gravity.CENTER_VERTICAL; leftMargin = dp(6f) })
        texts.addView(headerRow, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT))

        val phone = if (entry.phone.isNotEmpty()) (if (entry.phone.startsWith("+")) entry.phone else "+${entry.phone}") else "номер скрыт"
        val handle = if (entry.username.isNotEmpty()) "@${entry.username}" else "без юзернейма"
        val subRow = LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding(0, dp(2f), 0, 0)
        }
        subRow.addView(TextView(context).apply {
            text = "$phone  ·  $handle"
            textSize = 12.5f
            setTextColor(color(Theme.key_windowBackgroundWhiteGrayText))
        })
        texts.addView(subRow, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT))

        val reasonTitle = GgtgSessionLog.reasonTitle(entry.reason)
        val techRow = LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding(0, dp(2f), 0, 0)
        }
        techRow.addView(GgtgUi.withLeadingIcon(TextView(context), R.drawable.msg_warning, color(Theme.key_text_RedRegular), 12f, 4f).apply {
            text = reasonTitle
            textSize = 12f
            setTextColor(color(Theme.key_text_RedRegular))
            setSingleLine(true)
            ellipsize = android.text.TextUtils.TruncateAt.END
        }, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))

        if (hasBackup) {
            techRow.addView(TextView(context).apply {
                text = "Резерв ↻"
                textSize = 11.5f
                setTypeface(null, Typeface.BOLD)
                setTextColor(color(Theme.key_featuredStickers_addButton))
                setPadding(dp(6f), dp(1f), dp(6f), dp(1f))
                background = rounded(0x200088CC.toInt(), dp(5f).toFloat())
            }, LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply {
                leftMargin = dp(6f)
            })
        }
        texts.addView(techRow, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT))

        card.addView(texts, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
        card.setOnClickListener { showLostAccountDialog(context, entry) }
        return card
    }

    private fun showLostAccountDialog(context: Context, entry: GgtgSessionLog.Entry) {
        val backups = GgtgSessions.getBackups(entry.userId)
        val scroll = ScrollView(context)
        val layout = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(20f), dp(16f), dp(20f), dp(16f))
        }
        scroll.addView(layout)

        fun infoRow(title: String, value: String, isAccent: Boolean = false, isRed: Boolean = false) {
            val row = LinearLayout(context).apply {
                orientation = LinearLayout.VERTICAL
                setPadding(0, dp(4f), 0, dp(4f))
            }
            row.addView(TextView(context).apply {
                text = title
                textSize = 12f
                setTextColor(color(Theme.key_windowBackgroundWhiteGrayText))
            })
            row.addView(TextView(context).apply {
                text = value
                textSize = 15f
                setTypeface(null, if (isAccent || isRed) Typeface.BOLD else Typeface.NORMAL)
                setTextColor(when {
                    isRed -> color(Theme.key_text_RedRegular)
                    isAccent -> color(Theme.key_featuredStickers_addButton)
                    else -> color(Theme.key_windowBackgroundWhiteBlackText)
                })
            })
            layout.addView(row)
        }

        infoRow("Статус", "Сессия разорвана", isRed = true)
        val dateStr = java.text.SimpleDateFormat("dd.MM.yyyy HH:mm", java.util.Locale.getDefault()).format(java.util.Date(entry.at * 1000))
        infoRow("Когда", dateStr)
        infoRow("Что случилось", GgtgSessionLog.reasonTitle(entry.reason))
        infoRow("Код Telegram", entry.reason + (if (entry.code != 0) " (код ${entry.code})" else ""))
        infoRow("Имя", entry.name.ifEmpty { "—" })
        infoRow("Username", if (entry.username.isNotEmpty()) "@${entry.username}" else "—")
        infoRow("Телефон", if (entry.phone.isNotEmpty()) (if (entry.phone.startsWith("+")) entry.phone else "+${entry.phone}") else "—")
        infoRow("ID", entry.userId.toString())
        infoRow("Папка", entry.folder.ifEmpty { "без папки" })
        if (entry.proxy.isNotEmpty()) {
            infoRow("Прокси", entry.proxy)
        }
        infoRow("Пароль 2FA", entry.twoFactor.ifEmpty { "не был сохранён" })

        val hintText = GgtgSessionLog.reasonHint(entry.reason) +
            "\n\nЧтобы вернуть аккаунт, войдите в него заново по этому номеру или восстановите из резервной сессии."
        layout.addView(TextView(context).apply {
            text = hintText
            textSize = 13f
            setTextColor(color(Theme.key_windowBackgroundWhiteGrayText))
            setPadding(dp(12f), dp(10f), dp(12f), dp(10f))
            background = rounded(color(Theme.key_windowBackgroundGray), dp(8f).toFloat())
        }, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply {
            topMargin = dp(8f); bottomMargin = dp(12f)
        })

        var dialogRef: org.telegram.ui.ActionBar.AlertDialog? = null

        if (backups.isEmpty()) {
            layout.addView(TextView(context).apply {
                text = "Для этого аккаунта нет резервной сессии — восстановить его нельзя, только войти заново по номеру. Чтобы так не бывало, создавайте резервные сессии заранее: «Создать резервную сессию»."
                textSize = 13f
                setTextColor(color(Theme.key_windowBackgroundWhiteGrayText))
                setPadding(0, dp(4f), 0, dp(8f))
            })
        } else {
            layout.addView(TextView(context).apply {
                text = "Резервные сессии (${backups.size}):"
                textSize = 14f
                setTypeface(null, Typeface.BOLD)
                setTextColor(color(Theme.key_windowBackgroundWhiteBlackText))
                setPadding(0, dp(4f), 0, dp(6f))
            })
            for (backup in backups) {
                val bCard = LinearLayout(context).apply {
                    orientation = LinearLayout.VERTICAL
                    background = rounded(color(Theme.key_windowBackgroundGray), dp(10f).toFloat())
                    setPadding(dp(12f), dp(10f), dp(12f), dp(10f))
                }
                val bDate = java.text.SimpleDateFormat("dd.MM.yyyy HH:mm", java.util.Locale.getDefault()).format(java.util.Date(backup.createdAt * 1000))
                bCard.addView(TextView(context).apply {
                    text = "Резервная от $bDate" + (if (backup.note.isNotEmpty()) " · ${backup.note}" else "")
                    textSize = 13f
                    setTypeface(null, Typeface.BOLD)
                    setTextColor(color(Theme.key_windowBackgroundWhiteBlackText))
                })
                val restoreBtn = TextView(context).apply {
                    text = "Восстановить аккаунт из этой сессии"
                    textSize = 14f
                    setTypeface(null, Typeface.BOLD)
                    setTextColor(0xFFFFFFFF.toInt())
                    gravity = Gravity.CENTER
                    background = rounded(color(Theme.key_featuredStickers_addButton), dp(8f).toFloat())
                    setPadding(dp(12f), dp(10f), dp(12f), dp(10f))
                    setOnClickListener {
                        val progress = org.telegram.ui.ActionBar.AlertDialog(context, 3)
                        progress.setCanCancel(false)
                        progress.show()
                        scope.launch {
                            val res = withContext(Dispatchers.IO) {
                                GgtgSessions.restoreBackup(entry.userId, backup.id)
                            }
                            try { progress.dismiss() } catch (_: Throwable) { }
                            if (res.isSuccess) {
                                Toast.makeText(context, "Аккаунт успешно восстановлен!", Toast.LENGTH_LONG).show()
                                dialogRef?.dismiss()
                                rebuildAll()
                            } else {
                                val err = res.exceptionOrNull()?.message ?: "Неизвестная ошибка"
                                org.telegram.ui.ActionBar.AlertDialog.Builder(context)
                                    .setTitle("Ошибка восстановления")
                                    .setMessage("Не удалось восстановить аккаунт: $err")
                                    .setPositiveButton("OK", null)
                                    .show()
                            }
                        }
                    }
                }
                bCard.addView(restoreBtn, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply {
                    topMargin = dp(8f)
                })
                layout.addView(bCard, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply {
                    bottomMargin = dp(8f)
                })
            }
        }

        val builder = org.telegram.ui.ActionBar.AlertDialog.Builder(context)
            .setTitle("Аккаунт: сессия разорвана")
            .setView(scroll)
            .setPositiveButton("Закрыть", null)
            .setNegativeButton("Убрать из списка") { _, _ ->
                org.telegram.ui.ActionBar.AlertDialog.Builder(context)
                    .setMessage("Убрать этот аккаунт из списка? Запись о разрыве сессии будет удалена.")
                    .setPositiveButton("Убрать") { _, _ ->
                        GgtgSessionLog.removeUser(entry.userId)
                        rebuildAll()
                    }
                    .setNegativeButton("Отмена", null)
                    .show()
            }

        dialogRef = builder.create()
        dialogRef.show()
    }

    private fun tag(context: Context, text: String, tint: Int, iconRes: Int = 0, onClick: (() -> Unit)? = null): TextView =
        TextView(context).apply {
            this.text = text
            textSize = 11.5f
            setTypeface(null, Typeface.BOLD)
            setTextColor(tint)
            setSingleLine(true)
            ellipsize = android.text.TextUtils.TruncateAt.MIDDLE
            setPadding(dp(7f), dp(3f), dp(7f), dp(3f))
            background = rounded((tint and 0x00FFFFFF) or 0x1F000000, dp(7f).toFloat())
            if (iconRes != 0) GgtgUi.withLeadingIcon(this, iconRes, tint, 12f, 4f)
            if (onClick != null) setOnClickListener { onClick() }
        }

    private fun accountCard(context: Context, slot: Int, number: Int): View {
        val uc = UserConfig.getInstance(slot)
        val user = GgtgAccounts.user(slot)
        val isCurrent = slot == UserConfig.selectedAccount
        val status = GgtgAccountStatus.of(slot)
        val isSelected = slot in selected
        val accent = color(Theme.key_featuredStickers_addButton)
        val gray = color(Theme.key_windowBackgroundWhiteGrayText)
        val name = listOfNotNull(user?.first_name, user?.last_name).joinToString(" ").trim().ifEmpty { "Аккаунт $number" }

        val card = LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            background = rounded(
                if (isSelected) (accent and 0x00FFFFFF) or 0x22000000 else color(Theme.key_windowBackgroundWhite),
                dp(12f).toFloat(),
                if (isCurrent) dp(1.5f) else 0,
                accent)
            setPadding(dp(10f), dp(10f), dp(4f), dp(10f))
            layoutParams = LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply {
                leftMargin = dp(10f); rightMargin = dp(10f); topMargin = dp(5f)
            }
        }

        if (selectionMode) {
            card.addView(GgtgUi.checkBox(context, isSelected).apply {
                setOnClickListener { toggleSelected(slot) }
            }, LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply {
                gravity = Gravity.CENTER_VERTICAL
                rightMargin = dp(4f)
            })
        }

        // avatar with the status dot
        val avatarBox = FrameLayout(context)
        avatarBox.addView(BackupImageView(context).apply {
            setRoundRadius(dp(21f))
            imageReceiver.setCurrentAccount(slot)
            if (user != null) setForUserOrChat(user, AvatarDrawable().also { it.setInfo(user) })
        }, FrameLayout.LayoutParams(dp(42f), dp(42f)))
        avatarBox.addView(View(context).apply {
            background = rounded(status.color, dp(7f).toFloat(), dp(2f), color(Theme.key_windowBackgroundWhite))
        }, FrameLayout.LayoutParams(dp(14f), dp(14f), Gravity.END or Gravity.BOTTOM))
        card.addView(avatarBox, LinearLayout.LayoutParams(dp(42f), dp(42f)).apply {
            gravity = Gravity.CENTER_VERTICAL
            rightMargin = dp(12f)
        })

        val texts = LinearLayout(context).apply { orientation = LinearLayout.VERTICAL }

        // ROW 1: name ... messages sent today
        val titleRow = LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
        }
        titleRow.addView(AccountNameView(context).apply {
            setAccount(slot, name)
            textSize = 15.5f
            setTypeface(null, Typeface.BOLD)
            setTextColor(color(Theme.key_windowBackgroundWhiteBlackText))
            setSingleLine(true)
            maxLines = 1
            ellipsize = android.text.TextUtils.TruncateAt.END
        }, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
        val sentToday = GgtgSms.sentToday(slot)
        if (sentToday > 0) {
            titleRow.addView(GgtgUi.withLeadingIcon(TextView(context), R.drawable.msg_send, gray, 11.5f, 3.5f).apply {
                text = "$sentToday"
                textSize = 12f
                setTextColor(gray)
            }, LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply {
                leftMargin = dp(8f)
            })
        }
        texts.addView(titleRow, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT))

        // ROW 2: phone · @username
        val phone = user?.phone?.takeIf { it.isNotEmpty() }?.let { "+$it" } ?: "номер скрыт"
        val handle = user?.username?.takeIf { it.isNotEmpty() }?.let { "@$it" } ?: "без юзернейма"
        texts.addView(TextView(context).apply {
            text = "$phone · $handle"
            textSize = 13f
            setTextColor(gray)
            setSingleLine(true)
            ellipsize = android.text.TextUtils.TruncateAt.END
            setPadding(0, dp(2f), 0, 0)
        }, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT))

        // ROW 3: tags - current, status (when it is not "free"), folder (outside the folder groups), proxy
        val tags = LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding(0, dp(6f), 0, 0)
        }
        fun addTag(view: View, weight: Boolean = false) {
            tags.addView(view, LinearLayout.LayoutParams(if (weight) 0 else LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT, if (weight) 1f else 0f).apply {
                rightMargin = dp(5f)
            })
        }
        if (isCurrent) addTag(tag(context, "текущий", accent))
        addTag(tag(context, status.text, status.color))
        val folder = uc.ncgFolder.orEmpty()
        val folderOutsideGroup = folder.isNotEmpty() && !(filterFolder == null && query.isEmpty() && statusFilter == STATUS_ALL && !viewTrash)
        if (folderOutsideGroup) addTag(tag(context, folder, gray, R.drawable.msg_folders))
        val proxyColor = color(if (uc.ncgProxyEnabled) Theme.key_windowBackgroundWhiteBlueText4 else Theme.key_text_RedRegular)
        val proxyText = if (uc.ncgProxyEnabled) "${uc.ncgProxyAddress}:${uc.ncgProxyPort}" else "без прокси"
        addTag(tag(context, proxyText, proxyColor, if (uc.ncgProxyEnabled) R.drawable.msg_language else R.drawable.msg_warning) {
            GgtgAccountMenus.editProxy(context, slot) { rebuildAll() }
        }, weight = true)
        texts.addView(tags, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT))
        card.addView(texts, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))

        // the menu of the account
        if (!selectionMode) {
            card.addView(android.widget.ImageView(context).apply {
                scaleType = android.widget.ImageView.ScaleType.CENTER
                setImageDrawable(GgtgUi.icon(context, R.drawable.ic_ab_other, color(Theme.key_windowBackgroundWhiteGrayIcon)))
                setOnClickListener { showAccountMenu(slot) }
            }, LinearLayout.LayoutParams(dp(40f), dp(40f)).apply { gravity = Gravity.CENTER_VERTICAL })
        }

        card.setOnClickListener { if (selectionMode) toggleSelected(slot) else openAccount(slot) }
        card.setOnLongClickListener {
            if (selectionMode) toggleSelected(slot) else enterSelection(slot)
            true
        }
        return card
    }

    // A tap opens the account itself (its chat list); the menu stays on the "⋮" button.
    private fun openAccount(slot: Int) {
        if (slot == UserConfig.selectedAccount) {
            finishFragment()
        } else {
            LaunchActivity.instance?.switchToAccount(slot, true)
        }
    }

    // What can be done with one account.
    private fun showAccountMenu(slot: Int) {
        val context = context ?: return
        val user = GgtgAccounts.user(slot)
        val name = listOfNotNull(user?.first_name, user?.last_name).joinToString(" ").trim().ifEmpty { GgtgAccounts.label(slot) }
        class Item(val title: String, val icon: Int, val action: () -> Unit)
        val trashed = trashedOf(slot)
        val items = ArrayList<Item>()
        if (slot != UserConfig.selectedAccount) {
            items.add(Item("Войти в аккаунт", R.drawable.msg_openprofile) {
                LaunchActivity.instance?.switchToAccount(slot, true)
                Toast.makeText(context, "Аккаунт: $name", Toast.LENGTH_SHORT).show()
                rebuildAll()
            })
        }
        items.add(Item("Информация", R.drawable.msg_info) { GgtgAccountInfo.show(context, slot) { rebuildAll() } })
        items.add(Item("Изменить профиль", R.drawable.msg_edit) { editProfile(slot) })
        items.add(Item("Прокси", R.drawable.msg_language) { GgtgAccountMenus.editProxy(context, slot) { rebuildAll() } })
        items.add(Item("Проверка SpamBot", R.drawable.msg_warning) { showSpamBotMenu(context, slot) })
        if (!trashed) items.add(Item("Переместить в папку", R.drawable.msg_folders) { moveSelected(listOf(slot)) })
        items.add(Item(if (trashed) "Вернуть из корзины" else "В корзину", R.drawable.msg_delete) { trashSelected(listOf(slot)) })
        items.add(Item("Все действия", R.drawable.msg_settings) { bulk.show(listOf(slot)) })
        org.telegram.ui.ActionBar.AlertDialog.Builder(context)
            .setTitle(name)
            .setItems(items.map { it.title }.toTypedArray(), items.map { it.icon }.toIntArray()) { _, which -> items[which].action() }
            .show()
    }

    private fun showSpamBotMenu(context: Context, slot: Int) {
        val userId = UserConfig.getInstance(slot).clientUserId
        val isBlocked = GgtgSpamStatus.isBlocked(userId)
        val items = mutableListOf<CharSequence>("Проверить статус в @SpamBot")
        if (isBlocked) {
            items.add("Подать авто-апелляцию в @SpamBot")
        }
        items.add("Открыть чат с @SpamBot")
        org.telegram.ui.ActionBar.AlertDialog.Builder(context)
            .setTitle("SpamBot: ${GgtgAccounts.label(slot)}")
            .setItems(items.toTypedArray()) { _, which ->
                when (items[which].toString()) {
                    "Проверить статус в @SpamBot" -> {
                        scope.launch {
                            Toast.makeText(context, "Спрашиваем @SpamBot…", Toast.LENGTH_SHORT).show()
                            val error = withContext(Dispatchers.IO) { GgtgSpamStatus.check(slot) }
                            Toast.makeText(context, error?.let { "SpamBot: $it" } ?: ("SpamBot: " + GgtgSpamStatus.statusText(userId)), Toast.LENGTH_SHORT).show()
                            rebuildRows()
                        }
                    }
                    "Подать авто-апелляцию в @SpamBot" -> {
                        scope.launch {
                            Toast.makeText(context, "Отправляем апелляцию в @SpamBot…", Toast.LENGTH_SHORT).show()
                            val err = withContext(Dispatchers.IO) { GgtgSpamStatus.autoAppeal(slot) }
                            Toast.makeText(context, err ?: "Апелляция успешно отправлена в @SpamBot", Toast.LENGTH_LONG).show()
                            rebuildRows()
                        }
                    }
                    "Открыть чат с @SpamBot" -> {
                        if (slot != UserConfig.selectedAccount) {
                            LaunchActivity.instance?.switchToAccount(slot, true)
                        }
                        val bundle = android.os.Bundle().apply {
                            putLong("user_id", 178220800L)
                        }
                        presentFragment(org.telegram.ui.ChatActivity(bundle))
                    }
                }
            }
            .setNegativeButton("Отмена", null)
            .show()
    }

    private fun barButton(context: Context, title: String, textColor: Int, onClick: () -> Unit) = TextView(context).apply {
        text = title; textSize = 13.5f
        setTypeface(null, Typeface.BOLD)
        setTextColor(textColor)
        maxLines = 1
        setPadding(dp(9f), dp(7f), dp(9f), dp(7f))
        setOnClickListener { onClick() }
    }

    // ---- actions ----

    private fun moveSelected(slots: List<Int> = selected.toList()) {
        val context = context ?: return
        if (slots.isEmpty()) { Toast.makeText(context, "Отметьте хотя бы один аккаунт", Toast.LENGTH_SHORT).show(); return }
        val existing = AccountFolderLogic.existingFolderNames()
        val labels = ArrayList<CharSequence>(existing).apply { add("Новая папка…"); add("Убрать из папок") }
        org.telegram.ui.ActionBar.AlertDialog.Builder(context)
            .setTitle("Переместить (${slots.size})")
            .setItems(labels.toTypedArray()) { _, which ->
                when {
                    which < existing.size -> applyFolder(existing[which], slots)
                    which == existing.size -> askNewFolder(slots)
                    else -> applyFolder("", slots)
                }
            }.show()
    }

    // «Корзина»: the account disappears from Telegram's own account list but stays logged in, so
    // it can still be picked in Squidward / SMS / bulk actions; «Вернуть» puts it back.
    private fun trashSelected(slots: List<Int> = selected.toList()) {
        val context = context ?: return
        if (slots.isEmpty()) { Toast.makeText(context, "Отметьте хотя бы один аккаунт", Toast.LENGTH_SHORT).show(); return }
        val toTrash = slots.none { trashedOf(it) }
        val moved = slots
        val message = if (toTrash)
            "Аккаунты пропадут из списка аккаунтов Telegram, но останутся в приложении: их можно выбирать в Сквидварде, рассылке и массовых действиях. Вернуть можно во вкладке «Корзина»."
        else "Вернуть выбранные аккаунты (${moved.size}) в обычный список?"
        org.telegram.ui.ActionBar.AlertDialog.Builder(context)
            .setTitle(if (toTrash) "В корзину (${moved.size})" else "Вернуть из корзины (${moved.size})")
            .setMessage(message)
            .setPositiveButton(if (toTrash) "В корзину" else "Вернуть") { _, _ ->
                GgtgAccounts.setTrashed(moved, toTrash)
                selected.clear()
                Toast.makeText(context, (if (toTrash) "В корзине: " else "Возвращено: ") + moved.size, Toast.LENGTH_SHORT).show()
                rebuildAll()
            }
            .setNegativeButton("Отмена", null).show()
    }

    private fun askNewFolder(slots: List<Int>) {
        val context = context ?: return
        val field = EditText(context).apply {
            hint = "Имя папки"; setSingleLine()
            setPadding(dp(24f), dp(10f), dp(24f), dp(10f))
            setTextColor(color(Theme.key_dialogTextBlack))
            setHintTextColor(color(Theme.key_dialogTextHint))
            background = null
        }
        org.telegram.ui.ActionBar.AlertDialog.Builder(context).setTitle("Новая папка").setView(field)
            .setPositiveButton("Готово") { _, _ -> field.text.toString().trim().takeIf { it.isNotEmpty() }?.let { applyFolder(it, slots) } }
            .setNegativeButton("Отмена", null).show()
    }

    private fun applyFolder(folder: String, slots: List<Int>) {
        val moved = slots
        for (slot in moved) {
            val uc = UserConfig.getInstance(slot)
            uc.ncgFolder = folder
            uc.saveConfig(false)
        }
        selected.clear()
        Toast.makeText(context, "Перемещено аккаунтов: ${moved.size}", Toast.LENGTH_SHORT).show()
        rebuildAll()
    }

    private fun editProfile(slot: Int) {
        val context = context ?: return
        val user = GgtgAccounts.user(slot) ?: return
        val box = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(24f), dp(8f), dp(24f), 0)
        }
        val first = EditText(context).apply {
            hint = "Имя"; setText(user.first_name.orEmpty()); setSingleLine()
            setTextColor(color(Theme.key_dialogTextBlack))
            setHintTextColor(color(Theme.key_dialogTextHint))
        }
        val last = EditText(context).apply {
            hint = "Фамилия"; setText(user.last_name.orEmpty()); setSingleLine()
            setTextColor(color(Theme.key_dialogTextBlack))
            setHintTextColor(color(Theme.key_dialogTextHint))
        }
        val username = EditText(context).apply {
            hint = "@username"; setText(user.username.orEmpty()); setSingleLine(); inputType = InputType.TYPE_CLASS_TEXT
            setTextColor(color(Theme.key_dialogTextBlack))
            setHintTextColor(color(Theme.key_dialogTextHint))
        }
        box.addView(first); box.addView(last); box.addView(username)
        org.telegram.ui.ActionBar.AlertDialog.Builder(context).setTitle("Изменить профиль").setView(box)
            .setPositiveButton("Сохранить") { _, _ ->
                val f = first.text.toString().trim()
                if (f.isEmpty()) { Toast.makeText(context, "Введите имя", Toast.LENGTH_SHORT).show(); return@setPositiveButton }
                val l = last.text.toString().trim()
                val u = username.text.toString().trim().removePrefix("@")
                scope.launch {
                    val (ok, msg) = withContext(Dispatchers.IO) {
                        val r = GgtgBulk.updateProfile(slot, f, l, null)
                        if (!r.first) return@withContext r
                        if (u != user.username.orEmpty()) GgtgBulk.updateUsername(slot, u) else r
                    }
                    Toast.makeText(context, if (ok) "Профиль обновлён" else "Не удалось: $msg", Toast.LENGTH_LONG).show()
                    rebuildAll()
                }
            }.setNegativeButton("Отмена", null).show()
    }
}
