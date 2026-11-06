package app.ggtg

import android.app.Activity
import android.content.Context
import android.content.Intent
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
import android.widget.Toast
import org.telegram.messenger.AndroidUtilities
import org.telegram.messenger.R
import org.telegram.ui.ActionBar.ActionBar.ActionBarMenuOnItemClick
import org.telegram.ui.ActionBar.BaseFragment
import org.telegram.ui.ActionBar.Theme
import org.telegram.ui.Components.LayoutHelper

// ng: one saved base - GGTG Desktop's "Изменить список" box, card-based like the rest of GGTG. A
// header card with the base's name + counts, the list itself in a collapsible card (editable for
// users bases), and the actions grouped in cards: open/export, the operations that make a new base
// (duplicate / subtract / merge / split / sample), rename and delete. Labels wrap, never cut.
class GgtgBaseActivity(private val baseId: Long) : BaseFragment() {

    private lateinit var editor: EditText
    private var infoView: TextView? = null
    private var titleView: TextView? = null
    private var listToggle: TextView? = null
    private var exportAs = "txt"

    private val meta get() = GgtgDb.loadIndex().firstOrNull { it.id == baseId }
    private val isGroups get() = meta?.kind == GgtgDb.BaseKind.Groups

    // ---- style helpers (shared language with the account manager / bases list) ----
    private fun dp(v: Float) = AndroidUtilities.dp(v)
    private fun color(key: Int) = Theme.getColor(key)
    private fun rounded(fill: Int, radius: Float) = GradientDrawable().apply { cornerRadius = radius; setColor(fill) }

    override fun createView(context: Context): View {
        actionBar.setBackButtonImage(R.drawable.ic_ab_back)
        actionBar.setAllowOverlayTitle(true)
        actionBar.setTitle("База")
        actionBar.setActionBarMenuOnItemClick(object : ActionBarMenuOnItemClick() {
            override fun onItemClick(id: Int) {
                when (id) { -1 -> finishFragment(); 1 -> rename() }
            }
        })
        actionBar.createMenu().addItem(1, R.drawable.msg_edit)

        val frame = FrameLayout(context)
        frame.setBackgroundColor(color(Theme.key_windowBackgroundGray))
        fragmentView = frame
        val scroll = ScrollView(context).apply { isFillViewport = true }
        val root = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(0, 0, 0, dp(16f))
        }
        scroll.addView(root, LayoutHelper.createScroll(LayoutHelper.MATCH_PARENT, LayoutHelper.WRAP_CONTENT, Gravity.TOP))
        frame.addView(scroll, LayoutHelper.createFrame(LayoutHelper.MATCH_PARENT, LayoutHelper.MATCH_PARENT.toFloat()))

        // header card: badge + title + stat line
        val header = card(context)
        header.orientation = LinearLayout.HORIZONTAL
        header.gravity = Gravity.CENTER_VERTICAL
        val headerIcon = android.widget.ImageView(context).apply {
            setImageDrawable(GgtgUi.icon(context, if (isGroups) R.drawable.msg_groups else R.drawable.msg_contacts, GgtgUi.accent))
        }
        val headerCircle = android.widget.FrameLayout(context).apply {
            background = rounded(color(Theme.key_windowBackgroundGray), dp(23f).toFloat())
            addView(headerIcon, android.widget.FrameLayout.LayoutParams(dp(24f), dp(24f), Gravity.CENTER))
        }
        header.addView(headerCircle, LinearLayout.LayoutParams(dp(46f), dp(46f)).apply { rightMargin = dp(12f) })
        val headerTexts = LinearLayout(context).apply { orientation = LinearLayout.VERTICAL }
        headerTexts.addView(TextView(context).apply {
            text = meta?.title ?: "База"; textSize = 17f
            setTypeface(null, Typeface.BOLD)
            setTextColor(color(Theme.key_windowBackgroundWhiteBlackText))
            titleView = this // the whole name wraps here; the toolbar would cut it
        })
        val info = TextView(context).apply {
            textSize = 13f
            setTextColor(color(Theme.key_windowBackgroundWhiteGrayText))
            setPadding(0, dp(2f), 0, 0)
        }
        infoView = info
        headerTexts.addView(info)
        header.addView(headerTexts, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
        root.addView(header)

        // list card: a toggle row that reveals the list below it
        val listCard = card(context)
        val listTitle = if (isGroups) "Группы" else "Список пользователей"
        val toggle = TextView(context).apply {
            text = listTitle; textSize = 15f
            setTypeface(null, Typeface.BOLD)
            setTextColor(color(Theme.key_windowBackgroundWhiteBlackText))
            setPadding(0, dp(4f), 0, dp(4f))
        }
        listToggle = toggle
        GgtgUi.trailingChevron(toggle, false)
        listCard.addView(toggle, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT))
        editor = EditText(context).apply {
            inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_MULTI_LINE
            minLines = 6; gravity = Gravity.TOP
            background = rounded(color(Theme.key_windowBackgroundGray), dp(10f).toFloat())
            setTextColor(color(Theme.key_windowBackgroundWhiteBlackText))
            setHintTextColor(color(Theme.key_windowBackgroundWhiteHintText))
            setPadding(dp(12f), dp(10f), dp(12f), dp(10f))
            visibility = View.GONE
        }
        listCard.addView(editor, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply { topMargin = dp(8f) })
        toggle.setOnClickListener {
            val show = editor.visibility != View.VISIBLE
            editor.visibility = if (show) View.VISIBLE else View.GONE
            GgtgUi.trailingChevron(toggle, show)
        }
        root.addView(listCard)
        reload()

        // primary actions card
        val main = card(context)
        if (!isGroups) {
            actionRow(main, R.drawable.msg_saved, "Сохранить изменения") { save() }
        }
        if (!isGroups) {
            // names and "last seen" are loaded on THIS button (one account after another, flood-safe), never by opening the list
            actionRow(main, R.drawable.msg_contacts, "Загрузить профили (имя, когда был в сети)…", chevron = true) {
                val ctx = context ?: parentActivity
                if (ctx != null) {
                    GgtgBaseChecker.chooseAndCheck(ctx, baseId, meta?.title ?: "База") { reload() }
                }
            }
        }
        actionRow(main, R.drawable.msg_viewchats, "Открыть списком (кликабельно)", chevron = true) { presentFragment(GgtgBaseListActivity(baseId, isGroups)) }
        if (!isGroups) {
            actionRow(main, R.drawable.msg_clear, "Очистка базы…", chevron = true) { cleanup() }
        }
        root.addView(main)

        // operations card
        sectionLabel(root, "Операции")
        val ops = card(context)
        actionRow(ops, R.drawable.msg_forward, "Экспорт в файл (txt / csv)…") { chooseExport() }
        if (!isGroups) {
            actionRow(ops, R.drawable.msg_addcontact, "Объединить") { merge() }
            actionRow(ops, R.drawable.msg_reorder, "Разбить на части…") { split() }
        }
        root.addView(ops)

        // delete, in its own card
        val danger = card(context)
        actionRow(danger, R.drawable.msg_delete, "Удалить базу", destructive = true) { delete() }
        root.addView(danger)
        return frame
    }

    // ---- card / row helpers ----

    private fun card(context: Context): LinearLayout {
        return LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            background = rounded(color(Theme.key_windowBackgroundWhite), dp(14f).toFloat())
            setPadding(dp(14f), dp(6f), dp(14f), dp(6f))
            layoutParams = LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply {
                leftMargin = dp(12f); rightMargin = dp(12f); topMargin = dp(8f)
            }
        }
    }

    private fun sectionLabel(root: LinearLayout, text: String) {
        root.addView(TextView(root.context).apply {
            this.text = text; textSize = 13f
            setTypeface(null, Typeface.BOLD)
            setTextColor(color(Theme.key_windowBackgroundWhiteGrayText))
            setPadding(dp(18f), dp(14f), dp(18f), dp(2f))
        })
    }

    private fun actionRow(card: LinearLayout, iconRes: Int, label: String, destructive: Boolean = false, chevron: Boolean = false, onClick: () -> Unit) {
        val row = LinearLayout(card.context).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding(0, dp(12f), 0, dp(12f))
            background = Theme.createSelectorDrawable(color(Theme.key_listSelector), 2)
            setOnClickListener { onClick() }
        }
        val iconColor = if (destructive) color(Theme.key_text_RedRegular) else color(Theme.key_windowBackgroundWhiteGrayIcon)
        val iv = android.widget.ImageView(card.context).apply {
            setImageDrawable(GgtgUi.icon(card.context, iconRes, iconColor))
        }
        row.addView(iv, LinearLayout.LayoutParams(dp(24f), dp(24f)).apply { rightMargin = dp(12f) })
        row.addView(TextView(card.context).apply {
            text = label; textSize = 15f
            setTextColor(if (destructive) color(Theme.key_text_RedRegular) else color(Theme.key_windowBackgroundWhiteBlackText))
        }, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
        if (chevron) row.addView(TextView(card.context).apply {
            text = "›"; textSize = 20f; setTextColor(color(Theme.key_windowBackgroundWhiteGrayIcon))
        })
        card.addView(row, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT))
    }

    private fun toast(text: String) { context?.let { Toast.makeText(it, text, Toast.LENGTH_LONG).show() } }

    // ---- data ----

    private fun names(): List<String> =
        if (isGroups) GgtgDb.loadGroupEntries(baseId).map { if (it.username.isNotEmpty()) it.username else it.link }
        else GgtgDb.loadUsernames(baseId)

    private fun reload() {
        val m = meta
        actionBar.setTitle("База")
        titleView?.text = m?.title ?: "База"
        val srcTitle = GgtgDb.sourceTitle(m?.source ?: "")
        val srcText = " · Источник: $srcTitle"
        if (isGroups) {
            val entries = GgtgDb.loadGroupEntries(baseId)
            editor.setText(entries.joinToString("\n") {
                (if (it.username.isNotEmpty()) "@" + it.username else it.link) +
                    (if (it.title.isNotEmpty()) " — " + it.title else "") + (if (it.membersCount > 0) " · ${it.membersCount} уч." else "")
            })
            editor.isEnabled = false
            infoView?.text = "Групп: ${entries.size}$srcText"
            return
        }
        val list = GgtgDb.loadUsernames(baseId)
        val bots = list.count { GgtgDb.isBotUsername(it) }
        infoView?.text = "Юзернеймов: ${list.size}" + (if (bots > 0) " · боты: $bots" else "") + srcText
        if (list.size > EDITOR_LIMIT) {
            editor.setText("")
            editor.isEnabled = false
            editor.hint = "Список слишком большой для редактирования здесь (${list.size}). Используйте кнопки ниже."
        } else {
            editor.setText(list.joinToString("\n") { "@$it" })
        }
    }

    private fun save() {
        val parsed = GgtgDb.parseUsernames(editor.text.toString().split("\n"))
        val title = meta?.title ?: return
        GgtgDb.saveUsernames(baseId, title, parsed.valid)
        toast("Сохранено: ${parsed.valid.size}" + if (parsed.rejected.isNotEmpty()) ", не распознано строк: ${parsed.rejected.size}" else "")
        reload()
    }

    /** One window to clean a base: tick what to remove (several at once), see the count for each, confirm once. */
    private fun cleanup() {
        val ctx = context ?: parentActivity ?: return
        val title = meta?.title ?: return
        val fromSquidward = meta?.source == "squidward"
        val list = GgtgDb.loadUsernames(baseId)
        if (list.isEmpty()) { toast("В этой базе нет пользователей"); return }
        fun norm(u: String) = u.trim().removePrefix("@").lowercase()
        val names = Array(6) { HashSet<String>() }
        var notChecked = 0
        for (u in list) {
            val key = norm(u)
            if (key.isEmpty()) continue
            if (GgtgDb.isBotUsername(key)) names[2].add(key)
            val lookedUp = GgtgDb.lookupUser(key)
            if (lookedUp == null || lookedUp.id == 0L) notChecked++
            else if (lookedUp.lastSeenTime == 0L && !GgtgDb.isBotUsername(key)) names[0].add(key) // profile loaded, "was seen a long time ago"
            when (GgtgDb.findSmsRecord(key)?.status) {
                GgtgDb.SmsStatus.LongAgo -> names[0].add(key)
                GgtgDb.SmsStatus.NotFound -> names[1].add(key)
                GgtgDb.SmsStatus.SuspectedFake -> names[3].add(key)
                GgtgDb.SmsStatus.Restricted -> names[4].add(key)
                GgtgDb.SmsStatus.Sent -> names[5].add(key)
                else -> {}
            }
        }
        GgtgFakeDetector.filterUsernames(list, 60).second.forEach { names[3].add(norm(it)) }
        val labels = arrayOf(
            "Неактивные («был(а) давно»)", "Не найденные в Telegram", "Боты",
            "Фейки и подозрительные", "Закрытые для сообщений", "Уже написанные (статус OK)"
        )
        val hints = arrayOf(
            "Давно не заходили в Telegram", "Юзернейма уже нет", "Юзернейм заканчивается на «bot»",
            "По имени и профилю, порог 60%", "Запретили писать или заблокировали нас", "Им уже отправляли сообщение"
        )
        val chosen = BooleanArray(6) { fromSquidward && it in 0..3 }

        val column = LinearLayout(ctx).apply { orientation = LinearLayout.VERTICAL; setPadding(dp(20f), dp(8f), dp(20f), dp(4f)) }
        column.addView(TextView(ctx).apply {
            text = if (fromSquidward) "База собрана через Сквидвард: данные могли быть неточными, очистка рекомендуется."
            else "Источник: ${GgtgDb.sourceTitle(meta?.source.orEmpty())}. Из открытой группы приходят реальные люди, такую базу обычно чистить не нужно."
            textSize = 13f
            setTextColor(color(Theme.key_windowBackgroundWhiteGrayText))
            setPadding(0, 0, 0, dp(8f))
        })
        for (i in labels.indices) {
            column.addView(android.widget.CheckBox(ctx).apply {
                text = "${labels[i]} — ${names[i].size}"
                isChecked = chosen[i]
                setTextColor(color(Theme.key_windowBackgroundWhiteBlackText))
                setOnCheckedChangeListener { _, checked -> chosen[i] = checked }
            })
            column.addView(TextView(ctx).apply {
                text = hints[i]
                textSize = 12f
                setTextColor(color(Theme.key_windowBackgroundWhiteGrayText))
                setPadding(dp(32f), 0, 0, dp(6f))
            })
        }
        column.addView(TextView(ctx).apply {
            text = (if (notChecked > 0) "Профили не загружены у $notChecked человек. " else "Профили загружены у всех. ") +
                "Проверка загружает имя, статус и признак бота: после неё очистка точнее."
            textSize = 12f
            setTextColor(color(Theme.key_windowBackgroundWhiteGrayText))
            setPadding(0, dp(8f), 0, 0)
        })
        val scroll = ScrollView(ctx).apply { addView(column) }
        val builder = org.telegram.ui.ActionBar.AlertDialog.Builder(ctx)
            .setTitle("Очистка базы: $title")
            .setView(scroll)
            .setPositiveButton("Очистить") { _, _ ->
                val remove = HashSet<String>()
                val lines = ArrayList<String>()
                for (i in labels.indices) if (chosen[i]) { remove.addAll(names[i]); if (names[i].isNotEmpty()) lines.add("• ${labels[i]}: ${names[i].size}") }
                if (remove.isEmpty()) { toast("Ничего не отмечено или нечего убирать"); return@setPositiveButton }
                org.telegram.ui.ActionBar.AlertDialog.Builder(ctx)
                    .setTitle("Удалить из базы «$title»")
                    .setMessage(lines.joinToString("\n") + "\n\nВсего человек (без повторов): ${remove.size}. Список изменится насовсем.")
                    .setPositiveButton("Удалить") { _, _ ->
                        val current = GgtgDb.loadUsernames(baseId)
                        val kept = current.filter { norm(it) !in remove }
                        GgtgDb.saveUsernames(baseId, title, kept)
                        toast("Убрано из базы: ${current.size - kept.size}, осталось: ${kept.size}")
                        reload()
                    }
                    .setNegativeButton("Отмена", null).show()
            }
            .setNegativeButton("Отмена", null)
        if (notChecked > 0) {
            builder.setNeutralButton("Проверить профили") { _, _ ->
                GgtgBaseChecker.chooseAndCheck(ctx, baseId, title) { reload() }
            }
        }
        builder.show()
    }

    private fun removeInactive() {
        val list = GgtgDb.loadUsernames(baseId)
        val inactive = list.count { GgtgDb.findSmsRecord(it)?.status == GgtgDb.SmsStatus.LongAgo }
        if (inactive == 0) { toast("Неактивных («был(а) давно») в базе нет"); return }
        GgtgDb.saveUsernames(baseId, meta?.title ?: return, list.filter { GgtgDb.findSmsRecord(it)?.status != GgtgDb.SmsStatus.LongAgo })
        toast("Удалено неактивных: $inactive")
        reload()
    }

    private fun removeSent() {
        val list = GgtgDb.loadUsernames(baseId)
        val sent = list.count { GgtgDb.findSmsRecord(it)?.status == GgtgDb.SmsStatus.Sent }
        if (sent == 0) { toast("Написанных (OK) в базе нет"); return }
        GgtgDb.saveUsernames(baseId, meta?.title ?: return, list.filter { GgtgDb.findSmsRecord(it)?.status != GgtgDb.SmsStatus.Sent })
        toast("Удалено написанных: $sent")
        reload()
    }

    private fun removeBots() {
        val list = GgtgDb.loadUsernames(baseId)
        val bots = list.count { GgtgDb.isBotUsername(it) }
        if (bots == 0) { toast("Ботов в списке нет"); return }
        GgtgDb.saveUsernames(baseId, meta?.title ?: return, list.filter { !GgtgDb.isBotUsername(it) })
        toast("Убрано ботов: $bots")
        reload()
    }

    private fun rename() {
        val context = context ?: return
        val m = meta ?: return
        val field = EditText(context).apply {
            setText(m.title); setSingleLine()
            setPadding(AndroidUtilities.dp(24f), AndroidUtilities.dp(8f), AndroidUtilities.dp(24f), AndroidUtilities.dp(8f))
        }
        org.telegram.ui.ActionBar.AlertDialog.Builder(context).setTitle("Название").setView(field)
            .setPositiveButton("Сохранить") { _, _ ->
                val name = field.text.toString().trim()
                if (name.isEmpty()) { toast("Введите название"); return@setPositiveButton }
                if (isGroups) GgtgDb.saveGroupEntries(baseId, name, GgtgDb.loadGroupEntries(baseId))
                else GgtgDb.saveUsernames(baseId, name, GgtgDb.loadUsernames(baseId), crossDedupe = false)
                reload()
            }.setNegativeButton("Отмена", null).show()
    }

    private fun delete() {
        val context = context ?: return
        org.telegram.ui.ActionBar.AlertDialog.Builder(context)
            .setMessage("Удалить базу «${meta?.title}» (${names().size})? Это нельзя отменить.")
            .setPositiveButton("Удалить") { _, _ -> GgtgDb.deleteBase(baseId); finishFragment() }
            .setNegativeButton("Отмена", null).show()
    }

    // ---- export ----

    private fun chooseExport() {
        val context = context ?: return
        org.telegram.ui.ActionBar.AlertDialog.Builder(context).setTitle("Экспорт списка")
            .setItems(arrayOf("Текст (.txt)", "CSV (.csv)")) { _, which ->
                exportAs = if (which == 0) "txt" else "csv"
                val intent = Intent(Intent.ACTION_CREATE_DOCUMENT).apply {
                    addCategory(Intent.CATEGORY_OPENABLE)
                    type = if (exportAs == "csv") "text/csv" else "text/plain"
                    putExtra(Intent.EXTRA_TITLE, (meta?.title ?: "base").replace(Regex("[^\\p{L}\\p{N}_-]+"), "_") + "." + exportAs)
                }
                startActivityForResult(intent, EXPORT_REQUEST)
            }.show()
    }

    override fun onActivityResultFragment(requestCode: Int, resultCode: Int, data: Intent?) {
        if (requestCode != EXPORT_REQUEST || resultCode != Activity.RESULT_OK) return
        val uri = data?.data ?: return
        val context = context ?: return
        try {
            val list = names()
            val text = if (exportAs == "csv") "username\n" + list.joinToString("\n") { "@$it" } else list.joinToString("\n") { "@$it" }
            context.contentResolver.openOutputStream(uri)?.use { it.write(text.toByteArray()) }
            toast("Экспортировано: ${list.size}")
        } catch (t: Throwable) { toast("Не удалось записать файл") }
    }

    // ---- new base from this one ----

    private fun otherUserBases() = GgtgDb.loadIndex().filter { it.id != baseId && it.kind == GgtgDb.BaseKind.Users }

    private fun merge() {
        val context = context ?: return
        val others = otherUserBases()
        if (others.isEmpty()) { toast("Других баз пользователей нет"); return }
        val checked = BooleanArray(others.size)
        GgtgPageUi.multiChoice(context, "Объединить с…", others.map { "${it.title} (${it.count})" as CharSequence }, checked, "Объединить") {
                val chosen = others.filterIndexed { i, _ -> checked[i] }
                if (chosen.isEmpty()) { toast("Отметьте хотя бы одну базу"); return@multiChoice }
                // The chosen bases move into this one (a person lives in one
                // base only), then they are gone from the list.
                val all = GgtgDb.loadUsernames(baseId) + chosen.flatMap { GgtgDb.loadUsernames(it.id) }
                chosen.forEach { GgtgDb.deleteBase(it.id) }
                GgtgDb.saveUsernames(baseId, meta?.title ?: "База", all.distinctBy { it.lowercase() }, crossDedupe = false)
                toast("Базы объединены: ${GgtgDb.loadUsernames(baseId).size}")
                reload()
            }
    }

    private fun split() {
        val list = GgtgDb.loadUsernames(baseId)
        askNumber("Разбить на части", "На сколько частей", 2) { parts ->
            if (parts < 2 || parts > list.size) { toast("Частей должно быть от 2 до ${list.size}"); return@askNumber }
            val size = (list.size + parts - 1) / parts
            // The first part stays in this base, the others become new ones.
            val title = meta?.title ?: "База"
            val chunks = list.chunked(size)
            GgtgDb.saveUsernames(baseId, "$title (часть 1/${chunks.size})", chunks.first(), crossDedupe = false)
            chunks.drop(1).forEachIndexed { i, chunk ->
                GgtgDb.saveUsernames(GgtgDb.generateManualBaseId(), "$title (часть ${i + 2}/${chunks.size})", chunk, crossDedupe = false)
            }
            toast("Создано частей: ${chunks.size}")
            reload()
        }
    }

    private fun askNumber(title: String, hint: String, preset: Int, onNumber: (Int) -> Unit) {
        val context = context ?: return
        val field = EditText(context).apply {
            this.hint = hint; setText(preset.toString()); inputType = InputType.TYPE_CLASS_NUMBER
            setPadding(AndroidUtilities.dp(24f), AndroidUtilities.dp(8f), AndroidUtilities.dp(24f), AndroidUtilities.dp(8f))
        }
        org.telegram.ui.ActionBar.AlertDialog.Builder(context).setTitle(title).setView(field)
            .setPositiveButton("Готово") { _, _ -> field.text.toString().toIntOrNull()?.let(onNumber) }
            .setNegativeButton("Отмена", null).show()
    }

    private companion object {
        const val EDITOR_LIMIT = 3000
        const val EXPORT_REQUEST = 4711
    }
}
