package app.ggtg

import android.content.Context
import android.text.InputType
import android.view.Gravity
import android.view.View
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import android.widget.Toast
import org.json.JSONArray
import org.json.JSONObject
import org.telegram.messenger.AndroidUtilities
import org.telegram.messenger.ApplicationLoader
import org.telegram.messenger.UserConfig
import org.telegram.tgnet.TLRPC
import org.telegram.ui.ActionBar.BottomSheet
import org.telegram.ui.ActionBar.Theme
import org.telegram.ui.Components.ChatActivityEnterView
import java.io.File

// ng: saved message templates ("Шаблоны сообщений" in GGTG Desktop). Typing "/" alone in a private
// chat lists them; tap inserts the template rendered for that chat ([NAME], {а|б}), long press
// opens "Изменить / Удалить" - the desktop's right-click menu.
object GgtgTemplates {

    data class Template(val id: Long, val name: String, val text: String)

    private val lock = Any()
    private fun file() = File(File(ApplicationLoader.getFilesDirFixed(), "ggtg_bulk_db").also { it.mkdirs() }, "templates.json")

    fun load(): List<Template> = synchronized(lock) {
        val f = file()
        if (!f.exists()) return emptyList()
        try {
            val arr = JSONArray(f.readText())
            (0 until arr.length()).map {
                val o = arr.getJSONObject(it)
                Template(o.getLong("id"), o.getString("name"), o.getString("text"))
            }
        } catch (t: Throwable) { emptyList() }
    }

    private fun save(list: List<Template>) = synchronized(lock) {
        val arr = JSONArray()
        list.forEach { arr.put(JSONObject().put("id", it.id).put("name", it.name).put("text", it.text)) }
        file().writeText(arr.toString())
    }

    fun upsert(id: Long, name: String, text: String) {
        val list = load().toMutableList()
        val i = list.indexOfFirst { it.id == id }
        val item = Template(id, name, text)
        if (i >= 0) list[i] = item else list.add(item)
        save(list)
    }

    fun delete(id: Long) = save(load().filter { it.id != id })

    fun newId(): Long = System.currentTimeMillis()

    // ---- UI ----

    @JvmStatic
    fun showPicker(context: Context, enterView: ChatActivityEnterView, user: TLRPC.User) {
        val self = UserConfig.getInstance(UserConfig.selectedAccount).currentUser
        lateinit var sheet: BottomSheet
        val list = LinearLayout(context).apply { orientation = LinearLayout.VERTICAL }

        // where this person was found (the group's name with its @username): a tap copies it
        val source = GgtgDb.sourceOf(user.username.orEmpty())
        val groupName = GgtgSms.groupNameFromTitle(source)

        fun rebuild() {
            list.removeAllViews()
            if (source.isNotEmpty()) {
                list.addView(label(context, "Откуда контакт: $source", true).apply {
                    setOnClickListener {
                        AndroidUtilities.addToClipboard(source)
                        android.widget.Toast.makeText(context, "Скопировано", android.widget.Toast.LENGTH_SHORT).show()
                    }
                })
            }
            val templates = load()
            if (templates.isEmpty()) list.addView(label(context, "Шаблонов пока нет", false))
            for (t in templates) {
                val row = row(context, t.name, t.text)
                row.setOnClickListener {
                    sheet.dismiss()
                    val rendered = GgtgSms.render(t.text, user.first_name.orEmpty(), user.last_name.orEmpty(),
                        self?.first_name.orEmpty(), self?.last_name.orEmpty(), groupName)
                    enterView.setFieldText(rendered)
                    enterView.editField?.let { it.requestFocus(); it.setSelection(it.length()) }
                }
                row.setOnLongClickListener {
                    showItemMenu(context, t) { rebuild() }
                    true
                }
                list.addView(row)
            }
            val add = label(context, "＋ Новый шаблон…", true)
            add.setOnClickListener { edit(context, null, "") { rebuild() } }
            list.addView(add)
        }
        rebuild()

        val scroll = ScrollView(context).apply { addView(list) }
        sheet = BottomSheet.Builder(context).setTitle("Шаблоны", true).setCustomView(scroll).show()
    }

    private fun label(context: Context, text: String, accent: Boolean) = TextView(context).apply {
        this.text = text
        textSize = 16f
        setTextColor(Theme.getColor(if (accent) Theme.key_windowBackgroundWhiteBlueText4 else Theme.key_windowBackgroundWhiteGrayText))
        setPadding(AndroidUtilities.dp(22f), AndroidUtilities.dp(14f), AndroidUtilities.dp(22f), AndroidUtilities.dp(14f))
        if (accent) setBackground(Theme.createSelectorDrawable(Theme.getColor(Theme.key_listSelector), 2))
    }

    private fun row(context: Context, name: String, text: String) = LinearLayout(context).apply {
        orientation = LinearLayout.VERTICAL
        setPadding(AndroidUtilities.dp(22f), AndroidUtilities.dp(10f), AndroidUtilities.dp(22f), AndroidUtilities.dp(10f))
        setBackground(Theme.createSelectorDrawable(Theme.getColor(Theme.key_listSelector), 2))
        addView(TextView(context).apply {
            this.text = name; textSize = 16f
            setTextColor(Theme.getColor(Theme.key_windowBackgroundWhiteBlackText))
        })
        addView(TextView(context).apply {
            this.text = text; textSize = 13f
            setTextColor(Theme.getColor(Theme.key_windowBackgroundWhiteGrayText))
            maxLines = 6 // a long template shows its first lines whole, never cut with "…"
        })
    }

    private fun showItemMenu(context: Context, t: Template, onChanged: () -> Unit) {
        org.telegram.ui.ActionBar.AlertDialog.Builder(context)
            .setTitle(t.name)
            .setItems(arrayOf("Изменить…", "Удалить")) { _, which ->
                if (which == 0) edit(context, t, "", onChanged)
                else { delete(t.id); Toast.makeText(context, "Шаблон удалён", Toast.LENGTH_SHORT).show(); onChanged() }
            }
            .show()
    }

    /** Create ([existing] null) or edit a template; also used by the "save field text" action. */
    @JvmStatic
    fun edit(context: Context, existing: Template?, presetText: String = "", onSaved: () -> Unit) {
        val box = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(AndroidUtilities.dp(22f), AndroidUtilities.dp(8f), AndroidUtilities.dp(22f), 0)
        }
        val name = EditText(context).apply { hint = "Название"; setText(existing?.name.orEmpty()); setSingleLine() }
        val text = EditText(context).apply {
            hint = "Текст шаблона ([NAME], {а|б})"
            inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_MULTI_LINE
            minLines = 3; gravity = Gravity.TOP
            setText(existing?.text ?: presetText)
        }
        box.addView(name); box.addView(text)
        org.telegram.ui.ActionBar.AlertDialog.Builder(context)
            .setTitle(if (existing == null) "Новый шаблон" else "Изменить шаблон")
            .setView(box)
            .setPositiveButton("Сохранить") { _, _ ->
                val n = name.text.toString().trim()
                val t = text.text.toString()
                if (n.isEmpty()) { Toast.makeText(context, "Введите название", Toast.LENGTH_SHORT).show(); return@setPositiveButton }
                if (t.isBlank()) { Toast.makeText(context, "Введите текст шаблона", Toast.LENGTH_SHORT).show(); return@setPositiveButton }
                upsert(existing?.id ?: newId(), n, t)
                Toast.makeText(context, "Шаблон сохранён", Toast.LENGTH_SHORT).show()
                onSaved()
            }
            .setNegativeButton("Отмена", null)
            .show()
    }
}
