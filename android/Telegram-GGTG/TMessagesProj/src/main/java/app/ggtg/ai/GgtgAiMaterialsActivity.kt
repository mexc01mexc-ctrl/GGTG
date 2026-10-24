package app.ggtg.ai

import android.content.Context
import android.content.Intent
import android.view.View
import android.widget.LinearLayout
import app.ggtg.GgtgUi
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.launch
import org.telegram.messenger.R
import org.telegram.ui.ActionBar.BaseFragment

// ggtg: step 3 of the "ИИ" screen: "Материалы", one knowledge base. It takes PDF, Word, text files, pictures
// (the text on them is read), links to sites and YouTube (by the subtitles, or Gemini watches the
// video itself). Everything is cut into chunks, and the ones that fit the conversation are put in
// front of the model with every reply. Texts go to a cloud model to be read; the base itself
// stays on this phone.
class GgtgAiMaterialsActivity : BaseFragment() {

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main)
    private var column: LinearLayout? = null
    private var status = ""
    private var busy = false

    override fun createView(context: Context): View {
        val (frame, col) = GgtgAiUi.page(this, context, "3. Материалы")
        column = col
        rebuild()
        return frame
    }

    override fun onFragmentDestroy() { scope.cancel(); super.onFragmentDestroy() }

    private fun rebuild() {
        val col = column ?: return
        val context = col.context
        col.removeAllViews()
        val head = GgtgUi.card(context)
        head.addView(GgtgUi.body(context, "Одна база для всего, о чём идёт разговор: презентации, прайсы, описания, FAQ. Принимает PDF, Word, txt " +
            "и другие документы, картинки (текст на них читается), ссылки на сайты и YouTube (по субтитрам, а если их нет, видео смотрит Gemini). " +
            "Всё режется на смысловые куски, и в каждый ответ подтягиваются те, что подходят к разговору.", 13f))
        GgtgUi.addCard(col, head, top = 12f)

        GgtgUi.addCard(col, GgtgAiUi.row(context, GgtgAiUi.iconBadge(context, R.drawable.msg_folders), "Добавить файлы или картинки", "PDF, docx, txt, pptx, фото") {
            if (busy) return@row
            val intent = Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
                addCategory(Intent.CATEGORY_OPENABLE)
                type = "*/*"
                putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true)
            }
            startActivityForResult(intent, PICK)
        })
        GgtgUi.addCard(col, GgtgAiUi.row(context, GgtgAiUi.iconBadge(context, R.drawable.msg_link), "Добавить ссылку", "Сайт, PDF или YouTube") {
            if (busy) return@row
            GgtgAiUi.prompt(context, "Добавить ссылку", "Ссылки, по одной в строке", multi = true, ok = "Добавить") { text ->
                val links = text.split('\n').map { it.trim() }.filter { it.isNotEmpty() }
                if (links.isNotEmpty()) run(links.map { link -> job { progress -> GgtgAiKnowledge.addUrl(link, progress) } to link })
            }
        })
        GgtgUi.addCard(col, GgtgAiUi.row(context, GgtgAiUi.iconBadge(context, R.drawable.msg_edit), "Добавить текст", "Вставить или написать самому") {
            if (busy) return@row
            GgtgAiUi.prompt(context, "Добавить текст", "Текст (прайс, FAQ, описание)", multi = true, ok = "Добавить") { text ->
                if (text.isNotEmpty()) {
                    val name = text.lineSequence().first().take(40).ifEmpty { "Текст" }
                    run(listOf(job { progress -> GgtgAiKnowledge.addText(name, text, progress) } to name))
                }
            }
        })

        if (status.isNotEmpty()) GgtgUi.addCard(col, GgtgUi.card(context).also { it.addView(GgtgUi.body(context, status, 13f)) })

        col.addView(GgtgAiUi.section(context, "В базе"))
        val sources = GgtgAiKnowledge.list()
        if (sources.isEmpty()) GgtgUi.addCard(col, GgtgUi.card(context).also { it.addView(GgtgUi.body(context, "Пока ничего не добавлено.")) })
        for (src in sources) {
            val card = GgtgUi.card(context)
            card.addView(GgtgUi.title(context, src.name, 15f))
            card.addView(GgtgUi.body(context, GgtgAiKnowledge.kindTitle(src.kind) + " · кусков: " + src.chunks.size, 13f))
            card.addView(GgtgAiUi.links(context, GgtgAiUi.danger("Убрать") {
                GgtgAiKnowledge.remove(src.id); rebuild()
            }))
            GgtgUi.addCard(col, card)
        }
        col.addView(GgtgAiUi.gap(context, 24f))
    }

    private fun job(f: suspend ((String) -> Unit) -> String?) = f

    // Sources are added one after another; each reports into the status line, the problems are shown together at the end.
    private fun run(jobs: List<Pair<suspend ((String) -> Unit) -> String?, String>>) {
        busy = true
        scope.launch {
            val problems = ArrayList<String>()
            for ((job, name) in jobs) {
                val error = try { job { text -> status = text; rebuild() } } catch (t: Throwable) { t.message ?: "ошибка" }
                if (error != null) problems.add("$name: $error")
            }
            busy = false
            status = if (problems.isEmpty()) "Готово: база пополнена." else "Готово, но не всё:\n" + problems.joinToString("\n")
            rebuild()
        }
    }

    override fun onActivityResultFragment(requestCode: Int, resultCode: Int, data: Intent?) {
        if (requestCode != PICK || resultCode != android.app.Activity.RESULT_OK || data == null) return
        val context = context ?: return
        val uris = ArrayList<android.net.Uri>()
        data.data?.let { uris.add(it) }
        data.clipData?.let { clip -> for (i in 0 until clip.itemCount) uris.add(clip.getItemAt(i).uri) }
        if (uris.isEmpty()) return
        run(uris.map { uri -> job { progress -> GgtgAiKnowledge.addFile(context, uri, progress) } to (uri.lastPathSegment ?: "файл") })
    }

    private companion object { const val PICK = 7311 }
}
