package app.ggtg

import android.content.Context
import android.view.Gravity
import android.view.View
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.ScrollView
import org.telegram.messenger.AndroidUtilities
import org.telegram.messenger.R
import org.telegram.ui.ActionBar.ActionBar.ActionBarMenuOnItemClick
import org.telegram.ui.ActionBar.BaseFragment
import org.telegram.ui.ActionBar.Theme
import org.telegram.ui.Components.LayoutHelper

// ng: the GGTG hub - one screen listing every bulk-tools section, mirroring the desktop app's
// GGTG window (window/window_main_menu.cpp). Each row opens its own fragment. Titles kept in
// Russian to match the desktop build the user works from. With inTab it is the second page of the
// bottom bar (no back button, room under the list for the floating tabs).
class GgtgHubActivity @JvmOverloads constructor(private val inTab: Boolean = false) : BaseFragment() {

    private data class Section(
        val title: String,
        val description: String,
        val icon: Int,
        val kind: GgtgProcess.Kind? = null,
        val open: (GgtgHubActivity) -> Unit
    )

    private val sections = listOf(
        Section("Менеджер аккаунтов", "Аккаунты по папкам, статус SpamBot и массовые действия: отметьте аккаунты → «Действия», или долгое нажатие на аккаунт.",
            R.drawable.msg_contacts, GgtgProcess.Kind.Bulk) { it.presentFragment(GgtgAccountManagerActivity()) },
        Section("База", "Списки групп и пользователей: правка, экспорт, операции, импорт, поиск групп.",
            R.drawable.msg_folders) { it.presentFragment(GgtgBasesActivity()) },
        Section("Сквидвард", "Пробив через @skvidvarde_bot, сбор групп человека или участников группы в базу.",
            R.drawable.msg_squidward) { it.presentFragment(GgtgSquidwardActivity()) },
        Section("Рассылка SMS", "Сообщение по шаблону всем из выбранных баз, с паузами и лимитами.",
            R.drawable.msg_send, GgtgProcess.Kind.Sms) { it.presentFragment(GgtgSmsActivity()) },
        Section("Накрутка", "Просмотры постов и вступление в каналы/группы.",
            R.drawable.msg_channel, GgtgProcess.Kind.Views) { it.presentFragment(GgtgViewsActivity()) },
        Section("Прогрев", "Повышение траста аккаунтов: посты, реакции, белые боты и взаимные P2P диалоги.",
            R.drawable.msg_stats, GgtgProcess.Kind.Warmup) { it.presentFragment(GgtgWarmupActivity()) },
        Section("Прокси", "Пул прокси: списки, правка, проверка — расходуется при импорте .ggtg.",
            R.drawable.msg_language) { it.presentFragment(GgtgProxyActivity()) },
        Section("Процессы", "Все идущие и завершённые операции с логом.",
            R.drawable.msg_recent) { it.presentFragment(GgtgProcessesActivity()) },
        Section("ИИ", "Чат с вашим помощником: подключение, разговор, обучение и настройки в одном месте. Бесплатно и без карты.",
            R.drawable.msg_bots) { it.presentFragment(app.ggtg.ai.GgtgAiBrainActivity(false)) },
        Section("Списки чатов", "Свои списки переписок (заинтересованные, подумать, клиенты): открыть чат в один клик или написать сразу всем.",
            R.drawable.msg_list) { it.presentFragment(app.ggtg.ai.GgtgChatListsActivity()) },
        Section("Импорт Сессия + JSON", "Файлы .session и .json (Telethon, Pyrogram, софт): 2FA, параметры устройства и прокси.",
            R.drawable.msg_download) { it.presentFragment(GgtgSessionJsonImportActivity()) },
        Section("Импорт .ggtg", "Аккаунты из GGTG Desktop: сессия, папка, прокси и 2FA — сразу.",
            R.drawable.msg_addcontact) { it.presentFragment(GgtgImportActivity()) },
        Section("Импорт tdata", "Дублирование сессии из Telegram Desktop: папка или архив, прокси обязателен.",
            R.drawable.msg_folders) { it.presentFragment(GgtgTdataImportActivity()) },
        Section("Экспорт аккаунтов", "Экспорт в TData (ПК), Сессия + JSON (Android) или .ggtg: основные, с дубликацией или резервные.",
            R.drawable.msg_forward) { it.presentFragment(GgtgTdataExportActivity()) },
        Section("Синхронизация", "Базы, отправленные, шаблоны и память ИИ между этим телефоном и GGTG Desktop через один главный аккаунт.",
            R.drawable.msg_retry) { it.presentFragment(GgtgSyncActivity()) }
    )

    // Group headings, shown above the section at that index.
    private val groups = mapOf(0 to "Аккаунты и данные", 3 to "Рассылка и накрутка", 9 to "ИИ и списки", 11 to "Импорт и экспорт")

    private var root: LinearLayout? = null
    private val processListener = { AndroidUtilities.runOnUIThread { rebuild() } }

    override fun onResume() { super.onResume(); GgtgProcess.addListener(processListener); rebuild() }
    override fun onPause() { super.onPause(); GgtgProcess.removeListener(processListener) }

    override fun createView(context: Context): View {
        if (!inTab) actionBar.setBackButtonImage(R.drawable.ic_ab_back)
        actionBar.setAllowOverlayTitle(true)
        actionBar.setTitle("Главная")
        actionBar.setActionBarMenuOnItemClick(object : ActionBarMenuOnItemClick() {
            override fun onItemClick(id: Int) {
                if (id == -1) finishFragment()
            }
        })

        val frame = FrameLayout(context)
        frame.setBackgroundColor(Theme.getColor(Theme.key_windowBackgroundGray))
        fragmentView = frame
        val scroll = ScrollView(context)
        val column = LinearLayout(context).apply { orientation = LinearLayout.VERTICAL }
        scroll.addView(column, LayoutHelper.createScroll(LayoutHelper.MATCH_PARENT, LayoutHelper.WRAP_CONTENT, Gravity.TOP))
        // As a tab the action bar floats over the page (no layout container pushes the content down).
        val top = if (inTab) org.telegram.ui.ActionBar.ActionBar.getCurrentActionBarHeight() + AndroidUtilities.statusBarHeight else 0
        frame.addView(scroll, FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT).apply { topMargin = top })
        root = column
        rebuild()
        return frame
    }

    private fun rebuild() {
        val column = root ?: return
        val context = column.context
        column.removeAllViews()
        // the dashboard over the list of sections (the desktop's "Главная")
        runCatching { GgtgHome.build(this, column, context) }
        for ((index, section) in sections.withIndex()) {
            groups[index]?.let { column.addView(GgtgUi.sectionLabel(context, it)) }
            val card = GgtgUi.card(context) { section.open(this) }
            val row = LinearLayout(context).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
            val icon = context.resources.getDrawable(section.icon).mutate()
            icon.colorFilter = android.graphics.PorterDuffColorFilter(GgtgUi.accent, android.graphics.PorterDuff.Mode.MULTIPLY)
            val badge = android.widget.ImageView(context).apply {
                setImageDrawable(icon)
                scaleType = android.widget.ImageView.ScaleType.CENTER
                background = GgtgUi.rounded((GgtgUi.accent and 0x00FFFFFF) or 0x26000000, GgtgUi.dp(20f).toFloat())
            }
            row.addView(badge, LinearLayout.LayoutParams(GgtgUi.dp(40f), GgtgUi.dp(40f)).apply { rightMargin = GgtgUi.dp(12f) })
            val texts = LinearLayout(context).apply { orientation = LinearLayout.VERTICAL }
            val titleRow = LinearLayout(context).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
            titleRow.addView(GgtgUi.title(context, section.title))
            val running = when {
                section.title == "Процессы" -> GgtgProcess.runningCount()
                section.kind != null && GgtgProcess.running(section.kind) != null -> 1
                else -> 0
            }
            if (running > 0) {
                val text = if (section.title == "Процессы") "$running идёт" else "идёт"
                titleRow.addView(GgtgUi.pill(context, text, 0xFFFFFFFF.toInt(), GgtgUi.accent),
                    LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply { leftMargin = GgtgUi.dp(8f) })
            }
            texts.addView(titleRow)
            texts.addView(GgtgUi.body(context, section.description), LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply { topMargin = GgtgUi.dp(2f) })
            row.addView(texts, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
            row.addView(android.widget.TextView(context).apply {
                text = "›"; textSize = 22f
                setTextColor(Theme.getColor(Theme.key_windowBackgroundWhiteGrayText))
            }, LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply { leftMargin = GgtgUi.dp(8f) })
            card.addView(row)
            GgtgUi.addCard(column, card)
        }
        val bottom = if (inTab) GgtgUi.dp(16f) + AndroidUtilities.dp(org.telegram.ui.DialogsActivity.MAIN_TABS_HEIGHT_WITH_MARGINS.toFloat()) else GgtgUi.dp(16f)
        column.addView(View(context), LinearLayout.LayoutParams(1, bottom))
    }
}
