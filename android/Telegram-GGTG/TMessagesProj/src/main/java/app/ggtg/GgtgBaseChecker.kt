package app.ggtg

import android.content.Context
import android.widget.LinearLayout
import android.widget.ProgressBar
import android.widget.TextView
import android.widget.Toast
import kotlinx.coroutines.*
import org.telegram.messenger.*
import org.telegram.tgnet.ConnectionsManager
import org.telegram.tgnet.TLRPC
import org.telegram.ui.ActionBar.AlertDialog
import org.telegram.ui.ActionBar.Theme

// ng: Smart base checker that enriches usernames with name, id and last-seen metadata.
// Prioritizes Recycle Bin (trash) accounts to avoid flooding normal/active accounts.
// If no trash accounts exist, rotates across live accounts in small batches (3 queries each).
// Handles FLOOD_WAIT by rotating immediately to another worker without stalling the queue.
object GgtgBaseChecker {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main)
    private var checkJob: Job? = null

    /** Asks which profiles to load (missing ones / all again), with the counts, then runs [checkBase]. */
    @JvmStatic
    fun chooseAndCheck(context: Context, baseId: Long, title: String, onDone: (() -> Unit)? = null) {
        val usernames = GgtgDb.loadUsernames(baseId)
        if (usernames.isEmpty()) { Toast.makeText(context, "База пуста", Toast.LENGTH_SHORT).show(); return }
        val now = System.currentTimeMillis() / 1000
        var without = 0; var missing = 0; var unknownAge = 0; var stale = 0; var fresh = 0
        for (u in usernames) {
            val m = GgtgDb.lookupUser(u)
            when {
                m?.missing == true -> missing++
                m == null || m.id == 0L -> without++
                m.loadedAt == 0L -> unknownAge++
                now - m.loadedAt > 3 * 24 * 3600 -> stale++
                else -> fresh++
            }
        }
        val msg = "Для каждого человека загружается профиль: имя, статус «был(а) в сети», бот или нет. Загруженный профиль со временем устаревает.\n\n" +
            "Всего в базе: ${usernames.size}\nПрофиль свежий (до 3 дней): $fresh\nПрофиль старше 3 дней: $stale\n" +
            "Профиль загружен давно, время неизвестно: $unknownAge\nПрофиля нет: $without\nTelegram ответил «такого юзернейма нет»: $missing"
        val b = org.telegram.ui.ActionBar.AlertDialog.Builder(context)
            .setTitle("Загрузить профили: $title")
            .setMessage(msg)
            .setNegativeButton("Отмена", null)
            .setNeutralButton("Обновить всех (${usernames.size})") { _, _ -> checkBase(context, baseId, title, onDone, refreshAll = true) }
        if (without > 0) b.setPositiveButton("Недостающие ($without)") { _, _ -> checkBase(context, baseId, title, onDone) }
        b.show()
    }

    fun checkBase(
        context: Context,
        baseId: Long,
        title: String,
        onDone: (() -> Unit)? = null,
        refreshAll: Boolean = false
    ) {
        val usernames = GgtgDb.loadUsernames(baseId)
        if (usernames.isEmpty()) {
            Toast.makeText(context, "База пуста", Toast.LENGTH_SHORT).show()
            return
        }
        val toCheck = usernames.filter { u ->
            val meta = GgtgDb.lookupUser(u)
            refreshAll || ((meta == null || meta.id == 0L) && meta?.missing != true)
        }
        if (toCheck.isEmpty()) {
            Toast.makeText(context, "У всех есть профиль или Telegram ответил, что такого юзернейма нет. Чтобы обновить всех, выберите «Обновить все»", Toast.LENGTH_LONG).show()
            onDone?.invoke()
            return
        }

        // Determine worker pool: trash accounts prioritized
        val trashSlots = (0 until UserConfig.MAX_ACCOUNT_COUNT).filter {
            val uc = UserConfig.getInstance(it)
            uc.isClientActivated && uc.ncgTrashed
        }
        val liveSlots = (0 until UserConfig.MAX_ACCOUNT_COUNT).filter {
            val uc = UserConfig.getInstance(it)
            uc.isClientActivated && !uc.ncgTrashed
        }
        val usingTrash = trashSlots.isNotEmpty()
        val workers = if (usingTrash) trashSlots else liveSlots
        if (workers.isEmpty()) {
            Toast.makeText(context, "Нет подключённых аккаунтов", Toast.LENGTH_SHORT).show()
            return
        }

        if (usingTrash) {
            GgtgAccounts.wake(trashSlots)
        }

        val pad = AndroidUtilities.dp(16f)
        val root = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(pad, pad, pad, pad)
        }
        val modeTv = TextView(context).apply {
            textSize = 13f
            setTextColor(Theme.getColor(Theme.key_windowBackgroundWhiteGrayText))
            text = if (usingTrash) {
                "Используются аккаунты из корзины (${workers.size} шт.) — рабочие аккаунты защищены от флуда."
            } else {
                "Используется ротация рабочих аккаунтов (${workers.size} шт.) — по 3 запроса на аккаунт. «В фоне» — окно закроется, проверка продолжится."
            }
            setPadding(0, 0, 0, AndroidUtilities.dp(8f))
        }
        root.addView(modeTv)

        val progress = ProgressBar(context, null, android.R.attr.progressBarStyleHorizontal).apply {
            max = toCheck.size
            this.progress = 0
            isIndeterminate = false
        }
        root.addView(progress, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT))

        val statusTv = TextView(context).apply {
            textSize = 14f
            setTextColor(Theme.getColor(Theme.key_windowBackgroundWhiteBlackText))
            text = "Проверено: 0 из ${toCheck.size}"
            setPadding(0, AndroidUtilities.dp(8f), 0, 0)
        }
        root.addView(statusTv)

        val currentWorkerTv = TextView(context).apply {
            textSize = 12f
            setTextColor(Theme.getColor(Theme.key_windowBackgroundWhiteGrayText))
            text = "Аккаунт: ${GgtgAccounts.label(workers.first())}"
            setPadding(0, AndroidUtilities.dp(4f), 0, 0)
        }
        root.addView(currentWorkerTv)

        var dialog: AlertDialog? = null
        dialog = AlertDialog.Builder(context)
            .setTitle("Проверка базы: $title")
            .setView(root)
            .setNegativeButton("Остановить") { d, _ ->
                checkJob?.cancel()
                d.dismiss()
            }
            .setNeutralButton("В фоне") { d, _ -> d.dismiss() }
            .create()
        dialog.show()

        checkJob = scope.launch(Dispatchers.IO) {
            var checkedCount = 0
            var resolvedCount = 0
            var workerIdx = 0
            var batchCount = 0
            var maxBatch = if (usingTrash) 15 + kotlin.random.Random.nextInt(16) else 3
            fun newBatch() { maxBatch = if (usingTrash) 15 + kotlin.random.Random.nextInt(16) else 3 }
            val cooldownUntil = HashMap<Int, Long>()

            try {
                for (username in toCheck) {
                    if (!isActive) break

                    // the same person is retried on another account when one runs into its limit - a flood answer
                    // used to be taken for "checked" and the person was silently dropped
                    var settled = false
                    var tries = 0
                    while (!settled && isActive && tries++ < 60) {
                        val now = System.currentTimeMillis()
                        var attempts = workers.size
                        while (attempts-- > 0) {
                            if (workerIdx >= workers.size) workerIdx = 0
                            val slot = workers[workerIdx]
                            val cool = cooldownUntil[slot] ?: 0L
                            if (now >= cool) break
                            workerIdx = (workerIdx + 1) % workers.size
                        }
                        val chosenSlot = workers[workerIdx]
                        val cool = cooldownUntil[chosenSlot] ?: 0L
                        if (cool > now) {
                            // every account is resting: say so, then wait for the nearest one
                            val soonest = workers.minOf { cooldownUntil[it] ?: 0L }
                            withContext(Dispatchers.Main) {
                                if (dialog?.isShowing == true) currentWorkerTv.text = "Все аккаунты на паузе (лимит Telegram). Продолжу через ${((soonest - now) / 1000).coerceAtLeast(1)} с"
                            }
                            delay(minOf(maxOf(soonest - now, 1000L), 15000L))
                            continue
                        }

                        withContext(Dispatchers.Main) {
                            if (dialog?.isShowing == true) currentWorkerTv.text = "Аккаунт: ${GgtgAccounts.label(chosenSlot)}"
                        }

                        try {
                            val req = TLRPC.TL_contacts_resolveUsername().apply { this.username = username }
                            val pair = GgtgAccounts.request(chosenSlot, req, ConnectionsManager.RequestFlagFailOnServerErrors)
                            val err = pair?.second
                            val errText = err?.text.orEmpty()
                            if (errText.startsWith("FLOOD_WAIT")) {
                                // the error comes back as an answer, not as an exception: this account rests for as long
                                // as Telegram says (not a fixed minute) and the person goes to the next account
                                val seconds = Regex("\\d+").find(errText)?.value?.toLongOrNull() ?: 60L
                                cooldownUntil[chosenSlot] = System.currentTimeMillis() + seconds.coerceAtMost(6 * 3600L) * 1000L
                                batchCount = 0
                                newBatch()
                                workerIdx = (workerIdx + 1) % workers.size
                                delay(GgtgRegistration.jitter(600))
                                continue
                            }
                            if (errText == "USERNAME_NOT_OCCUPIED" || errText == "USERNAME_INVALID") GgtgDb.rememberMissing(username)
                            val res = pair?.first as? TLRPC.TL_contacts_resolvedPeer
                            if (res != null) {
                                val user = res.users.firstOrNull()
                                if (user != null) {
                                    GgtgDb.rememberUser(user)
                                    resolvedCount++
                                }
                            }
                            settled = true
                            batchCount++
                            if (batchCount >= maxBatch) {
                                batchCount = 0
                                newBatch()
                                workerIdx = (workerIdx + 1) % workers.size
                            }
                        } catch (t: Throwable) {
                            val msg = t.message ?: ""
                            if (msg.contains("FLOOD_WAIT", ignoreCase = true)) {
                                val seconds = Regex("\\d+").find(msg)?.value?.toLongOrNull() ?: 60L
                                cooldownUntil[chosenSlot] = System.currentTimeMillis() + seconds.coerceAtMost(6 * 3600L) * 1000L
                                batchCount = 0
                                newBatch()
                                workerIdx = (workerIdx + 1) % workers.size
                            } else {
                                settled = true
                                batchCount++
                                if (batchCount >= maxBatch) {
                                    batchCount = 0
                                newBatch()
                                    workerIdx = (workerIdx + 1) % workers.size
                                }
                            }
                        }
                    }

                    checkedCount++
                    withContext(Dispatchers.Main) {
                        progress.progress = checkedCount
                        statusTv.text = "Проверено: $checkedCount из ${toCheck.size}\nПрофилей загружено: $resolvedCount\nОсталось: ${toCheck.size - checkedCount}"
                    }
                    delay(GgtgRegistration.jitter(450))
                }
            } finally {
                if (usingTrash) {
                    GgtgAccounts.sleepIdle()
                }
                withContext(Dispatchers.Main) {
                    dialog?.dismiss()
                    Toast.makeText(context, "Проверка завершена. Определено пользователей: $resolvedCount", Toast.LENGTH_SHORT).show()
                    onDone?.invoke()
                }
            }
        }
    }
}
