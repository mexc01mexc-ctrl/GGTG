package org.telegram.ui.Components;

import android.content.Context;
import android.content.SharedPreferences;

import org.telegram.messenger.ApplicationLoader;

import java.util.Collections;
import java.util.LinkedHashSet;
import java.util.Set;

// ng: lightweight persisted metadata for account-switcher folders. A folder itself isn't stored
// as a separate list anywhere - it exists exactly when >=1 account carries that name in its own
// UserConfig.ncgFolder field. This only remembers the extra per-folder state that isn't owned by
// any single account: whether the folder is pinned, expanded, and its manual sort order.
public class AccountFolderStore {
    private static final String PREFS = "ncg_account_folders";

    private static SharedPreferences prefs() {
        return ApplicationLoader.applicationContext.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
    }

    private static Set<String> stringSet(String key) {
        return new LinkedHashSet<>(prefs().getStringSet(key, Collections.emptySet()));
    }

    public static boolean isPinned(String folder) {
        return stringSet("pinned").contains(folder);
    }

    public static void setPinned(String folder, boolean pinned) {
        Set<String> set = stringSet("pinned");
        if (pinned) set.add(folder); else set.remove(folder);
        prefs().edit().putStringSet("pinned", set).apply();
    }

    public static boolean isExpanded(String folder) {
        return stringSet("expanded").contains(folder);
    }

    public static void setExpanded(String folder, boolean expanded) {
        Set<String> set = stringSet("expanded");
        if (expanded) set.add(folder); else set.remove(folder);
        prefs().edit().putStringSet("expanded", set).apply();
    }

    public static int getOrder(String folder) {
        return prefs().getInt("order_" + folder, -1);
    }

    public static void setOrder(String folder, int order) {
        prefs().edit().putInt("order_" + folder, order).apply();
    }

    /** ng: carries a folder's pin/expand/order state over to a new name after a rename. */
    public static void rename(String oldName, String newName) {
        boolean pinned = isPinned(oldName);
        boolean expanded = isExpanded(oldName);
        int order = getOrder(oldName);
        delete(oldName);
        setPinned(newName, pinned);
        setExpanded(newName, expanded);
        if (order != -1) setOrder(newName, order);
    }

    public static void delete(String folder) {
        setPinned(folder, false);
        setExpanded(folder, false);
        prefs().edit().remove("order_" + folder).apply();
    }
}
