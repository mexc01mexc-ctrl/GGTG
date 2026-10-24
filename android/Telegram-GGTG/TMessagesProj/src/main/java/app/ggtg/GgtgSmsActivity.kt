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
import org.telegram.ui.Cells.TextCheckCell
import org.telegram.ui.Components.LayoutHelper

// ng: "Рассылка SMS" - sends a spintax template to every recipient of the chosen user bases
// through GgtgSms. Progress in "Процессы". Built from the same settings-screen cells the rest of
// the official app uses (HeaderCell / ShadowSectionCell / TextCheckCell) instead of raw
// CheckBox/EditText widgets, so it reads as one native-looking screen instead of a form.
class GgtgSmsActivity : BaseFragment() {

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main)
    private var picker: GgtgAccountPicker? = null
    private val baseRows = ArrayList<Pair<GgtgDb.GroupMeta, android.widget.CheckBox>>()
    private lateinit var templates: EditText
    private lateinit var greetingSticker: android.widget.CheckBox
    private lateinit var translateCheckbox: android.widget.CheckBox
    private var protectionMode = 1
    private lateinit var nightSleep: android.widget.CheckBox
    private lateinit var simulateTypos: android.widget.CheckBox
    private lateinit var filterByGgr: android.widget.CheckBox
    private lateinit var skipSuspectedFakes: android.widget.CheckBox
    private lateinit var quota: EditText
    private lateinit var threads: EditText
    private lateinit var daily: EditText
    private lateinit var pause: EditText
    private lateinit var pauseAccount: EditText

    // The counters under the accounts follow a running campaign live.
    private val processListener = { AndroidUtilities.runOnUIThread { picker?.refreshToday() } }

    override fun onResume() {
        super.onResume()
        GgtgProcess.addListener(processListener)
        picker?.refreshToday()
    }

    override fun onPause() {
        super.onPause()
        GgtgProcess.removeListener(processListener)
    }

    override fun createView(context: Context): View {
        actionBar.setBackButtonImage(R.drawable.ic_ab_back)
        actionBar.setAllowOverlayTitle(true)
        actionBar.setTitle("Рассылка SMS")
        actionBar.setActionBarMenuOnItemClick(object : ActionBarMenuOnItemClick() {
            override fun onItemClick(id: Int) { if (id == -1) finishFragment() }
        })

        val frame = FrameLayout(context)
        frame.setBackgroundColor(Theme.getColor(Theme.key_windowBackgroundGray))
        fragmentView = frame
        val root = GgtgUi.page(context, frame)

        // 1. accounts
        root.addView(GgtgUi.sectionLabel(context, "1. Аккаунты"))
        val accounts = GgtgUi.card(context)
        val p = GgtgAccountPicker(context)
        p.setBackgroundColor(android.graphics.Color.TRANSPARENT)
        accounts.addView(p, LinearLayout.LayoutParams(LayoutHelper.MATCH_PARENT, LayoutHelper.WRAP_CONTENT))
        GgtgUi.addCard(root, accounts)
        picker = p
        // how many each account has sent in the last 24 hours (of the daily limit, when one is set)
        p.todayProvider = { slot ->
            val sent = GgtgSms.sentToday(slot)
            sent.toString()
        }

        // 2. recipients: user bases only (group bases hold groups, not people)
        root.addView(GgtgUi.sectionLabel(context, "2. Получатели"))
        val bases = GgtgDb.loadIndex().filter { it.kind == GgtgDb.BaseKind.Users }
        val recipients = GgtgUi.card(context)
        if (bases.isEmpty()) {
            recipients.addView(GgtgUi.body(context, "Нет баз пользователей — создайте их в разделе «База»", 14f))
        } else {
            for (meta in bases) {
                val row = LinearLayout(context).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
                val box = android.widget.CheckBox(context)
                baseRows.add(meta to box)
                row.addView(box)
                row.addView(GgtgUi.title(context, meta.title, 15f), LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
                row.addView(GgtgUi.pill(context, "${GgtgDb.remainingIn(meta.id)} из ${meta.count}", 0xFFFFFFFF.toInt(), GgtgUi.accent))
                row.setOnClickListener { box.isChecked = !box.isChecked }
                recipients.addView(row, LinearLayout.LayoutParams(LayoutHelper.MATCH_PARENT, LayoutHelper.WRAP_CONTENT).apply { topMargin = GgtgUi.dp(2f) })
            }
        }
        GgtgUi.addCard(root, recipients)

        // 3. message
        root.addView(GgtgUi.sectionLabel(context, "3. Текст сообщения"))
        val message = GgtgUi.card(context)
        // a 👋 sticker first, the text 10-15 seconds later; with an empty text only the sticker goes
        val stickerRow = LinearLayout(context).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
        greetingSticker = android.widget.CheckBox(context)
        stickerRow.addView(greetingSticker)
        stickerRow.addView(GgtgUi.title(context, "Сначала приветственный стикер", 15f), LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
        stickerRow.setOnClickListener { greetingSticker.isChecked = !greetingSticker.isChecked }
        message.addView(stickerRow, LinearLayout.LayoutParams(LayoutHelper.MATCH_PARENT, LayoutHelper.WRAP_CONTENT))
        message.addView(GgtgUi.body(context,
            "Если текст пустой, уйдёт только стикер; если указан - сначала стикер, а через 10-15 секунд текст.", 12f),
            LinearLayout.LayoutParams(LayoutHelper.MATCH_PARENT, LayoutHelper.WRAP_CONTENT).apply { bottomMargin = GgtgUi.dp(8f) })

        val transRow = LinearLayout(context).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
        translateCheckbox = android.widget.CheckBox(context).apply { isChecked = false }
        transRow.addView(translateCheckbox)
        transRow.addView(GgtgUi.title(context, "Переводить сообщения (бесплатный переводчик)", 15f), LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
        transRow.setOnClickListener { translateCheckbox.isChecked = !translateCheckbox.isChecked }
        message.addView(transRow, LinearLayout.LayoutParams(LayoutHelper.MATCH_PARENT, LayoutHelper.WRAP_CONTENT))
        message.addView(GgtgUi.body(context,
            "Бесплатный перевод шаблона на язык получателя через Google Translate: корейцу по-корейски, китайцу по-китайски. Работает быстро и без расхода ключей ИИ.", 12f),
            LinearLayout.LayoutParams(LayoutHelper.MATCH_PARENT, LayoutHelper.WRAP_CONTENT).apply { bottomMargin = GgtgUi.dp(8f) })
        templates = GgtgUi.input(context, "Привет, [NAME|друг]! {Как дела|Есть минутка}?", multi = true, minLines = 4)
        GgtgUi.labeled(context, message, "Шаблоны", "По одному в строке — рассылка идёт по кругу", templates)
        message.addView(GgtgUi.body(context,
            "{а|б} — случайный вариант · [NAME] [SURNAME] — имя и фамилия получателя · [MYNAME] [MYSURNAME] — ваши · [GROUP] — группа, где нашли получателя (или [GROUP|друзья]) · [NAME|друг] — запасное слово · [SPLIT] — разбить на отдельные сообщения с одного аккаунта", 12f),
            LinearLayout.LayoutParams(LayoutHelper.MATCH_PARENT, LayoutHelper.WRAP_CONTENT).apply { topMargin = GgtgUi.dp(8f) })
        GgtgUi.addCard(root, message)

        // 4. AI Account Protection
        root.addView(GgtgUi.sectionLabel(context, "4. ИИ-Защита аккаунтов"))
        val protectionCard = GgtgUi.card(context)
        val modeOptions = listOf(
            Triple(0, "Консервативный", "Задержки x1.5 · Просмотр анкет 90% · Опечатки 8% · Сон 01:00–07:00"),
            Triple(1, "Оптимальный", "Задержки x1.0 · Просмотр анкет 70% · Опечатки 5% · Сон 02:00–06:00"),
            Triple(2, "Скоростной", "Задержки x0.7 · Просмотр анкет 30% · Опечатки 2% · Без ночного сна")
        )
        val modeViews = ArrayList<android.widget.RadioButton>()
        val radioGroup = android.widget.RadioGroup(context)
        for ((m, title, desc) in modeOptions) {
            val rb = android.widget.RadioButton(context).apply {
                text = "$title\n$desc"
                textSize = 13f
                isChecked = (m == protectionMode)
                setOnClickListener { protectionMode = m }
                setPadding(GgtgUi.dp(6f), GgtgUi.dp(4f), GgtgUi.dp(6f), GgtgUi.dp(8f))
            }
            modeViews.add(rb)
            radioGroup.addView(rb)
        }
        radioGroup.setOnCheckedChangeListener { _, checkedId ->
            for (idx in modeViews.indices) {
                if (modeViews[idx].id == checkedId) {
                    protectionMode = idx
                    break
                }
            }
        }
        protectionCard.addView(radioGroup)

        val sleepRow = LinearLayout(context).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
        nightSleep = android.widget.CheckBox(context).apply { isChecked = true }
        sleepRow.addView(nightSleep)
        sleepRow.addView(GgtgUi.title(context, "Ночной сон по гео-таймзоне", 15f), LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
        sleepRow.setOnClickListener { nightSleep.isChecked = !nightSleep.isChecked }
        protectionCard.addView(sleepRow, LinearLayout.LayoutParams(LayoutHelper.MATCH_PARENT, LayoutHelper.WRAP_CONTENT).apply { topMargin = GgtgUi.dp(8f) })
        protectionCard.addView(GgtgUi.body(context, "Не отправляет сообщения ночью по местному времени прокси/номера аккаунта.", 12f),
            LinearLayout.LayoutParams(LayoutHelper.MATCH_PARENT, LayoutHelper.WRAP_CONTENT).apply { bottomMargin = GgtgUi.dp(8f) })

        val typosRow = LinearLayout(context).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
        simulateTypos = android.widget.CheckBox(context).apply { isChecked = true }
        typosRow.addView(simulateTypos)
        typosRow.addView(GgtgUi.title(context, "Эмуляция опечаток и правок", 15f), LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
        typosRow.setOnClickListener { simulateTypos.isChecked = !simulateTypos.isChecked }
        protectionCard.addView(typosRow, LinearLayout.LayoutParams(LayoutHelper.MATCH_PARENT, LayoutHelper.WRAP_CONTENT).apply { topMargin = GgtgUi.dp(4f) })
        protectionCard.addView(GgtgUi.body(context, "Имитирует паузы и стирание текста через Backspace перед отправкой сообщения.", 12f),
            LinearLayout.LayoutParams(LayoutHelper.MATCH_PARENT, LayoutHelper.WRAP_CONTENT).apply { bottomMargin = GgtgUi.dp(8f) })

        val ggrRow = LinearLayout(context).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
        filterByGgr = android.widget.CheckBox(context).apply { isChecked = false }
        ggrRow.addView(filterByGgr)
        ggrRow.addView(GgtgUi.title(context, "Только аккаунты с высоким трастом (GGR ≥ 7.0)", 15f), LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
        ggrRow.setOnClickListener { filterByGgr.isChecked = !filterByGgr.isChecked }
        protectionCard.addView(ggrRow, LinearLayout.LayoutParams(LayoutHelper.MATCH_PARENT, LayoutHelper.WRAP_CONTENT).apply { topMargin = GgtgUi.dp(4f) })
        protectionCard.addView(GgtgUi.body(context, "Автоматически отсеивает молодые сессии и аккаунты с риском блокировки.", 12f),
            LinearLayout.LayoutParams(LayoutHelper.MATCH_PARENT, LayoutHelper.WRAP_CONTENT).apply { bottomMargin = GgtgUi.dp(8f) })

        val fakesRow = LinearLayout(context).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
        skipSuspectedFakes = android.widget.CheckBox(context).apply { isChecked = true }
        fakesRow.addView(skipSuspectedFakes)
        fakesRow.addView(GgtgUi.title(context, "Не писать предположительным фейкам", 15f), LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
        fakesRow.setOnClickListener { skipSuspectedFakes.isChecked = !skipSuspectedFakes.isChecked }
        protectionCard.addView(fakesRow, LinearLayout.LayoutParams(LayoutHelper.MATCH_PARENT, LayoutHelper.WRAP_CONTENT).apply { topMargin = GgtgUi.dp(4f) })
        protectionCard.addView(GgtgUi.body(context, "Перед отправкой проверяет профиль и фото; подозрительные боты пропускаются и помечаются в базе.", 12f),
            LinearLayout.LayoutParams(LayoutHelper.MATCH_PARENT, LayoutHelper.WRAP_CONTENT))

        GgtgUi.addCard(root, protectionCard)

        // 5. limits and pauses
        root.addView(GgtgUi.sectionLabel(context, "5. Лимиты и паузы"))
        val params = GgtgUi.card(context)
        quota = GgtgUi.input(context, "20", "20", numeric = true)
        GgtgUi.labeled(context, params, "Сообщений с одного аккаунта", "0 — без лимита", quota)
        threads = GgtgUi.input(context, "1", "1", numeric = true)
        GgtgUi.labeled(context, params, "Потоков", "Сколько аккаунтов пишут одновременно", threads)
        daily = GgtgUi.input(context, "0", "0", numeric = true)
        GgtgUi.labeled(context, params, "Лимит в сутки на аккаунт", "0 — без лимита", daily)
        pause = GgtgUi.input(context, "30-60", "30-60")
        GgtgUi.labeled(context, params, "Пауза между смс, сек", "Формат от-до, например 10-20, или одно число", pause)
        pauseAccount = GgtgUi.input(context, "60-90", "60-90")
        GgtgUi.labeled(context, params, "Пауза между аккаунтами, сек", "Перед тем как поток возьмёт следующий аккаунт. Формат от-до", pauseAccount)
        GgtgUi.addCard(root, params)

        GgtgUi.addButton(root, GgtgUi.button(context, "Запустить рассылку") { start() }, top = 14f)
        GgtgUi.addButton(root, GgtgUi.button(context, "Остановить", destructive = true) { GgtgSms.cancelled = true; toast("Останавливаю…") }, top = 8f)
        root.addView(GgtgUi.spacer(context))
        p.refreshToday()
        return frame
    }

    private fun start() {
        val slots = picker?.selectedSlots() ?: emptyList()
        if (slots.isEmpty()) { toast("Выберите аккаунты"); return }
        val chosen = baseRows.filter { it.second.isChecked }.map { it.first }
        if (chosen.isEmpty()) { toast("Выберите базы"); return }
        val sticker = greetingSticker.isChecked
        var tpl = templates.text.toString().lines().map { it.trim() }.filter { it.isNotEmpty() }
        // with the sticker on, an empty text is fine: the sticker alone is the whole message
        if (tpl.isEmpty() && sticker) tpl = listOf("")
        if (tpl.isEmpty()) { toast("Введите шаблон"); return }
        if (GgtgProcess.running(GgtgProcess.Kind.Sms) != null) { toast("Рассылка уже идёт"); return }

        val baseOf = HashMap<String, Long>()
        val usernames = LinkedHashSet<String>()
        for (meta in chosen) for (name in GgtgDb.loadUsernames(meta.id)) {
            if (usernames.add(name)) baseOf[name.lowercase()] = meta.id
        }
        if (usernames.isEmpty()) { toast("В выбранных базах пусто"); return }

        val (minMs, maxMs) = parsePauseRange(pause.text.toString())
        val (accMinMs, accMaxMs) = parsePauseRange(pauseAccount.text.toString())
        val config = GgtgSms.Config(
            slots = slots,
            usernames = usernames.toList(),
            baseOf = baseOf,
            templates = tpl,
            perAccountQuota = quota.text.toString().toIntOrNull() ?: 0,
            threads = threads.text.toString().toIntOrNull() ?: 1,
            dailyLimit = daily.text.toString().toIntOrNull() ?: 0,
            pauseMinMs = minMs,
            pauseMaxMs = maxMs,
            pauseAccountMinMs = accMinMs,
            pauseAccountMaxMs = accMaxMs,
            greetingSticker = sticker,
            protectionMode = protectionMode,
            nightSleep = nightSleep.isChecked,
            simulateTypos = simulateTypos.isChecked,
            filterByGgr = filterByGgr.isChecked,
            skipSuspectedFakes = skipSuspectedFakes.isChecked,
            translate = translateCheckbox.isChecked
        )
        val (planned, seconds) = GgtgSms.estimate(config)
        toast("Запущено: около $planned сообщений, примерно ${GgtgSms.durationText(seconds)}")
        context?.let { GgtgBattery.askOnce(it) }
        scope.launch { withContext(Dispatchers.IO) { GgtgSms.run(config) } }
    }

    private fun toast(text: String) { context?.let { Toast.makeText(it, text, Toast.LENGTH_LONG).show() } }

    // ng: "10-20" -> a random pause in that range each time; a bare "15" -> a fixed 15s pause.
    private fun parsePauseRange(text: String): Pair<Long, Long> {
        val parts = text.trim().split("-", limit = 2).mapNotNull { it.trim().toLongOrNull() }
        val minSec = parts.getOrNull(0) ?: 30L
        val maxSec = maxOf(minSec, parts.getOrNull(1) ?: minSec)
        return (minSec * 1000) to (maxSec * 1000)
    }
}
