package app.ggtg.ai

import android.content.Context
import android.view.View
import android.widget.LinearLayout
import android.widget.TextView
import app.ggtg.GgtgUi
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.launch
import org.telegram.messenger.AndroidUtilities
import org.telegram.messenger.browser.Browser
import org.telegram.ui.ActionBar.Theme

// ggtg: connecting a free AI service, inside the chat with the AI: the services as rows, a tap shows how
// to get its key, a button to the site and the field for the key. Like AiConnect on the desktop.
object GgtgAiConnect {

    fun stepsFor(id: String): String = when (id) {
        "gemini" -> "1. Откройте сайт и войдите в аккаунт Google.\n2. Нажмите «Create API key» и скопируйте ключ (начинается с AIza).\n3. Вставьте его ниже."
        "groq" -> "1. Откройте сайт, войдите через Google или почту.\n2. «Create API Key», скопируйте ключ (начинается с gsk_).\n3. Вставьте его ниже."
        "openrouter" -> "1. Откройте сайт и войдите.\n2. «Create Key», скопируйте ключ (начинается с sk-or-).\n3. Вставьте его ниже."
        "nvidia" -> "1. Откройте сайт, войдите и подтвердите телефон.\n2. «Generate API Key», скопируйте ключ (начинается с nvapi-).\n3. Вставьте его ниже."
        "llm7" -> "Регистрация не нужна: просто нажмите «Подключить»."
        else -> ""
    }

    fun stepsSpannable(context: Context, provider: GgtgAi.Provider): CharSequence {
        val raw = stepsFor(provider.id)
        val target = "Откройте сайт"
        val idx = raw.indexOf(target)
        if (idx < 0 || provider.keyUrl.isEmpty()) return raw
        val span = android.text.SpannableStringBuilder(raw)
        val clickable = object : android.text.style.ClickableSpan() {
            override fun onClick(widget: View) {
                Browser.openUrl(context, provider.keyUrl)
            }
            override fun updateDrawState(ds: android.text.TextPaint) {
                super.updateDrawState(ds)
                ds.color = GgtgUi.accent
                ds.isUnderlineText = true
            }
        }
        span.setSpan(clickable, idx, idx + target.length, android.text.Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
        return span
    }

    /** Puts a pasted key into the settings: (the key, null) or (null, what is wrong). */
    fun addKey(raw: String, hint: String?): Pair<String?, String?> {
        val key = raw.replace(Regex("\\s+"), "").removePrefix("Bearer").removePrefix("bearer").replace("\"", "").replace("'", "")
        if (key.isEmpty()) return null to "Сначала вставьте ключ."
        val provider = GgtgAi.detectProvider(key) ?: hint
            ?: return null to "Не узнал, от какого сервиса ключ: выберите сервис выше и вставьте ключ ещё раз."
        val s = GgtgAi.settings()
        if (s.keys.none { it.key == key }) {
            s.keys.add(GgtgAi.CloudKey(key, provider)); s.enabled = true; GgtgAi.save()
        }
        return key to null
    }

    /** The block itself; [connected] is called when a key has answered. */
    fun build(context: Context, scope: CoroutineScope, connected: () -> Unit): LinearLayout {
        val card = GgtgUi.card(context)
        card.addView(GgtgUi.title(context, "Подключите бесплатный ИИ", 16f))
        card.addView(GgtgUi.body(context, "Это минута, карта не нужна. Лучше подключить два сервиса: когда у одного кончится дневной лимит, ответит " +
            "другой. Ключ можно и просто вставить в поле внизу."), LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT,
            LinearLayout.LayoutParams.WRAP_CONTENT).apply { topMargin = GgtgUi.dp(4f) })
        var open: String? = null
        val rows = LinearLayout(context).apply { orientation = LinearLayout.VERTICAL }
        card.addView(rows, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply { topMargin = GgtgUi.dp(6f) })
        lateinit var rebuild: () -> Unit
        rebuild = {
            rows.removeAllViews()
            for (p in GgtgAi.providers) {
                val row = LinearLayout(context).apply {
                    orientation = LinearLayout.VERTICAL
                    setPadding(0, GgtgUi.dp(8f), 0, GgtgUi.dp(8f))
                    setOnClickListener { open = if (open == p.id) null else p.id; rebuild() }
                }
                row.addView(TextView(context).apply {
                    text = (if (open == p.id) "▾ " else "▸ ") + p.name; textSize = 15f
                    setTypeface(null, android.graphics.Typeface.BOLD); setTextColor(GgtgUi.accent)
                })
                row.addView(GgtgUi.body(context, p.note, 12f))
                rows.addView(row)
                if (open != p.id) continue
                val box = LinearLayout(context).apply {
                    orientation = LinearLayout.VERTICAL
                    setPadding(GgtgUi.dp(12f), GgtgUi.dp(8f), GgtgUi.dp(12f), GgtgUi.dp(10f))
                    background = GgtgUi.rounded(Theme.getColor(Theme.key_windowBackgroundGray), GgtgUi.dp(12f).toFloat())
                }
                val stepsView = GgtgUi.body(context, stepsSpannable(context, p), 13f).apply {
                    movementMethod = android.text.method.LinkMovementMethod.getInstance()
                }
                box.addView(stepsView)
                val result = GgtgUi.body(context, "", 13f)
                val check: (String) -> Unit = { key ->
                    result.text = "Проверяю ключ..."
                    scope.launch {
                        val (ok, message) = GgtgAi.testKey(key)
                        result.text = if (ok) "Подключено: ${p.name}. Можно говорить с ИИ." else "$message"
                        if (ok) connected()
                    }
                }
                if (p.id == "llm7") {
                    box.addView(GgtgAiUi.links(context, GgtgAiUi.link("Подключить") {
                        addKey("unused", "llm7").first?.let(check)
                    }))
                } else {
                    val field = GgtgAiUi.field(context, "Вставьте ключ ${p.name}")
                    box.addView(field, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply { topMargin = GgtgUi.dp(8f) })
                    val connect = {
                        val (key, error) = addKey(field.text.toString(), p.id)
                        if (error != null) result.text = error else { field.setText(""); check(key!!) }
                    }
                    val test = {
                        val raw = field.text.toString().replace(Regex("\\s+"), "").removePrefix("Bearer").removePrefix("bearer").replace("\"", "").replace("'", "")
                        if (raw.isEmpty()) {
                            result.text = "Сначала вставьте ключ."
                        } else {
                            result.text = "Проверяю ключ..."
                            scope.launch {
                                val (ok, message) = GgtgAi.testKey(raw, p.id)
                                result.text = if (ok) "Ключ работает: ${p.name}." else "$message"
                            }
                        }
                    }
                    box.addView(GgtgAiUi.links(context,
                        GgtgAiUi.link("Проверить") { test() },
                        GgtgAiUi.link("Добавить") { connect() }))
                }
                box.addView(result)
                rows.addView(box)
            }
        }
        rebuild()
        return card
    }
}
