package org.telegram.ui.Components;

import android.app.AlertDialog;
import android.content.Context;

import org.telegram.messenger.LocaleController;
import org.telegram.messenger.MessagesStorage;
import org.telegram.messenger.NotificationCenter;
import org.telegram.messenger.R;
import org.telegram.messenger.UserConfig;

import java.util.ArrayList;
import java.util.Collections;
import java.util.LinkedHashSet;
import java.util.List;

import static org.telegram.messenger.AndroidUtilities.dp;

// ng: business logic shared between the account-switcher popup (AccountSwitcherView) and the
// folder-aware "Аккаунты" section on the Edit Profile screen (UserInfoActivity) - grouping/sort
// algorithm, folder CRUD, and the long-press dialogs. Kept independent of any specific list/adapter
// so both screens can render the same underlying state their own way.
public class AccountFolderLogic {

    /** ng: ordered top-level entities (ungrouped accounts as Integer, folder names as String) -
     * pinned first, then unpinned, each bucket by manual order (falling back to login time). */
    public static List<Object> buildTopLevelOrder(List<Integer> accounts) {
        List<Integer> ungrouped = new ArrayList<>();
        List<String> folderNames = new ArrayList<>();
        for (String folder : groupByFolder(accounts).keySet()) {
            folderNames.add(folder);
        }
        for (int acc : accounts) {
            if (UserConfig.getInstance(acc).ncgTrashed) continue; // ggtg: «Корзина» is hidden from Telegram's lists
            if (UserConfig.getInstance(acc).ncgFolder.isEmpty()) ungrouped.add(acc);
        }

        List<Object> topLevel = new ArrayList<>();
        topLevel.addAll(ungrouped);
        topLevel.addAll(folderNames);
        Collections.sort(topLevel, (a, b) -> {
            boolean pa = a instanceof Integer ? UserConfig.getInstance((Integer) a).ncgPinned : AccountFolderStore.isPinned((String) a);
            boolean pb = b instanceof Integer ? UserConfig.getInstance((Integer) b).ncgPinned : AccountFolderStore.isPinned((String) b);
            if (pa != pb) return pa ? -1 : 1;
            int oa = a instanceof Integer ? UserConfig.getInstance((Integer) a).ncgOrder : AccountFolderStore.getOrder((String) a);
            int ob = b instanceof Integer ? UserConfig.getInstance((Integer) b).ncgOrder : AccountFolderStore.getOrder((String) b);
            if (oa == -1 && ob == -1) {
                long la = a instanceof Integer ? UserConfig.getInstance((Integer) a).loginTime : 0;
                long lb = b instanceof Integer ? UserConfig.getInstance((Integer) b).loginTime : 0;
                return Long.compare(la, lb);
            }
            if (oa == -1) return 1;
            if (ob == -1) return -1;
            return Integer.compare(oa, ob);
        });
        return topLevel;
    }

    public static java.util.LinkedHashMap<String, List<Integer>> groupByFolder(List<Integer> accounts) {
        java.util.LinkedHashMap<String, List<Integer>> byFolder = new java.util.LinkedHashMap<>();
        for (int acc : accounts) {
            UserConfig uc = UserConfig.getInstance(acc);
            if (uc.ncgTrashed) continue;
            if (!uc.ncgFolder.isEmpty()) {
                byFolder.computeIfAbsent(uc.ncgFolder, k -> new ArrayList<>()).add(acc);
            }
        }
        return byFolder;
    }

    public static List<Integer> sortFolderMembers(List<Integer> members) {
        List<Integer> sorted = new ArrayList<>(members);
        Collections.sort(sorted, (a, b) -> {
            UserConfig ca = UserConfig.getInstance(a);
            UserConfig cb = UserConfig.getInstance(b);
            if (ca.ncgPinned != cb.ncgPinned) return ca.ncgPinned ? -1 : 1;
            if (ca.ncgOrder == -1 && cb.ncgOrder == -1) return Integer.compare(ca.loginTime, cb.loginTime);
            if (ca.ncgOrder == -1) return 1;
            if (cb.ncgOrder == -1) return -1;
            return Integer.compare(ca.ncgOrder, cb.ncgOrder);
        });
        return sorted;
    }

    // Live unread badges: subscribe to every activated account's counter event.
    public static void observeUnread(NotificationCenter.NotificationCenterDelegate delegate, boolean add) {
        for (int a = 0; a < UserConfig.MAX_ACCOUNT_COUNT; a++) {
            if (!UserConfig.getInstance(a).isClientActivated()) continue;
            if (add) {
                NotificationCenter.getInstance(a).addObserver(delegate, NotificationCenter.dialogsUnreadCounterChanged);
            } else {
                NotificationCenter.getInstance(a).removeObserver(delegate, NotificationCenter.dialogsUnreadCounterChanged);
            }
        }
    }

    public static int folderUnreadCount(List<Integer> members) {
        int count = 0;
        for (int acc : members) {
            count += MessagesStorage.getInstance(acc).getMainUnreadCount();
        }
        return count;
    }

    public static List<String> existingFolderNames() {
        LinkedHashSet<String> names = new LinkedHashSet<>();
        for (int a = 0; a < UserConfig.MAX_ACCOUNT_COUNT; a++) {
            UserConfig uc = UserConfig.getInstance(a);
            if (uc.isClientActivated() && !uc.ncgFolder.isEmpty()) {
                names.add(uc.ncgFolder);
            }
        }
        return new ArrayList<>(names);
    }

    public static void assignFolder(int account, String folder, Runnable onChanged) {
        UserConfig uc = UserConfig.getInstance(account);
        uc.ncgFolder = folder;
        uc.saveConfig(false);
        if (onChanged != null) onChanged.run();
    }

    public static void renameFolder(String oldName, String newName, Runnable onChanged) {
        if (newName.isEmpty() || newName.equals(oldName)) return;
        for (int a = 0; a < UserConfig.MAX_ACCOUNT_COUNT; a++) {
            UserConfig uc = UserConfig.getInstance(a);
            if (uc.isClientActivated() && uc.ncgFolder.equals(oldName)) {
                uc.ncgFolder = newName;
                uc.saveConfig(false);
            }
        }
        AccountFolderStore.rename(oldName, newName);
        if (onChanged != null) onChanged.run();
    }

    public static void deleteFolder(String name, Runnable onChanged) {
        for (int a = 0; a < UserConfig.MAX_ACCOUNT_COUNT; a++) {
            UserConfig uc = UserConfig.getInstance(a);
            if (uc.isClientActivated() && uc.ncgFolder.equals(name)) {
                uc.ncgFolder = "";
                uc.saveConfig(false);
            }
        }
        AccountFolderStore.delete(name);
        if (onChanged != null) onChanged.run();
    }

    public static void toggleFolder(String name, Runnable onChanged) {
        boolean expanding = !AccountFolderStore.isExpanded(name);
        if (expanding) {
            // ng: accordion - only one folder open at a time
            for (String other : existingFolderNames()) {
                if (!other.equals(name)) AccountFolderStore.setExpanded(other, false);
            }
        }
        AccountFolderStore.setExpanded(name, expanding);
        if (onChanged != null) onChanged.run();
    }

    public static void showFolderMenu(Context context, int account, boolean includePinOption, Runnable onChanged) {
        UserConfig uc = UserConfig.getInstance(account);
        List<String> folders = existingFolderNames();
        folders.remove(uc.ncgFolder);

        List<String> labels = new ArrayList<>(folders);
        labels.add(LocaleController.getString(R.string.NicegramNewFolder));
        if (!uc.ncgFolder.isEmpty()) {
            labels.add(LocaleController.getString(R.string.NicegramRemoveFromFolder));
        }
        int pinIndex = -1;
        if (includePinOption) {
            pinIndex = labels.size();
            labels.add(LocaleController.getString(uc.ncgPinned ? R.string.NicegramUnpin : R.string.NicegramPin));
        }
        final int finalPinIndex = pinIndex;

        new AlertDialog.Builder(context)
                .setTitle(LocaleController.getString(R.string.NicegramMoveToFolder))
                .setItems(labels.toArray(new CharSequence[0]), (dialog, which) -> {
                    if (which < folders.size()) {
                        assignFolder(account, folders.get(which), onChanged);
                    } else if (which == folders.size()) {
                        promptNewFolder(context, account, onChanged);
                    } else if (finalPinIndex >= 0 && which == finalPinIndex) {
                        uc.ncgPinned = !uc.ncgPinned;
                        uc.saveConfig(false);
                        if (onChanged != null) onChanged.run();
                    } else {
                        assignFolder(account, "", onChanged);
                    }
                })
                .show();
    }

    public static void showFolderHeaderMenu(Context context, String folder, boolean includePinOption, Runnable onChanged) {
        List<String> labels = new ArrayList<>();
        labels.add(LocaleController.getString(R.string.NicegramRenameFolder));
        labels.add(LocaleController.getString(R.string.NicegramDeleteFolder));
        if (includePinOption) {
            labels.add(LocaleController.getString(AccountFolderStore.isPinned(folder) ? R.string.NicegramUnpin : R.string.NicegramPin));
        }
        new AlertDialog.Builder(context)
                .setTitle(folder)
                .setItems(labels.toArray(new CharSequence[0]), (dialog, which) -> {
                    if (which == 0) {
                        promptRenameFolder(context, folder, onChanged);
                    } else if (which == 1) {
                        deleteFolder(folder, onChanged);
                    } else {
                        AccountFolderStore.setPinned(folder, !AccountFolderStore.isPinned(folder));
                        if (onChanged != null) onChanged.run();
                    }
                })
                .show();
    }

    public static void promptNewFolder(Context context, int account, Runnable onChanged) {
        android.widget.EditText input = new android.widget.EditText(context);
        input.setSingleLine(true);
        input.setPadding(dp(24), dp(8), dp(24), dp(8));
        new AlertDialog.Builder(context)
                .setTitle(LocaleController.getString(R.string.NicegramNewFolder))
                .setView(input)
                .setPositiveButton(LocaleController.getString(R.string.Done), (dialog, which) -> {
                    String name = input.getText().toString().trim();
                    if (!name.isEmpty()) {
                        assignFolder(account, name, onChanged);
                    }
                })
                .setNegativeButton(LocaleController.getString(R.string.Cancel), null)
                .show();
    }

    public static void promptRenameFolder(Context context, String oldName, Runnable onChanged) {
        android.widget.EditText input = new android.widget.EditText(context);
        input.setSingleLine(true);
        input.setText(oldName);
        input.setSelection(oldName.length());
        input.setPadding(dp(24), dp(8), dp(24), dp(8));
        new AlertDialog.Builder(context)
                .setTitle(LocaleController.getString(R.string.NicegramRenameFolder))
                .setView(input)
                .setPositiveButton(LocaleController.getString(R.string.Done), (dialog, which) -> renameFolder(oldName, input.getText().toString().trim(), onChanged))
                .setNegativeButton(LocaleController.getString(R.string.Cancel), null)
                .show();
    }
}
