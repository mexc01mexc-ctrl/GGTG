package app.ggtg

import android.content.Context
import android.content.res.ColorStateList
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.graphics.drawable.RippleDrawable
import android.view.Gravity
import android.view.View
import android.widget.LinearLayout
import android.widget.TextView
import org.telegram.messenger.AndroidUtilities
import org.telegram.ui.ActionBar.Theme
import org.telegram.ui.Components.LayoutHelper

// ng: the shared "premium" look of the GGTG screens - rounded white cards on the gray background,
// accent pills, wrapping text (maxLines without ellipsize, so nothing is ever cut to "…").
object GgtgUi {

    /** One line of a long-press menu (the desktop's right-click menu). */
    class MenuItem(val text: String, val icon: Int, val action: () -> Unit)

    /** A long press = the right click of the desktop: the secondary actions of a row in Telegram's own
     *  dialog with icons, the main action staying on a plain tap. */
    fun menu(context: Context, title: String?, items: List<MenuItem>) {
        if (items.isEmpty()) return
        org.telegram.ui.ActionBar.AlertDialog.Builder(context).apply {
            if (!title.isNullOrEmpty()) setTitle(title)
            setItems(items.map { it.text }.toTypedArray(), items.map { it.icon }.toIntArray()) { _, which -> items[which].action() }
            setNegativeButton("Отмена", null)
        }.show()
    }
    fun dp(v: Float) = AndroidUtilities.dp(v)
    fun color(key: Int) = Theme.getColor(key)
    val accent get() = color(Theme.key_featuredStickers_addButton)

    fun rounded(fill: Int, radius: Float, stroke: Int = 0, strokeColor: Int = 0) = GradientDrawable().apply {
        setColor(fill)
        cornerRadius = radius
        if (stroke > 0) setStroke(stroke, strokeColor)
    }

    fun pill(context: Context, text: String, textColor: Int, fill: Int) = TextView(context).apply {
        this.text = text
        textSize = 12f
        setTextColor(textColor)
        setTypeface(null, Typeface.BOLD)
        setPadding(dp(9f), dp(3f), dp(9f), dp(3f))
        background = rounded(fill, dp(10f).toFloat())
    }

    fun tintCheckBox(cb: android.widget.CheckBox) {
        val checked = Theme.getColor(Theme.key_windowBackgroundWhiteBlueHeader)
        val unchecked = if (Theme.isCurrentThemeDark()) 0xFF8E8E93.toInt() else 0xFF757575.toInt()
        cb.buttonTintList = ColorStateList(
            arrayOf(
                intArrayOf(android.R.attr.state_checked),
                intArrayOf()
            ),
            intArrayOf(checked, unchecked)
        )
    }

    fun checkBox(context: Context, checked: Boolean = false): android.widget.CheckBox =
        android.widget.CheckBox(context).apply {
            isChecked = checked
            tintCheckBox(this)
        }

    /** A vertical white card with a ripple when [onClick] is given. */
    fun card(context: Context, onClick: (() -> Unit)? = null): LinearLayout = LinearLayout(context).apply {
        orientation = LinearLayout.VERTICAL
        val base = rounded(color(Theme.key_windowBackgroundWhite), dp(14f).toFloat())
        background = if (onClick != null)
            RippleDrawable(ColorStateList.valueOf(color(Theme.key_listSelector)), base, rounded(0xFFFFFFFF.toInt(), dp(14f).toFloat()))
        else base
        setPadding(dp(14f), dp(12f), dp(14f), dp(12f))
        if (onClick != null) setOnClickListener { onClick() }
    }

    fun addCard(root: LinearLayout, card: View, top: Float = 6f) {
        root.addView(card, LayoutHelper.createLinear(LayoutHelper.MATCH_PARENT, LayoutHelper.WRAP_CONTENT, 12f, top, 12f, 0f))
    }

    fun sectionLabel(context: Context, text: String) = TextView(context).apply {
        this.text = text.uppercase()
        textSize = 12f
        setTypeface(null, Typeface.BOLD)
        setTextColor(color(Theme.key_windowBackgroundWhiteGrayText))
        setPadding(dp(18f), dp(16f), dp(18f), dp(2f))
    }

    fun title(context: Context, text: String, size: Float = 16f) = TextView(context).apply {
        this.text = text
        textSize = size
        setTypeface(null, Typeface.BOLD)
        setTextColor(color(Theme.key_windowBackgroundWhiteBlackText))
    }

    fun body(context: Context, text: CharSequence, size: Float = 13f) = TextView(context).apply {
        this.text = text
        textSize = size
        setTextColor(color(Theme.key_windowBackgroundWhiteGrayText))
    }

    // Telegram's own icons instead of emoji: every glyph the GGTG screens used to show as an emoji.
    private val EMOJI_ICONS = mapOf(
        "👥" to org.telegram.messenger.R.drawable.msg_groups,
        "👤" to org.telegram.messenger.R.drawable.msg_contacts,
        "💾" to org.telegram.messenger.R.drawable.msg_download,
        "🤖" to org.telegram.messenger.R.drawable.msg_bots,
        "📋" to org.telegram.messenger.R.drawable.msg_list,
        "📤" to org.telegram.messenger.R.drawable.msg_shareout,
        "📑" to org.telegram.messenger.R.drawable.msg_copy,
        "➖" to org.telegram.messenger.R.drawable.msg_remove,
        "➕" to org.telegram.messenger.R.drawable.msg_add,
        "✂" to org.telegram.messenger.R.drawable.msg_reorder,
        "🎲" to org.telegram.messenger.R.drawable.msg_select,
        "🗑" to org.telegram.messenger.R.drawable.msg_delete,
        "🌐" to org.telegram.messenger.R.drawable.msg_language,
        "🗂" to org.telegram.messenger.R.drawable.msg_folders,
        "📁" to org.telegram.messenger.R.drawable.msg_folders,
        "🗜" to org.telegram.messenger.R.drawable.msg_archive,
        "📦" to org.telegram.messenger.R.drawable.msg_archive,
        "📂" to org.telegram.messenger.R.drawable.msg_openin,
        "🦑" to org.telegram.messenger.R.drawable.msg_search,
        "🔒" to org.telegram.messenger.R.drawable.msg_secret,
        "⚠" to org.telegram.messenger.R.drawable.msg_warning,
        "📷" to org.telegram.messenger.R.drawable.msg_qrcode,
        "🔁" to org.telegram.messenger.R.drawable.msg_retry,
        "📥" to org.telegram.messenger.R.drawable.msg_download,
        "⬇" to org.telegram.messenger.R.drawable.msg_download,
        "🔑" to org.telegram.messenger.R.drawable.msg2_permissions,
        "📊" to org.telegram.messenger.R.drawable.msg_stats,
        "📱" to org.telegram.messenger.R.drawable.msg2_devices,
        "📞" to org.telegram.messenger.R.drawable.msg_calls,
        "✉" to org.telegram.messenger.R.drawable.msg_message,
        "📎" to org.telegram.messenger.R.drawable.msg_link,
        "🧠" to org.telegram.messenger.R.drawable.msg_bots,
        "📚" to org.telegram.messenger.R.drawable.msg_list,
        "🔥" to org.telegram.messenger.R.drawable.msg_stats,
        "✕" to org.telegram.messenger.R.drawable.msg_close,
        "✓" to org.telegram.messenger.R.drawable.msg_select,
        "ℹ" to org.telegram.messenger.R.drawable.msg_info,
        "💰" to org.telegram.messenger.R.drawable.msg_payment_card,
        "🔐" to org.telegram.messenger.R.drawable.msg_secret
    )

    /** The Telegram icon for a glyph (the emoji variation selector ignored); any glyph without its own icon gets the
     *  neutral info icon - an emoji is never shown in its place. */
    fun iconRes(glyph: String): Int? {
        val key = glyph.replace("\uFE0F", "").trim()
        return EMOJI_ICONS[key] ?: if (key.isNotEmpty() && key.codePointAt(0) >= 0x2190) org.telegram.messenger.R.drawable.msg_info else null
    }

    /** The glyph as a Telegram icon (a tinted ImageView), or the plain text when there is no icon for it. */
    fun glyphView(context: Context, glyph: String, sizeDp: Float, tint: Int): View {
        val res = iconRes(glyph)
        return if (res != null) android.widget.ImageView(context).apply {
            setImageDrawable(icon(context, res, tint))
            layoutParams = android.widget.FrameLayout.LayoutParams(dp(sizeDp), dp(sizeDp), Gravity.CENTER)
        } else if (glyph.isNotEmpty()) TextView(context).apply {
            text = glyph; textSize = sizeDp * 0.8f; gravity = Gravity.CENTER
        } else android.widget.ImageView(context).apply {
            setImageDrawable(icon(context, org.telegram.messenger.R.drawable.msg_info, tint))
            layoutParams = android.widget.FrameLayout.LayoutParams(dp(sizeDp), dp(sizeDp), Gravity.CENTER)
        }
    }

    /** Round chip with the glyph's Telegram icon in the middle. */
    fun circleIcon(context: Context, glyph: String, sizeDp: Float, fill: Int, tint: Int): android.widget.FrameLayout =
        android.widget.FrameLayout(context).apply {
            background = rounded(fill, dp(sizeDp / 2).toFloat())
            addView(glyphView(context, glyph, sizeDp * 0.5f, tint))
        }

    fun circleIcon(context: Context, res: Int, sizeDp: Float, fill: Int, tint: Int): android.widget.FrameLayout =
        android.widget.FrameLayout(context).apply {
            background = rounded(fill, dp(sizeDp / 2).toFloat())
            val iv = android.widget.ImageView(context).apply {
                setImageDrawable(icon(context, res, tint))
                layoutParams = android.widget.FrameLayout.LayoutParams(dp(sizeDp * 0.5f), dp(sizeDp * 0.5f), Gravity.CENTER)
            }
            addView(iv)
        }

    /** Round accent badge with the glyph's Telegram icon inside. */
    fun badge(context: Context, glyph: String, size: Float = 40f): View =
        circleIcon(context, glyph, size, (accent and 0x00FFFFFF) or 0x26000000, accent)

    fun badge(context: Context, res: Int, size: Float = 40f): View =
        circleIcon(context, res, size, (accent and 0x00FFFFFF) or 0x26000000, accent)

    /** [view]'s text led by a Telegram icon in [tint]. */
    fun withLeadingIcon(view: TextView, res: Int, tint: Int, sizeDp: Float = 15f, gapDp: Float = 6f): TextView = view.apply {
        val d = icon(context, res, tint)
        d.setBounds(0, 0, dp(sizeDp), dp(sizeDp))
        setCompoundDrawablesRelative(d, null, null, null)
        compoundDrawablePadding = dp(gapDp)
    }

    /** A Telegram icon from the app's own set, tinted. */
    fun icon(context: Context, res: Int, tint: Int): android.graphics.drawable.Drawable =
        context.resources.getDrawable(res).mutate().apply {
            colorFilter = android.graphics.PorterDuffColorFilter(tint, android.graphics.PorterDuff.Mode.MULTIPLY)
        }

    /** The «Корзина» icon (Telegram's trash can) set as the leading icon of a text view. */
    fun withTrashIcon(view: TextView, tint: Int, sizeDp: Float = 18f): TextView = view.apply {
        val d = icon(context, org.telegram.messenger.R.drawable.msg_delete, tint)
        d.setBounds(0, 0, dp(sizeDp), dp(sizeDp))
        setCompoundDrawablesRelative(d, null, null, null)
        compoundDrawablePadding = dp(8f)
    }

    private val MATCH = LinearLayout.LayoutParams.MATCH_PARENT
    private val WRAP = LinearLayout.LayoutParams.WRAP_CONTENT

    /** A text field in a soft rounded gray box (sits inside a card). */
    fun input(
        context: Context, hint: String, text: String = "", multi: Boolean = false, minLines: Int = 1,
        numeric: Boolean = false
    ) = android.widget.EditText(context).apply {
        this.hint = hint
        setText(text)
        textSize = 15f
        inputType = when {
            numeric -> android.text.InputType.TYPE_CLASS_NUMBER
            multi -> android.text.InputType.TYPE_CLASS_TEXT or android.text.InputType.TYPE_TEXT_FLAG_MULTI_LINE
            else -> android.text.InputType.TYPE_CLASS_TEXT or android.text.InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS
        }
        if (multi) { setMinLines(minLines); gravity = Gravity.TOP or Gravity.START } else setSingleLine()
        setTextColor(color(Theme.key_windowBackgroundWhiteBlackText))
        setHintTextColor(color(Theme.key_windowBackgroundWhiteHintText))
        background = rounded(color(Theme.key_windowBackgroundGray), dp(10f).toFloat())
        setPadding(dp(12f), dp(10f), dp(12f), dp(10f))
    }

    /** Adds `label` (wrapping, with an optional gray hint line) and the [input] under it to [card]. */
    fun labeled(context: Context, card: LinearLayout, label: String, sub: String?, input: View) {
        card.addView(title(context, label, 15f), LinearLayout.LayoutParams(MATCH, WRAP).apply { topMargin = dp(6f) })
        if (sub != null) card.addView(body(context, sub, 12f), LinearLayout.LayoutParams(MATCH, WRAP))
        card.addView(input, LinearLayout.LayoutParams(MATCH, WRAP).apply { topMargin = dp(6f) })
    }

    /** Full-width rounded button: accent by default, red when [destructive]. */
    fun button(context: Context, text: String, destructive: Boolean = false, onClick: () -> Unit) = TextView(context).apply {
        this.text = text
        textSize = 15f
        gravity = Gravity.CENTER
        setTypeface(null, Typeface.BOLD)
        setTextColor(0xFFFFFFFF.toInt())
        background = rounded(if (destructive) color(Theme.key_text_RedRegular) else accent, dp(12f).toFloat())
        setPadding(dp(14f), dp(13f), dp(14f), dp(13f))
        setOnClickListener { onClick() }
    }

    fun addButton(root: LinearLayout, button: View, top: Float = 10f) {
        root.addView(button, LayoutHelper.createLinear(LayoutHelper.MATCH_PARENT, LayoutHelper.WRAP_CONTENT, 12f, top, 12f, 0f))
    }

    /** A scrolling gray page; returns the column to fill. */
    fun page(context: Context, fragmentFrame: android.widget.FrameLayout): LinearLayout {
        val scroll = android.widget.ScrollView(context)
        val column = LinearLayout(context).apply { orientation = LinearLayout.VERTICAL }
        scroll.addView(column, LayoutHelper.createScroll(LayoutHelper.MATCH_PARENT, LayoutHelper.WRAP_CONTENT, Gravity.TOP))
        fragmentFrame.addView(scroll, LayoutHelper.createFrame(LayoutHelper.MATCH_PARENT, LayoutHelper.MATCH_PARENT.toFloat()))
        return column
    }

    fun spacer(context: Context, h: Float = 16f) = View(context).apply { minimumHeight = dp(h) }

    /** Info card: round badge + bold title + wrapping explanation. */
    fun infoCard(context: Context, glyph: String, title: String, text: String): LinearLayout {
        val card = card(context)
        val row = LinearLayout(context).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
        row.addView(badge(context, glyph), LinearLayout.LayoutParams(dp(40f), dp(40f)).apply { rightMargin = dp(12f) })
        row.addView(title(context, title), LinearLayout.LayoutParams(0, WRAP, 1f))
        card.addView(row)
        card.addView(body(context, text, 14f), LinearLayout.LayoutParams(MATCH, WRAP).apply { topMargin = dp(8f) })
        return card
    }

    fun infoCard(context: Context, res: Int, title: String, text: String): LinearLayout {
        val card = card(context)
        val row = LinearLayout(context).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
        row.addView(badge(context, res), LinearLayout.LayoutParams(dp(40f), dp(40f)).apply { rightMargin = dp(12f) })
        row.addView(title(context, title), LinearLayout.LayoutParams(0, WRAP, 1f))
        card.addView(row)
        card.addView(body(context, text, 14f), LinearLayout.LayoutParams(MATCH, WRAP).apply { topMargin = dp(8f) })
        return card
    }

    /** Tappable row card: badge + title + wrapping subtitle + chevron. */
    fun actionCard(context: Context, glyph: String, title: String, sub: String, onClick: () -> Unit): LinearLayout {
        val card = card(context, onClick)
        val row = LinearLayout(context).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
        row.addView(badge(context, glyph), LinearLayout.LayoutParams(dp(40f), dp(40f)).apply { rightMargin = dp(12f) })
        val texts = LinearLayout(context).apply { orientation = LinearLayout.VERTICAL }
        texts.addView(title(context, title))
        texts.addView(body(context, sub), LinearLayout.LayoutParams(MATCH, WRAP).apply { topMargin = dp(2f) })
        row.addView(texts, LinearLayout.LayoutParams(0, WRAP, 1f))
        row.addView(TextView(context).apply { text = "›"; textSize = 22f; setTextColor(color(Theme.key_windowBackgroundWhiteGrayText)) })
        card.addView(row)
        return card
    }

    fun actionCard(context: Context, res: Int, title: String, sub: String, onClick: () -> Unit): LinearLayout {
        val card = card(context, onClick)
        val row = LinearLayout(context).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
        row.addView(badge(context, res), LinearLayout.LayoutParams(dp(40f), dp(40f)).apply { rightMargin = dp(12f) })
        val texts = LinearLayout(context).apply { orientation = LinearLayout.VERTICAL }
        texts.addView(title(context, title))
        texts.addView(body(context, sub), LinearLayout.LayoutParams(MATCH, WRAP).apply { topMargin = dp(2f) })
        row.addView(texts, LinearLayout.LayoutParams(0, WRAP, 1f))
        row.addView(TextView(context).apply { text = "›"; textSize = 22f; setTextColor(color(Theme.key_windowBackgroundWhiteGrayText)) })
        card.addView(row)
        return card
    }
}
