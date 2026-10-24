package app.ggtg.ai

import android.content.Context
import android.view.View
import android.widget.LinearLayout
import app.ggtg.GgtgUi
import org.telegram.messenger.AndroidUtilities
import org.telegram.messenger.R
import org.telegram.ui.ActionBar.BaseFragment
import org.telegram.ui.ActionBar.Theme

// ggtg: the "ИИ" screen: one page with five numbered steps - the keys, the business and the goal, the
// materials, training on the user's chats, the feed of answers - and the rest folded below, like the
// "ИИ" window of the desktop app. Cloud models only: a free key is all it needs.
class GgtgAiActivity : BaseFragment() {

    private var column: LinearLayout? = null
    private val refresh: () -> Unit = { AndroidUtilities.runOnUIThread { rebuild() } }

    override fun onFragmentCreate(): Boolean {
        GgtgAi.addListener(refresh)
        GgtgAiInbox.addListener(refresh)
        return super.onFragmentCreate()
    }

    override fun onFragmentDestroy() {
        GgtgAi.removeListener(refresh)
        GgtgAiInbox.removeListener(refresh)
        super.onFragmentDestroy()
    }

    override fun createView(context: Context): View {
        val (frame, col) = GgtgAiUi.page(this, context, "Настройки ИИ")
        column = col
        rebuild()
        return frame
    }

    override fun onResume() { super.onResume(); rebuild() }

    private fun rebuild() {
        val col = column ?: return
        val context = col.context
        col.removeAllViews()

        // the state
        val ready = GgtgAi.ready()
        val state = GgtgUi.card(context)
        val head = LinearLayout(context).apply { orientation = LinearLayout.HORIZONTAL; gravity = android.view.Gravity.CENTER_VERTICAL }
        head.addView(View(context).apply {
            background = GgtgUi.rounded(if (ready) 0xFF3FC25C.toInt() else Theme.getColor(Theme.key_windowBackgroundWhiteGrayText), GgtgUi.dp(6f).toFloat())
        }, LinearLayout.LayoutParams(GgtgUi.dp(12f), GgtgUi.dp(12f)).apply { rightMargin = GgtgUi.dp(10f) })
        head.addView(GgtgUi.title(context, when {
            !GgtgAi.settings().enabled -> "ИИ выключен"
            !ready -> "Нужен ключ"
            else -> "ИИ работает"
        }, 17f))
        state.addView(head)
        state.addView(GgtgUi.body(context, GgtgAi.statusText(), 13f), LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply { topMargin = GgtgUi.dp(6f) })
        GgtgUi.addCard(col, state, top = 12f)

        // all the settings of the AI, nothing hidden behind "more": the services, where it helps, how it behaves
        col.addView(GgtgAiUi.section(context, "Подключения"))
        GgtgUi.addCard(col, GgtgAiUi.row(context, GgtgAiUi.iconBadge(context, R.drawable.msg_permissions), "Ключи по сервисам",
            GgtgAi.statusText() + " · все сервисы бесплатные и без карты") { presentFragment(GgtgAiKeysActivity()) })
        GgtgUi.addCard(col, GgtgAiConnect.build(context, kotlinx.coroutines.MainScope()) { rebuild() })

        col.addView(GgtgAiUi.section(context, "Помощники"))
        val s = GgtgAi.settings()
        GgtgUi.addCard(col, GgtgAiUi.switchRow(context, "ИИ включён",
            "Главный выключатель: без него ни один помощник ничего не отправляет в облако.", s.enabled) {
            s.enabled = it; GgtgAi.save()
        })

        col.addView(GgtgAiUi.section(context, "Поведение"))
        GgtgUi.addCard(col, GgtgAiUi.switchRow(context, "Учиться на моих чатах по ночам",
            "С 1:00 до 7:00 читает новые чаты отмеченных аккаунтов, утром останавливается.", s.trainNightly) {
            s.trainNightly = it; GgtgAi.save()
        })
        GgtgUi.addCard(col, GgtgAiUi.row(context, GgtgAiUi.iconBadge(context, R.drawable.msg_info), "Память ИИ",
            "Что ИИ знает о вас, материалы, выученное из чатов") { presentFragment(GgtgAiMemoryActivity()) })
        col.addView(GgtgAiUi.gap(context, 24f))
    }

    private fun profileSummary(): String {
        val facts = GgtgAiProfile.count()
        val prompt = GgtgAi.settings().chatPrompt.trim()
        return (if (prompt.isEmpty()) "Цель и описание не заполнены" else "Описание заполнено") + " · в профиле пунктов: $facts"
    }

    private fun materialsSummary(): String {
        val list = GgtgAiKnowledge.list()
        return if (list.isEmpty()) "Пока ничего не добавлено" else "Источников: ${list.size} · кусков: ${list.sumOf { it.chunks.size }}"
    }
}
