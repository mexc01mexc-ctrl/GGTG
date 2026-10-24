package app.ggtg

import org.telegram.messenger.MessagesController
import org.telegram.messenger.NotificationCenter
import org.telegram.messenger.UserConfig
import org.telegram.messenger.UserObject
import org.telegram.tgnet.TLRPC

// ng: GGTG Desktop's TrackManualSends. A message sent by hand from an ordinary chat (outside a
// campaign) to a person with a username puts that username into the "Отправленные" registry
// (✓ OK in the clickable lists). messageReceivedByServer fires once a local message got its real id,
// i.e. it was actually delivered. An existing record is never overwritten.
object GgtgManualSends : NotificationCenter.NotificationCenterDelegate {

    private val tracked = HashSet<Int>()
    // (account, recipient) pairs already counted into the 24h counter today - a chat with several messages counts once
    private val counted = HashSet<Pair<Int, Long>>()
    private var countedDay = 0L

    @JvmStatic
    fun track(account: Int) {
        app.ggtg.ai.GgtgAiInbox.startBackground() // the feed's own timers (the autopilot, the morning brief)
        if (!tracked.add(account)) return
        NotificationCenter.getInstance(account).addObserver(this, NotificationCenter.messageReceivedByServer)
        NotificationCenter.getInstance(account).addObserver(this, NotificationCenter.didReceiveNewMessages)
    }

    override fun didReceivedNotification(id: Int, account: Int, vararg args: Any?) {
        if (id == NotificationCenter.didReceiveNewMessages) {
            onIncoming(args)
            return
        }
        if (id != NotificationCenter.messageReceivedByServer) return
        val oldId = args.getOrNull(0) as? Int ?: return
        val message = args.getOrNull(2) as? TLRPC.Message ?: return
        val dialogId = args.getOrNull(3) as? Long ?: return
        val scheduled = args.getOrNull(6) as? Boolean ?: false
        if (oldId >= 0 || scheduled || !message.out || dialogId <= 0 || message.action != null) return
        if (message.message.isNullOrEmpty() && message.media == null) return
        val controller = MessagesController.getInstance(account)
        val user = controller.getUser(dialogId) ?: return
        val username = UserObject.getPublicUsername(user)
        if (username.isNullOrEmpty() || user.bot || user.self || user.id == UserConfig.getInstance(account).getClientUserId()) return
        // counts into the account's "SMS 24h" like a campaign send does (Desktop: RegisterManualSmsSent)
        val day = System.currentTimeMillis() / 86_400_000L
        if (day != countedDay) { countedDay = day; counted.clear() }
        if (counted.add(account to dialogId)) {
            GgtgSms.registerDaySent(account)
            GgtgProcess.notifyChanged()
        }
        if (GgtgDb.findSmsRecord(username) != null) return
        val label = GgtgAccounts.label(account)
        // the base the person came from is looked up away from the main thread (it reads the bases)
        Thread {
            GgtgDb.markSmsStatus(username, GgtgDb.SmsStatus.Sent, label, GgtgDb.findBaseOf(username))
        }.start()
    }

    // An answer from someone this app wrote to (a campaign or by hand) goes into the replies list.
    private fun onIncoming(args: Array<out Any?>) {
        val dialogId = args.getOrNull(0) as? Long ?: return
        if (dialogId <= 0 || (args.getOrNull(2) as? Boolean) == true) return
        val list = args.getOrNull(1) as? java.util.ArrayList<*> ?: return
        for (item in list) {
            val message = (item as? org.telegram.messenger.MessageObject)?.messageOwner ?: continue
            if (message.out || message.action != null) continue
            val text = message.message
            if (text.isNullOrEmpty()) continue
            val slot = currentAccountOf(item)
            val user = MessagesController.getInstance(slot).getUser(dialogId) ?: continue
            val username = UserObject.getPublicUsername(user)
            if (username.isNullOrEmpty()) continue
            if (GgtgDb.findSmsRecord(username)?.status != GgtgDb.SmsStatus.Sent) continue
            GgtgDb.saveReply(username, text, message.date.toLong())
            // the feed of answers ("ИИ" → «5. Лента ответов»): the person is waiting for a word
            app.ggtg.ai.GgtgAiInbox.onIncoming(slot, dialogId, username, UserObject.getUserName(user), text, message.id.toLong(), message.date.toLong())
        }
    }

    private fun currentAccountOf(item: Any?): Int = (item as org.telegram.messenger.MessageObject).currentAccount
}
