package app.ggtg.chatlists

import android.content.Context
import android.text.Editable
import android.text.TextWatcher
import android.view.Gravity
import android.view.View
import android.widget.LinearLayout
import android.widget.TextView
import app.ggtg.GgtgUi
import org.telegram.messenger.R
import org.telegram.ui.ActionBar.BaseFragment
import org.telegram.ui.ActionBar.Theme
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

// ggtg: "Списки чатов": the lists as rows; a tap opens a list as a screen of its own. In it a tap on a chat
// opens the chat (in its account) on top of the list, and "back" comes back to the same place in the
// list, so the user goes on down the list. Chats are put in from a chat's menu: "В список GGTG".
class GgtgChatListsActivity : BaseFragment() {

    private var column: LinearLayout? = null
    private var query = ""
    private var search: android.widget.EditText? = null
    private var results: LinearLayout? = null
    private val changed: () -> Unit = { rebuild() }

    override fun onFragmentCreate(): Boolean { GgtgChatLists.addListener(changed); return super.onFragmentCreate() }
    override fun onFragmentDestroy() { GgtgChatLists.removeListener(changed); super.onFragmentDestroy() }

    override fun createView(context: Context): View {
        val (frame, col) = app.ggtg.GgtgPageUi.page(this, context, "Списки чатов") { id -> if (id == NEW) newList() }
        actionBar.createMenu().addItem(NEW, R.drawable.msg_add)
        column = col
        rebuild()
        return frame
    }

    override fun onResume() { super.onResume(); rebuild() }

    private fun newList() {
        val context = context ?: return
        app.ggtg.GgtgPageUi.prompt(context, "Новый список", "Название (например, Заинтересованные)", ok = "Создать") { name ->
            val id = GgtgChatLists.create(name)
            if (id != null) presentFragment(GgtgChatListActivity(id))
        }
    }

    private fun rebuild() {
        val col = column ?: return
        val context = col.context
        col.removeAllViews()
        val head = GgtgUi.card(context)
        head.addView(GgtgUi.body(context, "Свои списки переписок: «Заинтересованные», «Подумать», «Клиенты»... Чтобы добавить чат, откройте его, нажмите " +
            "«⋮» → «В список GGTG» (или выделите чаты в списке чатов и выберите это в меню). Список открывается нажатием; в нём нажатие на чат " +
            "открывает его в нужном аккаунте, а «назад» возвращает в список.", 13f))
        GgtgUi.addCard(col, head, top = 12f)
        val hadFocus = search?.hasFocus() == true
        // one search over all the lists (also chats of accounts that are no longer in the program)
        val searchCard = GgtgUi.card(context)
        val field = app.ggtg.GgtgPageUi.field(context, "Найти чат во всех списках", query)
        field.addTextChangedListener(object : TextWatcher {
            override fun beforeTextChanged(p0: CharSequence?, p1: Int, p2: Int, p3: Int) {}
            override fun onTextChanged(p0: CharSequence?, p1: Int, p2: Int, p3: Int) {}
            override fun afterTextChanged(p0: Editable?) {
                val text = p0?.toString().orEmpty().trim()
                if (text != query) { query = text; showBody() }
            }
        })
        search = field
        searchCard.addView(field, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT))
        GgtgUi.addCard(col, searchCard, top = 12f)
        results = LinearLayout(context).apply { orientation = LinearLayout.VERTICAL }
        col.addView(results)
        showBody()
        if (hadFocus) field.requestFocus()
    }

    private fun showBody() {
        val col = results ?: return
        val context = col.context
        col.removeAllViews()
        if (query.isNotEmpty()) {
            val fmt = SimpleDateFormat("dd.MM.yyyy", Locale.getDefault())
            val found = ArrayList<Pair<GgtgChatLists.ChatList, GgtgChatLists.Entry>>()
            for (l in GgtgChatLists.all()) for (e in l.entries) {
                if (e.name.contains(query, true) || e.username.contains(query, true) || e.accountName.contains(query, true)) found.add(l to e)
            }
            found.sortByDescending { it.second.at }
            if (found.isEmpty()) GgtgUi.addCard(col, GgtgUi.card(context).also { it.addView(GgtgUi.body(context, "Ничего не найдено.")) }, top = 8f)
            for ((l, e) in found.take(200)) {
                val title = e.name.ifEmpty { if (e.username.isEmpty()) "Чат" else "@" + e.username }
                val gone = app.ggtg.GgtgPageUi.slotOf(e.accountUserId) < 0
                val sub = l.name + " · " + (if (e.username.isEmpty()) "" else "@${e.username} · ") + e.accountName +
                    (if (gone) " (аккаунта нет в программе)" else "") + " · " + fmt.format(Date(e.at * 1000))
                GgtgUi.addCard(col, app.ggtg.GgtgPageUi.row(context, app.ggtg.GgtgPageUi.iconBadge(context, R.drawable.msg_list), title, sub) {
                    GgtgChatLists.openChat(this, e)
                }, top = 4f)
            }
            col.addView(app.ggtg.GgtgPageUi.gap(context, 24f))
            return
        }
        val lists = GgtgChatLists.all()
        if (lists.isEmpty()) GgtgUi.addCard(col, GgtgUi.card(context).also { it.addView(GgtgUi.body(context, "Списков пока нет. Нажмите «+» вверху.")) })
        for (l in lists) {
            GgtgUi.addCard(col, app.ggtg.GgtgPageUi.row(context, app.ggtg.GgtgPageUi.iconBadge(context, R.drawable.msg_list), l.name, "Чатов: ${l.entries.size}") {
                presentFragment(GgtgChatListActivity(l.id))
            })
        }
        GgtgUi.addButton(col, GgtgUi.button(context, "Новый список") { newList() }, top = 14f)
        col.addView(app.ggtg.GgtgPageUi.gap(context, 24f))
    }

    private companion object { const val NEW = 1 }
}

// ggtg: one list of chats: the newest first, a search, a tap opens the chat (the list stays under it), the
// cross takes a chat out. "Написать всем" sends one message (with a photo and a caption) to the whole list.
class GgtgChatListActivity(private val listId: String) : BaseFragment() {

    private var column: LinearLayout? = null
    private var query = ""
    private var limit = PAGE
    private var search: android.widget.EditText? = null
    private val changed: () -> Unit = { rebuild() }

    override fun onFragmentCreate(): Boolean { GgtgChatLists.addListener(changed); return super.onFragmentCreate() }
    override fun onFragmentDestroy() { GgtgChatLists.removeListener(changed); super.onFragmentDestroy() }

    override fun createView(context: Context): View {
        val (frame, col) = app.ggtg.GgtgPageUi.page(this, context, GgtgChatLists.find(listId)?.name ?: "Список") { id ->
            when (id) { RENAME -> rename(); DELETE -> deleteList() }
        }
        val menu = actionBar.createMenu()
        val other = menu.addItem(0, R.drawable.ic_ab_other)
        other.addSubItem(RENAME, R.drawable.msg_edit, "Переименовать")
        other.addSubItem(DELETE, R.drawable.msg_delete, "Удалить список")
        column = col
        rebuild()
        return frame
    }

    private fun rename() {
        val context = context ?: return
        val list = GgtgChatLists.find(listId) ?: return
        app.ggtg.GgtgPageUi.prompt(context, "Переименовать список", "Название", list.name) { GgtgChatLists.rename(listId, it); actionBar.setTitle(GgtgChatLists.find(listId)?.name ?: "") }
    }

    private fun deleteList() {
        val context = context ?: return
        val list = GgtgChatLists.find(listId) ?: return
        app.ggtg.GgtgPageUi.confirm(context, "Удалить список «${list.name}»? Сами чаты не удалятся.") { GgtgChatLists.delete(listId); finishFragment() }
    }

    private fun rebuild() {
        val col = column ?: return
        val context = col.context
        val list = GgtgChatLists.find(listId)
        if (list == null) { finishFragment(); return }
        val hadFocus = search?.hasFocus() == true
        col.removeAllViews()
        actionBar.setTitle(list.name)

        val head = GgtgUi.card(context)
        head.addView(GgtgUi.title(context, "${list.name} · ${list.entries.size}", 16f))
        val field = app.ggtg.GgtgPageUi.field(context, "Найти чат по имени или @username", query)
        field.addTextChangedListener(object : TextWatcher {
            override fun beforeTextChanged(p0: CharSequence?, p1: Int, p2: Int, p3: Int) {}
            override fun onTextChanged(p0: CharSequence?, p1: Int, p2: Int, p3: Int) {}
            override fun afterTextChanged(p0: Editable?) {
                val text = p0?.toString().orEmpty().trim()
                if (text != query) { query = text; limit = PAGE; rebuildRows() }
            }
        })
        search = field
        head.addView(field, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply { topMargin = GgtgUi.dp(8f) })
        GgtgUi.addCard(col, head, top = 12f)
        GgtgUi.addCard(col, app.ggtg.GgtgPageUi.row(context, app.ggtg.GgtgPageUi.iconBadge(context, R.drawable.msg_send), "Написать всем",
            "Текст или фото с подписью, с паузами между чатами") { presentFragment(GgtgChatListSendActivity(listId)) })
        rows = LinearLayout(context).apply { orientation = LinearLayout.VERTICAL }
        col.addView(rows)
        rebuildRows()
        if (hadFocus) field.requestFocus()
    }

    private var rows: LinearLayout? = null

    private fun rebuildRows() {
        val box = rows ?: return
        val context = box.context
        val list = GgtgChatLists.find(listId) ?: return
        box.removeAllViews()
        // all chats stay, also of accounts that are gone from the program: only the user takes them out
        val entries = list.entries.filter {
            (query.isEmpty() || it.name.contains(query, true) || it.username.contains(query, true) || it.accountName.contains(query, true))
        }.sortedByDescending { it.at }
        if (entries.isEmpty()) {
            GgtgUi.addCard(box, GgtgUi.card(context).also {
                it.addView(GgtgUi.body(context, if (query.isEmpty()) "Список пуст. Откройте чат → «⋮» → «В список GGTG»." else "Ничего не найдено."))
            })
            return
        }
        val fmt = SimpleDateFormat("dd.MM.yyyy", Locale.getDefault())
        for (e in entries.take(limit)) {
            val title = e.name.ifEmpty { if (e.username.isEmpty()) "Чат" else "@" + e.username }
            val sub = (if (e.username.isEmpty()) "" else "@${e.username} · ") + e.accountName +
                (if (app.ggtg.GgtgPageUi.slotOf(e.accountUserId) < 0) " (аккаунта нет в программе)" else "") + " · " + fmt.format(Date(e.at * 1000))
            val card = GgtgUi.card(context) { GgtgChatLists.openChat(this, e) }
            val line = LinearLayout(context).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
            val texts = LinearLayout(context).apply { orientation = LinearLayout.VERTICAL }
            texts.addView(GgtgUi.title(context, title, 15f))
            texts.addView(GgtgUi.body(context, sub, 12f))
            line.addView(texts, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
            line.addView(app.ggtg.GgtgPageUi.icon(context, R.drawable.msg_delete, Theme.getColor(Theme.key_text_RedRegular)).apply {
                setOnClickListener { GgtgChatLists.remove(listId, e.accountUserId, e.dialogId) }
            }, LinearLayout.LayoutParams(GgtgUi.dp(40f), GgtgUi.dp(40f)))
            card.addView(line)
            GgtgUi.addCard(box, card, top = 4f)
        }
        if (entries.size > limit) {
            val more = entries.size - limit
            GgtgUi.addButton(box, GgtgUi.button(context, "Показать ещё ${minOf(more, PAGE)} (осталось $more)") { limit += PAGE; rebuildRows() }, top = 10f)
        }
        box.addView(app.ggtg.GgtgPageUi.gap(context, 24f))
    }

    private companion object { const val PAGE = 100; const val RENAME = 11; const val DELETE = 12 }
}
