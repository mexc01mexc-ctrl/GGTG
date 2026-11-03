package app.ggtg.ai

import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.net.Uri
import android.provider.OpenableColumns
import android.util.Base64
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.json.JSONArray
import org.json.JSONObject
import java.io.ByteArrayOutputStream
import java.io.File
import java.security.MessageDigest
import java.util.zip.ZipInputStream
import kotlin.math.ln

// ggtg: one knowledge base, "Материалы" (core/ggtg_ai_knowledge.h). It takes PDF, Word, text files, pictures
// (the text on them is read), links to sites and YouTube (a video is taken by its subtitles; when there
// are none, Gemini watches it itself). Everything is cut into chunks of about a paragraph; the ones
// that fit the conversation (by meaning when there is a Gemini key, by words otherwise) are put in
// front of the model with every reply. The base lives in ggtg_bulk_db/ai_knowledge/, one file per source.
object GgtgAiKnowledge {

    class Source(
        val id: String,
        val name: String,
        val kind: String, // file, pdf, image, web, youtube, text, playbook
        val origin: String,
        val added: Long,
        val chunks: List<String>
    )

    private const val CHUNK = 900
    private const val LONG_PIECE = 1200
    private const val MAX_CHUNKS = 800
    private const val MAX_TEXT = 400_000
    private const val MAX_INLINE = 18 * 1024 * 1024

    private val lock = Any()
    private var sources: MutableList<Source>? = null

    private fun dir() = File(GgtgAi.dir(), "ai_knowledge").also { it.mkdirs() }

    fun idOf(origin: String): String {
        val bytes = MessageDigest.getInstance("SHA-1").digest(origin.toByteArray(Charsets.UTF_8))
        return bytes.joinToString("") { "%02x".format(it) }.take(16)
    }

    private fun load(): MutableList<Source> {
        sources?.let { return it }
        val list = ArrayList<Source>()
        for (f in dir().listFiles { _, n -> n.endsWith(".json") }.orEmpty()) {
            try {
                val o = JSONObject(f.readText())
                val arr = o.optJSONArray("chunks") ?: continue
                val chunks = (0 until arr.length()).map { arr.getString(it) }
                if (chunks.isEmpty()) continue
                list.add(Source(f.nameWithoutExtension, o.optString("name"), o.optString("kind"), o.optString("origin"),
                    o.optLong("added"), chunks))
            } catch (t: Throwable) { }
        }
        list.sortWith(compareBy({ it.added }, { it.name }))
        sources = list
        return list
    }

    fun list(): List<Source> = synchronized(lock) { load().toList() }

    fun chunkCount(): Int = list().sumOf { it.chunks.size }

    fun allChunks(): List<String> = list().flatMap { it.chunks }

    fun allText(limit: Int = 300_000): String {
        val sb = StringBuilder()
        for (s in list()) for (c in s.chunks) { if (sb.length >= limit) return sb.toString(); sb.append(c).append('\n') }
        return sb.toString()
    }

    fun remove(id: String) {
        synchronized(lock) {
            load().removeAll { it.id == id }
            File(dir(), "$id.json").delete()
        }
    }

    private fun save(source: Source) {
        synchronized(lock) {
            val list = load()
            list.removeAll { it.id == source.id }
            val arr = JSONArray()
            source.chunks.forEach { arr.put(it) }
            GgtgAi.writeText(File(dir(), source.id + ".json"), JSONObject()
                .put("name", source.name).put("kind", source.kind).put("origin", source.origin)
                .put("added", source.added).put("chunks", arr).toString())
            list.add(source)
        }
    }

    fun kindTitle(kind: String) = when (kind) {
        "playbook" -> "Из ваших чатов"; "pdf" -> "PDF"; "image" -> "Картинка"; "web" -> "Сайт"
        "youtube" -> "YouTube"; "text" -> "Текст"; else -> "Файл"
    }

    // ---- chunks ----

    /** A text cut into chunks of about [CHUNK] characters on paragraph and sentence borders; a short
     *  heading goes together with the text under it. */
    fun split(textRaw: String): List<String> {
        val text = textRaw.replace("\r\n", "\n").replace('\r', '\n').replace(' ', ' ')
        val pieces = ArrayList<String>()
        fun addLong(run: String) {
            var current = StringBuilder()
            fun flush() { if (current.isNotBlank()) pieces.add(current.toString().trim()); current = StringBuilder() }
            for (part in run.split(Regex("(?<=[.!?…。])\\s+")).filter { it.isNotEmpty() }) {
                if (part.length > LONG_PIECE) {
                    flush()
                    var rest = part
                    while (rest.length > LONG_PIECE) {
                        var cut = rest.lastIndexOf(' ', CHUNK)
                        if (cut < CHUNK / 2) cut = CHUNK
                        pieces.add(rest.take(cut).trim())
                        rest = rest.substring(cut)
                    }
                    if (rest.isNotBlank()) pieces.add(rest.trim())
                    continue
                }
                if (current.isNotEmpty() && current.length + part.length > CHUNK) flush()
                if (current.isNotEmpty()) current.append(' ')
                current.append(part)
            }
            flush()
        }
        for (raw in text.split(Regex("\\n\\s*\\n")).filter { it.isNotBlank() }) {
            val paragraph = raw.trim()
            if (paragraph.length <= LONG_PIECE) pieces.add(paragraph)
            else if (paragraph.contains('\n')) {
                var current = StringBuilder()
                for (line in paragraph.split('\n')) {
                    if (line.length > LONG_PIECE) {
                        if (current.isNotEmpty()) { pieces.add(current.toString()); current = StringBuilder() }
                        addLong(line)
                        continue
                    }
                    if (current.isNotEmpty() && current.length + line.length > CHUNK) { pieces.add(current.toString()); current = StringBuilder() }
                    if (current.isNotEmpty()) current.append('\n')
                    current.append(line)
                }
                if (current.isNotBlank()) pieces.add(current.toString())
            } else addLong(paragraph)
        }
        val result = ArrayList<String>()
        var current = ""
        var last = ""
        for (piece in pieces) {
            if (current.isNotEmpty() && current.length + piece.length + 1 > CHUNK) {
                result.add(current)
                val heading = last.length < 120 && !last.endsWith(".") && !last.endsWith("!") && !last.endsWith("?") && !last.endsWith(":")
                current = if (heading) last else ""
            }
            current += (if (current.isEmpty()) "" else "\n") + piece
            last = piece
        }
        if (current.isNotEmpty()) result.add(current)
        if (result.size > 1) result.removeAll { it.length < 30 }
        while (result.size > MAX_CHUNKS) result.removeAt(result.size - 1)
        return result
    }

    // ---- finding what fits the conversation ----

    private val stop = setOf("что", "как", "это", "для", "при", "или", "все", "его", "она", "они", "мне", "вас", "нас", "так",
        "уже", "еще", "ещё", "был", "the", "and", "for", "you", "are", "not", "but", "with", "that", "this", "have")

    private fun stems(text: String): List<String> =
        Regex("[\\p{L}\\p{N}]{3,}").findAll(text.lowercase()).map { it.value }.filter { it !in stop }.map { it.take(5) }.toList()

    /** Makes the meaning vector of the query (and of any chunk without one) ready; never waits long. */
    suspend fun warm(query: String) {
        if (!GgtgAiEmbed.available() || query.isBlank() || chunkCount() == 0) return
        kotlinx.coroutines.withTimeoutOrNull(8_000) { GgtgAiEmbed.prepare(listOf(query) + allChunks()) }
    }

    /** What the assistant is told from the base, at most [limit] characters: the chunks closest to
     *  [query], grouped by source; without a query or when nothing matches, the beginning of each source. */
    fun block(limit: Int, query: String = ""): String {
        val all = list()
        if (all.isEmpty()) return ""
        class E(val source: Int, val index: Int, var score: Double)
        val entries = ArrayList<E>()
        for ((s, src) in all.withIndex()) for (i in src.chunks.indices) entries.add(E(s, i, 0.0))
        val wanted = stems(query).toSet()
        if (wanted.isNotEmpty()) {
            val sets = entries.map { e -> stems(all[e.source].chunks[e.index]).filter { it in wanted }.toSet() }
            val df = HashMap<String, Int>()
            for (set in sets) for (s in set) df[s] = (df[s] ?: 0) + 1
            val idf = df.mapValues { ln(1.0 + entries.size.toDouble() / it.value) }
            val idfSum = idf.values.sum()
            val qv = GgtgAiEmbed.cached(query)
            for ((i, e) in entries.withIndex()) {
                var words = sets[i].sumOf { idf[it] ?: 0.0 }
                if (idfSum > 0) words /= idfSum
                var meaning = -1.0
                if (qv != null) GgtgAiEmbed.cached(all[e.source].chunks[e.index])?.let { meaning = GgtgAiEmbed.similarity(qv, it).coerceIn(0.0, 1.0) }
                e.score = if (meaning >= 0) 0.7 * meaning + 0.3 * words else words
            }
            entries.sortByDescending { it.score }
        }
        if (wanted.isEmpty() || entries.first().score <= 0.0) {
            val each = maxOf(limit / all.size, 400)
            return all.joinToString("") { "• ${it.name}: ${it.chunks.first().take(each).trim()}\n" }
        }
        val top = entries.first().score
        val picked = ArrayList<E>()
        var used = 0
        for (e in entries) {
            val chunk = all[e.source].chunks[e.index]
            val size = chunk.length + all[e.source].name.length + 4
            if (e.score < top * 0.5 || (picked.isNotEmpty() && used + size > limit)) break
            picked.add(e); used += size
        }
        picked.sortWith(compareBy({ it.source }, { it.index }))
        return picked.joinToString("") { "• ${all[it.source].name}: ${all[it.source].chunks[it.index].trim()}\n" }
    }

    // ---- reading the sources ----

    private suspend fun ingest(name: String, kind: String, origin: String, textRaw: String, progress: (String) -> Unit): String? {
        val text = textRaw.trim().take(MAX_TEXT)
        val chunks = split(text)
        if (chunks.isEmpty()) return "в «$name» не нашлось текста"
        save(Source(idOf(origin), name, kind, origin, System.currentTimeMillis() / 1000, chunks))
        if (GgtgAiEmbed.available()) {
            progress("Строю смысловой индекс (${chunks.size} кусков)...")
            GgtgAiEmbed.prepare(chunks)
        }
        return null
    }

    /** The source that is not added by hand: what was learned from the user's own chats. Empty = removed. */
    suspend fun setLearned(name: String, chunks: List<String>) {
        val id = idOf("learned:playbook")
        if (chunks.isEmpty()) { remove(id); return }
        val c = chunks.take(MAX_CHUNKS)
        save(Source(id, name, "playbook", "learned:playbook", System.currentTimeMillis() / 1000, c))
        if (GgtgAiEmbed.available()) GgtgAiEmbed.prepare(c)
    }

    /** Gemini's own interface: it takes a PDF as it is and watches a YouTube video by its address. */
    private suspend fun geminiNative(parts: JSONArray, maxTokens: Int, timeoutMs: Int): Pair<String?, String?> {
        for (attempt in 0 until 4) {
            val candidate = GgtgAi.candidates(GgtgAi.Task.Vision, false).firstOrNull { it.key.provider == "gemini" }
                ?: return null to (if (attempt > 0) "Gemini сейчас не отвечает (лимит или ошибка), попробуйте позже" else "для этого нужен ключ Gemini: раздел «ИИ» → «1. Ключи»")
            val lite = candidate.model.startsWith("gemini-2.5-flash")
            val generation = JSONObject().put("maxOutputTokens", if (lite) maxTokens else maxTokens + 4096).put("temperature", 0.1)
            if (lite) generation.put("thinkingConfig", JSONObject().put("thinkingBudget", 0))
            val body = JSONObject().put("contents", JSONArray().put(JSONObject().put("parts", parts))).put("generationConfig", generation)
            val r = GgtgAi.http("POST", "https://generativelanguage.googleapis.com/v1beta/models/${candidate.model}:generateContent",
                mapOf("x-goog-api-key" to candidate.key.key.trim()), body.toString(), timeoutMs)
            var answer = ""
            if (r.status in 200..299) {
                try {
                    val content = JSONObject(r.body).optJSONArray("candidates")?.optJSONObject(0)?.optJSONObject("content")?.optJSONArray("parts")
                    if (content != null) for (i in 0 until content.length()) {
                        val o = content.getJSONObject(i)
                        if (!o.optBoolean("thought")) answer += o.optString("text")
                    }
                } catch (t: Throwable) { }
            }
            if (answer.isBlank()) {
                GgtgAi.failed(candidate, r.status, r.body, r.retryAfter)
                if (attempt == 3) return null to GgtgAi.failureText(r.status, r.body, "Gemini вернул пустой ответ")
                continue
            }
            return answer.trim() to null
        }
        return null to "Gemini не ответил"
    }

    private fun displayName(context: Context, uri: Uri): String {
        try {
            context.contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use {
                if (it.moveToFirst()) return it.getString(0)
            }
        } catch (t: Throwable) { }
        return uri.lastPathSegment ?: "файл"
    }

    private fun imageDataUrl(bytes: ByteArray): String? {
        val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
        BitmapFactory.decodeByteArray(bytes, 0, bytes.size, bounds)
        if (bounds.outWidth <= 0) return null
        var sample = 1
        while (maxOf(bounds.outWidth, bounds.outHeight) / sample > 2560) sample *= 2
        val bmp = BitmapFactory.decodeByteArray(bytes, 0, bytes.size, BitmapFactory.Options().apply { inSampleSize = sample }) ?: return null
        val scale = 1280f / maxOf(bmp.width, bmp.height)
        val fit = if (scale < 1f) Bitmap.createScaledBitmap(bmp, (bmp.width * scale).toInt(), (bmp.height * scale).toInt(), true) else bmp
        val out = ByteArrayOutputStream()
        fit.compress(Bitmap.CompressFormat.JPEG, 85, out)
        return "data:image/jpeg;base64," + Base64.encodeToString(out.toByteArray(), Base64.NO_WRAP)
    }

    private fun xmlText(xml: String): String = xml
        .replace("&amp;", "&").replace("&lt;", "<").replace("&gt;", ">").replace("&quot;", "\"").replace("&apos;", "'")

    private fun zipEntries(bytes: ByteArray): Map<String, String> {
        val result = LinkedHashMap<String, String>()
        ZipInputStream(bytes.inputStream()).use { zip ->
            while (true) {
                val e = zip.nextEntry ?: break
                if (e.name == "word/document.xml" || (e.name.startsWith("ppt/slides/slide") && e.name.endsWith(".xml"))) {
                    result[e.name] = zip.readBytes().toString(Charsets.UTF_8)
                }
            }
        }
        return result
    }

    private fun docxText(bytes: ByteArray): String {
        val xml = zipEntries(bytes)["word/document.xml"] ?: return ""
        val sb = StringBuilder()
        for (m in Regex("<w:t(?:\\s[^>]*)?>([^<]*)</w:t>|</w:p>").findAll(xml)) {
            if (m.value == "</w:p>") sb.append('\n') else sb.append(m.groupValues[1])
        }
        return xmlText(sb.toString())
    }

    private fun pptxText(bytes: ByteArray): String {
        val slides = zipEntries(bytes).filterKeys { it.startsWith("ppt/slides/slide") }
            .toSortedMap(compareBy({ it.length }, { it }))
        val sb = StringBuilder()
        var n = 0
        for ((_, xml) in slides) {
            val line = Regex("<a:t>([^<]*)</a:t>").findAll(xml).joinToString(" ") { it.groupValues[1] }.trim()
            if (line.isNotEmpty()) sb.append("Слайд ${++n}: ").append(xmlText(line)).append('\n')
        }
        return sb.toString()
    }

    private fun htmlText(html: String): String {
        val cleaned = html.replace(Regex("(?is)<(script|style|noscript|svg)[^>]*>.*?</\\1>"), " ")
        @Suppress("DEPRECATION")
        return android.text.Html.fromHtml(cleaned, android.text.Html.FROM_HTML_MODE_LEGACY).toString()
            .replace(Regex("[ \\t]+"), " ").replace(Regex("\\n\\s*\\n+"), "\n\n")
    }

    /** Adds a file: a document, a PDF or a picture. Returns an error text, null when it is in the base. */
    suspend fun addFile(context: Context, uri: Uri, progress: (String) -> Unit): String? {
        val name = displayName(context, uri)
        val suffix = name.substringAfterLast('.', "").lowercase()
        val bytes = withContext(Dispatchers.IO) {
            try { context.contentResolver.openInputStream(uri)?.use { it.readBytes() } } catch (t: Throwable) { null }
        } ?: return "не удалось открыть «$name»"
        val origin = "file:$name"
        if (suffix in setOf("png", "jpg", "jpeg", "webp", "gif", "bmp", "heic")) {
            progress("Смотрю на «$name»...")
            val url = withContext(Dispatchers.Default) { imageDataUrl(bytes) } ?: return "не удалось прочитать картинку"
            val r = GgtgAi.ask(
                "Ты переносишь содержимое картинки в базу знаний помощника. Ответ только по-русски, без вступлений.",
                "Сначала перепиши весь видимый текст и цифры дословно, построчно. Потом коротко опиши, что изображено " +
                    "(слайд, схема, прайс, скриншот, документ, фото) и о чём оно. До 3000 символов.",
                0.1, 1800, listOf(url))
            if (r.error != null) return r.error
            return ingest(name, "image", origin, r.text, progress)
        }
        if (suffix == "pdf") {
            progress("Читаю PDF «$name» (Gemini)...")
            if (bytes.size > MAX_INLINE) return "PDF больше 18 МБ: разделите его на части"
            val parts = JSONArray()
                .put(JSONObject().put("inline_data", JSONObject().put("mime_type", "application/pdf")
                    .put("data", Base64.encodeToString(bytes, Base64.NO_WRAP))))
                .put(JSONObject().put("text", "Извлеки весь текст этого документа дословно, по порядку. Таблицы передай строками. " +
                    "Текст на картинках и сканах тоже перепиши. Без комментариев и вступлений."))
            val (text, error) = geminiNative(parts, 16000, 300_000)
            if (error != null) return "не получилось прочитать «$name»: $error"
            return ingest(name, "pdf", origin, text.orEmpty(), progress)
        }
        progress("Читаю «$name»...")
        val text = withContext(Dispatchers.Default) {
            when (suffix) {
                "docx" -> docxText(bytes)
                "pptx" -> pptxText(bytes)
                "html", "htm" -> htmlText(bytes.toString(Charsets.UTF_8))
                "txt", "md", "csv", "tsv", "json", "log", "xml", "rtf" -> bytes.toString(Charsets.UTF_8)
                else -> ""
            }
        }
        if (text.isBlank()) return "не получилось прочитать «$name» (подходят pdf, txt, md, csv, docx, pptx, html и картинки)"
        return ingest(name, "file", origin, text, progress)
    }

    fun youtubeId(address: String): String? =
        Regex("(?:youtu\\.be/|youtube(?:-nocookie)?\\.com/(?:watch\\?(?:[^#]*&)?v=|shorts/|embed/|live/|v/))([A-Za-z0-9_-]{11})")
            .find(address)?.groupValues?.get(1)

    private suspend fun youtubeTranscript(id: String): Triple<String, String, String?> {
        val page = GgtgAi.http("GET", "https://www.youtube.com/watch?v=$id&hl=ru",
            mapOf("Cookie" to "CONSENT=YES+cb; SOCS=CAI", "User-Agent" to AGENT, "Accept-Language" to "ru,en;q=0.8"), null, 30_000)
        if (page.status != 200) return Triple("", "", "YouTube не открылся (${page.status})")
        val html = page.body
        var title = Regex("<title[^>]*>(.*?)</title>", RegexOption.DOT_MATCHES_ALL).find(html)?.groupValues?.get(1).orEmpty()
            .replace(" - YouTube", "").trim()
        val marker = html.indexOf("\"captionTracks\":[")
        if (marker < 0) return Triple(title, "", "у видео нет субтитров")
        val start = html.indexOf('[', marker)
        var depth = 0; var inString = false; var end = -1
        var i = start
        while (i < html.length) {
            val ch = html[i]
            if (inString) { if (ch == '\\') i++ else if (ch == '"') inString = false }
            else if (ch == '"') inString = true
            else if (ch == '[') depth++
            else if (ch == ']') { depth--; if (depth == 0) { end = i; break } }
            i++
        }
        if (end < 0) return Triple(title, "", "у видео нет субтитров")
        val tracks = try { JSONArray(html.substring(start, end + 1)) } catch (t: Throwable) { return Triple(title, "", "у видео нет субтитров") }
        var best: JSONObject? = null
        var bestRank = -1
        for (k in 0 until tracks.length()) {
            val t = tracks.getJSONObject(k)
            val lang = t.optString("languageCode")
            val auto = t.optString("kind") == "asr"
            val rank = (if (lang.startsWith("ru")) 4 else 0) + (if (lang.startsWith("en")) 2 else 0) + (if (auto) 0 else 6) + 1
            if (rank > bestRank) { bestRank = rank; best = t }
        }
        val address = best?.optString("baseUrl").orEmpty()
        if (address.isEmpty()) return Triple(title, "", "у видео нет субтитров")
        val caps = GgtgAi.http("GET", "$address&fmt=json3", mapOf("Cookie" to "CONSENT=YES+cb; SOCS=CAI", "User-Agent" to AGENT), null, 30_000)
        val sb = StringBuilder()
        try {
            val events = JSONObject(caps.body).optJSONArray("events") ?: JSONArray()
            for (e in 0 until events.length()) {
                val segs = events.getJSONObject(e).optJSONArray("segs") ?: continue
                val line = (0 until segs.length()).joinToString("") { segs.getJSONObject(it).optString("utf8") }.trim().replace(Regex("\\s+"), " ")
                if (line.isNotEmpty()) sb.append(line).append(' ')
            }
        } catch (t: Throwable) { }
        val text = sb.toString().trim()
        return Triple(title, text, if (text.isEmpty()) "субтитры пустые" else null)
    }

    private const val AGENT = "Mozilla/5.0 (Linux; Android 13; Pixel 7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0 Mobile Safari/537.36"

    /** Adds a link: a YouTube video or a page of a site. Returns an error text, null when it is in the base. */
    suspend fun addUrl(raw: String, progress: (String) -> Unit): String? {
        var address = raw.trim()
        if (address.isEmpty()) return "вставьте ссылку"
        if (!address.contains("://")) address = "https://$address"
        youtubeId(address)?.let { id ->
            val origin = "https://www.youtube.com/watch?v=$id"
            progress("Беру расшифровку видео...")
            val (title, text, error) = youtubeTranscript(id)
            val name = "YouTube: " + title.ifEmpty { id }
            if (error == null && text.length >= 100) return ingest(name, "youtube", origin, text, progress)
            progress("Субтитров нет (${error ?: "мало текста"}): видео смотрит Gemini...")
            val parts = JSONArray()
                .put(JSONObject().put("file_data", JSONObject().put("file_uri", origin)))
                .put(JSONObject().put("text", "Сделай подробную расшифровку этого видео по порядку: что в нём говорится, дословно там, " +
                    "где важны цифры, цены, условия, названия и имена. Если речи нет, опиши, что показано. Без вступлений и комментариев."))
            val (result, geminiError) = geminiNative(parts, 16000, 600_000)
            if (geminiError != null) return "у видео нет доступных субтитров, а Gemini не смог его посмотреть: $geminiError"
            return ingest(name, "youtube", origin, result.orEmpty(), progress)
        }
        progress("Открываю страницу...")
        val r = GgtgAi.http("GET", address, mapOf("User-Agent" to AGENT, "Accept-Language" to "ru,en;q=0.8"), null, 30_000)
        var text = if (r.status == 200) htmlText(r.body).trim() else ""
        var title = Regex("<title[^>]*>(.*?)</title>", RegexOption.DOT_MATCHES_ALL).find(r.body)?.groupValues?.get(1)?.trim().orEmpty()
        if (text.length < 400) {
            // pages that are empty without their scripts: through a reader that opens them in a browser
            val via = GgtgAi.http("GET", "https://r.jina.ai/$address", mapOf("User-Agent" to AGENT), null, 40_000)
            if (via.status == 200 && via.body.trim().length >= 200) text = via.body.trim()
        }
        if (text.isBlank()) return "страница не прочиталась (${if (r.status == 0) "сайт не открылся" else "ответ ${r.status}"})"
        val name = title.ifEmpty { Uri.parse(address).host.orEmpty() }.take(100)
        return ingest(name, "web", address, text, progress)
    }

    /** Adds a piece of text written or pasted by hand. */
    suspend fun addText(name: String, text: String, progress: (String) -> Unit): String? =
        ingest(name, "text", "text:$name", text, progress)
}
