package app.ggtg

import android.content.Context
import android.net.wifi.WifiManager
import android.os.PowerManager
import org.telegram.messenger.ApplicationLoader
import org.telegram.messenger.FileLog

// ng: keeps the CPU and Wi-Fi active when long operations (Desktop -> Android sync, account import,
// registration, SMS campaigns, warmup, views) run, even if the device screen locks or turns off.
object GgtgWakeLock {
    private var wakeLock: PowerManager.WakeLock? = null
    private var wifiLock: WifiManager.WifiLock? = null
    private var refCount = 0

    @Synchronized
    fun acquire(tag: String = "ggtg:task") {
        refCount++
        if (refCount == 1) {
            val ctx = ApplicationLoader.applicationContext ?: return
            runCatching {
                val pm = ctx.getSystemService(Context.POWER_SERVICE) as? PowerManager
                if (pm != null && (wakeLock == null || !wakeLock!!.isHeld)) {
                    wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "ggtg:wake_lock").apply {
                        setReferenceCounted(false)
                        acquire(3600_000L) // 1 hour max safety timeout
                    }
                    FileLog.d("ggtg wakelock: acquired for $tag")
                }
            }
            runCatching {
                val wm = ctx.applicationContext.getSystemService(Context.WIFI_SERVICE) as? WifiManager
                if (wm != null && (wifiLock == null || !wifiLock!!.isHeld)) {
                    @Suppress("DEPRECATION")
                    wifiLock = wm.createWifiLock(WifiManager.WIFI_MODE_FULL_HIGH_PERF, "ggtg:wifi_lock").apply {
                        setReferenceCounted(false)
                        acquire()
                    }
                    FileLog.d("ggtg wifilock: acquired for $tag")
                }
            }
        } else {
            FileLog.d("ggtg wakelock: refCount=$refCount for $tag")
        }
    }

    @Synchronized
    fun release(tag: String = "ggtg:task") {
        if (refCount <= 0) return
        refCount--
        if (refCount == 0) {
            runCatching {
                if (wakeLock != null && wakeLock!!.isHeld) {
                    wakeLock?.release()
                }
                wakeLock = null
                FileLog.d("ggtg wakelock: released after $tag")
            }
            runCatching {
                if (wifiLock != null && wifiLock!!.isHeld) {
                    wifiLock?.release()
                }
                wifiLock = null
                FileLog.d("ggtg wifilock: released after $tag")
            }
        } else {
            FileLog.d("ggtg wakelock: refCount=$refCount after releasing $tag")
        }
    }

    /** Run a block keeping WakeLock + WifiLock active */
    inline fun <T> withLock(tag: String = "ggtg:task", block: () -> T): T {
        acquire(tag)
        try {
            return block()
        } finally {
            release(tag)
        }
    }
}
