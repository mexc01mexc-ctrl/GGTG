package app.ggtg.tdata

import java.io.File
import org.telegram.messenger.FileLog
import java.nio.charset.Charset

// ng: finds and parses the sidecar "2FA note" that usually sits next to a tdata folder, e.g.
// twoFA.txt containing "TelegramExpert.pro: 6xfhsi" (label, separator, password). Returns password
// candidates best-first so the caller burns as few wrong attempts against the server as possible.
object TdataPasswordFinder {

    private const val MAX_CANDIDATES = 6
    private const val MAX_FILE_BYTES = 64 * 1024
    private val TEXT_EXTENSIONS = setOf("", "txt", "text", "md", "csv", "json", "log", "rtf")
    private val NAME_HINTS = listOf("2fa", "twofa", "2фа", "password", "passwd", "pass", "pwd", "пароль", "parol", "cloudpass")
    private val DOMAIN_LIKE = Regex("^[\\p{L}\\d-]+(\\.[\\p{L}\\d-]+)+$")
    private val LABEL_WORDS = setOf("telegram", "password", "pass", "pwd", "2fa", "twofa", "пароль", "tg")
    private val SEPARATOR = Regex("\\s*[:=|\\t]\\s*|\\s+[-–—]\\s+")

    /**
     * [tdataFolder] is the folder holding key_* files. The note is looked for beside it (its parent),
     * inside it, and one level further up (archives often wrap "account/tdata" + "account/twoFA.txt").
     * Lines that mention [phone] or [userId] are ranked first, for notes covering several accounts.
     */
    fun findCandidates(tdataFolder: File, userId: Long, phone: String?, stopAt: File? = null): List<String> {
        // parent first, then the folder itself, then every ancestor up to (and including) [stopAt]
        val dirs = mutableListOf<File>()
        tdataFolder.parentFile?.let { dirs += it }
        dirs += tdataFolder
        var up = tdataFolder.parentFile?.parentFile
        var depth = 0
        while (up != null && depth++ < 8) {
            dirs += up
            if (stopAt != null && up.absoluteFile == stopAt.absoluteFile) break
            up = up.parentFile
        }
        var files = dirs.flatMap { dir -> noteFilesIn(dir) }.distinct()
        // last resort: a note anywhere inside the import root (one shared file for all accounts)
        if (files.isEmpty() && stopAt != null) {
            files = stopAt.walkTopDown().maxDepth(6).filter { it.isFile && isNoteFile(it) }.take(5).toList()
        }
        FileLog.d("tdata 2FA note: ${files.size} file(s) found for userId=$userId")
        val phoneDigits = phone?.filter { it.isDigit() }?.takeIf { it.length >= 6 }
        val ranked = LinkedHashMap<String, Int>()
        for (file in files) {
            val text = readText(file) ?: continue
            if (file.name.endsWith(".json", ignoreCase = true)) {
                try {
                    val json = org.json.JSONObject(text)
                    val jsonPasswords = listOfNotNull(
                        json.optString("twoFactor").takeIf { it.isNotEmpty() },
                        json.optString("twoFA").takeIf { it.isNotEmpty() },
                        json.optString("two_factor").takeIf { it.isNotEmpty() },
                        json.optString("password").takeIf { it.isNotEmpty() },
                        json.optJSONObject("settings")?.optString("twoFactor")?.takeIf { it.isNotEmpty() }
                    )
                    for (c in jsonPasswords) {
                        ranked[c] = 0
                    }
                } catch (_: Throwable) {
                }
            }
            for ((candidate, score) in parse(text, phoneDigits, userId)) {
                val prev = ranked[candidate]
                if (prev == null || score < prev) ranked[candidate] = score
            }
        }
        return ranked.entries.sortedBy { it.value }.map { it.key }.take(MAX_CANDIDATES)
    }

    private fun noteFilesIn(dir: File): List<File> {
        val all = dir.listFiles() ?: return emptyList()
        return all.filter { isNoteFile(it) }.sortedBy { f ->
            val n = normalized(f.nameWithoutExtension)
            if (f.name.equals("ggtg.json", ignoreCase = true) || n.contains("2fa") || n.contains("twofa") || n.contains("2фа")) 0 else 1
        }
    }

    private fun isNoteFile(f: File): Boolean =
        f.isFile && f.length() in 1..MAX_FILE_BYTES && (
            f.name.equals("ggtg.json", ignoreCase = true) ||
            f.name.endsWith(".json", ignoreCase = true) ||
            (f.extension.lowercase() in TEXT_EXTENSIONS &&
             normalized(f.nameWithoutExtension).let { n -> NAME_HINTS.any { n.contains(it) } })
        )

    private fun normalized(name: String) = name.lowercase().filter { it.isLetterOrDigit() }

    private fun readText(file: File): String? = try {
        val bytes = file.readBytes()
        when {
            bytes.size >= 3 && bytes[0] == 0xEF.toByte() && bytes[1] == 0xBB.toByte() && bytes[2] == 0xBF.toByte() ->
                String(bytes, 3, bytes.size - 3, Charsets.UTF_8)
            bytes.size >= 2 && bytes[0] == 0xFF.toByte() && bytes[1] == 0xFE.toByte() ->
                String(bytes, 2, bytes.size - 2, Charset.forName("UTF-16LE"))
            bytes.size >= 2 && bytes[0] == 0xFE.toByte() && bytes[1] == 0xFF.toByte() ->
                String(bytes, 2, bytes.size - 2, Charset.forName("UTF-16BE"))
            else -> String(bytes, Charsets.UTF_8)
        }
    } catch (e: Exception) {
        null
    }

    /** Lower score = try earlier. */
    internal fun parse(text: String, phoneDigits: String?, userId: Long): List<Pair<String, Int>> {
        val out = mutableListOf<Pair<String, Int>>()
        val lines = text.lineSequence().map { it.trim() }.filter { it.isNotEmpty() }.toList()
        for (line in lines) {
            val mine = (phoneDigits != null && line.filter { it.isDigit() }.contains(phoneDigits)) ||
                (userId > 0 && line.contains(userId.toString()))
            val base = if (mine) 0 else 10
            val sep = SEPARATOR.find(line)
            if (sep != null) {
                // "Label: password" - the label side is never a password candidate
                val value = clean(line.substring(sep.range.last + 1))
                if (value.isNotEmpty()) {
                    out.add(value to base)
                    // spaces inside the value: it may be "password (comment)", so also try its first word
                    val first = value.split(Regex("\\s+")).first()
                    if (first != value) out.add(clean(first) to base + 1)
                }
            } else {
                val tokens = line.split(Regex("\\s+")).map { clean(it) }.filter { it.isNotEmpty() }
                tokens.forEachIndexed { i, token ->
                    val labelLike = DOMAIN_LIKE.matches(token) || token.lowercase() in LABEL_WORDS
                    val hasDigit = token.any { it.isDigit() }
                    val score = base + 2 + (if (labelLike) 4 else 0) + (if (hasDigit) 0 else 1) + i.coerceAtMost(1)
                    out.add(token to score)
                }
            }
        }
        return out.filter { it.first.isNotEmpty() }
    }

    private fun clean(s: String): String = s.trim().trim('"', '\'', '`', '«', '»', '(', ')', '[', ']', '<', '>', ',', ';')
}
