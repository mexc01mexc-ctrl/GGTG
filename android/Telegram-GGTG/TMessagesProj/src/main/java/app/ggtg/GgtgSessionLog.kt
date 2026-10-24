package app.ggtg

import org.json.JSONArray
import org.json.JSONObject
import org.telegram.messenger.ApplicationLoader
import org.telegram.messenger.UserConfig
import java.io.File

// ggtg: journal of accounts whose session Telegram ended while the app ran (the native layer reports it
// through ConnectionsManager.onLogout, after which the app wipes the account). Without this note the
// account would just vanish. Mirrors the desktop's tdata/ggtg_db/sessions.json (core/ggtg_sessions.h).
object GgtgSessionLog {

    data class Entry(
        val at: Long,
        val userId: Long,
        val phone: String,
        val name: String,
        val proxy: String = "",
        val username: String = "",
        val folder: String = "",
        val twoFactor: String = "",
        val reason: String = "AUTH_KEY_UNREGISTERED",
        val description: String = "",
        val code: Int = 401,
        val liveAccounts: Int = 0
    )

    private const val MAX_ENTRIES = 5000
    private val lock = Any()

    private fun file() = File(File(ApplicationLoader.getFilesDirFixed(), "ggtg_bulk_db").also { it.mkdirs() }, "sessions.json")

    fun reasonTitle(reason: String): String = when (reason) {
        "AUTH_KEY_DUPLICATED" -> "Аккаунт открыли в двух местах одновременно"
        "AUTH_KEY_UNREGISTERED" -> "Telegram больше не признаёт эту сессию"
        "SESSION_REVOKED" -> "Сессию завершили"
        "SESSION_EXPIRED" -> "Сессия истекла"
        "USER_DEACTIVATED" -> "Аккаунт удалён"
        "USER_DEACTIVATED_BAN" -> "Аккаунт заблокирован Telegram"
        "SWAP" -> "Переключение сессии не завершилось"
        else -> "Telegram закрыл сессию"
    }

    fun reasonHint(reason: String): String = when (reason) {
        "AUTH_KEY_DUPLICATED" ->
            "Одну и ту же сессию (одну и ту же папку tdata) запустили " +
            "сразу в двух приложениях - например на Mac и на Windows, или " +
            "включили копию сессии, пока оригинал был в сети. " +
            "В таком случае Telegram закрывает сессию. Для копии используйте " +
            "в экспорте «Новую сессию»: у неё своя отдельная сессия."
        "AUTH_KEY_UNREGISTERED" ->
            "Сессию закрыли в другом месте, либо Telegram снял её сам. " +
            "Чаще всего так бывает из-за подозрительной активности (много " +
            "входов с одного адреса, массовые действия, частая смена " +
            "прокси) или если аккаунт заблокирован."
        "SESSION_REVOKED" ->
            "Сессию закрыли вручную: в настройках аккаунта («Активные " +
            "сеансы»), через массовое действие «Завершить другие сессии» " +
            "или с другого устройства."
        "SESSION_EXPIRED" ->
            "Аккаунтом долго не пользовались, и сессия закрылась сама " +
            "(в настройках Telegram можно выбрать, через сколько неактивные " +
            "сессии завершаются)."
        "USER_DEACTIVATED" ->
            "Аккаунт удалён - им самим или из-за долгого неиспользования."
        "USER_DEACTIVATED_BAN" ->
            "Telegram заблокировал аккаунт. Войти в него заново обычно нельзя."
        "SWAP" ->
            "Приложение начало переключать аккаунт на другую сессию, " +
            "но не довело это до конца (сбой или закрытие). Прежняя " +
            "сессия сохранена как резервная: нажмите «Восстановить»."
        else ->
            "Telegram ответил, что сессия недействительна. Точная причина " +
            "не указана, код ошибки показан рядом."
    }

    private fun load(): MutableList<Entry> {
        val result = ArrayList<Entry>()
        try {
            val f = file()
            if (!f.exists()) return result
            val text = f.readText().trim()
            if (text.isEmpty()) return result
            val array = if (text.startsWith("{")) {
                val root = JSONObject(text)
                root.optJSONArray("lost") ?: JSONArray()
            } else {
                JSONArray(text)
            }
            for (i in 0 until array.length()) {
                val o = array.getJSONObject(i)
                val uid = o.optLong("userId").takeIf { it != 0L }
                    ?: o.optString("userId").toLongOrNull() ?: 0L
                result.add(
                    Entry(
                        at = o.optLong("at"),
                        userId = uid,
                        phone = o.optString("phone"),
                        name = o.optString("name"),
                        proxy = o.optString("proxy"),
                        username = o.optString("username"),
                        folder = o.optString("folder"),
                        twoFactor = o.optString("twoFactor"),
                        reason = o.optString("reason", "AUTH_KEY_UNREGISTERED"),
                        description = o.optString("description"),
                        code = o.optInt("code", 401),
                        liveAccounts = o.optInt("liveAccounts")
                    )
                )
            }
        } catch (_: Throwable) { }
        return result
    }

    private fun save(entries: List<Entry>) {
        try {
            val kept = if (entries.size > MAX_ENTRIES) entries.takeLast(MAX_ENTRIES) else entries
            val array = JSONArray()
            for (e in kept) {
                array.put(JSONObject().apply {
                    put("at", e.at)
                    put("userId", e.userId.toString())
                    put("phone", e.phone)
                    put("name", e.name)
                    put("username", e.username)
                    put("folder", e.folder)
                    put("twoFactor", e.twoFactor)
                    put("proxy", e.proxy)
                    put("reason", e.reason)
                    put("description", e.description)
                    put("code", e.code)
                    put("liveAccounts", e.liveAccounts)
                })
            }
            val root = JSONObject().apply {
                put("version", 1)
                put("lost", array)
            }
            file().writeText(root.toString(2))
        } catch (_: Throwable) { }
    }

    /** Call BEFORE the account's config is cleared. */
    @JvmStatic
    @JvmOverloads
    fun record(slot: Int, reason: String = "AUTH_KEY_UNREGISTERED", description: String = "", code: Int = 401) {
        try {
            val uc = UserConfig.getInstance(slot)
            val user = uc.currentUser
            val userId = uc.clientUserId
            if (userId == 0L) return
            val liveCount = (0 until UserConfig.MAX_ACCOUNT_COUNT).count {
                UserConfig.getInstance(it).isClientActivated
            }
            val entry = Entry(
                at = System.currentTimeMillis() / 1000,
                userId = userId,
                phone = user?.phone.orEmpty(),
                name = listOfNotNull(user?.first_name, user?.last_name).joinToString(" ").trim(),
                proxy = if (uc.ncgProxyEnabled) "${uc.ncgProxyAddress}:${uc.ncgProxyPort}" else "",
                username = user?.username.orEmpty(),
                folder = uc.ncgFolder.orEmpty(),
                twoFactor = uc.ncgTwoFactor.orEmpty(),
                reason = reason,
                description = description,
                code = code,
                liveAccounts = liveCount
            )
            synchronized(lock) {
                val all = load()
                all.add(entry)
                save(all)
            }
        } catch (_: Throwable) { }
    }

    fun forUser(userId: Long): List<Entry> = synchronized(lock) {
        load().filter { it.userId == userId }
    }

    /** Returns newest lost session per user id that is not currently active in UserConfig */
    fun currentLostAccounts(): List<Entry> = synchronized(lock) {
        val all = load()
        val activeIds = HashSet<Long>()
        for (i in 0 until UserConfig.MAX_ACCOUNT_COUNT) {
            val uc = UserConfig.getInstance(i)
            if (uc.isClientActivated && uc.clientUserId != 0L) {
                activeIds.add(uc.clientUserId)
            }
        }
        val latestPerUser = LinkedHashMap<Long, Entry>()
        // Iterate in reverse (newest first)
        for (i in all.indices.reversed()) {
            val e = all[i]
            if (e.userId != 0L && !activeIds.contains(e.userId) && !latestPerUser.containsKey(e.userId)) {
                latestPerUser[e.userId] = e
            }
        }
        latestPerUser.values.toList()
    }

    fun removeUser(userId: Long) = synchronized(lock) {
        val all = load()
        all.removeAll { it.userId == userId }
        save(all)
    }

    fun clear() = synchronized(lock) {
        save(emptyList())
    }
}
