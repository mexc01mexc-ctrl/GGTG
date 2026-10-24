package app.ggtg

// ggtg: random Desktop client profile (device, Windows/macOS, Telegram Desktop version, language)
// matching authentic Telegram Desktop sessions (TGEX Windows / TGEX Mac standard parameters).
// api_id = 2040, lang_pack = "tdesktop".
object GgtgDesktopProfiles {

    const val APP_ID = 2040
    const val APP_HASH = "b18441a1ff607e10a989891a5462e627"
    const val LANG_PACK = "tdesktop"

    class Profile(
        val deviceModel: String,
        val systemVersion: String,
        val appVersion: String,
        val cloudLang: String,
        val systemLang: String
    )

    val windowsSystems = listOf(
        "Windows 11 x64",
        "Windows 10 x64"
    )

    val macSystems = listOf(
        "macOS 15.1",
        "macOS 15.0.1",
        "macOS 15.0",
        "macOS 14.7.1",
        "macOS 14.7",
        "macOS 14.6.1",
        "macOS 14.5",
        "macOS 14.4.1",
        "macOS 13.6.9"
    )

    val windowsVersions = listOf(
        "7.3.1 x64",
        "7.3.0 x64",
        "7.2.9 x64",
        "7.2.8 x64",
        "7.1.5 x64",
        "7.1.0 x64",
        "7.0.8 x64",
        "6.9.4 x64",
        "6.9.0 x64",
        "6.8.2 x64"
    )

    val macVersions = listOf(
        "7.3.1",
        "7.3.0",
        "7.2.9",
        "7.2.8",
        "7.1.5",
        "7.1.0",
        "7.0.8",
        "6.9.4"
    )

    val windowsDevices = listOf(
        "FA506QM-HN124W", "G713IH-HX020T", "G513QR-HQ222TS", "TP200SA-DH01T",
        "G733QR-HG105TS", "GA401II-HE233TS", "GZ301ZC-LD123WS", "G512LI-HN097T",
        "G533QS-HF240TS", "FA566IV-HN413T", "GA401QC-HZ094TS", "G513QY-HQ032WS",
        "M6500QF-HN741WS", "FX516PR-HN109TS", "G634JY-NM054WS", "G713QM-K4215TS",
        "15-ds1010wm", "15-ec1073dx", "15ACH6", "16ACH6H", "16ARH7H",
        "FA506IC-HN075W", "FA506II-AL117T", "FA566IU-HN249T", "FA566QM-HN087TS",
        "FA577RM-HQ032WS", "FA706ICB-HX061W", "FX506HF-HN025W", "FX506HM-HN016T",
        "FX506LH-HN258T", "FX506LI-HN109TS", "FX516PCZ-HN089T", "G513IC-HN055T",
        "G513RC-HN063W", "G614JV-N4141WS", "G713IE-HX040W", "GA401QH-HZ080TS",
        "GV301QH-K5459TS", "GA503RM-LN095WS", "GU604VZ-NM050WS", "GX701GXR-EV025T",
        "UX582LR-H701TS", "UX482EGR-KA521WS", "PC 64bit"
    )

    val curatedWindows = listOf(
        "Стандартный ПК (PC 64bit)" to "PC 64bit",
        "DESKTOP-UECSK40" to "DESKTOP-UECSK40",
        "LAPTOP-V13BLVG" to "LAPTOP-V13BLVG",
        "LAPTOP-A23H4TH" to "LAPTOP-A23H4TH",
        "Lenovo Legion 5 (15ACH6)" to "15ACH6",
        "Lenovo Legion 7 (16ACH6H)" to "16ACH6H",
        "HP Pavilion Gaming (15-ec1073dx)" to "15-ec1073dx",
        "ASUS TUF Gaming A15 (FA506QM)" to "FA506QM-HN124W",
        "ASUS ROG Strix G17 (G713QM)" to "G713QM-K4215TS",
        "ASUS ROG Zephyrus G14 (GA401QC)" to "GA401QC-HZ094TS",
        "ASUS ZenBook Pro Duo (UX582LR)" to "UX582LR-H701TS"
    )

    val macDevices = listOf(
        "MacBook Pro",
        "MacBook Air",
        "iMac",
        "Mac mini",
        "Mac Studio",
        "MacBook Pro 16\"",
        "MacBook Pro 14\"",
        "MacBook Air 13\"",
        "MacBook Air 15\"",
        "Mac Pro"
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
        "pl" to "pl-pl",
        "nl" to "nl-nl",
        "cs" to "cs-cz",
        "id" to "id-id",
        "ko" to "ko-kr"
    )

    fun randomWindowsVersion(): String = windowsVersions.random()
    fun randomWindowsSystem(): String = windowsSystems.random()
    fun randomWindowsDevice(): String = windowsDevices.random()

    fun randomMacVersion(): String = macVersions.random()
    fun randomMacSystem(): String = macSystems.random()
    fun randomMacDevice(): String = macDevices.random()

    fun randomWindows(phone: String = ""): Profile {
        val geo = if (phone.isNotEmpty()) GgtgGeo.matchForPhone(phone) else GgtgGeo.GeoMatch("US", -18000, "en-US", "en")
        return Profile(
            deviceModel = randomWindowsDevice(),
            systemVersion = randomWindowsSystem(),
            appVersion = randomWindowsVersion(),
            cloudLang = geo.cloudLang,
            systemLang = geo.systemLang.lowercase()
        )
    }

    fun randomMac(phone: String = ""): Profile {
        val geo = if (phone.isNotEmpty()) GgtgGeo.matchForPhone(phone) else GgtgGeo.GeoMatch("US", -18000, "en-US", "en")
        return Profile(
            deviceModel = randomMacDevice(),
            systemVersion = randomMacSystem(),
            appVersion = randomMacVersion(),
            cloudLang = geo.cloudLang,
            systemLang = geo.systemLang.lowercase()
        )
    }

    fun random(phone: String = "", preferMac: Boolean? = null): Profile {
        val chooseMac = preferMac ?: (kotlin.random.Random.nextInt(100) < 40)
        return if (chooseMac) randomMac(phone) else randomWindows(phone)
    }
}
