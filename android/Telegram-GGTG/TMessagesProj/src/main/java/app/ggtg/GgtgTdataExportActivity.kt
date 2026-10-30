package app.ggtg

import android.content.Context
import android.view.Gravity
import android.view.View
import android.widget.CheckBox
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.Toast
import org.telegram.messenger.AndroidUtilities
import org.telegram.messenger.R
import org.telegram.ui.ActionBar.ActionBar.ActionBarMenuOnItemClick
import org.telegram.ui.ActionBar.BaseFragment
import org.telegram.ui.ActionBar.Theme
import org.telegram.ui.Components.LayoutHelper

// GGTG: "Экспорт аккаунтов" - multi-format export matching GGTG Desktop:
// Formats: TData, Session + JSON, or .ggtg pack.
// Sources: New Duplicate, Working (with swap to backup), Stored Backup, or Working Copy.
class GgtgTdataExportActivity(private val preselectedSlots: List<Int> = emptyList()) : BaseFragment() {

    private var picker: GgtgAccountPicker? = null
    private var selectedFormat = TdataExportHelper.ExportFormat.TData
    private var selectedSource = TdataExportHelper.SessionSource.NewDuplicate
    private var sidecarsEnabled = true
    private var newWhenNoBackup = true

    override fun createView(context: Context): View {
        actionBar.setBackButtonImage(R.drawable.ic_ab_back)
        actionBar.setAllowOverlayTitle(true)
        actionBar.setTitle("Экспорт аккаунтов")
        actionBar.setActionBarMenuOnItemClick(object : ActionBarMenuOnItemClick() {
            override fun onItemClick(id: Int) { if (id == -1) finishFragment() }
        })

        val frame = FrameLayout(context)
        frame.setBackgroundColor(Theme.getColor(Theme.key_windowBackgroundGray))
        fragmentView = frame
        val root = GgtgUi.page(context, frame)

        // 1. Header info card (NO EMOJIS)
        GgtgUi.addCard(
            root,
            GgtgUi.infoCard(
                context,
                org.telegram.messenger.R.drawable.msg_shareout,
                "Экспорт сессий и аккаунтов",
                "Экспорт выбранных аккаунтов в Telegram Desktop (tdata), сессии с JSON или единый пакет .ggtg. Доступен экспорт основной сессии с переходом на резервную, экспорт готовой резервной сессии или создание нового независимого дубликата."
            ),
            top = 10f
        )

        // 2. Accounts picker
        root.addView(GgtgUi.sectionLabel(context, "1. Выберите аккаунты"))
        val accounts = GgtgUi.card(context)
        val p = GgtgAccountPicker(context)
        p.setBackgroundColor(android.graphics.Color.TRANSPARENT)
        if (preselectedSlots.isNotEmpty()) {
            p.setSelected(preselectedSlots)
        }
        accounts.addView(p, LinearLayout.LayoutParams(LayoutHelper.MATCH_PARENT, LayoutHelper.WRAP_CONTENT))
        GgtgUi.addCard(root, accounts)
        picker = p

        // 3. Format selection
        root.addView(GgtgUi.sectionLabel(context, "2. Формат экспорта"))
        val formatCards = ArrayList<Pair<LinearLayout, TdataExportHelper.ExportFormat>>()
        fun refreshFormats() {
            for ((card, fmt) in formatCards) {
                val active = (fmt == selectedFormat)
                val bg = if (active) (GgtgUi.accent and 0x00FFFFFF) or 0x1A000000 else Theme.getColor(Theme.key_windowBackgroundWhite)
                card.background = GgtgUi.rounded(
                    bg,
                    AndroidUtilities.dp(14f).toFloat(),
                    if (active) AndroidUtilities.dp(2f) else 0,
                    GgtgUi.accent
                )
            }
        }
        fun addFormatOption(fmt: TdataExportHelper.ExportFormat, title: String, desc: String) {
            val c = GgtgUi.card(context) {
                selectedFormat = fmt
                refreshFormats()
            }
            c.addView(GgtgUi.title(context, title, 15f))
            c.addView(
                GgtgUi.body(context, desc),
                LinearLayout.LayoutParams(LayoutHelper.MATCH_PARENT, LayoutHelper.WRAP_CONTENT).apply {
                    topMargin = AndroidUtilities.dp(2f)
                }
            )
            formatCards.add(c to fmt)
            GgtgUi.addCard(root, c)
        }

        addFormatOption(
            TdataExportHelper.ExportFormat.TData,
            "TData (Папка Telegram Desktop)",
            "Готовые папки tdata для Telegram Desktop (macOS, Windows, Linux) в zip архиве."
        )
        addFormatOption(
            TdataExportHelper.ExportFormat.SessionJson,
            "Сессия + JSON (Android / Telethon / Pyrogram)",
            "Ключи авторизации session.json, параметры устройства и прокси для софта и мобильных клиентов."
        )
        addFormatOption(
            TdataExportHelper.ExportFormat.GgtgPack,
            "Пакет GGTG (.ggtg)",
            "Единый файл .ggtg для быстрого импорта на телефон или Desktop со всеми настройками."
        )
        refreshFormats()

        // 4. Session Source selection
        root.addView(GgtgUi.sectionLabel(context, "3. Какую сессию отдать"))
        val sourceCards = ArrayList<Pair<LinearLayout, TdataExportHelper.SessionSource>>()
        val backupOptionsCard = GgtgUi.card(context).apply { orientation = LinearLayout.VERTICAL }

        fun refreshSources() {
            for ((card, src) in sourceCards) {
                val active = (src == selectedSource)
                val bg = if (active) (GgtgUi.accent and 0x00FFFFFF) or 0x1A000000 else Theme.getColor(Theme.key_windowBackgroundWhite)
                card.background = GgtgUi.rounded(
                    bg,
                    AndroidUtilities.dp(14f).toFloat(),
                    if (active) AndroidUtilities.dp(2f) else 0,
                    GgtgUi.accent
                )
            }
            backupOptionsCard.visibility = if (selectedSource == TdataExportHelper.SessionSource.Backup) View.VISIBLE else View.GONE
        }

        fun addSourceOption(src: TdataExportHelper.SessionSource, title: String, desc: String) {
            val c = GgtgUi.card(context) {
                selectedSource = src
                refreshSources()
            }
            c.addView(GgtgUi.title(context, title, 15f))
            c.addView(
                GgtgUi.body(context, desc),
                LinearLayout.LayoutParams(LayoutHelper.MATCH_PARENT, LayoutHelper.WRAP_CONTENT).apply {
                    topMargin = AndroidUtilities.dp(2f)
                }
            )
            sourceCards.add(c to src)
            GgtgUi.addCard(root, c)
        }

        addSourceOption(
            TdataExportHelper.SessionSource.NewDuplicate,
            "Новую дублированную (Рекомендуется)",
            "Создаётся новая независимая сессия (с прокси). Телефон остаётся на своей сессии."
        )
        addSourceOption(
            TdataExportHelper.SessionSource.WorkingSwap,
            "Основную (программа перейдёт на резервную)",
            "Текущая сессия отдаётся в экспорт, а телефон переключается на сохранённую резервную сессию."
        )
        addSourceOption(
            TdataExportHelper.SessionSource.Backup,
            "Резервную (станет дублированной)",
            "Берётся сохранённая резервная сессия из хранилища. Телефон остаётся на своей рабочей сессии."
        )
        addSourceOption(
            TdataExportHelper.SessionSource.WorkingCopy,
            "Основную (без перехода)",
            "Текущая сессия отдаётся в экспорт как есть."
        )

        // Sub-option for Backup: "Если резервной нет — отдать новую дублированную"
        val noBackupRow = LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding(0, AndroidUtilities.dp(4f), 0, AndroidUtilities.dp(4f))
        }
        val noBackupCb = CheckBox(context).apply { isChecked = newWhenNoBackup }
        val noBackupTexts = LinearLayout(context).apply { orientation = LinearLayout.VERTICAL }
        noBackupTexts.addView(GgtgUi.title(context, "Если резервной нет — отдать новую дублированную", 14f))
        noBackupTexts.addView(GgtgUi.body(context, "Для аккаунтов без сохранённой резервной сессии будет автоматически создан новый дубликат", 12f))
        noBackupRow.addView(noBackupCb, LayoutHelper.createLinear(LayoutHelper.WRAP_CONTENT, LayoutHelper.WRAP_CONTENT))
        noBackupRow.addView(noBackupTexts, LayoutHelper.createLinear(0, LayoutHelper.WRAP_CONTENT, 1f))
        noBackupRow.setOnClickListener {
            noBackupCb.isChecked = !noBackupCb.isChecked
            newWhenNoBackup = noBackupCb.isChecked
        }
        backupOptionsCard.addView(noBackupRow)
        GgtgUi.addCard(root, backupOptionsCard)

        refreshSources()

        // 5. Additional options
        root.addView(GgtgUi.sectionLabel(context, "4. Настройки файлов"))
        val extraCard = GgtgUi.card(context)
        val sidecarRow = LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding(0, AndroidUtilities.dp(4f), 0, AndroidUtilities.dp(4f))
        }
        val sidecarCb = CheckBox(context).apply { isChecked = sidecarsEnabled }
        val sidecarTexts = LinearLayout(context).apply { orientation = LinearLayout.VERTICAL }
        sidecarTexts.addView(GgtgUi.title(context, "Положить рядом 2fa.txt и ggtg.json", 14f))
        sidecarTexts.addView(GgtgUi.body(context, "Сохранить файл пароля 2FA, параметры устройства, прокси и папку аккаунта", 12f))
        sidecarRow.addView(sidecarCb, LayoutHelper.createLinear(LayoutHelper.WRAP_CONTENT, LayoutHelper.WRAP_CONTENT))
        sidecarRow.addView(sidecarTexts, LayoutHelper.createLinear(0, LayoutHelper.WRAP_CONTENT, 1f))
        sidecarRow.setOnClickListener {
            sidecarCb.isChecked = !sidecarCb.isChecked
            sidecarsEnabled = sidecarCb.isChecked
        }
        extraCard.addView(sidecarRow)
        GgtgUi.addCard(root, extraCard)

        // 6. Action button
        GgtgUi.addButton(root, GgtgUi.button(context, "Экспортировать") { startExport() }, top = 16f)
        root.addView(GgtgUi.spacer(context))

        return frame
    }

    private fun startExport() {
        val context = context ?: return
        val activity = parentActivity as? androidx.fragment.app.FragmentActivity ?: return
        val slots = picker?.selectedSlots().orEmpty()
        if (slots.isEmpty()) {
            Toast.makeText(context, "Выберите хотя бы один аккаунт", Toast.LENGTH_SHORT).show()
            return
        }

        val options = TdataExportHelper.ExportOptions(
            format = selectedFormat,
            source = selectedSource,
            newWhenNoBackup = newWhenNoBackup,
            sidecars = sidecarsEnabled
        )

        TdataExportHelper.exportWithOptions(activity, slots, options) { message ->
            GgtgApiOverride.inform(context, "Экспорт аккаунтов", message)
        }
    }
}
