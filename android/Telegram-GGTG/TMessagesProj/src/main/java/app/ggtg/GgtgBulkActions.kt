package app.ggtg

import android.content.Context
import android.content.Intent
import android.text.InputType
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.Toast
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.launch
import org.telegram.messenger.AndroidUtilities
import org.telegram.messenger.MessagesController
import org.telegram.messenger.R
import org.telegram.ui.ActionBar.AlertDialog
import org.telegram.ui.ActionBar.BaseFragment
import org.telegram.ui.ActionBar.BottomSheet
import org.telegram.ui.ActionBar.Theme
import org.telegram.ui.Components.AccountFolderLogic

// ng: "Массовое действие" of the account manager - the same chooser as GGTG Desktop's
// MakeBulkActionsChooserBox (settings/bulk_accounts_actions.cpp), opened for the ticked accounts
// or, on a long press, for that one account alone. Steps run one account at a time (GgtgBulk),
// the SpamBot check 2-3 at a time (GgtgSpamStatus); everything is logged to "Процессы".
class GgtgBulkActions(private val fragment: BaseFragment, private val onChanged: () -> Unit) {

    private class Item(val title: String, val icon: Int, val action: () -> Unit)

    private val context: Context? get() = fragment.parentActivity

    fun show(slots: List<Int>) {
        val context = context ?: return
        if (slots.isEmpty()) { toast("Отметьте хотя бы один аккаунт"); return }
        val single = slots.size == 1
        val items = ArrayList<Item>()
        if (single) {
            items += Item("Информация об аккаунте (i)", R.drawable.msg_info) {
                GgtgAccountInfo.show(context, slots[0], onChanged)
            }
            items += Item("Меню аккаунта (прокси, 2FA, выйти…)", R.drawable.msg_settings) {
                GgtgAccountMenus.showAccountMenu(context, slots[0], fragment, onChanged)
            }
        }
        if (GgtgProcess.running(GgtgProcess.Kind.Bulk) != null) {
            items += Item("Остановить текущее действие", R.drawable.msg_cancel) {
                GgtgBulk.cancelled = true; toast("Останавливаю после текущего аккаунта")
            }
        }
        // Профиль
        items += Item("Фото", R.drawable.msg_addphoto) { askPhoto(slots) }
        items += Item("Имя и фамилия", R.drawable.msg_edit) { askName(slots) }
        items += Item("@username", R.drawable.msg_mention) { askUsername(slots) }
        items += Item("Описание", R.drawable.msg_info) { askBio(slots) }
        // Управление
        items += Item("Переместить в папку", R.drawable.msg_folders) { askFolder(slots) }
        items += Item("Деактивировать другие сессии", R.drawable.msg_leave) { confirm(slots, GgtgBulk.Action.TerminateOtherSessions) }
        items += Item("Заменить прокси", R.drawable.msg_language) {
            GgtgAccountMenus.replaceProxy(context, "у ${slots.size} акк.", slots, onChanged)
        }
        items += Item("Проверить SpamBot", R.drawable.msg_block) {
            val started = GgtgSpamStatus.checkMany(slots)
            toast(if (started > 0) "Проверка SpamBot запущена: $started, по 2-3 аккаунта раз в 3-5 сек." else "Нет подключённых аккаунтов")
            onChanged()
        }
        items += Item("Авто-апелляция @SpamBot", R.drawable.msg_help) {
            val started = GgtgSpamStatus.autoAppealMany(slots)
            toast(if (started > 0) "Авто-апелляция @SpamBot запущена: $started акк. (ход процесса во вкладке «Процессы»)" else "Нет подключённых аккаунтов")
            onChanged()
        }
        items += Item("Прогрев аккаунтов (повышение траста)", R.drawable.msg_stats) {
            fragment.presentFragment(GgtgWarmupActivity(slots))
        }
        items += Item("Создать резервную сессию", R.drawable.msg_retry) { askBulkBackup(slots) }
        items += Item("Перейти на резервную сессию", R.drawable.msg_forward_replace) { askBulkSwap(slots) }
        items += Item("Смена номера (миграция)", R.drawable.msg_calls) {
            fragment.presentFragment(GgtgPhoneMigrationActivity(slots))
        }
        items += Item("Экспорт аккаунтов…", R.drawable.msg_forward) { fragment.presentFragment(GgtgTdataExportActivity(slots)) }
        items += Item("Удалить из приложения…", R.drawable.msg_delete) { askRemove(slots) }

        BottomSheet.Builder(context)
            .setTitle(if (single) GgtgAccounts.label(slots[0]) else "Действие для ${slots.size} акк.", true)
            .setItems(items.map { it.title as CharSequence }.toTypedArray(), items.map { it.icon }.toIntArray()) { _, which ->
                items[which].action()
            }.show()
    }

    // ---- dialogs ----

    private fun field(context: Context, hint: String, multiLine: Boolean = false) = EditText(context).apply {
        this.hint = hint
        if (multiLine) inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_MULTI_LINE else setSingleLine()
        setTextColor(Theme.getColor(Theme.key_dialogTextBlack))
        setHintTextColor(Theme.getColor(Theme.key_dialogTextHint))
    }

    private fun box(context: Context, vararg views: EditText) = LinearLayout(context).apply {
        orientation = LinearLayout.VERTICAL
        setPadding(AndroidUtilities.dp(20f), AndroidUtilities.dp(8f), AndroidUtilities.dp(20f), 0)
        views.forEach { addView(it) }
    }

    private fun askName(slots: List<Int>) {
        val context = context ?: return
        val first = field(context, "Имя")
        val last = field(context, "Фамилия")
        if (slots.size == 1) GgtgAccounts.user(slots[0])?.let { first.setText(it.first_name.orEmpty()); last.setText(it.last_name.orEmpty()) }
        AlertDialog.Builder(context).setTitle("Имя и фамилия" + forWhom(slots)).setView(box(context, first, last))
            .setPositiveButton("Начать") { _, _ ->
                val f = first.text.toString().trim()
                if (f.isEmpty()) { toast("Введите имя"); return@setPositiveButton }
                launch(slots, GgtgBulk.Action.SetName(f, last.text.toString().trim()))
            }.setNegativeButton("Отмена", null).show()
    }

    private fun askBio(slots: List<Int>) {
        val context = context ?: return
        val text = field(context, "Описание (пусто — удалить)", multiLine = true)
        AlertDialog.Builder(context).setTitle("Описание" + forWhom(slots)).setView(box(context, text))
            .setPositiveButton("Начать") { _, _ -> launch(slots, GgtgBulk.Action.SetBio(text.text.toString())) }
            .setNegativeButton("Отмена", null).show()
    }

    private fun askUsername(slots: List<Int>) {
        val context = context ?: return
        val labels = arrayOf("Сгенерировать автоматически", "Удалить юзернейм")
        org.telegram.ui.ActionBar.AlertDialog.Builder(context).setTitle("@username" + forWhom(slots))
            .setItems(labels) { _, which ->
                confirm(slots, if (which == 0) GgtgBulk.Action.SetUsernameAuto() else GgtgBulk.Action.RemoveUsername)
            }.show()
    }

    private fun askFolder(slots: List<Int>) {
        val context = context ?: return
        val existing = AccountFolderLogic.existingFolderNames()
        val labels = ArrayList<CharSequence>(existing).apply { add("Новая папка…"); add("Убрать из папок") }
        org.telegram.ui.ActionBar.AlertDialog.Builder(context).setTitle("Переместить (${slots.size})")
            .setItems(labels.toTypedArray()) { _, which ->
                when {
                    which < existing.size -> moveTo(slots, existing[which])
                    which == existing.size -> {
                        val name = field(context, "Имя папки")
                        AlertDialog.Builder(context).setTitle("Новая папка").setView(box(context, name))
                            .setPositiveButton("Готово") { _, _ ->
                                name.text.toString().trim().takeIf { it.isNotEmpty() }?.let { moveTo(slots, it) }
                            }.setNegativeButton("Отмена", null).show()
                    }
                    else -> moveTo(slots, "")
                }
            }.show()
    }

    // Moving is local and instant: no pauses, no process.
    private fun moveTo(slots: List<Int>, folder: String) {
        for (slot in slots) {
            val uc = org.telegram.messenger.UserConfig.getInstance(slot)
            uc.ncgFolder = folder
            uc.saveConfig(false)
        }
        toast("Перемещено аккаунтов: ${slots.size}")
        onChanged()
    }

    private fun askRemove(slots: List<Int>) {
        val context = context ?: return
        val active = org.telegram.messenger.UserConfig.selectedAccount
        val removable = slots.filter { it != active }
        if (removable.isEmpty()) { toast("Текущий аккаунт удалить нельзя — сначала переключитесь на другой"); return }
        if (GgtgProcess.running(GgtgProcess.Kind.Bulk) != null || GgtgProcess.running(GgtgProcess.Kind.Sms) != null) {
            toast("Идёт массовое действие или рассылка — дождитесь окончания"); return
        }
        AlertDialog.Builder(context).setTitle("Удалить из приложения")
            .setMessage("Удалить ${removable.size} аккаунт(ов) из приложения? Сессии на сервере Telegram останутся." +
                if (removable.size < slots.size) "\n\nТекущий аккаунт пропущен." else "")
            .setPositiveButton("Удалить") { _, _ ->
                removable.forEach { MessagesController.getInstance(it).performLogout(0) }
                toast("Удалено: ${removable.size}")
                onChanged()
            }.setNegativeButton("Отмена", null).show()
    }

    private fun confirm(slots: List<Int>, action: GgtgBulk.Action) {
        val context = context ?: return
        AlertDialog.Builder(context).setTitle(action.title)
            .setMessage(if (slots.size == 1) "Применить к ${GgtgAccounts.label(slots[0])}?" else "Применить к ${slots.size} аккаунтам?")
            .setPositiveButton("Начать") { _, _ -> launch(slots, action) }
            .setNegativeButton("Отмена", null).show()
    }

    private fun forWhom(slots: List<Int>) = if (slots.size == 1) "" else " — для ${slots.size}"

    // ---- photo ----

    private var photoSlots: List<Int> = emptyList()
    private var photoReplace = false

    private fun askPhoto(slots: List<Int>) {
        val context = context ?: return
        org.telegram.ui.ActionBar.AlertDialog.Builder(context).setTitle("Фото" + forWhom(slots))
            .setItems(arrayOf("Просто добавить новое", "Удалить старое и добавить новое")) { _, which ->
                photoReplace = which == 1
                photoSlots = slots
                val intent = Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
                    addCategory(Intent.CATEGORY_OPENABLE)
                    type = "image/*"
                    putExtra(Intent.EXTRA_ALLOW_MULTIPLE, slots.size > 1)
                }
                fragment.startActivityForResult(intent, PHOTO_REQUEST)
            }.show()
    }

    /** Call from the fragment's onActivityResultFragment; true when the result was ours. */
    fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?): Boolean {
        if (requestCode != PHOTO_REQUEST) return false
        val context = context ?: return true
        if (resultCode != android.app.Activity.RESULT_OK || data == null) return true
        val uris = ArrayList<android.net.Uri>()
        data.data?.let { uris.add(it) }
        data.clipData?.let { clip -> for (i in 0 until clip.itemCount) uris.add(clip.getItemAt(i).uri) }
        val files = uris.distinct().mapIndexedNotNull { i, uri ->
            try {
                val f = java.io.File(context.cacheDir, "ggtg_pick_${System.nanoTime()}_$i")
                context.contentResolver.openInputStream(uri)?.use { input -> f.outputStream().use { input.copyTo(it) } }
                f.absolutePath
            } catch (t: Throwable) { null }
        }
        val slots = photoSlots
        if (files.isEmpty() || slots.isEmpty()) { toast("Файлы не выбраны"); return true }
        if (slots.size == 1) { launch(slots, GgtgBulk.Action.SetPhoto(files, photoReplace)); return true }
        org.telegram.ui.ActionBar.AlertDialog.Builder(context).setTitle("Фото — массово")
            .setMessage("Выбрано файлов: ${files.size}. Они будут розданы по очереди ${slots.size} аккаунтам (например, 3 фото на 5 аккаунтов: 1-2-3-1-2).")
            .setPositiveButton("Начать") { _, _ -> launch(slots, GgtgBulk.Action.SetPhoto(files, photoReplace)) }
            .setNegativeButton("Отмена", null).show()
        return true
    }

    private fun askBulkBackup(slots: List<Int>) {
        val context = context ?: return
        val activity = fragment.parentActivity as? androidx.fragment.app.FragmentActivity ?: return
        AlertDialog.Builder(context)
            .setTitle("Создать резервную сессию")
            .setMessage("Для каждого выбранного аккаунта будет создана отдельная независимая резервная сессия (новый вход с прокси) и сохранена в хранилище программы.\n\nОна не запускается, пока не понадобится: если рабочая сессия погибнет, аккаунт можно восстановить из резервной.\n\nАккаунты обрабатываются по одному.")
            .setPositiveButton("Создать") { _, _ ->
                val progress = app.ggtg.ui.TdataImportProgressDialog(activity, "Резервные сессии")
                progress.setWorkingText("Подготовка…")
                CoroutineScope(Dispatchers.Main + SupervisorJob()).launch {
                    var successCount = 0
                    val errors = ArrayList<String>()
                    for ((i, slot) in slots.withIndex()) {
                        val label = GgtgAccounts.label(slot)
                        progress.setWorkingText("Создание резервной сессии для $label (${i + 1} из ${slots.size})…")
                        val completed = CompletableDeferred<Result<GgtgSessions.StoredSession>>()
                        GgtgSessions.createBackupSession(activity, slot, progress) { res ->
                            completed.complete(res)
                        }
                        val res = completed.await()
                        res.onSuccess { successCount++ }.onFailure { errors.add("$label: ${it.message}") }
                    }
                    progress.dismiss()
                    val summary = StringBuilder("Создано резервных сессий: $successCount из ${slots.size}.")
                    if (errors.isNotEmpty()) summary.append("\nОшибки:\n").append(errors.joinToString("\n"))
                    GgtgApiOverride.inform(context, "Резервные сессии", summary.toString())
                    onChanged()
                }
            }
            .setNegativeButton("Отмена", null)
            .show()
    }

    private fun askBulkSwap(slots: List<Int>) {
        val context = context ?: return
        AlertDialog.Builder(context)
            .setTitle("Перейти на резервную сессию")
            .setMessage("Выбранные аккаунты переключатся на сохранённые резервные сессии. Текущие сессии сохранятся в хранилище как резервные.\n\nАккаунты без резервной сессии будут пропущены.")
            .setPositiveButton("Переключить") { _, _ ->
                CoroutineScope(Dispatchers.Main + SupervisorJob()).launch {
                    var swapped = 0
                    val errors = ArrayList<String>()
                    for (slot in slots) {
                        val label = GgtgAccounts.label(slot)
                        val res = GgtgSessions.swapToBackup(slot)
                        res.onSuccess { swapped++ }.onFailure { errors.add("$label: ${it.message}") }
                    }
                    val summary = StringBuilder("Переключено аккаунтов: $swapped из ${slots.size}.")
                    if (errors.isNotEmpty()) summary.append("\nОшибки:\n").append(errors.joinToString("\n"))
                    GgtgApiOverride.inform(context, "Переключение сессий", summary.toString())
                    onChanged()
                }
            }
            .setNegativeButton("Отмена", null)
            .show()
    }

    private fun launch(slots: List<Int>, action: GgtgBulk.Action) {
        if (GgtgProcess.running(GgtgProcess.Kind.Bulk) != null) { toast("Уже идёт массовое действие — дождитесь его в «Процессах»"); return }
        toast(if (slots.size == 1) "Запущено" else "Запущено — смотрите «Процессы»")
        CoroutineScope(SupervisorJob() + Dispatchers.IO).launch {
            GgtgBulk.run(slots, action)
            AndroidUtilities.runOnUIThread { onChanged() }
        }
    }

    private fun toast(text: String) { context?.let { Toast.makeText(it, text, Toast.LENGTH_LONG).show() } }

    private companion object { const val PHOTO_REQUEST = 4712 }
}
