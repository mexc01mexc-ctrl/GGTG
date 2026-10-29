package app.ggtg

import android.content.Context
import android.graphics.Typeface
import android.view.Gravity
import android.view.View
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
import org.telegram.messenger.R
import org.telegram.ui.ActionBar.ActionBar.ActionBarMenuOnItemClick
import org.telegram.ui.ActionBar.AlertDialog
import org.telegram.ui.ActionBar.BaseFragment
import org.telegram.ui.ActionBar.Theme
import org.telegram.ui.Components.LayoutHelper
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

// GGTG: Dedicated Phone Migration Activity for Android — full feature parity with Mac version
class GgtgPhoneMigrationActivity(
    private val initialSlots: List<Int> = emptyList()
) : BaseFragment() {

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main)

    private lateinit var migAccountPicker: GgtgAccountPicker
    private lateinit var serviceNameView: TextView
    private lateinit var apiKeyInput: EditText
    private lateinit var balanceText: TextView
    private lateinit var countryView: TextView
    private lateinit var tokenServiceView: TextView
    private lateinit var tokenApiKeyInput: EditText
    private lateinit var aidLayout: LinearLayout
    private lateinit var aidInput: EditText
    private lateinit var tokenBalanceText: TextView
    private lateinit var captchaServiceView: TextView
    private lateinit var captchaApiKeyInput: EditText
    private lateinit var startButton: View
    private lateinit var statusView: TextView
    private lateinit var logConsoleView: TextView

    private var currentConfig = GgtgRegistration.loadConfig()

    override fun createView(context: Context): View {
        actionBar.setBackButtonImage(R.drawable.ic_ab_back)
        actionBar.setAllowOverlayTitle(true)
        actionBar.setTitle("Смена номера (миграция)")
        actionBar.setActionBarMenuOnItemClick(object : ActionBarMenuOnItemClick() {
            override fun onItemClick(id: Int) {
                if (id == -1) finishFragment()
            }
        })

        val frame = FrameLayout(context)
        frame.setBackgroundColor(Theme.getColor(Theme.key_windowBackgroundGray))
        fragmentView = frame

        val root = GgtgUi.page(context, frame)

        // -------------------------------------------------------------------------
        // 1. Правило безопасности (24 часа)
        // -------------------------------------------------------------------------
        val infoCard = GgtgUi.card(context)
        infoCard.addView(GgtgUi.title(context, "Правило безопасности Telegram", 15f))
        val infoText = GgtgUi.body(context,
            "Telegram требует отлёжку мобильной сессии не менее 24 часов перед сменой номера телефона.\n\n" +
            "Если у выбранного аккаунта ещё нет доверенной мобильной сессии старше 24 часов, программа автоматически создаст её (Android Session) и запустит таймер отлёжки. Повторите процедуру через 24 часа.",
            13f
        )
        infoText.setPadding(0, GgtgUi.dp(4f), 0, 0)
        infoCard.addView(infoText)
        GgtgUi.addCard(root, infoCard)

        // -------------------------------------------------------------------------
        // 2. Выбор аккаунтов
        // -------------------------------------------------------------------------
        root.addView(sectionHeader(context, "1. Аккаунты для смены номера", "Выберите аккаунты, на которых нужно заменить номер", R.drawable.msg_contacts))
        val pickCard = GgtgUi.card(context)
        migAccountPicker = GgtgAccountPicker(context)
        if (initialSlots.isNotEmpty()) {
            migAccountPicker.setSelected(initialSlots)
        }
        pickCard.addView(migAccountPicker)
        GgtgUi.addCard(root, pickCard)

        // -------------------------------------------------------------------------
        // 3. SMS-сервис и страна нового номера
        // -------------------------------------------------------------------------
        root.addView(sectionHeader(context, "2. SMS-сервис и новый номер", "Откуда покупать новый телефонный номер", R.drawable.msg2_devices))
        val srvCard = GgtgUi.card(context)

        val currentServiceName = GgtgRegistration.SERVICES.firstOrNull { it.id == currentConfig.serviceId }?.name ?: GgtgRegistration.SERVICES.first().name
        serviceNameView = pickerRow(context, srvCard, "SMS-сервис", currentServiceName) { showServicePicker(context) }

        apiKeyInput = GgtgUi.input(context, "API-ключ сервиса", currentConfig.apiKey)
        GgtgUi.labeled(context, srvCard, "API-ключ сервиса", "Сохраняется отдельно для каждого сервиса, вводить заново не нужно", apiKeyInput)
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
        srvCard.addView(balanceRow)

        countryView = pickerRow(context, srvCard, "Страна нового номера", GgtgRegistration.currentCountryTitle(currentConfig)) { showCountryPicker(context) }

        GgtgUi.addCard(root, srvCard)

        // -------------------------------------------------------------------------
        // 3. Защита и токены (Push, Captcha)
        // -------------------------------------------------------------------------
        root.addView(sectionHeader(context, "3. Защита и токены (Push, Captcha)", "Play Integrity и обход reCAPTCHA Enterprise", R.drawable.msg_secret))
        val tokCard = GgtgUi.card(context)

        val currentTokenName = GgtgRegistration.tokenServiceName(currentConfig.tokenService)
        tokenServiceView = pickerRow(context, tokCard, "Сервис токенов Play Integrity", currentTokenName) { showTokenServicePicker(context) }

        tokenApiKeyInput = GgtgUi.input(context, "API-ключ токенов", currentConfig.tokenApiKey)
        tokenApiKeyInput.addTextChangedListener(object : android.text.TextWatcher {
            override fun beforeTextChanged(s: CharSequence?, start: Int, count: Int, after: Int) {}
            override fun onTextChanged(s: CharSequence?, start: Int, before: Int, count: Int) {}
            override fun afterTextChanged(s: android.text.Editable?) {
                currentConfig = currentConfig.copy(tokenApiKey = s.toString().trim())
                GgtgRegistration.saveConfig(currentConfig)
            }
        })
        GgtgUi.labeled(context, tokCard, "API-ключ токенов", "Master API key сервиса TokenFast или AntiSafety", tokenApiKeyInput)

        aidLayout = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            visibility = if (currentConfig.tokenService == 2) View.VISIBLE else View.GONE
        }
        aidInput = GgtgUi.input(context, "308aba4e-5680-466b-81a5-477ac6befa95", currentConfig.antiSafetyAid)
        aidInput.addTextChangedListener(object : android.text.TextWatcher {
            override fun beforeTextChanged(s: CharSequence?, start: Int, count: Int, after: Int) {}
            override fun onTextChanged(s: CharSequence?, start: Int, before: Int, count: Int) {}
            override fun afterTextChanged(s: android.text.Editable?) {
                currentConfig = currentConfig.copy(antiSafetyAid = s.toString().trim())
                GgtgRegistration.saveConfig(currentConfig)
            }
        })
        GgtgUi.labeled(context, aidLayout, "AID аккаунта AntiSafety", "Идентификатор аккаунта antisafety.net", aidInput)
        tokCard.addView(aidLayout)

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
        tokCard.addView(tokenBalanceRow)

        val divCap = View(context).apply { setBackgroundColor(GgtgUi.color(Theme.key_divider)) }
        tokCard.addView(divCap, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 1).apply {
            topMargin = GgtgUi.dp(8f)
            bottomMargin = GgtgUi.dp(8f)
        })

        // Captcha
        val currentCaptchaName = GgtgRegistration.captchaServiceName(currentConfig.captchaService)
        captchaServiceView = pickerRow(context, tokCard, "Обход reCAPTCHA Enterprise", currentCaptchaName) { showCaptchaServicePicker(context) }

        captchaApiKeyInput = GgtgUi.input(context, "пусто = ключ сервиса токенов", currentConfig.captchaApiKey)
        captchaApiKeyInput.addTextChangedListener(object : android.text.TextWatcher {
            override fun beforeTextChanged(s: CharSequence?, start: Int, count: Int, after: Int) {}
            override fun onTextChanged(s: CharSequence?, start: Int, before: Int, count: Int) {}
            override fun afterTextChanged(s: android.text.Editable?) {
                currentConfig = currentConfig.copy(captchaApiKey = s.toString().trim())
                GgtgRegistration.saveConfig(currentConfig)
            }
        })
        GgtgUi.labeled(context, tokCard, "API-ключ капчи", "Если отличается от ключа сервиса токенов", captchaApiKeyInput)

        GgtgUi.addCard(root, tokCard)

        // -------------------------------------------------------------------------
        // 4. Запуск смены номера
        // -------------------------------------------------------------------------
        root.addView(sectionHeader(context, "4. Запуск смены номера", "Статус выполнения и журнал", R.drawable.msg_stats))
        val execCard = GgtgUi.card(context)

        statusView = GgtgUi.title(context, "Готов к запуску", 15f)
        setStatus(statusView, "Готов к запуску", R.drawable.msg_info)
        execCard.addView(statusView)

        val btn = GgtgUi.button(context, "Запустить смену номера", false) {
            startPhoneMigration()
        }
        btn.setCompoundDrawablesWithIntrinsicBounds(R.drawable.msg_select, 0, 0, 0)
        btn.compoundDrawablePadding = GgtgUi.dp(8f)
        btn.setPadding(GgtgUi.dp(16f), GgtgUi.dp(10f), GgtgUi.dp(16f), GgtgUi.dp(10f))
        startButton = btn
        execCard.addView(startButton, LinearLayout.LayoutParams(LayoutHelper.MATCH_PARENT, LayoutHelper.WRAP_CONTENT).apply {
            topMargin = GgtgUi.dp(10f)
            bottomMargin = GgtgUi.dp(8f)
        })

        logConsoleView = TextView(context).apply {
            text = "Лог смены номеров готов..."
            textSize = 12f
            typeface = Typeface.MONOSPACE
            setTextColor(GgtgUi.color(Theme.key_windowBackgroundWhiteBlackText))
            setPadding(GgtgUi.dp(8f), GgtgUi.dp(8f), GgtgUi.dp(8f), GgtgUi.dp(8f))
            background = GgtgUi.rounded(GgtgUi.color(Theme.key_windowBackgroundGray), GgtgUi.dp(8f).toFloat())
        }
        execCard.addView(logConsoleView)
        GgtgUi.addCard(root, execCard)

        root.addView(GgtgUi.spacer(context, 24f))

        return frame
    }

    private fun startPhoneMigration() {
        val slots = migAccountPicker.selectedSlots()
        if (slots.isEmpty()) {
            Toast.makeText(context, "Выберите хотя бы один аккаунт", Toast.LENGTH_SHORT).show()
            return
        }
        val key = apiKeyInput.text.toString().trim()
        if (key.isEmpty()) {
            Toast.makeText(context, "Укажите API-ключ SMS-сервиса", Toast.LENGTH_SHORT).show()
            return
        }
        if (currentConfig.countryId < 0) {
            Toast.makeText(context, "Выберите страну нового номера", Toast.LENGTH_SHORT).show()
            return
        }

        currentConfig = currentConfig.copy(apiKey = key)
        GgtgRegistration.saveApiKeyFor(currentConfig.serviceId, key)
        GgtgRegistration.saveConfig(currentConfig)

        val context = context ?: return
        val act = context as? androidx.fragment.app.FragmentActivity
        val progress = if (act != null) app.ggtg.ui.TdataImportProgressDialog(act, "Смена номера") else null
        progress?.setWorkingText("Подготовка...")

        setStatus(statusView, "Смена номеров выполняется...", R.drawable.msg_recent)
        val timeStr = SimpleDateFormat("HH:mm:ss", Locale.getDefault()).format(Date())
        logConsoleView.text = "[$timeStr] Запуск смены номеров для ${slots.size} аккаунтов...\n"

        scope.launch {
            var successCount = 0
            for (slot in slots) {
                val label = GgtgAccounts.label(slot)
                val statusPrefix = "Аккаунт $label: "
                progress?.setWorkingText(statusPrefix + "проверка доверенной сессии...")
                logConsoleView.append("\n$statusPrefix запуск...")

                val res = GgtgSessions.changePhoneNumber(slot, currentConfig) { st ->
                    progress?.setWorkingText(statusPrefix + st)
                    logConsoleView.append("\n  › $st")
                }

                res.onSuccess {
                    successCount++
                    logConsoleView.append("\n  [OK] Номер успешно изменён!")
                }.onFailure { err ->
                    logConsoleView.append("\n  [ОШИБКА] ${err.message}")
                    Toast.makeText(context, "$label: ${err.message}", Toast.LENGTH_LONG).show()
                }
            }
            progress?.dismiss()
            setStatus(statusView, "Завершено. Успешно: $successCount из ${slots.size}", R.drawable.msg_info)
            Toast.makeText(context, "Смена номеров завершена! Успешно: $successCount из ${slots.size}", Toast.LENGTH_LONG).show()
        }
    }

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
            val query = search.text.toString().trim().lowercase(Locale.getDefault())
            var count = 0
            for (c in allCountries) {
                val title = GgtgRegistration.countryTitle(c)
                val idStr = c.id.toString()
                val codeStr = c.phoneCode.lowercase(Locale.getDefault())
                val isoStr = c.isoCode.lowercase(Locale.getDefault())
                if (query.isNotEmpty() && !title.lowercase(Locale.getDefault()).contains(query) &&
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
                        GgtgRegistration.saveConfig(currentConfig)
                        dialog.dismiss()
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

    private fun setStatus(view: TextView, text: String, iconRes: Int) {
        view.text = text
        view.setCompoundDrawablesWithIntrinsicBounds(iconRes, 0, 0, 0)
        view.compoundDrawablePadding = GgtgUi.dp(6f)
    }

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
}
