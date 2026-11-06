package app.ggtg

import android.content.Context
import android.widget.Toast
import org.json.JSONArray
import org.json.JSONObject
import org.telegram.ui.ActionBar.AlertDialog
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

// «Корзина пользователей»: the people the user threw out of a base by hand (the ✕ in the opened list) - what the user
// counts as rubbish and fakes. The detector learns from them (GgtgFakeDetector.learnFromMarks), and a person can be
// put back. The same as Core::GgtgUsersTrash on the desktop.
object GgtgUsersTrash {
    data class Entry(
        val username: String, // lowercase, without @
        val name: String = "",
        val id: Long = 0,
        val lastSeenText: String = "",
        val baseTitle: String = "",
        val at: Long = System.currentTimeMillis() / 1000
    )

    private fun file() = File(GgtgDb.dir(), "users_trash.json")
    private val lock = Any()
    private var cache: MutableList<Entry>? = null

    private fun list(): MutableList<Entry> = synchronized(lock) {
        cache?.let { return it }
        val result = ArrayList<Entry>()
        try {
            if (file().exists()) {
                val a = JSONArray(file().readText())
                for (i in 0 until a.length()) {
                    val o = a.optJSONObject(i) ?: continue
                    result.add(Entry(o.optString("username"), o.optString("name"), o.optString("id").toLongOrNull() ?: 0L,
                        o.optString("lastseen"), o.optString("base"), o.optString("at").toLongOrNull() ?: 0L))
                }
            }
        } catch (_: Throwable) {}
        cache = result
        result
    }

    private fun save() = synchronized(lock) {
        val a = JSONArray()
        for (e in list()) a.put(JSONObject().put("username", e.username).put("name", e.name).put("id", e.id.toString())
            .put("lastseen", e.lastSeenText).put("base", e.baseTitle).put("at", e.at.toString()))
        runCatching {
            val tmp = File(file().path + ".tmp"); tmp.writeText(a.toString()); tmp.renameTo(file())
        }
        Unit
    }

    fun all(): List<Entry> = synchronized(lock) { ArrayList(list()) }
    fun count(): Int = synchronized(lock) { list().size }

    fun add(entry: Entry) = synchronized(lock) {
        val clean = entry.copy(username = entry.username.trim().removePrefix("@").lowercase())
        if (clean.username.isEmpty()) return
        list().removeAll { it.username == clean.username }
        list().add(clean)
        save()
    }

    fun remove(username: String) = synchronized(lock) {
        val key = username.trim().removePrefix("@").lowercase()
        if (list().removeAll { it.username == key }) save()
    }

    fun clear() = synchronized(lock) { list().clear(); save() }

    /** The window of the trash: the newest first; a click puts the person back; the detector can learn from it. */
    fun showDialog(context: Context, onChanged: (() -> Unit)? = null) {
        val entries = all().asReversed().take(100)
        if (entries.isEmpty()) {
            AlertDialog.Builder(context).setTitle("Корзина пользователей").setMessage("Корзина пуста. Сюда попадают люди, которых вы убрали из базы крестиком в открытом списке: то, что вы считаете мусором и фейками. Программа учится на них и находит похожих при очистке базы.").setPositiveButton("Закрыть", null).show()
            return
        }
        val labels = entries.map { e ->
            val title = if (e.name.isEmpty()) "@${e.username}" else e.name
            val sub = "@${e.username}" + (if (e.baseTitle.isNotEmpty()) " · из «${e.baseTitle}»" else "") + " · " + SimpleDateFormat("dd.MM HH:mm", Locale.getDefault()).format(Date(e.at * 1000))
            "$title\n$sub"
        }.toTypedArray<CharSequence>()
        AlertDialog.Builder(context)
            .setTitle("Корзина пользователей (${count()})")
            .setItems(labels) { _, which ->
                val e = entries[which]
                val bases = GgtgDb.loadIndex().filter { it.kind == GgtgDb.BaseKind.Users }
                val target = bases.firstOrNull { it.title == e.baseTitle } ?: bases.firstOrNull()
                if (target == null) { Toast.makeText(context, "Нет базы пользователей, куда вернуть", Toast.LENGTH_SHORT).show(); return@setItems }
                GgtgDb.saveUsernames(target.id, target.title, GgtgDb.loadUsernames(target.id) + e.username, crossDedupe = false)
                remove(e.username)
                Toast.makeText(context, "@${e.username} возвращён в «${target.title}»", Toast.LENGTH_SHORT).show()
                onChanged?.invoke()
            }
            .setPositiveButton("Учиться") { _, _ -> GgtgFakeDetector.trainFromMarked(context) }
            .setNeutralButton("Очистить") { _, _ ->
                AlertDialog.Builder(context).setTitle("Очистить корзину?").setMessage("Людей в базу это не вернёт, программа только забудет, чему на них училась. Уже найденные признаки останутся.")
                    .setPositiveButton("Очистить") { _, _ -> clear(); onChanged?.invoke() }.setNegativeButton("Отмена", null).show()
            }
            .setNegativeButton("Закрыть", null)
            .show()
    }
}
