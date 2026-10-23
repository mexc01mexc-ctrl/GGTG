package org.telegram.ui.Components;

import android.content.Context;
import android.content.SharedPreferences;

import org.telegram.messenger.ApplicationLoader;

import java.util.ArrayList;
import java.util.Collections;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Set;

// ng: named, consumable proxy lists for the "choose proxy source" add-account flow. Each list is
// just a newline-joined "host:port:username:password" string keyed by name - popping a proxy off
// a list removes it permanently, so it can never be handed to a second account.
public class GgtgProxyPoolStore {
    private static final String PREFS = "ncg_proxy_pools";

    public static class Entry {
        public final String host;
        public final int port;
        public final String username;
        public final String password;

        public Entry(String host, int port, String username, String password) {
            this.host = host;
            this.port = port;
            this.username = username;
            this.password = password;
        }

        public static Entry parse(String line) {
            String[] parts = line.trim().split(":", 4);
            if (parts.length != 4 || parts[0].isEmpty()) {
                return null;
            }
            int port;
            try {
                port = Integer.parseInt(parts[1].trim());
            } catch (NumberFormatException e) {
                return null;
            }
            return new Entry(parts[0].trim(), port, parts[2].trim(), parts[3].trim());
        }

        String serialize() {
            return host + ":" + port + ":" + username + ":" + password;
        }
    }

    private static SharedPreferences prefs() {
        return ApplicationLoader.applicationContext.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
    }

    public static List<String> getListNames() {
        return new ArrayList<>(new LinkedHashSet<>(prefs().getStringSet("names", Collections.emptySet())));
    }

    private static List<String> rawLines(String name) {
        String joined = prefs().getString("list_" + name, "");
        List<String> lines = new ArrayList<>();
        if (!joined.isEmpty()) {
            Collections.addAll(lines, joined.split("\n"));
        }
        return lines;
    }

    private static void saveLines(String name, List<String> lines) {
        prefs().edit().putString("list_" + name, android.text.TextUtils.join("\n", lines)).apply();
    }

    public static void addProxiesToList(String name, String rawPastedText) {
        Set<String> names = new LinkedHashSet<>(prefs().getStringSet("names", Collections.emptySet()));
        names.add(name);
        prefs().edit().putStringSet("names", names).apply();

        List<String> lines = rawLines(name);
        for (String raw : rawPastedText.split("\n")) {
            if (raw.trim().isEmpty()) continue;
            Entry entry = Entry.parse(raw);
            if (entry != null) {
                lines.add(entry.serialize());
            }
        }
        saveLines(name, lines);
    }

    public static int getCount(String name) {
        return rawLines(name).size();
    }

    public static Entry peekFirst(String name) {
        List<String> lines = rawLines(name);
        if (lines.isEmpty()) return null;
        return Entry.parse(lines.get(0));
    }

    public static Entry popFirst(String name) {
        List<String> lines = rawLines(name);
        if (lines.isEmpty()) return null;
        String first = lines.remove(0);
        saveLines(name, lines);
        return Entry.parse(first);
    }

    // ng: list management for the GGTG "Прокси" section (GGTG Desktop's proxy list editor)
    public static String getListText(String name) {
        return android.text.TextUtils.join("\n", rawLines(name));
    }

    /** Replaces the list's content with the valid host:port:user:pass lines of [rawText]; returns how many were kept. */
    public static int replaceList(String name, String rawText) {
        Set<String> names = new LinkedHashSet<>(prefs().getStringSet("names", Collections.emptySet()));
        names.add(name);
        prefs().edit().putStringSet("names", names).apply();
        List<String> lines = new ArrayList<>();
        for (String raw : rawText.split("\n")) {
            if (raw.trim().isEmpty()) continue;
            Entry entry = Entry.parse(raw);
            if (entry != null) lines.add(entry.serialize());
        }
        saveLines(name, lines);
        return lines.size();
    }

    public static void createList(String name) {
        Set<String> names = new LinkedHashSet<>(prefs().getStringSet("names", Collections.emptySet()));
        names.add(name);
        prefs().edit().putStringSet("names", names).apply();
    }

    public static void renameList(String oldName, String newName) {
        if (newName.isEmpty() || newName.equals(oldName)) return;
        String text = getListText(oldName);
        deleteList(oldName);
        replaceList(newName, text);
    }

    public static synchronized List<Entry> getListEntries(String name) {
        List<String> lines = rawLines(name);
        List<Entry> result = new ArrayList<>();
        for (String line : lines) {
            Entry e = Entry.parse(line);
            if (e != null) {
                result.add(e);
            }
        }
        return result;
    }

    public static synchronized boolean removeEntry(String name, String host, int port) {
        List<String> lines = rawLines(name);
        for (int i = 0; i < lines.size(); i++) {
            Entry e = Entry.parse(lines.get(i));
            if (e != null && e.host.equalsIgnoreCase(host) && e.port == port) {
                lines.remove(i);
                saveLines(name, lines);
                return true;
            }
        }
        return false;
    }

    public static void deleteList(String name) {
        Set<String> names = new LinkedHashSet<>(prefs().getStringSet("names", Collections.emptySet()));
        names.remove(name);
        prefs().edit().putStringSet("names", names).remove("list_" + name).apply();
    }

    public static synchronized String toJson() {
        org.json.JSONArray array = new org.json.JSONArray();
        for (String name : getListNames()) {
            try {
                org.json.JSONObject list = new org.json.JSONObject();
                list.put("name", name);
                org.json.JSONArray entries = new org.json.JSONArray();
                for (String line : rawLines(name)) {
                    Entry e = Entry.parse(line);
                    if (e != null) {
                        org.json.JSONObject item = new org.json.JSONObject();
                        item.put("host", e.host);
                        item.put("port", e.port);
                        item.put("user", e.username);
                        item.put("password", e.password);
                        entries.put(item);
                    }
                }
                list.put("entries", entries);
                array.put(list);
            } catch (Throwable ignored) {
            }
        }
        return array.toString();
    }

    public static synchronized int mergeJson(String jsonStr) {
        int addedCount = 0;
        try {
            org.json.JSONArray array = new org.json.JSONArray(jsonStr);
            for (int i = 0; i < array.length(); i++) {
                org.json.JSONObject listObj = array.getJSONObject(i);
                String name = listObj.optString("name").trim();
                if (name.isEmpty()) continue;
                org.json.JSONArray entries = listObj.optJSONArray("entries");
                if (entries == null) continue;

                StringBuilder sb = new StringBuilder();
                List<String> existing = rawLines(name);
                Set<String> keys = new java.util.HashSet<>();
                for (String line : existing) {
                    Entry e = Entry.parse(line);
                    if (e != null) keys.add(e.host + ":" + e.port + ":" + e.username + ":" + e.password);
                }
                for (int j = 0; j < entries.length(); j++) {
                    org.json.JSONObject item = entries.getJSONObject(j);
                    String host = item.optString("host").trim();
                    int port = item.optInt("port");
                    String user = item.optString("user");
                    String pass = item.optString("password");
                    String key = host + ":" + port + ":" + user + ":" + pass;
                    if (!keys.contains(key) && !host.isEmpty() && port > 0) {
                        keys.add(key);
                        sb.append(key).append("\n");
                        addedCount++;
                    }
                }
                if (sb.length() > 0) {
                    addProxiesToList(name, sb.toString());
                } else if (!getListNames().contains(name)) {
                    Set<String> names = new LinkedHashSet<>(prefs().getStringSet("names", Collections.emptySet()));
                    names.add(name);
                    prefs().edit().putStringSet("names", names).apply();
                }
            }
        } catch (Throwable t) {
            org.telegram.messenger.FileLog.e(t);
        }
        return addedCount;
    }
}

