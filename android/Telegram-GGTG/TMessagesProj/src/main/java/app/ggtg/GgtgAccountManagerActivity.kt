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

// ng: "Менеджер аккаунтов" - card-based, same model as GGTG Desktop's manager
// (bulk_accounts_actions.cpp): folder chips (ALL / each folder / "Без папки") that filter, a
// search box, a selection bar with "Действия" (bulk) and "Переместить", and one card per account
// with avatar, name, handle, a meta line, a proxy line and a status badge. Nothing is cut with
// "…": names wrap, meta wraps, the badge shrinks. Tap a card = switch to it, tap the name = edit,
// tap the proxy = its settings, tap the badge = ask SpamBot, long press = bulk actions for that
// one account. In the "ALL" view folders are collapsed groups that unfold on tap.
class GgtgAccountManagerActivity : BaseFragment() {

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main)
    private var chips: LinearLayout? = null
    private var chipScrollView: View? = null
    private var slimView: TextView? = null
    private var listScroll: ScrollView? = null
    private var chipsFolded = false
    private var chipsPinnedOpen = false
    private var chipExpandedHeight = 0
    private var foldAnimator: android.animation.ValueAnimator? = null
    private var rows: LinearLayout? = null
    private var countView: TextView? = null
    private var selectionBar: View? = null
    private var masterBox: CheckBox? = null

    private var filterFolder: String? = null // null = ALL
    // «Корзина»: accounts hidden from Telegram's own list but still usable (Squidward, bulk actions).
    private var viewTrash = false
    private var moveBtn: TextView? = null
    private var trashBtn: TextView? = null
    private var query = ""
    // 0 as is, 1 free ones first, 2 the ones in trouble first
    private var statusSort = 0
    private var sortBtn: TextView? = null
    private val selected = HashSet<Int>()
    private val expanded = HashSet<String>()
    private val bulk = GgtgBulkActions(this) { rebuildAll() }
    private val statusListener: () -> Unit = { AndroidUtilities.runOnUIThread { rebuildRows() } }

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
            textSize = 11.5f
            setTypeface(null, Typeface.BOLD)
            setTextColor(textColor)
            gravity = Gravity.CENTER
            setPadding(dp(8f), dp(3f), dp(8f), dp(3f))
            background = rounded(fill, dp(9f).toFloat())
        }

    override fun createView(context: Context): View {
        actionBar.setBackButtonImage(R.drawable.ic_ab_back)
        actionBar.setAllowOverlayTitle(true)
        actionBar.setTitle("Менеджер аккаунтов")
        // "+": the proxy lists and every way to add accounts
        actionBar.createMenu().addItem(1, R.drawable.msg_add)
        actionBar.setActionBarMenuOnItemClick(object : ActionBarMenuOnItemClick() {
            override fun onItemClick(id: Int) {
                when (id) {
                    -1 -> finishFragment()
                    1 -> presentFragment(GgtgAddAccountsActivity())
                }
            }
        })

        val frame = FrameLayout(context)
        frame.setBackgroundColor(color(Theme.key_windowBackgroundGray))
        fragmentView = frame
        // The folders, the search and the selection bar stay on top; only the account cards scroll under them, and
        // the folder chips fold into one slim line when the list is scrolled down (more room for accounts).
        val root = LinearLayout(context).apply { orientation = LinearLayout.VERTICAL }
        frame.addView(root, LayoutHelper.createFrame(LayoutHelper.MATCH_PARENT, LayoutHelper.MATCH_PARENT.toFloat()))
        val scroll = ScrollView(context)

        // folder chips
        val chipScroll = HorizontalScrollView(context).apply { isHorizontalScrollBarEnabled = false }
        val chipRow = LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL
            setPadding(dp(12f), dp(12f), dp(12f), dp(6f))
        }
        chipScroll.addView(chipRow)
        chips = chipRow
        root.addView(chipScroll, LayoutHelper.createLinear(LayoutHelper.MATCH_PARENT, LayoutHelper.WRAP_CONTENT))
        // the slim line the chips fold into; a tap unfolds them
        val slim = TextView(context).apply {
            textSize = 14f
            setTypeface(null, Typeface.BOLD)
            setTextColor(color(Theme.key_windowBackgroundWhiteBlueText4))
            setPadding(dp(16f), dp(10f), dp(16f), dp(6f))
            visibility = View.GONE
            setOnClickListener { chipsPinnedOpen = true; foldChips(false) }
        }
        slimView = slim
        chipScrollView = chipScroll
        root.addView(slim, LayoutHelper.createLinear(LayoutHelper.MATCH_PARENT, LayoutHelper.WRAP_CONTENT))

        // search, in a rounded card
        val searchCard = LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            background = rounded(color(Theme.key_windowBackgroundWhite), dp(12f).toFloat())
            setPadding(dp(14f), dp(2f), dp(14f), dp(2f))
        }
        val search = EditText(context).apply {
            hint = "Поиск: имя, @username, телефон, ID, папка"
            setSingleLine()
            background = null
            setTextColor(color(Theme.key_windowBackgroundWhiteBlackText))
            setHintTextColor(color(Theme.key_windowBackgroundWhiteHintText))
            setPadding(0, dp(10f), 0, dp(10f))
            addTextChangedListener(object : TextWatcher {
                override fun beforeTextChanged(s: CharSequence?, a: Int, b: Int, c: Int) {}
                override fun onTextChanged(s: CharSequence?, a: Int, b: Int, c: Int) {}
                override fun afterTextChanged(s: Editable?) { query = s?.toString().orEmpty().trim(); rebuildRows() }
            })
        }
        searchCard.addView(search, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
        // sort by status: as is -> free first -> trouble first
        val sort = barButton(context, sortLabel(), color(Theme.key_windowBackgroundWhiteBlueText4)) {
            statusSort = (statusSort + 1) % 3
            sortBtn?.text = sortLabel()
            rebuildRows()
        }
        sortBtn = sort
        searchCard.addView(sort)
        root.addView(searchCard, LayoutHelper.createLinear(LayoutHelper.MATCH_PARENT, LayoutHelper.WRAP_CONTENT, 12f, 0f, 12f, 0f))

        // selection bar, in a card
        val bar = LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            background = rounded(color(Theme.key_windowBackgroundWhite), dp(12f).toFloat())
            setPadding(dp(10f), dp(2f), dp(6f), dp(2f))
        }
        selectionBar = bar
        val master = GgtgUi.checkBox(context)
        masterBox = master
        master.setOnClickListener {
            val shown = shownSlots()
            if (master.isChecked) selected.addAll(shown) else selected.removeAll(shown.toSet())
            rebuildRows()
        }
        bar.addView(master)
        val count = TextView(context).apply {
            textSize = 14f
            setTextColor(color(Theme.key_windowBackgroundWhiteBlackText))
        }
        countView = count
        bar.addView(count, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
        moveBtn = barButton(context, "Папка", color(Theme.key_windowBackgroundWhiteBlueText4)) { moveSelected() }
        trashBtn = barButton(context, "В корзину", color(Theme.key_text_RedRegular)) { trashSelected() }
        bar.addView(moveBtn)
        bar.addView(trashBtn)
        bar.addView(barButton(context, "Действия", color(Theme.key_featuredStickers_addButton)) { bulk.show(selectedInOrder()) })
        root.addView(bar, LayoutHelper.createLinear(LayoutHelper.MATCH_PARENT, LayoutHelper.WRAP_CONTENT, 12f, 8f, 12f, 4f))

        val list = LinearLayout(context).apply { orientation = LinearLayout.VERTICAL }
        rows = list
        scroll.addView(list, LayoutHelper.createScroll(LayoutHelper.MATCH_PARENT, LayoutHelper.WRAP_CONTENT, Gravity.TOP))
        root.addView(scroll, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f).apply { topMargin = dp(4f); bottomMargin = dp(8f) })
        listScroll = scroll
        // Folded only when enough of the list is left that the room it gives back cannot pull the end of the
        // list under the viewport (that would scroll it back and unfold the chips again: a flicker).
        scroll.setOnScrollChangeListener { _, _, top, _, _ ->
            val room = chipExpandedHeight.coerceAtLeast(dp(48f))
            if (top <= dp(2f)) {
                chipsPinnedOpen = false
                foldChips(false)
            } else if (top > dp(70f) && !chipsPinnedOpen && list.height - scroll.height > room + dp(160f)) {
                foldChips(true)
            }
        }
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
        if (viewTrash) return emptyList()
        val all = GgtgSessionLog.currentLostAccounts()
        return all.filter { entry ->
            (filterFolder == null || entry.folder == filterFolder) && matchesLost(entry)
        }
    }

    private fun trashedOf(slot: Int) = UserConfig.getInstance(slot).ncgTrashed

    private fun shownSlots(): List<Int> {
        val list = allSlots().filter {
            trashedOf(it) == viewTrash && (viewTrash || filterFolder == null || folderOf(it) == filterFolder) && matches(it)
        }
        if (statusSort == 0) return list
        val ranked = list.associateWith { GgtgAccountStatus.of(it).rank }
        val name = { slot: Int -> GgtgAccounts.user(slot)?.let { u -> (u.first_name.orEmpty() + " " + u.last_name.orEmpty()).trim().lowercase() }.orEmpty() }
        return if (statusSort == 1) list.sortedWith(compareBy<Int>({ ranked[it] }, { name(it) }))
        else list.sortedWith(compareBy<Int>({ -(ranked[it] ?: 0) }, { name(it) }))
    }

    private fun sortLabel() = when (statusSort) { 1 -> "Статус ▲"; 2 -> "Статус ▼"; else -> "Статус" }

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
        fun chip(title: String, n: Int, active: Boolean, onClick: () -> Unit, onLong: (() -> Unit)? = null, trashIcon: Boolean = false) {
            val v = TextView(context).apply {
                text = "$title  $n"
                textSize = 14f
                setTypeface(null, if (active) Typeface.BOLD else Typeface.NORMAL)
                setTextColor(if (active) 0xFFFFFFFF.toInt() else color(Theme.key_windowBackgroundWhiteBlackText))
                setPadding(dp(14f), dp(8f), dp(14f), dp(8f))
                background = rounded(
                    if (active) color(Theme.key_featuredStickers_addButton) else color(Theme.key_windowBackgroundWhite),
                    dp(16f).toFloat())
                setOnClickListener { onClick() }
                if (onLong != null) setOnLongClickListener { onLong(); true }
                if (trashIcon) {
                    // Telegram's trash icon, in the chip's own text color
                    GgtgUi.withTrashIcon(this, if (active) 0xFFFFFFFF.toInt() else color(Theme.key_windowBackgroundWhiteBlackText), 16f)
                    compoundDrawablePadding = dp(6f)
                }
            }
            row.addView(v, LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply { rightMargin = dp(8f) })
        }
        chip("ALL", counts.values.sum(), !viewTrash && filterFolder == null, { viewTrash = false; filterFolder = null; selected.clear(); rebuildAll() })
        for ((folder, n) in counts) {
            chip(if (folder.isEmpty()) "Без папки" else folder, n, !viewTrash && filterFolder == folder, { viewTrash = false; filterFolder = folder; rebuildAll() },
                if (folder.isEmpty()) null else { { GgtgAccountMenus.showFolderMenu(context, folder) { rebuildAll() } } })
        }
        if (trashCount > 0) chip("Корзина", trashCount, viewTrash, { viewTrash = true; filterFolder = null; selected.clear(); rebuildAll() }, trashIcon = true)
    }

    // The chips and the slim line: one of them is on top, the other is hidden.
    private fun foldChips(fold: Boolean) {
        val chipsView = chipScrollView ?: return
        val slim = slimView ?: return
        if (chipsFolded == fold) return
        chipsFolded = fold
        foldAnimator?.cancel()
        if (fold) {
            if (chipsView.height > 0) chipExpandedHeight = chipsView.height
            updateSlim()
            slim.visibility = View.VISIBLE
        } else {
            slim.visibility = View.GONE
        }
        val from = chipsView.layoutParams.height.takeIf { it >= 0 } ?: chipsView.height
        val to = if (fold) 0 else chipExpandedHeight
        if (!fold && to <= 0) {
            chipsView.layoutParams = chipsView.layoutParams.also { it.height = LinearLayout.LayoutParams.WRAP_CONTENT }
            chipsView.requestLayout()
            return
        }
        foldAnimator = android.animation.ValueAnimator.ofInt(from, to).apply {
            duration = 180
            addUpdateListener {
                val value = it.animatedValue as Int
                chipsView.layoutParams = chipsView.layoutParams.also { lp -> lp.height = value }
            }
            addListener(object : android.animation.AnimatorListenerAdapter() {
                override fun onAnimationEnd(animation: android.animation.Animator) {
                    if (!fold) {
                        chipsView.layoutParams = chipsView.layoutParams.also { lp -> lp.height = LinearLayout.LayoutParams.WRAP_CONTENT }
                    }
                }
            })
            start()
        }
    }

    // "▾ Все · 200 аккаунтов", "▾ Папка Work · 12", "▾ Корзина · 3": what the folded chips stand for.
    private fun updateSlim() {
        val slim = slimView ?: return
        val where = when {
            viewTrash -> "Корзина"
            filterFolder == null -> "Все"
            filterFolder!!.isEmpty() -> "Без папки"
            else -> "Папка ${filterFolder}"
        }
        val totalShown = shownSlots().size + shownLostAccounts().size
        slim.text = "▾  $where · $totalShown  (папки)"
    }

    private fun rebuildRows() {
        val list = rows ?: return
        val context = list.context
        list.removeAllViews()
        val shown = shownSlots()
        val lost = shownLostAccounts()
        val totalShown = shown.size + lost.size
        if (chipsFolded) updateSlim()
        selected.retainAll(allSlots().toSet())
        countView?.text = if (selected.isEmpty()) "Показано: $totalShown" else "Выбрано ${selected.size} · показано $totalShown"
        moveBtn?.visibility = if (viewTrash) View.GONE else View.VISIBLE
        trashBtn?.apply {
            text = if (viewTrash) "Вернуть" else "В корзину"
            setTextColor(if (viewTrash) color(Theme.key_windowBackgroundWhiteBlueText4) else color(Theme.key_text_RedRegular))
        }
        masterBox?.isChecked = shown.isNotEmpty() && selected.containsAll(shown)
        val hasAnyAccounts = allSlots().isNotEmpty() || GgtgSessionLog.currentLostAccounts().isNotEmpty()
        selectionBar?.visibility = if (hasAnyAccounts) View.VISIBLE else View.GONE

        if (totalShown == 0) {
            list.addView(TextView(context).apply {
                text = when {
                    !hasAnyAccounts -> "Нет добавленных аккаунтов"
                    viewTrash -> "Корзина пуста"
                    else -> "Ничего не найдено"
                }
                setTextColor(color(Theme.key_windowBackgroundWhiteGrayText))
                gravity = Gravity.CENTER
                setPadding(dp(16f), dp(28f), dp(16f), dp(16f))
            }, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT))
            return
        }
        // ALL view: folders are collapsed groups; a chosen folder / search shows the plain list
        val grouped = !viewTrash && filterFolder == null && query.isEmpty()
        if (!grouped) {
            shown.forEachIndexed { i, slot -> list.addView(accountCard(context, slot, i + 1)) }
            if (lost.isNotEmpty()) {
                list.addView(lostSectionHeader(context, lost.size))
                lost.forEach { entry -> list.addView(lostAccountCard(context, entry)) }
            }
            return
        }
        var index = 0
        val byFolder = shown.groupBy { folderOf(it) }
        val lostByFolder = lost.groupBy { it.folder }
        for (slot in byFolder[""].orEmpty()) list.addView(accountCard(context, slot, ++index))
        for (entry in lostByFolder[""].orEmpty()) list.addView(lostAccountCard(context, entry))

        val allFolderNames = (byFolder.keys + lostByFolder.keys).filter { it.isNotEmpty() }.toSortedSet()
        for (folder in allFolderNames) {
            val slots = byFolder[folder].orEmpty()
            val lostInFolder = lostByFolder[folder].orEmpty()
            val open = folder in expanded
            list.addView(folderHeader(context, folder, slots, lostInFolder.size, open))
            if (open) {
                for (slot in slots) list.addView(accountCard(context, slot, ++index))
                for (entry in lostInFolder) list.addView(lostAccountCard(context, entry))
            }
        }
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
        background = rounded(color(Theme.key_windowBackgroundWhite), dp(12f).toFloat())
        setPadding(dp(12f), dp(12f), dp(12f), dp(12f))
        layoutParams = LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply {
            leftMargin = dp(12f); rightMargin = dp(12f); topMargin = dp(6f)
        }
        val picked = slots.count { it in selected }
        val box = GgtgUi.checkBox(context, slots.isNotEmpty() && picked == slots.size).apply {
            setOnClickListener {
                if (isChecked) selected.addAll(slots) else selected.removeAll(slots.toSet())
                rebuildRows()
            }
        }
        addView(box)
        addView(LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            addView(GgtgUi.withLeadingIcon(TextView(context), R.drawable.msg_folders, color(Theme.key_windowBackgroundWhiteGrayIcon), 18f, 8f).apply {
                text = folder; textSize = 16f
                setTypeface(null, Typeface.BOLD)
                setTextColor(color(Theme.key_windowBackgroundWhiteBlackText))
            })
            val totalCount = slots.size + lostCount
            val sub = "$totalCount аккаунт(ов)" + (if (picked > 0) " · выбрано $picked" else "") + (if (lostCount > 0) " · разрывов: $lostCount" else "")
            addView(TextView(context).apply {
                text = sub
                textSize = 13f
                setTextColor(color(Theme.key_windowBackgroundWhiteGrayText))
            })
        }, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
        addView(TextView(context).apply {
            text = if (open) "▾" else "▸"; textSize = 18f
            setTextColor(color(Theme.key_windowBackgroundWhiteGrayIcon))
        })
        setOnClickListener { if (open) expanded.remove(folder) else expanded.add(folder); rebuildRows() }
        setOnLongClickListener { GgtgAccountMenus.showFolderMenu(context, folder) { rebuildAll() }; true }
    }

    private fun lostAccountCard(context: Context, entry: GgtgSessionLog.Entry): View {
        val hasBackup = GgtgSessions.hasBackup(entry.userId)
        val card = LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL
            background = rounded(
                color(Theme.key_windowBackgroundWhite),
                dp(14f).toFloat(),
                dp(1.5f),
                color(Theme.key_text_RedRegular)
            )
            setPadding(dp(10f), dp(12f), dp(12f), dp(12f))
            layoutParams = LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply {
                leftMargin = dp(12f); rightMargin = dp(12f); topMargin = dp(6f)
            }
        }

        val avatar = FrameLayout(context).apply {
            background = rounded(0x26FF3B30.toInt(), dp(23f).toFloat())
            val icon = TextView(context).apply {
                text = "!"
                textSize = 20f
                gravity = Gravity.CENTER
                setTypeface(null, Typeface.BOLD)
                setTextColor(color(Theme.key_text_RedRegular))
            }
            addView(icon, LayoutHelper.createFrame(LayoutHelper.MATCH_PARENT, LayoutHelper.MATCH_PARENT, Gravity.CENTER))
        }
        card.addView(avatar, LinearLayout.LayoutParams(dp(46f), dp(46f)).apply {
            gravity = Gravity.CENTER_VERTICAL; rightMargin = dp(12f)
        })

        val texts = LinearLayout(context).apply { orientation = LinearLayout.VERTICAL }
        val name = entry.name.ifEmpty { "Без имени" }

        texts.addView(LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL
            addView(TextView(context).apply {
                text = name; textSize = 16f
                setTypeface(null, Typeface.BOLD)
                setTextColor(color(Theme.key_windowBackgroundWhiteBlackText))
            }, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
            addView(pill(context, "Сессия разорвана", 0xFFFFFFFF.toInt(), color(Theme.key_text_RedRegular)),
                LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply {
                    leftMargin = dp(6f); gravity = Gravity.CENTER_VERTICAL
                })
        }, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT))

        val handle = if (entry.username.isNotEmpty()) "@${entry.username}" else "без юзернейма"
        texts.addView(TextView(context).apply {
            text = handle; textSize = 14f
            setTextColor(if (entry.username.isEmpty()) color(Theme.key_windowBackgroundWhiteGrayText) else color(Theme.key_windowBackgroundWhiteBlueText4))
        })

        val phone = if (entry.phone.isNotEmpty()) (if (entry.phone.startsWith("+")) entry.phone else "+${entry.phone}") else "номер скрыт"
        val folder = entry.folder
        texts.addView(TextView(context).apply {
            text = "$phone  ·  ID ${entry.userId}  ·  " + if (folder.isEmpty()) "без папки" else "папка: $folder"
            textSize = 13f
            setTextColor(color(Theme.key_windowBackgroundWhiteGrayText))
            setPadding(0, dp(3f), 0, 0)
        })

        val reasonTitle = GgtgSessionLog.reasonTitle(entry.reason)
        texts.addView(GgtgUi.withLeadingIcon(TextView(context), R.drawable.msg_warning, color(Theme.key_text_RedRegular), 14f, 6f).apply {
            text = reasonTitle
            textSize = 13f
            setTextColor(color(Theme.key_text_RedRegular))
            setPadding(0, dp(2f), 0, 0)
        })

        if (hasBackup) {
            texts.addView(GgtgUi.withLeadingIcon(TextView(context), R.drawable.msg_retry, color(Theme.key_featuredStickers_addButton), 14f, 6f).apply {
                text = "Есть резервная сессия · нажмите для переключения"
                textSize = 13f
                setTypeface(null, Typeface.BOLD)
                setTextColor(color(Theme.key_featuredStickers_addButton))
                setPadding(0, dp(2f), 0, 0)
            })
        }

        card.addView(texts, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))

        card.addView(TextView(context).apply {
            text = "i"; textSize = 15f; gravity = Gravity.CENTER
            setTypeface(null, Typeface.BOLD)
            setTextColor(color(Theme.key_text_RedRegular))
            background = rounded(0, dp(13f).toFloat(), dp(1.5f), color(Theme.key_text_RedRegular))
            setOnClickListener { showLostAccountDialog(context, entry) }
        }, LinearLayout.LayoutParams(dp(26f), dp(26f)).apply { gravity = Gravity.CENTER_VERTICAL; leftMargin = dp(8f) })

        card.setOnClickListener {
            showLostAccountDialog(context, entry)
        }
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

    private fun accountCard(context: Context, slot: Int, number: Int): View {
        val uc = UserConfig.getInstance(slot)
        val user = GgtgAccounts.user(slot)
        val isCurrent = slot == UserConfig.selectedAccount

        val card = LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL
            background = rounded(
                color(Theme.key_windowBackgroundWhite),
                dp(14f).toFloat(),
                if (isCurrent) dp(1.5f) else 0,
                color(Theme.key_featuredStickers_addButton))
            setPadding(dp(10f), dp(12f), dp(12f), dp(12f))
            layoutParams = LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply {
                leftMargin = dp(12f); rightMargin = dp(12f); topMargin = dp(6f)
            }
        }

        card.addView(GgtgUi.checkBox(context, slot in selected).apply {
            setOnClickListener { if (isChecked) selected.add(slot) else selected.remove(slot); rebuildRows() }
        }, LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply { gravity = Gravity.CENTER_VERTICAL })

        val avatar = BackupImageView(context).apply {
            setRoundRadius(dp(23f))
            imageReceiver.setCurrentAccount(slot)
            if (user != null) setForUserOrChat(user, AvatarDrawable().also { it.setInfo(user) })
        }
        card.addView(avatar, LinearLayout.LayoutParams(dp(46f), dp(46f)).apply {
            gravity = Gravity.CENTER_VERTICAL; rightMargin = dp(12f)
        })

        val texts = LinearLayout(context).apply { orientation = LinearLayout.VERTICAL }
        val name = listOfNotNull(user?.first_name, user?.last_name).joinToString(" ").trim().ifEmpty { "Аккаунт $number" }

        // name + "текущий" tag; name wraps to 2 lines, never cut
        texts.addView(LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL
            addView(AccountNameView(context).apply {
                setAccount(slot, name); textSize = 16f
                setTypeface(null, Typeface.BOLD)
                setTextColor(color(Theme.key_windowBackgroundWhiteBlackText))
                setOnClickListener { editProfile(slot) }
            }, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
            if (isCurrent) addView(pill(context, "текущий", 0xFFFFFFFF.toInt(), color(Theme.key_featuredStickers_addButton)), LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply {
                leftMargin = dp(6f); gravity = Gravity.CENTER_VERTICAL
            })
        }, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT))

        val handle = user?.username?.takeIf { it.isNotEmpty() }?.let { "@$it" } ?: "без юзернейма"
        texts.addView(TextView(context).apply {
            text = handle; textSize = 14f
            setTextColor(if (user?.username.isNullOrEmpty()) color(Theme.key_windowBackgroundWhiteGrayText) else color(Theme.key_windowBackgroundWhiteBlueText4))
            setOnClickListener { editProfile(slot) }
        })

        // meta line wraps fully, nothing cut
        val phone = user?.phone?.takeIf { it.isNotEmpty() }?.let { "+$it" } ?: "номер скрыт"
        val folder = uc.ncgFolder.orEmpty()
        texts.addView(TextView(context).apply {
            text = "$phone  ·  ID ${uc.clientUserId}  ·  " + if (folder.isEmpty()) "без папки" else "папка: $folder"
            textSize = 13f
            setTextColor(color(Theme.key_windowBackgroundWhiteGrayText))
            setPadding(0, dp(3f), 0, 0)
        })

        // proxy as its own coloured line, tap = edit
        val proxyColor = color(if (uc.ncgProxyEnabled) Theme.key_windowBackgroundWhiteBlueText4 else Theme.key_text_RedRegular)
        texts.addView(GgtgUi.withLeadingIcon(TextView(context), if (uc.ncgProxyEnabled) R.drawable.msg_language else R.drawable.msg_warning, proxyColor, 14f, 6f).apply {
            text = if (uc.ncgProxyEnabled) "${uc.ncgProxyAddress}:${uc.ncgProxyPort}" else "без прокси"
            textSize = 13f
            setTextColor(proxyColor)
            setPadding(0, dp(2f), 0, 0)
            setOnClickListener { GgtgAccountMenus.editProxy(context, slot) { rebuildAll() } }
        })

        // how many SMS this account has sent in the last 24 hours (updates live while a campaign runs)
        val sentToday = GgtgSms.sentToday(slot)
        val sentColor = color(if (sentToday > 0) Theme.key_featuredStickers_addButton else Theme.key_windowBackgroundWhiteGrayText)
        texts.addView(GgtgUi.withLeadingIcon(TextView(context), R.drawable.msg_send, sentColor, 14f, 6f).apply {
            text = "$sentToday"
            textSize = 13f
            setTextColor(sentColor)
            setPadding(0, dp(2f), 0, 0)
        })

        card.addView(texts, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
        card.addView(TextView(context).apply {
            text = "i"; textSize = 15f; gravity = Gravity.CENTER
            setTypeface(null, Typeface.BOLD)
            setTextColor(color(Theme.key_featuredStickers_addButton))
            background = rounded(0, dp(13f).toFloat(), dp(1.5f), color(Theme.key_featuredStickers_addButton))
            setOnClickListener { GgtgAccountInfo.show(context, slot) { rebuildAll() } }
        }, LinearLayout.LayoutParams(dp(26f), dp(26f)).apply { gravity = Gravity.CENTER_VERTICAL; leftMargin = dp(8f) })
        card.addView(statusBadge(context, slot), LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply {
            gravity = Gravity.CENTER_VERTICAL; leftMargin = dp(8f)
        })

        card.setOnClickListener {
            if (slot != UserConfig.selectedAccount) LaunchActivity.instance?.switchToAccount(slot, true)
            Toast.makeText(context, "Аккаунт: $name", Toast.LENGTH_SHORT).show()
            rebuildAll()
        }
        // Long press: a bulk action for this one account, even when others are ticked.
        card.setOnLongClickListener { bulk.show(listOf(slot)); true }
        return card
    }

    // SpamBot verdict / "no proxy", a coloured pill that wraps to two lines and shrinks instead of
    // being cut; tap re-checks this account through @SpamBot.
    private fun statusBadge(context: Context, slot: Int): View {
        val uc = UserConfig.getInstance(slot)
        val userId = uc.clientUserId
        val status = GgtgAccountStatus.of(slot)
        val text = status.text
        val c = status.color
        return TextView(context).apply {
            this.text = text.replace("Спам-блок ", "Спам-блок\n")
            textSize = if (text.length > 12) 10.5f else 12f
            gravity = Gravity.CENTER
            setTextColor(c)
            setTypeface(null, Typeface.BOLD)
            maxWidth = dp(92f)
            setPadding(dp(9f), dp(5f), dp(9f), dp(5f))
            background = rounded((c and 0x00FFFFFF) or 0x22000000, dp(10f).toFloat())
            setOnClickListener {
                scope.launch {
                    Toast.makeText(context, "Спрашиваем @SpamBot…", Toast.LENGTH_SHORT).show()
                    val error = withContext(Dispatchers.IO) { GgtgSpamStatus.check(slot) }
                    Toast.makeText(context, error?.let { "SpamBot: $it" } ?: ("SpamBot: " + GgtgSpamStatus.statusText(userId)), Toast.LENGTH_SHORT).show()
                }
            }
        }
    }

    private fun barButton(context: Context, title: String, textColor: Int, onClick: () -> Unit) = TextView(context).apply {
        text = title; textSize = 14f
        setTypeface(null, Typeface.BOLD)
        setTextColor(textColor)
        maxLines = 1
        setPadding(dp(12f), dp(10f), dp(12f), dp(10f))
        setOnClickListener { onClick() }
    }

    // ---- actions ----

    private fun moveSelected() {
        val context = context ?: return
        if (selected.isEmpty()) { Toast.makeText(context, "Отметьте хотя бы один аккаунт", Toast.LENGTH_SHORT).show(); return }
        val existing = AccountFolderLogic.existingFolderNames()
        val labels = ArrayList<CharSequence>(existing).apply { add("Новая папка…"); add("Убрать из папок") }
        org.telegram.ui.ActionBar.AlertDialog.Builder(context)
            .setTitle("Переместить (${selected.size})")
            .setItems(labels.toTypedArray()) { _, which ->
                when {
                    which < existing.size -> applyFolder(existing[which])
                    which == existing.size -> askNewFolder()
                    else -> applyFolder("")
                }
            }.show()
    }

    // «Корзина»: the account disappears from Telegram's own account list but stays logged in, so
    // it can still be picked in Squidward / SMS / bulk actions; «Вернуть» puts it back.
    private fun trashSelected() {
        val context = context ?: return
        if (selected.isEmpty()) { Toast.makeText(context, "Отметьте хотя бы один аккаунт", Toast.LENGTH_SHORT).show(); return }
        val toTrash = !viewTrash
        val moved = selected.toList()
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

    private fun askNewFolder() {
        val context = context ?: return
        val field = EditText(context).apply {
            hint = "Имя папки"; setSingleLine()
            setPadding(dp(24f), dp(10f), dp(24f), dp(10f))
            setTextColor(color(Theme.key_dialogTextBlack))
            setHintTextColor(color(Theme.key_dialogTextHint))
            background = null
        }
        org.telegram.ui.ActionBar.AlertDialog.Builder(context).setTitle("Новая папка").setView(field)
            .setPositiveButton("Готово") { _, _ -> field.text.toString().trim().takeIf { it.isNotEmpty() }?.let { applyFolder(it) } }
            .setNegativeButton("Отмена", null).show()
    }

    private fun applyFolder(folder: String) {
        val moved = selected.toList()
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
