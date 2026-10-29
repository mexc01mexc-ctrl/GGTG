package app.ggtg

import android.content.Context
import android.graphics.Typeface
import android.view.Gravity
import android.view.View
import android.widget.LinearLayout
import android.widget.TextView
import android.widget.Toast
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.launch
import org.telegram.messenger.AndroidUtilities
import org.telegram.messenger.R
import org.telegram.messenger.UserConfig
import org.telegram.messenger.UserObject
import org.telegram.ui.ActionBar.BaseFragment
import org.telegram.ui.ActionBar.Theme
import org.telegram.ui.SessionsActivity
import app.ggtg.ai.GgtgAiUi

// ggtg: the "i" of an account in "Менеджер аккаунтов": everything about one account on a screen of its own,
// in cards that follow the theme (the old window was a white system dialog that clashed with a dark
// theme): who it is, the connection (proxy, status, SpamBot), the device, the session. Like the
// desktop's ShowAccountInfoBox. A tap on a value copies it; the proxy and SpamBot rows act.
class GgtgAccountInfoActivity(private val slot: Int, private val onChanged: () -> Unit) : BaseFragment() {

    private var column: LinearLayout? = null

    override fun createView(context: Context): View {
        val (frame, col) = GgtgAiUi.page(this, context, "Информация об аккаунте")
        column = col
        rebuild()
        return frame
    }

    override fun onResume() { super.onResume(); rebuild() }

    private fun dp(v: Float) = AndroidUtilities.dp(v)

    private fun copy(context: Context, value: String) {
        AndroidUtilities.addToClipboard(value)
        Toast.makeText(context, "Скопировано", Toast.LENGTH_SHORT).show()
    }

    /** One line of a group: a gray caption over the value, an optional gray explanation; a tap acts. */
    private fun line(context: Context, group: LinearLayout, caption: String, value: String, hint: String? = null, accent: Boolean = false, onClick: (() -> Unit)? = null) {
        if (group.childCount > 0) group.addView(View(context).apply {
            setBackgroundColor((Theme.getColor(Theme.key_windowBackgroundWhiteGrayText) and 0x00FFFFFF) or 0x22000000)
        }, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 1))
        val row = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(14f), dp(10f), dp(14f), dp(10f))
            if (onClick != null) {
                background = Theme.createSelectorDrawable(Theme.getColor(Theme.key_listSelector), 2)
                setOnClickListener { onClick() }
            }
        }
        row.addView(TextView(context).apply {
            text = caption; textSize = 12f
            setTextColor(Theme.getColor(Theme.key_windowBackgroundWhiteGrayText))
        })
        row.addView(TextView(context).apply {
            text = value; textSize = 15f
            setTextColor(if (accent) app.ggtg.GgtgUi.accent else Theme.getColor(Theme.key_windowBackgroundWhiteBlackText))
        }, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply { topMargin = dp(2f) })
        if (hint != null) row.addView(TextView(context).apply {
            text = hint; textSize = 12f
            setTextColor(Theme.getColor(Theme.key_windowBackgroundWhiteGrayText))
        }, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply { topMargin = dp(3f) })
        group.addView(row)
    }

    private fun group(context: Context, col: LinearLayout, title: String): LinearLayout {
        col.addView(GgtgUi.sectionLabel(context, title))
        val card = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            background = GgtgUi.rounded(Theme.getColor(Theme.key_windowBackgroundWhite), dp(14f).toFloat())
        }
        GgtgUi.addCard(col, card)
        return card
    }

    private fun rebuild() {
        val col = column ?: return
        val context = col.context
        col.removeAllViews()
        val uc = UserConfig.getInstance(slot)
        val user = GgtgAccounts.user(slot)
        if (!uc.isClientActivated || user == null) {
            GgtgUi.addCard(col, GgtgUi.card(context).also { it.addView(GgtgUi.body(context, "Аккаунт больше не залогинен.")) }, top = 12f)
            return
        }
        val name = UserObject.getUserName(user).trim()
        val username = user.username?.takeIf { it.isNotEmpty() }?.let { "@$it" }.orEmpty()
        val phone = user.phone?.takeIf { it.isNotEmpty() }?.let { "+$it" }.orEmpty()

        // who it is
        val head = GgtgUi.card(context)
        val top = LinearLayout(context).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
        top.addView(TextView(context).apply {
            text = (name.firstOrNull() ?: '?').toString().uppercase()
            textSize = 22f; setTypeface(null, Typeface.BOLD); gravity = Gravity.CENTER
            setTextColor(0xFFFFFFFF.toInt())
            background = GgtgUi.rounded(GgtgUi.accent, dp(28f).toFloat())
        }, LinearLayout.LayoutParams(dp(56f), dp(56f)).apply { rightMargin = dp(14f) })
        val texts = LinearLayout(context).apply { orientation = LinearLayout.VERTICAL }
        texts.addView(GgtgUi.title(context, name.ifEmpty { "Без имени" }, 18f))
        val sub = listOf(username, phone).filter { it.isNotEmpty() }.joinToString("  ·  ")
        if (sub.isNotEmpty()) texts.addView(GgtgUi.body(context, sub, 14f), LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply { topMargin = dp(2f) })
        top.addView(texts, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
        head.addView(top)
        val st = GgtgAccountStatus.of(slot)
        val status = st.text
        head.addView(GgtgUi.pill(context, status, 0xFFFFFFFF.toInt(), st.color),
            LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply { topMargin = dp(10f) })
        GgtgUi.addCard(col, head, top = 12f)

        // the account
        val account = group(context, col, "Аккаунт")
        if (name.isNotEmpty()) line(context, account, "Имя", name, onClick = { copy(context, name) })
        if (username.isNotEmpty()) line(context, account, "Username", username, onClick = { copy(context, username) })
        if (phone.isNotEmpty()) line(context, account, "Телефон", phone, onClick = { copy(context, phone) })
        val idDate = GgtgIdDate.estimate(uc.clientUserId)
        line(context, account, "ID", "${uc.clientUserId}", onClick = { copy(context, uc.clientUserId.toString()) })
        if (idDate.isNotEmpty()) {
            line(context, account, "Регистрация", idDate, onClick = { copy(context, idDate) })
        }
        line(context, account, "Папка", uc.ncgFolder.orEmpty().ifEmpty { "—" })

        val hasBackup = GgtgSessions.hasBackup(uc.clientUserId)
        val backups = GgtgSessions.getSwitchableSessions(uc.clientUserId)
        val backupCount = backups.size
        val backupText = if (hasBackup) "Доступно сессий: $backupCount" else "Нет (нажмите, чтобы создать)"
        line(context, account, "Сессии аккаунта", backupText, accent = true) {
            val act = context as? androidx.fragment.app.FragmentActivity ?: return@line
            val items = ArrayList<String>()
            items.add("Защита от угона (Анти-угон)")
            items.add("Смена номера (миграция)")
            if (hasBackup) {
                if (backups.size == 1) {
                    items.add("Перейти на ${GgtgSessions.roleTitle(backups[0].role).lowercase()}")
                } else {
                    items.add("Выбрать сессию для переключения ($backupCount)")
                }
                items.add("Создать резервную сессию")
            } else {
                items.add("Создать резервную сессию")
            }
            items.add("Устройства и активные сессии")

            androidx.appcompat.app.AlertDialog.Builder(context)
                .setTitle("Сессии аккаунта")
                .setItems(items.toTypedArray()) { _, which ->
                    val chosen = items[which]
                    when {
                        chosen.startsWith("Перейти") || chosen.startsWith("Выбрать") -> {
                            val pickAction: (GgtgSessions.StoredSession) -> Unit = { target ->
                                val warning = GgtgSessions.switchWarning(target)
                                val msg = (if (!warning.isNullOrEmpty()) "Внимание: $warning\n\n" else "") +
                                    "Текущая сессия сохранится в хранилище, а аккаунт переключится на эту сессию (${GgtgSessions.roleTitle(target.role)})."
                                androidx.appcompat.app.AlertDialog.Builder(context)
                                    .setTitle("Переключить сессию?")
                                    .setMessage(msg)
                                    .setPositiveButton("Переключить") { _, _ ->
                                        val progress = app.ggtg.ui.TdataImportProgressDialog(act, "Переключение сессии")
                                        progress.setWorkingText("Переключение…")
                                        GgtgSessions.swapToBackupAsync(slot, target.id) { success, retMsg ->
                                            progress.dismiss()
                                            if (success) {
                                                Toast.makeText(context, "Аккаунт переключён на выбранную сессию!", Toast.LENGTH_SHORT).show()
                                                onChanged(); rebuild()
                                            } else {
                                                Toast.makeText(context, "Ошибка: $retMsg", Toast.LENGTH_LONG).show()
                                            }
                                        }
                                    }
                                    .setNegativeButton("Отмена", null)
                                    .show()
                            }

                            if (backups.size == 1) {
                                pickAction(backups[0])
                            } else {
                                val sessionNames = backups.map { s ->
                                    val title = GgtgSessions.roleTitle(s.role)
                                    val dev = s.device?.optString("deviceModel", "")?.ifEmpty { null }
                                    val dateStr = java.text.SimpleDateFormat("dd.MM.yyyy", java.util.Locale.getDefault()).format(java.util.Date(s.createdAt * 1000))
                                    "$title ${dev?.let { "($it) " } ?: ""}· $dateStr"
                                }.toTypedArray()
                                androidx.appcompat.app.AlertDialog.Builder(context)
                                    .setTitle("Выберите сессию")
                                    .setItems(sessionNames) { _, sIdx ->
                                        pickAction(backups[sIdx])
                                    }
                                    .setNegativeButton("Отмена", null)
                                    .show()
                            }
                        }
                        chosen.startsWith("Создать") -> {
                            val progress = app.ggtg.ui.TdataImportProgressDialog(act, "Резервная сессия")
                            progress.setWorkingText("Создание резервной сессии…")
                            GgtgSessions.createBackupSession(act, slot, progress) { res ->
                                progress.dismiss()
                                res.onSuccess {
                                    Toast.makeText(context, "Резервная сессия создана!", Toast.LENGTH_LONG).show()
                                    onChanged(); rebuild()
                                }.onFailure {
                                    Toast.makeText(context, "Ошибка: ${it.message}", Toast.LENGTH_LONG).show()
                                }
                            }
                        }
                        chosen == "Защита от угона (Анти-угон)" -> {
                            val backupSessions = GgtgSessions.getSwitchableSessions(uc.clientUserId)
                                .filter { it.role in listOf("backup", "anti_theft_backup") }
                                .sortedBy { it.createdAt }

                            val bld = androidx.appcompat.app.AlertDialog.Builder(context)
                                .setTitle("Защита от угона (Анти-угон)")

                            if (backupSessions.isEmpty()) {
                                bld.setMessage("Для защиты купленного аккаунта требуется создать резервную сессию программы и дать ей отлежаться 24 часа в Telegram (сервер блокирует сброс чужих сессий для новых авторизаций).\n\nНажмите «Создать сессию», чтобы запустить 24ч таймер.")
                                bld.setPositiveButton("Создать сессию") { _, _ ->
                                    val progress = app.ggtg.ui.TdataImportProgressDialog(act, "Анти-угон")
                                    progress.setWorkingText("Создание резервной сессии…")
                                    GgtgSessions.createBackupSession(act, slot, progress) { res ->
                                        progress.dismiss()
                                        res.onSuccess {
                                            Toast.makeText(context, "Резервная сессия создана. Таймер 24ч отлёжки запущен.", Toast.LENGTH_LONG).show()
                                            onChanged(); rebuild()
                                        }.onFailure {
                                            Toast.makeText(context, "Ошибка: ${it.message}", Toast.LENGTH_LONG).show()
                                        }
                                    }
                                }
                            } else {
                                val oldest = backupSessions.first()
                                val isTrust = GgtgSessions.isTrusted(oldest)
                                val countdown = GgtgSessions.trustCountdownString(oldest)
                                val desc = "Статус резервной сессии: $countdown\n\n" +
                                    if (isTrust) "Сессия выдержана (> 24ч) и готова к защите. Нажмите «Защитить аккаунт», чтобы переключиться на неё, сбросить сессию продавца и удалить её."
                                    else "Telegram заблокирует сброс сессий до завершения 24 часов (ошибка FRESH_RESET_AUTHORISATION_FORBIDDEN). Дождитесь окончания отлёжки."

                                bld.setMessage(desc)
                                if (isTrust) {
                                    bld.setPositiveButton("Защитить аккаунт") { _, _ ->
                                        val progress = app.ggtg.ui.TdataImportProgressDialog(act, "Анти-угон")
                                        progress.setWorkingText("Выполнение защиты…")
                                        kotlinx.coroutines.MainScope().launch {
                                            val res = GgtgSessions.performAntiTheftProtection(act, slot)
                                            progress.dismiss()
                                            res.onSuccess { msg ->
                                                Toast.makeText(context, msg, Toast.LENGTH_LONG).show()
                                                onChanged(); rebuild()
                                            }.onFailure { err ->
                                                Toast.makeText(context, "Ошибка: ${err.message}", Toast.LENGTH_LONG).show()
                                            }
                                        }
                                    }
                                }
                            }
                            bld.setNegativeButton("Закрыть", null).show()
                        }
                        chosen == "Смена номера (миграция)" -> {
                            presentFragment(GgtgPhoneMigrationActivity(listOf(slot)))
                        }
                        chosen.contains("сессии") -> {
                            presentFragment(SessionsActivity(0))
                        }
                    }
                }
                .setNegativeButton("Отмена", null)
                .show()
        }

        // the connection
        val connection = group(context, col, "Соединение")
        line(context, connection, "Прокси",
            if (uc.ncgProxyEnabled) "${uc.ncgProxyAddress}:${uc.ncgProxyPort}" else "нет — аккаунт использует общий прокси",
            if (uc.ncgProxyEnabled) "нажмите, чтобы изменить" else "нажмите, чтобы задать свой", accent = true) {
            GgtgAccountMenus.editProxy(context, slot) { onChanged(); rebuild() }
        }
        val explanation = when {
            uc.ncgTrashed -> "Аккаунт спит без соединения, поэтому проверки для него не считаются."
            !uc.ncgProxyEnabled -> "У аккаунта не задан прокси: он использует общий. Задайте свой в строке «Прокси»."
            else -> null
        }
        line(context, connection, "Статус", status, explanation)
        line(context, connection, "SpamBot", GgtgSpamStatus.statusText(uc.clientUserId), "нажмите, чтобы проверить сейчас", accent = true) {
            Toast.makeText(context, "Спрашиваем @SpamBot…", Toast.LENGTH_SHORT).show()
            CoroutineScope(SupervisorJob() + Dispatchers.IO).launch {
                val error = GgtgSpamStatus.check(slot)
                AndroidUtilities.runOnUIThread {
                    Toast.makeText(context, error?.let { "SpamBot: $it" } ?: ("SpamBot: " + GgtgSpamStatus.statusText(uc.clientUserId)), Toast.LENGTH_SHORT).show()
                    onChanged(); rebuild()
                }
            }
        }
        val ggr = GgtgSpamStatus.calculateGgr(slot)
        line(context, connection, "Рейтинг GGR", "${String.format(java.util.Locale.US, "%.1f", ggr.score)} (${ggr.tier})", "Индекс здоровья и уровня прогрева (1.0 – 10.0)")
        if (GgtgSpamStatus.isBlocked(uc.clientUserId)) {
            line(context, connection, "Авто-апелляция @SpamBot", "Подать апелляцию", "нажмите для автоматической отправки запроса в @SpamBot", accent = true) {
                Toast.makeText(context, "Отправляем апелляцию в @SpamBot…", Toast.LENGTH_SHORT).show()
                CoroutineScope(SupervisorJob() + Dispatchers.IO).launch {
                    val err = GgtgSpamStatus.autoAppeal(slot)
                    AndroidUtilities.runOnUIThread {
                        Toast.makeText(context, err ?: "Апелляция успешно отправлена", Toast.LENGTH_SHORT).show()
                        onChanged(); rebuild()
                    }
                }
            }
        }

        // the device
        val device = group(context, col, "Устройство")
        val devText = if (uc.ncgDeviceModel.isEmpty()) "по умолчанию" else {
            val base = GgtgDevice.phoneLabel(uc.ncgDeviceModel, uc.ncgSystemVersion)
            if (uc.ncgAppVersion.isNotEmpty()) "$base (${uc.ncgAppVersion})" else base
        }
        val packInfo = if (uc.ncgLangPack.isNotEmpty()) " [${uc.ncgLangPack}]" else ""
        line(context, device, "Устройство", devText + packInfo, "нажмите, чтобы настроить параметры", accent = true) {
            showEditDeviceDialog(context, slot)
        }
        line(context, device, "Язык и часовой пояс",
            (if (uc.ncgCloudLang.isEmpty()) "язык по умолчанию" else GgtgDevice.langLabel(uc.ncgSystemLang, uc.ncgCloudLang)) +
                " · " + (if (uc.ncgTzSet) GgtgDevice.zoneLabel(uc.ncgTzOffset) else "пояс по умолчанию"),
            "нажмите, чтобы настроить параметры", accent = true) {
            showEditLangAndTzDialog(context, slot)
        }

        // the session
        val session = group(context, col, "Сессия")
        line(context, session, "Сессия", if (slot == UserConfig.selectedAccount) "активна (текущий аккаунт)" else "активна", "нажмите, чтобы открыть активные сессии", accent = true) {
            presentFragment(SessionsActivity(0))
        }
        val breaks = GgtgSessionLog.forUser(uc.clientUserId)
        line(context, session, "Разрывы сессии",
            if (breaks.isEmpty()) "не было"
            else "${breaks.size} (последний: " + java.text.SimpleDateFormat("dd.MM.yyyy HH:mm", java.util.Locale.getDefault()).format(java.util.Date(breaks.last().at * 1000)) +
                ", Telegram закрыл сессию)")
        if (uc.ncgTwoFactor.isNotEmpty()) line(context, session, "Пароль 2FA", uc.ncgTwoFactor, "нажмите, чтобы скопировать", accent = true) { copy(context, uc.ncgTwoFactor) }
        else line(context, session, "Пароль 2FA", "не сохранён")
        col.addView(GgtgAiUi.gap(context, 24f))
    }

    private fun showEditDeviceDialog(context: Context, slot: Int) {
        val uc = UserConfig.getInstance(slot)

        val isDesktop = uc.ncgSystemVersion.contains("Windows") || uc.ncgSystemVersion.contains("macOS") || uc.ncgSystemVersion.contains("OS X")
        val isIos = uc.ncgDeviceModel.contains("iPhone") || uc.ncgSystemVersion.contains("iOS")
        var curModel = if (uc.ncgDeviceModel.isNotEmpty()) uc.ncgDeviceModel else if (isDesktop) GgtgDevice.phones.first().model else GgtgDevice.phones.first().model
        var curSystem = if (uc.ncgSystemVersion.isNotEmpty()) uc.ncgSystemVersion else if (isIos) GgtgIosProfiles.systems.first() else GgtgDevice.phones.first().system
        var curApp = if (uc.ncgAppVersion.isNotEmpty()) uc.ncgAppVersion else if (isIos) GgtgIosProfiles.appVersions.first() else GgtgDevice.curatedBuilds.first()

        val layout = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(20f), dp(12f), dp(20f), dp(8f))
        }

        val card = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            background = GgtgUi.rounded(Theme.getColor(Theme.key_windowBackgroundGray), dp(12f).toFloat())
            setPadding(dp(12f), dp(8f), dp(12f), dp(8f))
        }
        layout.addView(card, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT))

        // Model row
        var updateViews: () -> Unit = {}
        val rowModel = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(8f), dp(8f), dp(8f), dp(8f))
            background = Theme.createSelectorDrawable(Theme.getColor(Theme.key_listSelector), 2)
        }
        val capModel = TextView(context).apply {
            textSize = 12f
            setTextColor(Theme.getColor(Theme.key_windowBackgroundWhiteGrayText))
        }
        val valModel = TextView(context).apply {
            textSize = 15f
            setTypeface(null, Typeface.BOLD)
            setTextColor(GgtgUi.accent)
        }
        rowModel.addView(capModel)
        rowModel.addView(valModel, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply { topMargin = dp(2f) })
        card.addView(rowModel)

        card.addView(View(context).apply {
            setBackgroundColor((Theme.getColor(Theme.key_windowBackgroundWhiteGrayText) and 0x00FFFFFF) or 0x22000000)
        }, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 1))

        // System row
        val rowSystem = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(8f), dp(8f), dp(8f), dp(8f))
            background = Theme.createSelectorDrawable(Theme.getColor(Theme.key_listSelector), 2)
        }
        val capSystem = TextView(context).apply {
            textSize = 12f
            setTextColor(Theme.getColor(Theme.key_windowBackgroundWhiteGrayText))
        }
        val valSystem = TextView(context).apply {
            textSize = 15f
            setTypeface(null, Typeface.BOLD)
            setTextColor(GgtgUi.accent)
        }
        rowSystem.addView(capSystem)
        rowSystem.addView(valSystem, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply { topMargin = dp(2f) })
        card.addView(rowSystem)

        card.addView(View(context).apply {
            setBackgroundColor((Theme.getColor(Theme.key_windowBackgroundWhiteGrayText) and 0x00FFFFFF) or 0x22000000)
        }, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 1))

        // App row
        val rowApp = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(8f), dp(8f), dp(8f), dp(8f))
            background = Theme.createSelectorDrawable(Theme.getColor(Theme.key_listSelector), 2)
        }
        val capApp = TextView(context).apply {
            textSize = 12f
            setTextColor(Theme.getColor(Theme.key_windowBackgroundWhiteGrayText))
        }
        val valApp = TextView(context).apply {
            textSize = 15f
            setTypeface(null, Typeface.BOLD)
            setTextColor(GgtgUi.accent)
        }
        rowApp.addView(capApp)
        rowApp.addView(valApp, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply { topMargin = dp(2f) })
        card.addView(rowApp)

        // Random iPhone button
        val btnRandomIos = TextView(context).apply {
            text = "Случайный iPhone (iOS)"
            textSize = 14f
            setTypeface(null, Typeface.BOLD)
            gravity = Gravity.CENTER
            setTextColor(GgtgUi.accent)
            setPadding(dp(12f), dp(10f), dp(12f), dp(10f))
            background = Theme.createSelectorDrawable(Theme.getColor(Theme.key_listSelector), 2)
        }
        layout.addView(btnRandomIos, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply {
            topMargin = dp(12f)
        })

        // Random Android button
        val btnRandom = TextView(context).apply {
            text = "Случайный Android"
            textSize = 14f
            setTypeface(null, Typeface.BOLD)
            gravity = Gravity.CENTER
            setTextColor(Theme.getColor(Theme.key_windowBackgroundWhiteGrayText))
            setPadding(dp(12f), dp(10f), dp(12f), dp(10f))
            background = Theme.createSelectorDrawable(Theme.getColor(Theme.key_listSelector), 2)
        }
        layout.addView(btnRandom, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply {
            topMargin = dp(6f)
        })

        updateViews = {
            val isCurrentIos = curModel.contains("iPhone") || curSystem.contains("iOS")
            if (isCurrentIos) {
                capModel.text = "Модель iPhone"
                capSystem.text = "Версия iOS"
                capApp.text = "Версия Telegram iOS"
                valModel.text = curModel
            } else {
                capModel.text = "Модель Android устройства"
                capSystem.text = "Версия системы (SDK)"
                capApp.text = "Версия Telegram Android"
                valModel.text = GgtgDevice.phoneLabel(curModel, "")
            }
            valSystem.text = curSystem
            valApp.text = curApp
        }

        rowModel.setOnClickListener {
            val categories = arrayOf("Apple iPhone (iOS)", "Android телефоны")
            androidx.appcompat.app.AlertDialog.Builder(context)
                .setTitle("Выберите платформу")
                .setItems(categories) { _, catIndex ->
                    if (catIndex == 0) {
                        val items = GgtgIosProfiles.devices.toTypedArray()
                        androidx.appcompat.app.AlertDialog.Builder(context)
                            .setTitle("Модели iPhone")
                            .setItems(items) { _, which ->
                                curModel = items[which]
                                if (!curSystem.contains("iOS")) curSystem = GgtgIosProfiles.randomSystem()
                                if (!curApp.startsWith("11.")) curApp = GgtgIosProfiles.randomAppVersion()
                                updateViews()
                            }
                            .setNegativeButton("Отмена", null)
                            .show()
                    } else {
                        val items = GgtgDevice.phones.map { it.label }.toTypedArray()
                        androidx.appcompat.app.AlertDialog.Builder(context)
                            .setTitle("Модели Android")
                            .setItems(items) { _, which ->
                                curModel = GgtgDevice.phones[which].model
                                curSystem = GgtgDevice.phones[which].system
                                if (curApp.startsWith("11.")) curApp = GgtgDevice.curatedBuilds.first()
                                updateViews()
                            }
                            .setNegativeButton("Отмена", null)
                            .show()
                    }
                }
                .setNegativeButton("Отмена", null)
                .show()
        }

        rowSystem.setOnClickListener {
            val isCurrentIos = curModel.contains("iPhone") || curSystem.contains("iOS")
            if (isCurrentIos) {
                val items = GgtgIosProfiles.systems.toTypedArray()
                androidx.appcompat.app.AlertDialog.Builder(context)
                    .setTitle("Версия iOS")
                    .setItems(items) { _, which ->
                        curSystem = items[which]
                        updateViews()
                    }
                    .setNegativeButton("Отмена", null)
                    .show()
            } else {
                val items = arrayOf(
                    "SDK 35 (Android 15)",
                    "SDK 34 (Android 14)",
                    "SDK 33 (Android 13)",
                    "SDK 32 (Android 12L)",
                    "SDK 31 (Android 12)",
                    "SDK 30 (Android 11)"
                )
                androidx.appcompat.app.AlertDialog.Builder(context)
                    .setTitle("Версия Android SDK")
                    .setItems(items) { _, which ->
                        curSystem = items[which].substringBefore(" ") + " " + items[which].substringAfter("SDK ").substringBefore(" ")
                        updateViews()
                    }
                    .setNegativeButton("Отмена", null)
                    .show()
            }
        }

        rowApp.setOnClickListener {
            val isCurrentIos = curModel.contains("iPhone") || curSystem.contains("iOS")
            if (isCurrentIos) {
                val items = GgtgIosProfiles.appVersions.toTypedArray()
                androidx.appcompat.app.AlertDialog.Builder(context)
                    .setTitle("Версия Telegram iOS")
                    .setItems(items) { _, which ->
                        curApp = items[which]
                        updateViews()
                    }
                    .setNegativeButton("Отмена", null)
                    .show()
            } else {
                val items = GgtgDevice.curatedBuilds.toTypedArray()
                androidx.appcompat.app.AlertDialog.Builder(context)
                    .setTitle("Версия Telegram Android")
                    .setItems(items) { _, which ->
                        curApp = GgtgDevice.curatedBuilds[which]
                        updateViews()
                    }
                    .setNegativeButton("Отмена", null)
                    .show()
            }
        }

        btnRandomIos.setOnClickListener {
            val profile = GgtgIosProfiles.random()
            curModel = profile.deviceModel
            curSystem = profile.systemVersion
            curApp = profile.appVersion
            updateViews()
        }

        btnRandom.setOnClickListener {
            val rndPhone = GgtgDevice.phones.random()
            curModel = rndPhone.model
            curSystem = rndPhone.system
            curApp = GgtgDevice.curatedBuilds.random()
            updateViews()
        }

        updateViews()

        androidx.appcompat.app.AlertDialog.Builder(context)
            .setTitle("Параметры устройства")
            .setView(layout)
            .setPositiveButton("Сохранить") { _, _ ->
                val isSaveIos = curModel.contains("iPhone") || curSystem.contains("iOS")
                val langPack = if (isSaveIos) GgtgIosProfiles.LANG_PACK else "android"
                val appId = if (isSaveIos) GgtgIosProfiles.APP_ID else 6
                val appHash = if (isSaveIos) GgtgIosProfiles.APP_HASH else "eb06d4abfb49dc3eeb1aeb98ae0f581e"
                GgtgDevice.save(
                    slot, curModel, curSystem,
                    uc.ncgSystemLang.ifEmpty { "ru-ru" },
                    uc.ncgCloudLang.ifEmpty { "ru" },
                    if (uc.ncgTzSet) uc.ncgTzOffset else null,
                    curApp, langPack, appId, appHash
                )
                Toast.makeText(context, "Параметры сохранены", Toast.LENGTH_SHORT).show()
                onChanged(); rebuild()
            }
            .setNegativeButton("Отмена", null)
            .show()
    }

    private fun showEditLangAndTzDialog(context: Context, slot: Int) {
        val uc = UserConfig.getInstance(slot)
        var curSysLang = uc.ncgSystemLang.ifEmpty { "ru-ru" }
        var curCloudLang = uc.ncgCloudLang.ifEmpty { "ru" }
        var curTzOffset = if (uc.ncgTzSet) uc.ncgTzOffset else 10800
        var isTzSet = uc.ncgTzSet

        val layout = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(20f), dp(12f), dp(20f), dp(8f))
        }

        val card = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            background = GgtgUi.rounded(Theme.getColor(Theme.key_windowBackgroundGray), dp(12f).toFloat())
            setPadding(dp(12f), dp(8f), dp(12f), dp(8f))
        }
        layout.addView(card, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT))

        // Language row
        val rowLang = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(8f), dp(8f), dp(8f), dp(8f))
            background = Theme.createSelectorDrawable(Theme.getColor(Theme.key_listSelector), 2)
        }
        val capLang = TextView(context).apply {
            text = "Язык интерфейса и системы"
            textSize = 12f
            setTextColor(Theme.getColor(Theme.key_windowBackgroundWhiteGrayText))
        }
        val valLang = TextView(context).apply {
            textSize = 15f
            setTypeface(null, Typeface.BOLD)
            setTextColor(GgtgUi.accent)
        }
        rowLang.addView(capLang)
        rowLang.addView(valLang, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply { topMargin = dp(2f) })
        card.addView(rowLang)

        card.addView(View(context).apply {
            setBackgroundColor((Theme.getColor(Theme.key_windowBackgroundWhiteGrayText) and 0x00FFFFFF) or 0x22000000)
        }, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 1))

        // Timezone row
        val rowTz = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(8f), dp(8f), dp(8f), dp(8f))
            background = Theme.createSelectorDrawable(Theme.getColor(Theme.key_listSelector), 2)
        }
        val capTz = TextView(context).apply {
            text = "Часовой пояс"
            textSize = 12f
            setTextColor(Theme.getColor(Theme.key_windowBackgroundWhiteGrayText))
        }
        val valTz = TextView(context).apply {
            textSize = 15f
            setTypeface(null, Typeface.BOLD)
            setTextColor(GgtgUi.accent)
        }
        rowTz.addView(capTz)
        rowTz.addView(valTz, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply { topMargin = dp(2f) })
        card.addView(rowTz)

        // Auto-detect button
        val btnAuto = TextView(context).apply {
            text = "Определить по номеру и прокси"
            textSize = 14f
            setTypeface(null, Typeface.BOLD)
            gravity = Gravity.CENTER
            setTextColor(GgtgUi.accent)
            setPadding(dp(12f), dp(10f), dp(12f), dp(10f))
            background = Theme.createSelectorDrawable(Theme.getColor(Theme.key_listSelector), 2)
        }
        layout.addView(btnAuto, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply { topMargin = dp(12f) })

        fun updateViews() {
            valLang.text = GgtgDevice.langLabel(curSysLang, curCloudLang)
            valTz.text = if (isTzSet) GgtgDevice.zoneLabel(curTzOffset) else "Пояс по умолчанию"
        }
        updateViews()

        rowLang.setOnClickListener {
            val items = GgtgDevice.langs.map { "${it.name} (${it.system})" }.toTypedArray()
            androidx.appcompat.app.AlertDialog.Builder(context)
                .setTitle("Выберите язык")
                .setItems(items) { _, which ->
                    val chosen = GgtgDevice.langs[which]
                    curSysLang = chosen.system
                    curCloudLang = chosen.cloud
                    updateViews()
                }
                .setNegativeButton("Отмена", null)
                .show()
        }

        rowTz.setOnClickListener {
            val items = GgtgDevice.zones.map { "${it.name} (UTC${GgtgDevice.formatOffset(it.seconds)})" }.toTypedArray()
            androidx.appcompat.app.AlertDialog.Builder(context)
                .setTitle("Выберите часовой пояс")
                .setItems(items) { _, which ->
                    curTzOffset = GgtgDevice.zones[which].seconds
                    isTzSet = true
                    updateViews()
                }
                .setNegativeButton("Отмена", null)
                .show()
        }

        btnAuto.setOnClickListener {
            val phone = uc.currentUser?.phone.orEmpty()
            val phoneGeo = if (phone.isNotEmpty()) GgtgDevice.matchForPhone(phone) else null
            if (phoneGeo != null) {
                curSysLang = phoneGeo.systemLang
                curCloudLang = phoneGeo.cloudLang
                curTzOffset = phoneGeo.tzOffset
                isTzSet = true
                updateViews()
                Toast.makeText(context, "Определено по номеру: ${phoneGeo.country} (${phoneGeo.systemLang}, UTC${GgtgDevice.formatOffset(phoneGeo.tzOffset)})", Toast.LENGTH_SHORT).show()
            } else {
                Toast.makeText(context, "Определяю по прокси...", Toast.LENGTH_SHORT).show()
                CoroutineScope(Dispatchers.Main + SupervisorJob()).launch {
                    try {
                        val geo = GgtgDevice.detect(slot)
                        curSysLang = geo.lang.system
                        curCloudLang = geo.lang.cloud
                        curTzOffset = geo.offset
                        isTzSet = true
                        updateViews()
                        Toast.makeText(context, "Определено: ${geo.country} (${geo.lang.system}, UTC${GgtgDevice.formatOffset(geo.offset)})", Toast.LENGTH_SHORT).show()
                    } catch (t: Throwable) {
                        Toast.makeText(context, "Не удалось определить автоматически: ${t.message}", Toast.LENGTH_SHORT).show()
                    }
                }
            }
        }

        androidx.appcompat.app.AlertDialog.Builder(context)
            .setTitle("Язык и часовой пояс")
            .setView(layout)
            .setPositiveButton("Сохранить") { _, _ ->
                GgtgDevice.save(
                    slot,
                    uc.ncgDeviceModel,
                    uc.ncgSystemVersion,
                    curSysLang,
                    curCloudLang,
                    if (isTzSet) curTzOffset else null,
                    uc.ncgAppVersion,
                    uc.ncgLangPack.ifEmpty { "android" },
                    if (uc.ncgAppId != 0) uc.ncgAppId else 6,
                    uc.ncgAppHash.ifEmpty { "eb06d4abfb49dc3eeb1aeb98ae0f581e" }
                )
                Toast.makeText(context, "Язык и пояс сохранены", Toast.LENGTH_SHORT).show()
                onChanged()
                rebuild()
            }
            .setNegativeButton("Отмена", null)
            .show()
    }
}
