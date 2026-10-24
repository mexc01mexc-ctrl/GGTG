package app.ggtg

import android.content.Context
import android.text.InputType
import android.view.Gravity
import android.view.View
import android.widget.CheckBox
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
import org.telegram.ui.Components.LayoutHelper

// GGTG: "Прогрев аккаунтов" - Trust Booster with strictly 2 modes:
// 1. Browsing International Channels & Groups (human pacing, reading posts, reactions, joining)
// 2. Inter-Account Mutual Conversation (natural English dialogue, typing simulation, typos, reactions)
class GgtgWarmupActivity(
    private val initialSelected: Collection<Int>? = null
) : BaseFragment() {

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main)
    private var picker: GgtgAccountPicker? = null

    private var currentMode = GgtgWarmup.WarmupMode.ChannelsAndGroups
    private lateinit var joinsInput: EditText
    private lateinit var postsInput: EditText
    private lateinit var reactsInput: EditText
    private lateinit var customChannelsInput: EditText
    private lateinit var turnsInput: EditText
    private lateinit var dialogueReactsCheckbox: CheckBox

    private lateinit var modeChannelsCard: LinearLayout
    private lateinit var modeDialogueCard: LinearLayout
    private lateinit var channelsParamsCard: LinearLayout
    private lateinit var dialogueParamsCard: LinearLayout

    private var logTextView: TextView? = null
    private var statusPill: TextView? = null

    private val processListener = {
        AndroidUtilities.runOnUIThread {
            updateProcessUI()
            picker?.refreshToday()
        }
    }

    override fun onResume() {
        super.onResume()
        GgtgProcess.addListener(processListener)
        picker?.refreshToday()
        updateProcessUI()
    }

    override fun onPause() {
        super.onPause()
        GgtgProcess.removeListener(processListener)
    }

    override fun createView(context: Context): View {
        actionBar.setBackButtonImage(R.drawable.ic_ab_back)
        actionBar.setAllowOverlayTitle(true)
        actionBar.setTitle("Прогрев аккаунтов")
        actionBar.setActionBarMenuOnItemClick(object : ActionBarMenuOnItemClick() {
            override fun onItemClick(id: Int) {
                if (id == -1) finishFragment()
            }
        })

        val frame = FrameLayout(context)
        frame.setBackgroundColor(Theme.getColor(Theme.key_windowBackgroundGray))
        fragmentView = frame
        val root = GgtgUi.page(context, frame)

        // Header info banner
        val introCard = GgtgUi.card(context)
        introCard.addView(GgtgUi.title(context, "Бустер траста аккаунтов"))
        introCard.addView(
            GgtgUi.body(
                context,
                "Максимальное повышение траста аккаунтов в Telegram:\n" +
                "• Режим 1: Чтение международных каналов и групп, реакции и подписки с человеческими паузами.\n" +
                "• Режим 2: Взаимная переписка между аккаунтами на живом английском языке с реалистичным набором текста, опечатками и реакциями."
            ),
            LinearLayout.LayoutParams(LayoutHelper.MATCH_PARENT, LayoutHelper.WRAP_CONTENT).apply {
                topMargin = GgtgUi.dp(6f)
            }
        )
        GgtgUi.addCard(root, introCard)

        // 1. Account picker
        root.addView(GgtgUi.sectionLabel(context, "1. Выбор аккаунтов"))
        val accounts = GgtgUi.card(context)
        val p = GgtgAccountPicker(context)
        p.setBackgroundColor(android.graphics.Color.TRANSPARENT)
        p.todayProvider = { slot ->
            val stats = GgtgWarmup.getStats(slot)
            val badge = when {
                stats.trustScore >= 75 -> "Высокий траст"
                stats.trustScore >= 45 -> "Средний траст"
                else -> "Новый аккаунт"
            }
            "Траст: ${stats.trustScore}% ($badge) · Постов: ${stats.postsRead}"
        }
        accounts.addView(p, LinearLayout.LayoutParams(LayoutHelper.MATCH_PARENT, LayoutHelper.WRAP_CONTENT))
        GgtgUi.addCard(root, accounts)
        picker = p
        if (initialSelected != null && initialSelected.isNotEmpty()) {
            p.setSelected(initialSelected)
        }

        // 2. Modes (Strictly 2)
        root.addView(GgtgUi.sectionLabel(context, "2. Выберите режим прогрева"))

        modeChannelsCard = buildModeCard(
            context,
            "1. Просмотр зарубежных каналов и групп",
            "Аккаунты последовательно просматривают международные каналы, читают посты с человеческой задержкой, ставят реакции и вступают.",
            GgtgWarmup.WarmupMode.ChannelsAndGroups
        )
        GgtgUi.addCard(root, modeChannelsCard)

        modeDialogueCard = buildModeCard(
            context,
            "2. Взаимная переписка между аккаунтами (English)",
            "Выбранные аккаунты переписываются друг с другом на живом английском языке: просмотр профилей, естественные паузы, реалистичный набор текста с опечатками, реакции и стикеры.",
            GgtgWarmup.WarmupMode.MutualDialogue
        )
        GgtgUi.addCard(root, modeDialogueCard)

        // 3. Mode Parameters
        root.addView(GgtgUi.sectionLabel(context, "3. Настройки выбранного режима"))

        // Mode 1: Channels Card
        channelsParamsCard = GgtgUi.card(context)
        joinsInput = GgtgUi.input(context, "2", "2", numeric = true)
        GgtgUi.labeled(context, channelsParamsCard, "Подписок на каналы", "Максимум новых каналов на каждый аккаунт (0 = не вступать)", joinsInput)

        postsInput = GgtgUi.input(context, "15", "15", numeric = true)
        GgtgUi.labeled(context, channelsParamsCard, "Просмотров постов в канале", "Количество постов, пролистываемых в каждом канале", postsInput)

        reactsInput = GgtgUi.input(context, "70", "70", numeric = true)
        GgtgUi.labeled(context, channelsParamsCard, "Вероятность реакции (%)", "Шанс реакции на просмотренный пост", reactsInput)

        customChannelsInput = GgtgUi.input(
            context,
            "techcrunch, nature, bbcnews\nили оставьте пустым",
            multi = true,
            minLines = 2
        )
        GgtgUi.labeled(
            context,
            channelsParamsCard,
            "Свои каналы (необязательно)",
            "По умолчанию используется безопасный международный каталог (TechCrunch, Nature, BBC Earth, Bloomberg...)",
            customChannelsInput
        )
        GgtgUi.addCard(root, channelsParamsCard)

        // Mode 2: Dialogue Card
        dialogueParamsCard = GgtgUi.card(context)
        turnsInput = GgtgUi.input(context, "2", "2", numeric = true)
        GgtgUi.labeled(context, dialogueParamsCard, "Раундов диалога на пару (1–4)", "Каждый раунд: отправка сообщения и ответ собеседника с паузами и набором текста", turnsInput)

        val reactsRow = LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding(0, GgtgUi.dp(8f), 0, GgtgUi.dp(8f))
        }
        dialogueReactsCheckbox = CheckBox(context).apply { isChecked = true }
        val rTexts = LinearLayout(context).apply { orientation = LinearLayout.VERTICAL }
        rTexts.addView(GgtgUi.title(context, "Ставить реакции в диалоге", 14f))
        rTexts.addView(GgtgUi.body(context, "Собеседники реагируют эмодзи (👍, 🔥, ❤️, 👏, 🎉) на реплики", 12f))
        reactsRow.addView(dialogueReactsCheckbox, LayoutHelper.createLinear(LayoutHelper.WRAP_CONTENT, LayoutHelper.WRAP_CONTENT))
        reactsRow.addView(rTexts, LayoutHelper.createLinear(0, LayoutHelper.WRAP_CONTENT, 1f))
        reactsRow.setOnClickListener { dialogueReactsCheckbox.isChecked = !dialogueReactsCheckbox.isChecked }
        dialogueParamsCard.addView(reactsRow)
        GgtgUi.addCard(root, dialogueParamsCard)

        // 4. Action Buttons
        GgtgUi.addButton(root, GgtgUi.button(context, "Запустить прогрев") { startWarmup() })
        GgtgUi.addButton(root, GgtgUi.button(context, "Остановить прогрев", destructive = true) {
            GgtgWarmup.cancelled.set(true)
            toast("Останавливаю прогрев...")
        })

        // 5. Live log card
        root.addView(GgtgUi.sectionLabel(context, "Лог прогрева"))
        val logCard = GgtgUi.card(context)
        val headRow = LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
        }
        headRow.addView(GgtgUi.title(context, "Статус: "), LayoutHelper.createLinear(LayoutHelper.WRAP_CONTENT, LayoutHelper.WRAP_CONTENT))
        val pill = GgtgUi.pill(context, "Ожидание", 0xFFFFFFFF.toInt(), 0xFF888888.toInt())
        headRow.addView(pill, LayoutHelper.createLinear(LayoutHelper.WRAP_CONTENT, LayoutHelper.WRAP_CONTENT))
        logCard.addView(headRow)
        statusPill = pill

        val logView = TextView(context).apply {
            textSize = 12f
            setTextColor(GgtgUi.color(Theme.key_windowBackgroundWhiteBlackText))
            setPadding(0, GgtgUi.dp(8f), 0, 0)
            text = "Готов к запуску. Выберите аккаунты и режим."
        }
        logCard.addView(logView)
        logTextView = logView
        GgtgUi.addCard(root, logCard)

        // Initial selection
        selectMode(GgtgWarmup.WarmupMode.ChannelsAndGroups)
        updateProcessUI()

        return frame
    }

    private fun buildModeCard(
        context: Context,
        title: String,
        desc: String,
        mode: GgtgWarmup.WarmupMode
    ): LinearLayout {
        val card = GgtgUi.card(context) {
            selectMode(mode)
        }
        card.addView(GgtgUi.title(context, title, 15f))
        card.addView(
            GgtgUi.body(context, desc, 12f),
            LinearLayout.LayoutParams(LayoutHelper.MATCH_PARENT, LayoutHelper.WRAP_CONTENT).apply {
                topMargin = GgtgUi.dp(3f)
            }
        )
        return card
    }

    private fun selectMode(mode: GgtgWarmup.WarmupMode) {
        currentMode = mode
        val activeFill = (GgtgUi.accent and 0x00FFFFFF) or 0x1A000000
        val white = GgtgUi.color(Theme.key_windowBackgroundWhite)
        val stroke = GgtgUi.accent

        modeChannelsCard.background = if (mode == GgtgWarmup.WarmupMode.ChannelsAndGroups)
            GgtgUi.rounded(activeFill, GgtgUi.dp(14f).toFloat(), GgtgUi.dp(2f), stroke)
        else GgtgUi.rounded(white, GgtgUi.dp(14f).toFloat())

        modeDialogueCard.background = if (mode == GgtgWarmup.WarmupMode.MutualDialogue)
            GgtgUi.rounded(activeFill, GgtgUi.dp(14f).toFloat(), GgtgUi.dp(2f), stroke)
        else GgtgUi.rounded(white, GgtgUi.dp(14f).toFloat())

        channelsParamsCard.visibility = if (mode == GgtgWarmup.WarmupMode.ChannelsAndGroups) View.VISIBLE else View.GONE
        dialogueParamsCard.visibility = if (mode == GgtgWarmup.WarmupMode.MutualDialogue) View.VISIBLE else View.GONE
    }

    private fun updateProcessUI() {
        val running = GgtgProcess.running(GgtgProcess.Kind.Warmup)
        if (running != null) {
            statusPill?.text = "Идёт прогрев"
            statusPill?.background = GgtgUi.rounded(GgtgUi.accent, GgtgUi.dp(10f).toFloat())
        } else {
            statusPill?.text = "Завершено / Ожидание"
            statusPill?.background = GgtgUi.rounded(0xFF888888.toInt(), GgtgUi.dp(10f).toFloat())
        }
    }

    private fun appendLog(line: String) {
        AndroidUtilities.runOnUIThread {
            val tv = logTextView ?: return@runOnUIThread
            val prev = tv.text.toString()
            val lines = prev.split("\n").takeLast(30)
            tv.text = (lines + line).joinToString("\n")
        }
    }

    private fun startWarmup() {
        val pickerSlots = picker?.selectedSlots() ?: emptyList()
        if (pickerSlots.isEmpty()) {
            toast("Выберите хотя бы один аккаунт!")
            return
        }

        if (currentMode == GgtgWarmup.WarmupMode.MutualDialogue && pickerSlots.size < 2) {
            toast("Для взаимной переписки выберите как минимум 2 аккаунта!")
            return
        }

        val running = GgtgProcess.running(GgtgProcess.Kind.Warmup)
        if (running != null) {
            toast("Прогрев уже выполняется!")
            return
        }

        val joins = joinsInput.text.toString().trim().toIntOrNull() ?: 2
        val posts = postsInput.text.toString().trim().toIntOrNull() ?: 15
        val reacts = reactsInput.text.toString().trim().toIntOrNull() ?: 70
        val customCh = customChannelsInput.text.toString().trim()
        val turns = turnsInput.text.toString().trim().toIntOrNull() ?: 2
        val dialogueReacts = dialogueReactsCheckbox.isChecked

        val config = GgtgWarmup.WarmupConfig(
            mode = currentMode,
            preset = currentMode,
            delayMinSec = 4,
            delayMaxSec = 8,
            joinChannels = joins > 0,
            maxJoinsPerAccount = joins,
            readPostsPerChannel = posts.coerceAtLeast(1),
            enableReactions = reacts > 0,
            reactionChance = reacts.coerceIn(0, 100),
            customChannels = customCh,
            dialogueTurns = turns.coerceIn(1, 4),
            enableDialogueReactions = dialogueReacts
        )

        updateProcessUI()
        val modeLabel = if (currentMode == GgtgWarmup.WarmupMode.MutualDialogue) "Взаимная переписка (English)" else "Просмотр каналов"
        appendLog("=== Старт прогрева ${pickerSlots.size} аккаунтов ($modeLabel) ===")

        scope.launch {
            try {
                GgtgWarmup.runWarmup(pickerSlots, config) { logLine ->
                    appendLog(logLine)
                }
                withContext(Dispatchers.Main) {
                    toast("Прогрев завершён!")
                    picker?.refreshToday()
                    updateProcessUI()
                }
            } catch (e: Exception) {
                withContext(Dispatchers.Main) {
                    appendLog("Ошибка прогрева: ${e.message}")
                    toast("Ошибка: ${e.message}")
                    updateProcessUI()
                }
            }
        }
    }

    private fun toast(msg: String) {
        Toast.makeText(context, msg, Toast.LENGTH_SHORT).show()
    }
}
