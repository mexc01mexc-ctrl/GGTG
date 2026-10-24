package app.ggtg

import android.content.Context
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.view.Gravity
import android.view.View
import android.widget.LinearLayout
import android.widget.TextView
import org.telegram.ui.ActionBar.BaseFragment
import org.telegram.ui.ActionBar.Theme
import java.text.SimpleDateFormat
import java.util.Calendar
import java.util.Date
import java.util.Locale

// ggtg: "Главная" of the GGTG tab, like the desktop one: a greeting with the one thing to do, tiles
// with the numbers, what is running right now, the answers of people and (until everything is set
// up) the first steps. Drawn in the colors of the theme. It sits above the list of sections.
object GgtgHome {

    private fun dp(v: Float) = GgtgUi.dp(v)
    private val green = 0xFF3FC25C.toInt()
    private val orange = 0xFFFF9F0A.toInt()

    private fun gray() = Theme.getColor(Theme.key_windowBackgroundWhiteGrayText)
    private fun text() = Theme.getColor(Theme.key_windowBackgroundWhiteBlackText)

    private class Numbers(
        val accounts: Int, val blocked: Int, val remaining: Int, val userBases: Int,
        val lastValue: String, val lastSub: String, val hasRun: Boolean, val proxyLists: Int
    )

    private fun numbers(): Numbers {
        val live = GgtgAccounts.activated().filter { !it.folder.equals(GgtgAccounts.TRASH_FOLDER) }
        var blocked = 0
        for (ref in live) if (GgtgAccountStatus.of(ref.slot).rank in 4..6) blocked++
        var bases = 0
        var remaining = 0
        for (meta in GgtgDb.loadIndex()) if (meta.kind == GgtgDb.BaseKind.Users) { bases++; remaining += GgtgDb.remainingIn(meta.id) }
        // The last run: the records that follow each other with gaps under two hours
        val records = GgtgDb.loadAllSmsRecords()
        var value = "Пока не было"
        var sub = ""
        if (records.isNotEmpty()) {
            val run = ArrayList<GgtgDb.SmsRecord>()
            var prev = records.first().at
            for (r in records) { if (prev - r.at > 2 * 3600) break; run.add(r); prev = r.at }
            val sent = run.count { it.status == GgtgDb.SmsStatus.Sent }
            val replied = run.count { GgtgDb.findReply(it.username) != null }
            value = SimpleDateFormat("dd.MM HH:mm", Locale.getDefault()).format(Date(records.first().at * 1000))
            sub = "отправлено $sent · ответили $replied"
        }
        return Numbers(live.size, blocked, remaining, bases, value, sub, records.isNotEmpty(), 0)
    }

    /** Adds the dashboard to [column]; [open] opens a fragment of the hub. */
    fun build(fragment: BaseFragment, column: LinearLayout, context: Context) {
        val n = numbers()
        val aiReady = app.ggtg.ai.GgtgAi.ready()
        val running = GgtgProcess.snapshots().filter { it.status == GgtgProcess.Status.Running }
        val open = { f: BaseFragment -> fragment.presentFragment(f) }

        // ---- the greeting ----
        val hour = Calendar.getInstance().get(Calendar.HOUR_OF_DAY)
        val greeting = when (hour) { in 5..11 -> "Доброе утро"; in 12..17 -> "Добрый день"; in 18..22 -> "Добрый вечер"; else -> "Доброй ночи" }
        var message: String
        var button: String
        var target: () -> Unit
        when {
            n.accounts == 0 -> { message = "Начнём с малого: добавьте первый аккаунт, и остальное оживёт."; button = "Добавить аккаунт"
                target = { open(GgtgAddAccountsActivity()) } }
            !aiReady -> { message = "Подключите бесплатный ИИ: это минута, карта не нужна."; button = "Открыть ИИ"
                target = { open(app.ggtg.ai.GgtgAiBrainActivity(false)) } }
            running.isNotEmpty() -> { message = "Сейчас идёт: ${running.first().title}" + (if (running.size > 1) " и ещё ${running.size - 1}" else "") + "."
                button = "Посмотреть ход"; target = { open(GgtgProcessesActivity()) } }
            n.remaining > 0 -> { message = "Всё спокойно. В базах ждут ${n.remaining} человек - можно запускать рассылку."; button = "Запустить рассылку"
                target = { open(GgtgSmsActivity()) } }
            else -> { message = "Всё спокойно. Найдите аудиторию в Сквидварде и сохраните её в базу."; button = "Открыть Сквидвард"
                target = { open(GgtgSquidwardActivity()) } }
        }
        val hero = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(18f), dp(16f), dp(18f), dp(16f))
            background = GradientDrawable(GradientDrawable.Orientation.TL_BR,
                intArrayOf((GgtgUi.accent and 0x00FFFFFF) or 0x55000000, Theme.getColor(Theme.key_windowBackgroundWhite))).apply { cornerRadius = dp(16f).toFloat() }
        }
        hero.addView(TextView(context).apply {
            this.text = greeting; textSize = 21f; setTypeface(null, Typeface.BOLD); setTextColor(text())
        })
        hero.addView(TextView(context).apply {
            this.text = message; textSize = 14f; setTextColor(text())
        }, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply { topMargin = dp(6f) })
        hero.addView(TextView(context).apply {
            this.text = button; textSize = 14f; setTypeface(null, Typeface.BOLD); setTextColor(0xFFFFFFFF.toInt())
            gravity = Gravity.CENTER
            setPadding(dp(18f), dp(9f), dp(18f), dp(9f))
            background = GgtgUi.rounded(GgtgUi.accent, dp(18f).toFloat())
            setOnClickListener { target() }
        }, LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply { topMargin = dp(12f) })
        GgtgUi.addCard(column, hero, top = 12f)

        // ---- the numbers ----
        fun tile(label: String, value: String, sub: String, color: Int, onClick: () -> Unit): View = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(14f), dp(12f), dp(14f), dp(12f))
            background = android.graphics.drawable.RippleDrawable(
                android.content.res.ColorStateList.valueOf(Theme.getColor(Theme.key_listSelector)),
                GgtgUi.rounded(Theme.getColor(Theme.key_windowBackgroundWhite), dp(14f).toFloat()), null)
            setOnClickListener { onClick() }
            addView(TextView(context).apply { this.text = label; textSize = 12f; setTextColor(gray()); maxLines = 1 })
            addView(TextView(context).apply { this.text = value; textSize = 22f; setTypeface(null, Typeface.BOLD); setTextColor(color); maxLines = 1 },
                LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply { topMargin = dp(2f) })
            if (sub.isNotEmpty()) addView(TextView(context).apply { this.text = sub; textSize = 12f; setTextColor(gray()) },
                LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply { topMargin = dp(2f) })
        }
        val tiles = listOf(
            tile("Аккаунты", n.accounts.toString(), if (n.blocked > 0) "спам-блок: ${n.blocked}" else if (n.accounts > 0) "подключены" else "пока нет",
                if (n.blocked > 0) orange else GgtgUi.accent) { open(GgtgAccountManagerActivity()) },
            tile("Уже написали", GgtgDb.countSmsByStatus(GgtgDb.SmsStatus.Sent).toString(), "осталось написать: ${n.remaining} · списков: ${n.userBases}", GgtgUi.accent) { open(GgtgSmsActivity()) },
            tile("Последняя рассылка", n.lastValue, n.lastSub, GgtgUi.accent) { open(GgtgSmsActivity()) },
            tile("ИИ", if (aiReady) "Работает" else "Нужен ключ",
                if (aiReady) "всё в порядке" else "бесплатный, за минуту",
                if (aiReady) green else orange) { open(app.ggtg.ai.GgtgAiBrainActivity(false)) }
        )
        for (pair in tiles.chunked(2)) {
            val row = LinearLayout(context).apply { orientation = LinearLayout.HORIZONTAL }
            pair.forEachIndexed { i, view ->
                row.addView(view, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.MATCH_PARENT, 1f).apply { if (i == 0) rightMargin = dp(5f) else leftMargin = dp(5f) })
            }
            column.addView(row, LayoutHelperLinear(top = 10f))
        }

        // ---- what is running ----
        column.addView(GgtgUi.sectionLabel(context, "Сейчас"))
        if (running.isEmpty()) {
            column.addView(GgtgUi.body(context, "Сейчас ничего не запущено.").apply { setPadding(dp(18f), dp(4f), dp(18f), dp(4f)) })
        }
        for (p in running) {
            val card = GgtgUi.card(context) { open(GgtgProcessesActivity()) }
            card.addView(GgtgUi.title(context, p.title, 15f))
            card.addView(GgtgUi.body(context, if (p.progressTotal > 0) "Обработано: ${p.progressCurrent} из ${p.progressTotal}" else "Выполняется..."),
                LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply { topMargin = dp(2f) })
            if (p.progressTotal > 0) {
                val bar = LinearLayout(context).apply { orientation = LinearLayout.HORIZONTAL; background = GgtgUi.rounded((gray() and 0x00FFFFFF) or 0x33000000, dp(3f).toFloat()) }
                val ratio = (p.progressCurrent.toFloat() / p.progressTotal).coerceIn(0.02f, 1f)
                bar.addView(View(context).apply { background = GgtgUi.rounded(GgtgUi.accent, dp(3f).toFloat()) }, LinearLayout.LayoutParams(0, dp(6f), ratio))
                bar.addView(View(context), LinearLayout.LayoutParams(0, dp(6f), 1f - ratio))
                card.addView(bar, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, dp(6f)).apply { topMargin = dp(8f) })
            }
            GgtgUi.addCard(column, card)
        }

        // ---- the first steps, until everything is set up ----
        class Step(val done: Boolean, val title: String, val hint: String, val go: () -> Unit)
        val steps = listOf(
            Step(n.accounts > 0, "1. Добавьте аккаунты", "Прокси, импорт или вход по номеру") {
                open(if (n.accounts > 0) GgtgAccountManagerActivity() else GgtgAddAccountsActivity()) },
            Step(n.userBases > 0, "2. Найдите аудиторию", "Пробейте группу в Сквидварде и сохраните людей в базу") { open(GgtgSquidwardActivity()) },
            Step(n.hasRun, "3. Запустите рассылку", "Выберите аккаунты, базу и текст") { open(GgtgSmsActivity()) }
        )
        if (steps.any { !it.done }) {
            column.addView(GgtgUi.sectionLabel(context, "Быстрый старт"))
            for (s in steps) {
                val card = GgtgUi.card(context) { s.go() }
                val row = LinearLayout(context).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
                val texts = LinearLayout(context).apply { orientation = LinearLayout.VERTICAL }
                texts.addView(GgtgUi.title(context, s.title, 15f))
                texts.addView(GgtgUi.body(context, s.hint))
                row.addView(texts, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
                if (s.done) row.addView(GgtgUi.pill(context, "готово", 0xFFFFFFFF.toInt(), green))
                card.addView(row)
                GgtgUi.addCard(column, card)
            }
        }
        column.addView(GgtgUi.sectionLabel(context, "Разделы"))
    }

    private fun LayoutHelperLinear(top: Float) = LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply {
        leftMargin = dp(12f); rightMargin = dp(12f); topMargin = dp(top)
    }
}

