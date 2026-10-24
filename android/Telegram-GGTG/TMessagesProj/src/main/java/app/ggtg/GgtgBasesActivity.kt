package app.ggtg

import android.content.Context
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.text.InputType
import android.view.Gravity
import android.view.View
import android.widget.EditText
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.telegram.messenger.AndroidUtilities
import org.telegram.messenger.R
import org.telegram.ui.ActionBar.ActionBar.ActionBarMenuOnItemClick
import org.telegram.ui.ActionBar.AlertDialog
import org.telegram.ui.ActionBar.BaseFragment
import org.telegram.ui.ActionBar.Theme
import org.telegram.ui.Components.LayoutHelper
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

// ng: the "База" screen - the saved groups/users lists (GgtgDb), card-based like the account
// manager. Tap a base to view it; import a list as a new base; search public groups. Titles wrap
// and are never cut with "…".
class GgtgBasesActivity : BaseFragment() {

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main)
    private var container: LinearLayout? = null
    private var query = ""
    private var sortMode = 0 // 0 = newest, 1 = name, 2 = size
    private var listsHolder: LinearLayout? = null
    private val expanded = HashSet<GgtgDb.BaseKind>()

    // ---- style helpers (shared language with the account manager) ----
    private fun dp(v: Float) = AndroidUtilities.dp(v)
    private fun color(key: Int) = Theme.getColor(key)
    private fun rounded(fill: Int, radius: Float) = GradientDrawable().apply { cornerRadius = radius; setColor(fill) }

    override fun createView(context: Context): View {
        actionBar.setBackButtonImage(R.drawable.ic_ab_back)
        actionBar.setAllowOverlayTitle(true)
        actionBar.setTitle("База")
        actionBar.setActionBarMenuOnItemClick(object : ActionBarMenuOnItemClick() {
            override fun onItemClick(id: Int) {
                when (id) {
                    -1 -> finishFragment()
                    1 -> showImportDialog()
                    2 -> showSearchDialog()
                    3 -> showImportGroupsDialog()
                    4 -> presentFragment(GgtgSentActivity())
                }
            }
        })
        val menu = actionBar.createMenu()
        menu.addItem(4, R.drawable.msg_send)
        menu.addItem(2, R.drawable.msg_search)
        menu.addItem(3, R.drawable.msg_folders)
        menu.addItem(1, R.drawable.msg_addcontact)

        val frame = FrameLayout(context)
        frame.setBackgroundColor(color(Theme.key_windowBackgroundGray))
        fragmentView = frame
        val scroll = ScrollView(context).apply { isFillViewport = true }
        val list = LinearLayout(context).apply { orientation = LinearLayout.VERTICAL }
        scroll.addView(list, LayoutHelper.createScroll(LayoutHelper.MATCH_PARENT, LayoutHelper.WRAP_CONTENT, Gravity.TOP))
        frame.addView(scroll, LayoutHelper.createFrame(LayoutHelper.MATCH_PARENT, LayoutHelper.MATCH_PARENT.toFloat()))
        container = list
        rebuild()
        return frame
    }

    override fun onResume() { super.onResume(); rebuild() }

    private fun rebuild() {
        val list = container ?: return
        val context = list.context
        list.removeAllViews()

        // search in a rounded card
        val searchCard = LinearLayout(context).apply {
            background = rounded(color(Theme.key_windowBackgroundWhite), dp(12f).toFloat())
            setPadding(dp(14f), dp(2f), dp(14f), dp(2f))
        }
        val search = EditText(context).apply {
            hint = "Поиск по названию"; setSingleLine(); setText(query)
            background = null
            setTextColor(color(Theme.key_windowBackgroundWhiteBlackText))
            setHintTextColor(color(Theme.key_windowBackgroundWhiteHintText))
            setPadding(0, dp(10f), 0, dp(10f))
            addTextChangedListener(object : android.text.TextWatcher {
                override fun beforeTextChanged(s: CharSequence?, a: Int, b: Int, c: Int) {}
                override fun onTextChanged(s: CharSequence?, a: Int, b: Int, c: Int) {}
                override fun afterTextChanged(s: android.text.Editable?) {
                    val q = s?.toString().orEmpty().trim()
                    if (q != query) { query = q; rebuildLists() }
                }
            })
        }
        searchCard.addView(search, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT))
        list.addView(searchCard, LayoutHelper.createLinear(LayoutHelper.MATCH_PARENT, LayoutHelper.WRAP_CONTENT, 12f, 12f, 12f, 0f))

        // sort as three pills
        val sortRow = LinearLayout(context).apply { orientation = LinearLayout.HORIZONTAL }
        listOf("Новые", "По названию", "По размеру").forEachIndexed { i, name ->
            sortRow.addView(TextView(context).apply {
                text = name; textSize = 13f
                setTypeface(null, if (sortMode == i) Typeface.BOLD else Typeface.NORMAL)
                setTextColor(if (sortMode == i) 0xFFFFFFFF.toInt() else color(Theme.key_windowBackgroundWhiteBlackText))
                setPadding(dp(14f), dp(7f), dp(14f), dp(7f))
                background = rounded(if (sortMode == i) color(Theme.key_featuredStickers_addButton) else color(Theme.key_windowBackgroundWhite), dp(15f).toFloat())
                setOnClickListener { sortMode = i; rebuild() }
            }, LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply { rightMargin = dp(8f) })
        }
        list.addView(sortRow, LayoutHelper.createLinear(LayoutHelper.WRAP_CONTENT, LayoutHelper.WRAP_CONTENT, 12f, 10f, 12f, 2f))

        val lists = LinearLayout(context).apply { orientation = LinearLayout.VERTICAL }
        listsHolder = lists
        list.addView(lists, LayoutHelper.createLinear(LayoutHelper.MATCH_PARENT, LayoutHelper.WRAP_CONTENT, 0f, 0f, 0f, 16f))
        rebuildLists()
    }

    private fun rebuildLists() {
        val holder = listsHolder ?: return
        val context = holder.context
        holder.removeAllViews()
        val all = GgtgDb.loadIndex().filter { query.isEmpty() || it.title.lowercase().contains(query.lowercase()) }
        val sorted = when (sortMode) {
            1 -> all.sortedBy { it.title.lowercase() }
            2 -> all.sortedByDescending { it.count }
            else -> all.sortedByDescending { it.savedAt }
        }
        if (sorted.isEmpty()) {
            holder.addView(TextView(context).apply {
                text = if (query.isEmpty()) "Пока нет сохранённых баз.\nИмпортируйте список или соберите через «Сквидвард»." else "Ничего не найдено"
                setTextColor(color(Theme.key_windowBackgroundWhiteGrayText))
                gravity = Gravity.CENTER
                setPadding(dp(16f), dp(28f), dp(16f), dp(16f))
            }, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT))
            return
        }
        for ((title, kind) in listOf("Группы" to GgtgDb.BaseKind.Groups, "Пользователи" to GgtgDb.BaseKind.Users)) {
            val part = sorted.filter { it.kind == kind }
            if (part.isEmpty()) continue
            val open = expanded.contains(kind)
            holder.addView(sectionHeader(context, title, part.size, open) {
                if (!expanded.remove(kind)) expanded.add(kind); rebuildLists()
            })
            if (!open) continue
            for (meta in part) holder.addView(baseCard(context, meta, kind))
        }
    }

    private fun sectionHeader(context: Context, title: String, n: Int, open: Boolean, onClick: () -> Unit) =
        LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding(dp(18f), dp(14f), dp(18f), dp(6f))
            addView(TextView(context).apply {
                text = "$title · $n"; textSize = 13f
                setTypeface(null, Typeface.BOLD)
                setTextColor(color(Theme.key_windowBackgroundWhiteGrayText))
            }, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
            addView(TextView(context).apply {
                text = if (open) "▾" else "▸"; textSize = 15f
                setTextColor(color(Theme.key_windowBackgroundWhiteGrayIcon))
            })
            setOnClickListener { onClick() }
        }

    private fun baseCard(context: Context, meta: GgtgDb.GroupMeta, kind: GgtgDb.BaseKind): View {
        val card = LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            background = rounded(color(Theme.key_windowBackgroundWhite), dp(14f).toFloat())
            setPadding(dp(14f), dp(12f), dp(14f), dp(12f))
            layoutParams = LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply {
                leftMargin = dp(12f); rightMargin = dp(12f); topMargin = dp(6f)
            }
        }
        val isGroups = kind == GgtgDb.BaseKind.Groups
        // round icon badge
        card.addView(GgtgUi.circleIcon(context, if (isGroups) R.drawable.msg_groups else R.drawable.msg_contact, 42f, color(Theme.key_windowBackgroundGray), GgtgUi.accent),
            LinearLayout.LayoutParams(dp(42f), dp(42f)).apply { rightMargin = dp(12f) })

        val texts = LinearLayout(context).apply { orientation = LinearLayout.VERTICAL }
        texts.addView(TextView(context).apply {
            text = meta.title; textSize = 16f
            setTypeface(null, Typeface.BOLD)
            setTextColor(color(Theme.key_windowBackgroundWhiteBlackText))
        })
        val date = if (meta.savedAt > 0) SimpleDateFormat("dd.MM.yyyy", Locale.getDefault()).format(Date(meta.savedAt * 1000)) else ""
        val groupWord = if (meta.count % 10 == 1 && meta.count % 100 != 11) "группа" else if (meta.count % 10 in 2..4 && meta.count % 100 !in 12..14) "группы" else "групп"
        texts.addView(TextView(context).apply {
            text = (if (isGroups) "${meta.count} $groupWord" else "осталось ${GgtgDb.remainingIn(meta.id)} из ${meta.count}") + (if (date.isNotEmpty()) "  ·  $date" else "")
            textSize = 13f
            setTextColor(color(Theme.key_windowBackgroundWhiteGrayText))
            setPadding(0, dp(2f), 0, 0)
        })
        card.addView(texts, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
        card.addView(TextView(context).apply {
            text = "›"; textSize = 22f
            setTextColor(color(Theme.key_windowBackgroundWhiteGrayIcon))
        }, LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply { leftMargin = dp(6f) })

        card.setOnClickListener { presentFragment(GgtgBaseActivity(meta.id)) }
        // long press: delete right from the list. Whoever was written to keeps the note where they came from.
        card.setOnLongClickListener {
            GgtgUi.menu(context, meta.title, listOf(
                GgtgUi.MenuItem("Открыть списком (кликабельно)", R.drawable.msg_viewchats) { presentFragment(GgtgBaseListActivity(meta.id, isGroups)) },
                *(if (isGroups) emptyArray() else arrayOf(
                    GgtgUi.MenuItem("Очистить от фейков и ботов…", R.drawable.msg_bots) {
                        GgtgFakeDetector.showCleanBaseDialog(context, meta.id, meta.title) { rebuildLists() }
                    },
                    GgtgUi.MenuItem("Анализ и чистка базы…", R.drawable.msg_info) { GgtgAnalysis.open(this, meta.id, meta.title) { rebuildLists() } })),
                GgtgUi.MenuItem("Копировать название", R.drawable.msg_copy) {
                    AndroidUtilities.addToClipboard(meta.title)
                    android.widget.Toast.makeText(context, "Скопировано", android.widget.Toast.LENGTH_SHORT).show()
                },
                GgtgUi.MenuItem("Удалить", R.drawable.msg_delete) {
                    org.telegram.ui.ActionBar.AlertDialog.Builder(context)
                        .setTitle(meta.title)
                        .setMessage("Удалить список «${meta.title}» (${meta.count})? Это нельзя отменить.\n\n" +
                            "Кому из этого списка уже писали - у тех останется пометка, из какого списка они были.")
                        .setPositiveButton("Удалить") { _, _ -> GgtgDb.deleteBase(meta.id); rebuildLists() }
                        .setNegativeButton("Отмена", null)
                        .show()
                }))
            true
        }
        return card
    }

    private fun showImportDialog() {
        val context = context ?: return
        val root = LinearLayout(context)
        root.orientation = LinearLayout.VERTICAL
        root.setPadding(AndroidUtilities.dp(20f), 0, AndroidUtilities.dp(20f), 0)
        val nameField = EditText(context).apply { hint = "Название базы" }
        val listField = EditText(context).apply {
            hint = "@username по одному в строке, или t.me/ссылки"
            inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_MULTI_LINE
            minLines = 4
        }
        root.addView(nameField)
        root.addView(listField)
        AlertDialog.Builder(context)
            .setTitle("Импорт списка пользователей")
            .setView(root)
            .setPositiveButton("Сохранить") { _, _ ->
                val parsed = GgtgDb.parseUsernames(listField.text.toString().split("\n"))
                if (parsed.valid.isEmpty()) {
                    showToast("Не найдено ни одного корректного @username")
                    return@setPositiveButton
                }
                val title = nameField.text.toString().trim().ifEmpty { "Импорт (${parsed.valid.size})" }
                GgtgDb.saveUsernames(GgtgDb.generateManualBaseId(), title, parsed.valid)
                showToast("Сохранено: ${parsed.valid.size}, дублей ${parsed.duplicates}, отброшено ${parsed.rejected.size}")
                rebuild()
            }
            .setNegativeButton("Отмена", null)
            .show()
    }

    private fun showImportGroupsDialog() {
        val context = context ?: return
        val root = LinearLayout(context)
        root.orientation = LinearLayout.VERTICAL
        root.setPadding(AndroidUtilities.dp(20f), 0, AndroidUtilities.dp(20f), 0)
        val nameField = EditText(context).apply { hint = "Название базы" }
        val listField = EditText(context).apply {
            hint = "@username или t.me/ссылка, по одной в строке; после табуляции или « — » — название"
            inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_MULTI_LINE
            minLines = 4
        }
        root.addView(nameField)
        root.addView(listField)
        AlertDialog.Builder(context)
            .setTitle("Импорт списка групп")
            .setView(root)
            .setPositiveButton("Сохранить") { _, _ ->
                val entries = GgtgDb.parseGroupLines(listField.text.toString().split("\n"))
                if (entries.isEmpty()) {
                    showToast("Не найдено ни одной корректной группы/ссылки")
                    return@setPositiveButton
                }
                val title = nameField.text.toString().trim().ifEmpty { "Импорт (${entries.size})" }
                GgtgDb.saveGroupEntries(GgtgDb.generateManualBaseId(), title, entries)
                showToast("Сохранено групп: ${entries.size}")
                rebuild()
            }
            .setNegativeButton("Отмена", null)
            .show()
    }

    private fun showSearchDialog() {
        val context = context ?: return
        val root = LinearLayout(context)
        root.orientation = LinearLayout.VERTICAL
        root.setPadding(AndroidUtilities.dp(20f), 0, AndroidUtilities.dp(20f), 0)
        val keyword = EditText(context).apply { hint = "Ключевое слово" }
        val minMembers = EditText(context).apply { hint = "Минимум участников (0 — любые)"; inputType = InputType.TYPE_CLASS_NUMBER }
        root.addView(keyword)
        root.addView(minMembers)
        AlertDialog.Builder(context)
            .setTitle("Поиск групп")
            .setView(root)
            .setPositiveButton("Искать") { _, _ ->
                val kw = keyword.text.toString().trim()
                if (kw.isEmpty()) { showToast("Введите ключевое слово"); return@setPositiveButton }
                val min = minMembers.text.toString().toIntOrNull() ?: 0
                showToast("Поиск запущен — смотрите «Процессы»")
                scope.launch { withContext(Dispatchers.IO) { GgtgScraper.searchGroups(kw, min) }; rebuild() }
            }
            .setNegativeButton("Отмена", null)
            .show()
    }

    private fun showToast(text: String) {
        val context = context ?: return
        android.widget.Toast.makeText(context, text, android.widget.Toast.LENGTH_LONG).show()
    }
}
