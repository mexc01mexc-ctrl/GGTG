package app.ggtg.ai

import android.content.Context
import android.view.View
import android.widget.LinearLayout
import android.widget.Toast
import app.ggtg.GgtgUi
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.launch
import org.telegram.messenger.AndroidUtilities
import org.telegram.messenger.R
import org.telegram.ui.ActionBar.BaseFragment

// ggtg: "Память ИИ": everything the AI knows in one screen, like the desktop's ShowAiMemoryBox: what it
// learned about the user (a long press on a fact forgets it), the materials, what it took from the chats,
// and forgetting.
class GgtgAiMemoryActivity : BaseFragment() {

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main)
    private var column: LinearLayout? = null

    override fun createView(context: Context): View {
        val (frame, col) = GgtgAiUi.page(this, context, "Память ИИ")
        column = col
        rebuild()
        return frame
    }

    override fun onResume() { super.onResume(); rebuild() }
    override fun onFragmentDestroy() { scope.cancel(); super.onFragmentDestroy() }

    private fun rebuild() {
        val col = column ?: return
        val context = col.context
        col.removeAllViews()

        col.addView(GgtgAiUi.section(context, "Что ИИ знает о вас"))
        val facts = GgtgUi.card(context)
        var any = false
        for (section in GgtgAiProfile.Section.values()) {
            val entries = GgtgAiProfile.entries(section)
            if (entries.isEmpty()) continue
            any = true
            facts.addView(GgtgUi.body(context, section.title, 12f), LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT).apply { topMargin = GgtgUi.dp(6f) })
            for (e in entries) {
                facts.addView(GgtgUi.title(context, "• " + e.text, 14f).apply {
                    setTypeface(null, android.graphics.Typeface.NORMAL)
                    setPadding(0, GgtgUi.dp(4f), 0, GgtgUi.dp(4f))
                    setOnLongClickListener {
                        GgtgUi.menu(context, null, listOf(
                            GgtgUi.MenuItem("Копировать", R.drawable.msg_copy) {
                                AndroidUtilities.addToClipboard(e.text)
                                Toast.makeText(context, "Скопировано", Toast.LENGTH_SHORT).show()
                            },
                            GgtgUi.MenuItem("Забыть это", R.drawable.msg_delete) { GgtgAiProfile.remove(section, e.text); rebuild() }))
                        true
                    }
                })
            }
        }
        if (!any) facts.addView(GgtgUi.body(context, "Пока ничего. Расскажите о себе в чате или нажмите там «Расскажу о себе»: ИИ расспросит и запомнит."))
        else facts.addView(GgtgUi.body(context, "Долгое нажатие на факт - забыть его.", 12f), LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply { topMargin = GgtgUi.dp(8f) })
        facts.addView(GgtgAiUi.links(context, GgtgAiUi.link("Изменить вручную") { presentFragment(GgtgAiProfileActivity()) }))
        GgtgUi.addCard(col, facts)

        col.addView(GgtgAiUi.section(context, "Знания"))
        val materials = GgtgAiKnowledge.list()
        GgtgUi.addCard(col, GgtgAiUi.row(context, GgtgAiUi.iconBadge(context, R.drawable.msg_folders), "Материалы",
            if (materials.isEmpty()) "Прайсы, сайты, PDF, видео: ИИ опирается на них в ответах"
            else "${materials.size} источников, кусков: ${materials.sumOf { it.chunks.size }}") { presentFragment(GgtgAiMaterialsActivity()) })
        GgtgUi.addCard(col, GgtgAiUi.row(context, GgtgAiUi.iconBadge(context, R.drawable.msg_download), "Выучено из ваших чатов",
            GgtgAiTrain.summary()) { presentFragment(GgtgAiTrainActivity()) })

        col.addView(GgtgAiUi.section(context, "Забыть"))
        val forget = GgtgUi.card(context)
        forget.addView(GgtgAiUi.links(context,
            GgtgAiUi.danger("Выученное из чатов") {
                GgtgAiUi.confirm(context, "Удалить вопросы, возражения и ответы, выученные из чатов, и прогресс обучения?", "Забыть") {
                    scope.launch { GgtgAiTrain.forget(); rebuild() }
                }
            },
            GgtgAiUi.danger("Все материалы") {
                GgtgAiUi.confirm(context, "Убрать все материалы (файлы, ссылки, тексты)?", "Убрать") {
                    GgtgAiKnowledge.list().forEach { GgtgAiKnowledge.remove(it.id) }; rebuild()
                }
            },
            GgtgAiUi.danger("Профиль целиком") {
                GgtgAiUi.confirm(context, "Стереть всё, что ИИ знает о вас? Это нельзя отменить.", "Стереть") {
                    for (section in GgtgAiProfile.Section.values()) GgtgAiProfile.entries(section).forEach { GgtgAiProfile.remove(section, it.text) }
                    rebuild()
                }
            }))
        GgtgUi.addCard(col, forget)
        col.addView(GgtgAiUi.gap(context, 24f))
    }
}
