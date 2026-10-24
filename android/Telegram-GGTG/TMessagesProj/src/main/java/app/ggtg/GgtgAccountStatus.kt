package app.ggtg

import org.telegram.messenger.MessagesController
import org.telegram.messenger.UserConfig

// ggtg: one status per account, the most serious thing known about it (like LiveAccountStatus on the
// desktop). `rank` is the sort key of the manager: low = ready to work with, high = trouble.
//   0 Свободен   1 Не проверен / В корзине   2 Проверяется   3 Нет прокси
//   4 Спам-блок до...   5 Спам-блок навсегда   6 Заморожен
// An account in the trash keeps its real status: a frozen one stays "Заморожен", one with a spam
// block keeps it; only an account with nothing wrong is plain "В корзине".
object GgtgAccountStatus {
    class Status(val rank: Int, val text: String, val color: Int)

    private const val GRAY = 0xFF8E8E93.toInt()

    private fun frozen(slot: Int): Boolean {
        val mc = MessagesController.getInstance(slot)
        if (mc.freezeSinceDate <= 0L) return false
        val until = mc.freezeUntilDate
        return until <= 0L || until > System.currentTimeMillis() / 1000
    }

    /** Frozen or under a spam block: what is known without asking the connection; null when neither. */
    private fun trouble(slot: Int, userId: Long): Status? {
        if (frozen(slot)) return Status(6, "Заморожен", 0xFF5AC8FA.toInt())
        val s = GgtgSpamStatus.find(userId) ?: return null
        val now = System.currentTimeMillis() / 1000
        return when {
            s.state == GgtgSpamStatus.State.Permanent -> Status(5, "Спам-блок навсегда", 0xFFFF453A.toInt())
            s.state == GgtgSpamStatus.State.Temporary && s.until > now -> Status(4, GgtgSpamStatus.statusText(userId), 0xFFFF9F0A.toInt())
            else -> null
        }
    }

    fun of(slot: Int): Status {
        val uc = UserConfig.getInstance(slot)
        val userId = uc.clientUserId
        if (uc.ncgTrashed) return trouble(slot, userId) ?: Status(1, "В корзине", GRAY)
        if (uc.ncgOffline) return Status(1, "Офлайн", 0xFF788CA0.toInt())
        if (!uc.ncgProxyEnabled && !GgtgSpamStatus.isChecking(userId)) return Status(3, "Нет прокси", 0xFFFF9F0A.toInt())
        trouble(slot, userId)?.let { return it }
        if (GgtgSpamStatus.isChecking(userId)) return Status(2, "Проверяется…", 0xFFFFC107.toInt())
        if (GgtgSpamStatus.find(userId) == null) return Status(1, "Не проверен", GRAY)
        return Status(0, "Свободен", 0xFF4CD964.toInt())
    }
}
