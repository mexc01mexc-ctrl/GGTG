package org.telegram.messenger;

import org.telegram.tgnet.ConnectionsManager;
import org.telegram.tgnet.TLRPC;

import java.util.ArrayList;

/**
 * Staged account start-up (same idea as ggtg-macos Domain::startNextBatch): activated accounts
 * are brought up a few at a time instead of all at once. The next batch starts when the
 * previous one is connected, or after a timeout, so a dead proxy cannot stall the queue.
 * Empty slots are never initialized.
 */
public class AccountStartup {

    private static final int BATCH_SIZE = 4;
    private static final long CHECK_DELAY = 500;
    private static final long BATCH_TIMEOUT = 15000;

    private static final boolean[] started = new boolean[UserConfig.MAX_ACCOUNT_COUNT];
    private static final ArrayList<Integer> pending = new ArrayList<>();
    private static final ArrayList<Integer> batch = new ArrayList<>();
    private static long batchStartedAt;

    public static boolean isStarted(int account) {
        return account >= 0 && account < started.length && started[account];
    }

    /** Must be called on the main thread after every UserConfig has been loaded. */
    public static void begin() {
        pending.clear();
        for (int a = 0; a < UserConfig.MAX_ACCOUNT_COUNT; a++) {
            if (a != UserConfig.selectedAccount && UserConfig.getInstance(a).isClientActivated() && !UserConfig.getInstance(a).ncgOffline) {
                pending.add(a);
            }
        }
        startNextBatch(UserConfig.getInstance(UserConfig.selectedAccount).isClientActivated() ? UserConfig.selectedAccount : -1);
    }

    /** Idempotent: brings one account's controllers and connection up. */
    public static void start(int a) {
        if (a < 0 || a >= started.length || started[a]) {
            return;
        }
        started[a] = true;
        app.ggtg.GgtgManualSends.track(a);
        MessagesController.getInstance(a);
        ConnectionsManager.getInstance(a);
        TLRPC.User user = UserConfig.getInstance(a).getCurrentUser();
        if (user != null) {
            MessagesController.getInstance(a).putUser(user, true);
            SendMessagesHelper.getInstance(a).checkUnsentMessages();
        }
        if (UserConfig.getInstance(a).isClientActivated()) {
            ContactsController.getInstance(a).checkAppAccount();
        }
        DownloadController.getInstance(a);
    }

    private static void startNextBatch(int first) {
        batch.clear();
        if (first >= 0) {
            start(first);
            batch.add(first);
        }
        while (!pending.isEmpty() && batch.size() < BATCH_SIZE) {
            int a = pending.remove(0);
            start(a);
            batch.add(a);
        }
        batchStartedAt = System.currentTimeMillis();
        if (!pending.isEmpty()) {
            AndroidUtilities.runOnUIThread(AccountStartup::check, CHECK_DELAY);
        }
    }

    private static void check() {
        boolean waiting = false;
        for (int a : batch) {
            int state = ConnectionsManager.getInstance(a).getConnectionState();
            if (state == ConnectionsManager.ConnectionStateConnecting
                    || state == ConnectionsManager.ConnectionStateWaitingForNetwork
                    || state == ConnectionsManager.ConnectionStateConnectingToProxy) {
                waiting = true;
                break;
            }
        }
        if (!waiting || System.currentTimeMillis() - batchStartedAt >= BATCH_TIMEOUT) {
            startNextBatch(-1);
        } else {
            AndroidUtilities.runOnUIThread(AccountStartup::check, CHECK_DELAY);
        }
    }
}
