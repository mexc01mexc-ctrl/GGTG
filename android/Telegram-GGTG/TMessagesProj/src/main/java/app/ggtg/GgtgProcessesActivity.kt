package app.ggtg

import android.content.Context
import android.graphics.Typeface
import android.view.Gravity
import android.view.View
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import org.telegram.messenger.AndroidUtilities
import org.telegram.messenger.R
import org.telegram.ui.ActionBar.ActionBar.ActionBarMenuOnItemClick
import org.telegram.ui.ActionBar.BaseFragment
import org.telegram.ui.ActionBar.Theme
import org.telegram.ui.Components.LayoutHelper

// ng: the "Процессы" screen - every long-running GGTG operation with its live log. Reads
// GgtgProcess and refreshes as handles log/finish.
class GgtgProcessesActivity : BaseFragment() {

    private var container: LinearLayout? = null
    private val expanded = HashSet<Long>()
    private val listener = { AndroidUtilities.runOnUIThread { rebuild() } }

    override fun createView(context: Context): View {
        actionBar.setBackButtonImage(R.drawable.ic_ab_back)
        actionBar.setAllowOverlayTitle(true)
        actionBar.setTitle("Процессы")
        actionBar.setActionBarMenuOnItemClick(object : ActionBarMenuOnItemClick() {
            override fun onItemClick(id: Int) {
                if (id == -1) finishFragment()
                else if (id == 1) { GgtgProcess.clearFinished(); rebuild() }
            }
        })
        val menu = actionBar.createMenu()
        menu.addItem(1, R.drawable.msg_clear_recent)

        val frame = FrameLayout(context)
        frame.setBackgroundColor(Theme.getColor(Theme.key_windowBackgroundGray))
        fragmentView = frame

        val scroll = ScrollView(context)
        val list = LinearLayout(context)
        list.orientation = LinearLayout.VERTICAL
        scroll.addView(list, LayoutHelper.createScroll(LayoutHelper.MATCH_PARENT, LayoutHelper.WRAP_CONTENT, Gravity.TOP))
        frame.addView(scroll, LayoutHelper.createFrame(LayoutHelper.MATCH_PARENT, LayoutHelper.MATCH_PARENT.toFloat()))
        container = list
        rebuild()
        return frame
    }

    override fun onResume() {
        super.onResume()
        GgtgProcess.addListener(listener)
        rebuild()
    }

    override fun onPause() {
        super.onPause()
        GgtgProcess.removeListener(listener)
    }

    private fun stopFor(kind: GgtgProcess.Kind) {
        when (kind) {
            GgtgProcess.Kind.Sms -> GgtgSms.cancelled = true
            GgtgProcess.Kind.Bulk -> GgtgBulk.cancelled = true
            GgtgProcess.Kind.Views -> GgtgViews.cancelled = true
            else -> {}
        }
    }

    private fun canStop(kind: GgtgProcess.Kind) =
        kind == GgtgProcess.Kind.Sms || kind == GgtgProcess.Kind.Bulk || kind == GgtgProcess.Kind.Views

    private fun rebuild() {
        val list = container ?: return
        val context = list.context
        list.removeAllViews()
        val snapshots = GgtgProcess.snapshots()
        if (snapshots.isEmpty()) {
            list.addView(TextView(context).apply {
                text = "Пока нет операций.\nРассылки, сборы и массовые действия появятся здесь."
                gravity = Gravity.CENTER
                setTextColor(Theme.getColor(Theme.key_windowBackgroundWhiteGrayText))
                setPadding(GgtgUi.dp(24f), GgtgUi.dp(40f), GgtgUi.dp(24f), 0)
            })
            return
        }
        for (s in snapshots) {
            val open = s.id in expanded
            val card = GgtgUi.card(context) { if (open) expanded.remove(s.id) else expanded.add(s.id); rebuild() }

            val (word, fg) = when (s.status) {
                GgtgProcess.Status.Running -> "идёт" to GgtgUi.accent
                GgtgProcess.Status.Succeeded -> "готово" to 0xFF2E9E5B.toInt()
                GgtgProcess.Status.Failed -> "ошибка" to Theme.getColor(Theme.key_text_RedRegular)
            }
            val head = LinearLayout(context).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
            head.addView(GgtgUi.title(context, s.title), LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
            head.addView(GgtgUi.pill(context, word, 0xFFFFFFFF.toInt(), fg),
                LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply { leftMargin = GgtgUi.dp(8f) })
            card.addView(head)

            val started = java.text.SimpleDateFormat("dd.MM HH:mm:ss", java.util.Locale.getDefault()).format(java.util.Date(s.startedAt * 1000))
            card.addView(GgtgUi.body(context, "начато $started" + if (s.progressTotal > 0) " · Обработано: ${s.progressCurrent} / ${s.progressTotal}" else ""),
                LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply { topMargin = GgtgUi.dp(2f) })

            if (s.progressTotal > 0) {
                val bar = android.widget.ProgressBar(context, null, android.R.attr.progressBarStyleHorizontal).apply {
                    max = s.progressTotal
                    progress = s.progressCurrent.coerceAtMost(s.progressTotal)
                    progressTintList = android.content.res.ColorStateList.valueOf(fg)
                }
                card.addView(bar, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, GgtgUi.dp(6f)).apply { topMargin = GgtgUi.dp(8f) })
            }

            // what each lane is doing right now, step by step (a running SMS campaign)
            if (s.status == GgtgProcess.Status.Running && s.live.isNotEmpty()) {
                val liveBox = LinearLayout(context).apply { orientation = LinearLayout.VERTICAL }
                for (line in s.live) liveBox.addView(GgtgUi.body(context, line, 13f).apply { setTextColor(fg) })
                card.addView(liveBox, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply { topMargin = GgtgUi.dp(8f) })
            }

            val lines = if (open) s.log else s.log.takeLast(3)
            if (lines.isEmpty()) {
                card.addView(GgtgUi.body(context, "Лог пока пуст."), LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply { topMargin = GgtgUi.dp(8f) })
            } else {
                val logBox = LinearLayout(context).apply {
                    orientation = LinearLayout.VERTICAL
                    background = GgtgUi.rounded(Theme.getColor(Theme.key_windowBackgroundGray), GgtgUi.dp(10f).toFloat())
                    setPadding(GgtgUi.dp(10f), GgtgUi.dp(8f), GgtgUi.dp(10f), GgtgUi.dp(8f))
                }
                for (line in lines) logBox.addView(GgtgUi.body(context, line.text, 12f))
                card.addView(logBox, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply { topMargin = GgtgUi.dp(8f) })
            }
            if (!open && s.log.size > 3) {
                card.addView(GgtgUi.body(context, "Показать весь лог (${s.log.size}) ›").apply {
                    setTextColor(Theme.getColor(Theme.key_windowBackgroundWhiteBlueText4))
                }, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply { topMargin = GgtgUi.dp(6f) })
            }

            val actions = LinearLayout(context).apply { orientation = LinearLayout.HORIZONTAL }
            if (s.status == GgtgProcess.Status.Running && canStop(s.kind)) {
                actions.addView(GgtgUi.pill(context, "Остановить", 0xFFFFFFFF.toInt(), Theme.getColor(Theme.key_text_RedRegular)).apply {
                    textSize = 13f
                    setOnClickListener { stopFor(s.kind); rebuild() }
                }, LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply { rightMargin = GgtgUi.dp(8f) })
            }
            val handle = GgtgProcess.handle(s.id)
            if (handle?.reopen != null) {
                actions.addView(GgtgUi.pill(context, "Открыть", 0xFFFFFFFF.toInt(), GgtgUi.accent).apply {
                    textSize = 13f
                    setOnClickListener { handle.reopen?.invoke() }
                })
            }
            if (actions.childCount > 0) card.addView(actions, LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply { topMargin = GgtgUi.dp(10f) })
            GgtgUi.addCard(list, card)
        }
        list.addView(View(context), LinearLayout.LayoutParams(1, GgtgUi.dp(16f)))
    }
}
