package app.ggtg.ai

import android.content.Context
import android.text.Editable
import android.text.TextWatcher
import android.view.Gravity
import android.view.View
import android.widget.LinearLayout
import android.widget.TextView
import app.ggtg.GgtgUi
import kotlinx.coroutines.launch
import org.telegram.messenger.R
import org.telegram.ui.ActionBar.BaseFragment
import org.telegram.ui.ActionBar.Theme

// ggtg: step 2 of the "ИИ" screen: the business and the goal. The user's own description (who they are, what the
// talk is for, how to answer) is the first thing every reply is written from; the profile is what the
// AI has learned and keeps in sections. "Проверить, что ИИ выучил" shows what is new since the last
// look, each with a way to remove it: a mistake the AI learned would otherwise spread into every reply.
class GgtgAiProfileActivity : BaseFragment() {

    private var column: LinearLayout? = null
    private var prompt: android.widget.EditText? = null

    private val template = "Кто я: \nЧем занимаюсь: \nЦель диалога (к чему вести собеседника): \n" +
        "Как общаться (стиль, обращение на «ты» или «вы»): \nЧто можно говорить (цены, условия, ссылки): \nЧего нельзя: "

    override fun createView(context: Context): View {
        val (frame, col) = GgtgAiUi.page(this, context, "2. О бизнесе и цель")
        column = col
        rebuild()
        return frame
    }

    private fun rebuild() {
        val col = column ?: return
        val context = col.context
        val keep = prompt?.text?.toString()
        col.removeAllViews()
        val s = GgtgAi.settings()

        col.addView(GgtgAiUi.section(context, "Опишите себя и цель диалога"))
        val card = GgtgUi.card(context)
        card.addView(GgtgUi.body(context, "Кто вы, чем занимаетесь, к чему нужно вести собеседника, как общаться и что можно называть. " +
            "Помощник пишет от вашего имени и подводит разговор к этой цели. Учитываются первые 2000 символов: самое важное пишите в начале.", 13f))
        val field = GgtgAiUi.field(context, "Ваше описание", keep ?: s.chatPrompt, true, 6)
        field.addTextChangedListener(object : TextWatcher {
            override fun beforeTextChanged(p0: CharSequence?, p1: Int, p2: Int, p3: Int) {}
            override fun onTextChanged(p0: CharSequence?, p1: Int, p2: Int, p3: Int) {}
            override fun afterTextChanged(p0: Editable?) { s.chatPrompt = p0?.toString().orEmpty().trim(); GgtgAi.save() }
        })
        prompt = field
        card.addView(field, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply { topMargin = GgtgUi.dp(8f) })
        card.addView(GgtgAiUi.links(context, GgtgAiUi.link("Вставить шаблон для заполнения") {
            if (field.text.toString().isBlank()) field.setText(template)
            field.requestFocus()
        }))
        GgtgUi.addCard(col, card, top = 8f)

        col.addView(GgtgAiUi.section(context, "Расскажите ИИ о себе в разговоре"))
        GgtgUi.addCard(col, GgtgAiUi.row(context, GgtgAiUi.iconBadge(context, R.drawable.msg_help), "ИИ расспросит меня",
            "Он задаст вопросы по одному и сам запишет ответы в профиль.") { presentFragment(GgtgAiBrainActivity(true)) })

        // what is new since the last look
        val since = GgtgAiInbox.mem().lastReview
        val newFacts = GgtgAiProfile.newSince(since)
        val newEntries = GgtgAiPlaybook.since(since).filter { it.answers.isNotEmpty() }
        col.addView(GgtgAiUi.section(context, "Проверить, что ИИ выучил" + if (newFacts.size + newEntries.size > 0) " (новое: ${newFacts.size + newEntries.size})" else ""))
        val review = GgtgUi.card(context)
        review.addView(GgtgUi.body(context, if (newFacts.isEmpty() && newEntries.isEmpty()) "С прошлой проверки ничего нового."
        else "Новое с прошлой проверки. Уберите то, что неверно или устарело: иначе ИИ будет повторять это в ответах.", 13f))
        for ((section, entry) in newFacts) {
            review.addView(factRow(context, section.title + ": " + entry.text) {
                GgtgAiProfile.remove(section, entry.text); rebuild()
            }, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply { topMargin = GgtgUi.dp(6f) })
        }
        for (e in newEntries) {
            review.addView(factRow(context, GgtgAiPlaybook.chunk(e)) {
                GgtgAiPlaybook.remove(e.what)
                kotlinx.coroutines.CoroutineScope(kotlinx.coroutines.Dispatchers.IO).launch { GgtgAiPlaybook.syncKnowledge() }
                rebuild()
            }, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply { topMargin = GgtgUi.dp(6f) })
        }
        if (newFacts.isNotEmpty() || newEntries.isNotEmpty()) {
            GgtgUi.addCard(col, review)
            GgtgUi.addButton(col, GgtgUi.button(context, "Всё верно") { GgtgAiInbox.markReviewed(); rebuild() })
        } else GgtgUi.addCard(col, review)

        // the whole profile
        col.addView(GgtgAiUi.section(context, "Профиль: что ИИ знает о вас"))
        var any = false
        for (section in GgtgAiProfile.Section.values()) {
            val entries = GgtgAiProfile.entries(section)
            if (entries.isEmpty()) continue
            any = true
            val c = GgtgUi.card(context)
            c.addView(GgtgUi.title(context, section.title, 14f))
            for (e in entries.asReversed()) {
                c.addView(factRow(context, e.text) { GgtgAiProfile.remove(section, e.text); rebuild() },
                    LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply { topMargin = GgtgUi.dp(4f) })
            }
            GgtgUi.addCard(col, c)
        }
        if (!any) GgtgUi.addCard(col, GgtgUi.card(context).also {
            it.addView(GgtgUi.body(context, "Профиль пока пуст. Он пополняется разговорами с ИИ, правками ваших черновиков и обучением на чатах."))
        })
        col.addView(GgtgAiUi.gap(context, 24f))
    }

    private fun factRow(context: Context, text: String, onRemove: () -> Unit): View {
        val line = LinearLayout(context).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
        line.addView(TextView(context).apply {
            this.text = text
            textSize = 14f
            setTextColor(Theme.getColor(Theme.key_windowBackgroundWhiteBlackText))
        }, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
        line.addView(GgtgAiUi.icon(context, R.drawable.msg_close, Theme.getColor(Theme.key_windowBackgroundWhiteGrayIcon)).apply {
            setOnClickListener { onRemove() }
        }, LinearLayout.LayoutParams(GgtgUi.dp(36f), GgtgUi.dp(36f)))
        return line
    }
}
