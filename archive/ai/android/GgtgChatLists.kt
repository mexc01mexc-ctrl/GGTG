package app.ggtg.ai

import android.content.Context
import android.os.Bundle
import org.json.JSONArray
import org.json.JSONObject
import org.telegram.messenger.AndroidUtilities
import org.telegram.messenger.MessagesController
import org.telegram.messenger.UserConfig
import org.telegram.messenger.UserObject
import org.telegram.ui.ActionBar.AlertDialog
import org.telegram.ui.ActionBar.BaseFragment
import org.telegram.ui.ChatActivity
import org.telegram.ui.LaunchActivity
import java.io.File

// ggtg: the user's own lists of chats ("Заинтересованные", "Подумать", "Клиенты"): a chat is put into a list
// from the chat's menu or from the selection in the chat list ("В список GGTG"), and "Списки чатов"
// opens any listed chat in one click, in the account it was put from. Kept in
// ggtg_bulk_db/chat_lists.json.
object GgtgChatLists {

    class Entry(
        val accountUserId: Long,
        val dialogId: Long,
        val username: String,
        val name: String,
        val accountName: String,
        val at: Long
    )

    class ChatList(val id: String, var name: String, val entries: MutableList<Entry>)

    private val lock = Any()
    private var loaded = false
    private val lists = ArrayList<ChatList>()
    @Volatile var version = 0
        private set
    private val listeners = java.util.concurrent.CopyOnWriteArrayList<() -> Unit>()

    fun addListener(l: () -> Unit) { listeners.add(l) }
    fun removeListener(l: () -> Unit) { listeners.remove(l) }

    private fun file() = File(GgtgAi.dir(), "chat_lists.json")

    private fun ensureLoaded() {
        if (loaded) return
        loaded = true
        GgtgAi.readText(file())?.let { text ->
            try {
                val arr = JSONObject(text).optJSONArray("lists") ?: JSONArray()
                for (i in 0 until arr.length()) {
                    val o = arr.getJSONObject(i)
                    val entries = ArrayList<Entry>()
                    val ea = o.optJSONArray("entries") ?: JSONArray()
                    for (j in 0 until ea.length()) {
                        val e = ea.getJSONObject(j)
                        entries.add(Entry(e.optString("account").toLongOrNull() ?: 0, e.optString("dialog").toLongOrNull() ?: 0,
                            e.optString("username"), e.optString("name"), e.optString("accountName"), e.optLong("at")))
                    }
                    lists.add(ChatList(o.optString("id"), o.optString("name"), entries))
                }
            } catch (t: Throwable) { }
        }
    }

    private fun save() {
        version++
        val arr = JSONArray()
        for (l in lists) {
            val ea = JSONArray()
            for (e in l.entries) ea.put(JSONObject().put("account", e.accountUserId.toString()).put("dialog", e.dialogId.toString())
                .put("username", e.username).put("name", e.name).put("accountName", e.accountName).put("at", e.at))
            arr.put(JSONObject().put("id", l.id).put("name", l.name).put("entries", ea))
        }
        GgtgAi.writeText(file(), JSONObject().put("lists", arr).toString())
        listeners.forEach { runCatching { AndroidUtilities.runOnUIThread { it() } } }
    }

    fun all(): List<ChatList> = synchronized(lock) { ensureLoaded(); lists.toList() }

    fun find(id: String): ChatList? = synchronized(lock) { ensureLoaded(); lists.firstOrNull { it.id == id } }

    fun create(nameRaw: String): String? {
        val name = nameRaw.trim().take(60)
        if (name.isEmpty()) return null
        synchronized(lock) {
            ensureLoaded()
            var id = java.lang.Long.toString(System.currentTimeMillis(), 36)
            while (lists.any { it.id == id }) id += "x"
            lists.add(ChatList(id, name, ArrayList()))
            save()
            return id
        }
    }

    fun rename(id: String, nameRaw: String) {
        val name = nameRaw.trim().take(60)
        synchronized(lock) { ensureLoaded(); lists.firstOrNull { it.id == id }?.takeIf { name.isNotEmpty() }?.let { it.name = name; save() } }
    }

    fun delete(id: String) { synchronized(lock) { ensureLoaded(); lists.removeAll { it.id == id }; save() } }

    fun has(listId: String, account: Long, dialogId: Long): Boolean = synchronized(lock) {
        ensureLoaded(); lists.firstOrNull { it.id == listId }?.entries?.any { it.accountUserId == account && it.dialogId == dialogId } == true
    }

    fun add(listId: String, entry: Entry) {
        synchronized(lock) {
            ensureLoaded()
            val l = lists.firstOrNull { it.id == listId } ?: return
            if (l.entries.any { it.accountUserId == entry.accountUserId && it.dialogId == entry.dialogId }) return
            l.entries.add(entry)
            save()
        }
    }

    fun remove(listId: String, account: Long, dialogId: Long) {
        synchronized(lock) {
            ensureLoaded()
            lists.firstOrNull { it.id == listId }?.entries?.removeAll { it.accountUserId == account && it.dialogId == dialogId }
            save()
        }
    }

    fun mergeJson(jsonStr: String) {
        if (jsonStr.isBlank()) return
        synchronized(lock) {
            ensureLoaded()
            try {
                val root = JSONObject(jsonStr)
                val incomingLists = root.optJSONArray("lists") ?: JSONArray()
                for (i in 0 until incomingLists.length()) {
                    val lObj = incomingLists.optJSONObject(i) ?: continue
                    val id = lObj.optString("id").trim()
                    val name = lObj.optString("name").trim().take(60)
                    if (name.isEmpty()) continue
                    var local = lists.firstOrNull { it.id == id }
                    if (local == null) {
                        local = lists.firstOrNull { it.name.equals(name, ignoreCase = true) }
                    }
                    if (local == null) {
                        val newId = if (id.isNotEmpty()) id else java.lang.Long.toString(System.currentTimeMillis(), 36)
                        local = ChatList(newId, name, ArrayList())
                        lists.add(local)
                    }
                    val ea = lObj.optJSONArray("entries") ?: JSONArray()
                    for (j in 0 until ea.length()) {
                        val e = ea.optJSONObject(j) ?: continue
                        val account = e.optString("account").toLongOrNull() ?: 0L
                        val dialog = (if (e.has("dialog")) e.optString("dialog") else e.optString("peer")).toLongOrNull() ?: 0L
                        if (account == 0L || dialog == 0L) continue
                        val at = e.optLong("at")
                        val username = e.optString("username")
                        val entryName = e.optString("name")
                        val accountName = e.optString("accountName")
                        val existingIndex = local.entries.indexOfFirst { it.accountUserId == account && it.dialogId == dialog }
                        if (existingIndex < 0) {
                            local.entries.add(Entry(account, dialog, username, entryName, accountName, at))
                        } else if (at > local.entries[existingIndex].at) {
                            local.entries[existingIndex] = Entry(account, dialog, username, entryName, accountName, at)
                        }
                    }
                }
                save()
            } catch (t: Throwable) {
                // ignore
            }
        }
    }

    /** The entry of a dialog of account [slot] (the person's or the group's name and @username). */
    fun entryFor(slot: Int, dialogId: Long): Entry {
        val uc = UserConfig.getInstance(slot)
        val controller = MessagesController.getInstance(slot)
        var name = ""
        var username = ""
        if (dialogId > 0) {
            controller.getUser(dialogId)?.let { name = UserObject.getUserName(it); username = UserObject.getPublicUsername(it).orEmpty() }
        } else {
            controller.getChat(-dialogId)?.let { name = it.title.orEmpty(); username = it.username.orEmpty() }
        }
        return Entry(uc.clientUserId, dialogId, username, name, app.ggtg.GgtgAccounts.label(slot), System.currentTimeMillis() / 1000)
    }

    /** "В список GGTG": the lists to choose from (a check where the chats already are: a tap takes them
     *  out), and a new list. [dialogIds] are the chats of account [slot] being put in. */
    @JvmStatic
    fun showPicker(context: Context?, slot: Int, dialogIds: List<Long>) {
        if (context == null || dialogIds.isEmpty()) return
        val uc = UserConfig.getInstance(slot)
        val lists = all()
        val labels = ArrayList<CharSequence>()
        for (l in lists) {
            val inAll = dialogIds.all { has(l.id, uc.clientUserId, it) }
            val count = l.entries.count { GgtgAiText.slotOf(it.accountUserId) >= 0 }
            labels.add((if (inAll) "✓ " else "") + l.name + " · " + count)
        }
        labels.add("Новый список…")
        AlertDialog.Builder(context).setTitle("В список GGTG")
            .setItems(labels.toTypedArray()) { _, which ->
                if (which < lists.size) {
                    val l = lists[which]
                    val inAll = dialogIds.all { has(l.id, uc.clientUserId, it) }
                    for (d in dialogIds) if (inAll) remove(l.id, uc.clientUserId, d) else add(l.id, entryFor(slot, d))
                    GgtgAiUi.toast(context, if (inAll) "Убрано из списка «${l.name}»" else "Добавлено в список «${l.name}»")
                } else {
                    GgtgAiUi.prompt(context, "Новый список", "Название (например, Заинтересованные)", ok = "Создать") { name ->
                        val id = create(name)
                        if (id != null) {
                            for (d in dialogIds) add(id, entryFor(slot, d))
                            GgtgAiUi.toast(context, "Список «$name» создан, чат добавлен")
                        }
                    }
                }
            }.show()
    }

    /** Opens the chat in its account (the account is switched to when it is another one). */
    fun openChat(from: BaseFragment, entry: Entry) {
        val slot = GgtgAiText.slotOf(entry.accountUserId)
        if (slot < 0) { GgtgAiUi.toast(from.context, "Аккаунт этого чата сейчас недоступен"); return }
        if (slot != UserConfig.selectedAccount) LaunchActivity.instance?.switchToAccount(slot, true)
        val args = Bundle()
        if (entry.dialogId > 0) args.putLong("user_id", entry.dialogId) else args.putLong("chat_id", -entry.dialogId)
        from.presentFragment(ChatActivity(args))
    }
}
