package app.ggtg

import android.content.Context
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.RectF
import android.graphics.Typeface
import android.os.Handler
import android.os.Looper
import android.view.Gravity
import android.view.View
import android.widget.FrameLayout
import android.widget.HorizontalScrollView
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import android.widget.Toast
import org.telegram.messenger.AndroidUtilities
import org.telegram.messenger.R
import org.telegram.ui.ActionBar.ActionBar.ActionBarMenuOnItemClick
import org.telegram.ui.ActionBar.BaseFragment
import org.telegram.ui.ActionBar.Theme

// ng: the live window of a mailing - the same screen as the desktop app's progress window (see
// SmsRun): a status card with the colored progress line, six outcome tiles that filter the list,
// every chosen account with its state and counters, and the recipients with equal refusals folded.
class GgtgSmsRunActivity : BaseFragment() {

    private var run: SmsRun? = null
    private var filter = SmsFilter.All
    private var showAllAccounts = false
    private val expanded = HashSet<Int>()
    private var column: LinearLayout? = null
    private var scroll: ScrollView? = null
    private var recipientsLabel: View? = null
    private val handler = Handler(Looper.getMainLooper())
    private var pending = false
    private val listener: () -> Unit = { handler.post { schedule() }; Unit }

    override fun createView(context: Context): View {
        actionBar.setBackButtonImage(R.drawable.ic_ab_back)
        actionBar.setAllowOverlayTitle(true)
        actionBar.setTitle("Рассылка")
        actionBar.setActionBarMenuOnItemClick(object : ActionBarMenuOnItemClick() {
            override fun onItemClick(id: Int) { if (id == -1) finishFragment() }
        })
        val frame = FrameLayout(context)
        frame.setBackgroundColor(Theme.getColor(Theme.key_windowBackgroundGray))
        fragmentView = frame
        column = GgtgUi.page(context, frame)
        scroll = column?.parent as? ScrollView
        run = SmsRun.current
        rebuild()
        return frame
    }

    override fun onResume() {
        super.onResume()
        run = SmsRun.current
        run?.addListener(listener)
        rebuild()
    }

    override fun onPause() {
        super.onPause()
        run?.removeListener(listener)
    }

    // Many lanes report many times a second; the window redraws a few times a second at most.
    private fun schedule() {
        if (pending) return
        pending = true
        handler.postDelayed({ pending = false; rebuild() }, 300)
    }

    private fun dp(v: Float) = GgtgUi.dp(v)
    private val gray get() = Theme.getColor(Theme.key_windowBackgroundWhiteGrayText)
    private val black get() = Theme.getColor(Theme.key_windowBackgroundWhiteBlackText)

    private fun rebuild() {
        val col = column ?: return
        val context = col.context
        val keepY = scroll?.scrollY ?: 0
        col.removeAllViews()
        val r = run
        if (r == null) {
            col.addView(TextView(context).apply {
                text = "Рассылка ещё не запускалась."
                gravity = Gravity.CENTER
                setTextColor(gray)
                setPadding(dp(24f), dp(40f), dp(24f), 0)
            })
            return
        }
        val wrap = LinearLayout.LayoutParams.WRAP_CONTENT
        val match = LinearLayout.LayoutParams.MATCH_PARENT

        // 1. status
        val status = GgtgUi.card(context)
        val head = LinearLayout(context).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
        head.addView(GgtgUi.title(context, r.phase.title), LinearLayout.LayoutParams(0, wrap, 1f))
        head.addView(GgtgUi.title(context, "${r.processed()} / ${r.goal}", 20f))
        status.addView(head)
        val sub = if (r.finished) r.finishLine() else {
            val eta = r.etaSeconds()
            (if (r.lanes == 1) "1 поток" else "${r.lanes} потока") + if (eta > 0) " · осталось " + SmsRun.durationText(eta) else ""
        }
        status.addView(GgtgUi.body(context, sub, 13f), LinearLayout.LayoutParams(match, wrap).apply { topMargin = dp(4f) })
        if (r.earlier > 0) status.addView(GgtgUi.body(context, "Пропущены заранее (уже писали, боты): ${r.earlier}", 13f))
        status.addView(SegmentBar(context, r.counts.copyOf(), r.goal), LinearLayout.LayoutParams(match, dp(6f)).apply { topMargin = dp(10f) })
        GgtgUi.addCard(col, status, 10f)

        // 2. six tiles, two rows of three; a tap filters the list below
        val outcomes = SmsOutcome.values()
        for (rowStart in 0 until outcomes.size step 3) {
            val row = LinearLayout(context).apply { orientation = LinearLayout.HORIZONTAL }
            for (k in rowStart until rowStart + 3) {
                val o = outcomes[k]
                val selected = filter != SmsFilter.All && SmsFilter.of(o) == filter
                val tile = LinearLayout(context).apply {
                    orientation = LinearLayout.VERTICAL
                    background = GgtgUi.rounded(Theme.getColor(Theme.key_windowBackgroundWhite), dp(10f).toFloat(),
                        if (selected) dp(1f) else 0, o.color)
                    setPadding(dp(10f), dp(8f), dp(10f), dp(8f))
                    setOnClickListener {
                        val f = SmsFilter.of(o)
                        filter = if (filter == f) SmsFilter.All else f
                        rebuild()
                        scroll?.post { recipientsLabel?.let { v -> scroll?.smoothScrollTo(0, v.top) } }
                    }
                }
                val n = r.counts[o.ordinal]
                tile.addView(TextView(context).apply {
                    text = n.toString(); textSize = 20f; setTypeface(null, Typeface.BOLD)
                    setTextColor(if (n > 0) o.color else gray)
                })
                tile.addView(GgtgUi.body(context, o.tile, 12f))
                row.addView(tile, LinearLayout.LayoutParams(0, wrap, 1f).apply { marginEnd = if (k % 3 == 2) 0 else dp(6f) })
            }
            col.addView(row, LinearLayout.LayoutParams(match, wrap).apply { setMargins(dp(12f), dp(6f), dp(12f), 0) })
        }

        // 3. accounts: name, phone and @username (tells apart equal names), state, reason, counters
        col.addView(GgtgUi.sectionLabel(context, r.accountsHeadline()))
        val accounts = GgtgUi.card(context)
        val sorted = r.sortedAccounts()
        val shown = if (showAllAccounts) sorted else sorted.take(8)
        for (a in shown) accounts.addView(accountRow(context, r, a),
            LinearLayout.LayoutParams(match, wrap).apply { topMargin = dp(6f) })
        if (shown.size < sorted.size) {
            accounts.addView(GgtgUi.body(context, "ещё ${sorted.size - shown.size} аккаунтов ›", 13f).apply {
                setTextColor(Theme.getColor(Theme.key_windowBackgroundWhiteBlueText4))
                setPadding(0, dp(10f), 0, dp(2f))
                setOnClickListener { showAllAccounts = true; rebuild() }
            })
        }
        GgtgUi.addCard(col, accounts)

        // 4. recipients: filter chips and the list (equal refusals of one account fold into a row)
        val label = GgtgUi.sectionLabel(context, "Получатели")
        recipientsLabel = label
        col.addView(label)
        val chips = LinearLayout(context).apply { orientation = LinearLayout.HORIZONTAL; setPadding(dp(12f), dp(4f), dp(12f), dp(4f)) }
        for (f in SmsFilter.values()) {
            val count = r.counts.indices.filter { f.matches(SmsOutcome.values()[it]) }.sumOf { r.counts[it] }
            if (count == 0 && f != SmsFilter.All) continue
            val selected = f == filter
            chips.addView(TextView(context).apply {
                text = "${f.title} $count"; textSize = 13f
                setTypeface(null, if (selected) Typeface.BOLD else Typeface.NORMAL)
                setTextColor(if (selected) GgtgUi.accent else black)
                background = GgtgUi.rounded(if (selected) (GgtgUi.accent and 0x00FFFFFF) or 0x26000000 else 0,
                    dp(16f).toFloat(), if (selected) 0 else dp(1f), Theme.getColor(Theme.key_divider))
                setPadding(dp(12f), dp(6f), dp(12f), dp(6f))
                setOnClickListener { filter = f; rebuild() }
            }, LinearLayout.LayoutParams(wrap, wrap).apply { marginEnd = dp(6f) })
        }
        col.addView(HorizontalScrollView(context).apply { isHorizontalScrollBarEnabled = false; addView(chips) })

        val list = GgtgUi.card(context)
        val rows = r.rows(filter)
        if (rows.isEmpty()) list.addView(GgtgUi.body(context, "Пока никого: первые получатели появятся здесь.", 13f))
        for (row in rows) {
            list.addView(recipientRow(context, r, row), LinearLayout.LayoutParams(match, wrap).apply { topMargin = dp(4f) })
            if (row.indices.size > 1 && row.indices[0] in expanded) {
                for (i in row.indices) {
                    list.addView(recipientRow(context, r, SmsRow(listOf(i), row.outcome, row.account, false)),
                        LinearLayout.LayoutParams(match, wrap).apply { topMargin = dp(2f); marginStart = dp(18f) })
                }
            }
        }
        GgtgUi.addCard(col, list)

        // 5. stop (while the mailing runs)
        if (!r.finished) {
            GgtgUi.addButton(col, GgtgUi.button(context, "Остановить", destructive = true) {
                GgtgSms.cancelled = true
                Toast.makeText(context, "Останавливаю…", Toast.LENGTH_SHORT).show()
            }, top = 12f)
        }
        col.addView(GgtgUi.spacer(context))
        scroll?.post { scroll?.scrollTo(0, keepY) }
    }

    private fun accountRow(context: Context, r: SmsRun, a: SmsAccount): View {
        val color = when (a.state) {
            SmsAccountState.Working -> GgtgUi.accent
            SmsAccountState.Done -> SmsOutcome.Sent.color
            SmsAccountState.Retired -> SmsOutcome.Peer.color
            SmsAccountState.SpamBlocked -> SmsOutcome.Spam.color
            SmsAccountState.Waiting -> gray
        }
        val row = LinearLayout(context).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
        row.addView(TextView(context).apply {
            text = a.state.title; textSize = 12f; setTextColor(color); setTypeface(null, Typeface.BOLD)
            gravity = Gravity.CENTER
            background = GgtgUi.rounded((color and 0x00FFFFFF) or 0x29000000, dp(12f).toFloat())
            setPadding(dp(8f), dp(3f), dp(8f), dp(3f))
            minWidth = dp(78f)
        })
        val texts = LinearLayout(context).apply { orientation = LinearLayout.VERTICAL }
        texts.addView(GgtgUi.title(context, a.label, 14f))
        if (a.info.isNotEmpty()) texts.addView(GgtgUi.body(context, a.info, 12f))
        val detail = r.liveFor(a.slot).ifEmpty { a.note }
        if (detail.isNotEmpty()) texts.addView(GgtgUi.body(context, detail, 12f))
        row.addView(texts, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f).apply { setMargins(dp(10f), 0, dp(8f), 0) })
        row.addView(GgtgUi.body(context, a.counters(), 12f))
        row.isClickable = true
        row.setOnClickListener { presentFragment(GgtgSmsAccountLogActivity(a.slot)) }
        row.setOnLongClickListener {
            GgtgUi.menu(context, a.label, listOf(
                GgtgUi.MenuItem("Журнал действий", R.drawable.msg_log) { presentFragment(GgtgSmsAccountLogActivity(a.slot)) },
                GgtgUi.MenuItem("Копировать название", R.drawable.msg_copy) {
                    AndroidUtilities.addToClipboard(a.label)
                    android.widget.Toast.makeText(context, "Скопировано", android.widget.Toast.LENGTH_SHORT).show()
                }))
            true
        }
        return row
    }

    private fun recipientRow(context: Context, r: SmsRun, row: SmsRow): View {
        val folded = row.indices.size > 1
        val o = row.outcome
        val color = o?.color ?: GgtgUi.accent
        val line = LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL
            if (folded) background = GgtgUi.rounded((color and 0x00FFFFFF) or 0x1A000000, dp(8f).toFloat())
            setPadding(dp(8f), dp(6f), dp(8f), dp(6f))
        }
        // a colored dot: filled for an outcome, a ring while the recipient is being written to
        line.addView(View(context).apply {
            background = if (row.inProgress) GgtgUi.rounded(0, dp(5f).toFloat(), dp(2f), color) else GgtgUi.rounded(color, dp(5f).toFloat())
        }, LinearLayout.LayoutParams(dp(10f), dp(10f)).apply { marginEnd = dp(10f) })
        val first = r.recipients[row.indices[0]]
        val title = if (folded) "${o?.title} × ${row.indices.size} подряд" else "@${first.username}"
        line.addView(TextView(context).apply {
            text = title; textSize = 14f; setTextColor(if (folded) color else black)
            setTypeface(null, if (folded) Typeface.BOLD else Typeface.NORMAL)
        }, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
        val detail = when {
            folded -> row.account + if (row.indices[0] in expanded) "  ▴" else "  ▾"
            row.inProgress -> "в работе · ${row.account}"
            else -> listOfNotNull(
                o?.takeIf { it != SmsOutcome.Sent }?.let { first.reason.ifEmpty { it.inline } },
                row.account.takeIf { it.isNotEmpty() }).joinToString(" · ")
        }
        line.addView(GgtgUi.body(context, detail, 12f).apply { maxLines = 2; gravity = Gravity.END },
            LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f).apply { marginStart = dp(8f) })
        if (folded) line.setOnClickListener {
            val key = row.indices[0]
            if (!expanded.remove(key)) expanded.add(key)
            rebuild()
        }
        return line
    }

    /** The progress line split into the outcomes' colors over a muted track. */
    private class SegmentBar(context: Context, val counts: IntArray, val goal: Int) : View(context) {
        private val paint = Paint(Paint.ANTI_ALIAS_FLAG)
        override fun onDraw(canvas: Canvas) {
            val w = width.toFloat(); val h = height.toFloat()
            val r = h / 2
            paint.color = 0x1F8E8E93
            canvas.drawRoundRect(RectF(0f, 0f, w, h), r, r, paint)
            val total = maxOf(goal, counts.sum(), 1)
            canvas.save()
            canvas.clipPath(android.graphics.Path().apply { addRoundRect(RectF(0f, 0f, w, h), r, r, android.graphics.Path.Direction.CW) })
            var left = 0f
            for (o in SmsOutcome.values()) {
                val n = counts[o.ordinal]
                if (n == 0) continue
                val part = w * n / total
                paint.color = o.color
                canvas.drawRect(left, 0f, left + part, h, paint)
                left += part
            }
            canvas.restore()
        }
    }
}
