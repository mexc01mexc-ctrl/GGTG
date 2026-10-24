package app.ggtg

import android.app.Activity
import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.content.Intent
import android.graphics.Typeface
import android.net.Uri
import android.provider.OpenableColumns
import android.text.InputType
import android.view.Gravity
import android.view.View
import android.widget.CheckBox
import android.widget.EditText
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.TextView
import android.widget.Toast
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.launch
import org.telegram.messenger.AndroidUtilities
import org.telegram.messenger.ApplicationLoader
import org.telegram.messenger.R
import org.telegram.ui.ActionBar.ActionBar.ActionBarMenuOnItemClick
import org.telegram.ui.ActionBar.AlertDialog
import org.telegram.ui.ActionBar.BaseFragment
import org.telegram.ui.ActionBar.Theme
import org.telegram.ui.Components.GgtgProxyPoolStore
import org.telegram.ui.Components.LayoutHelper
import java.io.File

// GGTG: In-app Registration Activity for Android — full feature parity with Mac version
class GgtgRegistrationActivity : BaseFragment() {

    companion object {
        private const val REQ_PICK_FIRST_NAMES = 1001
        private const val REQ_PICK_LAST_NAMES = 1002
    }

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main)

    private lateinit var serviceNameView: TextView
    private lateinit var apiKeyInput: EditText
    private lateinit var balanceText: TextView

    private lateinit var countryView: TextView
    private lateinit var proxyView: TextView

    private lateinit var targetCountInput: EditText
    private lateinit var threadsCountInput: EditText
    private lateinit var stepDelayMinInput: EditText
    private lateinit var stepDelayMaxInput: EditText
    private lateinit var accountDelayMinInput: EditText
    private lateinit var accountDelayMaxInput: EditText

    private lateinit var smsWaitInput: EditText
    private lateinit var skipAppCheck: CheckBox
    private lateinit var filterCheck: CheckBox
    private lateinit var folderInput: EditText

    // 2FA controls
    private lateinit var single2faCheck: CheckBox
    private lateinit var single2faLayout: LinearLayout
    private lateinit var random2faLayout: LinearLayout
    private lateinit var twoFaInput: EditText

    // Names & Gender controls
    private lateinit var singleNameCheck: CheckBox
    private lateinit var singleNameLayout: LinearLayout
    private lateinit var randomNamesLayout: LinearLayout
    private lateinit var firstNameInput: EditText
    private lateinit var lastNameInput: EditText
    private lateinit var genderView: TextView
    private lateinit var firstNameFileStatusText: TextView
    private lateinit var firstNameFileClearBtn: View
    private lateinit var lastNameFileStatusText: TextView
    private lateinit var lastNameFileClearBtn: View
    private lateinit var autoSetUsernameCheck: CheckBox
    private lateinit var autoSetBioCheck: CheckBox
    private lateinit var bioInput: EditText
    private lateinit var precheckProxyCheck: CheckBox
    private lateinit var refundQueueLabel: TextView
    private lateinit var refundQueueBtn: View

    // SMS & Tokens controls
    private lateinit var operatorView: TextView
    private lateinit var maxPriceInput: EditText
    private lateinit var resetSessionsCheck: CheckBox
    private lateinit var reset2faCheck: CheckBox
    private lateinit var autoBuyEmailCheck: CheckBox
    private lateinit var emailApiKeyInput: EditText
    private lateinit var tokenServiceView: TextView
    private lateinit var tokenApiKeyInput: EditText
    private lateinit var tokenBalanceText: TextView
    private lateinit var replaceTokenCheck: CheckBox

    private lateinit var statusTitleView: TextView
    private lateinit var progressLabelView: TextView
    private lateinit var startButton: View
    private lateinit var runningButtonsRow: LinearLayout
    private lateinit var pauseButton: TextView
    private lateinit var stopButton: TextView
    private lateinit var logConsoleView: TextView

    private var currentConfig = GgtgRegistration.loadConfig()

    override fun createView(context: Context): View {
        actionBar.setBackButtonImage(R.drawable.ic_ab_back)
        actionBar.setAllowOverlayTitle(true)
        actionBar.setTitle("Авторегистрация по SMS")
        actionBar.setActionBarMenuOnItemClick(object : ActionBarMenuOnItemClick() {
            override fun onItemClick(id: Int) {
                if (id == -1) finishFragment()
            }
        })

        val frame = FrameLayout(context)
        frame.setBackgroundColor(Theme.getColor(Theme.key_windowBackgroundGray))
        fragmentView = frame

        val root = GgtgUi.page(context, frame)

        // -----------------------------------------------------------------------------
        // 1. SMS Service & API Key
        // -----------------------------------------------------------------------------
        root.addView(sectionHeader(context, "SMS-сервис и API-ключ", "Откуда брать номера для регистрации", R.drawable.msg2_devices))
        val serviceCard = GgtgUi.card(context)

        val currentServiceName = GgtgRegistration.SERVICES.firstOrNull { it.id == currentConfig.serviceId }?.name ?: GgtgRegistration.SERVICES.first().name
        serviceNameView = pickerRow(context, serviceCard, "Сервис", currentServiceName) { showServicePicker(context) }

        apiKeyInput = GgtgUi.input(context, "API-ключ сервиса", currentConfig.apiKey)
        GgtgUi.labeled(context, serviceCard, "API-ключ", "Сохраняется отдельно для каждого сервиса, вводить заново не нужно", apiKeyInput)
        apiKeyInput.addTextChangedListener(object : android.text.TextWatcher {
            override fun beforeTextChanged(s: CharSequence?, start: Int, count: Int, after: Int) {}
            override fun onTextChanged(s: CharSequence?, start: Int, before: Int, count: Int) {}
            override fun afterTextChanged(s: android.text.Editable?) {
                currentConfig = currentConfig.copy(apiKey = s.toString().trim())
                GgtgRegistration.saveConfig(currentConfig)
            }
        })

        val balanceRow = LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding(0, GgtgUi.dp(8f), 0, 0)
        }
        val checkBalBtn = GgtgUi.withLeadingIcon(GgtgUi.pill(context, "Проверить баланс", 0xFFFFFFFF.toInt(), GgtgUi.accent), R.drawable.msg_stats, 0xFFFFFFFF.toInt(), 13f, 5f)
        checkBalBtn.setOnClickListener { checkBalance() }
        balanceRow.addView(checkBalBtn)
        balanceText = GgtgUi.body(context, "  Баланс: —", 13f)
        balanceRow.addView(balanceText)
        serviceCard.addView(balanceRow)

        GgtgUi.addCard(root, serviceCard)

        // -----------------------------------------------------------------------------
        // 1.5 SMS Order & Email Settings (operator, maxPrice, email)
        // -----------------------------------------------------------------------------
        root.addView(sectionHeader(context, "Настройки SMS-заказа и Email", "Оператор, лимит цены и подтверждение почты", R.drawable.msg_payment_card))
        val smsOrderCard = GgtgUi.card(context)

        val currentOpTitle = GgtgRegistration.operatorDisplayTitle(currentConfig.smsOperator)
        operatorView = pickerRow(context, smsOrderCard, "Оператор связи", currentOpTitle) { showOperatorPicker(context) }

        maxPriceInput = GgtgUi.input(context, "без ограничения", currentConfig.maxPrice)
        GgtgUi.labeled(context, smsOrderCard, "Макс. цена номера", "Максимальная стоимость, например 15.00. Пусто = без лимита", maxPriceInput)

        autoBuyEmailCheck = checkBox(context, "Автопокупка Email при требовании Telegram", currentConfig.autoBuyEmail)
        smsOrderCard.addView(autoBuyEmailCheck)
        smsOrderCard.addView(GgtgUi.body(context, "Если Telegram требует указать Email для получения кода, программа автоматически купит и подтвердит временную почту (Hero-SMS / SMSBower).", 12f))

        emailApiKeyInput = GgtgUi.input(context, "пусто = такой же как у SMS-сервиса", currentConfig.emailApiKey)
        GgtgUi.labeled(context, smsOrderCard, "API-ключ почты (необязательно)", "Отдельный API-ключ для сервиса почты, если отличается от SMS-сервиса", emailApiKeyInput)

        resetSessionsCheck = checkBox(context, "Сбросить другие сессии после регистрации", currentConfig.resetSessionsAfterReg)
        smsOrderCard.addView(resetSessionsCheck)
        smsOrderCard.addView(GgtgUi.body(context, "После успешной регистрации вызывает auth.resetAuthorizations — все другие сессии номера будут отключены.", 12f))

        reset2faCheck = checkBox(context, "Запросить сброс 2FA на занятых номерах", currentConfig.reset2faOnOccupied)
        smsOrderCard.addView(reset2faCheck)
        smsOrderCard.addView(GgtgUi.body(context, "Если на номере стоит чужой 2FA пароль, отправляет запрос account.resetPassword (сброс через 7 дней).", 12f))

        GgtgUi.addCard(root, smsOrderCard)

        // -----------------------------------------------------------------------------
        // 2. Country Selection
        // -----------------------------------------------------------------------------
        root.addView(sectionHeader(context, "Страна номеров", "Номера какой страны покупать", R.drawable.msg_language))
        val countryCard = GgtgUi.card(context) { showCountryPicker(context) }
        countryView = GgtgUi.title(context, GgtgRegistration.currentCountryTitle(currentConfig), 15f)
        val countryRow = LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
        }
        countryRow.addView(GgtgUi.badge(context, R.drawable.msg_language, 36f), LinearLayout.LayoutParams(GgtgUi.dp(36f), GgtgUi.dp(36f)).apply { rightMargin = GgtgUi.dp(12f) })
        countryRow.addView(countryView, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
        countryRow.addView(chevron(context))
        countryCard.addView(countryRow)
        GgtgUi.addCard(root, countryCard)

        // -----------------------------------------------------------------------------
        // 2.5 Names & Surnames
        // -----------------------------------------------------------------------------
        root.addView(sectionHeader(context, "Имена и фамилии", "Настройка имён для создаваемых аккаунтов", R.drawable.msg_contacts))
        val namesCard = GgtgUi.card(context)

        singleNameCheck = checkBox(context, "Одно имя и фамилия на все аккаунты", currentConfig.singleNameForAll)
        namesCard.addView(singleNameCheck)
        namesCard.addView(GgtgUi.body(context, "Если включено — все создаваемые аккаунты получат указанные имя и фамилию. Если выключено — имена генерируются автоматически или берутся из файлов.", 12f))

        // Fixed single name container
        singleNameLayout = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(0, GgtgUi.dp(6f), 0, 0)
            visibility = if (currentConfig.singleNameForAll) View.VISIBLE else View.GONE
        }
        firstNameInput = GgtgUi.input(context, "Имя (например, Иван)", currentConfig.customFirstName)
        bindAutoSave(firstNameInput) { currentConfig = currentConfig.copy(customFirstName = it) }
        GgtgUi.labeled(context, singleNameLayout, "Имя для всех аккаунтов", "Будет установлено всем новым пользователям", firstNameInput)

        lastNameInput = GgtgUi.input(context, "Фамилия (например, Иванов)", currentConfig.customLastName)
        bindAutoSave(lastNameInput) { currentConfig = currentConfig.copy(customLastName = it) }
        GgtgUi.labeled(context, singleNameLayout, "Фамилия для всех аккаунтов", "Будет установлена всем новым пользователям", lastNameInput)
        namesCard.addView(singleNameLayout)

        // Random or file-based names container
        randomNamesLayout = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(0, GgtgUi.dp(6f), 0, 0)
            visibility = if (currentConfig.singleNameForAll) View.GONE else View.VISIBLE
        }
        val genderLabel = when (currentConfig.gender) { 1 -> "Мужской"; 2 -> "Женский"; else -> "Случайный (М/Ж)" }
        genderView = pickerRow(context, randomNamesLayout, "Пол", genderLabel) { showGenderPicker(context) }

        // First names file row
        randomNamesLayout.addView(GgtgUi.title(context, "Файл со списком имён", 14f).apply {
            setPadding(0, GgtgUi.dp(8f), 0, GgtgUi.dp(2f))
        })
        firstNameFileStatusText = GgtgUi.body(context, "Файл не выбран (случайные имена)", 13f)
        randomNamesLayout.addView(firstNameFileStatusText)

        val fnBtnRow = LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL
            setPadding(0, GgtgUi.dp(6f), 0, GgtgUi.dp(10f))
        }
        val pickFnBtn = GgtgUi.withLeadingIcon(
            GgtgUi.pill(context, "Выбрать файл с именами", 0xFFFFFFFF.toInt(), GgtgUi.accent),
            R.drawable.msg_folders, 0xFFFFFFFF.toInt(), 13f, 5f
        )
        pickFnBtn.setOnClickListener { pickNamesFile(REQ_PICK_FIRST_NAMES) }
        fnBtnRow.addView(pickFnBtn)

        firstNameFileClearBtn = GgtgUi.withLeadingIcon(
            GgtgUi.pill(context, "Очистить", 0xFFFFFFFF.toInt(), 0xFFE53935.toInt()),
            R.drawable.msg_clear, 0xFFFFFFFF.toInt(), 13f, 5f
        ).apply {
            setOnClickListener {
                currentConfig = currentConfig.copy(firstNameFile = "")
                GgtgRegistration.saveConfig(currentConfig)
                updateNamesFileLabels()
            }
        }
        fnBtnRow.addView(firstNameFileClearBtn, LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply {
            leftMargin = GgtgUi.dp(8f)
        })
        randomNamesLayout.addView(fnBtnRow)

        // Last names file row
        randomNamesLayout.addView(GgtgUi.title(context, "Файл со списком фамилий", 14f).apply {
            setPadding(0, GgtgUi.dp(4f), 0, GgtgUi.dp(2f))
        })
        lastNameFileStatusText = GgtgUi.body(context, "Файл не выбран (случайные фамилии)", 13f)
        randomNamesLayout.addView(lastNameFileStatusText)

        val lnBtnRow = LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL
            setPadding(0, GgtgUi.dp(6f), 0, GgtgUi.dp(4f))
        }
        val pickLnBtn = GgtgUi.withLeadingIcon(
            GgtgUi.pill(context, "Выбрать файл с фамилиями", 0xFFFFFFFF.toInt(), GgtgUi.accent),
            R.drawable.msg_folders, 0xFFFFFFFF.toInt(), 13f, 5f
        )
        pickLnBtn.setOnClickListener { pickNamesFile(REQ_PICK_LAST_NAMES) }
        lnBtnRow.addView(pickLnBtn)

        lastNameFileClearBtn = GgtgUi.withLeadingIcon(
            GgtgUi.pill(context, "Очистить", 0xFFFFFFFF.toInt(), 0xFFE53935.toInt()),
            R.drawable.msg_clear, 0xFFFFFFFF.toInt(), 13f, 5f
        ).apply {
            setOnClickListener {
                currentConfig = currentConfig.copy(lastNameFile = "")
                GgtgRegistration.saveConfig(currentConfig)
                updateNamesFileLabels()
            }
        }
        lnBtnRow.addView(lastNameFileClearBtn, LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply {
            leftMargin = GgtgUi.dp(8f)
        })
        randomNamesLayout.addView(lnBtnRow)

        namesCard.addView(randomNamesLayout)

        singleNameCheck.setOnCheckedChangeListener { _, isChecked ->
            currentConfig = currentConfig.copy(singleNameForAll = isChecked, autoRandomNames = !isChecked)
            GgtgRegistration.saveConfig(currentConfig)
            singleNameLayout.visibility = if (isChecked) View.VISIBLE else View.GONE
            randomNamesLayout.visibility = if (isChecked) View.GONE else View.VISIBLE
        }

        updateNamesFileLabels()

        autoSetUsernameCheck = checkBox(context, "Устанавливать @username при регистрации", currentConfig.autoSetUsername)
        autoSetUsernameCheck.setOnCheckedChangeListener { _, isChecked ->
            currentConfig = currentConfig.copy(autoSetUsername = isChecked)
            GgtgRegistration.saveConfig(currentConfig)
        }
        namesCard.addView(autoSetUsernameCheck)
        namesCard.addView(GgtgUi.body(context, "Генерирует уникальный юзернейм на основе имени и фамилии (например, ivan_ivanov_842).", 12f))

        autoSetBioCheck = checkBox(context, "Заполнять описание профиля (Bio)", currentConfig.autoSetBio)
        autoSetBioCheck.setOnCheckedChangeListener { _, isChecked ->
            currentConfig = currentConfig.copy(autoSetBio = isChecked)
            GgtgRegistration.saveConfig(currentConfig)
        }
        namesCard.addView(autoSetBioCheck)
        bioInput = GgtgUi.input(context, "Текст Bio (или оставьте пустым для случайного)", currentConfig.customBio)
        bindAutoSave(bioInput) { currentConfig = currentConfig.copy(customBio = it) }
        GgtgUi.labeled(context, namesCard, "Описание профиля (Bio)", "Оставьте пустым для реалистичных случайных фраз, либо укажите свой шаблон (поддерживает рандомизацию {A|B})", bioInput)

        GgtgUi.addCard(root, namesCard)

        // -----------------------------------------------------------------------------
        // 2.7 Token Service (Play Integrity / SafetyNet)
        // -----------------------------------------------------------------------------
        root.addView(sectionHeader(context, "Токены Play Integrity", "Сервис токенов для обхода защиты при регистрации", R.drawable.msg_secret))
        val tokenCard = GgtgUi.card(context)

        val currentTokenName = GgtgRegistration.tokenServiceName(currentConfig.tokenService)
        tokenServiceView = pickerRow(context, tokenCard, "Сервис токенов", currentTokenName) { showTokenServicePicker(context) }

        tokenApiKeyInput = GgtgUi.input(context, "API-ключ TokenFast / AntiSafety", currentConfig.tokenApiKey)
        bindAutoSave(tokenApiKeyInput) { currentConfig = currentConfig.copy(tokenApiKey = it) }
        GgtgUi.labeled(context, tokenCard, "API-ключ токенов", "Master API key сервиса TokenFast или AntiSafety", tokenApiKeyInput)

        replaceTokenCheck = checkBox(context, "Заменять токен при лимите флуда (FLOOD)", currentConfig.replaceTokenOnFlood)
        replaceTokenCheck.setOnCheckedChangeListener { _, isChecked ->
            currentConfig = currentConfig.copy(replaceTokenOnFlood = isChecked)
            GgtgRegistration.saveConfig(currentConfig)
        }
        tokenCard.addView(replaceTokenCheck)

        val tokenBalanceRow = LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding(0, GgtgUi.dp(8f), 0, 0)
        }
        val checkTokenBalBtn = GgtgUi.withLeadingIcon(GgtgUi.pill(context, "Проверить баланс", 0xFFFFFFFF.toInt(), GgtgUi.accent), R.drawable.msg_stats, 0xFFFFFFFF.toInt(), 13f, 5f)
        checkTokenBalBtn.setOnClickListener { checkTokenBalance() }
        tokenBalanceRow.addView(checkTokenBalBtn)
        tokenBalanceText = GgtgUi.body(context, "  Баланс: —", 13f)
        tokenBalanceRow.addView(tokenBalanceText)
        tokenCard.addView(tokenBalanceRow)

        GgtgUi.addCard(root, tokenCard)

        // -----------------------------------------------------------------------------
        // 3. Plan & Threads
        // -----------------------------------------------------------------------------
        root.addView(sectionHeader(context, "План и потоки", "Сколько аккаунтов и как быстро", R.drawable.msg_stats))
        val planCard = GgtgUi.card(context)

        targetCountInput = GgtgUi.input(context, "10", currentConfig.targetSuccessCount.toString(), numeric = true)
        bindAutoSave(targetCountInput) {
            val v = it.toIntOrNull()?.coerceAtLeast(1) ?: 10
            currentConfig = currentConfig.copy(targetSuccessCount = v)
        }
        GgtgUi.labeled(context, planCard, "Целевое количество успешных аккаунтов", "Программа работает в цикле, пока не достигнет этой цели", targetCountInput)

        threadsCountInput = GgtgUi.input(context, "2", currentConfig.threadsCount.toString(), numeric = true)
        bindAutoSave(threadsCountInput) {
            val v = it.toIntOrNull()?.coerceIn(1, 10) ?: 2
            currentConfig = currentConfig.copy(threadsCount = v)
        }
        GgtgUi.labeled(context, planCard, "Параллельных потоков", "Сколько аккаунтов регистрируются одновременно (1–10)", threadsCountInput)

        val stepDelayRow = LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding(0, GgtgUi.dp(6f), 0, 0)
        }
        stepDelayMinInput = GgtgUi.input(context, "10", currentConfig.stepDelayMin.toString(), numeric = true).apply {
            layoutParams = LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f)
        }
        bindAutoSave(stepDelayMinInput) {
            val v = it.toIntOrNull()?.coerceAtLeast(0) ?: 10
            currentConfig = currentConfig.copy(stepDelayMin = v)
        }
        stepDelayMaxInput = GgtgUi.input(context, "30", currentConfig.stepDelayMax.toString(), numeric = true).apply {
            layoutParams = LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f)
        }
        bindAutoSave(stepDelayMaxInput) {
            val v = it.toIntOrNull()?.coerceAtLeast(10) ?: 30
            currentConfig = currentConfig.copy(stepDelayMax = v)
        }
        stepDelayRow.addView(GgtgUi.body(context, "от ", 13f))
        stepDelayRow.addView(stepDelayMinInput)
        stepDelayRow.addView(GgtgUi.body(context, " до ", 13f))
        stepDelayRow.addView(stepDelayMaxInput)
        stepDelayRow.addView(GgtgUi.body(context, " сек", 13f))
        GgtgUi.labeled(context, planCard, "Пауза между шагами регистрации", "Интервал случайной задержки между действиями", stepDelayRow)

        val accDelayRow = LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding(0, GgtgUi.dp(6f), 0, 0)
        }
        accountDelayMinInput = GgtgUi.input(context, "15", currentConfig.accountDelayMin.toString(), numeric = true).apply {
            layoutParams = LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f)
        }
        bindAutoSave(accountDelayMinInput) {
            val v = it.toIntOrNull()?.coerceAtLeast(0) ?: 15
            currentConfig = currentConfig.copy(accountDelayMin = v)
        }
        accountDelayMaxInput = GgtgUi.input(context, "45", currentConfig.accountDelayMax.toString(), numeric = true).apply {
            layoutParams = LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f)
        }
        bindAutoSave(accountDelayMaxInput) {
            val v = it.toIntOrNull()?.coerceAtLeast(15) ?: 45
            currentConfig = currentConfig.copy(accountDelayMax = v)
        }
        accDelayRow.addView(GgtgUi.body(context, "от ", 13f))
        accDelayRow.addView(accountDelayMinInput)
        accDelayRow.addView(GgtgUi.body(context, " до ", 13f))
        accDelayRow.addView(accountDelayMaxInput)
        accDelayRow.addView(GgtgUi.body(context, " сек", 13f))
        GgtgUi.labeled(context, planCard, "Пауза между аккаунтами", "Интервал задержки перед запуском следующего номера", accDelayRow)

        smsWaitInput = GgtgUi.input(context, "300", currentConfig.smsWaitSeconds.toString(), numeric = true)
        bindAutoSave(smsWaitInput) {
            val v = it.toIntOrNull()?.coerceIn(120, 1800) ?: 300
            currentConfig = currentConfig.copy(smsWaitSeconds = v)
        }
        GgtgUi.labeled(context, planCard, "Сколько ждать SMS-код (сек)", "От 120 до 1800, рекомендуется 300: номер обычно живёт у сервиса ~20 минут, а слишком короткое ожидание отменяет номера, на которые код ещё придёт. Если SMS не придёт, номер отменяется и деньги возвращаются (статус 8)", smsWaitInput)

        skipAppCheck = checkBox(context, "Не регистрировать трасты (отменять номер, если код пришёл бы в приложение)", currentConfig.skipAppCode)
        skipAppCheck.setOnCheckedChangeListener { _, isChecked ->
            currentConfig = currentConfig.copy(skipAppCode = isChecked)
            GgtgRegistration.saveConfig(currentConfig)
        }
        planCard.addView(skipAppCheck)
        planCard.addView(GgtgUi.body(context, "Если Telegram шлёт код в приложение, номер уже зарегистрирован. Опция отменяет его и возвращает деньги, но на некоторых странах код в приложение приходит и для новых номеров - проверьте на небольшом объёме.", 12f))

        filterCheck = checkBox(context, "Фильтровать номера перед регистрацией", currentConfig.filterNumbers)
        filterCheck.setOnCheckedChangeListener { _, isChecked ->
            currentConfig = currentConfig.copy(filterNumbers = isChecked)
            GgtgRegistration.saveConfig(currentConfig)
        }
        planCard.addView(filterCheck)
        planCard.addView(GgtgUi.body(context, "Программа помнит номера, которые уже не подошли (заняты, заблокированы, неверные), и сразу отменяет их при повторной выдаче, не запрашивая код.", 12f))

        GgtgUi.addCard(root, planCard)

        // -----------------------------------------------------------------------------
        // 4. Proxy (Сеть и пулы прокси)
        // -----------------------------------------------------------------------------
        root.addView(sectionHeader(context, "Прокси и сеть", "Подключение аккаунтов через списки прокси", R.drawable.msg2_permissions))
        val proxyCard = GgtgUi.card(context)

        proxyView = pickerRow(context, proxyCard, "Список прокси", getProxyDisplayTitle()) { showProxyPicker(context) }

        val proxyManageRow = LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL
            setPadding(0, GgtgUi.dp(6f), 0, 0)
        }
        val manageProxyBtn = GgtgUi.withLeadingIcon(
            GgtgUi.pill(context, "Управление списками прокси", 0xFFFFFFFF.toInt(), GgtgUi.accent),
            R.drawable.msg_settings, 0xFFFFFFFF.toInt(), 13f, 5f
        )
        manageProxyBtn.setOnClickListener {
            presentFragment(GgtgProxyActivity())
        }
        proxyManageRow.addView(manageProxyBtn)
        proxyCard.addView(proxyManageRow)

        folderInput = GgtgUi.input(context, "Регистрация", currentConfig.folderTag)
        bindAutoSave(folderInput) { currentConfig = currentConfig.copy(folderTag = it) }
        GgtgUi.labeled(context, proxyCard, "Папка в GGTG", "Куда сохранять новые аккаунты", folderInput)

        precheckProxyCheck = checkBox(context, "Проверять прокси перед заказом номера", currentConfig.precheckProxy)
        precheckProxyCheck.setOnCheckedChangeListener { _, isChecked ->
            currentConfig = currentConfig.copy(precheckProxy = isChecked)
            GgtgRegistration.saveConfig(currentConfig)
        }
        proxyCard.addView(precheckProxyCheck)
        proxyCard.addView(GgtgUi.body(context, "Исключает списание средств за номер, если прокси из пула внезапно перестал отвечать.", 12f))

        GgtgUi.addCard(root, proxyCard)

        // -----------------------------------------------------------------------------
        // 4.5 Two-Factor Authentication (2FA)
        // -----------------------------------------------------------------------------
        root.addView(sectionHeader(context, "Облачный пароль (2FA)", "Двухфакторная защита для новых аккаунтов", R.drawable.msg_secret))
        val twoFaCard = GgtgUi.card(context)

        single2faCheck = checkBox(context, "Один пароль на все аккаунты", currentConfig.single2faForAll)
        twoFaCard.addView(single2faCheck)
        twoFaCard.addView(GgtgUi.body(context, "Если включено — на всех создаваемых аккаунтах будет установлен указанный пароль. Если выключено — для каждого аккаунта генерируется свой уникальный надёжный пароль (18 символов), автоматически сохраняемый в 2FA Vault.", 12f))

        single2faLayout = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(0, GgtgUi.dp(6f), 0, 0)
            visibility = if (currentConfig.single2faForAll) View.VISIBLE else View.GONE
        }
        twoFaInput = GgtgUi.input(context, "Пароль 2FA", currentConfig.twoFactorPassword)
        bindAutoSave(twoFaInput) { currentConfig = currentConfig.copy(twoFactorPassword = it) }
        GgtgUi.labeled(context, single2faLayout, "Пароль 2FA", "Будет установлен на всех регистрируемых аккаунтах", twoFaInput)

        val pwdBtnsRow = LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL
            setPadding(0, GgtgUi.dp(6f), 0, 0)
        }
        val genPwdBtn = GgtgUi.withLeadingIcon(GgtgUi.pill(context, "Сгенерировать", 0xFFFFFFFF.toInt(), GgtgUi.accent), R.drawable.msg_select, 0xFFFFFFFF.toInt(), 13f, 5f)
        genPwdBtn.setOnClickListener {
            val pwd = GgtgRegistration.generateStrongPassword(18)
            twoFaInput.setText(pwd)
            currentConfig = currentConfig.copy(twoFactorPassword = pwd)
            GgtgRegistration.saveConfig(currentConfig)
            copyToClipboard(context, pwd, "Пароль 2FA сгенерирован и скопирован")
        }
        pwdBtnsRow.addView(genPwdBtn)

        val copyPwdBtn = GgtgUi.withLeadingIcon(GgtgUi.pill(context, "Скопировать", 0xFFFFFFFF.toInt(), GgtgUi.accent), R.drawable.msg_copy, 0xFFFFFFFF.toInt(), 13f, 5f)
        copyPwdBtn.setOnClickListener {
            val pwd = twoFaInput.text.toString().trim()
            if (pwd.isNotEmpty()) {
                copyToClipboard(context, pwd, "Пароль скопирован")
            } else {
                Toast.makeText(context, "Поле пароля пустое", Toast.LENGTH_SHORT).show()
            }
        }
        pwdBtnsRow.addView(copyPwdBtn, LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply {
            leftMargin = GgtgUi.dp(10f)
        })
        single2faLayout.addView(pwdBtnsRow)
        twoFaCard.addView(single2faLayout)

        random2faLayout = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(0, GgtgUi.dp(6f), 0, 0)
            visibility = if (currentConfig.single2faForAll) View.GONE else View.VISIBLE
        }
        random2faLayout.addView(GgtgUi.body(context, "🔐 Для каждого аккаунта будет сгенерирован свой уникальный надежный пароль 2FA и сохранен в хранилище приложения (2FA Vault).", 13f))
        twoFaCard.addView(random2faLayout)

        single2faCheck.setOnCheckedChangeListener { _, isChecked ->
            currentConfig = currentConfig.copy(single2faForAll = isChecked)
            GgtgRegistration.saveConfig(currentConfig)
            single2faLayout.visibility = if (isChecked) View.VISIBLE else View.GONE
            random2faLayout.visibility = if (isChecked) View.GONE else View.VISIBLE
        }

        GgtgUi.addCard(root, twoFaCard)

        // -----------------------------------------------------------------------------
        // 5. Execution & Status
        // -----------------------------------------------------------------------------
        root.addView(sectionHeader(context, "Процесс регистрации", "Управление процессом авторегистрации", R.drawable.msg_list))
        val statusCard = GgtgUi.card(context)
        statusTitleView = GgtgUi.title(context, "Готов к запуску", 16f)
        setStatus(statusTitleView, "Готов к запуску", R.drawable.msg_info)
        statusCard.addView(statusTitleView)

        progressLabelView = GgtgUi.body(context, "Успешно: 0 / ${currentConfig.targetSuccessCount} | Потоков: 0 | Ошибок: 0", 14f)
        statusCard.addView(progressLabelView, LinearLayout.LayoutParams(LayoutHelper.MATCH_PARENT, LayoutHelper.WRAP_CONTENT).apply {
            topMargin = GgtgUi.dp(4f)
        })

        startButton = iconButton(context, "Запустить регистрацию", R.drawable.msg_select) { startRegistration() }
        statusCard.addView(startButton, LinearLayout.LayoutParams(LayoutHelper.MATCH_PARENT, LayoutHelper.WRAP_CONTENT).apply {
            topMargin = GgtgUi.dp(12f)
        })

        runningButtonsRow = LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL
            visibility = View.GONE
        }
        pauseButton = iconButton(context, "Пауза", R.drawable.ic_pause) {
            GgtgRegistration.Engine.togglePause()
        }
        stopButton = iconButton(context, "Остановить", R.drawable.msg_close, destructive = true) {
            stopRegistration()
        }
        runningButtonsRow.addView(pauseButton, LinearLayout.LayoutParams(0, LayoutHelper.WRAP_CONTENT, 1f).apply {
            rightMargin = GgtgUi.dp(6f)
        })
        runningButtonsRow.addView(stopButton, LinearLayout.LayoutParams(0, LayoutHelper.WRAP_CONTENT, 1f).apply {
            leftMargin = GgtgUi.dp(6f)
        })
        statusCard.addView(runningButtonsRow, LinearLayout.LayoutParams(LayoutHelper.MATCH_PARENT, LayoutHelper.WRAP_CONTENT).apply {
            topMargin = GgtgUi.dp(8f)
        })

        val refundQueueRow = LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding(0, GgtgUi.dp(10f), 0, 0)
        }
        refundQueueLabel = GgtgUi.body(context, "Очередь автовозврата: 0 номеров", 13f)
        refundQueueRow.addView(refundQueueLabel, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
        refundQueueBtn = GgtgUi.withLeadingIcon(
            GgtgUi.pill(context, "Проверить возвраты", 0xFFFFFFFF.toInt(), GgtgUi.accent),
            R.drawable.msg_retry, 0xFFFFFFFF.toInt(), 12f, 4f
        )
        refundQueueBtn.setOnClickListener {
            scope.launch {
                val qSize = GgtgRegistration.Engine.refundQueue.size
                if (qSize == 0) {
                    Toast.makeText(context, "Очередь возвратов пуста. Все средства возвращены!", Toast.LENGTH_SHORT).show()
                } else {
                    Toast.makeText(context, "Запуск немедленного опроса возвратов ($qSize в очереди)...", Toast.LENGTH_SHORT).show()
                    GgtgRegistration.Engine.refundPendingActivations(currentConfig)
                }
                checkBalance()
            }
        }
        refundQueueRow.addView(refundQueueBtn)
        statusCard.addView(refundQueueRow)

        GgtgUi.addCard(root, statusCard)

        // -----------------------------------------------------------------------------
        // 6. Action Log Console
        // -----------------------------------------------------------------------------
        root.addView(sectionHeader(context, "Журнал действий", "Что происходит прямо сейчас", R.drawable.msg_list))
        val logCard = GgtgUi.card(context)
        logConsoleView = TextView(context).apply {
            text = "Лог авторегистрации готов..."
            textSize = 12f
            typeface = Typeface.MONOSPACE
            setTextColor(GgtgUi.color(Theme.key_windowBackgroundWhiteBlackText))
            setPadding(GgtgUi.dp(8f), GgtgUi.dp(8f), GgtgUi.dp(8f), GgtgUi.dp(8f))
            background = GgtgUi.rounded(GgtgUi.color(Theme.key_windowBackgroundGray), GgtgUi.dp(8f).toFloat())
        }
        logCard.addView(logConsoleView)
        GgtgUi.addCard(root, logCard)

        root.addView(GgtgUi.spacer(context, 24f))

        if (currentConfig.apiKey.isNotEmpty() && currentConfig.countryId >= 0) {
            scope.launch {
                val ops = GgtgRegistration.fetchOperators(currentConfig, currentConfig.countryId).getOrNull()
                val cur = ops?.firstOrNull { it.name == currentConfig.smsOperator || (currentConfig.smsOperator.isEmpty() && it.name == "any") }
                if (cur != null) {
                    AndroidUtilities.runOnUIThread {
                        if (::operatorView.isInitialized) {
                            operatorView.text = GgtgRegistration.operatorDisplayTitle(currentConfig.smsOperator, cur.count)
                        }
                    }
                }
            }
        }

        setupEngineCallbacks()
        return frame
    }


    private fun chevron(context: Context) = TextView(context).apply {
        text = "›"
        textSize = 22f
        setTextColor(GgtgUi.color(Theme.key_windowBackgroundWhiteGrayText))
    }

    /** Section header: round accent badge with a Telegram icon, bold title and a gray hint line. */
    private fun sectionHeader(context: Context, title: String, sub: String, iconRes: Int): View {
        val row = LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding(GgtgUi.dp(18f), GgtgUi.dp(18f), GgtgUi.dp(18f), GgtgUi.dp(2f))
        }
        row.addView(GgtgUi.badge(context, iconRes, 32f), LinearLayout.LayoutParams(GgtgUi.dp(32f), GgtgUi.dp(32f)).apply { rightMargin = GgtgUi.dp(10f) })
        val texts = LinearLayout(context).apply { orientation = LinearLayout.VERTICAL }
        texts.addView(GgtgUi.title(context, title, 15f))
        texts.addView(GgtgUi.body(context, sub, 12f))
        row.addView(texts, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
        return row
    }

    /** A tappable "label - value ›" row in a card; returns the value view so it can be updated. */
    private fun pickerRow(context: Context, card: LinearLayout, label: String, value: String, onClick: () -> Unit): TextView {
        val row = LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            background = GgtgUi.rounded(GgtgUi.color(Theme.key_windowBackgroundGray), GgtgUi.dp(10f).toFloat())
            setPadding(GgtgUi.dp(12f), GgtgUi.dp(10f), GgtgUi.dp(12f), GgtgUi.dp(10f))
            setOnClickListener { onClick() }
        }
        row.addView(GgtgUi.body(context, "$label:  ", 14f))
        val valueView = GgtgUi.title(context, value, 14f)
        row.addView(valueView, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
        row.addView(chevron(context))
        card.addView(row, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT))
        return valueView
    }

    private fun iconButton(context: Context, text: String, icon: Int, destructive: Boolean = false, onClick: () -> Unit): TextView =
        GgtgUi.withLeadingIcon(GgtgUi.button(context, text, destructive, onClick), icon, 0xFFFFFFFF.toInt(), 18f, 8f)

    private fun setStatus(view: TextView, text: String, icon: Int, tint: Int = GgtgUi.accent) {
        view.text = text
        GgtgUi.withLeadingIcon(view, icon, tint, 20f, 8f)
    }

    private fun checkBox(context: Context, text: String, checked: Boolean) = CheckBox(context).apply {
        this.text = text
        isChecked = checked
        textSize = 14f
        setTextColor(GgtgUi.color(Theme.key_windowBackgroundWhiteBlackText))
        setPadding(0, GgtgUi.dp(6f), 0, 0)
    }

    private fun updatePauseButton(paused: Boolean) {
        val text = if (paused) "Продолжить" else "Пауза"
        val icon = if (paused) R.drawable.ic_play else R.drawable.ic_pause
        pauseButton.text = text
        GgtgUi.withLeadingIcon(pauseButton, icon, 0xFFFFFFFF.toInt(), 18f, 8f)
    }

    private fun setupEngineCallbacks() {
        val history = GgtgRegistration.Engine.getLogHistory()
        if (history.isNotEmpty()) {
            logConsoleView.text = history.takeLast(40).joinToString("\n")
        }

        val running = GgtgRegistration.Engine.isRunning.get()
        startButton.visibility = if (running) View.GONE else View.VISIBLE
        runningButtonsRow.visibility = if (running) View.VISIBLE else View.GONE
        updatePauseButton(GgtgRegistration.Engine.isPaused.get())
        if (running) {
            val s = GgtgRegistration.Engine.successCount.get()
            val t = currentConfig.targetSuccessCount
            val a = GgtgRegistration.Engine.activeThreads.get()
            val f = GgtgRegistration.Engine.failedCount.get()
            if (GgtgRegistration.Engine.isPaused.get()) {
                setStatus(statusTitleView, "Авторегистрация на паузе", R.drawable.msg_recent)
            } else {
                setStatus(statusTitleView, "Авторегистрация выполняется...", R.drawable.msg_recent)
            }
            progressLabelView.text = "Успешно: $s / $t | Потоков: $a | Ошибок: $f"
        }
        refundQueueLabel.text = "Очередь автовозврата: ${GgtgRegistration.Engine.refundQueue.size} номеров"

        GgtgRegistration.Engine.onPauseChanged = { paused ->
            AndroidUtilities.runOnUIThread {
                updatePauseButton(paused)
            }
        }

        GgtgRegistration.Engine.onLog = { line ->
            AndroidUtilities.runOnUIThread {
                val prev = logConsoleView.text.toString()
                logConsoleView.text = if (prev.isEmpty() || prev == "Лог авторегистрации готов...") line else "$prev\n$line"
                refundQueueLabel.text = "Очередь автовозврата: ${GgtgRegistration.Engine.refundQueue.size} номеров"
            }
        }

        GgtgRegistration.Engine.onStatus = { msg, isError, isSuccess ->
            AndroidUtilities.runOnUIThread {
                when {
                    isError -> setStatus(statusTitleView, msg, R.drawable.msg_warning, GgtgUi.color(Theme.key_text_RedRegular))
                    isSuccess -> setStatus(statusTitleView, msg, R.drawable.msg_select, GgtgUi.color(Theme.key_chats_actionBackground))
                    else -> setStatus(statusTitleView, msg, R.drawable.msg_recent)
                }
            }
        }

        GgtgRegistration.Engine.onProgress = { success, target, active, failed ->
            AndroidUtilities.runOnUIThread {
                progressLabelView.text = "Успешно: $success / $target | Потоков: $active | Ошибок: $failed"
                val isRunning = GgtgRegistration.Engine.isRunning.get()
                startButton.visibility = if (isRunning) View.GONE else View.VISIBLE
                runningButtonsRow.visibility = if (isRunning) View.VISIBLE else View.GONE
                updatePauseButton(GgtgRegistration.Engine.isPaused.get())
            }
        }

        GgtgRegistration.Engine.onFinished = { success, failed ->
            AndroidUtilities.runOnUIThread {
                startButton.visibility = View.VISIBLE
                runningButtonsRow.visibility = View.GONE
            }
        }
    }

    private fun showServicePicker(context: Context) {
        val names = GgtgRegistration.SERVICES.map { it.name }.toTypedArray()
        AlertDialog.Builder(context).apply {
            setTitle("Выберите SMS-сервис")
            setItems(names) { _, which ->
                val s = GgtgRegistration.SERVICES[which]
                // keep the key typed for the old service, bring the one kept for the new service
                GgtgRegistration.saveConfig(currentConfig.copy(apiKey = apiKeyInput.text.toString().trim()))
                currentConfig = currentConfig.copy(serviceId = s.id, countryName = "", apiKey = GgtgRegistration.apiKeyFor(s.id), smsOperator = "")
                apiKeyInput.setText(currentConfig.apiKey)
                serviceNameView.text = s.name
                countryView.text = GgtgRegistration.currentCountryTitle(currentConfig)
                if (::operatorView.isInitialized) {
                    operatorView.text = GgtgRegistration.operatorDisplayTitle("")
                }
                GgtgRegistration.saveConfig(currentConfig)
            }
            setNegativeButton("Отмена", null)
        }.show()
    }

    private fun showCountryPicker(context: Context) {
        // always ask the service: only its current list is shown
        currentConfig = currentConfig.copy(apiKey = apiKeyInput.text.toString().trim())
        GgtgRegistration.saveConfig(currentConfig)
        Toast.makeText(context, "Загружаю актуальный список стран...", Toast.LENGTH_SHORT).show()
        scope.launch {
            val res = GgtgRegistration.fetchCountries(currentConfig)
            if (res.isFailure && GgtgRegistration.countriesFor(currentConfig.serviceId).isEmpty()) {
                Toast.makeText(context, "Не удалось получить список стран: ${res.exceptionOrNull()?.message}", Toast.LENGTH_LONG).show()
                return@launch
            }
            showCountryDialog(context)
        }
    }

    private fun showCountryDialog(context: Context) {
        val allCountries = GgtgRegistration.countriesFor(currentConfig.serviceId)
        val box = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(GgtgUi.dp(20f), GgtgUi.dp(10f), GgtgUi.dp(20f), 0)
        }
        val search = GgtgUi.input(context, "Поиск страны или кода (+1, US, 0)...")
        box.addView(search)

        val list = LinearLayout(context).apply { orientation = LinearLayout.VERTICAL }
        val scroll = android.widget.ScrollView(context).apply { addView(list) }
        box.addView(scroll, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, GgtgUi.dp(360f)).apply {
            topMargin = GgtgUi.dp(8f)
        })

        val dialog = AlertDialog.Builder(context)
            .setTitle("Выберите страну номеров")
            .setView(box)
            .setNegativeButton("Отмена", null)
            .create()

        fun fill() {
            list.removeAllViews()
            val query = search.text.toString().trim().lowercase(java.util.Locale.getDefault())
            var count = 0
            for (c in allCountries) {
                val title = GgtgRegistration.countryTitle(c)
                val idStr = c.id.toString()
                val codeStr = c.phoneCode.lowercase(java.util.Locale.getDefault())
                val isoStr = c.isoCode.lowercase(java.util.Locale.getDefault())
                if (query.isNotEmpty() && !title.lowercase(java.util.Locale.getDefault()).contains(query) &&
                    !idStr.contains(query) && !codeStr.contains(query) && !isoStr.contains(query)) {
                    continue
                }
                count++
                val isSelected = c.id == currentConfig.countryId
                val row = GgtgUi.title(context, (if (isSelected) "● " else "○ ") + title, 15f).apply {
                    setPadding(0, GgtgUi.dp(10f), 0, GgtgUi.dp(10f))
                    setOnClickListener {
                        currentConfig = currentConfig.copy(countryId = c.id, countryName = title, smsOperator = "")
                        countryView.text = title
                        if (::operatorView.isInitialized) {
                            operatorView.text = GgtgRegistration.operatorDisplayTitle("")
                        }
                        GgtgRegistration.saveConfig(currentConfig)
                        dialog.dismiss()
                        scope.launch {
                            val ops = GgtgRegistration.fetchOperators(currentConfig, c.id).getOrNull()
                            val anyOp = ops?.firstOrNull { it.name == "any" }
                            if (anyOp != null) {
                                AndroidUtilities.runOnUIThread {
                                    if (::operatorView.isInitialized) {
                                        operatorView.text = GgtgRegistration.operatorDisplayTitle(currentConfig.smsOperator, anyOp.count)
                                    }
                                }
                            }
                        }
                    }
                }
                list.addView(row)
            }
            if (count == 0) {
                list.addView(GgtgUi.body(context, "Ничего не найдено").apply {
                    setPadding(0, GgtgUi.dp(16f), 0, GgtgUi.dp(16f))
                })
            }
        }

        search.addTextChangedListener(object : android.text.TextWatcher {
            override fun beforeTextChanged(s: CharSequence?, start: Int, count: Int, after: Int) {}
            override fun onTextChanged(s: CharSequence?, start: Int, before: Int, count: Int) {
                fill()
            }
            override fun afterTextChanged(s: android.text.Editable?) {}
        })

        fill()
        dialog.show()
    }

    private fun showOperatorPicker(context: Context) {
        val key = if (::apiKeyInput.isInitialized) apiKeyInput.text.toString().trim() else currentConfig.apiKey
        currentConfig = currentConfig.copy(apiKey = key)
        GgtgRegistration.saveConfig(currentConfig)
        if (currentConfig.apiKey.isEmpty()) {
            Toast.makeText(context, "Сначала укажите API-ключ сервиса", Toast.LENGTH_SHORT).show()
            return
        }
        Toast.makeText(context, "Загружаю список операторов...", Toast.LENGTH_SHORT).show()
        scope.launch {
            val res = GgtgRegistration.fetchOperators(currentConfig, currentConfig.countryId)
            val ops = res.getOrNull() ?: listOf(GgtgRegistration.OperatorInfo("any", "Любой оператор (any)"))
            AndroidUtilities.runOnUIThread {
                showOperatorDialog(context, ops)
            }
        }
    }

    private fun showOperatorDialog(context: Context, ops: List<GgtgRegistration.OperatorInfo>) {
        val box = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(GgtgUi.dp(20f), GgtgUi.dp(10f), GgtgUi.dp(20f), 0)
        }
        val search = GgtgUi.input(context, "Поиск оператора (mts, vodafone, any)...")
        box.addView(search)

        val list = LinearLayout(context).apply { orientation = LinearLayout.VERTICAL }
        val scroll = android.widget.ScrollView(context).apply { addView(list) }
        box.addView(scroll, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, GgtgUi.dp(360f)).apply {
            topMargin = GgtgUi.dp(8f)
        })

        var dialog: AlertDialog? = null

        fun fill() {
            list.removeAllViews()
            val query = search.text.toString().trim().lowercase(java.util.Locale.getDefault())
            var count = 0

            // Option to type custom manually
            val manualRow = GgtgUi.title(context, "✎ Ввести код вручную...", 14f).apply {
                setPadding(0, GgtgUi.dp(10f), 0, GgtgUi.dp(10f))
                setTextColor(GgtgUi.accent)
                setOnClickListener {
                    dialog?.dismiss()
                    showManualOperatorPrompt(context)
                }
            }
            list.addView(manualRow)

            val div = View(context).apply {
                setBackgroundColor(GgtgUi.color(Theme.key_divider))
            }
            list.addView(div, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 1).apply {
                bottomMargin = GgtgUi.dp(6f)
            })

            for (op in ops) {
                val nameLower = op.name.lowercase(java.util.Locale.getDefault())
                val titleLower = op.title.lowercase(java.util.Locale.getDefault())
                if (query.isNotEmpty() && !nameLower.contains(query) && !titleLower.contains(query)) {
                    continue
                }
                count++
                val isSelected = (op.name == currentConfig.smsOperator) || (currentConfig.smsOperator.isEmpty() && op.name == "any")
                val text = buildString {
                    append(if (isSelected) "● " else "○ ")
                    append(op.title)
                    if (op.count >= 0) {
                        append(" — доступно: ${op.count} шт.")
                    }
                    if (op.price.isNotEmpty()) {
                        append(" (от ${op.price}$)")
                    }
                }
                val row = GgtgUi.title(context, text, 14f).apply {
                    setPadding(0, GgtgUi.dp(10f), 0, GgtgUi.dp(10f))
                    setOnClickListener {
                        val chosen = if (op.name == "any") "" else op.name
                        currentConfig = currentConfig.copy(smsOperator = chosen)
                        if (::operatorView.isInitialized) {
                            operatorView.text = GgtgRegistration.operatorDisplayTitle(chosen, op.count)
                        }
                        GgtgRegistration.saveConfig(currentConfig)
                        dialog?.dismiss()
                    }
                }
                list.addView(row)
            }
            if (count == 0) {
                list.addView(GgtgUi.body(context, "Ничего не найдено").apply {
                    setPadding(0, GgtgUi.dp(16f), 0, GgtgUi.dp(16f))
                })
            }
        }

        search.addTextChangedListener(object : android.text.TextWatcher {
            override fun beforeTextChanged(s: CharSequence?, start: Int, count: Int, after: Int) {}
            override fun onTextChanged(s: CharSequence?, start: Int, before: Int, count: Int) {
                fill()
            }
            override fun afterTextChanged(s: android.text.Editable?) {}
        })

        fill()

        dialog = AlertDialog.Builder(context)
            .setTitle("Выберите оператора")
            .setView(box)
            .setNegativeButton("Отмена", null)
            .create()

        dialog.show()
    }

    private fun showManualOperatorPrompt(context: Context) {
        val input = GgtgUi.input(context, "Код оператора (например: mts, vodafone)", currentConfig.smsOperator)
        input.setSelection(input.text.length)
        AlertDialog.Builder(context)
            .setTitle("Код оператора вручную")
            .setView(FrameLayout(context).apply {
                setPadding(GgtgUi.dp(20f), GgtgUi.dp(10f), GgtgUi.dp(20f), GgtgUi.dp(10f))
                addView(input)
            })
            .setPositiveButton("Сохранить") { _, _ ->
                val op = input.text.toString().trim()
                currentConfig = currentConfig.copy(smsOperator = op)
                if (::operatorView.isInitialized) {
                    operatorView.text = GgtgRegistration.operatorDisplayTitle(op)
                }
                GgtgRegistration.saveConfig(currentConfig)
            }
            .setNegativeButton("Отмена", null)
            .show()
    }

    private fun showProxyPicker(context: Context) {
        val listNames = ArrayList<String>()
        listNames.add("Без прокси (напрямую)")
        listNames.addAll(GgtgProxyPoolStore.getListNames())
        AlertDialog.Builder(context).apply {
            setTitle("Выберите пул прокси")
            setItems(listNames.toTypedArray()) { _, which ->
                val selected = if (which == 0) "" else listNames[which]
                currentConfig = currentConfig.copy(proxyListName = selected)
                proxyView.text = getProxyDisplayTitle()
                GgtgRegistration.saveConfig(currentConfig)
            }
            setNegativeButton("Отмена", null)
        }.show()
    }

    private fun showGenderPicker(context: Context) {
        val options = arrayOf("Случайный (М/Ж)", "Мужской", "Женский")
        AlertDialog.Builder(context).apply {
            setTitle("Выберите пол для генерации имён")
            setItems(options) { _, which ->
                currentConfig = currentConfig.copy(gender = which)
                genderView.text = options[which]
                GgtgRegistration.saveConfig(currentConfig)
            }
            setNegativeButton("Отмена", null)
        }.show()
    }

    private fun showTokenServicePicker(context: Context) {
        val options = arrayOf("Нет (без токенов)", "TokenFast (tokenfast.shop/api)", "AntiSafety (antisafety.net)")
        AlertDialog.Builder(context).apply {
            setTitle("Выберите сервис токенов Play Integrity / SafetyNet")
            setItems(options) { _, which ->
                currentConfig = currentConfig.copy(tokenService = which)
                tokenServiceView.text = GgtgRegistration.tokenServiceName(which)
                GgtgRegistration.saveConfig(currentConfig)
            }
            setNegativeButton("Отмена", null)
        }.show()
    }

    private fun checkBalance() {
        val key = apiKeyInput.text.toString().trim()
        if (key.isEmpty()) {
            Toast.makeText(parentActivity, "Введите API-ключ", Toast.LENGTH_SHORT).show()
            return
        }
        currentConfig = currentConfig.copy(apiKey = key)
        GgtgRegistration.saveConfig(currentConfig)
        balanceText.text = "  Баланс: проверка..."
        scope.launch {
            val res = GgtgRegistration.fetchBalance(currentConfig)
            balanceText.text = if (res.isSuccess) "  Баланс: ${res.getOrNull()}" else "  Ошибка: ${res.exceptionOrNull()?.message}"
            // the same check refreshes the list of countries the service has
            if (GgtgRegistration.fetchCountries(currentConfig).isSuccess) {
                countryView.text = GgtgRegistration.currentCountryTitle(currentConfig)
            }
        }
    }

    private fun checkTokenBalance() {
        val key = tokenApiKeyInput.text.toString().trim()
        if (key.isEmpty()) {
            Toast.makeText(parentActivity, "Введите API-ключ сервиса токенов", Toast.LENGTH_SHORT).show()
            return
        }
        currentConfig = currentConfig.copy(tokenApiKey = key)
        GgtgRegistration.saveConfig(currentConfig)
        tokenBalanceText.text = "  Баланс: проверка..."
        scope.launch {
            val res = GgtgRegistration.fetchTokenBalance(currentConfig.tokenService, key)
            tokenBalanceText.text = if (res.isSuccess) "  Баланс: ${res.getOrNull()}" else "  Ошибка: ${res.exceptionOrNull()?.message}"
        }
    }

    private fun copyToClipboard(context: Context, text: String, toastMsg: String) {
        val clipboard = context.getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
        clipboard.setPrimaryClip(ClipData.newPlainText("2FA", text))
        Toast.makeText(context, toastMsg, Toast.LENGTH_SHORT).show()
    }

    private fun startRegistration() {
        val key = apiKeyInput.text.toString().trim()
        if (key.isEmpty()) {
            Toast.makeText(parentActivity, "Введите API-ключ SMS-сервиса", Toast.LENGTH_SHORT).show()
            return
        }
        if (currentConfig.countryId < 0) {
            Toast.makeText(parentActivity, "Выберите страну номера перед запуском", Toast.LENGTH_SHORT).show()
            return
        }
        val target = targetCountInput.text.toString().trim().toIntOrNull()?.coerceAtLeast(1) ?: 10
        val threads = threadsCountInput.text.toString().trim().toIntOrNull()?.coerceIn(1, 10) ?: 2
        val folder = folderInput.text.toString().trim().ifEmpty { "Регистрация" }
        val twoFa = twoFaInput.text.toString().trim()

        val stepMin = stepDelayMinInput.text.toString().trim().toIntOrNull()?.coerceAtLeast(0) ?: 10
        val stepMax = stepDelayMaxInput.text.toString().trim().toIntOrNull()?.coerceAtLeast(stepMin) ?: 30
        val accMin = accountDelayMinInput.text.toString().trim().toIntOrNull()?.coerceAtLeast(0) ?: 15
        val accMax = accountDelayMaxInput.text.toString().trim().toIntOrNull()?.coerceAtLeast(accMin) ?: 45

        currentConfig = currentConfig.copy(
            apiKey = key,
            targetSuccessCount = target,
            threadsCount = threads,
            folderTag = folder,
            single2faForAll = single2faCheck.isChecked,
            twoFactorPassword = twoFa,
            singleNameForAll = singleNameCheck.isChecked,
            customFirstName = firstNameInput.text.toString().trim(),
            customLastName = lastNameInput.text.toString().trim(),
            autoRandomNames = !singleNameCheck.isChecked,
            stepDelayMin = stepMin,
            stepDelayMax = stepMax,
            accountDelayMin = accMin,
            accountDelayMax = accMax,
            smsWaitSeconds = smsWaitInput.text.toString().trim().toIntOrNull()?.coerceIn(120, 1800) ?: 300,
            skipAppCode = skipAppCheck.isChecked,
            filterNumbers = filterCheck.isChecked,
            replaceTokenOnFlood = replaceTokenCheck.isChecked,
            autoBuyEmail = autoBuyEmailCheck.isChecked,
            emailApiKey = emailApiKeyInput.text.toString().trim(),
            smsOperator = currentConfig.smsOperator,
            maxPrice = maxPriceInput.text.toString().trim(),
            gender = currentConfig.gender,
            firstNameFile = currentConfig.firstNameFile,
            lastNameFile = currentConfig.lastNameFile,
            resetSessionsAfterReg = resetSessionsCheck.isChecked,
            reset2faOnOccupied = reset2faCheck.isChecked,
            tokenService = currentConfig.tokenService,
            tokenApiKey = tokenApiKeyInput.text.toString().trim(),
            autoSetBio = autoSetBioCheck.isChecked,
            customBio = bioInput.text.toString().trim(),
            autoSetUsername = autoSetUsernameCheck.isChecked,
            precheckProxy = precheckProxyCheck.isChecked
        )
        GgtgRegistration.saveConfig(currentConfig)

        startButton.visibility = View.GONE
        runningButtonsRow.visibility = View.VISIBLE
        updatePauseButton(false)
        setStatus(statusTitleView, "Авторегистрация запущена...", R.drawable.msg_recent)

        GgtgRegistration.Engine.start(currentConfig)
    }

    private fun stopRegistration() {
        GgtgRegistration.Engine.stop(userRequested = true)
        startButton.visibility = View.VISIBLE
        runningButtonsRow.visibility = View.GONE
    }


    private fun bindAutoSave(input: EditText, updater: (String) -> Unit) {
        input.addTextChangedListener(object : android.text.TextWatcher {
            override fun beforeTextChanged(s: CharSequence?, start: Int, count: Int, after: Int) {}
            override fun onTextChanged(s: CharSequence?, start: Int, before: Int, count: Int) {}
            override fun afterTextChanged(s: android.text.Editable?) {
                updater(s?.toString()?.trim() ?: "")
                GgtgRegistration.saveConfig(currentConfig)
            }
        })
    }

    private fun getProxyDisplayTitle(): String {
        val name = currentConfig.proxyListName
        if (name.isEmpty()) return "Без прокси (напрямую)"
        val count = GgtgProxyPoolStore.getCount(name)
        return "$name ($count шт.)"
    }

    private fun pickNamesFile(requestCode: Int) {
        try {
            val intent = Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
                addCategory(Intent.CATEGORY_OPENABLE)
                type = "*/*"
                putExtra(Intent.EXTRA_MIME_TYPES, arrayOf("text/plain", "text/*", "application/octet-stream"))
            }
            startActivityForResult(intent, requestCode)
        } catch (_: Exception) {
            try {
                val fallbackIntent = Intent(Intent.ACTION_GET_CONTENT).apply {
                    type = "*/*"
                }
                startActivityForResult(fallbackIntent, requestCode)
            } catch (e2: Exception) {
                Toast.makeText(parentActivity, "Не удалось открыть выбор файла: ${e2.message}", Toast.LENGTH_SHORT).show()
            }
        }
    }

    private fun handlePickedFile(uri: Uri, isFirstName: Boolean) {
        val act = parentActivity ?: ApplicationLoader.applicationContext ?: return
        try {
            var fileName = if (isFirstName) "names_first.txt" else "names_last.txt"
            act.contentResolver.query(uri, null, null, null, null)?.use { cursor ->
                if (cursor.moveToFirst()) {
                    val nameIndex = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME)
                    if (nameIndex >= 0) {
                        fileName = cursor.getString(nameIndex) ?: fileName
                    }
                }
            }

            val prefix = if (isFirstName) "first" else "last"
            val destFile = File(act.filesDir, "reg_${prefix}_${System.currentTimeMillis()}_$fileName")
            act.contentResolver.openInputStream(uri)?.use { input ->
                destFile.outputStream().use { output ->
                    input.copyTo(output)
                }
            }

            val path = destFile.absolutePath
            val count = GgtgRegistration.readFileLineCount(path)
            if (isFirstName) {
                currentConfig = currentConfig.copy(firstNameFile = path)
            } else {
                currentConfig = currentConfig.copy(lastNameFile = path)
            }
            GgtgRegistration.saveConfig(currentConfig)
            updateNamesFileLabels()
            Toast.makeText(act, "Загружено строк: $count", Toast.LENGTH_SHORT).show()
        } catch (e: Exception) {
            Toast.makeText(act, "Ошибка при открытии файла: ${e.message}", Toast.LENGTH_LONG).show()
        }
    }

    private fun formatNamesCount(count: Int): String {
        return "$count " + when {
            count % 10 == 1 && count % 100 != 11 -> "строка"
            count % 10 in 2..4 && (count % 100 !in 12..14) -> "строки"
            else -> "строк"
        }
    }

    private fun updateNamesFileLabels() {
        if (!::firstNameFileStatusText.isInitialized) return
        val fnPath = currentConfig.firstNameFile
        if (fnPath.isNotEmpty() && File(fnPath).exists()) {
            val fnFile = File(fnPath)
            val lines = GgtgRegistration.readFileLineCount(fnPath)
            firstNameFileStatusText.text = "Файл: ${fnFile.name} (${formatNamesCount(lines)})"
            firstNameFileClearBtn.visibility = View.VISIBLE
        } else {
            firstNameFileStatusText.text = "Файл не выбран (случайные имена)"
            firstNameFileClearBtn.visibility = View.GONE
        }

        val lnPath = currentConfig.lastNameFile
        if (lnPath.isNotEmpty() && File(lnPath).exists()) {
            val lnFile = File(lnPath)
            val lines = GgtgRegistration.readFileLineCount(lnPath)
            lastNameFileStatusText.text = "Файл: ${lnFile.name} (${formatNamesCount(lines)})"
            lastNameFileClearBtn.visibility = View.VISIBLE
        } else {
            lastNameFileStatusText.text = "Файл не выбран (случайные фамилии)"
            lastNameFileClearBtn.visibility = View.GONE
        }
    }

    override fun onActivityResultFragment(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResultFragment(requestCode, resultCode, data)
        if (resultCode == Activity.RESULT_OK && data != null) {
            val uri = data.data ?: return
            if (requestCode == REQ_PICK_FIRST_NAMES) {
                handlePickedFile(uri, isFirstName = true)
            } else if (requestCode == REQ_PICK_LAST_NAMES) {
                handlePickedFile(uri, isFirstName = false)
            }
        }
    }

    private fun saveAllFromUi() {
        try {
            val key = if (::apiKeyInput.isInitialized) apiKeyInput.text.toString().trim() else currentConfig.apiKey
            val target = if (::targetCountInput.isInitialized) targetCountInput.text.toString().trim().toIntOrNull() ?: currentConfig.targetSuccessCount else currentConfig.targetSuccessCount
            val threads = if (::threadsCountInput.isInitialized) threadsCountInput.text.toString().trim().toIntOrNull() ?: currentConfig.threadsCount else currentConfig.threadsCount
            val folder = if (::folderInput.isInitialized) folderInput.text.toString().trim() else currentConfig.folderTag
            val twoFa = if (::twoFaInput.isInitialized) twoFaInput.text.toString().trim() else currentConfig.twoFactorPassword
            val single2fa = if (::single2faCheck.isInitialized) single2faCheck.isChecked else currentConfig.single2faForAll
            val singleName = if (::singleNameCheck.isInitialized) singleNameCheck.isChecked else currentConfig.singleNameForAll
            val custFirst = if (::firstNameInput.isInitialized) firstNameInput.text.toString().trim() else currentConfig.customFirstName
            val custLast = if (::lastNameInput.isInitialized) lastNameInput.text.toString().trim() else currentConfig.customLastName

            val stepMin = if (::stepDelayMinInput.isInitialized) stepDelayMinInput.text.toString().trim().toIntOrNull() ?: currentConfig.stepDelayMin else currentConfig.stepDelayMin
            val stepMax = if (::stepDelayMaxInput.isInitialized) stepDelayMaxInput.text.toString().trim().toIntOrNull() ?: currentConfig.stepDelayMax else currentConfig.stepDelayMax
            val accMin = if (::accountDelayMinInput.isInitialized) accountDelayMinInput.text.toString().trim().toIntOrNull() ?: currentConfig.accountDelayMin else currentConfig.accountDelayMin
            val accMax = if (::accountDelayMaxInput.isInitialized) accountDelayMaxInput.text.toString().trim().toIntOrNull() ?: currentConfig.accountDelayMax else currentConfig.accountDelayMax
            val smsWait = if (::smsWaitInput.isInitialized) smsWaitInput.text.toString().trim().toIntOrNull() ?: currentConfig.smsWaitSeconds else currentConfig.smsWaitSeconds

            val skipApp = if (::skipAppCheck.isInitialized) skipAppCheck.isChecked else currentConfig.skipAppCode
            val filterNum = if (::filterCheck.isInitialized) filterCheck.isChecked else currentConfig.filterNumbers
            val replToken = if (::replaceTokenCheck.isInitialized) replaceTokenCheck.isChecked else currentConfig.replaceTokenOnFlood
            val autoBuy = if (::autoBuyEmailCheck.isInitialized) autoBuyEmailCheck.isChecked else currentConfig.autoBuyEmail
            val emailKey = if (::emailApiKeyInput.isInitialized) emailApiKeyInput.text.toString().trim() else currentConfig.emailApiKey
            val op = currentConfig.smsOperator
            val maxP = if (::maxPriceInput.isInitialized) maxPriceInput.text.toString().trim() else currentConfig.maxPrice
            val resetSess = if (::resetSessionsCheck.isInitialized) resetSessionsCheck.isChecked else currentConfig.resetSessionsAfterReg
            val reset2fa = if (::reset2faCheck.isInitialized) reset2faCheck.isChecked else currentConfig.reset2faOnOccupied
            val tokenKey = if (::tokenApiKeyInput.isInitialized) tokenApiKeyInput.text.toString().trim() else currentConfig.tokenApiKey

            currentConfig = currentConfig.copy(
                apiKey = key,
                targetSuccessCount = target,
                threadsCount = threads,
                folderTag = folder,
                single2faForAll = single2fa,
                twoFactorPassword = twoFa,
                singleNameForAll = singleName,
                customFirstName = custFirst,
                customLastName = custLast,
                autoRandomNames = !singleName,
                stepDelayMin = stepMin,
                stepDelayMax = stepMax,
                accountDelayMin = accMin,
                accountDelayMax = accMax,
                smsWaitSeconds = smsWait,
                skipAppCode = skipApp,
                filterNumbers = filterNum,
                replaceTokenOnFlood = replToken,
                autoBuyEmail = autoBuy,
                emailApiKey = emailKey,
                smsOperator = op,
                maxPrice = maxP,
                resetSessionsAfterReg = resetSess,
                reset2faOnOccupied = reset2fa,
                tokenApiKey = tokenKey,
                autoSetBio = if (::autoSetBioCheck.isInitialized) autoSetBioCheck.isChecked else currentConfig.autoSetBio,
                customBio = if (::bioInput.isInitialized) bioInput.text.toString().trim() else currentConfig.customBio,
                autoSetUsername = if (::autoSetUsernameCheck.isInitialized) autoSetUsernameCheck.isChecked else currentConfig.autoSetUsername,
                precheckProxy = if (::precheckProxyCheck.isInitialized) precheckProxyCheck.isChecked else currentConfig.precheckProxy
            )
            GgtgRegistration.saveConfig(currentConfig)
        } catch (_: Exception) {}
    }

    override fun onResume() {
        super.onResume()
        if (::proxyView.isInitialized) {
            proxyView.text = getProxyDisplayTitle()
        }
        updateNamesFileLabels()
    }

    override fun onPause() {
        super.onPause()
        saveAllFromUi()
    }

    override fun onFragmentDestroy() {
        saveAllFromUi()
        super.onFragmentDestroy()
        // Do not stop registration if user simply navigates back; engine runs in background!
        // Just detach UI callbacks so there is no view leak
        GgtgRegistration.Engine.onLog = null
        GgtgRegistration.Engine.onStatus = null
        GgtgRegistration.Engine.onProgress = null
        GgtgRegistration.Engine.onFinished = null
    }
}
