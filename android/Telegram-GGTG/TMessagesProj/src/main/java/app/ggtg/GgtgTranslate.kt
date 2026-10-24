package app.ggtg

import android.content.Context
import android.widget.Toast
import org.json.JSONArray
import org.json.JSONObject
import org.telegram.messenger.AndroidUtilities
import org.telegram.messenger.LocaleController
import java.net.HttpURLConnection
import java.net.URL
import java.net.URLEncoder

// ng: free online translation of a chat message (no key, no account, no model; mirrors the desktop
// app). "Перевести" in a message's long-press menu replaces the message's own text with the
// translation, "Оригинал" brings the text back. The language is the one of the app. Only the text
// being translated is sent. Several public services are tried one after another: a single one
// refuses requests now and then (typically from a VPN's address).
object GgtgTranslate {
    private const val MAX_LENGTH = 4500
    private const val AGENT = "Mozilla/5.0 (Linux; Android 13; Pixel 7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0 Mobile Safari/537.36"

    class Result(val text: String, val detected: String)

    private fun open(url: String, post: String? = null): HttpURLConnection {
        val connection = URL(url).openConnection() as HttpURLConnection
        connection.connectTimeout = 12_000
        connection.readTimeout = 20_000
        connection.setRequestProperty("User-Agent", AGENT)
        connection.setRequestProperty("Accept", "application/json,text/plain,*/*")
        if (post != null) {
            connection.requestMethod = "POST"
            connection.doOutput = true
            connection.setRequestProperty("Content-Type", "application/x-www-form-urlencoded;charset=UTF-8")
            connection.outputStream.use { it.write(post.toByteArray()) }
        }
        return connection
    }

    private fun read(connection: HttpURLConnection): String {
        val code = connection.responseCode
        if (code !in 200..299) throw IllegalStateException("http $code")
        return connection.inputStream.bufferedReader().use { it.readText() }
    }

    // 1. translate.googleapis.com (the one the Google Translate page itself uses)
    private fun google(text: String, target: String): Result {
        val body = read(open(
            "https://translate.googleapis.com/translate_a/single?client=gtx&sl=auto&dt=t&tl=" + URLEncoder.encode(target, "UTF-8"),
            "q=" + URLEncoder.encode(text, "UTF-8")
        ))
        val root = JSONArray(body)
        val parts = root.optJSONArray(0) ?: throw IllegalStateException("empty")
        val builder = StringBuilder()
        for (i in 0 until parts.length()) builder.append(parts.optJSONArray(i)?.optString(0) ?: "")
        if (builder.isEmpty()) throw IllegalStateException("empty")
        return Result(builder.toString(), root.optString(2))
    }

    // 2. the same engine through the Chrome extension's address
    private fun googleChrome(text: String, target: String): Result {
        val body = read(open(
            "https://clients5.google.com/translate_a/t?client=dict-chrome-ex&sl=auto&tl=" + URLEncoder.encode(target, "UTF-8") +
                "&q=" + URLEncoder.encode(text.take(1800), "UTF-8")
        ))
        val root = JSONArray(body)
        val first = root.opt(0)
        val translated: String
        var detected = ""
        if (first is JSONArray) {
            translated = first.optString(0)
            detected = first.optString(1)
        } else {
            translated = root.optString(0)
        }
        if (translated.isEmpty()) throw IllegalStateException("empty")
        return Result(translated, detected)
    }

    /** `done(result, error)` is called on the UI thread; exactly one of them is null. */
    fun translate(text: String, target: String, done: (Result?, String?) -> Unit) {
        Thread {
            var result: Result? = null
            var last = ""
            for (provider in listOf(::google, ::googleChrome)) {
                try {
                    result = provider(text, target)
                    break
                } catch (e: Exception) {
                    last = e.message ?: ""
                }
            }
            val error = if (result != null) null else
                "сервисы перевода сейчас недоступны ($last). Если включён VPN, попробуйте выключить его или повторить позже"
            AndroidUtilities.runOnUIThread { done(result, error) }
        }.start()
    }

    private val templateCache = java.util.concurrent.ConcurrentHashMap<String, String>()

    /** Translates a message template preserving [NAME], [GROUP], [SPLIT] and {a|b|c} tokens. */
    fun translateTemplate(templateText: String, targetCode: String, done: (String?, String?) -> Unit) {
        val normTarget = if (targetCode == "zh" || targetCode == "zh-CN") "zh-CN" else if (targetCode == "lat") "en" else targetCode.ifEmpty { "en" }
        val key = "$normTarget\n$templateText"
        val cached = templateCache[key]
        if (cached != null) {
            AndroidUtilities.runOnUIThread { done(cached, null) }
            return
        }

        Thread {
            try {
                data class Piece(var text: String, val translate: Boolean)
                val pieces = mutableListOf<Piece>()
                var run = StringBuilder()
                val letters = Regex("\\p{L}")

                fun flush() {
                    if (run.isNotEmpty()) {
                        val s = run.toString()
                        pieces.add(Piece(s, letters.containsMatchIn(s)))
                        run.clear()
                    }
                }

                var i = 0
                while (i < templateText.length) {
                    val ch = templateText[i]
                    if (ch == '[') {
                        val end = templateText.indexOf(']', i)
                        if (end > i) {
                            flush()
                            val token = templateText.substring(i, end + 1)
                            pieces.add(Piece(token, false))
                            i = end + 1
                            continue
                        }
                    }
                    if (ch == '{' || ch == '}' || ch == '|') {
                        flush()
                        pieces.add(Piece(ch.toString(), false))
                        i++
                        continue
                    }
                    run.append(ch)
                    i++
                }
                flush()

                for (p in pieces) {
                    if (p.translate) {
                        val original = p.text
                        val words = original.trim()
                        val start = original.indexOf(words)
                        val lead = original.substring(0, start)
                        val tail = original.substring(start + words.length)

                        var translated: String? = null
                        for (provider in listOf(::google, ::googleChrome)) {
                            try {
                                translated = provider(words, normTarget).text
                                if (!translated.isNullOrEmpty()) break
                            } catch (_: Exception) {}
                        }
                        if (!translated.isNullOrEmpty()) {
                            p.text = lead + translated + tail
                        }
                    }
                }

                val full = pieces.joinToString("") { it.text }
                templateCache[key] = full
                AndroidUtilities.runOnUIThread { done(full, null) }
            } catch (e: Exception) {
                AndroidUtilities.runOnUIThread { done(null, e.message ?: "error") }
            }
        }.start()
    }

    /** The text translated into the language of the app and handed to [apply]; problems come as a toast. */
    fun translateMessage(context: Context, rawText: String, apply: java.util.function.Consumer<String>) {
        val source = rawText.trim().take(MAX_LENGTH)
        if (source.isEmpty()) return
        val rest = rawText.trim().drop(source.length)
        val target = LocaleController.getInstance().currentLocale.language.ifEmpty { "ru" }
        translate(source, target) { result, error ->
            if (result == null) {
                Toast.makeText(context, "Не удалось перевести: $error", Toast.LENGTH_LONG).show()
            } else if (result.detected.isNotEmpty() && result.detected.take(2).equals(target.take(2), true)) {
                Toast.makeText(context, "Сообщение уже на языке приложения", Toast.LENGTH_SHORT).show()
            } else {
                apply.accept(result.text + rest)
            }
        }
    }
}
