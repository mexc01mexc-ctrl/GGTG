package app.ggtg.ai

import android.content.Context
import android.view.View
import android.widget.LinearLayout
import android.widget.TextView
import app.ggtg.GgtgUi
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.launch
import org.telegram.messenger.AndroidUtilities
import org.telegram.messenger.R
import org.telegram.messenger.browser.Browser
import org.telegram.ui.ActionBar.BaseFragment

// ggtg: step 1 of the "ИИ" screen: the free keys. Paste a key and the provider is told by the key itself and
// checked at once; any number of keys, from any providers, they stand in for each other: when a
// model hits its limit it rests and the next one takes the job.
class GgtgAiKeysActivity : BaseFragment() {

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main)
    private var column: LinearLayout? = null
    private val results = HashMap<String, String>()
    private val addResults = HashMap<String, String>()
    private val drafts = HashMap<String, String>()
    private val fields = HashMap<String, android.widget.EditText>()

    private fun keyCard(context: Context, col: LinearLayout, s: GgtgAi.Settings, entry: GgtgAi.CloudKey) {
        val status = results[entry.key] ?: when {
            !GgtgAi.keyUsable(entry) -> "Сервис не узнан - этот ключ не используется."
            GgtgAi.keyResting(entry.key) -> "Отдыхает после ошибки или лимита (кнопка «Проверить» его разбудит)."
            else -> ""
        }
        val card = GgtgUi.card(context)
        card.addView(GgtgUi.title(context, GgtgAi.shortKey(entry.key), 15f))
        if (status.isNotEmpty()) card.addView(GgtgUi.body(context, status, 13f), LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply { topMargin = GgtgUi.dp(4f) })
        card.addView(GgtgAiUi.links(context,
            GgtgAiUi.link("Проверить") { check(entry.key) },
            GgtgAiUi.danger("Удалить") {
                GgtgAiUi.confirm(context, "Удалить ключ ${GgtgAi.shortKey(entry.key)}? На странице сервиса он останется рабочим.") {
                    s.keys.removeAll { it.key == entry.key }
                    results.remove(entry.key)
                    GgtgAi.save()
                    rebuild()
                }
            }))
        // long press: the actions of the key, like the right click on the desktop
        card.setOnLongClickListener {
            GgtgUi.menu(context, GgtgAi.shortKey(entry.key), listOf(
                GgtgUi.MenuItem("Проверить", R.drawable.msg_retry) { check(entry.key) },
                GgtgUi.MenuItem("Копировать ключ", R.drawable.msg_copy) {
                    AndroidUtilities.addToClipboard(entry.key)
                    android.widget.Toast.makeText(context, "Скопировано", android.widget.Toast.LENGTH_SHORT).show()
                },
                GgtgUi.MenuItem("Удалить", R.drawable.msg_delete) {
                    GgtgAiUi.confirm(context, "Удалить ключ ${GgtgAi.shortKey(entry.key)}? На странице сервиса он останется рабочим.") {
                        s.keys.removeAll { it.key == entry.key }
                        results.remove(entry.key)
                        GgtgAi.save()
                        rebuild()
                    }
                }))
            true
        }
        GgtgUi.addCard(col, card)
    }

    override fun createView(context: Context): View {
        val (frame, col) = GgtgAiUi.page(this, context, "1. Ключи")
        column = col
        rebuild()
        return frame
    }

    override fun onFragmentDestroy() { scope.cancel(); super.onFragmentDestroy() }

    private fun check(key: String) {
        results[key] = "Проверяю..."
        rebuild()
        scope.launch {
            val (ok, message) = GgtgAi.testKey(key)
            results[key] = (if (ok) "✓ " else "✗ ") + message
            rebuild()
        }
    }

    private fun addKey(providerId: String, field: android.widget.EditText) {
        val s = GgtgAi.settings()
        val key = field.text.toString().replace(Regex("\\s+"), "").removePrefix("Bearer").replace("\"", "").replace("'", "")
        if (key.isEmpty()) { addResults[providerId] = "Сначала вставьте ключ."; rebuild(); return }
        // The service the key really belongs to, when it says so itself.
        val provider = GgtgAi.detectProvider(key)?.takeIf { it != providerId } ?: providerId
        if (s.keys.any { it.key == key }) { addResults[providerId] = "Этот ключ уже есть."; rebuild(); return }
        s.keys.add(GgtgAi.CloudKey(key, provider)); s.enabled = true; GgtgAi.save()
        drafts.remove(providerId); fields.remove(providerId)
        addResults[providerId] = if (provider == providerId) "Ключ добавлен, проверяю."
            else "Этот ключ от ${GgtgAi.providerName(provider)}: добавил его в список этого сервиса."
        check(key)
    }

    private fun rebuild() {
        val col = column ?: return
        val context = col.context
        for ((id, f) in fields) drafts[id] = f.text.toString()
        fields.clear()
        col.removeAllViews()
        val s = GgtgAi.settings()

        val hint = GgtgUi.card(context)
        hint.addView(GgtgUi.body(context, "У каждого сервиса свой список ключей: вставьте ключ в поле своего сервиса. Ключей может быть сколько угодно, они " +
            "подменяют друг друга: когда у модели кончился лимит или она не отвечает, она «отдыхает», а работу сразу берёт следующая. " +
            "Ключи хранятся только на этом телефоне. Карта не нужна.", 13f))
        GgtgUi.addCard(col, hint, top = 12f)

        for (p in GgtgAi.providers) {
            col.addView(GgtgAiUi.section(context, p.name))
            val mine = s.keys.filter { it.provider == p.id }
            val card = GgtgUi.card(context)
            card.addView(GgtgUi.body(context, p.note, 13f))
            GgtgUi.addCard(col, card)
            if (mine.isEmpty()) GgtgUi.addCard(col, GgtgUi.card(context).also { it.addView(GgtgUi.body(context, "Ключей этого сервиса пока нет.")) })
            for (entry in mine) keyCard(context, col, s, entry)

            val add = GgtgUi.card(context)
            val field = GgtgAiUi.field(context, "Вставьте ключ ${p.name}", drafts[p.id].orEmpty())
            fields[p.id] = field
            add.addView(field, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT))
            addResults[p.id]?.takeIf { it.isNotEmpty() }?.let {
                add.addView(GgtgUi.body(context, it, 13f), LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply { topMargin = GgtgUi.dp(6f) })
            }
            add.addView(GgtgAiUi.links(context,
                GgtgAiUi.link("Добавить и проверить") { addKey(p.id, field) },
                GgtgAiUi.link("Получить бесплатный ключ") { Browser.openUrl(context, p.keyUrl) }),
                LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply { topMargin = GgtgUi.dp(6f) })
            GgtgUi.addCard(col, add)
        }
        val unknown = s.keys.filter { GgtgAi.provider(it.provider) == null }
        if (unknown.isNotEmpty()) {
            col.addView(GgtgAiUi.section(context, "Другое"))
            for (entry in unknown) keyCard(context, col, s, entry)
        }

        if (s.keys.any { it.provider == "openrouter" }) {
            col.addView(GgtgAiUi.section(context, "Бесплатные модели OpenRouter"))
            val free = GgtgAi.openRouterFree()
            val card = GgtgUi.card(context)
            card.addView(GgtgUi.body(context, if (free.isEmpty()) "Список ещё не скачан: он скачивается сам раз в день." else
                "Сейчас бесплатных: ${free.size}. Лучшие: " + free.take(5).joinToString(", ") { it.id } +
                    ". Когда одна модель упирается в лимит, работу берёт следующая.", 13f))
            card.addView(GgtgAiUi.links(context, GgtgAiUi.link("Обновить список сейчас") {
                scope.launch { GgtgAi.refreshOpenRouterFree(true); rebuild() }
            }))
            GgtgUi.addCard(col, card)
            if (free.isEmpty()) scope.launch { if (GgtgAi.refreshOpenRouterFree(false)) rebuild() }
        }
        col.addView(GgtgAiUi.gap(context, 24f))
    }
}
