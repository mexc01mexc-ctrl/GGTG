package app.ggtg.ai

import android.content.Context
import android.graphics.Typeface
import android.view.Gravity
import android.view.View
import android.widget.EditText
import android.widget.FrameLayout
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import android.widget.Toast
import app.ggtg.GgtgUi
import org.telegram.messenger.AndroidUtilities
import org.telegram.messenger.R
import org.telegram.ui.ActionBar.ActionBar.ActionBarMenuOnItemClick
import org.telegram.ui.ActionBar.AlertDialog
import org.telegram.ui.ActionBar.BaseFragment
import org.telegram.ui.ActionBar.Theme
import org.telegram.ui.Components.LayoutHelper
import org.telegram.ui.Components.Switch

// ggtg: the building blocks of the AI screens on the phone: a gray page with rounded cards that follow the
// theme (dark or light), a numbered step, a row with an icon, switches, themed dialogs. Everything
// takes its colors from Theme.getColor, so nothing is white on a dark theme.
object GgtgAiUi {

    private val MATCH = LinearLayout.LayoutParams.MATCH_PARENT
    private val WRAP = LinearLayout.LayoutParams.WRAP_CONTENT

    fun dp(v: Float) = AndroidUtilities.dp(v)

    /** Sets the action bar of [fragment] (back arrow, [title]) and returns the frame and the column to fill.
     *  [onMenu] gets the id of an action bar item the fragment added itself. */
    fun page(fragment: BaseFragment, context: Context, title: String, onMenu: ((Int) -> Unit)? = null): Pair<FrameLayout, LinearLayout> {
        fragment.actionBar.setBackButtonImage(R.drawable.ic_ab_back)
        fragment.actionBar.setAllowOverlayTitle(true)
        fragment.actionBar.setTitle(title)
        fragment.actionBar.setActionBarMenuOnItemClick(object : ActionBarMenuOnItemClick() {
            override fun onItemClick(id: Int) {
                if (id == -1) fragment.finishFragment() else onMenu?.invoke(id)
            }
        })
        val frame = FrameLayout(context)
        frame.setBackgroundColor(Theme.getColor(Theme.key_windowBackgroundGray))
        fragment.fragmentView = frame
        val scroll = ScrollView(context)
        val column = LinearLayout(context).apply { orientation = LinearLayout.VERTICAL }
        scroll.addView(column, LayoutHelper.createScroll(MATCH, WRAP, Gravity.TOP))
        frame.addView(scroll, LayoutHelper.createFrame(MATCH, MATCH.toFloat()))
        return frame to column
    }

    fun toast(context: Context?, text: String) { if (context != null) Toast.makeText(context, text, Toast.LENGTH_LONG).show() }

    fun icon(context: Context, res: Int, tint: Int): ImageView = ImageView(context).apply {
        setImageDrawable(GgtgUi.icon(context, res, tint))
        scaleType = ImageView.ScaleType.CENTER
    }

    /** A round accent badge with a Telegram icon. */
    fun iconBadge(context: Context, res: Int, size: Float = 40f): View = icon(context, res, GgtgUi.accent).apply {
        background = GgtgUi.rounded((GgtgUi.accent and 0x00FFFFFF) or 0x26000000, dp(size / 2).toFloat())
        layoutParams = LinearLayout.LayoutParams(dp(size), dp(size))
    }

    /** A round accent badge with the number of a step. */
    fun numberBadge(context: Context, number: Int, size: Float = 40f): View = TextView(context).apply {
        text = number.toString()
        textSize = 16f
        setTypeface(null, Typeface.BOLD)
        gravity = Gravity.CENTER
        setTextColor(0xFFFFFFFF.toInt())
        background = GgtgUi.rounded(GgtgUi.accent, dp(size / 2).toFloat())
        layoutParams = LinearLayout.LayoutParams(dp(size), dp(size))
    }

    fun row(context: Context, badge: View, title: String, sub: String, chevron: Boolean = true, onClick: () -> Unit): LinearLayout =
        row(context, badge, title, sub, onClick, chevron)

    private fun chevron(context: Context) = TextView(context).apply {
        text = "›"; textSize = 22f
        setTextColor(Theme.getColor(Theme.key_windowBackgroundWhiteGrayText))
    }

    /** A tappable card: [badge] on the left, a bold [title], a wrapping [sub] and a chevron. */
    fun row(context: Context, badge: View, title: String, sub: String, onClick: (() -> Unit)?, chevron: Boolean = true): LinearLayout {
        val card = GgtgUi.card(context, onClick)
        val line = LinearLayout(context).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
        (badge.layoutParams as? LinearLayout.LayoutParams)?.rightMargin = dp(12f)
        line.addView(badge)
        val texts = LinearLayout(context).apply { orientation = LinearLayout.VERTICAL }
        texts.addView(GgtgUi.title(context, title))
        if (sub.isNotEmpty()) texts.addView(GgtgUi.body(context, sub), LinearLayout.LayoutParams(MATCH, WRAP).apply { topMargin = dp(2f) })
        line.addView(texts, LinearLayout.LayoutParams(0, WRAP, 1f))
        if (chevron && onClick != null) line.addView(chevron(context), LinearLayout.LayoutParams(WRAP, WRAP).apply { leftMargin = dp(8f) })
        card.addView(line)
        return card
    }

    fun section(context: Context, text: String) = GgtgUi.sectionLabel(context, text)

    /** A switch row: a title, an explanation and the switch; [onChange] gets the new value. */
    fun switchRow(context: Context, title: String, sub: String, checked: Boolean, onChange: (Boolean) -> Unit): LinearLayout {
        val sw = Switch(context)
        sw.setColors(Theme.key_switchTrack, Theme.key_switchTrackChecked, Theme.key_windowBackgroundWhite, Theme.key_windowBackgroundWhite)
        sw.setChecked(checked, false)
        val card = GgtgUi.card(context) {
            val now = !sw.isChecked
            sw.setChecked(now, true)
            onChange(now)
        }
        val line = LinearLayout(context).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
        val texts = LinearLayout(context).apply { orientation = LinearLayout.VERTICAL }
        texts.addView(GgtgUi.title(context, title, 15f))
        if (sub.isNotEmpty()) texts.addView(GgtgUi.body(context, sub, 12f), LinearLayout.LayoutParams(MATCH, WRAP).apply { topMargin = dp(2f) })
        line.addView(texts, LinearLayout.LayoutParams(0, WRAP, 1f))
        line.addView(sw, LinearLayout.LayoutParams(dp(46f), dp(26f)).apply { leftMargin = dp(10f) })
        card.addView(line)
        return card
    }

    /** Children in a row that wraps onto the next line when it is full. */
    class Flow(context: Context) : android.view.ViewGroup(context) {
        override fun onMeasure(widthSpec: Int, heightSpec: Int) {
            val width = MeasureSpec.getSize(widthSpec)
            var x = 0; var y = 0; var rowHeight = 0
            for (i in 0 until childCount) {
                val c = getChildAt(i)
                if (c.visibility == GONE) continue
                c.measure(MeasureSpec.makeMeasureSpec(width, MeasureSpec.AT_MOST), MeasureSpec.makeMeasureSpec(0, MeasureSpec.UNSPECIFIED))
                if (x > 0 && x + c.measuredWidth > width) { x = 0; y += rowHeight; rowHeight = 0 }
                x += c.measuredWidth
                rowHeight = maxOf(rowHeight, c.measuredHeight)
            }
            setMeasuredDimension(width, y + rowHeight)
        }

        override fun onLayout(changed: Boolean, l: Int, t: Int, r: Int, b: Int) {
            val width = r - l
            var x = 0; var y = 0; var rowHeight = 0
            for (i in 0 until childCount) {
                val c = getChildAt(i)
                if (c.visibility == GONE) continue
                if (x > 0 && x + c.measuredWidth > width) { x = 0; y += rowHeight; rowHeight = 0 }
                c.layout(x, y, x + c.measuredWidth, y + c.measuredHeight)
                x += c.measuredWidth
                rowHeight = maxOf(rowHeight, c.measuredHeight)
            }
        }
    }

    /** Text links under a card's text ("Проверить", "Удалить"), wrapping onto more lines; destructive ones are red. */
    fun links(context: Context, vararg links: Triple<String, Boolean, () -> Unit>): Flow = Flow(context).apply {
        for ((text, destructive, action) in links) {
            addView(TextView(context).apply {
                this.text = text
                textSize = 14f
                setTypeface(null, Typeface.BOLD)
                setTextColor(if (destructive) Theme.getColor(Theme.key_text_RedRegular) else GgtgUi.accent)
                setPadding(0, dp(8f), dp(20f), dp(4f))
                setOnClickListener { action() }
            })
        }
    }

    fun link(text: String, action: () -> Unit) = Triple(text, false, action)
    fun danger(text: String, action: () -> Unit) = Triple(text, true, action)

    fun gap(context: Context, h: Float = 8f) = View(context).apply { minimumHeight = dp(h) }

    /** A multi-line (or single) text field that follows the theme. */
    fun field(context: Context, hint: String, text: String = "", multi: Boolean = false, minLines: Int = 1): EditText =
        GgtgUi.input(context, hint, text, multi, minLines)

    // ---- themed dialogs (the platform ones are white on a dark theme) ----

    fun confirm(context: Context, text: String, ok: String = "Удалить", onOk: () -> Unit) {
        AlertDialog.Builder(context).setMessage(text)
            .setPositiveButton(ok) { _, _ -> onOk() }
            .setNegativeButton("Отмена", null).show()
    }

    fun prompt(context: Context, title: String, hint: String, initial: String = "", multi: Boolean = false, ok: String = "Готово", onOk: (String) -> Unit) {
        val input = field(context, hint, initial, multi, if (multi) 4 else 1)
        val box = FrameLayout(context).apply { setPadding(dp(20f), dp(8f), dp(20f), 0); addView(input) }
        AlertDialog.Builder(context).setTitle(title).setView(box)
            .setPositiveButton(ok) { _, _ -> onOk(input.text.toString().trim()) }
            .setNegativeButton("Отмена", null).show()
    }

    /** A list of checkboxes in a themed dialog; [checked] is changed in place, [onOk] runs on the OK button. */
    fun multiChoice(context: Context, title: String, labels: List<CharSequence>, checked: BooleanArray, ok: String, onOk: () -> Unit) {
        val box = LinearLayout(context).apply { orientation = LinearLayout.VERTICAL; setPadding(dp(12f), dp(4f), dp(12f), 0) }
        labels.forEachIndexed { i, label ->
            box.addView(android.widget.CheckBox(context).apply {
                text = label; isChecked = checked[i]; textSize = 15f
                setTextColor(Theme.getColor(Theme.key_dialogTextBlack))
                setOnCheckedChangeListener { _, v -> checked[i] = v }
            }, LinearLayout.LayoutParams(MATCH, WRAP))
        }
        val scroll = ScrollView(context).apply { addView(box) }
        AlertDialog.Builder(context).setTitle(title).setView(scroll)
            .setPositiveButton(ok) { _, _ -> onOk() }
            .setNegativeButton("Отмена", null).show()
    }

    fun info(context: Context, title: String, text: String) {
        AlertDialog.Builder(context).setTitle(title).setMessage(text).setPositiveButton("Закрыть", null).show()
    }
}
