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
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.telegram.messenger.AndroidUtilities
import org.telegram.messenger.R
import org.telegram.ui.ActionBar.ActionBar.ActionBarMenuOnItemClick
import org.telegram.ui.ActionBar.BaseFragment
import org.telegram.ui.ActionBar.Theme
import org.telegram.ui.Cells.HeaderCell
import org.telegram.ui.Cells.TextCell
import org.telegram.ui.Components.LayoutHelper
import org.telegram.ui.Components.GgtgProxyPoolStore

// ng: one proxy list of the pool - GGTG Desktop's "MakeProxyListEditorBox": the entries as text
// (host:port:user:pass, one per line), save, rename, delete, and a proxy check.
class GgtgProxyListActivity(private var name: String) : BaseFragment() {

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main)
    private lateinit var editor: EditText
    private var status: TextView? = null

    private var checkJob: kotlinx.coroutines.Job? = null
    private var isCheckingAll = false
    private lateinit var checkAllBtn: TextView
    private lateinit var summaryText: TextView
    private lateinit var removeDeadBtn: TextView
    private val deadEntries = mutableListOf<String>()

    override fun createView(context: Context): View {
        actionBar.setBackButtonImage(R.drawable.ic_ab_back)
        actionBar.setAllowOverlayTitle(true)
        actionBar.setTitle("Список прокси") // the full name is in the card below: a toolbar cuts long titles
        actionBar.setActionBarMenuOnItemClick(object : ActionBarMenuOnItemClick() {
            override fun onItemClick(id: Int) {
                when (id) { -1 -> finishFragment(); 1 -> rename() }
            }
        })
        actionBar.createMenu().addItem(1, R.drawable.msg_edit)

        val frame = FrameLayout(context)
        frame.setBackgroundColor(Theme.getColor(Theme.key_windowBackgroundGray))
        fragmentView = frame
        val root = GgtgUi.page(context, frame)

        val infoCard = GgtgUi.card(context)
        val info = GgtgUi.title(context, "")
        status = info
        infoCard.addView(info)
        infoCard.addView(GgtgUi.body(context, "Формат: host:port:логин:пароль, по одному в строке. Список расходуется при регистрации и импорте: каждому аккаунту достаётся свой прокси."),
            LinearLayout.LayoutParams(LayoutHelper.MATCH_PARENT, LayoutHelper.WRAP_CONTENT).apply { topMargin = GgtgUi.dp(4f) })
        GgtgUi.addCard(root, infoCard, top = 10f)

        // Asynchronous check card with live summary and purge dead proxies
        val checkCard = GgtgUi.card(context)
        checkAllBtn = GgtgUi.withLeadingIcon(GgtgUi.button(context, "Проверить все") { toggleCheckAll() }, R.drawable.msg_select, 0xFFFFFFFF.toInt(), 16f, 6f)
        checkCard.addView(checkAllBtn)

        summaryText = GgtgUi.body(context, "", 13f)
        summaryText.visibility = View.GONE
        checkCard.addView(summaryText, LinearLayout.LayoutParams(LayoutHelper.MATCH_PARENT, LayoutHelper.WRAP_CONTENT).apply { topMargin = GgtgUi.dp(6f) })

        removeDeadBtn = GgtgUi.withLeadingIcon(GgtgUi.button(context, "Убрать не отвечающие", destructive = true) { removeDeadProxies() }, R.drawable.msg_delete, 0xFFFFFFFF.toInt(), 16f, 6f)
        removeDeadBtn.visibility = View.GONE
        checkCard.addView(removeDeadBtn, LinearLayout.LayoutParams(LayoutHelper.MATCH_PARENT, LayoutHelper.WRAP_CONTENT).apply { topMargin = GgtgUi.dp(6f) })

        GgtgUi.addCard(root, checkCard, top = 8f)

        val editCard = GgtgUi.card(context)
        editor = GgtgUi.input(context, "1.2.3.4:8080:user:pass", GgtgProxyPoolStore.getListText(name), multi = true, minLines = 8)
        GgtgUi.labeled(context, editCard, "Прокси", null, editor)
        GgtgUi.addCard(root, editCard)
        refreshInfo()

        GgtgUi.addButton(root, GgtgUi.button(context, "Сохранить") { save() })
        GgtgUi.addButton(root, GgtgUi.button(context, "Проверить первую строку") { check() }, top = 8f)
        GgtgUi.addButton(root, GgtgUi.button(context, "Удалить список", destructive = true) { delete() }, top = 8f)
        root.addView(GgtgUi.spacer(context))
        return frame
    }

    private fun toast(text: String) { context?.let { Toast.makeText(it, text, Toast.LENGTH_LONG).show() } }

    private fun refreshInfo() { status?.text = "$name — прокси в списке: ${GgtgProxyPoolStore.getCount(name)}" }

    private fun save() {
        val kept = GgtgProxyPoolStore.replaceList(name, editor.text.toString())
        val total = editor.text.toString().lines().count { it.isNotBlank() }
        toast("Сохранено: $kept" + if (total > kept) ", не распознано строк: ${total - kept}" else "")
        editor.setText(GgtgProxyPoolStore.getListText(name))
        refreshInfo()
    }

    private fun toggleCheckAll() {
        if (isCheckingAll) {
            checkJob?.cancel()
            isCheckingAll = false
            checkAllBtn.text = "Проверить все"
            summaryText.text = summaryText.text.toString() + " (остановлено)"
            return
        }
        val lines = editor.text.toString().lines().map { it.trim() }.filter { it.isNotEmpty() }
        if (lines.isEmpty()) {
            toast("Список прокси пуст")
            return
        }
        isCheckingAll = true
        checkAllBtn.text = "Остановить проверку"
        deadEntries.clear()
        removeDeadBtn.visibility = View.GONE
        summaryText.visibility = View.VISIBLE

        checkJob = scope.launch {
            var alive = 0
            var dead = 0
            for ((index, line) in lines.withIndex()) {
                if (!isCheckingAll) break
                val entry = GgtgProxyPoolStore.Entry.parse(line)
                summaryText.text = "Проверено: $index из ${lines.size} (живых: $alive, не отвечают: $dead)…"
                if (entry == null) {
                    dead++
                    deadEntries.add(line)
                    continue
                }
                val res = withContext(Dispatchers.IO) {
                    GgtgProxyCheck.check(entry.host, entry.port, entry.username, entry.password)
                }
                if (res.ok) {
                    alive++
                } else {
                    dead++
                    deadEntries.add(line)
                }
                summaryText.text = "Проверено: ${index + 1} из ${lines.size} (живых: $alive, не отвечают: $dead)"
                if (dead > 0) {
                    removeDeadBtn.text = "Убрать не отвечающие ($dead)"
                    removeDeadBtn.visibility = View.VISIBLE
                }
            }
            isCheckingAll = false
            checkAllBtn.text = "Проверить все"
            summaryText.text = "Результат: проверено ${lines.size}. Живых: $alive, не отвечают: $dead."
        }
    }

    private fun removeDeadProxies() {
        if (deadEntries.isEmpty()) return
        val currentLines = editor.text.toString().lines().map { it.trim() }.filter { it.isNotEmpty() }
        val remaining = currentLines.filter { it !in deadEntries }
        val removed = currentLines.size - remaining.size
        editor.setText(remaining.joinToString("\n"))
        save()
        deadEntries.clear()
        removeDeadBtn.visibility = View.GONE
        summaryText.text = "Удалено не отвечающих прокси: $removed. Осталось: ${remaining.size}."
        toast("Удалено $removed не отвечающих прокси")
    }

    private fun check() {
        val entry = GgtgProxyPoolStore.Entry.parse(editor.text.toString().lines().firstOrNull { it.isNotBlank() }.orEmpty())
        if (entry == null) { toast("Нет прокси для проверки"); return }
        toast("Проверяю…")
        scope.launch {
            val result = withContext(Dispatchers.IO) { GgtgProxyCheck.check(entry.host, entry.port, entry.username, entry.password) }
            context?.let {
                org.telegram.ui.ActionBar.AlertDialog.Builder(it).setTitle("Проверка прокси").setMessage(result.text)
                    .setPositiveButton("Закрыть", null)
                    .setNeutralButton("Копировать") { _, _ -> AndroidUtilities.addToClipboard(result.text) }
                    .show()
            }
        }
    }

    private fun rename() {
        val context = context ?: return
        val field = EditText(context).apply {
            setText(name); setSingleLine()
            setPadding(AndroidUtilities.dp(24f), AndroidUtilities.dp(8f), AndroidUtilities.dp(24f), AndroidUtilities.dp(8f))
        }
        org.telegram.ui.ActionBar.AlertDialog.Builder(context).setTitle("Название списка").setView(field)
            .setPositiveButton("Сохранить") { _, _ ->
                val newName = field.text.toString().trim()
                if (newName.isEmpty()) return@setPositiveButton
                save()
                GgtgProxyPoolStore.renameList(name, newName)
                name = newName
                refreshInfo()
            }.setNegativeButton("Отмена", null).show()
    }

    private fun delete() {
        val context = context ?: return
        org.telegram.ui.ActionBar.AlertDialog.Builder(context)
            .setMessage("Удалить список «$name» (${GgtgProxyPoolStore.getCount(name)} прокси)? Это нельзя отменить.")
            .setPositiveButton("Удалить") { _, _ -> GgtgProxyPoolStore.deleteList(name); finishFragment() }
            .setNegativeButton("Отмена", null).show()
    }

    override fun onFragmentDestroy() {
        super.onFragmentDestroy()
        checkJob?.cancel()
    }
}
