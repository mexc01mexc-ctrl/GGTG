package app.ggtg

import android.app.Service
import android.content.Context
import android.content.Intent
import android.os.IBinder
import androidx.core.app.NotificationCompat
import androidx.core.app.NotificationManagerCompat
import org.telegram.messenger.AndroidUtilities
import org.telegram.messenger.ApplicationLoader
import org.telegram.messenger.NotificationsController
import org.telegram.messenger.R
import org.telegram.tgnet.ConnectionsManager

// ng: keeps GGTG bulk operations (SMS-less: массовые действия, накрутка, сбор) alive while the app
// is backgrounded. Runs as a dataSync foreground service for as long as GgtgProcess has at least
// one Running handle; the notification shows how many are going. GgtgProcess drives start/stop via
// onRunningCountChanged (wired in ApplicationLoaderImpl / on first start()).
class GgtgForegroundService : Service() {

    private val listener = { AndroidUtilities.runOnUIThread { refresh() } }

    // With the screen off the app counts as paused, and Telegram's network layer puts every connection
    // to sleep 10 seconds later, so the requests of a running operation would stall. While something runs,
    // every few seconds each account's network is woken again, and the CPU lock is renewed.
    private val heartbeat = object : Runnable {
        override fun run() {
            if (GgtgProcess.runningCount() == 0) return
            for (account in GgtgAccounts.activated()) {
                runCatching { ConnectionsManager.getInstance(account.slot).resumeNetworkMaybe() }
            }
            GgtgWakeLock.renew()
            AndroidUtilities.runOnUIThread(this, HEARTBEAT_MS)
        }
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        // startForegroundService() obliges us to call startForeground() - even when the operation
        // already finished before we got here (a short bulk action), or Android kills the app
        startForeground(NOTIFICATION_ID, buildNotification())
        GgtgWakeLock.acquire("foreground_service")
        GgtgProcess.addListener(listener)
        AndroidUtilities.cancelRunOnUIThread(heartbeat)
        AndroidUtilities.runOnUIThread(heartbeat)
        if (GgtgProcess.runningCount() == 0) {
            GgtgWakeLock.release("foreground_service")
            stopSelf()
            return START_NOT_STICKY
        }
        return START_NOT_STICKY
    }

    // Android 15+: stop at once if the system times the service out, or the app is killed
    override fun onTimeout(startId: Int, fgsType: Int) {
        stopSelf()
    }

    private fun refresh() {
        if (GgtgProcess.runningCount() == 0) { stopSelf(); return }
        NotificationManagerCompat.from(ApplicationLoader.applicationContext).notify(NOTIFICATION_ID, buildNotification())
    }

    private fun buildNotification(): android.app.Notification {
        NotificationsController.checkOtherNotificationsChannel()
        val running = GgtgProcess.runningCount()
        val text = "Идёт операций: $running"
        return NotificationCompat.Builder(ApplicationLoader.applicationContext)
            .setSmallIcon(android.R.drawable.stat_sys_upload)
            .setWhen(System.currentTimeMillis())
            .setChannelId(NotificationsController.OTHER_NOTIFICATIONS_CHANNEL)
            .setContentTitle("GGTG")
            .setContentText(text)
            .setTicker(text)
            .setOngoing(true)
            .setProgress(0, 0, true)
            .build()
    }

    override fun onDestroy() {
        super.onDestroy()
        AndroidUtilities.cancelRunOnUIThread(heartbeat)
        GgtgWakeLock.release("foreground_service")
        GgtgProcess.removeListener(listener)
        runCatching { stopForeground(true) }
        NotificationManagerCompat.from(ApplicationLoader.applicationContext).cancel(NOTIFICATION_ID)
    }

    companion object {
        private const val NOTIFICATION_ID = 51_741
        private const val HEARTBEAT_MS = 4_000L

        // Called by GgtgProcess whenever its running count changes; starts the service on the first
        // running operation, lets it stop itself once none remain.
        @JvmStatic
        fun sync(runningCount: Int) {
            val context = ApplicationLoader.applicationContext ?: return
            if (runningCount <= 0) return
            AndroidUtilities.runOnUIThread {
                // once: ask to take the app out of battery optimization (the system kills a long run otherwise)
                org.telegram.ui.LaunchActivity.getLastFragment()?.parentActivity?.let { GgtgBattery.askOnce(it) }
                runCatching {
                    val intent = Intent(context, GgtgForegroundService::class.java)
                    if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.O) {
                        context.startForegroundService(intent)
                    } else {
                        context.startService(intent)
                    }
                }
            }
        }
    }
}
