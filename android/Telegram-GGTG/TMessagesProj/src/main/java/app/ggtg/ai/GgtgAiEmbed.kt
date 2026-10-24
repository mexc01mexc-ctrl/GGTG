package app.ggtg.ai

import android.util.Base64
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.security.MessageDigest
import kotlin.math.sqrt

// ggtg: meaning search (core/ggtg_ai_embed.h). Google's free embedding model turns a text into numbers
// so that texts with the same meaning are close ("сколько стоит" and "какая цена" meet, which word
// matching cannot do). It needs a Gemini key; without one everything works by words. Vectors of
// texts already seen are kept in ggtg_bulk_db/ai_embed_cache.json, so each text is embedded once.
// The texts leave with names, numbers and usernames replaced by marks.
object GgtgAiEmbed {
    private const val MODEL = "gemini-embedding-001"
    private const val DIMENSIONS = 768

    private val vectors = HashMap<String, FloatArray>()
    private var loaded = false
    private var dirty = false

    private fun file() = File(GgtgAi.dir(), "ai_embed_cache.json")

    private fun keyOf(text: String): String {
        val bytes = MessageDigest.getInstance("SHA-1").digest(text.toByteArray(Charsets.UTF_8))
        return bytes.joinToString("") { "%02x".format(it) }
    }

    private fun norm(text: String) = text.trim().replace(Regex("\\s+"), " ").take(600)

    @Synchronized private fun ensureLoaded() {
        if (loaded) return
        loaded = true
        GgtgAi.readText(file())?.let { text ->
            try {
                val o = JSONObject(text)
                for (k in o.keys()) {
                    val bytes = Base64.decode(o.getString(k), Base64.NO_WRAP)
                    val floats = FloatArray(bytes.size / 4)
                    ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN).asFloatBuffer().get(floats)
                    vectors[k] = floats
                }
            } catch (t: Throwable) { }
        }
    }

    @Synchronized private fun saveCache() {
        if (!dirty) return
        dirty = false
        if (vectors.size > 6000) vectors.clear()
        val o = JSONObject()
        for ((k, v) in vectors) {
            val bb = ByteBuffer.allocate(v.size * 4).order(ByteOrder.LITTLE_ENDIAN)
            bb.asFloatBuffer().put(v)
            o.put(k, Base64.encodeToString(bb.array(), Base64.NO_WRAP))
        }
        GgtgAi.writeText(file(), o.toString())
    }

    private fun geminiKey(): GgtgAi.CloudKey? =
        GgtgAi.settings().keys.firstOrNull { it.provider == "gemini" && !GgtgAi.resting(it.key, MODEL) }

    fun available(): Boolean = GgtgAi.settings().enabled && geminiKey() != null

    @Synchronized fun cached(text: String): FloatArray? { ensureLoaded(); return vectors[keyOf(norm(text))] }

    fun similarity(a: FloatArray, b: FloatArray): Double {
        if (a.size != b.size || a.isEmpty()) return 0.0
        var sum = 0.0
        for (i in a.indices) sum += a[i].toDouble() * b[i]
        return sum
    }

    /** Makes sure every text has its vector (only the missing ones are asked for, fifty at a time). */
    suspend fun prepare(texts: List<String>): Boolean {
        ensureLoaded()
        val missing = LinkedHashSet<String>()
        for (t in texts) { val n = norm(t); if (n.isNotEmpty() && cached(n) == null) missing.add(n) }
        if (missing.isEmpty()) return true
        val queue = ArrayDeque(missing.toList())
        while (queue.isNotEmpty()) {
            val key = geminiKey()
            if (key == null) { saveCache(); return false }
            val batch = ArrayList<String>()
            while (batch.size < 50 && queue.isNotEmpty()) batch.add(queue.removeFirst())
            val input = JSONArray()
            for (t in batch) input.put(GgtgAi.mask(t).text)
            val body = JSONObject().put("input", input).put("model", MODEL)
            val r = GgtgAi.http("POST", GgtgAi.provider("gemini")!!.baseUrl + "/embeddings",
                mapOf("Authorization" to "Bearer " + key.key.trim()), body.toString(), 60_000)
            val rows = try { JSONObject(r.body).optJSONArray("data") } catch (t: Throwable) { null }
            if (r.status !in 200..299 || rows == null || rows.length() != batch.size) {
                GgtgAi.failed(GgtgAi.Candidate(key, MODEL, 0, 0), r.status, r.body, r.retryAfter)
                saveCache()
                return false
            }
            synchronized(this) {
                for (i in batch.indices) {
                    val numbers = rows.getJSONObject(i).optJSONArray("embedding") ?: continue
                    val n = minOf(numbers.length(), DIMENSIONS)
                    val v = FloatArray(n) { numbers.getDouble(it).toFloat() }
                    var sum = 0.0
                    for (x in v) sum += x.toDouble() * x
                    if (sum > 0) { val scale = (1.0 / sqrt(sum)).toFloat(); for (j in v.indices) v[j] *= scale }
                    vectors[keyOf(batch[i])] = v
                    dirty = true
                }
            }
        }
        saveCache()
        return true
    }

    fun clear() { synchronized(this) { vectors.clear(); dirty = false }; file().delete() }
}
