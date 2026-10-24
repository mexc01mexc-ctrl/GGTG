package app.ggtg

// ggtg: random Android client profile (device, Android version, Telegram build) that Telegram shows consistently
// in the account's session list. Same steps as the reference generator: brand -> SDK of that brand -> model of that
// brand + SDK -> build. Mirrors Ggtg::AndroidProfiles in the desktop's settings/ggtg_android_profiles.h; the data is
// GgtgAndroidProfilesData.kt (tools/gen_android_profiles.py).
object GgtgAndroidProfiles {

    class Profile(val deviceModel: String, val systemVersion: String, val appVersion: String)

    private class Entry(val brand: String, val model: String, val sdk: Int)

    private val entries: List<Entry> by lazy {
        GgtgAndroidProfilesData.devices.map { line ->
            val parts = line.split('|')
            Entry(parts[0], parts[1], parts[2].toInt())
        }
    }

    /** Language codes of the catalog: cloud code to Android locale ("id" to "in-id"). */
    val langs: List<Pair<String, String>> by lazy {
        GgtgAndroidProfilesData.langs.map { line -> line.split('|').let { it[0] to it[1] } }
    }

    fun randomBuild(): String = GgtgAndroidProfilesData.builds.random()

    fun random(): Profile {
        val brand = entries.map { it.brand }.distinct().random()
        val sdk = entries.filter { it.brand == brand }.map { it.sdk }.distinct().random()
        val model = entries.filter { it.brand == brand && it.sdk == sdk }.random().model
        return Profile("$brand $model", "SDK $sdk", randomBuild())
    }
}
