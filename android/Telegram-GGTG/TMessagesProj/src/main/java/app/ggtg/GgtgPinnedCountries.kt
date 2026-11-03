package app.ggtg

import org.json.JSONArray
import java.io.File

// Countries pinned to the top of the proxy country list. Stored as a JSON array of ISO2 codes,
// newest pin first, in filesDir/ggtg_bulk_db/pinned_countries.json (same format as Desktop).
object GgtgPinnedCountries {
    private fun file() = File(GgtgDb.dir(), "pinned_countries.json")

    @JvmStatic
    @Synchronized
    fun load(): List<String> {
        val f = file()
        if (!f.exists()) return emptyList()
        return try {
            val arr = JSONArray(f.readText())
            val out = ArrayList<String>()
            for (i in 0 until arr.length()) {
                val iso = arr.optString(i)
                if (iso.isNotEmpty() && !out.contains(iso)) out.add(iso)
            }
            out
        } catch (e: Exception) {
            emptyList()
        }
    }

    // Pins the country if it was not pinned, unpins it otherwise. Returns true when it is pinned now.
    @JvmStatic
    @Synchronized
    fun toggle(iso: String): Boolean {
        val list = ArrayList(load())
        val nowPinned = !list.remove(iso)
        if (nowPinned) list.add(0, iso)
        try {
            file().writeText(JSONArray(list).toString())
        } catch (_: Exception) {
        }
        return nowPinned
    }
}
