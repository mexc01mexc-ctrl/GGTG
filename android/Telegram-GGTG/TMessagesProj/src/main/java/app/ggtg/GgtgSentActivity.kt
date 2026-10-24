package app.ggtg

import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.os.Bundle
import android.text.Editable
import android.text.TextWatcher
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.widget.EditText
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.TextView
import android.widget.Toast
import androidx.recyclerview.widget.LinearLayoutManager
import androidx.recyclerview.widget.RecyclerView
import org.telegram.messenger.AndroidUtilities
import org.telegram.messenger.R
import org.telegram.ui.ActionBar.ActionBar.ActionBarMenuOnItemClick
import org.telegram.ui.ActionBar.BaseFragment
import org.telegram.ui.ActionBar.Theme
import org.telegram.ui.Cells.TextDetailCell
import org.telegram.ui.Components.LayoutHelper
import org.telegram.ui.Components.RecyclerListView
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

// ng: "Отправленные" - GGTG Desktop's browser of the global sent registry (sms_sent.json): who was
// already written to, with status filter + search. A row opens that record's actions (open chat /
// copy / reset status); the action-bar button drops the whole registry after a confirmation.
class GgtgSentActivity : BaseFragment() {

    private val shown = ArrayList<GgtgDb.SmsRecord>()
    private var all: List<GgtgDb.SmsRecord> = emptyList()
    private var query = ""
    private var filter = 0 // 0 all, 1 OK, 2 SPAM_ACC, 3 not found, 4 restricted
    private var listView: RecyclerListView? = null
    private var summary: TextView? = null
    private var filterButton: TextView? = null

    private val dbListener: () -> Unit = { AndroidUtilities.runOnUIThread { refresh() } }

    private fun statusText(s: GgtgDb.SmsStatus) = when (s) {
        GgtgDb.SmsStatus.SpamAcc -> "спам-блок аккаунта"
        GgtgDb.SmsStatus.NotFound -> "не найден"
        GgtgDb.SmsStatus.Restricted -> "закрыт для ЛС"
        GgtgDb.SmsStatus.PeerLimit -> "отказ Telegram"
        GgtgDb.SmsStatus.SuspectedFake -> "предположительный фейк"
        GgtgDb.SmsStatus.Sent -> "OK"
    }

    private fun filterLabel() = when (filter) {
        7 -> "Фильтр: предположительный фейк"; 6 -> "Фильтр: ответили"; 1 -> "Фильтр: OK"; 2 -> "Фильтр: спам-блок аккаунта"; 3 -> "Фильтр: не найден"; 4 -> "Фильтр: закрыт для ЛС"; 5 -> "Фильтр: отказ Telegram"
        else -> "Фильтр: все статусы"
    }

    override fun onFragmentCreate(): Boolean {
        GgtgDb.addListener(dbListener)
        return super.onFragmentCreate()
    }

    override fun onFragmentDestroy() {
        GgtgDb.removeListener(dbListener)
        super.onFragmentDestroy()
    }

    override fun createView(context: Context): View {
        actionBar.setBackButtonImage(R.drawable.ic_ab_back)
        actionBar.setAllowOverlayTitle(true)
        actionBar.setTitle("Отправленные")
        actionBar.setActionBarMenuOnItemClick(object : ActionBarMenuOnItemClick() {
            override fun onItemClick(id: Int) {
                when (id) { -1 -> finishFragment(); 1 -> confirmClear(); 2 -> resetPeerLimit(); 3 -> resetBlocked() }
            }
        })
        val more = actionBar.createMenu().addItem(0, R.drawable.ic_ab_other)
        more.addSubItem(2, R.drawable.msg_reset, "Сбросить только «не пустило»")
        more.addSubItem(3, R.drawable.msg_reset, "Сбросить спам-блок / не найден / закрыт для ЛС")
        more.addSubItem(1, R.drawable.msg_delete, "Очистить весь реестр")

        val frame = FrameLayout(context)
        frame.setBackgroundColor(Theme.getColor(Theme.key_windowBackgroundGray))
        fragmentView = frame

        val root = LinearLayout(context).apply { orientation = LinearLayout.VERTICAL }
        frame.addView(root, LayoutHelper.createFrame(LayoutHelper.MATCH_PARENT, LayoutHelper.MATCH_PARENT.toFloat()))

        val search = EditText(context).apply {
            hint = "Поиск по @username или аккаунту"
            setSingleLine()
            textSize = 15f
            background = GgtgUi.rounded(Theme.getColor(Theme.key_windowBackgroundWhite), AndroidUtilities.dp(12f).toFloat())
            setTextColor(Theme.getColor(Theme.key_windowBackgroundWhiteBlackText))
            setHintTextColor(Theme.getColor(Theme.key_windowBackgroundWhiteHintText))
            setPadding(AndroidUtilities.dp(14f), AndroidUtilities.dp(11f), AndroidUtilities.dp(14f), AndroidUtilities.dp(11f))
            addTextChangedListener(object : TextWatcher {
                override fun beforeTextChanged(s: CharSequence?, a: Int, b: Int, c: Int) {}
                override fun onTextChanged(s: CharSequence?, a: Int, b: Int, c: Int) {}
                override fun afterTextChanged(s: Editable?) { query = s.toString().trim().removePrefix("@").lowercase(); refresh() }
            })
        }
        root.addView(search, LayoutHelper.createLinear(LayoutHelper.MATCH_PARENT, LayoutHelper.WRAP_CONTENT, 12f, 10f, 12f, 0f))

        // status filter chips
        val chipsScroll = android.widget.HorizontalScrollView(context).apply { isHorizontalScrollBarEnabled = false }
        val chips = LinearLayout(context).apply { orientation = LinearLayout.HORIZONTAL }
        chipsScroll.addView(chips)
        chipViews.clear()
        for ((index, label) in listOf("Все", "OK", "Спам-блок акк.", "Не найден", "Закрыт для ЛС", "Отказ Telegram", "Ответили").withIndex()) {
            val chip = TextView(context).apply {
                text = label
                textSize = 14f
                setPadding(AndroidUtilities.dp(14f), AndroidUtilities.dp(8f), AndroidUtilities.dp(14f), AndroidUtilities.dp(8f))
                setOnClickListener { filter = index; styleChips(); refresh() }
            }
            chipViews.add(chip)
            chips.addView(chip, LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply { rightMargin = AndroidUtilities.dp(8f) })
        }
        styleChips()
        root.addView(chipsScroll, LayoutHelper.createLinear(LayoutHelper.MATCH_PARENT, LayoutHelper.WRAP_CONTENT, 12f, 8f, 12f, 0f))

        summary = TextView(context).apply {
            textSize = 13f
            setTextColor(Theme.getColor(Theme.key_windowBackgroundWhiteGrayText))
            setPadding(AndroidUtilities.dp(18f), AndroidUtilities.dp(8f), AndroidUtilities.dp(18f), AndroidUtilities.dp(4f))
        }
        root.addView(summary, LayoutHelper.createLinear(LayoutHelper.MATCH_PARENT, LayoutHelper.WRAP_CONTENT))

        val list = RecyclerListView(context)
        list.setLayoutManager(LinearLayoutManager(context))
        list.adapter = Adapter(context)
        list.setOnItemClickListener { _, position, _, _ -> shown.getOrNull(position)?.let { showActions(it) } }
        root.addView(list, LayoutHelper.createLinear(LayoutHelper.MATCH_PARENT, 0, 1f))
        listView = list
        refresh()
        return frame
    }

    private val chipViews = ArrayList<TextView>()

    private fun styleChips() {
        for ((i, chip) in chipViews.withIndex()) {
            val active = i == filter
            chip.setTextColor(if (active) 0xFFFFFFFF.toInt() else Theme.getColor(Theme.key_windowBackgroundWhiteBlackText))
            chip.setTypeface(null, if (active) android.graphics.Typeface.BOLD else android.graphics.Typeface.NORMAL)
            chip.background = GgtgUi.rounded(if (active) GgtgUi.accent else Theme.getColor(Theme.key_windowBackgroundWhite), AndroidUtilities.dp(16f).toFloat())
        }
    }

    private fun refresh() {
        all = GgtgDb.loadAllSmsRecords()
        val matched = all.filter { r ->
            val okFilter = when (filter) {
                1 -> r.status == GgtgDb.SmsStatus.Sent
                2 -> r.status == GgtgDb.SmsStatus.SpamAcc
                3 -> r.status == GgtgDb.SmsStatus.NotFound
                4 -> r.status == GgtgDb.SmsStatus.Restricted
                5 -> r.status == GgtgDb.SmsStatus.PeerLimit
                6 -> GgtgDb.findReply(r.username) != null
                7 -> r.status == GgtgDb.SmsStatus.SuspectedFake
                else -> true
            }
            okFilter && (query.isEmpty() || r.username.contains(query) || r.account.lowercase().contains(query)
                || GgtgDb.sourceOf(r).lowercase().contains(query))
        }
        shown.clear()
        shown.addAll(matched.take(PAGE_LIMIT))
        summary?.text = if (all.isEmpty()) "Пока пусто. Здесь появятся получатели после первой рассылки."
        else "Всего записей: ${all.size}, ответили: ${GgtgDb.countReplies()}. Подходит: ${matched.size}, показано: ${shown.size}."
        listView?.adapter?.notifyDataSetChanged()
    }

    private fun showActions(r: GgtgDb.SmsRecord) {
        val context = context ?: return
        val whenText = SimpleDateFormat("dd.MM.yyyy HH:mm", Locale.getDefault()).format(Date(r.at * 1000))
        org.telegram.ui.ActionBar.AlertDialog.Builder(context)
            .setTitle("@" + r.username)
            .setMessage("Статус: ${statusText(r.status)}\nАккаунт: ${r.account}" +
                (GgtgDb.sourceOf(r).let { if (it.isEmpty()) "" else "\nИз базы: «$it»" }) +
                "\nКогда: $whenText" +
                (GgtgDb.findReply(r.username)?.let { "\n\nОтветил: «${it.text}»" } ?: ""))
            .setItems(arrayOf("Открыть чат", "Копировать @username", "Сбросить статус (можно писать снова)")) { _, which ->
                when (which) {
                    0 -> openChat(r.username)
                    1 -> {
                        (context.getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager)
                            .setPrimaryClip(ClipData.newPlainText("username", "@" + r.username))
                        toast("Скопировано")
                    }
                    2 -> org.telegram.ui.ActionBar.AlertDialog.Builder(context)
                        .setMessage("Сбросить статус @${r.username}? Рассылка снова сможет ему написать.")
                        .setPositiveButton("Сбросить") { _, _ -> GgtgDb.deleteSmsRecord(r.username); toast("Статус сброшен") }
                        .setNegativeButton("Отмена", null).show()
                }
            }.setNegativeButton("Назад", null).show()
    }

    private fun openChat(username: String) {
        org.telegram.messenger.browser.Browser.openUrl(context ?: return, "https://t.me/$username")
    }

    // «не пустило»: Telegram refused the pair while the account was free - another account may try.
    private fun resetPeerLimit() {
        val removed = GgtgDb.clearSmsByStatus(setOf(GgtgDb.SmsStatus.PeerLimit))
        toast(if (removed > 0) "Сброшено «не пустило»: $removed. Теперь их может попробовать другой аккаунт." else "Записей «не пустило» нет")
    }

    private fun resetBlocked() {
        val removed = GgtgDb.clearSmsByStatus(setOf(GgtgDb.SmsStatus.SpamAcc, GgtgDb.SmsStatus.NotFound, GgtgDb.SmsStatus.Restricted, GgtgDb.SmsStatus.PeerLimit))
        toast(if (removed > 0) "Сброшено записей: $removed" else "Сбрасывать нечего")
    }

    private fun confirmClear() {
        val context = context ?: return
        if (all.isEmpty()) { toast("Список пуст"); return }
        org.telegram.ui.ActionBar.AlertDialog.Builder(context)
            .setMessage("Очистить весь реестр (${all.size})? Рассылка снова сможет писать всем, включая тех, кому уже писали.")
            .setPositiveButton("Очистить") { _, _ -> GgtgDb.clearSmsRecords(); toast("Реестр очищен") }
            .setNegativeButton("Отмена", null).show()
    }

    private fun toast(text: String) { context?.let { Toast.makeText(it, text, Toast.LENGTH_LONG).show() } }

    private inner class Adapter(val ctx: Context) : RecyclerListView.SelectionAdapter() {
        override fun isEnabled(holder: RecyclerView.ViewHolder) = true
        override fun getItemCount() = shown.size

        override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): RecyclerView.ViewHolder {
            val row = LinearLayout(ctx).apply {
                orientation = LinearLayout.HORIZONTAL
                gravity = Gravity.CENTER_VERTICAL
                setBackgroundColor(Theme.getColor(Theme.key_windowBackgroundWhite))
                setPadding(AndroidUtilities.dp(16f), AndroidUtilities.dp(10f), AndroidUtilities.dp(16f), AndroidUtilities.dp(10f))
                layoutParams = RecyclerView.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT)
            }
            val texts = LinearLayout(ctx).apply { orientation = LinearLayout.VERTICAL }
            texts.addView(GgtgUi.title(ctx, "", 16f).apply { tag = "user" })
            texts.addView(GgtgUi.body(ctx, "").apply { tag = "acc" })
            row.addView(texts, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
            row.addView(GgtgUi.pill(ctx, "", 0xFFFFFFFF.toInt(), 0).apply { tag = "pill" })
            return RecyclerListView.Holder(row)
        }

        override fun onBindViewHolder(holder: RecyclerView.ViewHolder, position: Int) {
            val r = shown[position]
            val row = holder.itemView as LinearLayout
            row.findViewWithTag<TextView>("user").text = "@${r.username}"
            val source = GgtgDb.sourceOf(r)
            row.findViewWithTag<TextView>("acc").text = r.account + (if (source.isEmpty()) "" else " · из «$source»") +
                (if (GgtgDb.findReply(r.username) != null) " · ответил" else "")
            row.findViewWithTag<TextView>("pill").apply {
                text = statusText(r.status)
                background = GgtgUi.rounded(
                    when (r.status) {
                        GgtgDb.SmsStatus.Sent -> 0xFF2E9E5B.toInt()
                        GgtgDb.SmsStatus.SpamAcc -> Theme.getColor(Theme.key_text_RedRegular)
                        else -> 0xFFE0A030.toInt()
                    }, AndroidUtilities.dp(10f).toFloat())
            }
        }
    }

    private companion object { const val PAGE_LIMIT = 300 }
}
