package app.ggtg.ai

import android.content.Context
import android.os.Bundle
import android.view.View
import android.widget.LinearLayout
import app.ggtg.GgtgAccounts
import app.ggtg.GgtgUi
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.launch
import org.telegram.messenger.AndroidUtilities
import org.telegram.messenger.R
import org.telegram.messenger.UserConfig
import org.telegram.ui.ActionBar.BaseFragment
import org.telegram.ui.ActionBar.Theme
import org.telegram.ui.ChatActivity
import org.telegram.ui.LaunchActivity

// ggtg: step 5 of the "ИИ" screen: "Лента ответов" - one list for everyone who answered a mailing. For each
// person the AI shows the stage they are at (a question, an objection, interest, a refusal) and a
// reply in the user's style; the user decides: "Отправить", "Изменить" (the change teaches the AI)
// or "Пропустить". A person ready to buy is a hot lead: the AI stays silent and the user is called.
// For a stage the user trusts (10 drafts sent unchanged in a row) the autopilot is offered.
class GgtgAiInboxActivity : BaseFragment() {

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main)
    private var column: LinearLayout? = null
    private var status = ""
    private var refreshing = false
    private val changed: () -> Unit = { rebuild() }

    override fun onFragmentCreate(): Boolean { GgtgAiInbox.addListener(changed); return super.onFragmentCreate() }
    override fun onFragmentDestroy() { GgtgAiInbox.removeListener(changed); scope.cancel(); super.onFragmentDestroy() }

    override fun createView(context: Context): View {
        val (frame, col) = GgtgAiUi.page(this, context, "5. Лента ответов") { id -> if (id == REFRESH) refresh() }
        actionBar.createMenu().addItem(REFRESH, R.drawable.msg_retry)
        column = col
        rebuild()
        refresh()
        return frame
    }

    private fun refresh() {
        if (refreshing) return
        refreshing = true
        status = "Ищу тех, кто ответил, и готовлю ответы..."
        rebuild()
        scope.launch {
            val error = GgtgAiInbox.refresh()
            refreshing = false
            status = if (error == null) "" else "Остановилось: $error"
            rebuild()
        }
    }

    private fun openChat(item: GgtgAiInbox.Item) {
        val slot = GgtgAiText.slotOf(item.accountUserId)
        if (slot < 0) { GgtgAiUi.toast(context, "Аккаунт сейчас недоступен"); return }
        if (slot != UserConfig.selectedAccount) LaunchActivity.instance?.switchToAccount(slot, true)
        presentFragment(ChatActivity(Bundle().apply { putLong("user_id", item.dialogId) }))
    }

    private fun rebuild() {
        val col = column ?: return
        val context = col.context
        col.removeAllViews()
        val head = GgtgUi.card(context)
        head.addView(GgtgUi.body(context, "Все, кто ответил на рассылку. ИИ определяет, на каком этапе человек (вопрос, возражение, интерес, отказ), и пишет " +
            "ответ в вашем стиле. Без вашего «Отправить» ничего не уходит. Если человек готов купить, ИИ молчит и зовёт вас.", 13f))
        if (status.isNotEmpty()) head.addView(GgtgUi.body(context, status, 13f), LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply { topMargin = GgtgUi.dp(6f) })
        GgtgUi.addCard(col, head, top = 12f)

        // the autopilot: offered for a stage the user trusts, and the stages it already answers by itself
        GgtgAiInbox.autopilotOffer()?.let { stage ->
            val card = GgtgUi.card(context)
            card.addView(GgtgUi.body(context, "На «${GgtgAiInbox.stageTitle(stage)}» вы ${GgtgAiInbox.AUTOPILOT_STREAK} раз подряд отправили ответ ИИ без правок. " +
                "Пусть ИИ отвечает на такое сам? Каждый такой ответ уходит через 3 минуты, и до этого его можно отменить. Горячих лидов и отказы автопилот не трогает.", 13f))
            card.addView(GgtgAiUi.links(context, GgtgAiUi.link("Включить автопилот") { GgtgAiInbox.setAutopilot(stage, true) },
                GgtgAiUi.link("Не сейчас") { GgtgAiInbox.declineAutopilot(stage) }))
            GgtgUi.addCard(col, card)
        }
        val on = GgtgAiInbox.mem().autopilot
        if (on.isNotEmpty()) {
            val card = GgtgUi.card(context)
            card.addView(GgtgUi.body(context, "Автопилот сам отвечает на: " + on.joinToString(", ") { GgtgAiInbox.stageTitle(it) }, 13f))
            card.addView(GgtgAiUi.links(context, *on.map { stage -> GgtgAiUi.link("Выключить «${GgtgAiInbox.stageTitle(stage)}»") { GgtgAiInbox.setAutopilot(stage, false) } }.toTypedArray()))
            GgtgUi.addCard(col, card)
        }

        val open = GgtgAiInbox.all().filter { it.status == GgtgAiInbox.Status.Pending || it.status == GgtgAiInbox.Status.Hot }
            .sortedWith(compareBy({ it.status != GgtgAiInbox.Status.Hot }, { -it.lastAt }))
        if (open.isEmpty()) {
            GgtgUi.addCard(col, GgtgUi.card(context).also {
                it.addView(GgtgUi.body(context, "Пока никто не ждёт ответа. Нажмите «обновить» вверху: лента ищет тех, кто ответил на рассылки."))
            })
        }
        for (item in open) col.addView(itemCard(context, item).also { (it.layoutParams as? LinearLayout.LayoutParams) })

        // the skipped ones are not lost: they wait here, one tap from coming back
        val skipped = GgtgAiInbox.all().filter { it.status == GgtgAiInbox.Status.Skipped }.sortedByDescending { it.updated }
        if (skipped.isNotEmpty()) {
            col.addView(GgtgAiUi.section(context, (if (showSkipped) "▾ " else "▸ ") + "Пропущенные (${skipped.size})").apply {
                setOnClickListener { showSkipped = !showSkipped; rebuild() }
            })
            if (showSkipped) for (item in skipped) {
                val card = GgtgUi.card(context)
                card.addView(GgtgUi.title(context, "@" + item.username + (if (item.name.isEmpty()) "" else " (${item.name})"), 15f))
                card.addView(GgtgUi.body(context, GgtgAiInbox.stageTitle(item.stage) + " · «" + item.theirText.take(160) + "»", 13f),
                    LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply { topMargin = GgtgUi.dp(4f) })
                card.addView(GgtgAiUi.links(context,
                    GgtgAiUi.link("Вернуть в ответы") { GgtgAiInbox.restore(item.key) },
                    GgtgAiUi.link("Открыть чат") { openChat(item) }))
                GgtgUi.addCard(col, card)
            }
        }
        col.addView(GgtgAiUi.gap(context, 24f))
    }

    private var showSkipped = false

    private fun skip(context: Context, item: GgtgAiInbox.Item) {
        GgtgAiInbox.skip(item.key)
        GgtgAiUi.toast(context, "Пропущено: @${item.username}. Он ниже, в «Пропущенных».")
    }

    private fun itemCard(context: Context, item: GgtgAiInbox.Item): View {
        val hot = item.status == GgtgAiInbox.Status.Hot
        val card = GgtgUi.card(context)
        card.addView(GgtgUi.title(context, (if (hot) "ГОРЯЧИЙ ЛИД · " else "") + "@" + item.username +
            (if (item.name.isEmpty()) "" else " (${item.name})"), 15f))
        card.addView(GgtgUi.pill(context, GgtgAiInbox.stageTitle(item.stage), 0xFFFFFFFF.toInt(), if (hot) 0xFFE5484D.toInt() else GgtgUi.accent),
            LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply { topMargin = GgtgUi.dp(4f) })
        card.addView(GgtgUi.body(context, "«" + item.theirText.take(300) + "»", 13f), LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply { topMargin = GgtgUi.dp(6f) })
        val lp = LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply { topMargin = GgtgUi.dp(8f) }
        val links = ArrayList<Triple<String, Boolean, () -> Unit>>()
        if (hot) {
            card.addView(GgtgUi.body(context, "Человек готов купить. ИИ ему не отвечает: ответьте сами и быстро.", 13f), lp)
            links.add(GgtgAiUi.link("Открыть чат") { openChat(item) })
            links.add(GgtgAiUi.link("Вернуть ИИ") { GgtgAiInbox.giveBack(item.key) })
            links.add(GgtgAiUi.link("Пропустить") { skip(context, item) })
        } else {
            val hasDraft = item.draft.isNotEmpty()
            when {
                item.drafting -> card.addView(GgtgUi.body(context, "ИИ пишет ответ...", 13f), lp)
                hasDraft -> {
                    card.addView(android.widget.TextView(context).apply {
                        text = item.draft; textSize = 15f
                        setTextColor(Theme.getColor(Theme.key_windowBackgroundWhiteBlackText))
                        setTextIsSelectable(true)
                    }, lp)
                    if (item.warning.isNotEmpty()) card.addView(GgtgUi.body(context, "" + item.warning, 12f))
                    if (item.autoAt > 0) card.addView(GgtgUi.body(context, "Автопилот отправит это сам около " +
                        java.text.SimpleDateFormat("HH:mm:ss", java.util.Locale.getDefault()).format(java.util.Date(item.autoAt)), 12f))
                }
                item.stage == "refusal" -> card.addView(GgtgUi.body(context, "Человек отказался: лучше ему больше не писать.", 13f), lp)
                item.stage.isNotEmpty() -> card.addView(GgtgUi.body(context, "ИИ не придумал ответ.", 13f), lp)
            }
            if (item.autoAt > 0) links.add(GgtgAiUi.link("Отменить автоотправку") { GgtgAiInbox.cancelAuto(item.key) })
            if (hasDraft && !item.drafting) {
                links.add(GgtgAiUi.link("Отправить") {
                    GgtgAiInbox.find(item.key)?.let { found ->
                        GgtgAiInbox.send(item.key, found.draft, false)?.let { GgtgAiUi.toast(context, "Не отправилось: $it") }
                    }
                })
                links.add(GgtgAiUi.link("Изменить") {
                    GgtgAiUi.prompt(context, "Ответ для @${item.username}", "Ваш ответ", item.draft, true, "Отправить") { text ->
                        GgtgAiInbox.send(item.key, text, text.trim().replace(Regex("\\s+"), " ") != item.draft.trim().replace(Regex("\\s+"), " "))
                            ?.let { GgtgAiUi.toast(context, "Не отправилось: $it") }
                    }
                })
            } else if (!item.drafting) {
                links.add(GgtgAiUi.link(if (item.stage == "refusal") "Всё же ответить" else "Составить ответ") {
                    scope.launch { GgtgAiInbox.draft(item.key, true)?.let { GgtgAiUi.toast(context, "Не вышло: $it") } }
                })
            }
            links.add(GgtgAiUi.link("Пропустить") { skip(context, item) })
            links.add(GgtgAiUi.link("Открыть чат") { openChat(item) })
        }
        card.addView(GgtgAiUi.links(context, *links.toTypedArray()))
        val wrap = LinearLayout(context)
        wrap.addView(card, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
        wrap.setPadding(GgtgUi.dp(12f), GgtgUi.dp(6f), GgtgUi.dp(12f), 0)
        return wrap
    }

    private companion object { const val REFRESH = 1 }
}
