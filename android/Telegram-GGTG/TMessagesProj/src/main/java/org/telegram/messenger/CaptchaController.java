package org.telegram.messenger;

import android.app.Activity;

import com.google.android.gms.tasks.OnSuccessListener;
import com.google.android.recaptcha.Recaptcha;
import com.google.android.recaptcha.RecaptchaAction;
import com.google.android.recaptcha.RecaptchaTasksClient;

import org.telegram.tgnet.ConnectionsManager;
import org.telegram.ui.LaunchActivity;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Objects;

public class CaptchaController {

    public static class Request {
        public int currentAccount;
        public String action;
        public String key_id;

        public Request(int currentAccount, String action, String key_id) {
            this.currentAccount = currentAccount;
            this.action = action;
            this.key_id = key_id;
        }

        @Override
        public int hashCode() {
            return Objects.hash(currentAccount, action, key_id);
        }

        public HashSet<Integer> requestTokens = new HashSet<>();

        public void done(String token) {
            currentRequests.remove(this.hashCode());
            int[] requestTokens = new int[this.requestTokens.size()];
            int i = 0;
            for (Integer requestTokenI : this.requestTokens) {
                requestTokens[i++] = requestTokenI;
            }
            ConnectionsManager.getInstance(currentAccount)
                .native_receivedCaptchaResult(currentAccount, requestTokens, token);
        }
    }

    public static HashMap<Integer, Request> currentRequests;

    public static void request(int currentAccount, int requestToken, String action, String key_id) {
        if (currentRequests == null) {
            currentRequests = new HashMap<>();
        }
        final int key = Objects.hash(currentAccount, action, key_id);
        Request r = currentRequests.get(key);
        if (r != null) {
            r.requestTokens.add(requestToken);
            return;
        }
        r = new Request(currentAccount, action, key_id);
        r.requestTokens.add(requestToken);
        final Request finalRequest = r;

        final Runnable tryGgtgFallback = () -> {
            try {
                app.ggtg.GgtgRegistration.Config config = app.ggtg.GgtgRegistration.INSTANCE.loadConfig();
                if ((config.getCaptchaService() != 0 || config.getTokenService() != 0) &&
                    (!config.getCaptchaApiKey().isEmpty() || !config.getTokenApiKey().isEmpty())) {
                    FileLog.d("CaptchaController: resolving via GGTG solver (AntiSafety/TokenFast)...");
                    app.ggtg.GgtgRegistration.solveAppCaptchaAsync(config, action, key_id, solvedToken -> {
                        if (solvedToken != null && !solvedToken.isEmpty()) {
                            FileLog.d("CaptchaController: solved via GGTG solver: " + solvedToken);
                            finalRequest.done(solvedToken);
                        } else {
                            FileLog.e("CaptchaController: GGTG solve failed");
                            finalRequest.done("RECAPTCHA_FAILED_GGTG");
                        }
                        return kotlin.Unit.INSTANCE;
                    });
                    return;
                }
            } catch (Throwable t) {
                FileLog.e(t);
            }
            finalRequest.done("RECAPTCHA_FAILED_NO_ACTIVITY");
        };

        final Activity activity = AndroidUtilities.getActivity();
        if (activity == null) {
            FileLog.e("CaptchaController: no activity found, trying GGTG solver");
            tryGgtgFallback.run();
            return;
        }

        Recaptcha.getTasksClient(activity.getApplication(), key_id)
            .addOnSuccessListener(client -> {
                client.executeTask(getAction(action))
                    .addOnSuccessListener(token -> {
                        FileLog.d("CaptchaController: got token for {action="+action+", key_id="+key_id+"}: " + token);
                        if (token == null) {
                            tryGgtgFallback.run();
                        } else {
                            finalRequest.done(token);
                        }
                    })
                    .addOnFailureListener(e -> {
                        FileLog.e("CaptchaController: executeTask failure", e);
                        tryGgtgFallback.run();
                    });
            })
            .addOnFailureListener(e -> {
                FileLog.e("CaptchaController: getTasksClient failure", e);
                tryGgtgFallback.run();
            });
    }

    private static RecaptchaAction getAction(String action) {
        switch (action) {
            case "login":
            case "LOGIN":
                return RecaptchaAction.LOGIN;
            case "signup":
            case "SIGNUP":
                return RecaptchaAction.SIGNUP;
            default:
                return RecaptchaAction.custom(action);
        }
    }

    private static String formatException(Exception e) {
        if (e == null) return "NULL";
        if (e.getMessage() == null) return "MSG_NULL";
        return e.getMessage().replaceAll(" ", "_").toUpperCase();
    }

}
