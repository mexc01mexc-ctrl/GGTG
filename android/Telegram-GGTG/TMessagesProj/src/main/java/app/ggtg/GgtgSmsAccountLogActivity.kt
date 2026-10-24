package app.ggtg

import android.content.Context
import android.os.Handler
import android.os.Looper
import android.view.View
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import android.widget.Toast
import org.telegram.messenger.AndroidUtilities
import org.telegram.messenger.R
import org.telegram.ui.ActionBar.ActionBar.ActionBarMenuOnItemClick
import org.telegram.ui.ActionBar.BaseFragment
import org.telegram.ui.ActionBar.Theme

// ggtg: the whole log of one account of the mailing (a tap on its row in the live window): what it took,
// every step (search, profile, text), the result of each recipient, pauses, errors and why it was
// taken out. Newest first, follows the mailing while open, can be copied. Like the desktop's window.
class GgtgSmsAccountLogActivity(private val slot: Int) : BaseFragment() {

    private var column: LinearLayout? = null
    private val handler = Handler(Looper.getMainLooper())
    private var run: SmsRun? = null
    private val tick = object : Runnable {
        override fun run() { rebuild(); handler.postDelayed(this, 1000) }
    }

    override fun createView(context: Context): View {
        actionBar.setBackButtonImage(R.drawable.ic_ab_back)
        actionBar.setAllowOverlayTitle(true)
        actionBar.setTitle(GgtgAccounts.label(slot))
        actionBar.setActionBarMenuOnItemClick(object : ActionBarMenuOnItemClick() {
            override fun onItemClick(id: Int) {
                if (id == -1) finishFragment() else if (id == COPY) copy()
            }
        })
        actionBar.createMenu().addItem(COPY, R.drawable.msg_copy)
        val frame = FrameLayout(context)
        frame.setBackgroundColor(Theme.getColor(Theme.key_windowBackgroundGray))
        fragmentView = frame
        column = GgtgUi.page(context, frame)
        run = SmsRun.current
        rebuild()
        return frame
    }

    override fun onResume() { super.onResume(); handler.removeCallbacks(tick); handler.post(tick) }
    override fun onPause() { handler.removeCallbacks(tick); super.onPause() }
    override fun onFragmentDestroy() { handler.removeCallbacks(tick); super.onFragmentDestroy() }

    private fun copy() {
        val lines = run?.logOf(slot)?.asReversed() ?: return
        AndroidUtilities.addToClipboard(lines.joinToString("\n"))
        Toast.makeText(context, "Журнал скопирован", Toast.LENGTH_SHORT).show()
    }

    private fun rebuild() {
        val col = column ?: return
        val context = col.context
        val r = run ?: SmsRun.current.also { run = it }
        col.removeAllViews()
        val a = r?.accounts?.firstOrNull { it.slot == slot }
        if (r == null || a == null) {
            GgtgUi.addCard(col, GgtgUi.card(context).also { it.addView(GgtgUi.body(context, "Рассылки сейчас нет.")) }, top = 12f)
            return
        }
        val head = GgtgUi.card(context)
        head.addView(GgtgUi.title(context, a.state.title + " · " + a.counters(), 16f))
        if (a.info.isNotEmpty()) head.addView(GgtgUi.body(context, a.info, 13f))
        val live = r.liveFor(slot).ifEmpty { a.note }
        if (live.isNotEmpty()) head.addView(GgtgUi.body(context, live, 13f))
        GgtgUi.addCard(col, head, top = 12f)

        col.addView(GgtgUi.sectionLabel(context, "Журнал (новое сверху)"))
        val lines = r.logOf(slot)
        val card = GgtgUi.card(context)
        if (lines.isEmpty()) card.addView(GgtgUi.body(context, "Пока ничего не происходило: аккаунт ещё не начал работу."))
        for (line in lines.take(300)) {
            val failed = line.contains("✗") || line.contains("Снят") || line.contains("Спам-блок")
            card.addView(TextView(context).apply {
                text = line; textSize = 13f; setTextIsSelectable(true)
                setTextColor(if (failed) Theme.getColor(Theme.key_text_RedRegular) else Theme.getColor(Theme.key_windowBackgroundWhiteBlackText))
                setPadding(0, AndroidUtilities.dp(3f), 0, AndroidUtilities.dp(3f))
            })
        }
        GgtgUi.addCard(col, card)
        col.addView(GgtgUi.spacer(context))
    }

    private companion object { const val COPY = 1 }
}
