package app.ggtg

import kotlinx.coroutines.delay
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlinx.coroutines.withTimeoutOrNull
import org.telegram.messenger.AccountInstance
import org.telegram.messenger.AndroidUtilities
import org.telegram.messenger.ApplicationLoader
import org.telegram.messenger.UserConfig
import org.telegram.messenger.UserObject
import org.telegram.tgnet.ConnectionsManager
import org.telegram.tgnet.TLObject
import org.telegram.tgnet.TLRPC
import kotlin.coroutines.resume

// ng: shared helpers for the GGTG bulk features - enumerating the logged-in accounts (grouped by
// the ncgFolder tag the account switcher already uses), a human label per account, and a suspend
// wrapper around ConnectionsManager.sendRequest bound to one specific account. Mirrors the
// desktop app's "run this on account N" model (settings/bulk_accounts_actions.cpp).
object GgtgAccounts {

    // The key of the «Корзина» group in the pickers (a private first character keeps it from clashing
    // with a folder the user named the same); shown as [folderTitle].
    const val TRASH_FOLDER = "\u2063Корзина"

    fun folderTitle(folder: String): String = when {
        folder == TRASH_FOLDER -> "Корзина"
        folder.isEmpty() -> "Без папки"
        else -> folder
    }

    data class AccountRef(val slot: Int, val userId: Long, val folder: String)

    fun activated(): List<AccountRef> {
        val result = ArrayList<AccountRef>()
        for (slot in 0 until UserConfig.MAX_ACCOUNT_COUNT) {
            val uc = UserConfig.getInstance(slot)
            if (!uc.isClientActivated) continue
            // An account in the «Корзина» is listed under it, not in the folder it used to be in.
            result.add(AccountRef(slot, uc.clientUserId, if (uc.ncgTrashed) TRASH_FOLDER else (uc.ncgFolder ?: "")))
        }
        // ungrouped first, then folders, the trash last, each keeping ncgOrder / loginTime order within
        return result.sortedWith(compareBy({ it.folder == TRASH_FOLDER }, { it.folder.isNotEmpty() }, { it.folder }))
    }

    /** «Корзина»: hides the accounts from Telegram's own lists (see AccountFolderLogic) while
     *  [activated] still returns them, so every GGTG picker keeps offering them. */
    fun setTrashed(slots: List<Int>, trashed: Boolean) {
        for (slot in slots) {
            val uc = UserConfig.getInstance(slot)
            uc.ncgTrashed = trashed
            uc.saveConfig(false)
        }
        // A trashed account goes to sleep (no connection, no notifications); a restored one wakes up.
        for (slot in slots) {
            val asleep = trashed && slot != UserConfig.selectedAccount
            AndroidUtilities.runOnUIThread { ConnectionsManager.getInstance(slot).setAsleep(asleep) }
        }
    }

    /** Wakes the accounts a campaign / check is about to use (a trashed account sleeps without a connection). */
    fun wake(slots: Collection<Int>) {
        for (slot in slots) {
            if (UserConfig.getInstance(slot).isClientActivated) {
                AndroidUtilities.runOnUIThread { ConnectionsManager.getInstance(slot).setAsleep(false) }
            }
        }
    }

    /** Puts trashed accounts back to sleep once nothing needs them: no operation is running and the
     *  account is neither the open one nor the one Squidward is talking through. */
    fun sleepIdle() {
        if (GgtgProcess.runningCount() > 0) return
        for (slot in 0 until UserConfig.MAX_ACCOUNT_COUNT) {
            val uc = UserConfig.getInstance(slot)
            if (!uc.isClientActivated || !uc.ncgTrashed) continue
            if (slot == UserConfig.selectedAccount || (slot == GgtgSquidward.viaSlot && GgtgSquidward.screenOpen)) continue
            AndroidUtilities.runOnUIThread { ConnectionsManager.getInstance(slot).setAsleep(true) }
        }
    }

    fun isTrashed(slot: Int): Boolean = UserConfig.getInstance(slot).ncgTrashed

    fun byFolder(): LinkedHashMap<String, MutableList<AccountRef>> {
        val map = LinkedHashMap<String, MutableList<AccountRef>>()
        for (ref in activated()) {
            map.getOrPut(ref.folder) { ArrayList() }.add(ref)
        }
        return map
    }

    fun user(slot: Int): TLRPC.User? = UserConfig.getInstance(slot).currentUser

    fun label(slot: Int): String {
        val uc = UserConfig.getInstance(slot)
        val user = uc.currentUser ?: return "Аккаунт ${slot + 1}"
        val name = UserObject.getUserName(user).trim()
        val phone = user.phone
        return when {
            name.isNotEmpty() && !phone.isNullOrEmpty() -> "$name · +$phone"
            name.isNotEmpty() -> name
            !phone.isNullOrEmpty() -> "+$phone"
            else -> "ID ${uc.clientUserId}"
        }
    }

    fun connections(slot: Int): ConnectionsManager = AccountInstance.getInstance(slot).connectionsManager

    // ---- network loss ----
    // A request must not be spent (and a recipient not wrongly written off) just because the phone has
    // no internet: while it is offline every GGTG request waits for the network instead, and a request
    // that timed out during an outage is simply sent again.

    @Volatile private var waitingForNetwork = false

    private fun anyStopRequested() =
        GgtgSms.cancelled || GgtgViews.cancelled || GgtgBulk.cancelled || GgtgSquidward.cancelled

    /** Suspends while the phone is offline. Returns false when a stop was requested meanwhile. */
    suspend fun awaitNetwork(): Boolean {
        if (ApplicationLoader.isNetworkOnline()) return true
        if (!waitingForNetwork) {
            waitingForNetwork = true
            GgtgProcess.logToRunning("Нет сети — жду возвращения интернета (рассылка на паузе)…")
        }
        while (!ApplicationLoader.isNetworkOnline()) {
            if (anyStopRequested()) { waitingForNetwork = false; return false }
            delay(2_000)
        }
        // a short breather for the connections to come back before the queue resumes
        delay(3_000)
        if (waitingForNetwork) {
            waitingForNetwork = false
            GgtgProcess.logToRunning("Сеть вернулась — продолжаю.")
        }
        return true
    }

    /** Bridges ConnectionsManager.sendRequest into a suspend call with a hard timeout, on the
     *  given account's own connection. Returns null on a real timeout (or a requested stop).
     *  Offline: waits for the network; a request that timed out while the phone was offline is
     *  sent again (same object, so the same random_id - the server never delivers a message twice). */
    suspend fun request(
        slot: Int,
        req: TLObject,
        flags: Int = 0,
        dcId: Int = ConnectionsManager.DEFAULT_DATACENTER_ID,
        timeoutMs: Long = 60_000
    ): Pair<TLObject?, TLRPC.TL_error?>? {
        while (true) {
            if (!awaitNetwork()) return null
            val result = withTimeoutOrNull(timeoutMs) {
                suspendCancellableCoroutine<Pair<TLObject?, TLRPC.TL_error?>> { cont ->
                    val token = connections(slot).sendRequest(req, { resp, err ->
                        if (cont.isActive) cont.resume(resp to err)
                    }, null, null, flags, dcId, ConnectionsManager.ConnectionTypeGeneric, true)
                    cont.invokeOnCancellation { connections(slot).cancelRequest(token, false) }
                }
            }
            if (result != null) return result
            // timed out: a real timeout while online, or the network dropped - then try again
            if (ApplicationLoader.isNetworkOnline()) return null
        }
    }
}
