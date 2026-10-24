package app.ggtg

import org.json.JSONArray
import org.json.JSONObject
import org.telegram.messenger.ApplicationLoader
import java.io.File

// ng: port of the desktop's saved Squidward lookups (window/ggtg_squidward.cpp, LookupCache):
// every card and every collected list is kept in filesDir/ggtg_bulk_db/squidward_cache.json, so a
// second lookup of the same person or group shows the same result at once, without asking the bot
// (and without spending its limits). "Пробить заново" bypasses it.
object GgtgSquidwardCache {

    private const val LIMIT = 300

    class Saved(
        val result: GgtgSquidward.LookupResult,
        var groups: GgtgSquidward.GroupCheck? = null,
        var users: List<String>? = null,
        var savedAt: Long = System.currentTimeMillis() / 1000
    )

    private val lock = Any()
    private var loaded = false
    private val entries = LinkedHashMap<String, Saved>()
    private val aliases = HashMap<String, String>()

    private fun file() = File(File(ApplicationLoader.getFilesDirFixed(), "ggtg_bulk_db").also { it.mkdirs() }, "squidward_cache.json")

    /** "@Name", "name", "t.me/name" and "https://t.me/name/" are one key. */
    fun key(query: String): String {
        var q = query.trim().lowercase()
        for (prefix in listOf("https://", "http://")) if (q.startsWith(prefix)) q = q.removePrefix(prefix)
        for (prefix in listOf("t.me/", "telegram.me/")) if (q.startsWith(prefix)) q = q.removePrefix(prefix)
        return q.trimStart('@').trimEnd('/')
    }

    fun find(query: String): Saved? = synchronized(lock) {
        ensureLoaded()
        var k = key(query)
        aliases[k]?.let { k = it }
        entries[k]
    }

    /** A fresh card starts the entry over: its old lists are another day's. */
    fun putCard(query: String, result: GgtgSquidward.LookupResult) = synchronized(lock) {
        if (result.kind == "unknown") return
        val k = key(query)
        if (k.isEmpty()) return
        ensureLoaded()
        entries.remove(k)
        entries[k] = Saved(result)
        val names = ArrayList<String>()
        result.userCard?.let { names.add(it.id); names.addAll(it.usernames) }
        result.groupCard?.let { names.add(it.id) }
        for (alias in names) {
            val a = key(alias)
            if (a.isNotEmpty() && a != k) aliases[a] = k
        }
        trim()
        save()
    }

    fun putGroups(query: String, check: GgtgSquidward.GroupCheck) = synchronized(lock) {
        ensureLoaded()
        val saved = entries[key(query)] ?: return
        saved.groups = check; saved.users = null; saved.savedAt = System.currentTimeMillis() / 1000
        save()
    }

    fun putUsers(query: String, users: List<String>) = synchronized(lock) {
        ensureLoaded()
        val saved = entries[key(query)] ?: return
        saved.users = users; saved.groups = null; saved.savedAt = System.currentTimeMillis() / 1000
        save()
    }

    private fun trim() {
        while (entries.size > LIMIT) {
            val oldest = entries.entries.minByOrNull { it.value.savedAt } ?: return
            entries.remove(oldest.key)
            aliases.values.removeAll { it == oldest.key }
        }
    }

    private fun ensureLoaded() {
        if (loaded) return
        loaded = true
        runCatching {
            val f = file()
            if (!f.exists()) return
            val root = JSONObject(f.readText())
            val list = root.optJSONObject("entries") ?: JSONObject()
            for (k in list.keys()) {
                val o = list.getJSONObject(k)
                val card = o.getJSONObject("card")
                val details = card.optJSONArray("details") ?: JSONArray()
                val result = GgtgSquidward.LookupResult(
                    kind = card.optString("kind"),
                    userCard = null,
                    groupCard = null,
                    rawText = card.optString("raw"),
                    details = (0 until details.length()).map { details.getString(it) },
                    baseTitle = card.optString("title"),
                    canCollect = card.optBoolean("canCollect"))
                val saved = Saved(result, savedAt = o.optLong("at"))
                o.optJSONObject("groups")?.let { g ->
                    val arr = g.optJSONArray("list") ?: JSONArray()
                    val kept = (0 until arr.length()).map {
                        val e = arr.getJSONObject(it)
                        GgtgDb.GroupEntry(e.optString("u"), e.optString("t"), e.optInt("m"), e.optString("l"), e.optBoolean("c"))
                    }
                    saved.groups = GgtgSquidward.GroupCheck(kept, g.optInt("channels"), g.optInt("closed"), g.optInt("unchecked"))
                }
                o.optJSONArray("users")?.let { u -> saved.users = (0 until u.length()).map { u.getString(it) } }
                entries[k] = saved
            }
            root.optJSONObject("aliases")?.let { a -> for (k in a.keys()) aliases[k] = a.getString(k) }
        }
    }

    private fun save() {
        runCatching {
            val list = JSONObject()
            for ((k, v) in entries) {
                val o = JSONObject()
                    .put("card", JSONObject()
                        .put("kind", v.result.kind)
                        .put("raw", v.result.rawText)
                        .put("details", JSONArray(v.result.details))
                        .put("title", v.result.baseTitle)
                        .put("canCollect", v.result.canCollect))
                    .put("at", v.savedAt)
                v.groups?.let { g ->
                    val arr = JSONArray()
                    for (e in g.kept) arr.put(JSONObject().put("u", e.username).put("t", e.title).put("m", e.membersCount).put("l", e.link).put("c", e.checked))
                    o.put("groups", JSONObject().put("list", arr).put("channels", g.channelsRemoved).put("closed", g.closed).put("unchecked", g.unchecked))
                }
                v.users?.let { o.put("users", JSONArray(it)) }
                list.put(k, o)
            }
            val a = JSONObject()
            for ((k, v) in aliases) a.put(k, v)
            val f = file()
            val tmp = File(f.parentFile, f.name + ".tmp")
            tmp.writeText(JSONObject().put("entries", list).put("aliases", a).toString())
            if (!tmp.renameTo(f)) { f.delete(); tmp.renameTo(f) }
        }
    }
}
