package app.ggtg

// ggtg: authentic Telegram iOS (iPhone) client profile generator.
// Matches official Telegram iOS sessions with Apple device models, iOS versions,
// and Telegram iOS build numbers.
// api_id = 10840, lang_pack = "ios".
object GgtgIosProfiles {

    const val APP_ID = 10840
    const val APP_HASH = "b7137f4139934259b6cb5039360c75c8"
    const val LANG_PACK = "ios"

    class Profile(
        val deviceModel: String,
        val systemVersion: String,
        val appVersion: String,
        val cloudLang: String = "ru",
        val systemLang: String = "ru-ru"
    )

    val devices = listOf(
        "iPhone 18 Pro Max",
        "iPhone 18 Pro",
        "iPhone 18 Plus",
        "iPhone 18",
        "iPhone 17 Pro Max",
        "iPhone 17 Pro",
        "iPhone 17 Air",
        "iPhone 17",
        "iPhone 16 Pro Max",
        "iPhone 16 Pro",
        "iPhone 16 Plus",
        "iPhone 16",
        "iPhone 15 Pro Max",
        "iPhone 15 Pro",
        "iPhone 15 Plus",
        "iPhone 15",
        "iPhone 14 Pro Max",
        "iPhone 14 Pro",
        "iPhone 14",
        "iPhone 13 Pro Max",
        "iPhone 13 Pro",
        "iPhone 13"
    )

    val systems = listOf(
        "iOS 27.0.1",
        "iOS 27.0",
        "iOS 18.2",
        "iOS 18.1.1",
        "iOS 18.1",
        "iOS 18.0.1",
        "iOS 18.0",
        "iOS 17.6.1",
        "iOS 17.5.1"
    )

    val appVersions = listOf(
        "11.5.0",
        "11.4.1",
        "11.3.0",
        "11.2.2",
        "11.2.1",
        "11.1.0",
        "11.0.0",
        "10.9.1"
    )

    val langs = listOf(
        "ru" to "ru-ru",
        "en" to "en-us",
        "en" to "en-gb",
        "uk" to "uk-ua",
        "de" to "de-de",
        "es" to "es-es",
        "fr" to "fr-fr",
        "it" to "it-it",
        "tr" to "tr-tr",
        "pl" to "pl-pl"
    )

    fun randomDevice(): String = devices.random()
    fun randomSystem(): String = systems.random()
    fun randomAppVersion(): String = appVersions.random()

    fun random(phone: String = ""): Profile {
        val geo = if (phone.isNotEmpty()) GgtgGeo.matchForPhone(phone) else GgtgGeo.GeoMatch("US", -18000, "en-US", "en")
        return Profile(
            deviceModel = randomDevice(),
            systemVersion = randomSystem(),
            appVersion = randomAppVersion(),
            cloudLang = geo.cloudLang,
            systemLang = geo.systemLang.lowercase()
        )
    }
}

