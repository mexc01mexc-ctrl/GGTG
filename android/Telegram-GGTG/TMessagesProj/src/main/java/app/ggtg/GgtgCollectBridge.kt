package app.ggtg

import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.launch

// ng: entry point for the group profile menu ("Спарсить участников" / "Собрать через Сквидвард").
// Runs in its own scope so the collection outlives the profile screen; progress is in "Процессы".
object GgtgCollectBridge {
    @JvmStatic
    fun start(username: String, viaSquidward: Boolean) {
        CoroutineScope(SupervisorJob() + Dispatchers.IO).launch {
            if (!viaSquidward) {
                runCatching { GgtgScraper.scrapeMembers(username, GgtgScraper.Options()) }
                return@launch
            }
            val q = "@$username"
            // Members collected before: saved straight away, the bot isn't asked again.
            GgtgSquidwardCache.find(q)?.let { saved ->
                val users = saved.users
                if (!users.isNullOrEmpty()) {
                    GgtgDb.saveUsernames(GgtgDb.generateManualBaseId(), saved.result.baseTitle, users)
                    GgtgProcess.start("Сбор участников: " + saved.result.baseTitle, GgtgProcess.Kind.Scrape)
                        .succeed("Из сохранённого результата Сквидварда · пользователей: ${users.size}")
                    return@launch
                }
            }
            val r = runCatching { GgtgSquidward.lookup(q) }.getOrNull() ?: return@launch
            if (!r.canCollect) return@launch
            runCatching { GgtgSquidward.collect(q, r.kind == "user", r.baseTitle) }
        }
    }
}
