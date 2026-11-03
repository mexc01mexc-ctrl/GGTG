package app.ggtg.ai

import android.content.Context
import android.view.Gravity
import android.view.View
import android.widget.EditText
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import app.ggtg.GgtgUi
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.launch
import org.telegram.messenger.AndroidUtilities
import org.telegram.messenger.R
import org.telegram.ui.ActionBar.ActionBar.ActionBarMenuOnItemClick
import org.telegram.ui.ActionBar.BaseFragment
import org.telegram.ui.ActionBar.Theme
import org.telegram.ui.Components.LayoutHelper

// ggtg: "Спросить свой второй мозг" (and, with [interview], "ИИ расспросит меня"): one chat with the
// agent. It sees the profile, the materials, the feed of answers, what was learned from the chats;
// in the interview it asks the user one question at a time and writes the answers into the profile.
class GgtgAiBrainActivity(private val interview: Boolean) : BaseFragment() {

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main)
    private val turns = ArrayList<GgtgAiBrain.Turn>()
    private val steps = ArrayList<String>()
    private var list: LinearLayout? = null
    private var scroll: ScrollView? = null
    private var input: EditText? = null
    private var thinking = false

    override fun createView(context: Context): View {
        actionBar.setBackButtonImage(R.drawable.ic_ab_back)
        actionBar.setAllowOverlayTitle(true)
        actionBar.setTitle(if (interview) "ИИ расспросит меня" else "ИИ")
        if (!interview) {
            // three doors, always in the same place: what it knows, how it is set up, a clean page
            val menu = actionBar.createMenu()
            menu.addItem(2, R.drawable.msg_info)
            menu.addItem(1, R.drawable.msg_settings)
            val more = menu.addItem(9, R.drawable.ic_ab_other)
            more.addSubItem(3, R.drawable.msg_edit, "Новый чат")
            more.addSubItem(4, R.drawable.msg_delete, "Забыть…")
        }
        actionBar.setActionBarMenuOnItemClick(object : ActionBarMenuOnItemClick() {
            override fun onItemClick(id: Int) {
                when (id) {
                    -1 -> finishFragment()
                    1 -> presentFragment(GgtgAiActivity())
                    2 -> presentFragment(GgtgAiMemoryActivity())
                    3 -> newChat()
                    4 -> forgetMenu(context)
                }
            }
        })
        val frame = FrameLayout(context)
        frame.setBackgroundColor(Theme.getColor(Theme.key_windowBackgroundGray))
        fragmentView = frame
        val root = LinearLayout(context).apply { orientation = LinearLayout.VERTICAL }
        val sv = ScrollView(context)
        val col = LinearLayout(context).apply { orientation = LinearLayout.VERTICAL; setPadding(0, 0, 0, GgtgUi.dp(8f)) }
        sv.addView(col, LayoutHelper.createScroll(LayoutHelper.MATCH_PARENT, LayoutHelper.WRAP_CONTENT, Gravity.TOP))
        root.addView(sv, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f))
        val bar = LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setBackgroundColor(Theme.getColor(Theme.key_windowBackgroundWhite))
            setPadding(GgtgUi.dp(10f), GgtgUi.dp(6f), GgtgUi.dp(6f), GgtgUi.dp(6f))
        }
        // "+": everything that is done rather than asked, next to the field
        if (!interview) bar.addView(GgtgAiUi.icon(context, R.drawable.msg_add, GgtgUi.accent).apply {
            setOnClickListener { plusMenu(context) }
        }, LinearLayout.LayoutParams(GgtgUi.dp(44f), GgtgUi.dp(48f)))
        val field = GgtgAiUi.field(context, if (interview) "Ваш ответ" else "Спросите что угодно", "", true, 1).apply { maxLines = 5 }
        bar.addView(field, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
        bar.addView(GgtgAiUi.icon(context, R.drawable.msg_send, GgtgUi.accent).apply {
            setOnClickListener { send() }
        }, LinearLayout.LayoutParams(GgtgUi.dp(48f), GgtgUi.dp(48f)))
        root.addView(bar, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT))
        frame.addView(root, LayoutHelper.createFrame(LayoutHelper.MATCH_PARENT, LayoutHelper.MATCH_PARENT.toFloat()))
        list = col
        scroll = sv
        input = field
        if (interview) {
            // the model starts: it asks the first question
            turns.add(GgtgAiBrain.Turn("user", "Начни: задай мне первый вопрос."))
            ask(hidden = true)
        } else {
            // the top: is it working, what it knows; connecting a service when no key works yet
            header = GgtgUi.card(context)
            GgtgUi.addCard(col, header!!, top = 10f)
            refreshHeader()
            if (!GgtgAi.ready()) {
                connectCard = GgtgAiConnect.build(context, scope) {
                    refreshHeader()
                    connectCard?.visibility = View.GONE
                }
                GgtgUi.addCard(col, connectCard!!)
            }
            firstMessage = col.childCount
            addSuggestions(context)
        }
        return frame
    }

    override fun onResume() {
        super.onResume()
        refreshHeader()
        AndroidUtilities.requestAdjustResize(parentActivity, classGuid)
    }

    override fun onPause() {
        super.onPause()
        AndroidUtilities.removeAdjustResize(parentActivity, classGuid)
    }

    override fun onFragmentDestroy() { scope.cancel(); super.onFragmentDestroy() }

    private fun add(mine: Boolean, text: String) {
        val col = list ?: return
        val context = col.context
        val bubble = TextView(context).apply {
            this.text = text
            textSize = 15f
            setTextIsSelectable(true)
            setTextColor(if (mine) 0xFFFFFFFF.toInt() else Theme.getColor(Theme.key_windowBackgroundWhiteBlackText))
            background = GgtgUi.rounded(if (mine) GgtgUi.accent else Theme.getColor(Theme.key_windowBackgroundWhite), GgtgUi.dp(14f).toFloat())
            setPadding(GgtgUi.dp(12f), GgtgUi.dp(9f), GgtgUi.dp(12f), GgtgUi.dp(9f))
        }
        col.addView(bubble, LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply {
            gravity = if (mine) Gravity.END else Gravity.START
            topMargin = GgtgUi.dp(8f)
            leftMargin = GgtgUi.dp(if (mine) 48f else 12f)
            rightMargin = GgtgUi.dp(if (mine) 12f else 48f)
        })
        scroll?.post { scroll?.fullScroll(View.FOCUS_DOWN) }
    }

    private fun addStep(text: String) {
        val col = list ?: return
        col.addView(GgtgUi.body(col.context, text, 12f), LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply {
            leftMargin = GgtgUi.dp(16f); topMargin = GgtgUi.dp(4f)
        })
    }

    private var firstMessage = 0
    private var suggestions: View? = null

    /** A clean page: where to start, as cards in two columns. */
    private fun addSuggestions(context: Context) {
        val col = list ?: return
        val box = LinearLayout(context).apply { orientation = LinearLayout.VERTICAL }
        box.addView(GgtgUi.title(context, "Чем помочь?", 20f).apply { setPadding(GgtgUi.dp(6f), GgtgUi.dp(14f), 0, GgtgUi.dp(6f)) })
        class Item(val title: String, val sub: String, val go: () -> Unit)
        val items = listOf(
            Item("Расскажу о себе", "ИИ задаст пару вопросов о деле и запомнит") {
                sendText("Расспроси меня о моём деле: задавай по одному короткому вопросу и запоминай ответы.") },
            Item("Первое сообщение", "3 варианта текста для рассылки") {
                sendText("Предложи 3 разных варианта первого сообщения для моей аудитории, чтобы я проверил их в рассылке.") },
            Item("Ответ на возражение", "Вставьте, что написал человек") {
                input?.setText("Человек пишет: «»\nКак лучше ответить?"); input?.requestFocus(); input?.setSelection(15) },
            Item("Что ты знаешь обо мне?", "Коротко, по пунктам") {
                sendText("Расскажи, что ты уже знаешь обо мне и моём деле, по пунктам.") },
            Item("Дать материалы", "Прайс, сайт, PDF, видео") { presentFragment(GgtgAiMaterialsActivity()) },
            Item("Учиться на моих чатах", "Ваш стиль и ответы на частые вопросы") { presentFragment(GgtgAiTrainActivity()) }
        )
        for (pair in items.chunked(2)) {
            val row = LinearLayout(context).apply { orientation = LinearLayout.HORIZONTAL }
            pair.forEachIndexed { i, item ->
                val card = GgtgUi.card(context) { item.go() }
                card.addView(GgtgUi.title(context, item.title, 15f))
                card.addView(GgtgUi.body(context, item.sub, 12f), LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT,
                    LinearLayout.LayoutParams.WRAP_CONTENT).apply { topMargin = GgtgUi.dp(2f) })
                row.addView(card, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.MATCH_PARENT, 1f).apply {
                    if (i == 0) rightMargin = GgtgUi.dp(4f) else leftMargin = GgtgUi.dp(4f)
                })
            }
            box.addView(row, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply { topMargin = GgtgUi.dp(8f) })
        }
        suggestions = box
        col.addView(box, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply {
            leftMargin = GgtgUi.dp(12f); rightMargin = GgtgUi.dp(12f)
        })
    }

    /** A clean conversation. */
    private fun newChat() {
        val col = list ?: return
        job?.cancel(); job = null; thinking = false
        turns.clear()
        while (col.childCount > firstMessage) col.removeViewAt(col.childCount - 1)
        addSuggestions(col.context)
        refreshHeader()
    }

    private fun plusMenu(context: Context) {
        GgtgUi.menu(context, null, listOf(
            GgtgUi.MenuItem("Добавить материал", R.drawable.msg_folders) { presentFragment(GgtgAiMaterialsActivity()) },
            GgtgUi.MenuItem("Учиться на моих чатах", R.drawable.msg_download) { presentFragment(GgtgAiTrainActivity()) },
            GgtgUi.MenuItem("Запомнить правило", R.drawable.msg_edit) {
                input?.setText("Запомни: "); input?.requestFocus(); input?.setSelection(input?.text?.length ?: 0)
            },
            GgtgUi.MenuItem("Расспроси меня", R.drawable.msg_bots) {
                sendText("Расспроси меня о моём деле: задавай по одному короткому вопросу и запоминай ответы.")
            },
            GgtgUi.MenuItem("3 варианта первого сообщения", R.drawable.msg_send) {
                sendText("Предложи 3 разных варианта первого сообщения для моей аудитории, чтобы я проверил их в рассылке.")
            }))
    }

    private fun send() {
        val text = input?.text?.toString()?.trim().orEmpty()
        if (text.isEmpty()) return
        input?.setText("")
        sendText(text)
    }

    /** Throw things away, each with a question first. */
    private fun forgetMenu(context: Context) {
        GgtgUi.menu(context, "Что забыть", listOf(
            GgtgUi.MenuItem("Отдельные факты о вас…", R.drawable.msg_edit) { presentFragment(GgtgAiProfileActivity()) },
            GgtgUi.MenuItem("Всё выученное из чатов", R.drawable.msg_delete) {
                GgtgAiUi.confirm(context, "Удалить вопросы, возражения и ответы, выученные из чатов, и прогресс обучения?", "Забыть") {
                    scope.launch { GgtgAiTrain.forget() }
                }
            },
            GgtgUi.MenuItem("Все материалы", R.drawable.msg_delete) {
                GgtgAiUi.confirm(context, "Убрать все материалы (файлы, ссылки, тексты)?", "Убрать") {
                    GgtgAiKnowledge.list().forEach { GgtgAiKnowledge.remove(it.id) }
                }
            },
            GgtgUi.MenuItem("Профиль целиком", R.drawable.msg_delete) {
                GgtgAiUi.confirm(context, "Стереть профиль: дело, цель, аудиторию, стиль и правила? Это нельзя отменить.", "Стереть") {
                    for (section in GgtgAiProfile.Section.values()) GgtgAiProfile.entries(section).forEach { GgtgAiProfile.remove(section, it.text) }
                }
            }))
    }

    private var header: LinearLayout? = null
    private var connectCard: LinearLayout? = null

    private fun refreshHeader() {
        val card = header ?: return
        val context = card.context
        card.removeAllViews()
        val s = GgtgAi.settings()
        val services = s.keys.filter { GgtgAi.keyUsable(it) }.map { GgtgAi.providerName(it.provider) }.distinct()
        card.addView(GgtgUi.title(context, when {
            !s.enabled -> "ИИ выключен (включается в настройках)"
            services.isEmpty() -> "Нужен бесплатный ключ"
            else -> "Работает · " + services.joinToString(", ")
        }, 15f))
        card.addView(GgtgUi.body(context, "О вас: ${GgtgAiProfile.count()} · материалов: ${GgtgAiKnowledge.list().size} · ${GgtgAiTrain.summary()}", 12f),
            LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply { topMargin = GgtgUi.dp(2f) })
        // only while the AI is doing something
        if (thinking || GgtgAiTrain.running) {
            card.addView(TextView(context).apply {
                text = "■ Остановить ИИ"; textSize = 14f
                setTypeface(null, android.graphics.Typeface.BOLD); setTextColor(0xFFFFFFFF.toInt())
                setPadding(GgtgUi.dp(16f), GgtgUi.dp(8f), GgtgUi.dp(16f), GgtgUi.dp(8f))
                background = GgtgUi.rounded(GgtgUi.accent, GgtgUi.dp(16f).toFloat())
                setOnClickListener { stopAll() }
            }, LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply { topMargin = GgtgUi.dp(8f) })
        }
    }

    private fun sendText(text: String) {
        if (thinking) return
        suggestions?.let { list?.removeView(it) }
        suggestions = null
        // a key pasted into the field is connected here and never goes to a model; the chat keeps only its tail
        if (!text.contains(' ') && GgtgAi.detectProvider(text) != null) {
            val (key, error) = GgtgAiConnect.addKey(text, null)
            if (key == null) { add(false, error ?: "Не вышло"); return }
            val name = GgtgAi.providerName(GgtgAi.detectProvider(key) ?: "")
            add(true, "Ключ $name ${GgtgAi.shortKey(key)}")
            add(false, "Проверяю ключ $name...")
            scope.launch {
                val (ok, message) = GgtgAi.testKey(key)
                add(false, if (ok) "Подключено: $name. Теперь можно со мной говорить: расскажите о своём деле или нажмите «Расспроси меня»."
                    else "Ключ не подошёл: $message")
                refreshHeader()
                if (ok) connectCard?.visibility = View.GONE
            }
            return
        }
        add(true, text)
        turns.add(GgtgAiBrain.Turn("user", text))
        ask(false)
    }

    private var job: kotlinx.coroutines.Job? = null
    private var waitView: View? = null

    private fun ask(hidden: Boolean) {
        thinking = true
        refreshHeader()
        val wait = list?.let { GgtgUi.body(it.context, "Думаю...", 13f) }
        waitView = wait
        if (wait != null) list?.addView(wait, LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply { leftMargin = GgtgUi.dp(16f); topMargin = GgtgUi.dp(8f) })
        job = scope.launch {
            val (answer, error) = GgtgAiBrain.ask(turns, interview) { step -> AndroidUtilities.runOnUIThread { addStep(step) } }
            if (wait != null) list?.removeView(wait)
            thinking = false
            refreshHeader()
            if (error != null) { add(false, "Не вышло: $error"); return@launch }
            turns.add(GgtgAiBrain.Turn("assistant", answer.orEmpty()))
            add(false, answer.orEmpty())
        }
    }

    /** "Остановить ИИ": the answer being written and the training on the chats. */
    private fun stopAll() {
        val training = GgtgAiTrain.running
        job?.cancel()
        job = null
        waitView?.let { list?.removeView(it) }
        waitView = null
        thinking = false
        GgtgAiTrain.stopRun()
        add(false, "Остановил." + if (training) " Обучение на паузе, продолжить можно в «Учиться на чатах»." else "")
        refreshHeader()
    }
}
