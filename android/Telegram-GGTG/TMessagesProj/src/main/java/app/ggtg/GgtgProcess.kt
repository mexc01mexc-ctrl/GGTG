package app.ggtg

import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import org.json.JSONArray
import org.json.JSONObject
import org.telegram.messenger.ApplicationLoader
import java.io.File
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.atomic.AtomicLong

// ng: Kotlin port of ggtg-macos' core/process_manager.h - the shared "Процессы" registry every
// long-running GGTG operation (SMS, bulk actions, views, scraping, export) logs into. In-memory
// and observable, and kept on disk (filesDir/ggtg_bulk_db/processes.json) so the list survives
// restarts - a process still running when the app died comes back as failed; a Foreground Service (GgtgForegroundService) keeps the process alive while any
// handle is Running so a campaign survives the app being backgrounded.
object GgtgProcess {

    enum class Kind { Other, Sms, Bulk, Views, Export, Scrape, Warmup, Registration }
    enum class Status { Running, Succeeded, Failed }

    data class LogLine(val text: String, val at: Long)

    data class Snapshot(
        val id: Long,
        val title: String,
        val status: Status,
        val kind: Kind,
        val log: List<LogLine>,
        val progressCurrent: Int,
        val progressTotal: Int,
        val startedAt: Long,
        val finishedAt: Long,
        // what each lane is doing right now ("Поток 1: аккаунт: ищет @x → ..."), running only
        val live: List<String> = emptyList()
    )

    class Handle internal constructor(
        val id: Long,
        val title: String,
        val kind: Kind,
        val startedAt: Long = System.currentTimeMillis() / 1000
    ) {
        val log = CopyOnWriteArrayList<LogLine>()
        @Volatile var status = Status.Running
        @Volatile var progressCurrent = 0
        @Volatile var progressTotal = 0
        @Volatile var finishedAt = 0L
        // Reopens this operation's own live screen from the "Процессы" list, when it has one.
        @Volatile var reopen: (() -> Unit)? = null

        fun log(line: String) {
            if (status != Status.Running) return
            log.add(LogLine(line, System.currentTimeMillis() / 1000))
            if (log.size > MAX_LOG_LINES) log.removeAt(0)
            notifyChanged()
        }

        // ng: live "what is happening right now" lines, one per lane - shown above the log while the
        // process runs and not kept in it (every step would flood the log); not persisted.
        private val live = java.util.concurrent.ConcurrentHashMap<Int, String>()

        fun setLive(key: Int, text: String) {
            if (status != Status.Running) return
            live[key] = text
            notifyChanged()
        }

        fun clearLive(key: Int) {
            if (live.remove(key) != null) notifyChanged()
        }

        fun progress(current: Int, total: Int) {
            if (status != Status.Running) return
            progressCurrent = current
            progressTotal = total
            notifyChanged()
        }

        fun succeed(finalLine: String = "") = finish(Status.Succeeded, finalLine)
        fun fail(finalLine: String = "") = finish(Status.Failed, finalLine)

        private fun finish(newStatus: Status, finalLine: String) {
            if (status != Status.Running) return
            if (finalLine.isNotEmpty()) log.add(LogLine(finalLine, System.currentTimeMillis() / 1000))
            status = newStatus
            live.clear()
            finishedAt = System.currentTimeMillis() / 1000
            onFinished()
            notifyChanged()
        }

        fun snapshot() = Snapshot(id, title, status, kind, log.toList(), progressCurrent, progressTotal, startedAt, finishedAt,
            live.entries.sortedBy { it.key }.map { it.value })
    }

    private const val MAX_LOG_LINES = 2000
    private const val SAVE_DELAY_MS = 2000L

    private val nextId = AtomicLong(SystemClock.elapsedRealtime())
    private val listeners = CopyOnWriteArrayList<() -> Unit>()
    @Volatile var onRunningCountChanged: ((Int) -> Unit)? = { GgtgForegroundService.sync(it) }

    fun addListener(l: () -> Unit) { listeners.add(l) }
    fun removeListener(l: () -> Unit) { listeners.remove(l) }
    fun notifyChanged() {
        scheduleSave()
        listeners.forEach { runCatching { it() } }
    }

    // ---- Persistence ----

    private val saver = Handler(Looper.getMainLooper())
    @Volatile private var savePending = false
    private val saveRunnable = Runnable {
        savePending = false
        Thread { save() }.start()
    }

    // Declared after the saver above: loading may schedule a save.
    private val handles = CopyOnWriteArrayList<Handle>().also { load(it) }

    private fun storeFile() = File(File(ApplicationLoader.getFilesDirFixed(), "ggtg_bulk_db").also { it.mkdirs() }, "processes.json")

    private fun scheduleSave() {
        if (savePending) return
        savePending = true
        saver.postDelayed(saveRunnable, SAVE_DELAY_MS)
    }

    @Synchronized
    private fun save() {
        runCatching {
            val list = JSONArray()
            for (h in handles) {
                val log = JSONArray()
                for (line in h.log) log.put(JSONObject().put("text", line.text).put("at", line.at))
                list.put(JSONObject()
                    .put("title", h.title)
                    .put("status", h.status.name)
                    .put("kind", h.kind.name)
                    .put("current", h.progressCurrent)
                    .put("total", h.progressTotal)
                    .put("started", h.startedAt)
                    .put("finished", h.finishedAt)
                    .put("log", log))
            }
            val file = storeFile()
            val tmp = File(file.parentFile, file.name + ".tmp")
            tmp.writeText(JSONObject().put("processes", list).toString())
            if (!tmp.renameTo(file)) { file.delete(); tmp.renameTo(file) }
        }
    }

    private fun load(into: MutableList<Handle>) {
        runCatching {
            val file = storeFile()
            if (!file.exists()) return
            val list = JSONObject(file.readText()).optJSONArray("processes") ?: return
            var interrupted = false
            for (i in 0 until list.length()) {
                val o = list.getJSONObject(i)
                val h = Handle(
                    nextId.incrementAndGet(),
                    o.optString("title"),
                    runCatching { Kind.valueOf(o.optString("kind")) }.getOrDefault(Kind.Other),
                    o.optLong("started"))
                val log = o.optJSONArray("log") ?: JSONArray()
                for (j in 0 until log.length()) {
                    val line = log.getJSONObject(j)
                    h.log.add(LogLine(line.optString("text"), line.optLong("at")))
                }
                h.progressCurrent = o.optInt("current")
                h.progressTotal = o.optInt("total")
                h.finishedAt = o.optLong("finished")
                h.status = runCatching { Status.valueOf(o.optString("status")) }.getOrDefault(Status.Failed)
                if (h.status == Status.Running) {
                    h.finishedAt = h.log.lastOrNull()?.at ?: h.startedAt
                    h.log.add(LogLine("Прервано: приложение было закрыто", h.finishedAt))
                    h.status = Status.Failed
                    interrupted = true
                }
                into.add(h)
            }
            if (interrupted) scheduleSave()
        }
    }

    fun start(title: String, kind: Kind = Kind.Other, reopen: (() -> Unit)? = null): Handle {
        val handle = Handle(nextId.incrementAndGet(), title, kind)
        handle.reopen = reopen
        handles.add(0, handle)
        onRunningCountChanged?.invoke(runningCount())
        notifyChanged()
        return handle
    }

    private fun onFinished() {
        onRunningCountChanged?.invoke(runningCount())
        GgtgAccounts.sleepIdle()
    }

    fun runningCount(): Int = handles.count { it.status == Status.Running }

    /** Writes [line] into the log of every running operation (e.g. "no network, waiting"). */
    fun logToRunning(line: String) {
        for (h in handles) if (h.status == Status.Running) h.log(line)
    }

    fun running(kind: Kind): Handle? = handles.firstOrNull { it.status == Status.Running && it.kind == kind }

    fun handle(id: Long): Handle? = handles.firstOrNull { it.id == id }

    fun snapshots(): List<Snapshot> = handles.map { it.snapshot() }

    fun clearFinished() {
        handles.removeAll { it.status != Status.Running }
        notifyChanged()
    }
}
