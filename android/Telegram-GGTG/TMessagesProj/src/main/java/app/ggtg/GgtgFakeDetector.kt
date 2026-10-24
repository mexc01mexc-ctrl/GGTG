package app.ggtg

import android.content.Context
import android.view.Gravity
import android.view.View
import android.widget.LinearLayout
import android.widget.RadioButton
import android.widget.RadioGroup
import android.widget.ScrollView
import android.widget.TextView
import android.widget.Toast
import app.ggtg.ai.GgtgAi
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONArray
import org.json.JSONObject
import org.telegram.messenger.AndroidUtilities
import org.telegram.messenger.ApplicationLoader
import org.telegram.messenger.FileLog
import org.telegram.messenger.UserObject
import org.telegram.tgnet.TLRPC
import org.telegram.ui.ActionBar.AlertDialog
import org.telegram.ui.ActionBar.Theme
import java.io.File

object GgtgFakeDetector {

    enum class Verdict {
        Alive,
        Suspicious,
        Fake
    }

    data class AnalysisResult(
        val fakeProbability: Int, // 0..100
        val verdict: Verdict,
        val verdictText: String,
        val reasons: List<String>,
        val isMarkedFake: Boolean = false,
        val isMarkedAlive: Boolean = false
    )

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main)

    private fun patternsFile(): File {
        val dir = File(ApplicationLoader.applicationContext.filesDir, "ggtg_bulk_db")
        dir.mkdirs()
        return File(dir, "fake_patterns.json")
    }

    data class StoredPatterns(
        val markedFakeIds: MutableSet<Long> = mutableSetOf(),
        val markedAliveIds: MutableSet<Long> = mutableSetOf(),
        val markedFakeUsernames: MutableSet<String> = mutableSetOf(),
        val markedAliveUsernames: MutableSet<String> = mutableSetOf(),
        val customKeywords: MutableSet<String> = mutableSetOf()
    )

    @Synchronized
    fun loadPatterns(): StoredPatterns {
        val file = patternsFile()
        if (!file.exists()) return StoredPatterns()
        return try {
            val json = JSONObject(file.readText())
            val fakeIds = mutableSetOf<Long>()
            json.optJSONArray("marked_fake_ids")?.let { arr ->
                for (i in 0 until arr.length()) fakeIds.add(arr.optLong(i))
            }
            val aliveIds = mutableSetOf<Long>()
            json.optJSONArray("marked_alive_ids")?.let { arr ->
                for (i in 0 until arr.length()) aliveIds.add(arr.optLong(i))
            }
            val fakeUsers = mutableSetOf<String>()
            json.optJSONArray("marked_fake_usernames")?.let { arr ->
                for (i in 0 until arr.length()) fakeUsers.add(arr.optString(i).lowercase().trim())
            }
            val aliveUsers = mutableSetOf<String>()
            json.optJSONArray("marked_alive_usernames")?.let { arr ->
                for (i in 0 until arr.length()) aliveUsers.add(arr.optString(i).lowercase().trim())
            }
            val keywords = mutableSetOf<String>()
            json.optJSONArray("custom_keywords")?.let { arr ->
                for (i in 0 until arr.length()) keywords.add(arr.optString(i).lowercase().trim())
            }
            StoredPatterns(fakeIds, aliveIds, fakeUsers, aliveUsers, keywords)
        } catch (t: Throwable) {
            FileLog.e(t)
            StoredPatterns()
        }
    }

    @Synchronized
    fun savePatterns(patterns: StoredPatterns) {
        try {
            val json = JSONObject().apply {
                put("marked_fake_ids", JSONArray(patterns.markedFakeIds))
                put("marked_alive_ids", JSONArray(patterns.markedAliveIds))
                put("marked_fake_usernames", JSONArray(patterns.markedFakeUsernames))
                put("marked_alive_usernames", JSONArray(patterns.markedAliveUsernames))
                put("custom_keywords", JSONArray(patterns.customKeywords))
            }
            patternsFile().writeText(json.toString(2))
        } catch (t: Throwable) {
            FileLog.e(t)
        }
    }

    @JvmStatic
    fun markAsFake(userId: Long, username: String?) {
        val p = loadPatterns()
        if (userId > 0) {
            p.markedFakeIds.add(userId)
            p.markedAliveIds.remove(userId)
        }
        val clean = username?.trim()?.removePrefix("@")?.lowercase().orEmpty()
        if (clean.isNotEmpty()) {
            p.markedFakeUsernames.add(clean)
            p.markedAliveUsernames.remove(clean)
        }
        savePatterns(p)
    }

    @JvmStatic
    fun markAsAlive(userId: Long, username: String?) {
        val p = loadPatterns()
        if (userId > 0) {
            p.markedAliveIds.add(userId)
            p.markedFakeIds.remove(userId)
        }
        val clean = username?.trim()?.removePrefix("@")?.lowercase().orEmpty()
        if (clean.isNotEmpty()) {
            p.markedAliveUsernames.add(clean)
            p.markedFakeUsernames.remove(clean)
        }
        savePatterns(p)
    }

    @JvmStatic
    fun clearMark(userId: Long, username: String?) {
        val p = loadPatterns()
        if (userId > 0) {
            p.markedFakeIds.remove(userId)
            p.markedAliveIds.remove(userId)
        }
        val clean = username?.trim()?.removePrefix("@")?.lowercase().orEmpty()
        if (clean.isNotEmpty()) {
            p.markedFakeUsernames.remove(clean)
            p.markedAliveUsernames.remove(clean)
        }
        savePatterns(p)
    }

    @JvmStatic
    fun analyzeUsername(rawUsername: String): AnalysisResult {
        val username = rawUsername.trim().removePrefix("@").lowercase()
        if (username.isEmpty()) {
            return AnalysisResult(30, Verdict.Alive, "Без юзернейма", emptyList())
        }

        val patterns = loadPatterns()
        if (patterns.markedFakeUsernames.contains(username)) {
            return AnalysisResult(100, Verdict.Fake, "Помечен как фейк (100%)", listOf("В вашем чёрном списке"), isMarkedFake = true)
        }
        if (patterns.markedAliveUsernames.contains(username)) {
            return AnalysisResult(0, Verdict.Alive, "Помечен как живой (0%)", listOf("В вашем белом списке"), isMarkedAlive = true)
        }

        var score = 15
        val reasons = mutableListOf<String>()

        if (username.endsWith("bot") || username.endsWith("_bot") || username.endsWith("robot")) {
            score += 65
            reasons.add("Имя бота")
        }

        if (username.matches(Regex(".*\\d{4,}$"))) {
            score += 35
            reasons.add("Случайный цифровой хвост")
        } else if (username.matches(Regex(".*\\d{3,}$"))) {
            score += 15
            reasons.add("Цифры в конце ника")
        }

        val spamKeywords = listOf(
            "crypto", "ton", "airdrop", "invest", "profit",
            "заработок", "сигнал", "trade", "trading", "bonus",
            "claim", "casino", "support", "official", "manager"
        )
        for (kw in spamKeywords) {
            if (username.contains(kw)) {
                score += 30
                reasons.add("Спам-слово: $kw")
                break
            }
        }

        if (username.length >= 6 && username.matches(Regex("^[bcdfghjklmnpqrstvwxyz_0-9]+$"))) {
            score += 25
            reasons.add("Нечитаемый никнейм")
        }

        score = score.coerceIn(0, 100)
        val verdict = when {
            score >= 65 -> Verdict.Fake
            score >= 35 -> Verdict.Suspicious
            else -> Verdict.Alive
        }
        val text = when (verdict) {
            Verdict.Fake -> "Фейк ($score%)"
            Verdict.Suspicious -> "Подозрительный ($score%)"
            Verdict.Alive -> "Живой (${100 - score}%)"
        }
        return AnalysisResult(score, verdict, text, reasons)
    }

    @JvmStatic
    @JvmOverloads
    fun analyzeUser(user: TLRPC.User?, fullUser: TLRPC.UserFull?, photoDates: List<Int> = emptyList()): AnalysisResult {
        if (user == null) {
            return AnalysisResult(0, Verdict.Alive, "Неизвестно", emptyList())
        }

        val userId = user.id
        val cleanUser = user.username?.trim()?.removePrefix("@")?.lowercase().orEmpty()
        val patterns = loadPatterns()

        if (userId > 0 && patterns.markedFakeIds.contains(userId) || cleanUser.isNotEmpty() && patterns.markedFakeUsernames.contains(cleanUser)) {
            return AnalysisResult(100, Verdict.Fake, "Помечен вами как фейк (100%)", listOf("Помечен вручную как фейк"), isMarkedFake = true)
        }
        if (userId > 0 && patterns.markedAliveIds.contains(userId) || cleanUser.isNotEmpty() && patterns.markedAliveUsernames.contains(cleanUser)) {
            return AnalysisResult(0, Verdict.Alive, "Помечен вами как живой (0%)", listOf("Помечен вручную как живой"), isMarkedAlive = true)
        }

        var score = 20
        val reasons = mutableListOf<String>()

        if (user.scam || user.fake) {
            score += 70
            reasons.add("Метка Scam/Fake от Telegram")
        }
        if (user.bot) {
            score += 70
            reasons.add("Telegram Bot")
        }
        if (user.verified) {
            score -= 50
            reasons.add("Верифицированный аккаунт")
        }
        if (user.premium) {
            score -= 35
            reasons.add("Telegram Premium")
        }

        val dates = if (photoDates.isNotEmpty()) {
            photoDates
        } else if (fullUser?.profile_photo != null && fullUser.profile_photo.date > 0) {
            listOf(fullUser.profile_photo.date)
        } else emptyList()

        val hasPhoto = user.photo != null && user.photo !is TLRPC.TL_userProfilePhotoEmpty
        if (!hasPhoto) {
            score += 30
            reasons.add("Нет фото профиля")
        } else {
            score -= 15
            val nowSec = (System.currentTimeMillis() / 1000L).toInt()
            if (dates.isNotEmpty()) {
                val latest = dates.maxOrNull() ?: 0
                if (latest in 1..nowSec) {
                    val age = nowSec - latest
                    if (age < 48 * 3600 && (userId > 7000000000L || cleanUser.isEmpty())) {
                        score += 20
                        reasons.add("Свежее фото профиля (< 48ч) на новом аккаунте")
                    } else if (age > 365 * 86400) {
                        score -= 25
                        reasons.add("Старое фото профиля (> 1 года, признак живого)")
                    }
                }
                if (dates.size >= 2) {
                    val sorted = dates.filter { it > 0 }.sorted()
                    var quickBatch = false
                    for (i in 1 until sorted.size) {
                        if (sorted[i] - sorted[i - 1] in 0 until 7200) {
                            quickBatch = true
                            break
                        }
                    }
                    if (quickBatch) {
                        score += 35
                        reasons.add("Фото залиты почти одновременно (< 2ч, признак авторега)")
                    } else if (sorted.last() - sorted.first() > 30 * 86400) {
                        score -= 20
                        reasons.add("Фото профиля менялись со временем (естественный аккаунт)")
                    }
                }
            }
        }

        // Registration Age by ID
        if (userId > 7900000000L) {
            score += 25
            reasons.add("Очень свежий аккаунт (2025–2026)")
        } else if (userId > 7200000000L) {
            score += 15
            reasons.add("Свежий аккаунт (2024–2025)")
        } else if (userId in 1..2500000000L) {
            score -= 20
            reasons.add("Старый аккаунт")
        }

        // Username
        if (cleanUser.isEmpty()) {
            score += 10
            if (userId > 7500000000L) {
                score += 15
                reasons.add("Новый ID без юзернейма (массовый авторег)")
            } else {
                reasons.add("Без @username")
            }
        } else {
            val uAnalysis = analyzeUsername(cleanUser)
            if (uAnalysis.fakeProbability >= 65) {
                score += 30
                reasons.addAll(uAnalysis.reasons)
            } else if (uAnalysis.fakeProbability <= 15) {
                score -= 15
            }
        }

        // Name
        val fullName = "${user.first_name.orEmpty()} ${user.last_name.orEmpty()}".lowercase().trim()
        val cleanFirst = user.first_name.orEmpty().trim().lowercase()
        val cleanLast = user.last_name.orEmpty().trim().lowercase()

        // Check repeated first and last name
        if (cleanFirst.isNotEmpty() && cleanLast.isNotEmpty() && cleanFirst == cleanLast && cleanFirst.length >= 3) {
            score += 25
            reasons.add("Одинаковые имя и фамилия («${user.first_name} ${user.last_name}»)")
        }

        // Check decorative unicode font in name (surrogate 0xD835 or U+1D400..U+1D7FF)
        var hasDecorative = false
        for (ch in "${user.first_name.orEmpty()}${user.last_name.orEmpty()}") {
            if (ch.code in 0x1D400..0x1D7FF || ch.code == 0xD835) {
                hasDecorative = true
                break
            }
        }
        if (hasDecorative) {
            score += 30
            reasons.add("Декоративный Unicode-шрифт в имени (почерк ботов)")
        }

        // Check emoji-only / symbol-only name
        if (cleanFirst.isNotEmpty() && cleanFirst.none { it.isLetterOrDigit() }) {
            score += 25
            reasons.add("Имя состоит только из символов/эмодзи")
        }

        val nameKeywords = listOf("crypto", "ton", "invest", "заработок", "сигналы", "manager", "admin", "support", "arbitrage")
        for (kw in nameKeywords) {
            if (fullName.contains(kw)) {
                score += 25
                reasons.add("Спам-слово в имени: $kw")
                break
            }
        }

        // Bio
        val bio = fullUser?.about.orEmpty().lowercase()
        if (bio.isNotEmpty()) {
            if (bio.contains("t.me/") || bio.contains("http") || bio.contains("bit.ly")) {
                score += 25
                reasons.add("Рекламная ссылка в Bio")
            }
            val bioKeywords = listOf("канал", "личка", "пиши сюда", "крипт", "profit", "usdt", "whatsapp", "бонус", "вывод", "инвестиции")
            for (kw in bioKeywords) {
                if (bio.contains(kw)) {
                    score += 20
                    reasons.add("Спам-фраза в Bio: $kw")
                    break
                }
            }
        }

        // Custom keywords from AI learning
        for (kw in patterns.customKeywords) {
            if (kw.isNotEmpty() && (cleanUser.contains(kw) || fullName.contains(kw) || bio.contains(kw))) {
                score += 25
                reasons.add("Ключевое слово из ИИ-обучения: $kw")
                break
            }
        }

        // Online status
        val status = user.status
        if (status is TLRPC.TL_userStatusEmpty) {
            score += 20
            reasons.add("Давно не был в сети")
        }

        score = score.coerceIn(0, 100)
        val verdict = when {
            score >= 65 -> Verdict.Fake
            score >= 35 -> Verdict.Suspicious
            else -> Verdict.Alive
        }
        val text = when (verdict) {
            Verdict.Fake -> "Фейк ($score%)"
            Verdict.Suspicious -> "Подозрительный ($score%)"
            Verdict.Alive -> "Живой (${100 - score}%)"
        }
        return AnalysisResult(score, verdict, text, reasons)
    }

    @JvmStatic
    fun filterUsernames(usernames: List<String>, thresholdPercent: Int = 60): Pair<List<String>, List<String>> {
        val alive = mutableListOf<String>()
        val fakes = mutableListOf<String>()
        for (u in usernames) {
            val res = analyzeUsername(u)
            if (res.fakeProbability >= thresholdPercent) {
                fakes.add(u)
            } else {
                alive.add(u)
            }
        }
        return Pair(alive, fakes)
    }

    @JvmStatic
    fun trainFromMarked(context: Context, onDone: ((Int) -> Unit)? = null) {
        val patterns = loadPatterns()
        val fakesList = patterns.markedFakeUsernames.toMutableList()
        // Collect from GgtgDb as well
        for (rec in GgtgDb.loadAllSmsRecords()) {
            if (rec.status == GgtgDb.SmsStatus.SuspectedFake && !fakesList.contains(rec.username)) {
                fakesList.add(rec.username)
            }
        }
        if (fakesList.isEmpty()) {
            Toast.makeText(context, "Нет помеченных фейков для обучения ИИ", Toast.LENGTH_SHORT).show()
            return
        }

        Toast.makeText(context, "ИИ анализирует базу фейков...", Toast.LENGTH_SHORT).show()

        val wordRe = Regex("[a-zA-Zа-яА-ЯёЁ]{3,}")
        val tokenCounts = HashMap<String, Int>()
        for (u in fakesList) {
            for (m in wordRe.findAll(u)) {
                val tok = m.value.lowercase()
                tokenCounts[tok] = (tokenCounts[tok] ?: 0) + 1
            }
        }
        val statisticalCandidates = tokenCounts.filter { (tok, count) ->
            count >= 2 && !patterns.markedAliveUsernames.contains(tok) && !patterns.customKeywords.contains(tok)
        }.keys.toList()

        val finish = { newKws: List<String>, summary: String ->
            val p = loadPatterns()
            val added = mutableListOf<String>()
            for (kw in newKws + statisticalCandidates) {
                val clean = kw.trim().lowercase()
                if (clean.isNotEmpty() && p.customKeywords.add(clean)) {
                    added.add(clean)
                }
            }
            savePatterns(p)

            val dir = File(ApplicationLoader.applicationContext.filesDir, "ggtg_bulk_db/ai_knowledge")
            dir.mkdirs()
            val kFile = File(dir, "fake_patterns.json")
            val kJson = JSONObject().apply {
                put("updated_at", System.currentTimeMillis() / 1000L)
                put("fakes_count", fakesList.size)
                put("keywords", JSONArray(p.customKeywords))
                put("summary", summary)
            }
            kFile.writeText(kJson.toString(2))

            val msg = buildString {
                append("Проанализировано фейков: ${fakesList.size}\nЖивых аккаунтов: ${patterns.markedAliveUsernames.size}\n\n")
                if (added.isNotEmpty()) {
                    append("Выявлено новых спам-токенов (${added.size}):\n• " + added.joinToString("\n• ") + "\n\n")
                } else {
                    append("Новых повторяющихся паттернов не обнаружено (все известные правила уже учтены).\n\n")
                }
                if (summary.isNotEmpty()) {
                    append("ИИ-заключение:\n$summary\n\n")
                }
                append("Все новые критерии сохранены и применяются при фильтрации и в рассылках.")
            }

            AlertDialog.Builder(context)
                .setTitle("Обучение детектора завершено")
                .setMessage(msg)
                .setPositiveButton("Отлично", null)
                .show()

            onDone?.invoke(added.size)
        }

        val sampleFakes = fakesList.take(40).joinToString(", ")
        val prompt = "Проанализируй список спам-ботов и фейковых Telegram-юзернеймов, помеченных пользователем:\n" +
            sampleFakes + "\n\nВыяви общие спам-шаблоны и назови 3-7 характерных ключевых слов или подозрительных корней слов. " +
            "Ответь в формате JSON:\n{\n  \"keywords\": [\"слово1\", \"слово2\"],\n  \"summary\": \"краткое описание шаблонов ботов\"\n}"

        scope.launch {
            val answer = withContext(Dispatchers.IO) {
                GgtgAi.ask(
                    "Ты эксперт по машинному обучению и выявлению ботов в Telegram. Анализируй шаблоны имен и возвращай строго JSON.",
                    prompt,
                    0.2,
                    500,
                    hard = false
                )
            }
            val text = answer.text.trim()
            val kws = mutableListOf<String>()
            var sum = ""
            val startIdx = text.indexOf('{')
            val endIdx = text.lastIndexOf('}')
            if (startIdx >= 0 && endIdx > startIdx) {
                try {
                    val obj = JSONObject(text.substring(startIdx, endIdx + 1))
                    obj.optJSONArray("keywords")?.let { arr ->
                        for (i in 0 until arr.length()) kws.add(arr.optString(i))
                    }
                    sum = obj.optString("summary")
                } catch (_: Throwable) {}
            }
            finish(kws, sum.ifEmpty { "Анализ проведён по частотности спам-токенов." })
        }
    }

    @JvmStatic
    fun showFakeDetailsDialog(
        context: Context,
        user: TLRPC.User?,
        fullUser: TLRPC.UserFull?,
        onUpdated: Runnable?
    ) {
        if (user == null) return
        val analysis = analyzeUser(user, fullUser)

        val root = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            val pad = AndroidUtilities.dp(16f)
            setPadding(pad, pad, pad, pad)
        }

        val verdictView = TextView(context).apply {
            text = "Вердикт: ${analysis.verdictText}"
            textSize = 17f
            setTypeface(null, android.graphics.Typeface.BOLD)
            setTextColor(Theme.getColor(Theme.key_windowBackgroundWhiteBlackText))
            setPadding(0, 0, 0, AndroidUtilities.dp(8f))
        }
        root.addView(verdictView)

        val reasonsText = if (analysis.reasons.isNotEmpty()) {
            "Факторы оценки:\n• " + analysis.reasons.joinToString("\n• ")
        } else {
            "Подозрительных признаков не обнаружено."
        }
        val reasonsView = TextView(context).apply {
            text = reasonsText
            textSize = 14f
            setTextColor(Theme.getColor(Theme.key_windowBackgroundWhiteGrayText))
            setPadding(0, 0, 0, AndroidUtilities.dp(16f))
        }
        root.addView(reasonsView)

        val aiStatusView = TextView(context).apply {
            textSize = 14f
            setTextColor(Theme.getColor(Theme.key_windowBackgroundWhiteBlackText))
            visibility = View.GONE
            setPadding(0, AndroidUtilities.dp(8f), 0, AndroidUtilities.dp(12f))
        }

        val aiButton = GgtgUi.button(context, "Запросить разбор у ИИ-нейросети") {
            aiStatusView.visibility = View.VISIBLE
            aiStatusView.text = "Анализирую профиль..."
            val prompt = "Оцени Telegram-профиль пользователя:\n" +
                "ID: ${user.id}\n" +
                "Username: @${user.username.orEmpty()}\n" +
                "Имя: ${UserObject.getUserName(user)}\n" +
                "Описание: ${fullUser?.about.orEmpty()}\n" +
                "Премиум: ${if (user.premium) "Да" else "Нет"}\n" +
                "Дай краткий вердикт: живой человек, бот или спам-аккаунт, и назови главную причину (до 2 предложений)."

            scope.launch {
                val answer = withContext(Dispatchers.IO) {
                    GgtgAi.ask(
                        "Ты эксперт по выявлению спам-ботов и фейковых аккаунтов в Telegram. Отвечай кратко и аргументированно.",
                        prompt,
                        0.2,
                        400,
                        hard = false
                    )
                }
                aiStatusView.text = "ИИ-вердикт:\n${answer.text.trim().ifEmpty { "Нет ответа от ИИ" }}"
            }
        }
        root.addView(aiButton)
        root.addView(aiStatusView)

        val markFakeBtn = GgtgUi.button(context, if (analysis.isMarkedFake) "✓ Помечен вами как фейк" else "Пометить: «Это фейк / бот»") {
            markAsFake(user.id, user.username)
            Toast.makeText(context, "Профиль добавлен в чёрный список фейков", Toast.LENGTH_SHORT).show()
            onUpdated?.run()
        }
        root.addView(GgtgUi.spacer(context, 10f))
        root.addView(markFakeBtn)

        val markAliveBtn = GgtgUi.button(context, if (analysis.isMarkedAlive) "✓ Помечен вами как живой" else "Пометить: «Живой человек»") {
            markAsAlive(user.id, user.username)
            Toast.makeText(context, "Профиль добавлен в белый список проверенных", Toast.LENGTH_SHORT).show()
            onUpdated?.run()
        }
        root.addView(GgtgUi.spacer(context, 8f))
        root.addView(markAliveBtn)

        if (analysis.isMarkedFake || analysis.isMarkedAlive) {
            val clearBtn = GgtgUi.button(context, "Сбросить мою метку") {
                clearMark(user.id, user.username)
                Toast.makeText(context, "Метка сброшена", Toast.LENGTH_SHORT).show()
                onUpdated?.run()
            }
            root.addView(GgtgUi.spacer(context, 8f))
            root.addView(clearBtn)
        }

        val trainBtn = GgtgUi.button(context, "Обучить ИИ на помеченных") {
            trainFromMarked(context) { onUpdated?.run() }
        }
        root.addView(GgtgUi.spacer(context, 8f))
        root.addView(trainBtn)

        val scroll = ScrollView(context).apply { addView(root) }

        AlertDialog.Builder(context)
            .setTitle("Анализ профиля: @${user.username ?: user.id}")
            .setView(scroll)
            .setPositiveButton("Закрыть", null)
            .show()
    }

    @JvmStatic
    fun showCleanBaseDialog(
        context: Context,
        baseId: Long,
        title: String,
        onDone: Runnable?
    ) {
        val usernames = GgtgDb.loadUsernames(baseId)
        if (usernames.isEmpty()) {
            Toast.makeText(context, "В этой базе нет пользователей", Toast.LENGTH_SHORT).show()
            return
        }

        val root = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            val pad = AndroidUtilities.dp(16f)
            setPadding(pad, pad, pad, pad)
        }

        var currentThreshold = 60
        var currentFiltered = filterUsernames(usernames, currentThreshold)

        val statsView = TextView(context).apply {
            textSize = 14f
            setTextColor(Theme.getColor(Theme.key_windowBackgroundWhiteBlackText))
            text = "Всего в базе: ${usernames.size}\nПодозрительных / фейков: ${currentFiltered.second.size}\nОстанется живых: ${currentFiltered.first.size}"
            setPadding(0, 0, 0, AndroidUtilities.dp(14f))
        }
        root.addView(statsView)

        val radioGroup = RadioGroup(context)
        val r80 = RadioButton(context).apply { text = "Мягкий (только явные боты >80%)"; id = 80 }
        val r60 = RadioButton(context).apply { text = "Стандартный (>60%, рекомендуется)"; id = 60; isChecked = true }
        val r40 = RadioButton(context).apply { text = "Строгий (удалить подозрительных >40%)"; id = 40 }
        radioGroup.addView(r80)
        radioGroup.addView(r60)
        radioGroup.addView(r40)

        radioGroup.setOnCheckedChangeListener { _, checkedId ->
            currentThreshold = checkedId
            currentFiltered = filterUsernames(usernames, currentThreshold)
            statsView.text = "Всего в базе: ${usernames.size}\nПодозрительных / фейков: ${currentFiltered.second.size}\nОстанется живых: ${currentFiltered.first.size}"
        }
        root.addView(radioGroup)

        val trainAiBtn = GgtgUi.button(context, "Обучить ИИ на помеченных") {
            trainFromMarked(context) {
                currentFiltered = filterUsernames(usernames, currentThreshold)
                statsView.text = "Всего в базе: ${usernames.size}\nПодозрительных / фейков: ${currentFiltered.second.size}\nОстанется живых: ${currentFiltered.first.size}"
            }
        }
        root.addView(GgtgUi.spacer(context, 12f))
        root.addView(trainAiBtn)

        AlertDialog.Builder(context)
            .setTitle("Очистка от фейков: $title")
            .setView(root)
            .setPositiveButton("Отделить в список фейков") { _, _ ->
                if (currentFiltered.second.isEmpty()) {
                    Toast.makeText(context, "Фейков при данном пороге не найдено", Toast.LENGTH_SHORT).show()
                    return@setPositiveButton
                }
                val fakeTitle = "$title - ФЕЙКИ"
                val fakeId = GgtgDb.generateManualBaseId()
                GgtgDb.saveUsernames(fakeId, fakeTitle, currentFiltered.second)
                GgtgDb.saveUsernames(baseId, title, currentFiltered.first)
                Toast.makeText(context, "Создана база «$fakeTitle» (${currentFiltered.second.size} чел.). В текущей осталось: ${currentFiltered.first.size}", Toast.LENGTH_LONG).show()
                onDone?.run()
            }
            .setNeutralButton("Удалить") { _, _ ->
                if (currentFiltered.second.isEmpty()) {
                    Toast.makeText(context, "Фейков при данном пороге не найдено", Toast.LENGTH_SHORT).show()
                    return@setNeutralButton
                }
                GgtgDb.saveUsernames(baseId, title, currentFiltered.first)
                Toast.makeText(context, "Удалено фейков: ${currentFiltered.second.size}. Осталось: ${currentFiltered.first.size}", Toast.LENGTH_LONG).show()
                onDone?.run()
            }
            .setNegativeButton("Отмена", null)
            .show()
    }
}

