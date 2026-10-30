package app.ggtg

import android.content.Context
import android.graphics.Typeface
import android.os.Bundle
import android.view.View
import android.view.Gravity
import android.view.ViewGroup
import android.widget.FrameLayout
import android.widget.TextView
import android.widget.Toast
import android.widget.LinearLayout
import org.telegram.messenger.AndroidUtilities
import org.telegram.messenger.ContactsController
import org.telegram.messenger.LocaleController
import androidx.recyclerview.widget.LinearLayoutManager
import androidx.recyclerview.widget.RecyclerView
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.launch
import org.telegram.messenger.MessagesController
import org.telegram.messenger.R
import org.telegram.messenger.UserConfig
import org.telegram.messenger.browser.Browser
import org.telegram.tgnet.ConnectionsManager
import org.telegram.tgnet.TLRPC
import org.telegram.ui.ActionBar.ActionBar.ActionBarMenuOnItemClick
import org.telegram.ui.ActionBar.BaseFragment
import org.telegram.ui.ActionBar.Theme
import org.telegram.ui.ChatActivity
import org.telegram.ui.ProfileActivity
import org.telegram.ui.Components.AvatarDrawable
import org.telegram.ui.Components.BackupImageView
import org.telegram.ui.Components.LayoutHelper
import org.telegram.ui.Components.RecyclerListView
import kotlinx.coroutines.Job
import kotlinx.coroutines.isActive
import kotlinx.coroutines.delay
import kotlinx.coroutines.withContext

// ng: "Открыть списком (кликабельно)" styled like native group members list:
// avatar with photo or colored circle with initials, bold name, @username,
// online / last seen date and sorting options (base, name, username, id, online).
class GgtgBaseListActivity(private val baseId: Long, private val isGroups: Boolean) : BaseFragment() {

    private data class Row(
        val username: String,
        val fallbackUrl: String = "",
        val title: String = "",
        val sub: String = "",
        val userId: Long = 0,
        val isOnline: Boolean = false,
        val lastSeenTime: Long = 0,
        val lastSeenText: String = "",
        val membersCount: Int = 0,
        val user: TLRPC.User? = null,
        val chat: TLRPC.Chat? = null
    )
    private data class Resolved(val users: ArrayList<TLRPC.User>, val chats: ArrayList<TLRPC.Chat>)

    enum class SortOrder { Base, Name, Username, Id, Online }
    private var currentOrder = SortOrder.Base

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main)
    private var resolveJob: Job? = null
    private val rows = ArrayList<Row>()
    private var listView: RecyclerListView? = null
    private var sortRowView: TextView? = null
    private var countView: TextView? = null
    private var opening = false
    private var searchQuery = ""
    private var activeAccountSlot: Int = UserConfig.selectedAccount

    private fun showAccountPicker(onChosen: (Int) -> Unit) {
        val ctx = context ?: return
        val activated = GgtgAccounts.activated()
        if (activated.isEmpty()) {
            toast("Нет подключённых аккаунтов")
            return
        }
        val items = activated.map { ref ->
            val label = GgtgAccounts.label(ref.slot)
            val folder = if (ref.folder.isNotEmpty()) " [${GgtgAccounts.folderTitle(ref.folder)}]" else ""
            val check = if (ref.slot == activeAccountSlot) "● " else "○ "
            check + label + folder
        }.toTypedArray()

        org.telegram.ui.ActionBar.AlertDialog.Builder(ctx)
            .setTitle("Выберите аккаунт для переписки")
            .setItems(items) { _, which ->
                val chosen = activated.getOrNull(which)?.slot ?: return@setItems
                onChosen(chosen)
            }
            .setNegativeButton("Отмена", null)
            .show()
    }

    private fun sortTitle(order: SortOrder) = when (order) {
        SortOrder.Base -> "Как в базе"
        SortOrder.Name -> "По имени"
        SortOrder.Username -> "По юзернейму"
        SortOrder.Id -> "По ID"
        SortOrder.Online -> "По дате онлайна"
    }

    private fun reloadRows() {
        val rawRows = ArrayList<Row>()
        if (isGroups) {
            val account = UserConfig.selectedAccount
            val msgCtrl = MessagesController.getInstance(account)
            for (e in GgtgDb.loadGroupEntries(baseId)) {
                val clean = e.username.trim().removePrefix("@")
                val chat: TLRPC.Chat? = if (clean.isNotEmpty()) msgCtrl.getUserOrChat(clean) as? TLRPC.Chat else null
                val raw = if (e.username.isNotEmpty()) "@" + e.username else e.link
                val url = if (raw.startsWith("http")) raw else if (raw.startsWith("t.me")) "https://$raw" else "https://t.me/${e.username}"
                val title = if (chat != null && !chat.title.isNullOrEmpty()) chat.title else e.title.ifEmpty { raw }
                val members = if (chat != null && chat.participants_count > 0) chat.participants_count else e.membersCount
                rawRows.add(Row(
                    username = clean,
                    fallbackUrl = url,
                    title = title,
                    sub = if (clean.isNotEmpty()) "@$clean" else raw,
                    membersCount = members,
                    chat = chat
                ))
            }
        } else {
            val account = UserConfig.selectedAccount
            val msgCtrl = MessagesController.getInstance(account)
            for (u in GgtgDb.loadUsernames(baseId)) {
                val clean = u.trim().removePrefix("@")
                val meta = GgtgDb.lookupUser(clean)
                val uid = meta?.id ?: 0L
                val user: TLRPC.User? = msgCtrl.getUser(clean) ?: (if (uid != 0L) msgCtrl.getUser(uid) else null)
                val actualUid = user?.id ?: uid
                val name = if (user != null) {
                    ContactsController.formatName(user.first_name, user.last_name).ifEmpty { meta?.name ?: "@$clean" }
                } else if (!meta?.name.isNullOrEmpty()) {
                    meta!!.name
                } else {
                    "@$clean"
                }
                val isOnline = user?.status is TLRPC.TL_userStatusOnline
                val now = System.currentTimeMillis() / 1000
                val lastSeenTime = if (user != null) {
                    when (val s = user.status) {
                        is TLRPC.TL_userStatusOnline -> now + 86400
                        is TLRPC.TL_userStatusRecently -> now - 3600
                        is TLRPC.TL_userStatusLastWeek -> now - 86400 * 3
                        is TLRPC.TL_userStatusLastMonth -> now - 86400 * 15
                        is TLRPC.TL_userStatusOffline -> s.expires.toLong()
                        else -> 0L
                    }
                } else {
                    meta?.lastSeenTime ?: 0L
                }
                val lastSeenText = if (user != null) {
                    LocaleController.formatUserStatus(account, user)
                } else {
                    meta?.lastSeenText ?: ""
                }
                rawRows.add(Row(
                    username = clean,
                    fallbackUrl = "https://t.me/$clean",
                    title = name,
                    sub = "@$clean",
                    userId = actualUid,
                    isOnline = isOnline,
                    lastSeenTime = lastSeenTime,
                    lastSeenText = lastSeenText,
                    user = user
                ))
            }
        }

        val totalCount = rawRows.size
        if (searchQuery.isNotEmpty()) {
            val q = searchQuery.removePrefix("@")
            rawRows.retainAll { r ->
                r.username.lowercase().contains(q) ||
                r.title.lowercase().contains(searchQuery) ||
                (r.userId != 0L && r.userId.toString().contains(searchQuery))
            }
        }

        when (currentOrder) {
            SortOrder.Base -> {}
            SortOrder.Name -> rawRows.sortWith(compareBy(String.CASE_INSENSITIVE_ORDER) { it.title })
            SortOrder.Username -> rawRows.sortWith(compareBy(String.CASE_INSENSITIVE_ORDER) { it.username })
            SortOrder.Id -> rawRows.sortWith(compareBy({ it.userId == 0L }, { it.userId }))
            SortOrder.Online -> rawRows.sortWith(compareByDescending { it.lastSeenTime })
        }
        rows.clear()
        rows.addAll(rawRows)
        listView?.adapter?.notifyDataSetChanged()
        countView?.text = if (searchQuery.isEmpty()) {
            (if (isGroups) "Групп: " else "Пользователей: ") + totalCount
        } else {
            "${rows.size} из $totalCount"
        }
        startBackgroundResolving()
    }

    private fun startBackgroundResolving() {
        resolveJob?.cancel()
        val account = UserConfig.selectedAccount
        val msgCtrl = MessagesController.getInstance(account)
        resolveJob = scope.launch(Dispatchers.IO) {
            val toResolve = synchronized(rows) {
                rows.mapIndexedNotNull { index, row ->
                    if (row.username.isNotEmpty() && ((!isGroups && row.user == null) || (isGroups && row.chat == null))) {
                        index to row.username
                    } else null
                }
            }
            for ((index, username) in toResolve) {
                if (!isActive) break
                try {
                    val req = TLRPC.TL_contacts_resolveUsername().apply { this.username = username }
                    val pair = GgtgAccounts.request(account, req, ConnectionsManager.RequestFlagFailOnServerErrors)
                    val resolved = pair?.first as? TLRPC.TL_contacts_resolvedPeer
                    if (resolved != null) {
                        withContext(Dispatchers.Main) {
                            msgCtrl.putUsers(resolved.users, false)
                            msgCtrl.putChats(resolved.chats, false)
                            if (index < rows.size && rows[index].username.equals(username, ignoreCase = true)) {
                                val old = rows[index]
                                if (isGroups) {
                                    val chat = resolved.chats.firstOrNull()
                                    if (chat != null) {
                                        val members = if (chat.participants_count > 0) chat.participants_count else old.membersCount
                                        rows[index] = old.copy(
                                            title = chat.title.ifEmpty { old.title },
                                            membersCount = members,
                                            chat = chat
                                        )
                                        listView?.adapter?.notifyItemChanged(index)
                                    }
                                } else {
                                    val user = resolved.users.firstOrNull()
                                    if (user != null) {
                                        val isOnline = user.status is TLRPC.TL_userStatusOnline
                                        val lastSeenText = LocaleController.formatUserStatus(account, user)
                                        val name = ContactsController.formatName(user.first_name, user.last_name).ifEmpty { old.title }
                                        rows[index] = old.copy(
                                            title = name,
                                            userId = user.id,
                                            isOnline = isOnline,
                                            lastSeenText = lastSeenText,
                                            user = user
                                        )
                                        listView?.adapter?.notifyItemChanged(index)
                                    }
                                }
                            }
                        }
                    }
                } catch (_: Throwable) {
                }
                delay(120)
            }
        }
    }

    private fun showSortDialog() {
        val ctx = context ?: return
        val items = arrayOf(
            (if (currentOrder == SortOrder.Base) "● " else "   ") + "Как в базе",
            (if (currentOrder == SortOrder.Name) "● " else "   ") + "По имени",
            (if (currentOrder == SortOrder.Username) "● " else "   ") + "По юзернейму",
            (if (currentOrder == SortOrder.Id) "● " else "   ") + "По ID",
            (if (currentOrder == SortOrder.Online) "● " else "   ") + "По дате онлайна"
        )
        org.telegram.ui.ActionBar.AlertDialog.Builder(ctx)
            .setTitle("Сортировка")
            .setItems(items) { _, which ->
                val newOrder = when (which) {
                    0 -> SortOrder.Base
                    1 -> SortOrder.Name
                    2 -> SortOrder.Username
                    3 -> SortOrder.Id
                    4 -> SortOrder.Online
                    else -> SortOrder.Base
                }
                if (currentOrder != newOrder) {
                    currentOrder = newOrder
                    sortRowView?.text = "≡  Сортировка: " + sortTitle(currentOrder) + " ▾"
                    reloadRows()
                }
            }
            .setNegativeButton("Отмена", null)
            .show()
    }

    override fun createView(context: Context): View {
        actionBar.setBackButtonImage(R.drawable.ic_ab_back)
        actionBar.setAllowOverlayTitle(true)
        val meta = GgtgDb.loadIndex().firstOrNull { it.id == baseId }
        actionBar.setTitle("Список")
        actionBar.setActionBarMenuOnItemClick(object : ActionBarMenuOnItemClick() {
            override fun onItemClick(id: Int) {
                when (id) {
                    -1 -> finishFragment()
                    2 -> showSortDialog()
                }
            }
        })
        val menu = actionBar.createMenu()
        menu.addItem(2, R.drawable.msg_reorder)

        val frame = FrameLayout(context)
        frame.setBackgroundColor(Theme.getColor(Theme.key_windowBackgroundGray))
        fragmentView = frame

        val list = RecyclerListView(context)
        list.setLayoutManager(LinearLayoutManager(context))
        list.adapter = Adapter(context)
        list.setOnItemClickListener { _, position, _, _ -> rows.getOrNull(position)?.let { open(it, preferChat = true) } }
        // long press: the extras (the icons on the right stay as they were)
        list.setOnItemLongClickListener(RecyclerListView.OnItemLongClickListener { _, position ->
            val row = rows.getOrNull(position) ?: return@OnItemLongClickListener false
            val ctx = context ?: return@OnItemLongClickListener false
            val items = ArrayList<GgtgUi.MenuItem>()
            val handle = if (row.username.isNotEmpty()) "@" + row.username else row.fallbackUrl
            items.add(GgtgUi.MenuItem(if (row.username.isNotEmpty()) "Копировать @username" else "Копировать ссылку", R.drawable.msg_copy) {
                AndroidUtilities.addToClipboard(handle); toast("Скопировано")
            })
            if (row.title.isNotEmpty() && row.title != handle) {
                items.add(GgtgUi.MenuItem("Копировать имя", R.drawable.msg_copy) {
                    AndroidUtilities.addToClipboard(row.title); toast("Скопировано")
                })
            }
            if (row.userId != 0L) {
                items.add(GgtgUi.MenuItem("Копировать ID", R.drawable.msg_copy) {
                    AndroidUtilities.addToClipboard(row.userId.toString()); toast("Скопировано")
                })
            }
            if (!isGroups) items.add(GgtgUi.MenuItem("Перенести в другую базу…", R.drawable.msg_forward) { moveRow(ctx, row) })
            if (!isGroups && GgtgDb.findSmsRecord(row.username) != null) {
                items.add(GgtgUi.MenuItem("Вернуть в очередь (сбросить статус)", R.drawable.msg_reset) {
                    GgtgDb.deleteSmsRecord(row.username); toast("Статус сброшен")
                })
            }
            GgtgUi.menu(ctx, row.title, items)
            true
        })
        val column = LinearLayout(context).apply { orientation = LinearLayout.VERTICAL }
        val head = GgtgUi.card(context)
        head.addView(GgtgUi.title(context, meta?.title ?: "Список", 17f))
        val countTv = GgtgUi.body(context, (if (isGroups) "Групп: " else "Пользователей: ") + (meta?.count ?: 0))
        countView = countTv
        head.addView(countTv,
            LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply { topMargin = AndroidUtilities.dp(2f) })
        head.addView(GgtgUi.body(context, "Долгое нажатие на строке — действия.", 12f),
            LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply { topMargin = AndroidUtilities.dp(4f) })

        val accountCard = LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            background = GgtgUi.rounded(Theme.getColor(Theme.key_windowBackgroundGray), AndroidUtilities.dp(8f).toFloat())
            setPadding(AndroidUtilities.dp(10f), AndroidUtilities.dp(8f), AndroidUtilities.dp(10f), AndroidUtilities.dp(8f))
        }
        val accIcon = android.widget.ImageView(context).apply {
            setImageDrawable(GgtgUi.icon(context, R.drawable.msg_contacts, Theme.getColor(Theme.key_windowBackgroundWhiteBlueText)))
        }
        val accText = TextView(context).apply {
            textSize = 13f
            setTypeface(null, Typeface.BOLD)
            setTextColor(Theme.getColor(Theme.key_windowBackgroundWhiteBlueText))
            setPadding(AndroidUtilities.dp(8f), 0, AndroidUtilities.dp(4f), 0)
        }
        val accChevron = TextView(context).apply {
            text = "▾"
            textSize = 14f
            setTypeface(null, Typeface.BOLD)
            setTextColor(Theme.getColor(Theme.key_windowBackgroundWhiteBlueText))
        }
        fun refreshAccText() {
            val label = GgtgAccounts.label(activeAccountSlot)
            accText.text = "Писать с: $label"
        }
        refreshAccText()
        accountCard.addView(accIcon, LinearLayout.LayoutParams(AndroidUtilities.dp(18f), AndroidUtilities.dp(18f)))
        accountCard.addView(accText, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
        accountCard.addView(accChevron)
        accountCard.setOnClickListener {
            showAccountPicker { slot ->
                activeAccountSlot = slot
                refreshAccText()
                toast("Аккаунт переключен: ${GgtgAccounts.label(slot)}")
            }
        }
        head.addView(accountCard,
            LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply {
                topMargin = AndroidUtilities.dp(8f)
            })

        val search = android.widget.EditText(context).apply {
            hint = "Поиск по имени, @username или ID"
            setSingleLine()
            textSize = 14f
            background = GgtgUi.rounded(Theme.getColor(Theme.key_windowBackgroundWhite), AndroidUtilities.dp(8f).toFloat())
            setTextColor(Theme.getColor(Theme.key_windowBackgroundWhiteBlackText))
            setHintTextColor(Theme.getColor(Theme.key_windowBackgroundWhiteHintText))
            setPadding(AndroidUtilities.dp(12f), AndroidUtilities.dp(8f), AndroidUtilities.dp(12f), AndroidUtilities.dp(8f))
            addTextChangedListener(object : android.text.TextWatcher {
                override fun beforeTextChanged(s: CharSequence?, a: Int, b: Int, c: Int) {}
                override fun onTextChanged(s: CharSequence?, a: Int, b: Int, c: Int) {}
                override fun afterTextChanged(s: android.text.Editable?) {
                    searchQuery = s?.toString()?.trim()?.lowercase() ?: ""
                    reloadRows()
                }
            })
        }
        head.addView(search,
            LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply {
                topMargin = AndroidUtilities.dp(8f)
                bottomMargin = AndroidUtilities.dp(2f)
            })

        val sortRow = TextView(context).apply {
            text = "≡  Сортировка: " + sortTitle(currentOrder) + " ▾"
            textSize = 14f
            setTypeface(null, Typeface.BOLD)
            setTextColor(Theme.getColor(Theme.key_windowBackgroundWhiteBlueText))
            setPadding(0, AndroidUtilities.dp(8f), 0, 0)
            setOnClickListener { showSortDialog() }
        }
        sortRowView = sortRow
        head.addView(sortRow)

        GgtgUi.addCard(column, head, top = 8f)
        column.addView(list, LayoutHelper.createLinear(LayoutHelper.MATCH_PARENT, 0, 1f, 0, 8, 0, 0))
        frame.addView(column, LayoutHelper.createFrame(LayoutHelper.MATCH_PARENT, LayoutHelper.MATCH_PARENT.toFloat()))
        listView = list

        reloadRows()
        return frame
    }

    private fun open(row: Row, preferChat: Boolean = true) {
        val context = context ?: return
        val account = activeAccountSlot
        if (row.username.isEmpty()) {
            Browser.openUrl(context, row.fallbackUrl)
            return
        }
        if (account != UserConfig.selectedAccount) {
            org.telegram.ui.LaunchActivity.instance?.switchToAccount(account, false)
        }
        if (row.user != null) {
            val args = Bundle().apply { putLong("user_id", row.user.id) }
            val fragment = if (preferChat) {
                ChatActivity(args)
            } else {
                ProfileActivity(args)
            }
            fragment.setCurrentAccount(account)
            presentFragment(fragment)
            return
        }
        if (row.chat != null) {
            val args = Bundle().apply { putLong("chat_id", row.chat.id) }
            val fragment = ChatActivity(args)
            fragment.setCurrentAccount(account)
            presentFragment(fragment)
            return
        }
        if (opening) return
        if (GgtgDb.findSmsRecord(row.username)?.status == GgtgDb.SmsStatus.NotFound) {
            toast("@${row.username} - не найден")
            return
        }
        opening = true
        scope.launch {
            val req = TLRPC.TL_contacts_resolveUsername().apply { username = row.username }
            val pair = GgtgAccounts.request(account, req, ConnectionsManager.RequestFlagFailOnServerErrors)
            val resolved = pair?.first as? TLRPC.TL_contacts_resolvedPeer
            var resp = resolved?.let { Resolved(it.users, it.chats) }
            if (resp == null && pair?.second?.text?.startsWith("FLOOD_WAIT") == true) {
                resp = searchFallback(account, row.username)
            }
            opening = false
            if (resp == null) {
                val err = pair?.second?.text
                if (err != null && (err.startsWith("USERNAME_") || err == "PEER_ID_INVALID")) {
                    toast("@${row.username} - не найден")
                    return@launch
                }
                toast(if (err != null) "Не удалось открыть @${row.username}: $err" else "Не удалось открыть @${row.username}")
                return@launch
            }
            MessagesController.getInstance(account).putUsers(resp.users, false)
            MessagesController.getInstance(account).putChats(resp.chats, false)
            val args = Bundle()
            val chat = resp.chats.firstOrNull()
            if (chat != null) {
                args.putLong("chat_id", chat.id)
                val fragment = ChatActivity(args)
                fragment.setCurrentAccount(account)
                presentFragment(fragment)
            } else {
                val user = resp.users.firstOrNull()
                if (user == null) { toast("Не найдено: @${row.username}"); return@launch }
                args.putLong("user_id", user.id)
                val fragment = if (preferChat) {
                    ChatActivity(args)
                } else {
                    ProfileActivity(args)
                }
                fragment.setCurrentAccount(account)
                presentFragment(fragment)
            }
        }
    }

    // ng: contacts.search - the same RPC the in-app search bar uses - as a fallback when
    // resolveUsername alone is flood-limited. Matches the exact username among the results.
    private suspend fun searchFallback(account: Int, username: String): Resolved? {
        val req = TLRPC.TL_contacts_search().apply { q = username; limit = 5 }
        val pair = GgtgAccounts.request(account, req, ConnectionsManager.RequestFlagFailOnServerErrors)
        val found = pair?.first as? TLRPC.TL_contacts_found ?: return null
        val user = found.users.firstOrNull { it.username?.equals(username, ignoreCase = true) == true }
        val chat = found.chats.firstOrNull { it.username?.equals(username, ignoreCase = true) == true }
        if (user == null && chat == null) return null
        return Resolved(ArrayList(listOfNotNull(user)), ArrayList(listOfNotNull(chat)))
    }

    // macOS parity (bulk_accounts_actions "⬇" / "🦑"): parse a group's members into a users base,
    // directly or through @skvidvarde_bot for groups that hide their members.
    private fun parseMembers(row: Row) {
        if (row.username.isEmpty()) { toast("Закрытая группа - только через Сквидвард"); return }
        toast("Запущено - смотрите «Процессы»")
        CoroutineScope(SupervisorJob() + Dispatchers.IO).launch {
            runCatching { GgtgScraper.scrapeMembers(row.username, GgtgScraper.Options()) }
        }
    }

    private fun viaSquidward(row: Row) {
        val q = if (row.username.isNotEmpty()) "@" + row.username else row.fallbackUrl
        toast("Запрос к Сквидварду…")
        CoroutineScope(SupervisorJob() + Dispatchers.IO).launch {
            val r = runCatching { GgtgSquidward.lookup(q, prefer = "group") }.getOrNull()
            if (r == null || !r.canCollect) {
                AndroidUtilities.runOnUIThread { toast(if (r == null) "Бот не ответил" else if (r.rawText == "Выбор отменён") r.rawText else "Сквидвард не даёт собрать участников") }
                return@launch
            }
            AndroidUtilities.runOnUIThread { toast("Запущено - смотрите «Процессы»") }
            runCatching { GgtgSquidward.collect(q, r.kind == "user", r.baseTitle) }
        }
    }

    private fun toast(text: String) { context?.let { Toast.makeText(it, text, Toast.LENGTH_LONG).show() } }

    // ✓ OK / — status from the shared "sent" registry, same marks as GGTG Desktop's clickable list
    private fun mark(row: Row): String {
        if (isGroups || row.username.isEmpty()) return ""
        val r = GgtgDb.findSmsRecord(row.username) ?: return ""
        return if (r.status == GgtgDb.SmsStatus.Sent) "  ✓ OK" else "  — " + when (r.status) {
            GgtgDb.SmsStatus.SpamAcc -> "спам-блок аккаунта"
            GgtgDb.SmsStatus.PeerLimit -> "отказ Telegram"
            GgtgDb.SmsStatus.NotFound -> "не найден"
            GgtgDb.SmsStatus.Restricted -> "закрыт для ЛС"
            GgtgDb.SmsStatus.SuspectedFake -> "предположительный фейк"
            GgtgDb.SmsStatus.LongAgo -> "был(а) давно"
            else -> "OK"
        }
    }

    // ✕ drops the row from the base at once (macOS "RemoveButton")
    private fun removeRow(row: Row) {
        val meta = GgtgDb.loadIndex().firstOrNull { it.id == baseId } ?: return
        val index = rows.indexOf(row)
        if (index < 0) return
        if (isGroups) {
            val left = GgtgDb.loadGroupEntries(baseId).filterNot {
                (row.username.isNotEmpty() && it.username.equals(row.username, true)) || (row.username.isEmpty() && it.link.removePrefix("https://").removePrefix("http://") == row.fallbackUrl.removePrefix("https://").removePrefix("http://"))
            }
            if (left.size == GgtgDb.loadGroupEntries(baseId).size) { toast("Не удалось удалить"); return }
            GgtgDb.saveGroupEntries(baseId, meta.title, left)
        } else {
            GgtgDb.saveUsernames(baseId, meta.title, GgtgDb.loadUsernames(baseId).filter { !it.equals(row.username, true) }, crossDedupe = false)
        }
        rows.removeAt(index)
        listView?.adapter?.notifyDataSetChanged()
        toast((if (row.username.isNotEmpty()) "@${row.username}" else "Запись") + " удалён из базы")
    }

    // long press -> "Перенести в другую базу…": a person lives in one base, so he leaves this one
    private fun moveRow(ctx: Context, row: Row) {
        val meta = GgtgDb.loadIndex().firstOrNull { it.id == baseId } ?: return
        val others = GgtgDb.loadIndex().filter { it.kind == GgtgDb.BaseKind.Users && it.id != baseId }
        if (others.isEmpty()) { toast("Других баз пользователей пока нет"); return }
        org.telegram.ui.ActionBar.AlertDialog.Builder(ctx)
            .setTitle("Перенести в базу")
            .setItems(others.map { it.title }.toTypedArray()) { _, which ->
                val target = others[which]
                GgtgDb.saveUsernames(baseId, meta.title, GgtgDb.loadUsernames(baseId).filter { !it.equals(row.username, true) }, crossDedupe = false)
                GgtgDb.saveUsernames(target.id, target.title, GgtgDb.loadUsernames(target.id) + row.username, crossDedupe = false)
                rows.remove(row)
                listView?.adapter?.notifyDataSetChanged()
                toast("@${row.username} перенесён в «${target.title}»")
            }
            .setNegativeButton("Отмена", null)
            .show()
    }

    private val dbListener: () -> Unit = {
        AndroidUtilities.runOnUIThread { listView?.adapter?.notifyDataSetChanged() }
    }

    override fun onFragmentCreate(): Boolean {
        GgtgDb.addListener(dbListener)
        return super.onFragmentCreate()
    }

    override fun onFragmentDestroy() {
        resolveJob?.cancel()
        GgtgDb.removeListener(dbListener)
        super.onFragmentDestroy()
    }

    // Premium row: avatar on the left, bold name, @username · online date · status in subtitle,
    // and action glyphs on the right.
    private inner class Adapter(val ctx: Context) : RecyclerListView.SelectionAdapter() {
        override fun isEnabled(holder: RecyclerView.ViewHolder) = true
        override fun getItemCount() = rows.size

        private fun dp(v: Float) = AndroidUtilities.dp(v)

        override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): RecyclerView.ViewHolder {
            val glyphs = if (isGroups) listOf("✕", "⬇", "🦑") else listOf("👁", "✕")
            val wrap = LinearLayout(ctx).apply {
                orientation = LinearLayout.HORIZONTAL
                gravity = Gravity.CENTER_VERTICAL
                setBackgroundColor(Theme.getColor(Theme.key_windowBackgroundWhite))
                setPadding(dp(16f), dp(8f), dp(8f), dp(8f))
            }

            val avatar = BackupImageView(ctx).apply {
                setRoundRadius(dp(21f))
                tag = "avatar"
            }
            wrap.addView(avatar, LinearLayout.LayoutParams(dp(42f), dp(42f)).apply {
                rightMargin = dp(12f)
            })

            val texts = LinearLayout(ctx).apply {
                orientation = LinearLayout.VERTICAL
                tag = "texts"
            }
            // Line 1: Name / Title
            texts.addView(TextView(ctx).apply {
                tag = "title"
                textSize = 15f
                setTypeface(null, Typeface.BOLD)
                setTextColor(Theme.getColor(Theme.key_windowBackgroundWhiteBlackText))
                setSingleLine()
                ellipsize = android.text.TextUtils.TruncateAt.END
            })
            // Line 2: @handle, link or ID
            texts.addView(TextView(ctx).apply {
                tag = "handle"
                textSize = 13f
                setTextColor(Theme.getColor(Theme.key_windowBackgroundWhiteGrayText))
                setSingleLine()
                ellipsize = android.text.TextUtils.TruncateAt.END
                setPadding(0, dp(1f), 0, 0)
            })
            // Line 3: Online status / Members count + status mark
            texts.addView(TextView(ctx).apply {
                tag = "status"
                textSize = 12f
                setSingleLine()
                ellipsize = android.text.TextUtils.TruncateAt.END
                setPadding(0, dp(1f), 0, 0)
            })
            wrap.addView(texts, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))

            for (glyph in glyphs) {
                // Telegram's own icons: profile/info (eye), remove, download members, look up via Squidward
                val res = when (glyph) {
                    "👁", "💬" -> R.drawable.msg_views
                    "✕" -> R.drawable.msg_delete
                    "⬇" -> R.drawable.msg_download
                    else -> R.drawable.msg_squidward
                }
                val tint = if (glyph == "✕") Theme.getColor(Theme.key_text_RedRegular) else Theme.getColor(Theme.key_windowBackgroundWhiteGrayIcon)
                wrap.addView(android.widget.ImageView(ctx).apply {
                    setImageDrawable(GgtgUi.icon(ctx, res, tint))
                    scaleType = android.widget.ImageView.ScaleType.CENTER
                    tag = glyph
                    background = Theme.createSelectorDrawable(Theme.getColor(Theme.key_listSelector), 1)
                }, LinearLayout.LayoutParams(dp(40f), dp(40f)))
            }
            wrap.layoutParams = RecyclerView.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT)
            return RecyclerListView.Holder(wrap)
        }

        override fun onBindViewHolder(holder: RecyclerView.ViewHolder, position: Int) {
            val row = rows[position]
            val wrap = holder.itemView as LinearLayout
            val avatarView = wrap.findViewWithTag<BackupImageView>("avatar")
            val texts = wrap.findViewWithTag<LinearLayout>("texts") ?: return
            val status = mark(row).trim()

            if (avatarView != null) {
                val avatarDrawable = AvatarDrawable()
                if (row.user != null) {
                    avatarDrawable.setInfo(row.user)
                    avatarView.setForUserOrChat(row.user, avatarDrawable)
                } else if (row.chat != null) {
                    avatarDrawable.setInfo(row.chat)
                    avatarView.setForUserOrChat(row.chat, avatarDrawable)
                } else {
                    val idForColor = if (row.userId != 0L) row.userId else row.username.hashCode().toLong()
                    avatarDrawable.setInfo(idForColor, row.title.removePrefix("@"), null)
                    avatarView.setImageDrawable(avatarDrawable)
                }
            }

            // Line 1: Title / Name
            texts.findViewWithTag<TextView>("title")?.text = row.title

            // Line 2: @handle, link or ID
            val handleTv = texts.findViewWithTag<TextView>("handle")
            val handleText = if (row.username.isNotEmpty()) {
                "@${row.username}"
            } else if (row.fallbackUrl.isNotEmpty()) {
                row.fallbackUrl
            } else if (row.userId != 0L) {
                "ID: ${row.userId}"
            } else ""
            handleTv?.text = handleText
            handleTv?.visibility = if (handleText.isEmpty() || handleText.equals(row.title, ignoreCase = true)) View.GONE else View.VISIBLE

            // Line 3: Online status / Members count + status mark
            val statusTv = texts.findViewWithTag<TextView>("status")
            val parts = ArrayList<String>()
            if (isGroups) {
                if (row.membersCount > 0) {
                    parts.add("${row.membersCount} уч.")
                }
            } else {
                if (row.lastSeenText.isNotEmpty()) {
                    parts.add(row.lastSeenText)
                } else if (row.userId != 0L && handleText.isEmpty()) {
                    parts.add("ID: ${row.userId}")
                }
            }
            if (status.isNotEmpty()) {
                parts.add(status)
            }
            val line3 = parts.joinToString("  ·  ")
            statusTv?.apply {
                text = line3
                visibility = if (line3.isEmpty()) View.GONE else View.VISIBLE
                if (row.isOnline) {
                    setTextColor(Theme.getColor(Theme.key_windowBackgroundWhiteBlueText))
                } else {
                    setTextColor(Theme.getColor(Theme.key_windowBackgroundWhiteGrayText))
                }
            }

            for (i in 2 until wrap.childCount) {
                val b = wrap.getChildAt(i)
                when (b.tag) {
                    "👁", "💬" -> b.setOnClickListener { open(row, preferChat = false) }
                    "🦑" -> b.setOnClickListener { viaSquidward(row) }
                    "⬇" -> {
                        b.alpha = if (row.username.isEmpty()) 0.3f else 1f
                        b.setOnClickListener { parseMembers(row) }
                    }
                    "✕" -> b.setOnClickListener { removeRow(row) }
                }
            }
        }
    }
}
