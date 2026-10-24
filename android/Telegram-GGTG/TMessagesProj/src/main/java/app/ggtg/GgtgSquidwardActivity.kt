package app.ggtg

import android.content.Context
import android.text.InputType
import android.view.Gravity
import android.view.View
import android.widget.EditText
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import android.widget.Toast
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.telegram.messenger.AndroidUtilities
import org.telegram.messenger.R
import org.telegram.messenger.UserConfig
import org.telegram.ui.ActionBar.ActionBar.ActionBarMenuOnItemClick
import org.telegram.ui.ActionBar.BaseFragment
import org.telegram.ui.ActionBar.Theme
import org.telegram.ui.Cells.HeaderCell
import org.telegram.ui.Cells.ShadowSectionCell
import org.telegram.ui.Cells.TextCell
import org.telegram.ui.Components.LayoutHelper

// ng: "Сквидвард" - looks a person or group up via @skvidvarde_bot from the current account
// (GgtgSquidward) and, when the card has a free Groups/People button, collects the list into a
// base. Collection runs in the background; progress in "Процессы". Same settings-screen cells as
// the rest of GGTG (HeaderCell / ShadowSectionCell, white-background fields) instead of bare
// EditText floating on the grey background.
class GgtgSquidwardActivity : BaseFragment() {

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main)
    private lateinit var query: EditText
    private lateinit var result: TextView
    private lateinit var collectCell: TextView
    private var last: GgtgSquidward.LookupResult? = null
    private var lastQuery = ""
    private var busy = false
    // The result on screen came from the saved lookups, not from the bot.
    private var fromCache = false
    private lateinit var refreshCell: TextView
    private lateinit var savedCell: TextView
    private lateinit var viaLabel: TextView

    override fun createView(context: Context): View {
        actionBar.setBackButtonImage(R.drawable.ic_ab_back)
        actionBar.setAllowOverlayTitle(true)
        actionBar.setTitle("Сквидвард")
        GgtgSquidward.screenOpen = true
        if (GgtgProcess.running(GgtgProcess.Kind.Scrape) == null) GgtgSquidward.viaSlot = -1
        actionBar.setActionBarMenuOnItemClick(object : ActionBarMenuOnItemClick() {
            override fun onItemClick(id: Int) { if (id == -1) finishFragment() }
        })

        val frame = FrameLayout(context)
        frame.setBackgroundColor(Theme.getColor(Theme.key_windowBackgroundGray))
        fragmentView = frame
        val root = GgtgUi.page(context, frame)

        // who talks to the bot
        val via = GgtgUi.card(context)
        viaLabel = GgtgUi.title(context, viaText(), 15f)
        via.addView(viaLabel)
        via.addView(GgtgUi.body(context, "Аккаунты из «Корзины» тоже можно выбрать"), LinearLayout.LayoutParams(LayoutHelper.MATCH_PARENT, LayoutHelper.WRAP_CONTENT).apply { topMargin = GgtgUi.dp(2f) })
        GgtgUi.addCard(root, via, top = 10f)
        GgtgUi.addButton(root, GgtgUi.button(context, "Сменить аккаунт…") { pickAccount() }, top = 8f)

        root.addView(GgtgUi.sectionLabel(context, "Запрос"))
        val search = GgtgUi.card(context)
        query = GgtgUi.input(context, "@username, ссылка или ID")
        GgtgUi.labeled(context, search, "Кого или что пробить", "Человек, группа или канал", query)
        GgtgUi.addCard(root, search)
        GgtgUi.addButton(root, GgtgUi.button(context, "Найти") { lookup() }, top = 8f)

        root.addView(GgtgUi.sectionLabel(context, "Результат"))
        val resultCard = GgtgUi.card(context)
        result = TextView(context).apply {
            setTextColor(Theme.getColor(Theme.key_windowBackgroundWhiteBlackText))
            setTextIsSelectable(true)
            textSize = 15f
            text = "—"
        }
        resultCard.addView(result)
        GgtgUi.addCard(root, resultCard)

        savedCell = actionButton(root, "Сохранить в базу") { saveFromCache() }
        collectCell = actionButton(root, "Собрать в базу") { collect() }
        refreshCell = actionButton(root, "Пробить заново") { lookup(force = true) }
        GgtgUi.addButton(root, GgtgUi.button(context, "Остановить", destructive = true) { GgtgSquidward.cancelled = true; toast("Остановлено") }, top = 8f)
        root.addView(GgtgUi.spacer(context))
        return frame
    }

    // The result actions are TextViews (hidden until a result calls for them) styled as buttons.
    private fun actionButton(root: LinearLayout, title: String, onClick: () -> Unit): TextView {
        val b = GgtgUi.button(root.context, title, onClick = onClick)
        b.visibility = View.GONE
        GgtgUi.addButton(root, b, top = 8f)
        return b
    }

    private fun viaText() = "Бот @skvidvarde_bot · через " + GgtgSquidward.viaLabel()

    // Every logged-in account, trashed ones included: the «Корзина» hides an account from Telegram's
    // own list, so this is where it is picked for a lookup. Searchable, grouped: «Сейчас», one section
    // per folder, then the «Корзина» (those connect only once chosen).
    private fun pickAccount() {
        val context = context ?: return
        val refs = GgtgAccounts.activated()
        if (refs.isEmpty()) { toast("Нет аккаунтов"); return }
        val current = GgtgSquidward.currentSlot()

        val root = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundColor(Theme.getColor(Theme.key_windowBackgroundGray))
            setPadding(GgtgUi.dp(12f), GgtgUi.dp(10f), GgtgUi.dp(12f), GgtgUi.dp(6f))
        }
        val search = GgtgUi.input(context, "Имя, @username или телефон")
        root.addView(search)
        val list = LinearLayout(context).apply { orientation = LinearLayout.VERTICAL }
        val scroll = ScrollView(context).apply { addView(list) }
        root.addView(scroll, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, GgtgUi.dp(380f)).apply { topMargin = GgtgUi.dp(8f) })
        val dialog = org.telegram.ui.ActionBar.AlertDialog.Builder(context)
            .setTitle("Через какой аккаунт пробивать")
            .setView(root)
            .setNegativeButton("Закрыть", null)
            .create()

        fun choose(slot: Int) {
            dialog.dismiss()
            GgtgSquidward.viaSlot = slot
            GgtgAccounts.wake(listOf(slot)) // a trashed account connects only now
            viaLabel.text = viaText()
        }

        // Folders and the trash start folded; a tap on a header unfolds / folds it, a search unfolds
        // everything it found.
        val open = HashSet<String>()
        var rebuildList: () -> Unit = {}

        fun addSection(title: String, slots: List<Int>, foldable: Boolean = true, trashIcon: Boolean = false) {
            if (slots.isEmpty()) return
            val searching = search.text.toString().isNotBlank()
            val unfolded = !foldable || searching || title in open
            if (foldable) {
                val header = GgtgUi.card(context) { if (!open.remove(title)) open.add(title); rebuildList() }
                val headerTitle = GgtgUi.title(context, (if (unfolded) "▾  " else "▸  ") + "$title · ${slots.size}", 15f)
                if (trashIcon) GgtgUi.withTrashIcon(headerTitle, Theme.getColor(Theme.key_windowBackgroundWhiteBlackText), 16f)
                header.addView(headerTitle)
                list.addView(header, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply { topMargin = GgtgUi.dp(8f) })
            } else {
                list.addView(GgtgUi.sectionLabel(context, "$title · ${slots.size}").apply { setPadding(GgtgUi.dp(6f), GgtgUi.dp(12f), GgtgUi.dp(6f), GgtgUi.dp(2f)) })
            }
            if (!unfolded) return
            for (slot in slots) {
                val uc = UserConfig.getInstance(slot)
                val trashed = uc.ncgTrashed
                val card = GgtgUi.card(context) { choose(slot) }
                card.addView(AccountNameView(context).apply {
                    setAccount(slot, GgtgAccounts.label(slot)); textSize = 15f
                    setTypeface(null, android.graphics.Typeface.BOLD)
                    setTextColor(Theme.getColor(Theme.key_windowBackgroundWhiteBlackText))
                })
                val notes = ArrayList<String>()
                notes += if (trashed) "Корзина" else (uc.ncgFolder?.takeIf { it.isNotEmpty() } ?: "Без папки")
                if (org.telegram.tgnet.ConnectionsManager.getInstance(slot).isAsleep) notes += "не подключён"
                if (slot == current) notes += "✓ сейчас"
                card.addView(GgtgUi.body(context, notes.joinToString(" · ")))
                list.addView(card, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply { topMargin = GgtgUi.dp(6f) })
            }
        }

        fun rebuild() {
            list.removeAllViews()
            val q = search.text.toString().trim().removePrefix("@").lowercase()
            val shown = refs.map { it.slot }.filter { slot ->
                val uc = UserConfig.getInstance(slot)
                q.isEmpty() || (GgtgAccounts.label(slot) + " " + (uc.ncgFolder ?: "")).lowercase().contains(q)
            }
            addSection("Сейчас", shown.filter { it == current }, foldable = false)
            val rest = shown.filter { it != current && !UserConfig.getInstance(it).ncgTrashed }
            val folders = rest.groupBy { UserConfig.getInstance(it).ncgFolder ?: "" }
            for ((folder, slots) in folders.entries.sortedBy { it.key.isEmpty() }) addSection(folder.ifEmpty { "Без папки" }, slots)
            addSection("Корзина (подключатся при выборе)", shown.filter { it != current && UserConfig.getInstance(it).ncgTrashed }, trashIcon = true)
            if (list.childCount == 0) list.addView(GgtgUi.body(context, "Ничего не найдено", 14f).apply { setPadding(GgtgUi.dp(6f), GgtgUi.dp(16f), GgtgUi.dp(6f), 0) })
        }
        search.addTextChangedListener(object : android.text.TextWatcher {
            override fun beforeTextChanged(s: CharSequence?, a: Int, b: Int, c: Int) {}
            override fun onTextChanged(s: CharSequence?, a: Int, b: Int, c: Int) {}
            override fun afterTextChanged(s: android.text.Editable?) { rebuild() }
        })
        rebuildList = { rebuild() }
        rebuild()
        dialog.show()
    }

    private fun header(root: LinearLayout, text: String) {
        root.addView(HeaderCell(root.context).apply { setText(text) })
    }

    private fun shadow(root: LinearLayout) {
        root.addView(ShadowSectionCell(root.context))
    }

    private fun addButton(root: LinearLayout, title: String, colorKey: Int? = null, onClick: () -> Unit): TextCell {
        val cell = TextCell(root.context)
        cell.setBackgroundColor(Theme.getColor(Theme.key_windowBackgroundWhite))
        cell.setText(title, false)
        if (colorKey != null) cell.setTextColor(Theme.getColor(colorKey))
        cell.setOnClickListener { onClick() }
        root.addView(cell, LayoutHelper.createLinear(LayoutHelper.MATCH_PARENT, LayoutHelper.WRAP_CONTENT))
        return cell
    }

    private fun lookup(force: Boolean = false, thenCollect: Boolean = false) {
        val q = if (force && lastQuery.isNotEmpty()) lastQuery else query.text.toString().trim()
        if (q.isEmpty()) { toast("Введите запрос"); return }
        if (busy) { toast("Уже идёт запрос"); return }
        last = null
        fromCache = false
        collectCell.visibility = View.GONE
        savedCell.visibility = View.GONE
        refreshCell.visibility = View.GONE
        if (!force) {
            GgtgSquidwardCache.find(q)?.let { showCached(q, it); return }
        }
        busy = true
        GgtgSquidward.cancelled = false
        result.text = "Запрос к боту…"
        scope.launch {
            val r = withContext(Dispatchers.IO) { runCatching { GgtgSquidward.lookup(q) }.getOrNull() }
            busy = false
            if (r == null) { result.text = "Бот не ответил (или не найден)"; return@launch }
            last = r
            lastQuery = q
            result.text = if (r.details.isNotEmpty()) r.details.joinToString("\n") else r.rawText
            showCollect(r)
            if (thenCollect && r.canCollect) collect()
        }
    }

    private fun showCollect(r: GgtgSquidward.LookupResult) {
        if (!r.canCollect) return
        collectCell.text = if (r.kind == "user") "Собрать группы в базу" else "Собрать участников в базу"
        collectCell.visibility = View.VISIBLE
    }

    private fun showCached(q: String, saved: GgtgSquidwardCache.Saved) {
        val r = saved.result
        last = r
        lastQuery = q
        fromCache = true
        val date = java.text.SimpleDateFormat("dd.MM.yyyy HH:mm", java.util.Locale.getDefault())
            .format(java.util.Date(saved.savedAt * 1000))
        val details = if (r.details.isNotEmpty()) r.details.joinToString("\n") else r.rawText
        val list = saved.groups?.let { "\n\n✓ " + GgtgSquidward.groupsSummary(it).removePrefix("Сохранено групп: ").let { s -> "Групп: $s" } }
            ?: saved.users?.let { "\n\n✓ Юзернеймов: ${it.size}" }
            ?: ""
        result.text = "Сохранённый результат от $date — бот не запрашивался\n\n$details$list"
        refreshCell.visibility = View.VISIBLE
        val count = saved.groups?.kept?.size ?: saved.users?.size
        if (count != null && count > 0) {
            savedCell.text = "Сохранить в базу (из сохранённого, $count)"
            savedCell.visibility = View.VISIBLE
        }
        showCollect(r)
    }

    private fun saveFromCache() {
        val saved = GgtgSquidwardCache.find(lastQuery) ?: return
        val title = saved.result.baseTitle
        val id = GgtgDb.generateManualBaseId()
        saved.groups?.let { GgtgDb.saveGroupEntries(id, title, it.kept) }
        saved.users?.let { GgtgDb.saveUsernames(id, title, it) }
        savedCell.visibility = View.GONE
        toast("Сохранено в «$title»")
    }

    private fun collect() {
        val r = last ?: return
        // A list can only be collected from the bot's live card: one from the saved lookups is
        // first asked for again.
        if (fromCache) { lookup(force = true, thenCollect = true); return }
        toast("Запущено — смотрите «Процессы»")
        collectCell.visibility = View.GONE
        // Collection outlives the screen on purpose: it runs in its own scope, not the fragment's.
        CoroutineScope(SupervisorJob() + Dispatchers.IO).launch {
            runCatching { GgtgSquidward.collect(lastQuery, r.kind == "user", r.baseTitle) }
        }
    }

    override fun onFragmentDestroy() {
        GgtgSquidward.screenOpen = false
        // Back to the open account - unless a collection is still running through the chosen one.
        if (GgtgProcess.running(GgtgProcess.Kind.Scrape) == null) GgtgSquidward.viaSlot = -1
        GgtgAccounts.sleepIdle()
        scope.cancel()
        super.onFragmentDestroy()
    }

    private fun toast(text: String) { context?.let { Toast.makeText(it, text, Toast.LENGTH_LONG).show() } }
}
