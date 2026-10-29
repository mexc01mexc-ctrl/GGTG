package app.ggtg

import android.app.Activity
import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.content.Intent
import android.graphics.Typeface
import android.net.Uri
import android.provider.OpenableColumns
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

    // Step 1: SMS Service & Number
    private lateinit var serviceNameView: TextView
    private lateinit var apiKeyInput: EditText
    private lateinit var balanceText: TextView
    private lateinit var countryView: TextView
    private lateinit var operatorView: TextView
    private lateinit var maxPriceInput: EditText
    private lateinit var filterCheck: CheckBox

    // Step 2: Proxy & Storage
    private lateinit var proxyView: TextView
    private lateinit var folderInput: EditText
    private lateinit var precheckProxyCheck: CheckBox

    // Step 3: Profile & 2FA
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
    private lateinit var single2faCheck: CheckBox
    private lateinit var single2faLayout: LinearLayout
    private lateinit var random2faLayout: LinearLayout
    private lateinit var twoFaInput: EditText

    // Step 4: Protection & Tokens
    private lateinit var tokenServiceView: TextView
    private lateinit var tokenApiKeyInput: EditText
    private lateinit var aidInput: EditText
    private lateinit var aidLayout: LinearLayout
    private lateinit var tokenBalanceText: TextView
    private lateinit var replaceTokenCheck: CheckBox
    private lateinit var captchaServiceView: TextView
    private lateinit var captchaApiKeyInput: EditText
    private lateinit var autoBuyEmailCheck: CheckBox
    private lateinit var emailServiceView: TextView
    private lateinit var emailApiKeyInput: EditText

    // Step 5: Plan & Execution
    private lateinit var targetCountInput: EditText
    private lateinit var threadsCountInput: EditText
    private lateinit var stepDelayMinInput: EditText
    private lateinit var stepDelayMaxInput: EditText
    private lateinit var accountDelayMinInput: EditText
    private lateinit var accountDelayMaxInput: EditText
    private lateinit var smsWaitInput: EditText
    private lateinit var skipAppCheck: CheckBox
    private lateinit var resetSessionsCheck: CheckBox
    private lateinit var reset2faCheck: CheckBox
    private lateinit var statusTitleView: TextView
    private lateinit var progressLabelView: TextView
    private lateinit var startButton: View
    private lateinit var runningButtonsRow: LinearLayout
    private lateinit var pauseButton: TextView
    private lateinit var stopButton: TextView
    private lateinit var refundQueueLabel: TextView
    private lateinit var refundQueueBtn: View

    // Live Log
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

        buildRegistrationUi(context, root)

        root.addView(GgtgUi.spacer(context, 24f))

        setupRegistrationCallbacks()

        return frame
    }

    // =============================================================================
    // BUILD REGISTRATION UI (Steps 1 to 5 + Live Log)
    // =============================================================================
    private fun buildRegistrationUi(context: Context, container: LinearLayout) {
        // -------------------------------------------------------------------------
        // ШАГ 1: SMS-сервис и номер
        // -------------------------------------------------------------------------
        container.addView(sectionHeader(context, "Шаг 1: SMS-сервис и номер", "Сервис, ключ, баланс, страна и оператор связи", R.drawable.msg2_devices))
        val s1Card = GgtgUi.card(context)

        val currentServiceName = GgtgRegistration.SERVICES.firstOrNull { it.id == currentConfig.serviceId }?.name ?: GgtgRegistration.SERVICES.first().name
        serviceNameView = pickerRow(context, s1Card, "SMS-сервис", currentServiceName) { showServicePicker(context) }

        apiKeyInput = GgtgUi.input(context, "API-ключ сервиса", currentConfig.apiKey)
        GgtgUi.labeled(context, s1Card, "API-ключ сервиса", "Сохраняется отдельно для каждого сервиса, вводить заново не нужно", apiKeyInput)
        apiKeyInput.addTextChangedListener(object : android.text.TextWatcher {
            override fun beforeTextChanged(s: CharSequence?, start: Int, count: Int, after: Int) {}
            override fun onTextChanged(s: CharSequence?, start: Int, before: Int, count: Int) {}
            override fun afterTextChanged(s: android.text.Editable?) {
                val key = s.toString().trim()
                currentConfig = currentConfig.copy(apiKey = key)
                GgtgRegistration.saveApiKeyFor(currentConfig.serviceId, key)
                GgtgRegistration.saveConfig(currentConfig)
            }
        })

        val balanceRow = LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding(0, GgtgUi.dp(6f), 0, GgtgUi.dp(6f))
        }
        val checkBalBtn = GgtgUi.withLeadingIcon(GgtgUi.pill(context, "Проверить баланс", 0xFFFFFFFF.toInt(), GgtgUi.accent), R.drawable.msg_stats, 0xFFFFFFFF.toInt(), 13f, 5f)
        checkBalBtn.setOnClickListener { checkBalance() }
        balanceRow.addView(checkBalBtn)
        balanceText = GgtgUi.body(context, "  Баланс: —", 13f)
        balanceRow.addView(balanceText)
        s1Card.addView(balanceRow)

        countryView = pickerRow(context, s1Card, "Страна номеров", GgtgRegistration.currentCountryTitle(currentConfig)) { showCountryPicker(context) }

        val currentOpTitle = GgtgRegistration.operatorDisplayTitle(currentConfig.smsOperator)
        operatorView = pickerRow(context, s1Card, "Оператор связи", currentOpTitle) { showOperatorPicker(context) }

        maxPriceInput = GgtgUi.input(context, "без ограничения", currentConfig.maxPrice)
        bindAutoSave(maxPriceInput) { currentConfig = currentConfig.copy(maxPrice = it) }
        GgtgUi.labeled(context, s1Card, "Макс. цена номера", "Максимальная стоимость (например 15.00). Пусто = без ограничения", maxPriceInput)

        filterCheck = checkBox(context, "Фильтровать номера перед регистрацией", currentConfig.filterNumbers)
        filterCheck.setOnCheckedChangeListener { _, isChecked ->
            currentConfig = currentConfig.copy(filterNumbers = isChecked)
            GgtgRegistration.saveConfig(currentConfig)
        }
        s1Card.addView(filterCheck)
        s1Card.addView(GgtgUi.body(context, "Программа помнит номера, которые уже не подошли (заняты, заблокированы, неверные), и сразу отменяет их при повторной выдаче.", 12f))

        GgtgUi.addCard(container, s1Card)

        // -------------------------------------------------------------------------
        // ШАГ 2: Прокси и сохранение
        // -------------------------------------------------------------------------
        container.addView(sectionHeader(context, "Шаг 2: Прокси и сохранение", "Пулы прокси и папка для сохранения аккаунтов", R.drawable.msg2_permissions))
        val s2Card = GgtgUi.card(context)

        proxyView = pickerRow(context, s2Card, "Список прокси", getProxyDisplayTitle()) { showProxyPicker(context) }

        val proxyManageRow = LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL
            setPadding(0, GgtgUi.dp(4f), 0, GgtgUi.dp(6f))
        }
        val manageProxyBtn = GgtgUi.withLeadingIcon(
            GgtgUi.pill(context, "Управление списками прокси", 0xFFFFFFFF.toInt(), GgtgUi.accent),
            R.drawable.msg_settings, 0xFFFFFFFF.toInt(), 13f, 5f
        )
        manageProxyBtn.setOnClickListener {
            presentFragment(GgtgProxyActivity())
        }
        proxyManageRow.addView(manageProxyBtn)
        s2Card.addView(proxyManageRow)

        folderInput = GgtgUi.input(context, "Регистрация", currentConfig.folderTag)
        bindAutoSave(folderInput) { currentConfig = currentConfig.copy(folderTag = it) }
        GgtgUi.labeled(context, s2Card, "Папка в GGTG", "Куда сохранять новые аккаунты", folderInput)

        precheckProxyCheck = checkBox(context, "Проверять прокси перед заказом номера", currentConfig.precheckProxy)
        precheckProxyCheck.setOnCheckedChangeListener { _, isChecked ->
            currentConfig = currentConfig.copy(precheckProxy = isChecked)
            GgtgRegistration.saveConfig(currentConfig)
        }
        s2Card.addView(precheckProxyCheck)
        s2Card.addView(GgtgUi.body(context, "Исключает списание средств за номер, если прокси из пула внезапно перестал отвечать.", 12f))

        GgtgUi.addCard(container, s2Card)

        // -------------------------------------------------------------------------
        // ШАГ 3: Профиль и безопасность (2FA)
        // -------------------------------------------------------------------------
        container.addView(sectionHeader(context, "Шаг 3: Профиль и безопасность (2FA)", "Имена, фамилии, пол, Bio, юзернейм и пароль 2FA", R.drawable.msg_contacts))
        val s3Card = GgtgUi.card(context)

        singleNameCheck = checkBox(context, "Одно имя и фамилия на все аккаунты", currentConfig.singleNameForAll)
        s3Card.addView(singleNameCheck)
        s3Card.addView(GgtgUi.body(context, "Если включено — все создаваемые аккаунты получат указанные имя и фамилию. Если выключено — имена генерируются автоматически или берутся из файлов.", 12f))

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
        s3Card.addView(singleNameLayout)

        randomNamesLayout = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(0, GgtgUi.dp(6f), 0, 0)
            visibility = if (currentConfig.singleNameForAll) View.GONE else View.VISIBLE
        }
        val genderLabel = when (currentConfig.gender) { 1 -> "Мужской"; 2 -> "Женский"; else -> "Случайный (М/Ж)" }
        genderView = pickerRow(context, randomNamesLayout, "Пол", genderLabel) { showGenderPicker(context) }

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

        s3Card.addView(randomNamesLayout)

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
        s3Card.addView(autoSetUsernameCheck)
        s3Card.addView(GgtgUi.body(context, "Генерирует уникальный юзернейм на основе имени и фамилии (например, ivan_ivanov_842).", 12f))

        autoSetBioCheck = checkBox(context, "Заполнять описание профиля (Bio)", currentConfig.autoSetBio)
        autoSetBioCheck.setOnCheckedChangeListener { _, isChecked ->
            currentConfig = currentConfig.copy(autoSetBio = isChecked)
            GgtgRegistration.saveConfig(currentConfig)
        }
        s3Card.addView(autoSetBioCheck)
        bioInput = GgtgUi.input(context, "Текст Bio (или оставьте пустым для случайного)", currentConfig.customBio)
        bindAutoSave(bioInput) { currentConfig = currentConfig.copy(customBio = it) }
        GgtgUi.labeled(context, s3Card, "Описание профиля (Bio)", "Оставьте пустым для реалистичных случайных фраз, либо укажите свой шаблон (поддерживает рандомизацию {A|B})", bioInput)

        // 2FA in Step 3
        val divFa = View(context).apply { setBackgroundColor(GgtgUi.color(Theme.key_divider)) }
        s3Card.addView(divFa, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 1).apply {
            topMargin = GgtgUi.dp(10f)
            bottomMargin = GgtgUi.dp(10f)
        })

        s3Card.addView(GgtgUi.title(context, "Облачный пароль (2FA)", 14f))
        single2faCheck = checkBox(context, "Один пароль на все аккаунты", currentConfig.single2faForAll)
        s3Card.addView(single2faCheck)
        s3Card.addView(GgtgUi.body(context, "Если включено — на всех создаваемых аккаунтах будет установлен указанный пароль. Если выключено — для каждого аккаунта генерируется свой уникальный надёжный пароль (18 символов), автоматически сохраняемый в 2FA Vault.", 12f))

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
        s3Card.addView(single2faLayout)

        random2faLayout = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(0, GgtgUi.dp(6f), 0, 0)
            visibility = if (currentConfig.single2faForAll) View.GONE else View.VISIBLE
        }
        random2faLayout.addView(GgtgUi.body(context, "Для каждого аккаунта будет сгенерирован свой уникальный надежный пароль 2FA и сохранен в хранилище приложения (2FA Vault).", 13f))
        s3Card.addView(random2faLayout)

        single2faCheck.setOnCheckedChangeListener { _, isChecked ->
            currentConfig = currentConfig.copy(single2faForAll = isChecked)
            GgtgRegistration.saveConfig(currentConfig)
            single2faLayout.visibility = if (isChecked) View.VISIBLE else View.GONE
            random2faLayout.visibility = if (isChecked) View.GONE else View.VISIBLE
        }

        GgtgUi.addCard(container, s3Card)

        // -------------------------------------------------------------------------
        // ШАГ 4: Защита и токены (Push, Captcha, Email)
        // -------------------------------------------------------------------------
        container.addView(sectionHeader(context, "Шаг 4: Защита и токены (Push, Captcha, Email)", "Play Integrity, reCAPTCHA Enterprise и подтверждение Email", R.drawable.msg_secret))
        val s4Card = GgtgUi.card(context)

        val currentTokenName = GgtgRegistration.tokenServiceName(currentConfig.tokenService)
        tokenServiceView = pickerRow(context, s4Card, "Сервис токенов Play Integrity", currentTokenName) { showTokenServicePicker(context) }

        tokenApiKeyInput = GgtgUi.input(context, "API-ключ токенов", currentConfig.tokenApiKey)
        bindAutoSave(tokenApiKeyInput) { currentConfig = currentConfig.copy(tokenApiKey = it) }
        GgtgUi.labeled(context, s4Card, "API-ключ токенов", "Master API key сервиса TokenFast или AntiSafety", tokenApiKeyInput)

        aidLayout = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            visibility = if (currentConfig.tokenService == 2) View.VISIBLE else View.GONE
        }
        aidInput = GgtgUi.input(context, "308aba4e-5680-466b-81a5-477ac6befa95", currentConfig.antiSafetyAid)
        bindAutoSave(aidInput) { currentConfig = currentConfig.copy(antiSafetyAid = it) }
        GgtgUi.labeled(context, aidLayout, "AID аккаунта AntiSafety", "Идентификатор аккаунта antisafety.net", aidInput)
        s4Card.addView(aidLayout)

        val tokenBalanceRow = LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding(0, GgtgUi.dp(6f), 0, GgtgUi.dp(6f))
        }
        val checkTokenBalBtn = GgtgUi.withLeadingIcon(GgtgUi.pill(context, "Проверить баланс", 0xFFFFFFFF.toInt(), GgtgUi.accent), R.drawable.msg_stats, 0xFFFFFFFF.toInt(), 13f, 5f)
        checkTokenBalBtn.setOnClickListener { checkTokenBalance() }
        tokenBalanceRow.addView(checkTokenBalBtn)
        tokenBalanceText = GgtgUi.body(context, "  Баланс: —", 13f)
        tokenBalanceRow.addView(tokenBalanceText)
        s4Card.addView(tokenBalanceRow)

        replaceTokenCheck = checkBox(context, "Заменять токен при лимите флуда (FLOOD)", currentConfig.replaceTokenOnFlood)
        replaceTokenCheck.setOnCheckedChangeListener { _, isChecked ->
            currentConfig = currentConfig.copy(replaceTokenOnFlood = isChecked)
            GgtgRegistration.saveConfig(currentConfig)
        }
        s4Card.addView(replaceTokenCheck)

        val divCap = View(context).apply { setBackgroundColor(GgtgUi.color(Theme.key_divider)) }
        s4Card.addView(divCap, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 1).apply {
            topMargin = GgtgUi.dp(8f)
            bottomMargin = GgtgUi.dp(8f)
        })

        // Captcha
        val currentCaptchaName = GgtgRegistration.captchaServiceName(currentConfig.captchaService)
        captchaServiceView = pickerRow(context, s4Card, "Обход reCAPTCHA Enterprise", currentCaptchaName) { showCaptchaServicePicker(context) }

        captchaApiKeyInput = GgtgUi.input(context, "пусто = ключ сервиса токенов", currentConfig.captchaApiKey)
        bindAutoSave(captchaApiKeyInput) { currentConfig = currentConfig.copy(captchaApiKey = it) }
        GgtgUi.labeled(context, s4Card, "API-ключ капчи", "Если отличается от ключа сервиса токенов", captchaApiKeyInput)

        val divMail = View(context).apply { setBackgroundColor(GgtgUi.color(Theme.key_divider)) }
        s4Card.addView(divMail, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 1).apply {
            topMargin = GgtgUi.dp(8f)
            bottomMargin = GgtgUi.dp(8f)
        })

        // Email
        autoBuyEmailCheck = checkBox(context, "Автопокупка Email при требовании Telegram", currentConfig.autoBuyEmail)
        s4Card.addView(autoBuyEmailCheck)
        s4Card.addView(GgtgUi.body(context, "Если Telegram требует указать Email для получения кода, программа автоматически купит и подтвердит временную почту.", 12f))

        val currentEmailName = GgtgRegistration.emailServiceName(currentConfig.emailService)
        emailServiceView = pickerRow(context, s4Card, "Сервис почты", currentEmailName) { showEmailServicePicker(context) }

        emailApiKeyInput = GgtgUi.input(context, "пусто = такой же как у SMS-сервиса", currentConfig.emailApiKey)
        bindAutoSave(emailApiKeyInput) { currentConfig = currentConfig.copy(emailApiKey = it) }
        GgtgUi.labeled(context, s4Card, "API-ключ почты (необязательно)", "Отдельный API-ключ для сервиса почты, если отличается от SMS-сервиса", emailApiKeyInput)

        GgtgUi.addCard(container, s4Card)

        // -------------------------------------------------------------------------
        // ШАГ 5: План и запуск
        // -------------------------------------------------------------------------
        container.addView(sectionHeader(context, "Шаг 5: План и запуск", "Объём, потоки, задержки, сбросы и управление", R.drawable.msg_stats))
        val s5Card = GgtgUi.card(context)

        targetCountInput = GgtgUi.input(context, "10", currentConfig.targetSuccessCount.toString(), numeric = true)
        bindAutoSave(targetCountInput) {
            val v = it.toIntOrNull()?.coerceAtLeast(1) ?: 10
            currentConfig = currentConfig.copy(targetSuccessCount = v)
        }
        GgtgUi.labeled(context, s5Card, "Целевое количество успешных аккаунтов", "Программа работает в цикле, пока не достигнет этой цели", targetCountInput)

        threadsCountInput = GgtgUi.input(context, "2", currentConfig.threadsCount.toString(), numeric = true)
        bindAutoSave(threadsCountInput) {
            val v = it.toIntOrNull()?.coerceIn(1, 10) ?: 2
            currentConfig = currentConfig.copy(threadsCount = v)
        }
        GgtgUi.labeled(context, s5Card, "Параллельных потоков", "Сколько аккаунтов регистрируются одновременно (1–10)", threadsCountInput)

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
        GgtgUi.labeled(context, s5Card, "Пауза между шагами регистрации", "Интервал случайной задержки между действиями", stepDelayRow)

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
        GgtgUi.labeled(context, s5Card, "Пауза между аккаунтами", "Интервал задержки перед запуском следующего номера", accDelayRow)

        smsWaitInput = GgtgUi.input(context, "300", currentConfig.smsWaitSeconds.toString(), numeric = true)
        bindAutoSave(smsWaitInput) {
            val v = it.toIntOrNull()?.coerceIn(120, 1800) ?: 300
            currentConfig = currentConfig.copy(smsWaitSeconds = v)
        }
        GgtgUi.labeled(context, s5Card, "Сколько ждать SMS-код (сек)", "От 120 до 1800, рекомендуется 300. Если SMS не придёт, деньги возвращаются", smsWaitInput)

        skipAppCheck = checkBox(context, "Не регистрировать трасты (отменять номер, если код в приложение)", currentConfig.skipAppCode)
        skipAppCheck.setOnCheckedChangeListener { _, isChecked ->
            currentConfig = currentConfig.copy(skipAppCode = isChecked)
            GgtgRegistration.saveConfig(currentConfig)
        }
        s5Card.addView(skipAppCheck)

        resetSessionsCheck = checkBox(context, "Сбросить другие сессии после регистрации", currentConfig.resetSessionsAfterReg)
        resetSessionsCheck.setOnCheckedChangeListener { _, isChecked ->
            currentConfig = currentConfig.copy(resetSessionsAfterReg = isChecked)
            GgtgRegistration.saveConfig(currentConfig)
        }
        s5Card.addView(resetSessionsCheck)

        reset2faCheck = checkBox(context, "Запросить сброс 2FA на занятых номерах", currentConfig.reset2faOnOccupied)
        reset2faCheck.setOnCheckedChangeListener { _, isChecked ->
            currentConfig = currentConfig.copy(reset2faOnOccupied = isChecked)
            GgtgRegistration.saveConfig(currentConfig)
        }
        s5Card.addView(reset2faCheck)

        val divSt = View(context).apply { setBackgroundColor(GgtgUi.color(Theme.key_divider)) }
        s5Card.addView(divSt, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 1).apply {
            topMargin = GgtgUi.dp(10f)
            bottomMargin = GgtgUi.dp(10f)
        })

        statusTitleView = GgtgUi.title(context, "Готов к запуску", 16f)
        setStatus(statusTitleView, "Готов к запуску", R.drawable.msg_info)
        s5Card.addView(statusTitleView)

        progressLabelView = GgtgUi.body(context, "Успешно: 0 / ${currentConfig.targetSuccessCount} | Потоков: 0 | Ошибок: 0", 14f)
        s5Card.addView(progressLabelView, LinearLayout.LayoutParams(LayoutHelper.MATCH_PARENT, LayoutHelper.WRAP_CONTENT).apply {
            topMargin = GgtgUi.dp(4f)
        })

        val startBtn = GgtgUi.button(context, "Запустить регистрацию", false) { startRegistration() }
        startBtn.setCompoundDrawablesWithIntrinsicBounds(R.drawable.msg_select, 0, 0, 0)
        startBtn.compoundDrawablePadding = GgtgUi.dp(8f)
        startBtn.setPadding(GgtgUi.dp(16f), GgtgUi.dp(10f), GgtgUi.dp(16f), GgtgUi.dp(10f))
        startButton = startBtn
        s5Card.addView(startButton, LinearLayout.LayoutParams(LayoutHelper.MATCH_PARENT, LayoutHelper.WRAP_CONTENT).apply {
            topMargin = GgtgUi.dp(12f)
        })

        runningButtonsRow = LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL
            visibility = View.GONE
        }
        val pauseBtn = GgtgUi.button(context, "Пауза", false) {
            GgtgRegistration.Engine.togglePause()
        }
        pauseBtn.setCompoundDrawablesWithIntrinsicBounds(R.drawable.ic_pause, 0, 0, 0)
        pauseBtn.compoundDrawablePadding = GgtgUi.dp(8f)
        pauseBtn.setPadding(GgtgUi.dp(16f), GgtgUi.dp(10f), GgtgUi.dp(16f), GgtgUi.dp(10f))
        pauseButton = pauseBtn

        val stopBtn = GgtgUi.button(context, "Остановить", true) {
            stopRegistration()
        }
        stopBtn.setCompoundDrawablesWithIntrinsicBounds(R.drawable.msg_close, 0, 0, 0)
        stopBtn.compoundDrawablePadding = GgtgUi.dp(8f)
        stopBtn.setPadding(GgtgUi.dp(16f), GgtgUi.dp(10f), GgtgUi.dp(16f), GgtgUi.dp(10f))
        stopButton = stopBtn

        runningButtonsRow.addView(pauseButton, LinearLayout.LayoutParams(0, LayoutHelper.WRAP_CONTENT, 1f).apply {
            rightMargin = GgtgUi.dp(6f)
        })
        runningButtonsRow.addView(stopButton, LinearLayout.LayoutParams(0, LayoutHelper.WRAP_CONTENT, 1f).apply {
            leftMargin = GgtgUi.dp(6f)
        })
        s5Card.addView(runningButtonsRow, LinearLayout.LayoutParams(LayoutHelper.MATCH_PARENT, LayoutHelper.WRAP_CONTENT).apply {
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
        s5Card.addView(refundQueueRow)

        GgtgUi.addCard(container, s5Card)

        // -------------------------------------------------------------------------
        // Журнал действий
        // -------------------------------------------------------------------------
        container.addView(sectionHeader(context, "Журнал действий", "Лог регистрации в реальном времени", R.drawable.msg_list))
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
        GgtgUi.addCard(container, logCard)
    }

    // =============================================================================
    // REGISTRATION ENGINE & CALLBACKS
    // =============================================================================
    private fun setupRegistrationCallbacks() {
        GgtgRegistration.Engine.onLog = { msg ->
            AndroidUtilities.runOnUIThread {
                if (::logConsoleView.isInitialized) {
                    val cur = logConsoleView.text.toString()
                    val lines = cur.lines()
                    val trimmed = if (lines.size > 250) lines.takeLast(200).joinToString("\n") else cur
                    logConsoleView.text = if (trimmed.isEmpty()) msg else "$trimmed\n$msg"
                }
            }
        }

        GgtgRegistration.Engine.onStatus = { st, isError, isSuccess ->
            AndroidUtilities.runOnUIThread {
                if (::statusTitleView.isInitialized) {
                    val icon = if (isError) R.drawable.msg_close else if (isSuccess) R.drawable.msg_select else R.drawable.msg_recent
                    setStatus(statusTitleView, st, icon)
                }
            }
        }

        GgtgRegistration.Engine.onProgress = { success, active, failed, target ->
            AndroidUtilities.runOnUIThread {
                if (::progressLabelView.isInitialized) {
                    progressLabelView.text = "Успешно: $success / $target | Потоков: $active | Ошибок: $failed"
                }
                val isRunning = GgtgRegistration.Engine.isRunning.get()
                if (::startButton.isInitialized) {
                    startButton.visibility = if (isRunning) View.GONE else View.VISIBLE
                }
                if (::runningButtonsRow.isInitialized) {
                    runningButtonsRow.visibility = if (isRunning) View.VISIBLE else View.GONE
                }
                updatePauseButton(GgtgRegistration.Engine.isPaused.get())
            }
        }

        GgtgRegistration.Engine.onFinished = { success, failed ->
            AndroidUtilities.runOnUIThread {
                if (::startButton.isInitialized) {
                    startButton.visibility = View.VISIBLE
                }
                if (::runningButtonsRow.isInitialized) {
                    runningButtonsRow.visibility = View.GONE
                }
            }
        }
    }

    // =============================================================================
    // PICKERS & DIALOGS
    // =============================================================================
    private fun showServicePicker(context: Context) {
        val names = GgtgRegistration.SERVICES.map { it.name }.toTypedArray()
        AlertDialog.Builder(context).apply {
            setTitle("Выберите SMS-сервис")
            setItems(names) { _, which ->
                val s = GgtgRegistration.SERVICES[which]
                val currentTypedKey = apiKeyInput.text.toString().trim()
                GgtgRegistration.saveApiKeyFor(currentConfig.serviceId, currentTypedKey)

                val restoredKey = GgtgRegistration.apiKeyFor(s.id)
                currentConfig = currentConfig.copy(
                    serviceId = s.id,
                    countryName = "",
                    apiKey = restoredKey,
                    smsOperator = ""
                )
                GgtgRegistration.saveConfig(currentConfig)

                apiKeyInput.setText(restoredKey)
                serviceNameView.text = s.name
                countryView.text = GgtgRegistration.currentCountryTitle(currentConfig)

                if (::operatorView.isInitialized) {
                    operatorView.text = GgtgRegistration.operatorDisplayTitle("")
                }
            }
            setNegativeButton("Отмена", null)
        }.show()
    }

    private fun showCountryPicker(context: Context) {
        val key = apiKeyInput.text.toString().trim()
        currentConfig = currentConfig.copy(apiKey = key)
        GgtgRegistration.saveApiKeyFor(currentConfig.serviceId, key)
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
        val key = apiKeyInput.text.toString().trim()
        currentConfig = currentConfig.copy(apiKey = key)
        GgtgRegistration.saveApiKeyFor(currentConfig.serviceId, key)
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

            val manualRow = GgtgUi.title(context, "Ввести код вручную...", 14f).apply {
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
                if (::proxyView.isInitialized) {
                    proxyView.text = getProxyDisplayTitle()
                }
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
                if (::genderView.isInitialized) {
                    genderView.text = options[which]
                }
                GgtgRegistration.saveConfig(currentConfig)
            }
            setNegativeButton("Отмена", null)
        }.show()
    }

    private fun showTokenServicePicker(context: Context) {
        val options = arrayOf("Нет (без токенов)", "TokenFast (tokenfast.shop/api)", "AntiSafety (antisafety.net)")
        AlertDialog.Builder(context).apply {
            setTitle("Выберите сервис токенов Play Integrity")
            setItems(options) { _, which ->
                currentConfig = currentConfig.copy(tokenService = which)
                if (::tokenServiceView.isInitialized) {
                    tokenServiceView.text = GgtgRegistration.tokenServiceName(which)
                }
                if (::aidLayout.isInitialized) {
                    aidLayout.visibility = if (which == 2) View.VISIBLE else View.GONE
                }
                GgtgRegistration.saveConfig(currentConfig)
            }
            setNegativeButton("Отмена", null)
        }.show()
    }

    private fun showCaptchaServicePicker(context: Context) {
        val options = arrayOf("Авто (по сервису токенов)", "TokenFast", "AntiSafety", "NextCaptcha")
        AlertDialog.Builder(context).apply {
            setTitle("Выберите сервис обхода капчи")
            setItems(options) { _, which ->
                currentConfig = currentConfig.copy(captchaService = which)
                if (::captchaServiceView.isInitialized) {
                    captchaServiceView.text = GgtgRegistration.captchaServiceName(which)
                }
                GgtgRegistration.saveConfig(currentConfig)
            }
            setNegativeButton("Отмена", null)
        }.show()
    }

    private fun showEmailServicePicker(context: Context) {
        val keys = arrayOf("auto", "hero-sms", "smsbower", "antisafety")
        val names = arrayOf("Авто", "Hero-SMS", "SMSBower", "AntiSafety")
        AlertDialog.Builder(context).apply {
            setTitle("Выберите сервис временной почты")
            setItems(names) { _, which ->
                currentConfig = currentConfig.copy(emailService = keys[which])
                if (::emailServiceView.isInitialized) {
                    emailServiceView.text = names[which]
                }
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
        GgtgRegistration.saveApiKeyFor(currentConfig.serviceId, key)
        GgtgRegistration.saveConfig(currentConfig)

        balanceText.text = "  Баланс: проверка..."

        scope.launch {
            val res = GgtgRegistration.fetchBalance(currentConfig)
            val txt = if (res.isSuccess) "  Баланс: ${res.getOrNull()}" else "  Ошибка: ${res.exceptionOrNull()?.message}"
            balanceText.text = txt

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
        if (::tokenBalanceText.isInitialized) tokenBalanceText.text = "  Баланс: проверка..."
        scope.launch {
            val res = GgtgRegistration.fetchTokenBalance(currentConfig.tokenService, key, currentConfig.antiSafetyAid)
            if (::tokenBalanceText.isInitialized) {
                tokenBalanceText.text = if (res.isSuccess) "  Баланс: ${res.getOrNull()}" else "  Ошибка: ${res.exceptionOrNull()?.message}"
            }
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
        val target = if (::targetCountInput.isInitialized) targetCountInput.text.toString().trim().toIntOrNull()?.coerceAtLeast(1) ?: 10 else 10
        val threads = if (::threadsCountInput.isInitialized) threadsCountInput.text.toString().trim().toIntOrNull()?.coerceIn(1, 10) ?: 2 else 2
        val folder = if (::folderInput.isInitialized) folderInput.text.toString().trim().ifEmpty { "Регистрация" } else "Регистрация"
        val twoFa = if (::twoFaInput.isInitialized) twoFaInput.text.toString().trim() else ""

        val stepMin = if (::stepDelayMinInput.isInitialized) stepDelayMinInput.text.toString().trim().toIntOrNull()?.coerceAtLeast(0) ?: 10 else 10
        val stepMax = if (::stepDelayMaxInput.isInitialized) stepDelayMaxInput.text.toString().trim().toIntOrNull()?.coerceAtLeast(stepMin) ?: 30 else 30
        val accMin = if (::accountDelayMinInput.isInitialized) accountDelayMinInput.text.toString().trim().toIntOrNull()?.coerceAtLeast(0) ?: 15 else 15
        val accMax = if (::accountDelayMaxInput.isInitialized) accountDelayMaxInput.text.toString().trim().toIntOrNull()?.coerceAtLeast(accMin) ?: 45 else 45

        currentConfig = currentConfig.copy(
            apiKey = key,
            targetSuccessCount = target,
            threadsCount = threads,
            folderTag = folder,
            single2faForAll = if (::single2faCheck.isInitialized) single2faCheck.isChecked else currentConfig.single2faForAll,
            twoFactorPassword = twoFa,
            singleNameForAll = if (::singleNameCheck.isInitialized) singleNameCheck.isChecked else currentConfig.singleNameForAll,
            customFirstName = if (::firstNameInput.isInitialized) firstNameInput.text.toString().trim() else currentConfig.customFirstName,
            customLastName = if (::lastNameInput.isInitialized) lastNameInput.text.toString().trim() else currentConfig.customLastName,
            autoRandomNames = if (::singleNameCheck.isInitialized) !singleNameCheck.isChecked else currentConfig.autoRandomNames,
            stepDelayMin = stepMin,
            stepDelayMax = stepMax,
            accountDelayMin = accMin,
            accountDelayMax = accMax,
            smsWaitSeconds = if (::smsWaitInput.isInitialized) smsWaitInput.text.toString().trim().toIntOrNull()?.coerceIn(120, 1800) ?: 300 else 300,
            skipAppCode = if (::skipAppCheck.isInitialized) skipAppCheck.isChecked else currentConfig.skipAppCode,
            filterNumbers = if (::filterCheck.isInitialized) filterCheck.isChecked else currentConfig.filterNumbers,
            replaceTokenOnFlood = if (::replaceTokenCheck.isInitialized) replaceTokenCheck.isChecked else currentConfig.replaceTokenOnFlood,
            autoBuyEmail = if (::autoBuyEmailCheck.isInitialized) autoBuyEmailCheck.isChecked else currentConfig.autoBuyEmail,
            emailApiKey = if (::emailApiKeyInput.isInitialized) emailApiKeyInput.text.toString().trim() else currentConfig.emailApiKey,
            smsOperator = currentConfig.smsOperator,
            maxPrice = if (::maxPriceInput.isInitialized) maxPriceInput.text.toString().trim() else currentConfig.maxPrice,
            gender = currentConfig.gender,
            firstNameFile = currentConfig.firstNameFile,
            lastNameFile = currentConfig.lastNameFile,
            resetSessionsAfterReg = if (::resetSessionsCheck.isInitialized) resetSessionsCheck.isChecked else currentConfig.resetSessionsAfterReg,
            reset2faOnOccupied = if (::reset2faCheck.isInitialized) reset2faCheck.isChecked else currentConfig.reset2faOnOccupied,
            tokenService = currentConfig.tokenService,
            tokenApiKey = if (::tokenApiKeyInput.isInitialized) tokenApiKeyInput.text.toString().trim() else currentConfig.tokenApiKey,
            antiSafetyAid = if (::aidInput.isInitialized) aidInput.text.toString().trim().ifEmpty { currentConfig.antiSafetyAid } else currentConfig.antiSafetyAid,
            autoSetBio = if (::autoSetBioCheck.isInitialized) autoSetBioCheck.isChecked else currentConfig.autoSetBio,
            customBio = if (::bioInput.isInitialized) bioInput.text.toString().trim() else currentConfig.customBio,
            autoSetUsername = if (::autoSetUsernameCheck.isInitialized) autoSetUsernameCheck.isChecked else currentConfig.autoSetUsername,
            precheckProxy = if (::precheckProxyCheck.isInitialized) precheckProxyCheck.isChecked else currentConfig.precheckProxy
        )
        GgtgRegistration.saveApiKeyFor(currentConfig.serviceId, key)
        GgtgRegistration.saveConfig(currentConfig)

        if (::startButton.isInitialized) startButton.visibility = View.GONE
        if (::runningButtonsRow.isInitialized) runningButtonsRow.visibility = View.VISIBLE
        updatePauseButton(false)
        if (::statusTitleView.isInitialized) setStatus(statusTitleView, "Авторегистрация запущена...", R.drawable.msg_recent)

        GgtgRegistration.Engine.start(currentConfig)
    }

    private fun stopRegistration() {
        GgtgRegistration.Engine.stop(userRequested = true)
        if (::startButton.isInitialized) startButton.visibility = View.VISIBLE
        if (::runningButtonsRow.isInitialized) runningButtonsRow.visibility = View.GONE
    }

    private fun updatePauseButton(paused: Boolean) {
        if (!::pauseButton.isInitialized) return
        pauseButton.text = if (paused) "Продолжить" else "Пауза"
    }

    private fun setStatus(view: TextView, text: String, iconRes: Int) {
        view.text = text
        view.setCompoundDrawablesWithIntrinsicBounds(iconRes, 0, 0, 0)
        view.compoundDrawablePadding = GgtgUi.dp(6f)
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
            val asfAid = if (::aidInput.isInitialized) aidInput.text.toString().trim() else currentConfig.antiSafetyAid

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
                antiSafetyAid = asfAid,
                autoSetBio = if (::autoSetBioCheck.isInitialized) autoSetBioCheck.isChecked else currentConfig.autoSetBio,
                customBio = if (::bioInput.isInitialized) bioInput.text.toString().trim() else currentConfig.customBio,
                autoSetUsername = if (::autoSetUsernameCheck.isInitialized) autoSetUsernameCheck.isChecked else currentConfig.autoSetUsername,
                precheckProxy = if (::precheckProxyCheck.isInitialized) precheckProxyCheck.isChecked else currentConfig.precheckProxy
            )
            GgtgRegistration.saveApiKeyFor(currentConfig.serviceId, key)
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
        GgtgRegistration.Engine.onLog = null
        GgtgRegistration.Engine.onStatus = null
        GgtgRegistration.Engine.onProgress = null
        GgtgRegistration.Engine.onFinished = null
    }

    // =============================================================================
    // UI HELPERS (Strict Telegram Style, No Emoji)
    // =============================================================================
    private fun sectionHeader(context: Context, title: String, subtitle: String, iconRes: Int): View {
        val row = LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding(GgtgUi.dp(16f), GgtgUi.dp(14f), GgtgUi.dp(16f), GgtgUi.dp(4f))
        }
        row.addView(GgtgUi.badge(context, iconRes, 28f), LinearLayout.LayoutParams(GgtgUi.dp(28f), GgtgUi.dp(28f)).apply {
            rightMargin = GgtgUi.dp(10f)
        })
        val texts = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
        }
        texts.addView(GgtgUi.title(context, title, 14f).apply {
            setTextColor(GgtgUi.accent)
        })
        texts.addView(GgtgUi.body(context, subtitle, 12f))
        row.addView(texts, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
        return row
    }

    private fun pickerRow(context: Context, card: LinearLayout, label: String, value: String, onClick: () -> Unit): TextView {
        val row = LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding(0, GgtgUi.dp(6f), 0, GgtgUi.dp(6f))
            setOnClickListener { onClick() }
        }
        val lbl = GgtgUi.body(context, label, 14f).apply {
            setTextColor(GgtgUi.color(Theme.key_windowBackgroundWhiteBlackText))
        }
        val valView = GgtgUi.body(context, value, 14f).apply {
            setTextColor(GgtgUi.accent)
            gravity = Gravity.END
        }
        row.addView(lbl, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
        row.addView(valView, LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT))
        val iv = android.widget.ImageView(context).apply {
            setImageResource(R.drawable.arrow_more)
            setColorFilter(GgtgUi.color(Theme.key_windowBackgroundWhiteGrayText))
        }
        row.addView(iv, LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply {
            leftMargin = GgtgUi.dp(6f)
        })
        card.addView(row)
        return valView
    }

    private fun checkBox(context: Context, text: String, checked: Boolean): CheckBox {
        return CheckBox(context).apply {
            this.text = text
            this.isChecked = checked
            setTextColor(GgtgUi.color(Theme.key_windowBackgroundWhiteBlackText))
            textSize = 14f
            setPadding(GgtgUi.dp(6f), GgtgUi.dp(6f), GgtgUi.dp(6f), GgtgUi.dp(6f))
        }
    }
}
