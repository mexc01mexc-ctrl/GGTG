package app.ggtg

import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * ggtg: sessions in the sync (same as Core::GgtgSessionCatalog on Desktop). One record per session travels
 * between the devices WITHOUT its key, so both show one picture: which session is on the phone, which on
 * Desktop, which waits in quarantine. Sessions this device does not hold are kept apart from the key vault,
 * in remote_sessions.json, and shown read-only; only the note is editable and the edit goes back to the
 * holder. Format: docs/ggtg-formats.md ("sessions").
 */
object GgtgSessionCatalog {
    data class Record(
        val id: String,
        val userId: String,
        val phone: String = "",
        val role: String = "",
        val note: String = "",
        val state: String = "",
        val platform: String = "",
        val holderId: String = "",
        val holder: String = "",
        val holderKind: String = "", // "desktop" or "android"
        val current: Boolean = false,
        val held: Boolean = true, // false: only a note about a session whose key left (never put in the catalog)
        val edited: Boolean = false, // local only: the note was edited here and must go back to the holder
        val createdAt: Long = 0L,
        val updatedAt: Long = 0L,
        val quarantineUntil: Long = 0L,
        val hash: Long = 0L
    )

    fun toJson(r: Record, local: Boolean = false): JSONObject = JSONObject().apply {
        put("id", r.id); put("userId", r.userId); put("phone", r.phone); put("role", r.role)
        put("note", r.note); put("state", r.state); put("platform", r.platform)
        put("holderId", r.holderId); put("holder", r.holder); put("holderKind", r.holderKind)
        put("current", r.current); put("held", r.held)
        put("createdAt", r.createdAt); put("updatedAt", r.updatedAt)
        put("quarantineUntil", r.quarantineUntil); put("hash", r.hash.toString())
        if (local && r.edited) put("edited", true)
    }

    fun fromJson(o: JSONObject) = Record(
        id = o.optString("id"), userId = o.optString("userId"), phone = o.optString("phone"),
        role = o.optString("role"), note = o.optString("note"), state = o.optString("state"),
        platform = o.optString("platform"), holderId = o.optString("holderId"),
        holder = o.optString("holder"), holderKind = o.optString("holderKind"),
        current = o.optBoolean("current"), held = o.optBoolean("held", true),
        edited = o.optBoolean("edited"), createdAt = o.optLong("createdAt"),
        updatedAt = o.optLong("updatedAt"), quarantineUntil = o.optLong("quarantineUntil"),
        hash = o.optString("hash").toLongOrNull() ?: 0L
    )

    private fun file() = File(GgtgDb.dir(), "remote_sessions.json")

    @Synchronized fun load(): List<Record> = try {
        val f = file()
        if (!f.exists()) emptyList() else {
            val arr = JSONObject(f.readText()).optJSONArray("sessions") ?: JSONArray()
            (0 until arr.length()).map { fromJson(arr.getJSONObject(it)) }
        }
    } catch (_: Throwable) { emptyList() }

    @Synchronized private fun save(list: List<Record>) {
        val arr = JSONArray()
        list.forEach { arr.put(toJson(it, true)) }
        try {
            val tmp = File(file().path + ".tmp")
            tmp.writeText(JSONObject().put("version", 1).put("sessions", arr).toString(2))
            if (!tmp.renameTo(file())) { file().writeText(tmp.readText()); tmp.delete() }
        } catch (_: Throwable) { }
    }

    /** Same session? By Telegram's number; failing that by account and the moment it was made. Never by account alone. */
    fun same(r: Record, s: GgtgSessions.StoredSession): Boolean {
        if (r.userId != s.userId.toString()) return false
        if (r.hash != 0L && s.hash != 0L) return r.hash == s.hash
        return r.createdAt > 0L && r.createdAt == s.createdAt
    }

    fun holderTitle(r: Record): String {
        val who = if (r.holder.isEmpty()) "" else " " + r.holder
        return if (r.holderKind == "android") "на телефоне$who" else "на Desktop$who"
    }

    /** "на телефоне Pixel · текущая · запасная · карантин до 07.11.2026 10:00 · подпись" */
    fun label(r: Record): String {
        val parts = ArrayList<String>()
        parts.add(holderTitle(r))
        if (r.current) parts.add("текущая")
        if (r.role.isNotEmpty() && r.role != "working") parts.add(GgtgSessions.roleTitle(r.role).lowercase(Locale.getDefault()))
        if (r.quarantineUntil > System.currentTimeMillis() / 1000) {
            parts.add("карантин до " + SimpleDateFormat("dd.MM.yyyy HH:mm", Locale.getDefault()).format(Date(r.quarantineUntil * 1000)))
        }
        if (r.state == "revoked") parts.add("завершена")
        if (r.createdAt > 0L) parts.add(SimpleDateFormat("dd.MM.yyyy", Locale.getDefault()).format(Date(r.createdAt * 1000)))
        if (r.note.isNotEmpty()) parts.add(r.note)
        return parts.joinToString(" · ")
    }

    /** Just what comes after the holder (for rows whose title already says it). */
    fun labelTail(r: Record): String = label(r).removePrefix(holderTitle(r)).removePrefix(" · ")

    fun forUser(userId: Long): List<Record> = load().filter { it.userId == userId.toString() }

    @JvmStatic
    fun byHash(userId: Long, hash: Long): Record? =
        if (hash == 0L) null else forUser(userId).firstOrNull { it.hash == hash }

    /** Edits the note of a session held elsewhere; it goes back with the next snapshot. */
    @JvmStatic
    fun editNote(holderId: String, id: String, note: String) {
        val now = System.currentTimeMillis() / 1000
        var found: Record? = null
        val all = load().map { r ->
            if (r.holderId == holderId && r.id == id) r.copy(note = note, updatedAt = now, edited = true).also { found = it } else r
        }
        save(all)
        val f = found ?: return
        for (s in GgtgSessions.listAll()) if (same(f, s)) GgtgSessions.updateNote(s.id, note)
    }

    /** Merges the records of one snapshot. [snapshotDevice] made it, [myDevice] is this device. */
    fun merge(records: List<Record>, snapshotDevice: String, myDevice: String): Int {
        var changed = 0
        // stubs older versions put in the vault (source "sync", no key) belong in the catalog, not there
        for (s in GgtgSessions.listAll()) {
            if (s.source == "sync" && !GgtgSessions.isHeld(s)) { GgtgSessions.removeStored(s.id); changed++ }
        }
        val vault = GgtgSessions.listAll()
        val catalog = load().toMutableList()
        val present = HashSet<Pair<String, String>>()
        for (r in records) {
            if (r.userId.isEmpty()) continue
            if (r.held && r.holderId == snapshotDevice) present.add(r.holderId to r.id)
            var matched = false
            for (s in vault) {
                if (!same(r, s) && !(r.holderId == myDevice && r.id == s.id)) continue
                matched = true
                if (r.updatedAt > s.updatedAt && r.note != s.note) {
                    GgtgSessions.updateNote(s.id, r.note)
                    GgtgSessions.setUpdatedAt(s.id, r.updatedAt)
                    changed++
                }
                if (r.hash != 0L && s.hash == 0L) GgtgSessions.updateHash(s.id, r.hash)
            }
            if (matched) continue
            if (r.holderId == myDevice && r.role == "working") {
                val uid = r.userId.toLongOrNull() ?: continue
                val own = GgtgSessions.sessionNote(uid)
                if (r.updatedAt > own.second && r.note != own.first) {
                    GgtgSessions.setSessionNote(uid, r.note, r.updatedAt)
                    changed++
                }
                continue
            }
            if (r.holderId == myDevice || !r.held) continue
            val i = catalog.indexOfFirst { it.holderId == r.holderId && it.id == r.id }
            if (i < 0) {
                catalog.add(r.copy(edited = false)); changed++
            } else {
                val c = catalog[i]
                if (c.edited && c.updatedAt > r.updatedAt) {
                    // our edit is newer than what the holder knows: keep it, it goes back
                    catalog[i] = r.copy(note = c.note, updatedAt = c.updatedAt, edited = true); changed++
                } else if (r.copy(edited = false) != c.copy(edited = false)) {
                    catalog[i] = r.copy(edited = false); changed++
                }
            }
        }
        // the snapshot's own device is the authority on what it holds: what it no longer lists is gone
        if (snapshotDevice.isNotEmpty() && snapshotDevice != myDevice) {
            if (catalog.removeAll { it.holderId == snapshotDevice && (it.holderId to it.id) !in present }) changed++
        }
        save(catalog)
        return changed
    }

    /** Every session this device knows, as records without keys. */
    fun localRecords(deviceId: String, deviceName: String): List<Record> {
        val result = ArrayList<Record>()
        val quarantine = GgtgSessions.loadQuarantine()
        val now = System.currentTimeMillis() / 1000
        val withCurrent = HashSet<Long>()
        for (s in GgtgSessions.listAll()) {
            if (s.userId == 0L) continue
            val held = GgtgSessions.isHeld(s)
            val cur = GgtgSessions.isCurrent(s)
            if (held && cur) withCurrent.add(s.userId)
            val until = quarantine.optJSONObject(s.userId.toString())?.optLong("until", 0L) ?: 0L
            result.add(Record(
                id = s.id, userId = s.userId.toString(), phone = s.phone, role = s.role, note = s.note,
                state = s.state, platform = GgtgSessions.platformOf(s),
                holderId = if (held) deviceId else "", holder = if (held) deviceName else "",
                holderKind = if (held) "android" else "",
                current = cur, held = held, createdAt = s.createdAt, updatedAt = s.updatedAt, hash = s.hash,
                quarantineUntil = if (held && until > now && s.role == "anti_theft_backup") until else 0L
            ))
        }
        // the session each live account works on here has no vault entry: it is the born date plus a note
        for (slot in 0 until org.telegram.messenger.UserConfig.MAX_ACCOUNT_COUNT) {
            val uc = org.telegram.messenger.UserConfig.getInstance(slot)
            if (!uc.isClientActivated) continue
            val uid = uc.clientUserId
            if (uid in withCurrent) continue
            GgtgSessions.noteSessionBornIfNew(uid)
            val born = GgtgSessions.sessionBorn(uid).first
            val note = GgtgSessions.sessionNote(uid)
            result.add(Record(
                id = "work-$uid", userId = uid.toString(), phone = uc.currentUser?.phone.orEmpty(),
                role = "working", note = note.first, state = "ok", platform = "android",
                holderId = deviceId, holder = deviceName, holderKind = "android", current = true,
                createdAt = born, updatedAt = maxOf(note.second, born)
            ))
        }
        // notes edited here about sessions of other devices go back to them
        result.addAll(load().filter { it.edited })
        return result
    }
}
