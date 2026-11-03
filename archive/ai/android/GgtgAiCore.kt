package app.ggtg.ai

import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.sync.Semaphore
import kotlinx.coroutines.sync.withPermit
import kotlinx.coroutines.withContext
import org.json.JSONArray
import org.json.JSONObject
import org.telegram.messenger.ApplicationLoader
import java.io.File
import java.net.HttpURLConnection
import java.net.URL
import java.util.concurrent.ConcurrentHashMap

// ggtg: the AI of the desktop app (core/ggtg_ai.h), on the phone. Cloud models only: Google Gemini,
// Groq, OpenRouter, GitHub Models and NVIDIA NIM all give a free key without a card. The user pastes
// any number of keys, the provider is told by the key itself. Every job goes to the models that
// suit it (Task): replies to people to the cleverest, bulk sorting to the fast ones with big
// limits, pictures to the ones that see. A model (or a whole key) that hits a limit rests and the
// next one takes the job. The memory, the knowledge and the style live on the phone, so a change of
// provider loses nothing. Bulk jobs carry other people's messages: names, numbers, @usernames,
// mails and links leave as marks and are put back into the answer.
object GgtgAi {

    enum class Task { Smart, Bulk, Vision }

    data class CloudKey(val key: String, val provider: String)

    class Provider(
        val id: String,
        val name: String,
        val baseUrl: String,
        val keyUrl: String,
        val note: String,
        val prefixes: List<String>
    )

    // Only services that give a key without a card (checked in October 2026). Gone: GitHub Models (closed on July 30, 2026),
    // SambaNova and Cerebras (both want a card now).
    val providers = listOf(
        Provider("gemini", "Google Gemini", "https://generativelanguage.googleapis.com/v1beta/openai",
            "https://aistudio.google.com/apikey", "самый умный, видит картинки. Нужен только аккаунт Google", listOf("AIza")),
        Provider("groq", "Groq", "https://api.groq.com/openai/v1",
            "https://console.groq.com/keys", "очень быстрый, большие лимиты. Вход через Google или почту", listOf("gsk_")),
        Provider("openrouter", "OpenRouter", "https://openrouter.ai/api/v1",
            "https://openrouter.ai/keys", "десятки бесплатных моделей, около 50 запросов в день", listOf("sk-or-")),
        Provider("nvidia", "NVIDIA", "https://integrate.api.nvidia.com/v1",
            "https://build.nvidia.com/settings/api-keys", "большие открытые модели (DeepSeek, Qwen). Просит подтвердить телефон, это не всегда проходит", listOf("nvapi-")),
        Provider("llm7", "LLM7", "https://api.llm7.io/v1",
            "https://token.llm7.io/", "работает совсем без регистрации. Запасной вариант, сервис сторонний", listOf("unused"))
    )

    fun provider(id: String): Provider? = providers.firstOrNull { it.id == id }
    fun providerName(id: String) = provider(id)?.name ?: "неизвестный сервис"

    /** The provider a key belongs to, by how it starts; null when unknown. */
    fun detectProvider(key: String): String? {
        val trimmed = key.trim()
        return providers.firstOrNull { p -> p.prefixes.any { trimmed.startsWith(it) } }?.id
    }

    fun shortKey(key: String): String {
        val t = key.trim()
        return if (t.length <= 12) t else t.take(6) + "…" + t.takeLast(4)
    }

    // ---- settings (ggtg_bulk_db/ai_settings.json) ----

    class Settings {
        var enabled = true
        var chatPrompt = ""
        var inboxAuto = true
        var briefDaily = true
        var trainNightly = false
        // chats stay marked "unread" after what the program does in them (a skipped answer, a reply from the feed)
        var keepUnread = true
        var keys = ArrayList<CloudKey>()
    }

    fun dir(): File = File(ApplicationLoader.getFilesDirFixed(), "ggtg_bulk_db").also { it.mkdirs() }

    internal fun readText(file: File): String? = try { if (file.exists()) file.readText() else null } catch (t: Throwable) { null }

    internal fun writeText(file: File, text: String) {
        val tmp = File(file.parentFile, file.name + ".tmp")
        tmp.writeText(text)
        if (!tmp.renameTo(file)) { file.writeText(text); tmp.delete() }
    }

    private val settingsLock = Any()
    @Volatile private var cached: Settings? = null
    private val changeListeners = java.util.concurrent.CopyOnWriteArrayList<() -> Unit>()
    fun addListener(l: () -> Unit) { changeListeners.add(l) }
    fun removeListener(l: () -> Unit) { changeListeners.remove(l) }

    fun settings(): Settings = cached ?: synchronized(settingsLock) {
        cached ?: load().also { cached = it }
    }

    private fun load(): Settings {
        val s = Settings()
        val text = readText(File(dir(), "ai_settings.json")) ?: return s
        try {
            val o = JSONObject(text)
            s.enabled = o.optBoolean("enabled", true)
            s.chatPrompt = o.optString("chatPrompt")
            s.inboxAuto = o.optBoolean("inboxAuto", true)
            s.briefDaily = o.optBoolean("briefDaily", true)
            s.keepUnread = o.optBoolean("keepUnread", true)
            s.trainNightly = o.optBoolean("trainNightly", false)
            val arr = o.optJSONArray("keys") ?: JSONArray()
            for (i in 0 until arr.length()) {
                val e = arr.getJSONObject(i)
                val key = e.optString("key").trim()
                if (key.isEmpty() || s.keys.any { it.key == key }) continue
                val provider = detectProvider(key) ?: e.optString("provider")
                // keys of the services that closed or want a card now can not answer any more
                if (provider == "github" || provider == "sambanova" || provider == "cerebras") continue
                s.keys.add(CloudKey(key, provider))
            }
        } catch (t: Throwable) { }
        return s
    }

    fun save() {
        val s = settings()
        val o = JSONObject()
        o.put("enabled", s.enabled)
        o.put("chatPrompt", s.chatPrompt)
        o.put("inboxAuto", s.inboxAuto)
        o.put("briefDaily", s.briefDaily)
        o.put("keepUnread", s.keepUnread)
        o.put("trainNightly", s.trainNightly)
        val arr = JSONArray()
        for (k in s.keys) arr.put(JSONObject().put("key", k.key).put("provider", k.provider))
        o.put("keys", arr)
        synchronized(settingsLock) { writeText(File(dir(), "ai_settings.json"), o.toString()) }
        changeListeners.forEach { runCatching { it() } }
    }

    fun keyUsable(k: CloudKey) = k.key.isNotBlank() && provider(k.provider) != null

    /** The AI is on and has at least one key it can use. */
    fun ready(): Boolean = settings().let { s -> s.enabled && s.keys.any { keyUsable(it) } }

    // ---- which model for which job ----

    private class Choice(val provider: String, val task: Task, val score: Int, val pattern: String, val fallback: String)

    private val choices = listOf(
        // Smart: the cleverest that answers.
        Choice("gemini", Task.Smart, 100, "^gemini-[\\d.]+-pro$", "gemini-2.5-pro"),
        Choice("gemini", Task.Smart, 92, "^gemini-flash-latest$", "gemini-flash-latest"),
        Choice("gemini", Task.Smart, 90, "^gemini-[\\d.]+-flash$", "gemini-2.5-flash"),
        Choice("nvidia", Task.Smart, 86, "^nvidia/llama-3\\.1-nemotron-70b-instruct", "nvidia/llama-3.1-nemotron-70b-instruct"),
        Choice("groq", Task.Smart, 84, "^moonshotai/kimi-k2", "moonshotai/kimi-k2-instruct"),
        Choice("groq", Task.Smart, 83, "^openai/gpt-oss-120b$", "openai/gpt-oss-120b"),
        Choice("nvidia", Task.Smart, 82, "^mistralai/mistral-large-2-instruct", "mistralai/mistral-large-2-instruct"),
        Choice("groq", Task.Smart, 80, "^llama-3\\.3-70b-versatile$", "llama-3.3-70b-versatile"),
        Choice("nvidia", Task.Smart, 78, "^nvidia/llama-3\\.1-nemotron-51b-instruct", "nvidia/llama-3.1-nemotron-51b-instruct"),
        Choice("llm7", Task.Smart, 72, "^DeepSeek-V4-Flash", "DeepSeek-V4-Flash-0731"),
        Choice("llm7", Task.Smart, 70, "^GLM-5\\.3-Flash$", "GLM-5.3-Flash"),
        // Bulk: fast, with the biggest free limits.
        Choice("gemini", Task.Bulk, 95, "^gemini-flash-lite-latest$", "gemini-flash-lite-latest"),
        Choice("gemini", Task.Bulk, 94, "^gemini-[\\d.]+-flash-lite$", "gemini-2.5-flash-lite"),
        Choice("groq", Task.Bulk, 93, "^openai/gpt-oss-20b$", "openai/gpt-oss-20b"),
        Choice("groq", Task.Bulk, 92, "^llama-3\\.1-8b-instant$", "llama-3.1-8b-instant"),
        Choice("groq", Task.Bulk, 88, "^llama-3\\.3-70b-versatile$", "llama-3.3-70b-versatile"),
        Choice("gemini", Task.Bulk, 86, "^gemini-[\\d.]+-flash$", "gemini-2.5-flash"),
        Choice("nvidia", Task.Bulk, 85, "^nvidia/llama-3\\.1-nemotron-70b-instruct", "nvidia/llama-3.1-nemotron-70b-instruct"),
        Choice("nvidia", Task.Bulk, 82, "^mistralai/mistral-7b-instruct-v0\\.3", "mistralai/mistral-7b-instruct-v0.3"),
        Choice("nvidia", Task.Bulk, 80, "^openai/gpt-oss-20b", "openai/gpt-oss-20b"),
        Choice("nvidia", Task.Bulk, 78, "^nvidia/llama-3\\.1-nemotron-51b-instruct", "nvidia/llama-3.1-nemotron-51b-instruct"),
        Choice("llm7", Task.Bulk, 66, "^gpt-oss:20b$", "gpt-oss:20b"),
        // Vision: models that read pictures.
        Choice("gemini", Task.Vision, 95, "^gemini-flash-latest$", "gemini-flash-latest"),
        Choice("gemini", Task.Vision, 94, "^gemini-[\\d.]+-flash$", "gemini-2.5-flash"),
        Choice("gemini", Task.Vision, 88, "^gemini-[\\d.]+-flash-lite$", "gemini-2.5-flash-lite"),
        Choice("groq", Task.Vision, 84, "^meta-llama/llama-4-maverick", "meta-llama/llama-4-maverick-17b-128e-instruct"),
        Choice("groq", Task.Vision, 83, "^meta-llama/llama-4-scout", "meta-llama/llama-4-scout-17b-16e-instruct"),
        Choice("nvidia", Task.Vision, 72, "^meta/llama-3\\.2-11b-vision-instruct", "meta/llama-3.2-11b-vision-instruct"),
        Choice("nvidia", Task.Vision, 70, "^meta/llama-3\\.2-90b-vision-instruct", "meta/llama-3.2-90b-vision-instruct")
    )

    private fun openRouterBase(task: Task) = when (task) { Task.Smart -> 85; Task.Bulk -> 70; Task.Vision -> 75 }

    // ---- OpenRouter's free models: the list is downloaded once a day ----

    class FreeModel(val id: String, val vision: Boolean, val tools: Boolean)

    @Volatile private var freeModels: List<FreeModel>? = null
    @Volatile private var freeAt = 0L
    @Volatile private var freeFetching = false

    private fun openRouterRank(id: String): Int {
        var biggest = 0.0
        for (m in Regex("(\\d+(?:\\.\\d+)?)b(?![a-z])", RegexOption.IGNORE_CASE).findAll(id)) {
            biggest = maxOf(biggest, m.groupValues[1].toDouble())
        }
        var rank = (minOf(biggest, 700.0) / 10.0).toInt()
        val families = listOf("deepseek" to 40, "kimi" to 38, "qwen3" to 30, "gpt-oss" to 30, "glm" to 28, "llama-4" to 25,
            "llama-3.3" to 22, "nemotron" to 22, "qwen" to 20, "gemma" to 15)
        for ((name, bonus) in families) if (id.contains(name, true)) { rank += bonus; break }
        return rank
    }

    fun openRouterFree(): List<FreeModel> {
        freeModels?.let { return it }
        val text = readText(File(dir(), "ai_openrouter_free.json"))
        if (text != null) try {
            val o = JSONObject(text)
            freeAt = o.optLong("at")
            val arr = o.optJSONArray("models") ?: JSONArray()
            freeModels = (0 until arr.length()).map {
                val e = arr.getJSONObject(it)
                FreeModel(e.optString("id"), e.optBoolean("vision"), e.optBoolean("tools"))
            }
        } catch (t: Throwable) { }
        return freeModels ?: emptyList()
    }

    /** Downloads OpenRouter's free list when it is older than a day (or [force]). */
    suspend fun refreshOpenRouterFree(force: Boolean): Boolean {
        val have = openRouterFree()
        if (!force && have.isNotEmpty() && System.currentTimeMillis() - freeAt < 24 * 3600 * 1000L) return true
        if (freeFetching) return have.isNotEmpty()
        freeFetching = true
        try {
            val r = http("GET", "https://openrouter.ai/api/v1/models", emptyMap(), null, 30_000)
            if (r.status != 200) return have.isNotEmpty()
            val bad = Regex("embed|guard|tts|whisper|audio|image-gen|moderation|rerank|^openrouter/", RegexOption.IGNORE_CASE)
            val rows = JSONObject(r.body).optJSONArray("data") ?: return have.isNotEmpty()
            val list = ArrayList<FreeModel>()
            for (i in 0 until rows.length()) {
                val row = rows.getJSONObject(i)
                val id = row.optString("id")
                val pricing = row.optJSONObject("pricing")
                val free = id.endsWith(":free") || (pricing != null && pricing.optString("prompt") == "0" && pricing.optString("completion") == "0")
                val arch = row.optJSONObject("architecture")
                val outs = arch?.optJSONArray("output_modalities")
                var textOut = outs == null || outs.length() == 0
                if (outs != null) for (j in 0 until outs.length()) if (outs.optString(j) == "text") textOut = true
                if (id.isEmpty() || !free || bad.containsMatchIn(id) || !textOut) continue
                var vision = false
                val ins = arch?.optJSONArray("input_modalities")
                if (ins != null) for (j in 0 until ins.length()) if (ins.optString(j) == "image") vision = true
                var tools = false
                val sp = row.optJSONArray("supported_parameters")
                if (sp != null) for (j in 0 until sp.length()) if (sp.optString(j) == "tools") tools = true
                list.add(FreeModel(id, vision, tools))
            }
            if (list.isEmpty()) return have.isNotEmpty()
            list.sortByDescending { openRouterRank(it.id) }
            freeModels = list
            freeAt = System.currentTimeMillis()
            val arr = JSONArray()
            for (m in list) arr.put(JSONObject().put("id", m.id).put("vision", m.vision).put("tools", m.tools))
            writeText(File(dir(), "ai_openrouter_free.json"), JSONObject().put("at", freeAt).put("models", arr).toString())
            return true
        } catch (t: Throwable) {
            return have.isNotEmpty()
        } finally {
            freeFetching = false
        }
    }

    // ---- each key's own list of models (asked once per run) ----

    private class KeyModels(var ids: List<String> = emptyList(), var known: Boolean = false, var tried: Long = 0, var fetching: Boolean = false)
    private val modelLists = ConcurrentHashMap<String, KeyModels>()

    private suspend fun fetchModels(key: CloudKey): Int {
        val entry = modelLists.getOrPut(key.key) { KeyModels() }
        if (entry.fetching) return 0
        entry.fetching = true
        entry.tried = System.currentTimeMillis()
        try {
            if (key.provider == "openrouter") {
                val ok = refreshOpenRouterFree(false)
                entry.ids = openRouterFree().map { it.id }
                entry.known = ok
                return if (ok) 200 else 0
            }
            val p = provider(key.provider) ?: return 0
            val url = if (key.provider == "github") "https://models.github.ai/catalog/models" else p.baseUrl + "/models"
            val r = http("GET", url, mapOf("Authorization" to "Bearer " + key.key.trim()), null, 30_000)
            if (r.status !in 200..299) return r.status
            val text = r.body.trim()
            val arr: JSONArray = if (text.startsWith("[")) JSONArray(text) else {
                val o = JSONObject(text)
                o.optJSONArray("data") ?: o.optJSONArray("models") ?: JSONArray()
            }
            val ids = ArrayList<String>()
            for (i in 0 until arr.length()) {
                val o = arr.optJSONObject(i) ?: continue
                var id = o.optString("id").ifEmpty { o.optString("name") }
                if (id.startsWith("models/")) id = id.substring(7)
                if (id.isNotEmpty()) ids.add(id)
            }
            if (ids.isNotEmpty()) { entry.ids = ids; entry.known = true }
            return r.status
        } catch (t: Throwable) {
            return 0
        } finally {
            entry.fetching = false
        }
    }

    // ---- resting: a model (or a whole key) that failed waits a while ----

    private val rests = ConcurrentHashMap<String, Long>() // "<key>\n<model>" or "<key>\n"

    private fun restUntil(key: String, model: String) = rests[key + "\n" + model] ?: 0L

    fun resting(key: String, model: String): Boolean {
        val now = System.currentTimeMillis()
        return restUntil(key, "") > now || (model.isNotEmpty() && restUntil(key, model) > now)
    }

    private fun rest(key: String, model: String, seconds: Int) { rests[key + "\n" + model] = System.currentTimeMillis() + seconds * 1000L }

    fun wake(key: String) { rests.keys.filter { it.startsWith("$key\n") }.forEach { rests.remove(it) } }

    fun keyResting(key: String) = restUntil(key, "") > System.currentTimeMillis()

    /** "через 3 мин" until the first resting key or model of the usable keys is back; "" when none rests. */
    fun soonestBack(): String {
        val now = System.currentTimeMillis()
        var soonest = 0L
        for ((name, until) in rests) {
            if (until <= now) continue
            if (settings().keys.any { name.startsWith(it.key + "\n") } && (soonest == 0L || until < soonest)) soonest = until
        }
        if (soonest == 0L) return ""
        val minutes = (soonest - now + 59_999) / 60_000
        return if (minutes >= 60) "через ${(minutes + 59) / 60} ч" else "через $minutes мин"
    }

    class Candidate(val key: CloudKey, val model: String, val score: Int, val order: Int)

    private fun newest(ids: List<String>, pattern: String): String? {
        val re = Regex(pattern)
        return ids.filter { re.containsMatchIn(it) }.maxOrNull()
    }

    fun candidates(task: Task, tools: Boolean): List<Candidate> {
        val result = ArrayList<Candidate>()
        fun add(c: Candidate) {
            if (resting(c.key.key, c.model)) return
            val i = result.indexOfFirst { it.key.key == c.key.key && it.model == c.model }
            if (i >= 0) { if (c.score > result[i].score) result[i] = c; return }
            result.add(c)
        }
        val keys = settings().keys
        for ((order, key) in keys.withIndex()) {
            if (!keyUsable(key) || keyResting(key.key)) continue
            if (key.provider == "openrouter") {
                var place = 0
                for (m in openRouterFree()) {
                    if ((task == Task.Vision && !m.vision) || (tools && !m.tools)) continue
                    add(Candidate(key, m.id, openRouterBase(task) * 100 - place++, order))
                }
                continue
            }
            val list = modelLists[key.key]
            val known = list?.known == true
            for (c in choices) {
                if (c.provider != key.provider || c.task != task) continue
                val model = if (known) newest(list!!.ids, c.pattern) else c.fallback
                if (!model.isNullOrEmpty()) add(Candidate(key, model, c.score * 100, order))
            }
        }
        return result.sortedWith(compareBy({ -it.score }, { it.order }))
    }

    private fun badKeyText(lower: String) =
        lower.contains("api key not valid") || lower.contains("api_key_invalid") || lower.contains("invalid api key")

    internal fun failed(c: Candidate, status: Int, body: String, retryAfter: String?) {
        val key = c.key.key
        val text = body.lowercase()
        when {
            status == 401 || badKeyText(text) -> rest(key, "", 10 * 3600)
            status == 403 && !text.contains("model") -> rest(key, "", 30 * 60) // blocked: the address, the rights
            status == 403 || status == 404 -> {
                rest(key, c.model, 24 * 3600)
                modelLists[key]?.let { it.known = false; it.tried = 0 }
            }
            status == 429 -> {
                val daily = text.contains("per day") || text.contains("perday") || text.contains("daily") ||
                    text.contains("per-day") || text.contains("limit: 0")
                if (daily && c.key.provider == "openrouter") rest(key, "", 6 * 3600)
                else if (daily) rest(key, c.model, 6 * 3600)
                else {
                    val seconds = retryAfter?.trim()?.toIntOrNull() ?: 0
                    rest(key, c.model, if (seconds > 0) seconds.coerceIn(20, 3600) else 300)
                }
            }
            status == 400 || status == 413 || status == 422 -> rest(key, c.model, 600)
            else -> rest(key, c.model, 60)
        }
    }

    internal fun failureText(status: Int, body: String, fallback: String): String {
        val lower = body.lowercase()
        var message = try {
            val o = JSONObject(body)
            o.optJSONObject("error")?.optString("message").orEmpty().ifEmpty { o.optString("message") }
        } catch (t: Throwable) { "" }
        if (message.isEmpty() && !body.trimStart().startsWith("<")) message = body
        message = message.replace(Regex("\\s+"), " ").trim().take(200)
        val region = listOf("location", "region", "country", "not supported", "cloudflare", "<html").any { lower.contains(it) }
        val tail = if (message.isEmpty()) "" else " Ответ сервиса: $message"
        return when {
            status == 401 || badKeyText(lower) -> "ключ не подошёл (401): скопируйте его заново целиком, без пробелов, или создайте новый.$tail"
            status == 403 -> "доступ запрещён (403): ключ без нужных прав или сервис не пускает с вашего адреса (часто это ограничение страны: включите VPN).$tail"
            status == 400 && region -> "сервис не работает из вашей страны или с вашего адреса (включите VPN).$tail"
            status == 404 -> "такой модели нет у сервиса"
            status == 429 -> "бесплатный лимит исчерпан"
            status == 0 -> "нет соединения с интернетом или сервис недоступен"
            message.isEmpty() -> "$fallback (HTTP $status)"
            else -> message
        }
    }

    // ---- http ----

    class HttpResult(val status: Int, val body: String, val retryAfter: String?)

    suspend fun http(method: String, url: String, headers: Map<String, String>, body: String?, timeoutMs: Int): HttpResult =
        withContext(Dispatchers.IO) {
            try {
                val c = URL(url).openConnection() as HttpURLConnection
                c.requestMethod = method
                c.connectTimeout = 20_000
                c.readTimeout = timeoutMs
                for ((k, v) in headers) c.setRequestProperty(k, v)
                if (body != null) {
                    c.doOutput = true
                    c.setRequestProperty("Content-Type", "application/json")
                    c.outputStream.use { it.write(body.toByteArray(Charsets.UTF_8)) }
                }
                val code = c.responseCode
                val stream = if (code in 200..299) c.inputStream else (c.errorStream ?: c.inputStream)
                val text = stream?.bufferedReader(Charsets.UTF_8)?.use { it.readText() } ?: ""
                HttpResult(code, text, c.getHeaderField("Retry-After"))
            } catch (t: Throwable) {
                HttpResult(0, t.message ?: "", null)
            }
        }

    // ---- privacy: other people's data leaves as marks ----

    class Masked(val text: String, val marks: List<Pair<String, String>>)

    private val maskMail = Regex("[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\\.[A-Za-z]{2,}")
    private val maskLink = Regex("(?:https?://|www\\.|t\\.me/|telegram\\.me/)\\S+", RegexOption.IGNORE_CASE)
    private val maskUser = Regex("@[A-Za-z][A-Za-z0-9_]{3,31}")
    private val maskPhone = Regex("(?<![\\w+])\\+?\\d[\\d\\s().-]{5,}\\d(?!\\w)")

    fun mask(text: String): Masked {
        var result = text
        val counters = HashMap<Char, Int>()
        val same = HashMap<String, String>()
        val marks = ArrayList<Pair<String, String>>()
        fun mark(value: String, kind: Char): String = same.getOrPut(value) {
            val n = (counters[kind] ?: 0) + 1
            counters[kind] = n
            "<$kind$n>".also { marks.add(it to value) }
        }
        fun replaceAll(re: Regex, kind: Char, accept: ((String) -> Boolean)? = null) {
            result = re.replace(result) { m -> if (accept == null || accept(m.value)) mark(m.value, kind) else m.value }
        }
        replaceAll(maskMail, 'E')
        replaceAll(maskLink, 'L')
        replaceAll(maskUser, 'U')
        replaceAll(maskPhone, 'P') { v ->
            val digits = v.count { it.isDigit() }
            if (v.startsWith("+")) digits >= 7 else digits >= 10
        }
        return Masked(result, marks)
    }

    fun unmask(text: String, masked: Masked): String {
        var result = text
        for ((name, original) in masked.marks.sortedByDescending { it.first.length }) result = result.replace(name, original)
        return result
    }

    /** The marks left in a text become plain words: nothing real may be kept. */
    fun plainMarks(text: String): String = Regex("<([NPUEL])\\d+>").replace(text) {
        when (it.groupValues[1]) {
            "N" -> "[имя]"; "P" -> "[номер]"; "U" -> "[@юзернейм]"; "E" -> "[почта]"; else -> "[ссылка]"
        }
    }

    /** Names of the person (and the user) hidden by hand before the text leaves the phone. */
    fun hideNames(text: String, names: List<String>): String {
        val words = names.flatMap { Regex("[\\p{L}\\p{N}_]{3,}").findAll(it).map { m -> m.value }.toList() }.distinctBy { it.lowercase() }
        if (words.isEmpty()) return text
        val re = Regex("(?<![\\p{L}\\p{N}_])(?:" + words.joinToString("|") { Regex.escape(it) } + ")(?![\\p{L}\\p{N}_])", RegexOption.IGNORE_CASE)
        return re.replace(text, "<N0>")
    }

    // ---- the call ----

    class Result(val text: String, val error: String?, val message: JSONObject? = null, val note: String = "")

    private val gate = Semaphore(3)

    private fun stripThinking(text: String) = text.replace(Regex("<think>.*?</think>", RegexOption.DOT_MATCHES_ALL), "").trim()

    internal fun tune(body: JSONObject, c: Candidate) {
        var tokens = maxOf(body.optInt("max_tokens", 800), 256)
        val model = c.model
        if (c.key.provider == "gemini") {
            if (model.startsWith("gemini-2.5-flash")) body.put("reasoning_effort", "none")
            else { body.put("reasoning_effort", "low"); tokens += 2048 }
        } else if (model.contains("gpt-oss") && (c.key.provider == "groq" || c.key.provider == "openrouter")) {
            body.put("reasoning_effort", "low"); tokens += 1024
        } else if (Regex("deepseek-r1|-r1\\b|qwq|thinking|reason", RegexOption.IGNORE_CASE).containsMatchIn(model)) {
            tokens += 1536
        }
        body.put("max_tokens", tokens)
    }

    private suspend fun warm() {
        val now = System.currentTimeMillis()
        for (key in settings().keys) {
            if (!keyUsable(key)) continue
            val e = modelLists.getOrPut(key.key) { KeyModels() }
            if (!e.known && !e.fetching && now - e.tried > 10 * 60 * 1000) fetchModels(key)
            else if (key.provider == "openrouter") refreshOpenRouterFree(false)
        }
    }

    /** Sends [body] (everything but "model") to the models that suit [task], the best first, until
     *  one gives a usable answer. */
    suspend fun send(task: Task, tools: Boolean, body: JSONObject, timeoutMs: Int): Result {
        val s = settings()
        if (!s.enabled) return Result("", "ИИ выключен - включите его в разделе «ИИ»")
        if (!ready()) return Result("", "нет ни одного ключа: добавьте бесплатный ключ в разделе «ИИ» → «Ключи»")
        warm()
        var last = ""
        for (attempt in 0 until 8) {
            val candidate = candidates(task, tools).firstOrNull()
            if (candidate == null) {
                val back = soonestBack()
                return Result("", (if (attempt > 0) "не ответила ни одна модель (последняя: $last)" else "все ключи сейчас отдыхают после лимита или ошибки") +
                    (if (back.isEmpty()) "" else "; ближайший освободится $back") + ". Можно добавить ещё ключ в разделе «ИИ» → «Ключи»")
            }
            val p = provider(candidate.key.provider)!!
            val tuned = JSONObject(body.toString())
            tuned.put("model", candidate.model)
            tune(tuned, candidate)
            val headers = HashMap<String, String>()
            headers["Authorization"] = "Bearer " + candidate.key.key.trim()
            if (candidate.key.provider == "openrouter") headers["X-Title"] = "GGTG"
            val r = http("POST", p.baseUrl + "/chat/completions", headers, tuned.toString(), timeoutMs)
            val note = candidate.key.provider + " " + candidate.model
            if (r.status !in 200..299) {
                failed(candidate, r.status, r.body, r.retryAfter)
                last = note + ": " + failureText(r.status, r.body, r.body.take(120))
                continue
            }
            val message = try { JSONObject(r.body).optJSONArray("choices")?.optJSONObject(0)?.optJSONObject("message") } catch (t: Throwable) { null }
            val content = stripThinking(message?.optString("content").orEmpty())
            val calls = if (tools) message?.optJSONArray("tool_calls") else null
            if (content.isEmpty() && (calls == null || calls.length() == 0)) {
                failed(candidate, 0, "", null)
                last = "$note: пустой ответ"
                continue
            }
            return Result(content, null, message, note)
        }
        return Result("", "не ответила ни одна модель (последняя: $last)")
    }

    /** One question. With [images] the models that see are used, with [hard] the cleverest (replies
     *  to people, teaching the AI), otherwise the fast ones - and then names, numbers and usernames in
     *  [user] leave as marks, which come back in the answer unless [keepMarks]. */
    suspend fun ask(
        system: String,
        user: String,
        temperature: Double = 0.6,
        maxTokens: Int = 800,
        images: List<String> = emptyList(),
        hard: Boolean = false,
        keepMarks: Boolean = false
    ): Result = gate.withPermit {
        val task = if (images.isNotEmpty()) Task.Vision else if (hard) Task.Smart else Task.Bulk
        val masked = if (task == Task.Bulk) mask(user) else Masked(user, emptyList())
        val messages = JSONArray()
        if (system.isNotEmpty()) messages.put(JSONObject().put("role", "system").put("content", system))
        if (images.isEmpty()) {
            messages.put(JSONObject().put("role", "user").put("content", masked.text))
        } else {
            val parts = JSONArray()
            for (image in images) parts.put(JSONObject().put("type", "image_url").put("image_url", JSONObject().put("url", image)))
            parts.put(JSONObject().put("type", "text").put("text", masked.text))
            messages.put(JSONObject().put("role", "user").put("content", parts))
        }
        val body = JSONObject().put("messages", messages).put("temperature", temperature).put("max_tokens", maxTokens).put("stream", false)
        val r = send(task, false, body, 120_000)
        log(if (task == Task.Bulk) "bulk" else if (task == Task.Smart) "smart" else "vision", r.error == null, r.note.ifEmpty { r.error.orEmpty() })
        if (r.error != null) return@withPermit r
        var text = if (keepMarks) plainMarks(r.text) else unmask(r.text, masked)
        // A long dash (em dash) gives away a machine-written text: never let it through.
        text = text.replace('—', '–')
        Result(text, null, r.message, r.note)
    }

    /** One step of a conversation with tools (OpenAI format): the model's message comes back, with
     *  "content" or "tool_calls". Always a job for the cleverest models. */
    suspend fun askWithTools(messages: JSONArray, tools: JSONArray, temperature: Double = 0.4, maxTokens: Int = 1400): Result = gate.withPermit {
        val body = JSONObject().put("messages", messages).put("temperature", temperature).put("max_tokens", maxTokens).put("stream", false)
        if (tools.length() > 0) body.put("tools", tools)
        val r = send(Task.Smart, tools.length() > 0, body, 300_000)
        log("agent", r.error == null, r.note.ifEmpty { r.error.orEmpty() })
        r
    }

    /** "Проверить" next to a key: is it the provider's, does it answer; a key that works is back in the queue. */
    suspend fun testKey(rawKey: String, hint: String? = null): Pair<Boolean, String> {
        val key = rawKey.trim()
        if (key.isEmpty()) return false to "Сначала вставьте ключ."
        // A key put into a service's list by hand keeps that service.
        val providerId = detectProvider(key) ?: hint ?: settings().keys.firstOrNull { it.key == key }?.provider
            ?: return false to "Не узнал ключ: ключи Gemini начинаются с AIza, Groq - с gsk_, OpenRouter - с sk-or-, NVIDIA - с nvapi-. Скопируйте ключ целиком."
        wake(key)
        val entry = CloudKey(key, providerId)
        val name = providerName(providerId)
        val status = fetchModels(entry)
        if (status == 401) {
            rest(key, "", 10 * 3600)
            return false to "$name: ключ не подошёл (401). Скопируйте его заново целиком, без пробелов, или создайте новый."
        }
        // The key alone is tried (not the others): a temporary settings copy of one.
        val p = provider(providerId)!!
        val models = candidatesFor(entry)
        var lastError = ""
        for (c in models.take(4)) {
            val body = JSONObject().put("messages", JSONArray().put(JSONObject().put("role", "user").put("content", "Ответь одним словом: готово")))
                .put("max_tokens", 20).put("temperature", 0.0).put("stream", false).put("model", c.model)
            tune(body, c)
            val headers = mapOf("Authorization" to "Bearer $key")
            val r = http("POST", p.baseUrl + "/chat/completions", headers, body.toString(), 30_000)
            if (r.status in 200..299) {
                val count = modelLists[key]?.ids?.size ?: 0
                return true to "$name работает, модель ${c.model}" + (if (count > 0) "; доступно моделей: $count" else "") + "."
            }
            failed(c, r.status, r.body, r.retryAfter)
            lastError = failureText(r.status, r.body, r.body.take(120))
            if (r.status == 401) break
        }
        return false to "$name: " + lastError.ifEmpty { "не ответил" }
    }

    private fun candidatesFor(key: CloudKey): List<Candidate> {
        val list = modelLists[key.key]
        val known = list?.known == true
        val result = ArrayList<Candidate>()
        if (key.provider == "openrouter") {
            for ((i, m) in openRouterFree().withIndex()) result.add(Candidate(key, m.id, 10_000 - i, 0))
            return result
        }
        for (c in choices) {
            if (c.provider != key.provider || c.task != Task.Bulk) continue
            var model = if (known) newest(list!!.ids, c.pattern) else c.fallback
            if (model.isNullOrEmpty()) model = c.fallback
            if (!model.isNullOrEmpty()) result.add(Candidate(key, model, c.score, 0))
        }
        if (result.isEmpty() && known && !list!!.ids.isNullOrEmpty()) {
            val fallbackModel = list.ids.firstOrNull { it.contains("nemotron") || it.contains("mistral-7b") || it.contains("gpt-oss") }
            if (fallbackModel != null) {
                result.add(Candidate(key, fallbackModel, 50, 0))
            }
        }
        return result.sortedByDescending { it.score }
    }

    // ---- the log (ggtg_bulk_db/ai_log.jsonl) ----

    private fun log(role: String, ok: Boolean, note: String) {
        try {
            val file = File(dir(), "ai_log.jsonl")
            if (file.length() > 2 * 1024 * 1024) file.delete()
            file.appendText(JSONObject().put("at", System.currentTimeMillis() / 1000).put("role", role).put("ok", ok)
                .put("note", note.take(160)).toString() + "\n")
        } catch (t: Throwable) { }
    }

    /** One line for the "ИИ" screen: how many keys of which providers, how many rest. */
    fun statusText(): String {
        val usable = settings().keys.filter { keyUsable(it) }
        if (usable.isEmpty()) return "Нет ни одного ключа: добавьте бесплатный ключ в шаге «1. Ключи»"
        val counts = LinkedHashMap<String, Int>()
        for (k in usable) counts[providerName(k.provider)] = (counts[providerName(k.provider)] ?: 0) + 1
        val parts = counts.entries.joinToString(", ") { if (it.value > 1) "${it.key} ×${it.value}" else it.key }
        var text = "Ключей: ${usable.size} ($parts)"
        val resting = usable.count { keyResting(it.key) }
        if (resting > 0) {
            val back = soonestBack()
            text += ". Отдыхают: $resting" + if (back.isEmpty()) "" else ", первый вернётся $back"
        }
        return text
    }

    /** Waits (up to [seconds]) while every key rests, in small steps; returns false when stopped. */
    suspend fun waitForKeys(seconds: Int, stopped: () -> Boolean): Boolean {
        var left = seconds
        while (left > 0) {
            if (stopped()) return false
            delay(5_000)
            left -= 5
        }
        return !stopped()
    }
}
