package app.ggtg.ai

import android.content.Context
import android.os.Handler
import android.os.Looper
import android.view.View
import android.widget.LinearLayout
import app.ggtg.GgtgAccounts
import app.ggtg.GgtgUi
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.launch
import org.telegram.messenger.R
import org.telegram.messenger.UserConfig
import org.telegram.ui.ActionBar.BaseFragment

// ggtg: step 4 of the "ИИ" screen: training on the user's own chats - a long background run: the AI reads
// the personal chats of the chosen accounts where the user wrote and takes the style, the questions
// people ask, the objections and the user's answers, and how the talks ended. The run is a row in
// "Процессы", waits out the limits of the free keys, can be stopped and goes on from the same chat.
class GgtgAiTrainActivity : BaseFragment() {

    private var column: LinearLayout? = null
    private val handler = Handler(Looper.getMainLooper())
    private var message = ""
    private val tick = object : Runnable {
        override fun run() { rebuild(); handler.postDelayed(this, 2000) }
    }

    override fun createView(context: Context): View {
        val (frame, col) = GgtgAiUi.page(this, context, "4. Обучение на чатах")
        column = col
        rebuild()
        return frame
    }

    override fun onResume() { super.onResume(); handler.postDelayed(tick, 2000) }
    override fun onPause() { super.onPause(); handler.removeCallbacks(tick) }

    private fun rebuild() {
        val col = column ?: return
        val context = col.context
        col.removeAllViews()
        val head = GgtgUi.card(context)
        head.addView(GgtgUi.body(context, "ИИ читает личные чаты выбранных аккаунтов, где писали вы, и берёт оттуда ваш стиль, частые вопросы людей, " +
            "возражения и ваши ответы на них, а ещё чем кончались диалоги. Ход виден в «Процессах», при лимитах ключей он ждёт сам, остановить можно в любой " +
            "момент и продолжить с того же места. Читаются последние 600 сообщений каждого чата; группы, каналы и боты не берутся. Тексты уходят облачной " +
            "модели без имён, номеров и @юзернеймов собеседников; в память попадают только обобщённые вопросы и ваши ответы.", 13f))
        GgtgUi.addCard(col, head, top = 12f)

        col.addView(GgtgAiUi.section(context, "Аккаунты"))
        val state = GgtgAiTrain.current()
        val slots = GgtgAiTrain.accountsToLearn()
        if (slots.isEmpty()) {
            GgtgUi.addCard(col, GgtgUi.card(context).also { it.addView(GgtgUi.body(context, "Нет подключённых аккаунтов.")) })
        } else {
            // The accounts in their folders, like in the mailing: a folder is one row, a tap unfolds it.
            val picker = app.ggtg.GgtgAccountPicker(context)
            picker.keepSelection = true
            picker.setSelected(slots.filter { UserConfig.getInstance(it).clientUserId in state.accounts })
            val count = GgtgUi.body(context, "", 13f)
            val sync = {
                for (slot in slots) state.accounts.remove(UserConfig.getInstance(slot).clientUserId)
                for (slot in picker.selectedSlots()) state.accounts.add(UserConfig.getInstance(slot).clientUserId)
                GgtgAiTrain.saveState()
                count.text = "Выбрано аккаунтов: " + picker.selectedCount()
            }
            picker.onSelectionChanged = { sync() }
            col.addView(picker, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply { topMargin = GgtgUi.dp(6f) })
            val line = GgtgUi.card(context)
            line.addView(count)
            line.addView(GgtgAiUi.links(context,
                GgtgAiUi.link("Выбрать все") { picker.selectAll(true) },
                GgtgAiUi.link("Снять выбор") { picker.selectAll(false) }))
            GgtgUi.addCard(col, line)
            count.text = "Выбрано аккаунтов: " + picker.selectedCount()
        }

        col.addView(GgtgAiUi.section(context, "Ход"))
        val status = GgtgUi.card(context)
        status.addView(GgtgUi.body(context, if (GgtgAiTrain.running) "Идёт: " + GgtgAiTrain.status.ifEmpty { "работаю..." } else message.ifEmpty { GgtgAiTrain.summary() }, 13f))
        GgtgUi.addCard(col, status)
        GgtgUi.addCard(col, GgtgAiUi.row(context, GgtgAiUi.iconBadge(context, R.drawable.msg_send), "Начать или продолжить обучение", "", {
            message = GgtgAiTrain.start() ?: ""
            rebuild()
        }, chevron = false))
        GgtgUi.addCard(col, GgtgAiUi.row(context, GgtgAiUi.iconBadge(context, R.drawable.msg_cancel), "Остановить", "", {
            GgtgAiTrain.stopRun(); message = "Останавливаю..."; rebuild()
        }, chevron = false))
        GgtgUi.addCard(col, GgtgAiUi.row(context, GgtgAiUi.iconBadge(context, R.drawable.msg_retry), "Прочитать все чаты заново", "Выученное остаётся", {
            if (GgtgAiTrain.running) message = "Сначала остановите обучение." else GgtgAiUi.confirm(context, "Все чаты снова станут непрочитанными. Выученное останется.", "Начать заново") {
                GgtgAiTrain.startOver(); message = ""; rebuild()
            }
            rebuild()
        }, chevron = false))
        GgtgUi.addCard(col, GgtgAiUi.row(context, GgtgAiUi.iconBadge(context, R.drawable.msg_delete), "Забыть всё выученное из чатов", "Вопросы, возражения, ответы и прогресс", {
            if (GgtgAiTrain.running) { message = "Сначала остановите обучение."; rebuild() }
            else GgtgAiUi.confirm(context, "Удалить вопросы, возражения и ответы, выученные из чатов, и прогресс обучения? Правила стиля и «что работает» в вашем профиле останутся: их можно убрать там.", "Забыть") {
                CoroutineScope(SupervisorJob() + Dispatchers.IO).launch { GgtgAiTrain.forget(); org.telegram.messenger.AndroidUtilities.runOnUIThread { message = ""; rebuild() } }
            }
        }, chevron = false))
        col.addView(GgtgAiUi.gap(context, 24f))
    }
}
