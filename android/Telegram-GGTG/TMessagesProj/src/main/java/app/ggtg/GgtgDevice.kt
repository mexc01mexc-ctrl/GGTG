package app.ggtg

import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONObject
import org.telegram.messenger.UserConfig
import org.telegram.tgnet.ConnectionsManager
import java.net.Authenticator
import java.net.HttpURLConnection
import java.net.InetSocketAddress
import java.net.PasswordAuthentication
import java.net.Proxy
import java.net.URL

// ggtg: per-account device identity for Android accounts - picked from ready-made phone / language /
// time-zone presets (never typed), random for a whole selection, and language + time zone fitted to the
// IP the account's proxy goes out through. Mirrors MakeAccountDeviceBox / AutoDeviceGeo in the desktop's
// settings/bulk_accounts_actions.cpp. Model / system version / time zone reach Telegram when the account's
// connection is created (app restart); the language is applied live.
object GgtgDevice {

    /** [model] is exactly what the real client sends (Build.MANUFACTURER + Build.MODEL). */
    class Phone(val label: String, val model: String, val system: String)
    class Lang(val name: String, val system: String, val cloud: String)
    class Tz(val name: String, val seconds: Int)
    class Geo(val ip: String, val country: String, val lang: Lang, val offset: Int)

    val phones: List<Phone> = listOf(
        Phone("Samsung Galaxy S25 Ultra", "samsungSM-S938B", "SDK 35"),
        Phone("Samsung Galaxy S24 Ultra", "samsungSM-S928B", "SDK 34"),
        Phone("Samsung Galaxy S24", "samsungSM-S921B", "SDK 34"),
        Phone("Samsung Galaxy S23", "samsungSM-S911B", "SDK 34"),
        Phone("Samsung Galaxy S22", "samsungSM-S901B", "SDK 34"),
        Phone("Samsung Galaxy A55", "samsungSM-A556B", "SDK 34"),
        Phone("Samsung Galaxy A54", "samsungSM-A546B", "SDK 34"),
        Phone("Samsung Galaxy A35", "samsungSM-A356B", "SDK 34"),
        Phone("Samsung Galaxy Z Flip5", "samsungSM-F731B", "SDK 34"),
        Phone("Google Pixel 9 Pro", "GooglePixel 9 Pro", "SDK 35"),
        Phone("Google Pixel 9", "GooglePixel 9", "SDK 35"),
        Phone("Google Pixel 8 Pro", "GooglePixel 8 Pro", "SDK 35"),
        Phone("Google Pixel 8", "GooglePixel 8", "SDK 35"),
        Phone("Google Pixel 7", "GooglePixel 7", "SDK 35"),
        Phone("Google Pixel 7a", "GooglePixel 7a", "SDK 35"),
        Phone("Xiaomi 14", "Xiaomi23127PN0CG", "SDK 34"),
        Phone("Xiaomi 13", "Xiaomi2211133G", "SDK 33"),
        Phone("Redmi Note 13 Pro", "Xiaomi2312DRA50G", "SDK 34"),
        Phone("POCO F6", "Xiaomi24069PC21G", "SDK 34"),
        Phone("OnePlus 12", "OnePlusCPH2581", "SDK 34"),
        Phone("OnePlus 11", "OnePlusCPH2449", "SDK 34"),
        Phone("realme GT 6", "realmeRMX3851", "SDK 34"),
        Phone("Nothing Phone (2)", "NothingA065", "SDK 34"),
        Phone("Motorola Moto G84", "motorolaXT2347-2", "SDK 34"),
        Phone("HONOR Magic6 Pro", "HONORBVL-N49", "SDK 34"),
    )

    val curatedSystems: List<Pair<String, String>> = listOf(
        "SDK 35 (Android 15)" to "SDK 35",
        "SDK 34 (Android 14)" to "SDK 34",
        "SDK 33 (Android 13)" to "SDK 33",
        "SDK 32 (Android 12L)" to "SDK 32",
        "SDK 31 (Android 12)" to "SDK 31"
    )

    val curatedBuilds: List<String> = listOf(
        "12.10.6 (71129)",
        "12.10.5 (71059)",
        "12.10.4 (70999)",
        "12.10.3 (70899)",
        "12.10.2 (70869)",
        "12.10.1 (70389)",
        "12.10.0 (70319)",
        "12.9.2 (69919)",
        "12.8.2 (69809)",
        "12.7.0 (69109)",
        "12.6.1 (68509)"
    )

    // Android reports the system locale lower-cased ("ru-ru") and the interface language as a bare code.
    val langs: List<Lang> = listOf(
        Lang("Русский", "ru-ru", "ru"),
        Lang("Українська", "uk-ua", "uk"),
        Lang("English (US)", "en-us", "en"),
        Lang("English (UK)", "en-gb", "en"),
        Lang("Deutsch", "de-de", "de"),
        Lang("Español", "es-es", "es"),
        Lang("Français", "fr-fr", "fr"),
        Lang("Italiano", "it-it", "it"),
        Lang("Português (Brasil)", "pt-br", "pt-br"),
        Lang("Türkçe", "tr-tr", "tr"),
        Lang("Polski", "pl-pl", "pl"),
        Lang("Nederlands", "nl-nl", "nl"),
        Lang("Čeština", "cs-cz", "cs"),
        Lang("العربية", "ar-sa", "ar"),
        Lang("Bahasa Indonesia", "id-id", "id"),
        Lang("한국어", "ko-kr", "ko"),
    )

    data class GeoMatch(
        val country: String,
        val tzOffset: Int,
        val systemLang: String,
        val cloudLang: String
    )

    private val phoneTable = listOf(
        Triple("998", "UZ", Pair(18000, "ru-ru" to "ru")),
        Triple("996", "KG", Pair(21600, "ru-ru" to "ru")),
        Triple("995", "GE", Pair(14400, "ru-ru" to "ru")),
        Triple("994", "AZ", Pair(14400, "ru-ru" to "ru")),
        Triple("992", "TJ", Pair(18000, "ru-ru" to "ru")),
        Triple("380", "UA", Pair(7200, "uk-ua" to "uk")),
        Triple("375", "BY", Pair(10800, "ru-ru" to "ru")),
        Triple("374", "AM", Pair(14400, "ru-ru" to "ru")),
        Triple("373", "MD", Pair(7200, "ru-ru" to "ru")),
        Triple("420", "CZ", Pair(3600, "cs-cz" to "cs")),
        Triple("234", "NG", Pair(3600, "en-us" to "en")),
        Triple("77",  "KZ", Pair(18000, "ru-ru" to "ru")),
        Triple("84",  "VN", Pair(25200, "vi-vn" to "vi")),
        Triple("91",  "IN", Pair(19800, "hi-in" to "hi")),
        Triple("90",  "TR", Pair(10800, "tr-tr" to "tr")),
        Triple("63",  "PH", Pair(28800, "en-us" to "en")),
        Triple("62",  "ID", Pair(25200, "id-id" to "id")),
        Triple("55",  "BR", Pair(-10800, "pt-br" to "pt-br")),
        Triple("49",  "DE", Pair(3600, "de-de" to "de")),
        Triple("48",  "PL", Pair(3600, "pl-pl" to "pl")),
        Triple("44",  "GB", Pair(0, "en-gb" to "en")),
        Triple("39",  "IT", Pair(3600, "it-it" to "it")),
        Triple("34",  "ES", Pair(3600, "es-es" to "es")),
        Triple("33",  "FR", Pair(3600, "fr-fr" to "fr")),
        Triple("31",  "NL", Pair(3600, "nl-nl" to "nl")),
        Triple("7",   "RU", Pair(10800, "ru-ru" to "ru")),
        Triple("1",   "US", Pair(-18000, "en-us" to "en"))
    )

    fun matchForPhone(rawPhone: String): GeoMatch? {
        val digits = rawPhone.filter { it.isDigit() }
        if (digits.isEmpty()) return null
        val match = GgtgGeo.matchForPhone(rawPhone)
        return GeoMatch(match.country, match.tzOffset, match.systemLang.lowercase(), match.cloudLang)
    }

    val zones: List<Tz> = listOf(
        Tz("Лондон", 0), Tz("Берлин, Париж, Рим", 3600), Tz("Киев, Афины", 2 * 3600),
        Tz("Москва, Стамбул", 3 * 3600), Tz("Дубай", 4 * 3600), Tz("Ташкент", 5 * 3600),
        Tz("Дели", 5 * 3600 + 1800), Tz("Алматы", 6 * 3600), Tz("Бангкок, Джакарта", 7 * 3600),
        Tz("Пекин, Сингапур", 8 * 3600), Tz("Токио, Сеул", 9 * 3600), Tz("Сидней", 10 * 3600),
        Tz("Нью-Йорк", -5 * 3600), Tz("Чикаго", -6 * 3600), Tz("Денвер", -7 * 3600),
        Tz("Лос-Анджелес", -8 * 3600), Tz("Сан-Паулу", -3 * 3600),
    )

    fun formatOffset(seconds: Int): String {
        val minutes = Math.abs(seconds) / 60
        return "%s%02d:%02d".format(if (seconds < 0) "-" else "+", minutes / 60, minutes % 60)
    }

    fun normalizeGluedBrand(model: String): String {
        val brands = listOf("Samsung", "Motorola", "Xiaomi", "Huawei", "Realme", "Google", "OnePlus", "Nothing", "Honor")
        for (b in brands) {
            if (model.startsWith(b, ignoreCase = true) && model.length > b.length) {
                val charAfter = model[b.length]
                if (charAfter.isUpperCase() || charAfter.isDigit()) {
                    val brandProper = b.replaceFirstChar { it.uppercase() }
                    return brandProper + " " + model.substring(b.length)
                }
            }
        }
        return model
    }

    fun phoneLabel(model: String, system: String): String {
        val clean = normalizeGluedBrand(model)
        val knownPhone = phones.firstOrNull { it.model == model || it.model == clean || it.label == model }
        if (knownPhone != null) {
            return knownPhone.label + (if (system.isNotEmpty()) " · $system" else "")
        }
        val knownWin = GgtgDesktopProfiles.curatedWindows.firstOrNull { it.second == model || it.first == model }
        if (knownWin != null) {
            return knownWin.first + (if (system.isNotEmpty()) " · $system" else "")
        }
        return clean + (if (system.isNotEmpty()) " · $system" else "")
    }

    fun langLabel(system: String, cloud: String): String =
        langs.firstOrNull { it.system == system && it.cloud == cloud }?.let { "${it.name} (${it.system})" } ?: "$system / $cloud"

    fun zoneLabel(seconds: Int): String =
        (zones.firstOrNull { it.seconds == seconds }?.let { it.name + " " } ?: "") + "UTC" + formatOffset(seconds)

    // ---- saving ----

    /** Empty strings / [tz] null = back to the phone's real value. Language is applied live, the rest on restart. */
    fun save(
        slot: Int,
        model: String,
        system: String,
        sysLang: String,
        cloudLang: String,
        tz: Int?,
        appVersion: String? = null,
        langPack: String = "android",
        appId: Int = 6,
        appHash: String = "eb06d4abfb49dc3eeb1aeb98ae0f581e"
    ) {
        val uc = UserConfig.getInstance(slot)
        if (appVersion != null) uc.ncgAppVersion = appVersion
        uc.ncgDeviceModel = model
        uc.ncgSystemVersion = system
        uc.ncgSystemLang = sysLang
        uc.ncgCloudLang = cloudLang
        uc.ncgTzSet = tz != null
        uc.ncgTzOffset = tz ?: 0
        uc.ncgLangPack = langPack
        uc.ncgAppId = appId
        uc.ncgAppHash = appHash
        uc.saveConfig(false)
        try {
            ConnectionsManager.getInstance(slot).reinit()
        } catch (_: Throwable) { }
        if (uc.isClientActivated) {
            if (cloudLang.isNotEmpty()) ConnectionsManager.native_setLangCode(slot, cloudLang)
            if (sysLang.isNotEmpty()) ConnectionsManager.native_setSystemLangCode(slot, sysLang)
        }
    }

    fun isCustom(slot: Int): Boolean {
        val uc = UserConfig.getInstance(slot)
        return uc.ncgDeviceModel.isNotEmpty() || uc.ncgSystemLang.isNotEmpty() || uc.ncgTzSet
    }

    fun getDeviceJson(slot: Int): JSONObject? {
        val uc = UserConfig.getInstance(slot)
        if (uc.ncgDeviceModel.isNullOrEmpty()) return null
        return JSONObject().apply {
            put("deviceModel", uc.ncgDeviceModel)
            put("systemVersion", uc.ncgSystemVersion)
            put("appVersion", uc.ncgAppVersion)
            put("systemLangCode", uc.ncgSystemLang)
            put("cloudLangCode", uc.ncgCloudLang)
            put("tzOffset", uc.ncgTzOffset)
            put("langPackName", uc.ncgLangPack)
            put("apiId", uc.ncgAppId)
            put("apiHash", uc.ncgAppHash)
        }
    }

    /** A random Android device and authentic version; language and time zone are matched to phone/proxy. */
    fun randomize(slot: Int): Phone {
        val uc = UserConfig.getInstance(slot)
        val profile = GgtgAndroidProfiles.random()
        val phone = uc.currentUser?.phone.orEmpty()
        val phoneGeo = if (phone.isNotEmpty()) matchForPhone(phone) else null
        val defSysLang = phoneGeo?.systemLang ?: "en-us"
        val defCloudLang = phoneGeo?.cloudLang ?: "en"
        val tz = if (uc.ncgTzSet) uc.ncgTzOffset else phoneGeo?.tzOffset
        save(
            slot, profile.deviceModel, profile.systemVersion,
            uc.ncgSystemLang.ifEmpty { defSysLang },
            uc.ncgCloudLang.ifEmpty { defCloudLang },
            tz,
            profile.appVersion, "android", 6, "eb06d4abfb49dc3eeb1aeb98ae0f581e"
        )
        return Phone(profile.deviceModel, profile.deviceModel, profile.systemVersion)
    }

    // ---- language / time zone from the proxy's IP ----

    fun langForCountry(country: String): Lang {
        val code = when (country.uppercase().trim()) {
            "RU", "BY", "KZ", "KG", "UZ", "TJ", "AZ", "AM", "GE", "MD" -> "ru-ru"
            "UA" -> "uk-ua"
            "GB", "IE" -> "en-gb"
            "DE", "AT", "CH" -> "de-de"
            "ES", "MX", "AR", "CO", "CL", "PE" -> "es-es"
            "FR", "BE" -> "fr-fr"
            "IT" -> "it-it"
            "BR", "PT" -> "pt-br"
            "TR" -> "tr-tr"
            "PL" -> "pl-pl"
            "NL" -> "nl-nl"
            "CZ" -> "cs-cz"
            "SA", "AE", "EG" -> "ar-sa"
            "ID" -> "id-id"
            "KR" -> "ko-kr"
            else -> "en-us"
        }
        return langs.firstOrNull { it.system == code } ?: langs.first()
    }

    // SOCKS5 login for java.net (it asks the default Authenticator); keyed by proxy host:port.
    private val socksLogins = HashMap<String, PasswordAuthentication>()
    private val nextFree = HashMap<String, Long>()

    init {
        Authenticator.setDefault(object : Authenticator() {
            override fun getPasswordAuthentication(): PasswordAuthentication? =
                synchronized(socksLogins) { socksLogins["$requestingHost:$requestingPort"] }
        })
    }

    /** Looks up the country / UTC offset of the IP behind this account's proxy (its own IP with none). Throws with a readable message. */
    suspend fun detect(slot: Int): Geo {
        val uc = UserConfig.getInstance(slot)
        val proxied = uc.ncgProxyEnabled && uc.ncgProxyAddress.isNotEmpty()
        if (proxied && uc.ncgProxySecret.isNotEmpty()) error("MTProto-прокси нельзя использовать для определения IP (нужен SOCKS5)")
        val key = if (proxied) "${uc.ncgProxyAddress}:${uc.ncgProxyPort}" else ""
        val wait = synchronized(nextFree) {
            val now = System.currentTimeMillis()
            val start = maxOf(now, nextFree[key] ?: 0L)
            nextFree[key] = start + 1500
            start - now
        }
        if (wait > 0) delay(wait)
        return withContext(Dispatchers.IO) {
            val proxy = if (proxied) {
                if (uc.ncgProxyUsername.isNotEmpty()) synchronized(socksLogins) {
                    socksLogins[key] = PasswordAuthentication(uc.ncgProxyUsername, uc.ncgProxyPassword.toCharArray())
                }
                Proxy(Proxy.Type.SOCKS, InetSocketAddress.createUnresolved(uc.ncgProxyAddress, uc.ncgProxyPort))
            } else Proxy.NO_PROXY

            var country = ""
            var offset = 0
            var query = ""
            var success = false

            // Attempt 1: ip-api.com
            try {
                val conn = URL("http://ip-api.com/json/?fields=status,message,query,countryCode,offset").openConnection(proxy) as HttpURLConnection
                conn.connectTimeout = 12_000
                conn.readTimeout = 12_000
                try {
                    val json = JSONObject(conn.inputStream.bufferedReader().use { it.readText() })
                    if (json.optString("status") == "success") {
                        country = json.optString("countryCode")
                        offset = json.optInt("offset")
                        query = json.optString("query")
                        success = true
                    }
                } finally {
                    conn.disconnect()
                }
            } catch (_: Throwable) {}

            // Attempt 2: fallback to https://api.country.is/
            if (!success) {
                try {
                    val conn2 = URL("https://api.country.is/").openConnection(proxy) as HttpURLConnection
                    conn2.connectTimeout = 10_000
                    conn2.readTimeout = 10_000
                    try {
                        val json = JSONObject(conn2.inputStream.bufferedReader().use { it.readText() })
                        country = json.optString("country")
                        query = json.optString("ip")
                        if (country.isNotEmpty()) {
                            offset = when (country.uppercase()) {
                                "RU", "BY", "TR" -> 10800
                                "UA", "MD" -> 7200
                                "DE", "FR", "IT", "ES", "PL", "NL", "CZ" -> 3600
                                "GB", "IE", "PT" -> 0
                                "US", "CA" -> -18000
                                "KZ", "UZ" -> 18000
                                else -> 0
                            }
                            success = true
                        }
                    } finally {
                        conn2.disconnect()
                    }
                } catch (_: Throwable) {}
            }

            if (!success) {
                val phone = uc.currentUser?.phone.orEmpty()
                val match = if (phone.isNotEmpty()) matchForPhone(phone) else null
                if (match != null) {
                    return@withContext Geo("phone", match.country, langForCountry(match.country), match.tzOffset)
                }
                error("не удалось определить IP — проверьте прокси")
            }

            Geo(query, country, langForCountry(country), offset)
        }
    }

    /** Fits this account's language and time zone to its proxy's location; device model / system are kept. Returns a one-line report. */
    suspend fun autoGeo(slot: Int): String {
        val uc = UserConfig.getInstance(slot)
        val phone = uc.currentUser?.phone.orEmpty()
        val phoneMatch = if (phone.isNotEmpty()) matchForPhone(phone) else null

        val geo = try {
            detect(slot)
        } catch (t: Throwable) {
            if (phoneMatch != null) {
                Geo("phone", phoneMatch.country, langForCountry(phoneMatch.country), phoneMatch.tzOffset)
            } else {
                throw t
            }
        }
        val defPhone = phones.first()
        save(
            slot,
            uc.ncgDeviceModel.ifEmpty { defPhone.model },
            uc.ncgSystemVersion.ifEmpty { defPhone.system },
            geo.lang.system, geo.lang.cloud, geo.offset,
            uc.ncgAppVersion.ifEmpty { curatedBuilds.first() },
            "android", 6, "eb06d4abfb49dc3eeb1aeb98ae0f581e"
        )
        return "${geo.ip}, ${geo.country} → ${geo.lang.system}, UTC${formatOffset(geo.offset)}"
    }

    /**
     * A freshly added account's identity, set BEFORE its ConnectionsManager is first touched: what the export carried
     * in [device] (deviceModel / systemVersion / systemLangCode / cloudLangCode / tzOffset), otherwise a random device
     * matching [isDesktop] (Desktop or Android) with language / time zone fitted to the proxy once it answers. Returns true when [device] was used.
     */
    @JvmOverloads
    @JvmStatic
    fun assignNew(slot: Int, device: org.json.JSONObject? = null, isDesktop: Boolean = false): Boolean {
        val rawModel = device?.optString("deviceModel").orEmpty()
        if (rawModel.isNotEmpty()) {
            val devSys = device!!.optString("systemVersion")
            val isDevIos = device.optString("langPackName") == "ios" ||
                rawModel.contains("iPhone") || rawModel.contains("iPad") || devSys.contains("iOS")
            val isDevDesktop = !isDevIos && (isDesktop || device.optString("langPackName") == "tdesktop" ||
                device.optInt("apiId") == 2040 ||
                devSys.contains("Windows") || devSys.contains("macOS") || devSys.contains("OS X"))
            val isDevMac = !isDevIos && (devSys.contains("macOS") || devSys.contains("OS X") || rawModel.startsWith("Mac") || rawModel.startsWith("iMac"))

            val model = rawModel
            val system = devSys

            val langPack = device.optString("langPackName").ifEmpty {
                when {
                    isDevIos -> GgtgIosProfiles.LANG_PACK
                    isDevDesktop -> GgtgDesktopProfiles.LANG_PACK
                    else -> "android"
                }
            }
            val appId = if (device.has("apiId")) device.optInt("apiId") else when {
                isDevIos -> GgtgIosProfiles.APP_ID
                isDevDesktop -> GgtgDesktopProfiles.APP_ID
                else -> 6
            }
            val appHash = device.optString("apiHash").ifEmpty {
                when {
                    isDevIos -> GgtgIosProfiles.APP_HASH
                    isDevDesktop -> GgtgDesktopProfiles.APP_HASH
                    else -> "eb06d4abfb49dc3eeb1aeb98ae0f581e"
                }
            }
            save(
                slot, model, system,
                device.optString("systemLangCode"), device.optString("cloudLangCode"),
                if (device.has("tzOffset")) device.optInt("tzOffset") else null,
                device.optString("appVersion").ifEmpty {
                    when {
                        isDevIos -> GgtgIosProfiles.randomAppVersion()
                        isDevDesktop -> if (isDevMac) GgtgDesktopProfiles.randomMacVersion() else GgtgDesktopProfiles.randomWindowsVersion()
                        else -> GgtgAndroidProfiles.randomBuild()
                    }
                },
                langPack, appId, appHash
            )
            // an export that carried no language / time zone: fit them to the proxy
            if (device.optString("systemLangCode").isEmpty() && !device.has("tzOffset")) autoGeoAfterProxy(slot)
            return true
        }

        val uc = UserConfig.getInstance(slot)
        val phone = uc.currentUser?.phone.orEmpty()
        val phoneGeo = if (phone.isNotEmpty()) matchForPhone(phone) else null

        val sysLang = phoneGeo?.systemLang ?: "ru-ru"
        val cloudLang = phoneGeo?.cloudLang ?: "ru"
        val tz = phoneGeo?.tzOffset

        if (isDesktop) {
            val profile = GgtgDesktopProfiles.random(phone)
            save(
                slot, profile.deviceModel, profile.systemVersion,
                sysLang, cloudLang, tz,
                profile.appVersion, GgtgDesktopProfiles.LANG_PACK,
                GgtgDesktopProfiles.APP_ID, GgtgDesktopProfiles.APP_HASH
            )
            autoGeoAfterProxy(slot)
            return false
        }

        // On Android, use native Android device profile (APP_ID 6, lang_pack android) to ensure 100% platform authenticity
        val profile = GgtgAndroidProfiles.random()
        save(
            slot, profile.deviceModel, profile.systemVersion,
            sysLang, cloudLang, tz,
            profile.appVersion, "android", 6, "eb06d4abfb49dc3eeb1aeb98ae0f581e"
        )
        autoGeoAfterProxy(slot)
        return false
    }

    /**
     * When logging in or registering with a phone number, automatically generates
     * new realistic Android device parameters matched to the phone country code and geo,
     * so the real device hardware is never exposed while remaining 100% platform authentic.
     */
    @JvmStatic
    fun randomizeForPhoneLogin(slot: Int, phone: String) {
        val phoneGeo = matchForPhone(phone)
        val sysLang = phoneGeo?.systemLang ?: "ru-ru"
        val cloudLang = phoneGeo?.cloudLang ?: "ru"
        val tz = phoneGeo?.tzOffset

        val p = GgtgAndroidProfiles.random()
        save(
            slot, p.deviceModel, p.systemVersion,
            sysLang, cloudLang, tz,
            p.appVersion, "android", 6, "eb06d4abfb49dc3eeb1aeb98ae0f581e"
        )
        autoGeoAfterProxy(slot)
    }

    /** After a proxy was set or replaced: refit language / time zone to it (only when the account has a real proxy). */
    fun autoGeoAfterProxy(slot: Int) {
        val uc = UserConfig.getInstance(slot)
        if (!uc.ncgProxyEnabled) return
        kotlinx.coroutines.CoroutineScope(kotlinx.coroutines.SupervisorJob() + Dispatchers.IO).launch {
            try { autoGeo(slot) } catch (_: Throwable) { }
        }
    }
}
