/*
 * This is the source code of Telegram for Android v. 1.3.x.
 * It is licensed under GNU GPL v. 2 or later.
 * You should have received a copy of the license in this archive (see LICENSE).
 *
 * Copyright Nikolai Kudashov, 2013-2018.
 */

package org.telegram.messenger;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.Service;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.os.Build;
import androidx.core.app.NotificationCompat;
import android.content.SharedPreferences;
import android.os.IBinder;

public class NotificationsService extends Service {

    @Override
    public void onCreate() {
        super.onCreate();
        ApplicationLoader.postInitApplication();
        goForeground();
    }

    // Keeps the process (and the push connections) alive in the background.
    private void goForeground() {
        try {
            String channelId = "ggtg_keepalive";
            if (Build.VERSION.SDK_INT >= 26) {
                NotificationManager nm = (NotificationManager) getSystemService(NOTIFICATION_SERVICE);
                if (nm.getNotificationChannel(channelId) == null) {
                    NotificationChannel channel = new NotificationChannel(channelId, "Соединение с аккаунтами", NotificationManager.IMPORTANCE_MIN);
                    channel.setShowBadge(false);
                    nm.createNotificationChannel(channel);
                }
            }
            Notification notification = new NotificationCompat.Builder(this, channelId)
                    .setSmallIcon(R.drawable.notification)
                    .setContentTitle("Telegram")
                    .setContentText("Аккаунты на связи")
                    .setPriority(NotificationCompat.PRIORITY_MIN)
                    .setOngoing(true)
                    .build();
            if (Build.VERSION.SDK_INT >= 29) {
                startForeground(7331, notification, ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC);
            } else {
                startForeground(7331, notification);
            }
        } catch (Throwable ignore) {

        }
    }

    // Android 15+: should the system still time the service out, stop it at once (otherwise the
    // app is killed with ForegroundServiceDidNotStopInTimeException).
    public void onTimeout(int startId, int fgsType) {
        stopForeground(true);
        stopSelf();
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        return START_STICKY;
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    public void onDestroy() {
        super.onDestroy();
        SharedPreferences preferences = MessagesController.getGlobalNotificationsSettings();
        if (preferences.getBoolean("pushService", true)) {
            Intent intent = new Intent("org.telegram.start");
            intent.setPackage(getPackageName());
            sendBroadcast(intent);
        }
    }
}
