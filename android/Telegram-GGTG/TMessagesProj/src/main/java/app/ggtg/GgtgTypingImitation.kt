package app.ggtg

import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import org.telegram.messenger.MessagesController
import org.telegram.messenger.R
import org.telegram.ui.ActionBar.BaseFragment
import org.telegram.ui.Components.Bulletin
import org.telegram.ui.Components.BulletinFactory
import java.util.concurrent.ConcurrentHashMap

// GGTG: Background typing imitation manager for Android — full parity with macOS Desktop
object GgtgTypingImitation {

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main)
    private val activeTasks = ConcurrentHashMap<Long, TypingTask>()

    private class TypingTask(
        val dialogId: Long,
        val account: Int,
        val threadMsgId: Long,
        val fullText: String,
        val totalMs: Long,
        var job: Job?,
        var bulletin: Bulletin?,
        var onRestore: ((String) -> Unit)?,
        val onSend: () -> Unit
    )

    fun isTyping(dialogId: Long): Boolean {
        return activeTasks.containsKey(dialogId)
    }

    fun start(
        account: Int,
        dialogId: Long,
        threadMsgId: Long,
        text: String,
        fragment: BaseFragment?,
        onRestore: ((String) -> Unit)?,
        onSend: () -> Unit
    ) {
        cancel(dialogId, false)
        if (text.isEmpty()) return

        val totalChars = text.length
        val totalMs = (totalChars * 75L + (0..400).random()).coerceIn(2000L, 25000L)
        val sec = (totalMs + 999L) / 1000L

        val task = TypingTask(
            dialogId = dialogId,
            account = account,
            threadMsgId = threadMsgId,
            fullText = text,
            totalMs = totalMs,
            job = null,
            bulletin = null,
            onRestore = onRestore,
            onSend = onSend
        )

        // Show official Telegram floating Bulletin with cancel button
        if (fragment != null) {
            val bulletinText = "Имитация печати... (отправка через $sec с)"
            task.bulletin = BulletinFactory.of(fragment).createSimpleBulletin(
                R.drawable.msg_edit,
                bulletinText,
                "Отменить"
            ) {
                cancel(dialogId, true)
            }
            task.bulletin?.show()
        }

        // Initial MTProto typing action
        MessagesController.getInstance(account).sendTyping(dialogId, threadMsgId, 0, 0)

        task.job = scope.launch {
            var lastTypingSent = System.currentTimeMillis()
            var remainingMs = totalMs
            while (isActive && remainingMs > 0) {
                val delayChunk = remainingMs.coerceAtMost(500L)
                delay(delayChunk)
                remainingMs -= delayChunk

                // Keep typing indicator active every ~3.5 seconds
                val now = System.currentTimeMillis()
                if (now - lastTypingSent >= 3500L) {
                    lastTypingSent = now
                    MessagesController.getInstance(account).sendTyping(dialogId, threadMsgId, 0, 0)
                }
            }

            if (isActive) {
                finish(dialogId)
            }
        }

        activeTasks[dialogId] = task
    }

    fun cancel(dialogId: Long, restoreText: Boolean = true) {
        val task = activeTasks.remove(dialogId) ?: return
        task.job?.cancel()
        task.bulletin?.hide()

        // Cancel typing status on server
        MessagesController.getInstance(task.account).sendTyping(task.dialogId, task.threadMsgId, 2, 0)

        if (restoreText) {
            task.onRestore?.invoke(task.fullText)
        }
    }

    private fun finish(dialogId: Long) {
        val task = activeTasks.remove(dialogId) ?: return
        task.bulletin?.hide()

        // Trigger message send
        task.onSend()
    }
}
