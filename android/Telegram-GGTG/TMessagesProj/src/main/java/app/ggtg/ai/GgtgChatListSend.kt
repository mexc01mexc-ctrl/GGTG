package app.ggtg.ai

import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.Handler
import android.os.Looper
import android.provider.OpenableColumns
import android.view.View
import android.widget.LinearLayout
import app.ggtg.GgtgAccounts
import app.ggtg.GgtgProcess
import app.ggtg.GgtgSms
import app.ggtg.GgtgUi
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import org.telegram.messenger.AccountInstance
import org.telegram.messenger.AndroidUtilities
import org.telegram.messenger.MessagesController
import org.telegram.messenger.R
import org.telegram.messenger.SendMessagesHelper
import org.telegram.messenger.UserConfig
import org.telegram.ui.ActionBar.BaseFragment
import java.io.File
import kotlin.random.Random

// ggtg: "Написать всему списку": one message (a text, or a photo / file with a caption) to every chat of a
// list of chats, each from the account the chat is in, with a random pause between the chats. It is
// a process of "Процессы". In the text, [NAME] (or [NAME|друг]) is the person's first name and
// {привет|здравствуйте} is one of the variants, chosen anew for each person.
object GgtgChatListSend {

    class Params(val listId: String, val text: String, val file: String?, val mime: String, val pauseMin: Int, val pauseMax: Int)

    @Volatile var running = false
        private set
    @Volatile var cancelled = false
    @Volatile var handle: GgtgProcess.Handle? = null
        private set
    @Volatile var sent = 0
        private set
    @Volatile var failed = 0
        private set
    @Volatile var total = 0
        private set
    @Volatile var status = ""
        private set

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)

    /** Copies the chosen file into the cache (the picker's address does not live long); returns its path and type. */
    fun copyPicked(context: Context, uri: Uri): Pair<String, String>? = try {
        var name = "file"
        context.contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use { if (it.moveToFirst()) name = it.getString(0) }
        val out = File(context.cacheDir, "ggtg_list_" + System.currentTimeMillis() + "_" + name.replace(Regex("[^A-Za-z0-9._-]"), "_"))
        context.contentResolver.openInputStream(uri)?.use { input -> out.outputStream().use { input.copyTo(it) } }
        out.absolutePath to (context.contentResolver.getType(uri) ?: "")
    } catch (t: Throwable) { null }

    private fun sendOne(params: Params, entry: GgtgChatLists.Entry): String? {
        val slot = GgtgAiText.slotOf(entry.accountUserId)
        if (slot < 0) return "аккаунт недоступен"
        val user = if (entry.dialogId > 0) MessagesController.getInstance(slot).getUser(entry.dialogId) else null
        val me = UserConfig.getInstance(slot).currentUser
        val text = GgtgSms.render(params.text, user?.first_name.orEmpty(), user?.last_name.orEmpty(), me?.first_name.orEmpty(), me?.last_name.orEmpty()).trim()
        if (params.file == null && text.isEmpty()) return "пустой текст"
        AndroidUtilities.runOnUIThread {
            if (params.file == null) {
                SendMessagesHelper.getInstance(slot).sendMessage(SendMessagesHelper.SendMessageParams.of(text, entry.dialogId))
            } else if (params.mime.startsWith("image/")) {
                SendMessagesHelper.prepareSendingPhoto(AccountInstance.getInstance(slot), params.file, null, entry.dialogId,
                    null, null, null, text, null, null, null, 0, null, true, 0, 0, null)
            } else {
                SendMessagesHelper.prepareSendingDocument(AccountInstance.getInstance(slot), params.file, params.file, null, text,
                    params.mime.ifEmpty { null }, entry.dialogId, null, null, null, null, null, true, 0, null, null, false)
            }
        }
        return null
    }

    /** Starts the run; returns an error text, null when it started. */
    fun start(params: Params): String? {
        if (running) return "Другая рассылка по списку уже идёт: дождитесь её или остановите."
        val list = GgtgChatLists.find(params.listId)
        if (list == null || list.entries.isEmpty()) return "В списке нет чатов."
        val entries = list.entries.filter { GgtgAiText.slotOf(it.accountUserId) >= 0 }.sortedBy { it.at }
        if (entries.isEmpty()) return "В списке нет чатов для доступных на этом устройстве аккаунтов."
        if (params.text.isBlank() && params.file == null) return "Напишите текст или выберите файл."
        running = true; cancelled = false; sent = 0; failed = 0; total = entries.size
        val h = GgtgProcess.start("Рассылка по списку «${list.name}»", GgtgProcess.Kind.Other)
        handle = h
        h.log("Получателей: ${entries.size}, пауза ${params.pauseMin}-${params.pauseMax} с, " +
            (if (params.file == null) "только текст" else "с файлом"))
        GgtgAccounts.wake(entries.map { GgtgAiText.slotOf(it.accountUserId) }.filter { it >= 0 }.toSet())
        scope.launch {
            try {
                for ((i, entry) in entries.withIndex()) {
                    if (cancelled) { h.succeed("Остановлено. Отправлено: $sent из ${entries.size}."); return@launch }
                    val who = entry.name.ifEmpty { "@" + entry.username }
                    val error = sendOne(params, entry)
                    if (error == null) { sent++; h.log("$who: отправлено") } else { failed++; h.log("$who: $error") }
                    h.progress(i + 1, entries.size)
                    if (i == entries.size - 1) break
                    val seconds = params.pauseMin + Random.nextInt(maxOf(params.pauseMax - params.pauseMin, 0) + 1)
                    status = "Пауза $seconds с перед следующим (${i + 2} из ${entries.size})"
                    var left = seconds * 1000L
                    while (left > 0 && !cancelled) { delay(500); left -= 500 }
                }
                delay(3000)
                h.succeed("Готово. Отправлено: $sent, не вышло: $failed.")
            } catch (t: Throwable) {
                h.fail("Ошибка: ${t.message}")
            } finally { running = false; status = "" }
        }
        return null
    }
}

// ggtg: the form of "Написать всем" and, once it runs, its progress with "Остановить".
class GgtgChatListSendActivity(private val listId: String) : BaseFragment() {

    private var column: LinearLayout? = null
    private var picked: Pair<String, String>? = null
    private var pickedName = ""
    private var text = ""
    private var pauseMin = "20"
    private var pauseMax = "40"
    private var message = ""
    private val handler = Handler(Looper.getMainLooper())
    private val tick = object : Runnable {
        override fun run() { if (GgtgChatListSend.running) rebuild(); handler.postDelayed(this, 1500) }
    }

    override fun createView(context: Context): View {
        val (frame, col) = GgtgAiUi.page(this, context, "Написать всему списку")
        column = col
        rebuild()
        return frame
    }

    override fun onResume() { super.onResume(); handler.postDelayed(tick, 1500) }
    override fun onPause() { super.onPause(); handler.removeCallbacks(tick) }

    private fun rebuild() {
        val col = column ?: return
        val context = col.context
        col.removeAllViews()
        val list = GgtgChatLists.find(listId)
        val count = list?.entries?.count { GgtgAiText.slotOf(it.accountUserId) >= 0 } ?: 0
        val head = GgtgUi.card(context)
        head.addView(GgtgUi.body(context, "Сообщение уйдёт каждому чату списка «${list?.name.orEmpty()}» ($count) из того аккаунта, в котором этот чат. " +
            "В тексте: [NAME] или [NAME|друг] - имя человека, {привет|здравствуйте} - случайный вариант для каждого.", 13f))
        GgtgUi.addCard(col, head, top = 12f)

        if (GgtgChatListSend.running) {
            val card = GgtgUi.card(context)
            card.addView(GgtgUi.title(context, "Рассылка идёт", 16f))
            card.addView(GgtgUi.body(context, "Отправлено ${GgtgChatListSend.sent} из ${GgtgChatListSend.total}, не вышло ${GgtgChatListSend.failed}.\n" +
                GgtgChatListSend.status, 13f), LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply { topMargin = GgtgUi.dp(4f) })
            GgtgUi.addCard(col, card)
            GgtgUi.addButton(col, GgtgUi.button(context, "Остановить", destructive = true) { GgtgChatListSend.cancelled = true; rebuild() })
            col.addView(GgtgAiUi.gap(context, 24f))
            return
        }

        val form = GgtgUi.card(context)
        val field = GgtgAiUi.field(context, "Текст или подпись к файлу", text, true, 5)
        field.addTextChangedListener(object : android.text.TextWatcher {
            override fun beforeTextChanged(p0: CharSequence?, p1: Int, p2: Int, p3: Int) {}
            override fun onTextChanged(p0: CharSequence?, p1: Int, p2: Int, p3: Int) {}
            override fun afterTextChanged(p0: android.text.Editable?) { text = p0?.toString().orEmpty() }
        })
        form.addView(field, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT))
        form.addView(GgtgUi.body(context, if (picked == null) "Файл не выбран (будет только текст)." else "Файл: $pickedName", 13f),
            LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply { topMargin = GgtgUi.dp(8f) })
        form.addView(GgtgAiUi.links(context,
            GgtgAiUi.link("Выбрать фото или файл…") {
                startActivityForResult(Intent(Intent.ACTION_OPEN_DOCUMENT).apply { addCategory(Intent.CATEGORY_OPENABLE); type = "*/*" }, PICK)
            },
            GgtgAiUi.link("Убрать файл") { picked = null; pickedName = ""; rebuild() }))
        GgtgUi.addCard(col, form)

        val pauses = GgtgUi.card(context)
        pauses.addView(GgtgUi.title(context, "Пауза между чатами, секунд (случайная от и до)", 14f))
        val from = GgtgUi.input(context, "От", pauseMin, numeric = true)
        val to = GgtgUi.input(context, "До", pauseMax, numeric = true)
        from.addTextChangedListener(object : android.text.TextWatcher {
            override fun beforeTextChanged(p0: CharSequence?, p1: Int, p2: Int, p3: Int) {}
            override fun onTextChanged(p0: CharSequence?, p1: Int, p2: Int, p3: Int) {}
            override fun afterTextChanged(p0: android.text.Editable?) { pauseMin = p0?.toString().orEmpty() }
        })
        to.addTextChangedListener(object : android.text.TextWatcher {
            override fun beforeTextChanged(p0: CharSequence?, p1: Int, p2: Int, p3: Int) {}
            override fun onTextChanged(p0: CharSequence?, p1: Int, p2: Int, p3: Int) {}
            override fun afterTextChanged(p0: android.text.Editable?) { pauseMax = p0?.toString().orEmpty() }
        })
        pauses.addView(from, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply { topMargin = GgtgUi.dp(8f) })
        pauses.addView(to, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply { topMargin = GgtgUi.dp(6f) })
        GgtgUi.addCard(col, pauses)

        if (message.isNotEmpty()) GgtgUi.addCard(col, GgtgUi.card(context).also { it.addView(GgtgUi.body(context, message, 13f)) })
        GgtgUi.addButton(col, GgtgUi.button(context, "Начать") {
            val min = pauseMin.trim().toIntOrNull() ?: 0
            val max = pauseMax.trim().toIntOrNull() ?: 0
            when {
                min < 3 || max < min -> { message = "Пауза: от 3 секунд, и «до» не меньше «от»."; rebuild() }
                picked != null && text.length > 1000 -> { message = "Подпись к файлу не длиннее 1000 символов."; rebuild() }
                else -> GgtgAiUi.confirm(context, "Отправить это сообщение $count чатам списка? Остановить рассылку можно в любой момент.", "Отправить") {
                    message = GgtgChatListSend.start(GgtgChatListSend.Params(listId, text.trim(), picked?.first, picked?.second.orEmpty(), min, max)) ?: ""
                    rebuild()
                }
            }
        })
        col.addView(GgtgAiUi.gap(context, 24f))
    }

    override fun onActivityResultFragment(requestCode: Int, resultCode: Int, data: Intent?) {
        if (requestCode != PICK || resultCode != android.app.Activity.RESULT_OK) return
        val uri = data?.data ?: return
        val context = context ?: return
        Thread {
            val copied = GgtgChatListSend.copyPicked(context, uri)
            AndroidUtilities.runOnUIThread {
                if (copied == null) message = "Файл не прочитался" else { picked = copied; pickedName = File(copied.first).name.substringAfter('_').substringAfter('_'); message = "" }
                rebuild()
            }
        }.start()
    }

    private companion object { const val PICK = 7312 }
}
