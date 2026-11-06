package app.ggtg

import org.json.JSONArray
import org.json.JSONObject
import org.telegram.messenger.ApplicationLoader
import org.telegram.messenger.UserConfig

// The arrangement of the accounts list (the order of accounts - which also gives the order of the folders - and
// which accounts are pinned) is shared between the devices by the sync, the same as core/ggtg_layout.h on the
// desktop. The device that changed it last wins; `at` is when this device's arrangement last changed (by the user,
// or taken over from another device).
object GgtgLayout {
    private fun prefs() = ApplicationLoader.applicationContext.getSharedPreferences("ggtg_layout", android.content.Context.MODE_PRIVATE)

    @Volatile var applying = false

    fun at(): Long = prefs().getLong("at", 0L)

    fun setAt(value: Long) { prefs().edit().putLong("at", value).apply() }

    /** The user reordered or pinned something here. */
    fun noteChanged() {
        if (!applying) setAt(System.currentTimeMillis() / 1000)
    }

    /** Folder names in the order the accounts list shows them (the trash is not among them, it is always last). */
    fun folderOrder(): List<String> {
        val slots = (0 until UserConfig.MAX_ACCOUNT_COUNT).filter { UserConfig.getInstance(it).isClientActivated }
        return org.telegram.ui.Components.AccountFolderLogic.buildTopLevelOrder(slots)
            .filterIsInstance<String>().filter { it != GgtgAccounts.TRASH_FOLDER }
    }

    fun toJson(): JSONObject {
        val accounts = JSONArray()
        for (ref in GgtgAccounts.activated()) {
            val uc = UserConfig.getInstance(ref.slot)
            accounts.put(JSONObject().put("userId", ref.userId.toString()).put("pinned", uc.ncgPinned))
        }
        return JSONObject().put("at", at().toString()).put("accounts", accounts).put("folders", JSONArray(folderOrder()))
    }

    /** Takes over the received arrangement when it is newer than this device's own. */
    fun apply(layout: JSONObject) {
        val remoteAt = layout.optString("at").toLongOrNull() ?: 0L
        if (remoteAt <= at()) return
        applying = true
        try {
            val accounts = layout.optJSONArray("accounts") ?: return
            val bySlot = HashMap<Long, Int>()
            for (slot in 0 until UserConfig.MAX_ACCOUNT_COUNT) {
                val uc = UserConfig.getInstance(slot)
                if (uc.isClientActivated) bySlot[uc.clientUserId] = slot
            }
            // Android keeps the top level (ungrouped accounts and folders) in one numbering and the accounts of each
            // folder in their own: the received order is cut into those the same way
            var top = 0
            val inFolder = HashMap<String, Int>()
            val folderSeen = HashSet<String>()
            val seen = HashSet<Int>()
            for (i in 0 until accounts.length()) {
                val o = accounts.optJSONObject(i) ?: continue
                val slot = bySlot[o.optString("userId").toLongOrNull() ?: 0L] ?: continue
                val uc = UserConfig.getInstance(slot)
                uc.ncgPinned = o.optBoolean("pinned", false)
                val folder = if (uc.ncgTrashed) "" else (uc.ncgFolder ?: "")
                if (uc.ncgTrashed) {
                    // the trash is always last and is not arranged
                } else if (folder.isEmpty()) {
                    uc.ncgOrder = top++
                } else {
                    if (folderSeen.add(folder)) org.telegram.ui.Components.AccountFolderStore.setOrder(folder, top++)
                    val n = inFolder.getOrDefault(folder, 0)
                    uc.ncgOrder = n
                    inFolder[folder] = n + 1
                }
                uc.saveConfig(false)
                seen.add(slot)
            }
            setAt(remoteAt)
        } finally {
            applying = false
        }
    }
}
