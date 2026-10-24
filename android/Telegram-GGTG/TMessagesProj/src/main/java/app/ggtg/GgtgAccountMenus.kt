package app.ggtg

import android.content.Context
import android.text.InputType
import android.widget.EditText
import android.widget.Toast
import androidx.fragment.app.FragmentActivity
import kotlinx.coroutines.MainScope
import kotlinx.coroutines.launch
import org.telegram.messenger.AndroidUtilities
import org.telegram.messenger.MessagesController
import org.telegram.messenger.R
import org.telegram.messenger.UserConfig
import org.telegram.tgnet.ConnectionsManager
import org.telegram.ui.ActionBar.AlertDialog
import org.telegram.ui.ActionBar.BottomSheet
import org.telegram.ui.ActionBar.BaseFragment
import org.telegram.ui.Components.AccountFolderLogic
import org.telegram.ui.Components.AccountFolderStore
import org.telegram.ui.Components.GgtgProxyChooser
import org.telegram.ui.LaunchActivity

// ng: the context menus GGTG Desktop shows on right click of an account / folder (account switcher
// and "Менеджер аккаунтов"), opened here on long press. One place, used by both screens, so the
// two behave the same. Mirrors settings_information.cpp (AccountsList context menus).
object GgtgAccountMenus {

    private class Entry(val title: String, val icon: Int, val action: () -> Unit)

    private fun show(context: Context, title: String, entries: List<Entry>) {
        BottomSheet.Builder(context)
            .setTitle(title, true)
            .setItems(
                entries.map { it.title as CharSequence }.toTypedArray(),
                entries.map { it.icon }.toIntArray()
            ) { _, which -> entries[which].action() }
            .show()
    }

    private fun toast(context: Context, text: String) = Toast.makeText(context, text, Toast.LENGTH_LONG).show()

    private fun activity(context: Context): FragmentActivity? =
        (context as? FragmentActivity) ?: LaunchActivity.instance

    // ---- account ----

    @JvmStatic
    fun showAccountMenu(context: Context, slot: Int, fragment: BaseFragment?, onChanged: () -> Unit) {
        val uc = UserConfig.getInstance(slot)
        val isActive = slot == UserConfig.selectedAccount
        val entries = ArrayList<Entry>()

        entries += Entry("Информация (i)", R.drawable.msg_info) { GgtgAccountInfo.show(context, slot, onChanged) }
        entries += Entry("Скопировать номер", R.drawable.msg_copy) {
            val phone = GgtgAccounts.user(slot)?.phone.orEmpty()
            if (phone.isEmpty()) toast(context, "Номер скрыт")
            else { AndroidUtilities.addToClipboard("+$phone"); toast(context, "Скопировано") }
        }
        if (!isActive) {
            entries += Entry("Переключиться", R.drawable.msg_contacts) {
                LaunchActivity.instance?.switchToAccount(slot, true)
                onChanged()
            }
        }
        entries += Entry(if (uc.ncgPinned) "Открепить" else "Закрепить", if (uc.ncgPinned) R.drawable.msg_unpin else R.drawable.msg_pin) {
            uc.ncgPinned = !uc.ncgPinned; uc.saveConfig(false); onChanged()
        }
        entries += Entry(if (uc.ncgMuted) "Включить звук" else "Отключить звук", if (uc.ncgMuted) R.drawable.msg_unmute else R.drawable.msg_mute) {
            uc.ncgMuted = !uc.ncgMuted; uc.saveConfig(false); onChanged()
        }
        entries += Entry(if (uc.ncgOffline) "Перевести в онлайн" else "Перевести в офлайн (сон)", if (uc.ncgOffline) R.drawable.msg_retry else R.drawable.msg_cancel) {
            uc.ncgOffline = !uc.ncgOffline
            uc.saveConfig(false)
            if (uc.ncgOffline) {
                ConnectionsManager.getInstance(slot).cleanup(false)
                toast(context, "Аккаунт переведён в офлайн (сон)")
            } else {
                ConnectionsManager.getInstance(slot).resumeNetworkMaybe()
                toast(context, "Аккаунт переведён в онлайн")
            }
            onChanged()
        }
        entries += Entry("Прокси", R.drawable.msg_language) { editProxy(context, slot, onChanged) }
        entries += Entry("Переподключить", R.drawable.msg_retry) {
            ConnectionsManager.getInstance(slot).checkConnection()
            toast(context, "Переподключаю…")
        }
        entries += Entry("Папка", R.drawable.msg_folders) {
            AccountFolderLogic.showFolderMenu(context, slot, false) { onChanged() }
        }
        entries += Entry("Пароль 2FA", R.drawable.msg_secret) { showTwoFactor(context, slot, onChanged) }
        entries += Entry("Создать резервную сессию", R.drawable.msg_retry) {
            val act = activity(context) ?: return@Entry
            val progress = app.ggtg.ui.TdataImportProgressDialog(act, "Резервная сессия")
            progress.setWorkingText("Создание резервной сессии…")
            GgtgSessions.createBackupSession(act, slot, progress) { res ->
                progress.dismiss()
                res.onSuccess {
                    toast(context, "Резервная сессия создана и сохранена в хранилище!")
                    onChanged()
                }.onFailure {
                    toast(context, "Ошибка: ${it.message}")
                }
            }
        }
        if (GgtgSessions.hasBackup(uc.clientUserId)) {
            entries += Entry("Перейти на резервную сессию", R.drawable.msg_forward_replace) {
                confirm(context, "Переключить ${GgtgAccounts.label(slot)} на резервную сессию? Текущая сессия сохранится как резервная.", "Переключить") {
                    kotlinx.coroutines.MainScope().launch {
                        val res = GgtgSessions.swapToBackup(slot)
                        res.onSuccess {
                            toast(context, "Аккаунт переключён на резервную сессию!")
                            onChanged()
                        }.onFailure {
                            toast(context, "Ошибка: ${it.message}")
                        }
                    }
                }
            }
        }
        entries += Entry("Экспорт аккаунта…", R.drawable.msg_forward) {
            (fragment ?: LaunchActivity.getLastFragment())?.presentFragment(GgtgTdataExportActivity(listOf(slot)))
        }
        if (!isActive) {
            entries += Entry("Выйти", R.drawable.msg_leave) {
                confirm(context, "Выйти из ${GgtgAccounts.label(slot)}? Сессия будет завершена на сервере Telegram.", "Выйти") {
                    MessagesController.getInstance(slot).performLogout(1); onChanged()
                }
            }
            entries += Entry("Удалить аккаунт…", R.drawable.msg_delete) {
                confirm(context, "Удалить ${GgtgAccounts.label(slot)} из приложения? Сессия на сервере Telegram останется.", "Удалить") {
                    MessagesController.getInstance(slot).performLogout(0); onChanged()
                }
            }
        }
        show(context, GgtgAccounts.label(slot), entries)
    }

    // ggtg: the official app has no per-account proxy screen (ProxySettingsActivity is app-wide),
    // so this account's own pinned proxy is edited inline here instead.
    fun editProxy(context: Context, slot: Int, onChanged: () -> Unit) {
        val uc = UserConfig.getInstance(slot)
        val field = EditText(context).apply {
            hint = "host:port:логин:пароль (пусто — убрать)"
            if (uc.ncgProxyEnabled) setText("${uc.ncgProxyAddress}:${uc.ncgProxyPort}:${uc.ncgProxyUsername}:${uc.ncgProxyPassword}")
            setPadding(AndroidUtilities.dp(24f), AndroidUtilities.dp(8f), AndroidUtilities.dp(24f), AndroidUtilities.dp(8f))
        }
        org.telegram.ui.ActionBar.AlertDialog.Builder(context)
            .setTitle("Прокси аккаунта")
            .setView(field)
            .setPositiveButton("Сохранить") { _, _ ->
                val text = field.text.toString().trim()
                if (text.isEmpty()) {
                    ConnectionsManager.setProxySettingsForAccount(slot, false, "", 0, "", "", "")
                    onChanged()
                    return@setPositiveButton
                }
                val parts = text.split(":")
                val port = parts.getOrNull(1)?.toIntOrNull()
                if (parts.isEmpty() || parts[0].isEmpty() || port == null) {
                    toast(context, "Формат: host:port:логин:пароль")
                    return@setPositiveButton
                }
                ConnectionsManager.setProxySettingsForAccount(slot, true, parts[0], port, parts.getOrElse(2) { "" }, parts.getOrElse(3) { "" }, "")
                GgtgDevice.autoGeoAfterProxy(slot)
                onChanged()
            }
            .setNegativeButton("Отмена", null)
            .show()
    }

    private fun showTwoFactor(context: Context, slot: Int, onChanged: () -> Unit) {
        val uc = UserConfig.getInstance(slot)
        val field = EditText(context).apply {
            hint = "Пароль 2FA (пусто — не сохранён)"
            inputType = InputType.TYPE_CLASS_TEXT
            setText(uc.ncgTwoFactor)
            setPadding(AndroidUtilities.dp(24f), AndroidUtilities.dp(8f), AndroidUtilities.dp(24f), AndroidUtilities.dp(8f))
        }
        org.telegram.ui.ActionBar.AlertDialog.Builder(context)
            .setTitle("Пароль 2FA")
            .setMessage(if (uc.ncgTwoFactor.isEmpty()) "Для этого аккаунта пароль 2FA не сохранён." else "Сохранённый пароль:")
            .setView(field)
            .setPositiveButton("Сохранить") { _, _ ->
                uc.ncgTwoFactor = field.text.toString(); uc.saveConfig(false); onChanged()
            }
            .setNeutralButton("Копировать") { _, _ ->
                if (uc.ncgTwoFactor.isNotEmpty()) { AndroidUtilities.addToClipboard(uc.ncgTwoFactor); toast(context, "Скопировано") }
            }
            .setNegativeButton("Закрыть", null)
            .show()
    }

    private fun confirm(context: Context, text: String, ok: String, onOk: () -> Unit) {
        org.telegram.ui.ActionBar.AlertDialog.Builder(context)
            .setMessage(text)
            .setPositiveButton(ok) { _, _ -> onOk() }
            .setNegativeButton("Отмена", null)
            .show()
    }

    // ---- folder ----

    private fun slotsOf(folder: String): List<Int> =
        (0 until UserConfig.MAX_ACCOUNT_COUNT).filter {
            UserConfig.getInstance(it).isClientActivated && UserConfig.getInstance(it).ncgFolder == folder
        }

    @JvmStatic
    fun showFolderMenu(context: Context, folder: String, onChanged: () -> Unit) {
        val slots = slotsOf(folder)
        val pinned = AccountFolderStore.isPinned(folder)
        val entries = ArrayList<Entry>()
        entries += Entry(if (pinned) "Открепить папку" else "Закрепить папку", if (pinned) R.drawable.msg_unpin else R.drawable.msg_pin) {
            AccountFolderStore.setPinned(folder, !pinned); onChanged()
        }
        entries += Entry("Перевести папку в онлайн", R.drawable.msg_retry) {
            slots.forEach { s ->
                val u = UserConfig.getInstance(s)
                u.ncgOffline = false
                u.saveConfig(false)
                ConnectionsManager.getInstance(s).resumeNetworkMaybe()
            }
            toast(context, "Папка «$folder» переведена в онлайн")
            onChanged()
        }
        entries += Entry("Перевести папку в офлайн (сон)", R.drawable.msg_cancel) {
            slots.forEach { s ->
                val u = UserConfig.getInstance(s)
                u.ncgOffline = true
                u.saveConfig(false)
                ConnectionsManager.getInstance(s).cleanup(false)
            }
            toast(context, "Папка «$folder» переведена в офлайн")
            onChanged()
        }
        entries += Entry("Переименовать папку", R.drawable.msg_edit) {
            AccountFolderLogic.promptRenameFolder(context, folder) { onChanged() }
        }
        entries += Entry("Удалить папку", R.drawable.msg_folders) {
            AccountFolderLogic.deleteFolder(folder) { onChanged() }
        }
        entries += Entry("Удалить папку и аккаунты…", R.drawable.msg_delete) {
            confirm(context, "Удалить ${slots.size} аккаунт(ов) из папки «$folder» из приложения? Сессии на сервере Telegram останутся.", "Удалить") {
                slots.forEach { MessagesController.getInstance(it).performLogout(0) }
                AccountFolderStore.delete(folder)
                onChanged()
            }
        }
        entries += Entry("Заменить прокси для папки…", R.drawable.msg_language) { replaceProxy(context, "в папке «$folder»", slots, onChanged) }
        show(context, folder, entries)
    }

    // one distinct proxy per account from a chosen list, like the desktop's multi-proxy flow
    // [where] finishes the process title: "в папке «X»", "у 5 аккаунтов".
    fun replaceProxy(context: Context, where: String, slots: List<Int>, onChanged: () -> Unit) {
        GgtgProxyChooser.show(context) { source ->
            val process = GgtgProcess.start("Замена прокси $where", GgtgProcess.Kind.Other)
            var changed = 0
            for (slot in slots) {
                val entry = source.popNext() ?: break
                ConnectionsManager.setProxySettingsForAccount(slot, true, entry.host, entry.port, entry.username ?: "", entry.password ?: "", "")
                GgtgDevice.autoGeoAfterProxy(slot)
                process.log("${GgtgAccounts.label(slot)} → ${entry.host}:${entry.port}")
                changed++
            }
            process.succeed("Прокси заменён у $changed аккаунт(ов).")
            toast(context, if (changed < slots.size) "Прокси заменён у $changed из ${slots.size}: список закончился" else "Прокси заменён у $changed аккаунт(ов)")
            onChanged()
        }
    }
}
