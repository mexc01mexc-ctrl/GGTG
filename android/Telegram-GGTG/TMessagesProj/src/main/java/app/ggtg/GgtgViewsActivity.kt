package app.ggtg

import android.content.Context
import android.text.InputType
import android.view.Gravity
import android.view.View
import android.widget.EditText
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import android.widget.Toast
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.telegram.messenger.AndroidUtilities
import org.telegram.messenger.R
import org.telegram.ui.ActionBar.ActionBar.ActionBarMenuOnItemClick
import org.telegram.ui.ActionBar.BaseFragment
import org.telegram.ui.ActionBar.Theme
import org.telegram.ui.Cells.HeaderCell
import org.telegram.ui.Cells.ShadowSectionCell
import org.telegram.ui.Cells.TextCell
import org.telegram.ui.Components.LayoutHelper

// ng: "Накрутка" - views (increment) for post links and joining channels/groups, spread over the
// chosen accounts (GgtgViews). Progress in "Процессы". Same settings-screen cells as the rest of
// GGTG (HeaderCell / ShadowSectionCell, white-background fields) instead of bare EditText floating
// on the grey background.
class GgtgViewsActivity : BaseFragment() {

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main)
    private var picker: GgtgAccountPicker? = null
    private lateinit var viewLinks: EditText
    private lateinit var joinLinks: EditText
    private lateinit var threads: EditText

    override fun createView(context: Context): View {
        actionBar.setBackButtonImage(R.drawable.ic_ab_back)
        actionBar.setAllowOverlayTitle(true)
        actionBar.setTitle("Накрутка")
        actionBar.setActionBarMenuOnItemClick(object : ActionBarMenuOnItemClick() {
            override fun onItemClick(id: Int) { if (id == -1) finishFragment() }
        })

        val frame = FrameLayout(context)
        frame.setBackgroundColor(Theme.getColor(Theme.key_windowBackgroundGray))
        fragmentView = frame
        val root = GgtgUi.page(context, frame)

        root.addView(GgtgUi.sectionLabel(context, "Аккаунты"))
        val accounts = GgtgUi.card(context)
        val p = GgtgAccountPicker(context)
        p.setBackgroundColor(android.graphics.Color.TRANSPARENT)
        accounts.addView(p, LinearLayout.LayoutParams(LayoutHelper.MATCH_PARENT, LayoutHelper.WRAP_CONTENT))
        GgtgUi.addCard(root, accounts)
        picker = p

        root.addView(GgtgUi.sectionLabel(context, "Просмотры"))
        val views = GgtgUi.card(context)
        viewLinks = GgtgUi.input(context, "t.me/channel/123 или t.me/channel/120-140", multi = true, minLines = 3)
        GgtgUi.labeled(context, views, "Ссылки на посты", "Можно диапазон: t.me/channel/120-140", viewLinks)
        GgtgUi.addCard(root, views)
        GgtgUi.addButton(root, GgtgUi.button(context, "Накрутить просмотры") { start(join = false) })

        root.addView(GgtgUi.sectionLabel(context, "Вступление"))
        val join = GgtgUi.card(context)
        joinLinks = GgtgUi.input(context, "@channel, t.me/group, t.me/+invite", multi = true, minLines = 3)
        GgtgUi.labeled(context, join, "Ссылки на каналы и группы", "По одной в строке", joinLinks)
        GgtgUi.addCard(root, join)
        GgtgUi.addButton(root, GgtgUi.button(context, "Вступить") { start(join = true) })

        root.addView(GgtgUi.sectionLabel(context, "Параметры"))
        val params = GgtgUi.card(context)
        threads = GgtgUi.input(context, "1", "1", numeric = true)
        GgtgUi.labeled(context, params, "Потоков", "Сколько аккаунтов работает одновременно", threads)
        GgtgUi.addCard(root, params)
        GgtgUi.addButton(root, GgtgUi.button(context, "Остановить", destructive = true) { GgtgViews.cancelled = true; toast("Останавливаю…") })
        root.addView(GgtgUi.spacer(context))
        return frame
    }

    private fun start(join: Boolean) {
        val slots = picker?.selectedSlots() ?: emptyList()
        if (slots.isEmpty()) { toast("Выберите аккаунты"); return }
        val targets = if (join) GgtgViews.parseJoinLinks(joinLinks.text.toString())
        else GgtgViews.parseViewLinks(viewLinks.text.toString())
        if (targets.isEmpty()) { toast("Не найдено корректных ссылок"); return }
        val t = threads.text.toString().toIntOrNull() ?: 1
        toast("Запущено — смотрите «Процессы»")
        scope.launch { withContext(Dispatchers.IO) { GgtgViews.run(slots, targets, t, join) } }
    }

    private fun toast(text: String) { context?.let { Toast.makeText(it, text, Toast.LENGTH_LONG).show() } }
}
