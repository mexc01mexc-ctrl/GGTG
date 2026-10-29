package app.ggtg

import android.content.Context
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeoutOrNull
import org.json.JSONObject
import org.telegram.messenger.AndroidUtilities
import org.telegram.messenger.ApplicationLoader
import org.telegram.messenger.BuildVars
import org.telegram.messenger.FileLog
import org.telegram.messenger.MessagesController
import org.telegram.messenger.MessagesStorage
import org.telegram.messenger.NotificationCenter
import org.telegram.messenger.UserConfig
import org.telegram.tgnet.ConnectionsManager
import org.telegram.tgnet.TLObject
import org.telegram.tgnet.TLRPC
import org.telegram.tgnet.tl.TL_account
import org.telegram.ui.Components.GgtgProxyPoolStore
import java.io.BufferedReader
import java.io.File
import java.io.InputStreamReader
import java.io.OutputStreamWriter
import java.net.HttpURLConnection
import java.net.URL
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicInteger
import kotlin.coroutines.resume
import kotlin.random.Random

// GGTG: Auto-registration of Telegram accounts in Android via SMS services with multi-threading,
// target count, realistic human simulation (typing delays, profile bio, online status),
// proxy pool allocation, token services (TokenFast, AntiSafety), and 2FA password store.
object GgtgRegistration {

    data class ServiceInfo(
        val id: String,
        val name: String,
        val baseUrl: String,
        val webUrl: String
    )

    val SERVICES = listOf(
        ServiceInfo("hero-sms", "Hero-SMS", "https://hero-sms.com/stubs/handler_api.php", "https://hero-sms.com"),
        ServiceInfo("smsbower", "SMSBower", "https://smsbower.page/stubs/handler_api.php", "https://smsbower.app"),
        ServiceInfo("onlinesim", "OnlineSim", "https://onlinesim.io/stubs/handler_api.php", "https://onlinesim.io"),
        ServiceInfo("sms-activate", "SMS-Activate", "https://api.sms-activate.org/stubs/handler_api.php", "https://sms-activate.org"),
        ServiceInfo("5sim", "5SIM", "https://5sim.net/stubs/handler_api.php", "https://5sim.net"),
        ServiceInfo("vak-sms", "Vak-SMS", "https://vak-sms.com/stubs/handler_api.php", "https://vak-sms.com"),
        ServiceInfo("smshub", "SMSHub", "https://smshub.org/stubs/handler_api.php", "https://smshub.org"),
        ServiceInfo("grizzly", "Grizzly SMS", "https://api.grizzlysms.com/stubs/handler_api.php", "https://grizzlysms.com"),
        ServiceInfo("sms-man", "SMS-MAN", "https://api.sms-man.com/stubs/handler_api.php", "https://sms-man.com"),
        ServiceInfo("custom", "Свой сервис (API URL)", "", "")
    )

    data class CountryItem(
        val id: Int,
        val flag: String,
        val name: String,
        val phoneCode: String,
        val isoCode: String = ""
    )

    val COUNTRIES = listOf(
        CountryItem(16, "🇬🇧", "Великобритания", "+44", "GB"),
        CountryItem(187, "🇺🇸", "США", "+1", "US"),
        CountryItem(6, "🇮🇩", "Индонезия", "+62", "ID"),
        CountryItem(2, "🇰🇿", "Казахстан", "+77", "KZ"),
        CountryItem(22, "🇮🇳", "Индия", "+91", "IN"),
        CountryItem(73, "🇧🇷", "Бразилия", "+55", "BR"),
        CountryItem(10, "🇻🇳", "Вьетнам", "+84", "VN"),
        CountryItem(19, "🇳🇬", "Нигерия", "+234", "NG"),
        CountryItem(4, "🇵🇭", "Филиппины", "+63", "PH"),
        CountryItem(11, "🇰🇬", "Кыргызстан", "+996", "KG"),
        CountryItem(35, "🇦🇿", "Азербайджан", "+994", "AZ"),
        CountryItem(40, "🇺🇿", "Узбекистан", "+998", "UZ"),
        CountryItem(51, "🇧🇾", "Беларусь", "+375", "BY"),
        CountryItem(1, "🇺🇦", "Украина", "+380", "UA"),
        CountryItem(143, "🇹🇯", "Таджикистан", "+992", "TJ"),
        CountryItem(128, "🇬🇪", "Грузия", "+995", "GE"),
        CountryItem(148, "🇦🇲", "Армения", "+374", "AM"),
        CountryItem(17, "🇲🇩", "Молдова", "+373", "MD"),
        CountryItem(0, "🇷🇺", "Россия", "+7", "RU")
    )

    data class Config(
        val serviceId: String = "hero-sms",
        val apiKey: String = "",
        val customUrl: String = "",
        val countryId: Int = -1,
        val countryName: String = "",
        val proxyListName: String = "",
        val folderTag: String = "Регистрация",
        val twoFactorPassword: String = "",
        val single2faForAll: Boolean = true, // true = same password on all accounts; false = random 18-char per account
        val singleNameForAll: Boolean = false, // true = same first and last name for all accounts
        val autoRandomNames: Boolean = true, // preserved for parity: !singleNameForAll
        val customFirstName: String = "",
        val customLastName: String = "",
        val targetSuccessCount: Int = 10,
        val threadsCount: Int = 2,
        val stepDelayMin: Int = 10,
        val stepDelayMax: Int = 30,
        val accountDelayMin: Int = 15,
        val accountDelayMax: Int = 45,
        val smsWaitSeconds: Int = 300,
        val skipAppCode: Boolean = false,
        val filterNumbers: Boolean = true,
        val replaceTokenOnFlood: Boolean = true,
        // --- Email settings (when Telegram requires setup email) ---
        val autoBuyEmail: Boolean = true,
        val emailService: String = "auto",
        val emailApiKey: String = "",
        val emailDomain: String = "any",
        // --- Features ported from Mac version ---
        val smsOperator: String = "",
        val maxPrice: String = "",
        val gender: Int = 0, // 0 = random, 1 = male, 2 = female
        val firstNameFile: String = "",
        val lastNameFile: String = "",
        val resetSessionsAfterReg: Boolean = false,
        val reset2faOnOccupied: Boolean = false,
        val tokenService: Int = 0, // 0 = None, 1 = TokenFast, 2 = AntiSafety
        val tokenApiKey: String = "",
        val antiSafetyAid: String = "308aba4e-5680-466b-81a5-477ac6befa95",
        val captchaService: Int = 0, // 0 = Auto, 1 = TokenFast, 2 = AntiSafety, 3 = NextCaptcha
        val captchaApiKey: String = "",
        val autoSetBio: Boolean = false,
        val customBio: String = "",
        val autoSetUsername: Boolean = false,
        val precheckProxy: Boolean = true
    )

    private const val PREFS_NAME = "ggtg_android_registration"

    fun loadConfig(): Config {
        val prefs = ApplicationLoader.applicationContext.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
        val singleName = prefs.getBoolean("singleNameForAll", !prefs.getBoolean("autoRandomNames", true))
        val sid = prefs.getString("serviceId", "hero-sms") ?: "hero-sms"
        return Config(
            serviceId = sid,
            apiKey = apiKeyFor(sid),
            customUrl = prefs.getString("customUrl", "") ?: "",
            countryId = prefs.getInt("countryId", 0),
            countryName = prefs.getString("countryName", "") ?: "",
            proxyListName = prefs.getString("proxyListName", "") ?: "",
            folderTag = prefs.getString("folderTag", "Регистрация") ?: "Регистрация",
            twoFactorPassword = prefs.getString("twoFactorPassword", "") ?: "",
            single2faForAll = prefs.getBoolean("single2faForAll", true),
            singleNameForAll = singleName,
            autoRandomNames = !singleName,
            customFirstName = prefs.getString("customFirstName", "") ?: "",
            customLastName = prefs.getString("customLastName", "") ?: "",
            targetSuccessCount = prefs.getInt("targetSuccessCount", 10).coerceAtLeast(1),
            threadsCount = prefs.getInt("threadsCount", 2).coerceIn(1, 10),
            stepDelayMin = prefs.getInt("stepDelayMin", 10).coerceAtLeast(0),
            stepDelayMax = prefs.getInt("stepDelayMax", 30).coerceAtLeast(10),
            accountDelayMin = prefs.getInt("accountDelayMin", 15).coerceAtLeast(0),
            accountDelayMax = prefs.getInt("accountDelayMax", 45).coerceAtLeast(15),
            smsWaitSeconds = prefs.getInt("smsWaitSeconds", 300).coerceIn(120, 1800),
            skipAppCode = prefs.getBoolean("skipAppCode", false),
            filterNumbers = prefs.getBoolean("filterNumbers", true),
            replaceTokenOnFlood = prefs.getBoolean("replaceTokenOnFlood", true),
            autoBuyEmail = prefs.getBoolean("autoBuyEmail", true),
            emailService = prefs.getString("emailService", "auto") ?: "auto",
            emailApiKey = prefs.getString("emailApiKey", "") ?: "",
            emailDomain = prefs.getString("emailDomain", "any") ?: "any",
            smsOperator = prefs.getString("smsOperator", "") ?: "",
            maxPrice = prefs.getString("maxPrice", "") ?: "",
            gender = prefs.getInt("gender", 0),
            firstNameFile = prefs.getString("firstNameFile", "") ?: "",
            lastNameFile = prefs.getString("lastNameFile", "") ?: "",
            resetSessionsAfterReg = prefs.getBoolean("resetSessionsAfterReg", false),
            reset2faOnOccupied = prefs.getBoolean("reset2faOnOccupied", false),
            tokenService = prefs.getInt("tokenService", 0),
            tokenApiKey = prefs.getString("tokenApiKey", "") ?: "",
            antiSafetyAid = prefs.getString("antiSafetyAid", "308aba4e-5680-466b-81a5-477ac6befa95") ?: "308aba4e-5680-466b-81a5-477ac6befa95",
            captchaService = prefs.getInt("captchaService", 0),
            captchaApiKey = prefs.getString("captchaApiKey", "") ?: "",
            autoSetBio = prefs.getBoolean("autoSetBio", false),
            customBio = prefs.getString("customBio", "") ?: "",
            autoSetUsername = prefs.getBoolean("autoSetUsername", false),
            precheckProxy = prefs.getBoolean("precheckProxy", true)
        )
    }

    fun saveConfig(cfg: Config) {
        ApplicationLoader.applicationContext.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE).edit()
            .putString("serviceId", cfg.serviceId)
            .putString("apiKey", cfg.apiKey)
            .putString("customUrl", cfg.customUrl)
            .putInt("countryId", cfg.countryId)
            .putString("countryName", cfg.countryName)
            .putString("apiKey_${cfg.serviceId}", cfg.apiKey)
            .putString("proxyListName", cfg.proxyListName)
            .putString("folderTag", cfg.folderTag)
            .putString("twoFactorPassword", cfg.twoFactorPassword)
            .putBoolean("single2faForAll", cfg.single2faForAll)
            .putBoolean("singleNameForAll", cfg.singleNameForAll)
            .putBoolean("autoRandomNames", !cfg.singleNameForAll)
            .putString("customFirstName", cfg.customFirstName)
            .putString("customLastName", cfg.customLastName)
            .putInt("targetSuccessCount", cfg.targetSuccessCount)
            .putInt("threadsCount", cfg.threadsCount)
            .putInt("stepDelayMin", cfg.stepDelayMin)
            .putInt("stepDelayMax", cfg.stepDelayMax)
            .putInt("accountDelayMin", cfg.accountDelayMin)
            .putInt("accountDelayMax", cfg.accountDelayMax)
            .putInt("smsWaitSeconds", cfg.smsWaitSeconds)
            .putBoolean("skipAppCode", cfg.skipAppCode)
            .putBoolean("filterNumbers", cfg.filterNumbers)
            .putBoolean("replaceTokenOnFlood", cfg.replaceTokenOnFlood)
            .putBoolean("autoBuyEmail", cfg.autoBuyEmail)
            .putString("emailService", cfg.emailService)
            .putString("emailApiKey", cfg.emailApiKey)
            .putString("emailDomain", cfg.emailDomain)
            .putString("smsOperator", cfg.smsOperator)
            .putString("maxPrice", cfg.maxPrice)
            .putInt("gender", cfg.gender)
            .putString("firstNameFile", cfg.firstNameFile)
            .putString("lastNameFile", cfg.lastNameFile)
            .putBoolean("resetSessionsAfterReg", cfg.resetSessionsAfterReg)
            .putBoolean("reset2faOnOccupied", cfg.reset2faOnOccupied)
            .putInt("tokenService", cfg.tokenService)
            .putString("tokenApiKey", cfg.tokenApiKey)
            .putString("antiSafetyAid", cfg.antiSafetyAid)
            .putInt("captchaService", cfg.captchaService)
            .putString("captchaApiKey", cfg.captchaApiKey)
            .putBoolean("autoSetBio", cfg.autoSetBio)
            .putString("customBio", cfg.customBio)
            .putBoolean("autoSetUsername", cfg.autoSetUsername)
            .putBoolean("precheckProxy", cfg.precheckProxy)
            .apply()
    }

    /** The API key kept for [serviceId] (each SMS service has its own). */
    fun apiKeyFor(serviceId: String): String {
        val prefs = ApplicationLoader.applicationContext.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
        return prefs.getString("apiKey_$serviceId", null)
            ?: (if (prefs.getString("serviceId", "hero-sms") == serviceId) prefs.getString("apiKey", "") else "")
            ?: ""
    }

    fun saveApiKeyFor(serviceId: String, key: String) {
        val prefs = ApplicationLoader.applicationContext.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
        prefs.edit().putString("apiKey_$serviceId", key).apply()
    }

    /** Numbers that already failed (registered, banned, invalid): kept between runs so they are never paid for twice. */
    fun isBadNumber(phone: String): Boolean =
        ApplicationLoader.applicationContext.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
            .getStringSet("badNumbers", emptySet())?.contains(phone) == true

    fun addBadNumber(phone: String) {
        val prefs = ApplicationLoader.applicationContext.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
        val set = HashSet(prefs.getStringSet("badNumbers", emptySet()) ?: emptySet())
        if (set.add(phone)) prefs.edit().putStringSet("badNumbers", set).apply()
    }

    fun resolveBaseUrl(config: Config): String {
        if (config.serviceId == "custom") return config.customUrl.trim()
        return SERVICES.firstOrNull { it.id == config.serviceId }?.baseUrl ?: SERVICES.first().baseUrl
    }

    // -----------------------------------------------------------------------------
    // HTTP Helpers
    // -----------------------------------------------------------------------------
    suspend fun fetchApiGet(urlString: String, headers: Map<String, String> = emptyMap()): Result<String> = withContext(Dispatchers.IO) {
        try {
            val url = URL(urlString)
            val conn = url.openConnection() as HttpURLConnection
            conn.connectTimeout = 15_000
            conn.readTimeout = 15_000
            conn.requestMethod = "GET"
            conn.setRequestProperty("User-Agent", "GGTG-Android/1.0")
            for ((k, v) in headers) conn.setRequestProperty(k, v)
            val code = conn.responseCode
            val stream = if (code in 200..299) conn.inputStream else conn.errorStream ?: conn.inputStream
            val response = BufferedReader(InputStreamReader(stream)).readText().trim()
            conn.disconnect()
            Result.success(response)
        } catch (t: Throwable) {
            Result.failure(t)
        }
    }

    suspend fun fetchApiPostJson(urlString: String, jsonBody: String, headers: Map<String, String> = emptyMap()): Result<String> = withContext(Dispatchers.IO) {
        try {
            val url = URL(urlString)
            val conn = url.openConnection() as HttpURLConnection
            conn.connectTimeout = 20_000
            conn.readTimeout = 20_000
            conn.requestMethod = "POST"
            conn.doOutput = true
            conn.setRequestProperty("User-Agent", "GGTG-Android/1.0")
            conn.setRequestProperty("Content-Type", "application/json")
            for ((k, v) in headers) conn.setRequestProperty(k, v)
            OutputStreamWriter(conn.outputStream).use { it.write(jsonBody) }
            val code = conn.responseCode
            val stream = if (code in 200..299) conn.inputStream else conn.errorStream ?: conn.inputStream
            val response = BufferedReader(InputStreamReader(stream)).readText().trim()
            conn.disconnect()
            Result.success(response)
        } catch (t: Throwable) {
            Result.failure(t)
        }
    }

    // -----------------------------------------------------------------------------
    // SMS Service HTTP API (Handler API standard)
    // -----------------------------------------------------------------------------
    suspend fun fetchBalance(config: Config): Result<String> {
        val base = resolveBaseUrl(config)
        val key = config.apiKey.trim()
        if (base.isEmpty() || key.isEmpty()) return Result.failure(Exception("Проверьте адрес сервиса и API-ключ"))
        val url = "$base?api_key=$key&action=getBalance"
        return fetchApiGet(url).map { resp ->
            when {
                resp.startsWith("ACCESS_BALANCE:") -> resp.removePrefix("ACCESS_BALANCE:").trim()
                resp == "BAD_KEY" -> throw Exception("Неверный API-ключ")
                resp == "ERROR_SQL" -> throw Exception("Ошибка базы на стороне SMS-сервиса")
                resp == "NO_KEY" -> throw Exception("Ключ не передан")
                else -> resp
            }
        }
    }

    // Countries the SMS service really has (action=getCountries), by service id. Only this current list is ever shown.
    private val liveCountries = java.util.concurrent.ConcurrentHashMap<String, List<CountryItem>>()

    fun countriesFor(serviceId: String): List<CountryItem> =
        liveCountries[serviceId]?.takeIf { it.isNotEmpty() } ?: COUNTRIES

    fun hasLiveCountries(serviceId: String) = liveCountries[serviceId]?.isNotEmpty() == true

    fun countryTitle(c: CountryItem): String =
        (if (c.flag.isEmpty()) c.name else "${c.flag}  ${c.name}") + (if (c.phoneCode.isEmpty()) "" else " (${c.phoneCode})")

    /** What the country row shows: the live list's name if loaded, else the name saved when it was picked. */
    fun currentCountryTitle(config: Config): String {
        if (config.countryId < 0) return "Не выбрана"
        return countriesFor(config.serviceId).firstOrNull { it.id == config.countryId }?.let { countryTitle(it) }
            ?: config.countryName.ifEmpty { "Не выбрана" }
    }

    private fun parseCountries(raw: String): List<CountryItem> {
        val result = ArrayList<CountryItem>()
        fun add(value: Any?, key: String) {
            var id = key.toIntOrNull() ?: return
            var name = ""
            if (value is String) {
                name = value
            } else if (value is org.json.JSONObject) {
                if (value.has("id")) id = value.optInt("id", id)
                if (value.has("visible") && value.optInt("visible", 1) == 0) return
                name = listOf("rus", "name", "title", "eng").map { value.optString(it, "").trim() }.firstOrNull { it.isNotEmpty() } ?: ""
            }
            if (name.isEmpty()) return
            val known = COUNTRIES.firstOrNull { it.id == id }
            result.add(CountryItem(id, known?.flag ?: "", name, known?.phoneCode ?: ""))
        }
        try {
            val text = raw.trim()
            if (text.startsWith("[")) {
                val arr = org.json.JSONArray(text)
                for (i in 0 until arr.length()) add(arr.get(i), i.toString())
            } else {
                val obj = org.json.JSONObject(text)
                for (k in obj.keys()) add(obj.get(k), k)
            }
        } catch (_: Throwable) {
        }
        return result.sortedBy { it.name }
    }

    suspend fun fetchCountries(config: Config): Result<Int> {
        val base = resolveBaseUrl(config)
        if (base.isEmpty()) return Result.failure(Exception("Не указан адрес SMS-сервиса"))
        return fetchApiGet("$base?api_key=${config.apiKey.trim()}&action=getCountries").map { resp ->
            val list = parseCountries(resp)
            if (list.isEmpty()) throw Exception("Сервис не вернул список стран")
            liveCountries[config.serviceId] = list
            list.size
        }
    }

    data class OperatorInfo(
        val name: String,
        val title: String,
        val count: Int = -1,
        val price: String = ""
    )

    fun operatorDisplayTitle(op: String, count: Int = -1): String {
        val trimmed = op.trim()
        if (trimmed.isEmpty() || trimmed == "any") {
            if (count >= 0) {
                return "Любой оператор (any) — доступно: $count шт."
            }
            return "Любой оператор (any)"
        }
        if (count >= 0) {
            return "$trimmed (доступно: $count шт.)"
        }
        return trimmed
    }

    suspend fun fetchOperators(config: Config, countryId: Int): Result<List<OperatorInfo>> = withContext(Dispatchers.IO) {
        val base = resolveBaseUrl(config)
        val key = config.apiKey.trim()
        if (base.isEmpty() || key.isEmpty() || countryId < 0) {
            return@withContext Result.failure(Exception("Не указан сервис, ключ или страна"))
        }
        val opUrl = "$base?api_key=$key&action=getOperators&country=$countryId"
        val opRes = fetchApiGet(opUrl)
        val opRaw = opRes.getOrNull() ?: ""

        val list = ArrayList<OperatorInfo>()
        val seen = HashSet<String>()
        runCatching {
            val obj = org.json.JSONObject(opRaw)
            val co = obj.optJSONObject("countryOperators")
            val arr = co?.optJSONArray(countryId.toString()) ?: obj.optJSONArray("operators")
            if (arr != null) {
                for (i in 0 until arr.length()) {
                    val name = arr.optString(i).trim()
                    if (name.isNotEmpty() && name != "any" && seen.add(name)) {
                        list.add(OperatorInfo(name, name))
                    }
                }
            }
        }

        val priceUrl = "$base?api_key=$key&action=getPrices&service=tg&country=$countryId"
        val priceRes = fetchApiGet(priceUrl)
        val priceRaw = priceRes.getOrNull() ?: ""
        var totalCount = -1
        var minCost = ""
        runCatching {
            val pObj = org.json.JSONObject(priceRaw)
            val cObj = pObj.optJSONObject(countryId.toString())
            val tgObj = cObj?.optJSONObject("tg")
            if (tgObj != null) {
                if (tgObj.has("count")) totalCount = tgObj.optInt("count", -1)
                if (tgObj.has("cost")) minCost = String.format(java.util.Locale.US, "%.2f", tgObj.optDouble("cost"))
                for (i in 0 until list.size) {
                    val op = list[i]
                    val sub = tgObj.optJSONObject(op.name)
                    var c = sub?.optInt("count", -1) ?: -1
                    var p = if (sub?.has("cost") == true) String.format(java.util.Locale.US, "%.2f", sub.optDouble("cost")) else ""
                    if (c < 0) c = totalCount
                    if (p.isEmpty()) p = minCost
                    list[i] = op.copy(count = c, price = p)
                }
            }
        }

        list.add(0, OperatorInfo("any", "Любой оператор (any)", totalCount, minCost))
        Result.success(list)
    }

    fun cleanPhoneForTelegram(phone: String): String {
        var p = phone.filter(Char::isDigit)
        if (p.startsWith("440") && p.length > 10) p = "44" + p.substring(3)
        else if (p.startsWith("610") && p.length > 10) p = "61" + p.substring(3)
        else if (p.startsWith("330") && p.length > 10) p = "33" + p.substring(3)
        else if (p.startsWith("490") && p.length > 10) p = "49" + p.substring(3)
        else if (p.startsWith("310") && p.length > 10) p = "31" + p.substring(3)
        else if (p.startsWith("270") && p.length > 10) p = "27" + p.substring(3)
        else if (p.startsWith("460") && p.length > 10) p = "46" + p.substring(3)
        else if (p.startsWith("470") && p.length > 10) p = "47" + p.substring(3)
        else if (p.startsWith("480") && p.length > 10) p = "48" + p.substring(3)
        else if (p.startsWith("620") && p.length > 10) p = "62" + p.substring(3)
        else if (p.startsWith("660") && p.length > 10) p = "66" + p.substring(3)
        else if (p.startsWith("840") && p.length > 10) p = "84" + p.substring(3)
        else if (p.startsWith("630") && p.length > 10) p = "63" + p.substring(3)
        else if (p.startsWith("2340") && p.length > 10) p = "234" + p.substring(4)
        else if (p.startsWith("2540") && p.length > 10) p = "254" + p.substring(4)
        else if (p.startsWith("200") && p.length > 10) p = "20" + p.substring(3)
        else if (p.startsWith("2120") && p.length > 10) p = "212" + p.substring(4)
        else if (p.startsWith("900") && p.length > 10) p = "90" + p.substring(3)
        else if (p.startsWith("550") && p.length > 10) p = "55" + p.substring(3)
        return p
    }

    suspend fun orderNumber(config: Config, countryId: Int): Result<Pair<String, String>> {
        val base = resolveBaseUrl(config)
        val key = config.apiKey.trim()
        if (base.isEmpty() || key.isEmpty()) return Result.failure(Exception("Не указан API-ключ SMS-сервиса"))
        var url = "$base?api_key=$key&action=getNumber&service=tg&country=$countryId"
        val op = config.smsOperator.trim()
        if (op.isNotEmpty() && op != "any") {
            url += "&operator=$op"
        }
        val mp = config.maxPrice.trim()
        if (mp.isNotEmpty()) {
            url += "&maxPrice=$mp"
        }
        return fetchApiGet(url).map { resp ->
            if (resp.startsWith("ACCESS_NUMBER:")) {
                val parts = resp.split(':')
                if (parts.size >= 3) {
                    return@map parts[1] to parts[2]
                }
            }
            throw when (resp) {
                "NO_NUMBERS" -> Exception("Нет свободных номеров для этой страны")
                "NO_BALANCE" -> Exception("Недостаточно средств на балансе SMS-сервиса")
                "BAD_KEY" -> Exception("Неверный API-ключ")
                "BAD_SERVICE" -> Exception("Сервис Telegram (tg) недоступен")
                "WRONG_MAX_PRICE" -> Exception("Цена номера выше установленного максимума (maxPrice)")
                else -> Exception("Ответ сервиса: $resp")
            }
        }
    }

    suspend fun pollSmsCode(config: Config, activationId: String): Triple<String, String?, String?> {
        val base = resolveBaseUrl(config)
        val key = config.apiKey.trim()
        val url = "$base?api_key=$key&action=getStatus&id=$activationId"
        val res = fetchApiGet(url).getOrNull() ?: return Triple("error", null, "Сетевой сбой при опросе SMS")
        return when {
            res == "STATUS_WAIT_CODE" -> Triple("wait", null, null)
            res == "STATUS_WAIT_RETRY" -> Triple("retry", null, null)
            res == "STATUS_CANCEL" -> Triple("cancel", null, null)
            res.startsWith("STATUS_OK:") -> {
                val raw = res.removePrefix("STATUS_OK:").trim()
                val lower = raw.lowercase()
                // ggtg: verify code is for Telegram and ignore other services on recycled numbers
                if (lower.contains("whatsapp") || lower.contains("viber") || lower.contains("google") || lower.contains("tiktok")) {
                    Triple("wait", null, null)
                } else {
                    val codeMatch = Regex("\\b(\\d{5,6})\\b").find(raw)
                    val code = codeMatch?.groupValues?.get(1) ?: raw
                    Triple("ok", code, null)
                }
            }
            else -> Triple("other", null, res)
        }
    }

    suspend fun setSmsStatus(config: Config, activationId: String, status: Int): Result<String> {
        val base = resolveBaseUrl(config)
        val key = config.apiKey.trim()
        val url = "$base?api_key=$key&action=setStatus&status=$status&id=$activationId"
        return fetchApiGet(url)
    }

    // -----------------------------------------------------------------------------
    // Email Activation API for Hero-SMS and SMSBower (when Telegram requires setup email)
    // -----------------------------------------------------------------------------
    suspend fun orderEmail(config: Config): Result<Pair<String, String>> = withContext(Dispatchers.IO) {
        val isAntiSafety = (config.emailService == "antisafety") || (config.emailService == "auto" && config.tokenService == 2)
        if (isAntiSafety) {
            val asfKey = config.emailApiKey.ifBlank { config.tokenApiKey.ifBlank { config.apiKey } }.trim()
            if (asfKey.isEmpty()) return@withContext Result.failure(Exception("Не указан API-ключ AntiSafety"))
            val aid = config.antiSafetyAid.ifBlank { "308aba4e-5680-466b-81a5-477ac6befa95" }.trim()
            val url = "https://antisafety.net/api/tasks/email/create?api_key=$asfKey&aid=$aid&type=premium"
            return@withContext fetchApiGet(url).mapCatching { resp ->
                val obj = JSONObject(resp)
                if (obj.optString("status") == "ok") {
                    val id = obj.optString("id", "")
                    val email = obj.optString("email", "")
                    if (id.isNotEmpty() && email.isNotEmpty()) {
                        return@mapCatching id to email
                    }
                }
                throw Exception(obj.optString("message", resp))
            }
        }

        val key = config.emailApiKey.ifBlank { config.apiKey }.trim()
        if (key.isEmpty()) return@withContext Result.failure(Exception("Не указан API-ключ для почты"))

        val useSmsBower = (config.emailService == "smsbower") || (config.emailService == "auto" && config.serviceId == "smsbower")

        if (useSmsBower) {
            val reqDomain = config.emailDomain.trim()
            val isAny = reqDomain.isBlank() || reqDomain.equals("any", ignoreCase = true) || reqDomain.equals("auto", ignoreCase = true)
            val domainsToTry = if (isAny) listOf("gmail.com", "icloud.com") else listOf(reqDomain)
            var lastErr = ""
            for (currentDomain in domainsToTry) {
                val url = "https://smsbower.online/api/mail/getActivation?api_key=$key&service=tg&domain=$currentDomain"
                val res = fetchApiGet(url)
                if (res.isFailure) {
                    lastErr = res.exceptionOrNull()?.message ?: "Network error"
                    continue
                }
                val resp = res.getOrNull() ?: ""
                val obj = try { JSONObject(resp) } catch (e: Throwable) {
                    lastErr = resp
                    continue
                }
                val st = obj.optInt("status", 0)
                if (st == 1) {
                    val mail = obj.optString("mail", "")
                    val mailId = if (obj.has("mailId")) obj.optString("mailId", "") else obj.optString("id", "")
                    if (mail.isNotEmpty() && mailId.isNotEmpty()) {
                        return@withContext Result.success(mailId to mail)
                    }
                }
                lastErr = obj.optString("error", obj.optString("msg", resp))
            }
            return@withContext Result.failure(Exception(if (lastErr.isNotBlank()) lastErr else "Нет доступных почт на SMS Bower"))
        } else {
            // Hero-SMS Email API
            val url = "https://hero-sms.com/api/v1/emails?api_key=$key"
            val body = "{\"service\":\"tg\"}"
            val headers = mapOf("Authorization" to "ApiKey $key", "X-Api-Key" to key)
            fetchApiPostJson(url, body, headers).map { resp ->
                val doc = try { JSONObject(resp) } catch (e: Throwable) { throw Exception(resp) }
                val obj = if (doc.has("data") && doc.get("data") is JSONObject) doc.getJSONObject("data") else doc
                val mailId = obj.optString("id", "").ifEmpty { obj.optString("mailId", "") }
                val mail = obj.optString("email", "").ifEmpty { obj.optString("mail", "") }
                if (mail.isNotEmpty() && mailId.isNotEmpty()) {
                    mailId to mail
                } else {
                    throw Exception(obj.optString("message", resp))
                }
            }
        }
    }

    suspend fun pollEmailCode(config: Config, mailId: String): Triple<String, String?, String?> = withContext(Dispatchers.IO) {
        val isAntiSafety = (config.emailService == "antisafety") || (config.emailService == "auto" && config.tokenService == 2)
        if (isAntiSafety) {
            val asfKey = config.emailApiKey.ifBlank { config.tokenApiKey.ifBlank { config.apiKey } }.trim()
            val aid = config.antiSafetyAid.ifBlank { "308aba4e-5680-466b-81a5-477ac6befa95" }.trim()
            val url = "https://antisafety.net/api/tasks/email/get?api_key=$asfKey&aid=$aid&id=$mailId"
            val res = fetchApiGet(url).getOrNull() ?: return@withContext Triple("wait", null, null)
            val obj = try { JSONObject(res) } catch (_: Throwable) { return@withContext Triple("wait", null, null) }
            val status = obj.optString("status", "")
            if (status == "ok") {
                val code = obj.optString("result", obj.optString("code", ""))
                if (code.isNotEmpty()) {
                    val match = Regex("\\b(\\d{5,6})\\b").find(code)
                    return@withContext Triple("ok", match?.groupValues?.get(1) ?: code, null)
                }
                return@withContext Triple("wait", null, null)
            }
            if (status == "error") {
                return@withContext Triple("error", null, obj.optString("message", res))
            }
            return@withContext Triple("wait", null, null)
        }

        val key = config.emailApiKey.ifBlank { config.apiKey }.trim()
        val useSmsBower = (config.emailService == "smsbower") || (config.emailService == "auto" && config.serviceId == "smsbower")

        if (useSmsBower) {
            val url = "https://smsbower.online/api/mail/getCode?api_key=$key&mailId=$mailId"
            val res = fetchApiGet(url).getOrNull() ?: return@withContext Triple("wait", null, null)
            val obj = try { JSONObject(res) } catch (_: Throwable) { return@withContext Triple("wait", null, null) }
            val st = obj.optInt("status", 0)
            val code = obj.optString("code", "")
            if (st == 1 && code.isNotEmpty()) {
                val match = Regex("\\b(\\d{5,6})\\b").find(code)
                Triple("ok", match?.groupValues?.get(1) ?: code, null)
            } else {
                Triple("wait", null, null)
            }
        } else {
            val url = "https://hero-sms.com/api/v1/emails/$mailId?api_key=$key"
            val headers = mapOf("Authorization" to "ApiKey $key", "X-Api-Key" to key)
            val res = fetchApiGet(url, headers).getOrNull() ?: return@withContext Triple("wait", null, null)
            val doc = try { JSONObject(res) } catch (_: Throwable) { return@withContext Triple("wait", null, null) }
            val obj = if (doc.has("data") && doc.get("data") is JSONObject) doc.getJSONObject("data") else doc
            val code = obj.optString("code", "")
            if (code.isNotEmpty()) {
                val match = Regex("\\b(\\d{5,6})\\b").find(code)
                Triple("ok", match?.groupValues?.get(1) ?: code, null)
            } else {
                Triple("wait", null, null)
            }
        }
    }

    suspend fun cancelEmail(config: Config, mailId: String): Result<String> = withContext(Dispatchers.IO) {
        val key = config.emailApiKey.ifBlank { config.apiKey }.trim()
        val useSmsBower = (config.emailService == "smsbower") || (config.emailService == "auto" && config.serviceId == "smsbower")
        if (useSmsBower) {
            fetchApiGet("https://smsbower.online/api/mail/setStatus?api_key=$key&id=$mailId&mailId=$mailId&status=2")
        } else {
            fetchApiGet("https://hero-sms.com/api/v1/emails/$mailId/cancel?api_key=$key", mapOf("Authorization" to "ApiKey $key"))
        }
    }

    // -----------------------------------------------------------------------------
    // TokenFast & AntiSafety Token Services
    // -----------------------------------------------------------------------------
    fun tokenServiceName(id: Int): String = when (id) {
        1 -> "TokenFast"
        2 -> "AntiSafety"
        else -> "Нет"
    }

    fun captchaServiceName(id: Int): String = when (id) {
        1 -> "TokenFast"
        2 -> "AntiSafety"
        3 -> "NextCaptcha"
        else -> "Авто (по сервису токенов)"
    }

    fun emailServiceName(id: String): String = when (id) {
        "hero-sms" -> "Hero-SMS"
        "smsbower" -> "SMSBower"
        "antisafety" -> "AntiSafety"
        else -> "Авто"
    }

    suspend fun fetchTokenBalance(tokenService: Int, tokenApiKey: String, aid: String = "308aba4e-5680-466b-81a5-477ac6befa95"): Result<String> {
        val key = tokenApiKey.trim()
        if (key.isEmpty()) return Result.failure(Exception("Введите API-ключ сервиса токенов"))
        return when (tokenService) {
            1 -> {
                val headers = mapOf("X-API-Key" to key)
                fetchApiGet("https://api.tokenfast.shop/v2/user/getBalance", headers).map { resp ->
                    try {
                        val obj = JSONObject(resp)
                        val bal = obj.optString("balance", "")
                        val cur = obj.optString("currency", "USD")
                        if (bal.isNotEmpty()) "$bal $cur" else resp
                    } catch (_: Throwable) {
                        resp
                    }
                }
            }
            2 -> {
                val apiKey = key.trim()
                val aidStr = aid.ifBlank { "308aba4e-5680-466b-81a5-477ac6befa95" }.trim()
                val url = "https://antisafety.net/api/tasks/balance?api_key=$apiKey&aid=$aidStr"
                fetchApiGet(url).map { resp ->
                    try {
                        val obj = JSONObject(resp)
                        if (obj.optString("status") == "ok" && obj.has("balance")) {
                            val bal = obj.optDouble("balance", 0.0) / 1000000.0
                            val cur = obj.optString("currency", "USD")
                            String.format(java.util.Locale.US, "%.2f %s", bal, cur)
                        } else {
                            val msg = obj.optString("message", resp)
                            throw Exception("AntiSafety: $msg")
                        }
                    } catch (e: Exception) {
                        throw e
                    } catch (_: Throwable) {
                        resp
                    }
                }
            }
            else -> Result.failure(Exception("Провайдер токенов не выбран"))
        }
    }

    suspend fun fetchMobileToken(tokenService: Int, tokenApiKey: String, phone: String, aid: String = "308aba4e-5680-466b-81a5-477ac6befa95"): Result<String> {
        val key = tokenApiKey.trim()
        if (tokenService == 0 || key.isEmpty()) return Result.success("")
        val formattedPhone = if (phone.startsWith("+")) phone else "+$phone"

        return when (tokenService) {
            1 -> fetchTokenFast(key, formattedPhone)
            2 -> fetchAntiSafety(key, aid.ifBlank { "308aba4e-5680-466b-81a5-477ac6befa95" })
            else -> Result.success("")
        }
    }

    private suspend fun fetchTokenFast(apiKey: String, phone: String): Result<String> {
        val uuid = "${Random.nextLong().toString(16)}-${Random.nextLong().toString(16)}"
        val headers = mapOf(
            "X-API-Key" to apiKey,
            "Idempotency-Key" to uuid
        )
        val body = JSONObject().apply {
            put("refId", "")
        }.toString()

        val orderResult = fetchApiPostJson("https://api.tokenfast.shop/v2/tokens/getToken", body, headers)
        if (orderResult.isFailure) return Result.failure(Exception("Ошибка TokenFast: ${orderResult.exceptionOrNull()?.message}"))

        val resp = orderResult.getOrThrow()
        val obj = try { JSONObject(resp) } catch (_: Throwable) {
            return Result.failure(Exception("Некорректный ответ TokenFast: $resp"))
        }
        val orderId = obj.optLong("orderId", 0)
        if (orderId == 0L) {
            val msg = obj.optString("message", resp)
            return Result.failure(Exception("TokenFast: $msg"))
        }

        return pollTokenFastStatus(apiKey, orderId.toString(), 20)
    }

    private suspend fun pollTokenFastStatus(apiKey: String, orderId: String, attemptsLeft: Int): Result<String> {
        if (attemptsLeft <= 0) return Result.failure(Exception("Тайм-аут ожидания токена TokenFast (orderId: $orderId)"))

        val headers = mapOf("X-API-Key" to apiKey)
        val res = fetchApiGet("https://api.tokenfast.shop/v2/tokens/getTokenStatus?orderId=$orderId", headers)
        if (res.isFailure) return Result.failure(Exception("Ошибка TokenFast poll: ${res.exceptionOrNull()?.message}"))

        val resp = res.getOrThrow()
        val obj = try { JSONObject(resp) } catch (_: Throwable) {
            return Result.failure(Exception("Некорректный ответ TokenFast: $resp"))
        }
        val data = obj.optJSONObject("data") ?: return Result.failure(Exception("Некорректный ответ TokenFast: $resp"))
        val msg = data.optString("message", "")

        return when (msg) {
            "SUCCESS" -> {
                val token = data.optString("token", "")
                if (token.isNotEmpty()) Result.success(token)
                else Result.failure(Exception("TokenFast вернул SUCCESS без токена"))
            }
            "CANCELED", "REFUND", "ORDER_NOT_FOUND" -> Result.failure(Exception("TokenFast: $msg"))
            else -> {
                delay(3000)
                pollTokenFastStatus(apiKey, orderId, attemptsLeft - 1)
            }
        }
    }

    private suspend fun fetchAntiSafety(apiKey: String, aid: String): Result<String> {
        val createUrl = "https://antisafety.net/api/tasks/android-push/create?api_key=${apiKey.trim()}&aid=${aid.trim()}"
        val res = fetchApiGet(createUrl)
        if (res.isFailure) return Result.failure(Exception("Ошибка AntiSafety create push: ${res.exceptionOrNull()?.message}"))
        val resp = res.getOrThrow()
        val obj = try { JSONObject(resp) } catch (_: Throwable) {
            return Result.failure(Exception("Некорректный ответ AntiSafety: $resp"))
        }
        if (obj.optString("status") == "ok") {
            val taskId = obj.optString("id", "")
            if (taskId.isNotEmpty()) {
                return pollAntiSafetyPushToken(apiKey, aid, taskId, 20)
            }
        }
        return Result.failure(Exception("AntiSafety: ${obj.optString("message", resp)}"))
    }

    private suspend fun pollAntiSafetyPushToken(apiKey: String, aid: String, taskId: String, attemptsLeft: Int): Result<String> {
        if (attemptsLeft <= 0) return Result.failure(Exception("Тайм-аут ожидания токена AntiSafety (taskId: $taskId)"))
        val pollUrl = "https://antisafety.net/api/tasks/android-push/get?api_key=${apiKey.trim()}&aid=${aid.trim()}&id=$taskId"
        val res = fetchApiGet(pollUrl)
        if (res.isFailure) return Result.failure(Exception("Ошибка AntiSafety poll: ${res.exceptionOrNull()?.message}"))
        val resp = res.getOrThrow()
        val obj = try { JSONObject(resp) } catch (_: Throwable) {
            return Result.failure(Exception("Некорректный ответ AntiSafety: $resp"))
        }
        val status = obj.optString("status", "")
        if (status == "ok") {
            val token = obj.optString("token", "")
            if (token.isNotEmpty()) return Result.success(token)
        }
        if (status == "error") {
            return Result.failure(Exception("AntiSafety: ${obj.optString("message", resp)}"))
        }
        delay(1500)
        return pollAntiSafetyPushToken(apiKey, aid, taskId, attemptsLeft - 1)
    }

    // -----------------------------------------------------------------------------
    // reCAPTCHA Enterprise Solvers (TokenFast, AntiSafety, NextCaptcha)
    // -----------------------------------------------------------------------------
    suspend fun solveTokenFastCaptcha(apiKey: String, action: String, keyId: String): Result<String> {
        val uuid = "${Random.nextLong().toString(16)}-${Random.nextLong().toString(16)}"
        val headers = mapOf(
            "X-API-Key" to apiKey.trim(),
            "Idempotency-Key" to uuid
        )
        val body = JSONObject().apply {
            put("action", action)
            put("key_id", keyId)
        }.toString()

        val orderResult = fetchApiPostJson("https://api.tokenfast.shop/v2/captcha/solveAppCaptcha", body, headers)
        if (orderResult.isFailure) return Result.failure(Exception("Ошибка TokenFast solveAppCaptcha: ${orderResult.exceptionOrNull()?.message}"))

        val resp = orderResult.getOrThrow()
        val obj = try { JSONObject(resp) } catch (_: Throwable) {
            return Result.failure(Exception("Некорректный ответ TokenFast: $resp"))
        }
        val orderId = obj.optLong("orderId", 0)
        if (orderId == 0L) {
            val msg = obj.optString("error", obj.optString("message", resp))
            return Result.failure(Exception("TokenFast: $msg"))
        }

        return pollTokenFastCaptcha(apiKey.trim(), orderId.toString(), 25)
    }

    private suspend fun pollTokenFastCaptcha(apiKey: String, orderId: String, attemptsLeft: Int): Result<String> {
        if (attemptsLeft <= 0) return Result.failure(Exception("Тайм-аут ожидания капчи TokenFast (orderId: $orderId)"))

        val headers = mapOf("X-API-Key" to apiKey)
        val res = fetchApiGet("https://api.tokenfast.shop/v2/captcha/getCaptchaStatus?orderId=$orderId", headers)
        if (res.isFailure) return Result.failure(Exception("Ошибка TokenFast getCaptchaStatus: ${res.exceptionOrNull()?.message}"))

        val resp = res.getOrThrow()
        val obj = try { JSONObject(resp) } catch (_: Throwable) {
            return Result.failure(Exception("Некорректный ответ TokenFast: $resp"))
        }
        val data = obj.optJSONObject("data") ?: return Result.failure(Exception("Некорректный ответ TokenFast: $resp"))
        val order = data.optJSONObject("order") ?: return Result.failure(Exception("Некорректный ответ TokenFast: $resp"))
        val status = order.optString("status", "")
        if (status == "success") {
            val token = order.optString("token", "")
            return if (token.isNotEmpty()) Result.success(token) else Result.failure(Exception("TokenFast вернул success без токена капчи"))
        }
        if (status == "failed" || status == "error" || status == "canceled") {
            return Result.failure(Exception("TokenFast капча отклонена: $status"))
        }
        delay(2500)
        return pollTokenFastCaptcha(apiKey, orderId, attemptsLeft - 1)
    }

    suspend fun solveNextCaptcha(apiKey: String, action: String, keyId: String): Result<String> {
        val taskObj = JSONObject().apply {
            put("type", "RecaptchaMobileTaskProxyless")
            put("appPackageName", "org.telegram.messenger")
            put("appKey", keyId)
            put("appAction", action)
            put("appDevice", "Android")
        }
        val body = JSONObject().apply {
            put("clientKey", apiKey.trim())
            put("task", taskObj)
        }.toString()

        val taskRes = fetchApiPostJson("https://api.nextcaptcha.com/createTask", body, emptyMap())
        if (taskRes.isFailure) return Result.failure(Exception("Ошибка NextCaptcha createTask: ${taskRes.exceptionOrNull()?.message}"))

        val resp = taskRes.getOrThrow()
        val obj = try { JSONObject(resp) } catch (_: Throwable) {
            return Result.failure(Exception("Некорректный ответ NextCaptcha createTask: $resp"))
        }
        if (obj.optInt("errorId", 0) != 0) {
            val desc = obj.optString("errorDescription", "")
            return Result.failure(Exception("NextCaptcha: $desc"))
        }
        val taskId = obj.optString("taskId", "")
        if (taskId.isEmpty()) return Result.failure(Exception("NextCaptcha не вернул taskId"))

        return pollNextCaptcha(apiKey.trim(), taskId, 25)
    }

    private suspend fun pollNextCaptcha(apiKey: String, taskId: String, attemptsLeft: Int): Result<String> {
        if (attemptsLeft <= 0) return Result.failure(Exception("Тайм-аут ожидания решения NextCaptcha (taskId: $taskId)"))

        val body = JSONObject().apply {
            put("clientKey", apiKey)
            put("taskId", taskId)
        }.toString()

        val res = fetchApiPostJson("https://api.nextcaptcha.com/getTaskResult", body, emptyMap())
        if (res.isFailure) return Result.failure(Exception("Ошибка NextCaptcha getTaskResult: ${res.exceptionOrNull()?.message}"))

        val resp = res.getOrThrow()
        val obj = try { JSONObject(resp) } catch (_: Throwable) {
            return Result.failure(Exception("Некорректный ответ NextCaptcha: $resp"))
        }
        if (obj.optInt("errorId", 0) != 0) {
            val desc = obj.optString("errorDescription", "")
            return Result.failure(Exception("NextCaptcha: $desc"))
        }
        val status = obj.optString("status", "")
        if (status == "ready") {
            val sol = obj.optJSONObject("solution")
            val token = sol?.optString("gRecaptchaResponse", "").orEmpty()
            return if (token.isNotEmpty()) Result.success(token) else Result.failure(Exception("NextCaptcha вернул ready без токена"))
        }
        if (status == "failed") {
            return Result.failure(Exception("NextCaptcha не смог решить капчу"))
        }
        delay(2500)
        return pollNextCaptcha(apiKey, taskId, attemptsLeft - 1)
    }

    suspend fun solveAntiSafetyCaptcha(apiKey: String, aid: String, action: String, keyId: String): Result<String> {
        val act = action.trim().ifEmpty { "login" }
        val aidStr = aid.ifBlank { "308aba4e-5680-466b-81a5-477ac6befa95" }.trim()
        val createUrl = "https://antisafety.net/api/tasks/recaptcha/create?api_key=${apiKey.trim()}&aid=$aidStr&action=$act&app_key=${keyId.trim()}&score=high"
        val res = fetchApiGet(createUrl)
        if (res.isFailure) return Result.failure(Exception("Ошибка AntiSafety recaptcha: ${res.exceptionOrNull()?.message}"))

        val resp = res.getOrThrow()
        val obj = try { JSONObject(resp) } catch (_: Throwable) {
            return Result.failure(Exception("Некорректный ответ AntiSafety recaptcha: $resp"))
        }
        if (obj.optString("status") == "ok") {
            val taskId = obj.optString("id", "")
            if (taskId.isNotEmpty()) {
                return pollAntiSafetyCaptcha(apiKey, aidStr, taskId, 25)
            }
        }
        return Result.failure(Exception("AntiSafety: ${obj.optString("message", resp)}"))
    }

    private suspend fun pollAntiSafetyCaptcha(apiKey: String, aid: String, taskId: String, attemptsLeft: Int): Result<String> {
        if (attemptsLeft <= 0) return Result.failure(Exception("Тайм-аут ожидания reCAPTCHA AntiSafety (taskId: $taskId)"))
        val pollUrl = "https://antisafety.net/api/tasks/recaptcha/get?api_key=${apiKey.trim()}&aid=${aid.trim()}&id=$taskId"
        val res = fetchApiGet(pollUrl)
        if (res.isFailure) return Result.failure(Exception("Ошибка AntiSafety recaptcha poll: ${res.exceptionOrNull()?.message}"))
        val resp = res.getOrThrow()
        val obj = try { JSONObject(resp) } catch (_: Throwable) {
            return Result.failure(Exception("Некорректный ответ AntiSafety recaptcha: $resp"))
        }
        val status = obj.optString("status", "")
        if (status == "ok") {
            val token = obj.optString("token", "")
            if (token.isNotEmpty()) return Result.success(token)
        }
        if (status == "error") {
            return Result.failure(Exception("AntiSafety recaptcha: ${obj.optString("message", resp)}"))
        }
        delay(2000)
        return pollAntiSafetyCaptcha(apiKey, aid, taskId, attemptsLeft - 1)
    }

    suspend fun solveAppCaptcha(config: Config, action: String, keyId: String): Result<String> {
        var service = config.captchaService
        var key = config.captchaApiKey.trim()
        if (service == 0) {
            if (config.tokenService == 1 && config.tokenApiKey.isNotBlank()) {
                service = 1
                key = config.tokenApiKey.trim()
            } else if (config.tokenService == 2 && config.tokenApiKey.isNotBlank()) {
                service = 2
                key = config.tokenApiKey.trim()
            } else if (config.captchaApiKey.isNotBlank()) {
                service = 3
                key = config.captchaApiKey.trim()
            } else if (config.tokenApiKey.isNotBlank()) {
                service = 1
                key = config.tokenApiKey.trim()
            }
        } else if (key.isEmpty()) {
            key = config.tokenApiKey.trim()
        }

        if (key.isEmpty()) {
            return Result.failure(Exception("Не задан API-ключ для решения капчи"))
        }

        return when (service) {
            3 -> solveNextCaptcha(key, action, keyId)
            2 -> solveAntiSafetyCaptcha(key, config.antiSafetyAid, action, keyId)
            else -> solveTokenFastCaptcha(key, action, keyId)
        }
    }

    @JvmStatic
    fun solveAppCaptchaAsync(config: Config, action: String, keyId: String, callback: (String?) -> Unit) {
        kotlinx.coroutines.CoroutineScope(kotlinx.coroutines.Dispatchers.IO).launch {
            val res = solveAppCaptcha(config, action, keyId)
            callback(res.getOrNull())
        }
    }

    // Play Integrity / SafetyNet token reserve for tokens obtained but not consumed by Telegram
    private val tokenReserve = java.util.Collections.synchronizedList(mutableListOf<String>())

    fun reserveToken(token: String) {
        if (token.isNotBlank()) tokenReserve.add(token)
    }

    fun popReservedToken(): String? {
        synchronized(tokenReserve) {
            return if (tokenReserve.isNotEmpty()) tokenReserve.removeAt(tokenReserve.size - 1) else null
        }
    }

    // -----------------------------------------------------------------------------
    // File-based name reading
    // -----------------------------------------------------------------------------
    fun readRandomLineFromFile(path: String): String? {
        if (path.isBlank()) return null
        return try {
            val file = File(path.trim())
            if (!file.exists() || !file.canRead()) return null
            val lines = file.readLines().map { it.trim() }.filter { it.isNotEmpty() }
            if (lines.isEmpty()) null else lines.random()
        } catch (_: Throwable) {
            null
        }
    }

    fun readFileLineCount(path: String): Int {
        if (path.isBlank()) return 0
        return try {
            val file = File(path.trim())
            if (!file.exists() || !file.canRead()) return 0
            file.readLines().count { it.trim().isNotEmpty() }
        } catch (_: Throwable) {
            0
        }
    }

    // -----------------------------------------------------------------------------
    // Generators (Bios, Names, Passwords, Realistic Delays)
    // -----------------------------------------------------------------------------
    private val SLAVIC_FIRST_M = listOf(
        "Александр", "Дмитрий", "Максим", "Сергей", "Андрей",
        "Алексей", "Артём", "Илья", "Кирилл", "Михаил",
        "Никита", "Матвей", "Роман", "Егор", "Арсений",
        "Иван", "Денис", "Евгений", "Даниил", "Тимофей"
    )
    private val SLAVIC_FIRST_F = listOf(
        "Анна", "Мария", "Елена", "Дарья", "Алина",
        "Полина", "Екатерина", "Виктория", "Анастасия", "Ксения",
        "София", "Вероника", "Юлия", "Валерия", "Алиса", "Ольга"
    )
    private val GENERAL_FIRST_M = listOf(
        "Alex", "James", "Michael", "David", "Daniel",
        "Robert", "John", "William", "Thomas", "Oliver",
        "Lucas", "Benjamin", "Henry", "Alexander", "Jack",
        "Noah", "Liam", "Mason"
    )
    private val GENERAL_FIRST_F = listOf(
        "Emma", "Olivia", "Sophia", "Isabella", "Mia",
        "Charlotte", "Amelia", "Harper", "Evelyn", "Emily",
        "Chloe", "Ava", "Ella", "Grace", "Lily"
    )

    private val SLAVIC_LAST_M = listOf(
        "Смирнов", "Иванов", "Кузнецов", "Попов", "Соколов",
        "Лебедев", "Козлов", "Новиков", "Морозов", "Петров",
        "Волков", "Соловьёв", "Васильев", "Зайцев", "Павлов",
        "Семенов", "Голубев", "Виноградов", "Богданов", "Воробьев"
    )
    private val SLAVIC_LAST_F = listOf(
        "Смирнова", "Иванова", "Кузнецова", "Попова", "Соколова",
        "Лебедева", "Козлова", "Новикова", "Морозова", "Петрова",
        "Волкова", "Соловьёва", "Васильева", "Зайцева", "Павлова"
    )
    private val GENERAL_LAST_NAMES = listOf(
        "Smith", "Johnson", "Williams", "Brown", "Jones",
        "Miller", "Davis", "Wilson", "Anderson", "Taylor",
        "Thomas", "Moore", "Jackson", "Martin", "Lee"
    )

    private val SLAVIC_BIOS = listOf(
        "Всегда на связи ☕",
        "Живу в моменте ✨",
        "Вопросы в ЛС",
        "Только по делу 📱",
        "Учусь, работаю, развиваюсь",
        "Пишите, отвечу как освобожусь",
        "В поисках интересного",
        "Здесь и сейчас 🌿",
        "Digital & Tech 🚀",
        "Всем добра и мира ✌️",
        "Музыка в наушниках, мысли в голове 🎧",
        "Work in progress...",
        "Coffee first ☕",
        "Счастье любит тишину",
        "Keep it simple 💫",
        "На связи по будням",
        "Life is good ☀️",
        "Просто человек"
    )

    private val GENERAL_BIOS = listOf(
        "Work in progress... 🚀",
        "Coffee & thoughts ☕",
        "Living the dream ✨",
        "Keep it simple",
        "Always learning 📚",
        "Available on Telegram",
        "DM for business 💼",
        "Design & Code",
        "Enjoying the journey",
        "Good vibes only ✌️",
        "Just another human",
        "Exploring the world 🌍",
        "Life is better with music 🎧",
        "Stay curious 🔍",
        "Here and now 🌿"
    )

    private fun isCisCountry(countryId: Int) =
        countryId == 0 || countryId == 2 || countryId == 11 || countryId == 35 || countryId == 40 ||
        countryId == 51 || countryId == 1 || countryId == 143 || countryId == 128 || countryId == 148 || countryId == 17

    fun generateRandomFirstName(countryId: Int, gender: Int = 0): String {
        val useMale = gender == 1 || (gender == 0 && Random.nextBoolean())
        val pool = if (isCisCountry(countryId)) {
            if (useMale) SLAVIC_FIRST_M else SLAVIC_FIRST_F
        } else {
            if (useMale) GENERAL_FIRST_M else GENERAL_FIRST_F
        }
        return pool.random()
    }

    fun generateRandomLastName(countryId: Int, gender: Int = 0): String {
        if (isCisCountry(countryId)) {
            val useMale = gender == 1 || (gender == 0 && Random.nextBoolean())
            return if (useMale) SLAVIC_LAST_M.random() else SLAVIC_LAST_F.random()
        }
        return GENERAL_LAST_NAMES.random()
    }

    fun generateRealisticBio(countryId: Int): String {
        val pool = if (isCisCountry(countryId)) SLAVIC_BIOS else GENERAL_BIOS
        return pool.random()
    }

    fun generateStrongPassword(length: Int = 18): String {
        val upper = "ABCDEFGHJKLMNPQRSTUVWXYZ"
        val lower = "abcdefghjkmnpqrstuvwxyz"
        val digits = "23456789"
        val special = "!@#$%^&*()_+="
        val all = upper + lower + digits + special
        val sb = StringBuilder(length)
        sb.append(upper.random())
        sb.append(lower.random())
        sb.append(digits.random())
        sb.append(special.random())
        for (i in 4 until length) {
            sb.append(all.random())
        }
        val chars = sb.toString().toCharArray()
        for (i in chars.indices.reversed()) {
            val j = Random.nextInt(i + 1)
            val tmp = chars[i]
            chars[i] = chars[j]
            chars[j] = tmp
        }
        return String(chars)
    }

    // -----------------------------------------------------------------------------
    // MTProto sendSuspend Helper
    // -----------------------------------------------------------------------------
    suspend fun ConnectionsManager.sendSuspend(
        request: TLObject,
        flags: Int = 0,
        dcId: Int = ConnectionsManager.DEFAULT_DATACENTER_ID,
        label: String = "",
        timeoutMs: Long = 30_000
    ): Pair<TLObject?, TLRPC.TL_error?>? = withTimeoutOrNull(timeoutMs) {
        suspendCancellableCoroutine { cont ->
            val reqId = sendRequest(request, { resp, err ->
                if (cont.isActive) cont.resume(resp to err)
            }, null, null, flags, dcId, ConnectionsManager.ConnectionTypeGeneric, true)
            cont.invokeOnCancellation {
                runCatching { cancelRequest(reqId, true) }
            }
        }
    }

    // -----------------------------------------------------------------------------
    // Registration Engine with Multi-threading & Queue Loop
    // -----------------------------------------------------------------------------
    object Engine {
        val isRunning = AtomicBoolean(false)
        val cancelled = AtomicBoolean(false)
        val isPaused = AtomicBoolean(false)
        val waitingForInternet = AtomicBoolean(false)
        val successCount = AtomicInteger(0)
        val failedCount = AtomicInteger(0)
        val activeThreads = AtomicInteger(0)

        @Volatile var processHandle: GgtgProcess.Handle? = null

        private val engineScope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
        private val logHistory = ArrayList<String>()

        private val reservedSlots = ConcurrentHashMap.newKeySet<Int>()
        private val inUseProxies = ConcurrentHashMap.newKeySet<String>()
        private val proxyCursor = AtomicInteger(0)

        var onLog: ((String) -> Unit)? = null
        var onStatus: ((msg: String, isError: Boolean, isSuccess: Boolean) -> Unit)? = null
        var onProgress: ((success: Int, target: Int, active: Int, failed: Int) -> Unit)? = null
        var onFinished: ((success: Int, failed: Int) -> Unit)? = null
        var onPauseChanged: ((Boolean) -> Unit)? = null

        val pendingActivations = ConcurrentHashMap<String, String>() // activationId -> phone
        val orderTimestamps = ConcurrentHashMap<String, Long>() // activationId -> orderTimeMs

        data class RefundTask(
            val activationId: String,
            val phone: String,
            val orderedAt: Long,
            val config: Config,
            var attempts: Int = 0
        )
        val refundQueue = ConcurrentHashMap<String, RefundTask>()
        private var refundJob: kotlinx.coroutines.Job? = null

        fun pause() {
            if (!isRunning.get() || isPaused.get()) return
            isPaused.set(true)
            log("Авторегистрация поставлена на паузу.")
            val target = activeConfig?.targetSuccessCount ?: 0
            status("На паузе. Успешно: ${successCount.get()} из $target")
            onPauseChanged?.invoke(true)
        }

        fun resume() {
            if (!isRunning.get() || !isPaused.get()) return
            isPaused.set(false)
            log("Авторегистрация возобновлена.")
            val target = activeConfig?.targetSuccessCount ?: 0
            status("Регистрация возобновлена: цель $target акк.")
            onPauseChanged?.invoke(false)
        }

        fun togglePause() {
            if (isPaused.get()) resume() else pause()
        }

        suspend fun awaitNetwork(): Boolean {
            if (ApplicationLoader.isNetworkOnline()) {
                if (waitingForInternet.getAndSet(false)) {
                    log("Интернет-соединение восстановлено! Продолжение авторегистрации...")
                    status("Интернет восстановлен. Продолжение регистрации...")
                }
                return true
            }
            if (!waitingForInternet.getAndSet(true)) {
                log("Нет интернет-соединения. Ожидание восстановления сети...")
                status("Нет интернет-соединения. Ожидание сети...")
            }
            while (!ApplicationLoader.isNetworkOnline()) {
                if (cancelled.get() || !isRunning.get()) {
                    waitingForInternet.set(false)
                    return false
                }
                delay(2000L)
            }
            delay(2000L)
            waitingForInternet.set(false)
            log("Интернет-соединение восстановлено! Продолжение авторегистрации...")
            status("Интернет восстановлен. Продолжение регистрации...")
            return true
        }

        suspend fun waitIfPausedOrNoNetwork(): Boolean {
            while (isRunning.get() && !cancelled.get()) {
                if (!awaitNetwork()) return false
                if (!isPaused.get()) break
                delay(500L)
            }
            return isRunning.get() && !cancelled.get()
        }

        fun startRefundSupervisor() {
            if (refundJob?.isActive == true) return
            refundJob = CoroutineScope(Dispatchers.IO + SupervisorJob()).launch {
                while (refundQueue.isNotEmpty()) {
                    delay(8000L)
                    val now = System.currentTimeMillis()
                    for ((id, task) in refundQueue.entries.toList()) {
                        val elapsed = (now - task.orderedAt) / 1000
                        if (elapsed >= 122) { // Minimum 2 minutes required by SMS services for refund
                            task.attempts++
                            val res = setSmsStatus(task.config, task.activationId, 8)
                            val resp = res.getOrNull() ?: ""
                            if (resp == "ACCESS_CANCEL" || resp == "ACCESS_CANCEL_ALREADY") {
                                log("[Автовозврат] Средства успешно возвращены сервисом: номер +${task.phone} (ID: ${task.activationId})!")
                                refundQueue.remove(id)
                                pendingActivations.remove(id)
                                orderTimestamps.remove(id)
                            } else if (resp == "STATUS_CANCEL") {
                                log("[Автовозврат] Заказ +${task.phone} (ID: ${task.activationId}) отменён сервисом, средства возвращены.")
                                refundQueue.remove(id)
                                pendingActivations.remove(id)
                                orderTimestamps.remove(id)
                            } else if (task.attempts > 12) {
                                log("[Автовозврат] Превышено число попыток возврата для ID: ${task.activationId} ($resp)")
                                refundQueue.remove(id)
                                pendingActivations.remove(id)
                                orderTimestamps.remove(id)
                            } else {
                                log("[Автовозврат] Сервис ответил: $resp. Повтор через 15 сек (попытка ${task.attempts})...")
                            }
                        }
                    }
                }
            }
        }

        fun enqueueRefund(config: Config, activationId: String, phone: String, orderedAt: Long = System.currentTimeMillis()) {
            if (activationId.isBlank()) return
            if (refundQueue.containsKey(activationId)) return
            log("Возврат средств: отправлен запрос на отмену номера +$phone (ID: $activationId)...")
            engineScope.launch {
                val res = setSmsStatus(config, activationId, 8)
                val resp = res.getOrNull() ?: ""
                if (resp == "ACCESS_CANCEL" || resp == "ACCESS_CANCEL_ALREADY") {
                    log("Возврат средств получен сразу: номер +$phone (ID: $activationId) отменён, средства возвращены!")
                    pendingActivations.remove(activationId)
                    refundQueue.remove(activationId)
                    orderTimestamps.remove(activationId)
                    return@launch
                }
                val task = RefundTask(activationId, phone, orderedAt, config)
                refundQueue[activationId] = task
                val elapsedSec = (System.currentTimeMillis() - orderedAt) / 1000
                val waitSec = maxOf(5L, 122L - elapsedSec)
                log("Сервис требует ожидания перед возвратом (осталось ${waitSec}с, ответ: $resp). Номер +$phone (ID: $activationId) поставлен в автоочередь возврата.")
                startRefundSupervisor()
            }
        }

        suspend fun stepDelay(threadId: Int, stepDesc: String, config: Config) {
            if (cancelled.get()) return
            val minSec = config.stepDelayMin.coerceAtLeast(0)
            val maxSec = config.stepDelayMax.coerceAtLeast(minSec)
            if (maxSec <= 0) return
            val delaySec = if (minSec == maxSec) minSec.toLong() else Random.nextLong(minSec.toLong(), maxSec.toLong() + 1L)
            log("[Поток $threadId] $stepDesc — пауза ${delaySec}с...")
            var elapsed = 0L
            val totalMs = delaySec * 1000L
            while (elapsed < totalMs && !cancelled.get()) {
                if (!waitIfPausedOrNoNetwork()) return
                val step = minOf(250L, totalMs - elapsed)
                delay(step)
                elapsed += step
            }
        }

        suspend fun accountDelay(threadId: Int, config: Config) {
            if (cancelled.get()) return
            val minSec = config.accountDelayMin.coerceAtLeast(0)
            val maxSec = config.accountDelayMax.coerceAtLeast(minSec)
            if (maxSec <= 0) return
            val delaySec = if (minSec == maxSec) minSec.toLong() else Random.nextLong(minSec.toLong(), maxSec.toLong() + 1L)
            log("[Поток $threadId] Пауза между аккаунтами — ${delaySec}с...")
            var elapsed = 0L
            val totalMs = delaySec * 1000L
            while (elapsed < totalMs && !cancelled.get()) {
                if (!waitIfPausedOrNoNetwork()) return
                val step = minOf(250L, totalMs - elapsed)
                delay(step)
                elapsed += step
            }
        }

        suspend fun refundPendingActivations(config: Config): Int {
            var count = 0
            val ids = pendingActivations.keys.toList()
            for (id in ids) {
                val num = pendingActivations[id] ?: ""
                val ordered = orderTimestamps[id] ?: (System.currentTimeMillis() - 125_000L)
                enqueueRefund(config, id, num, ordered)
                count++
            }
            return count
        }

        fun log(msg: String) {
            val time = SimpleDateFormat("HH:mm:ss", Locale.getDefault()).format(Date())
            val text = "[$time] $msg"
            FileLog.d("GgtgRegistration: $text")
            processHandle?.log(msg)
            synchronized(logHistory) {
                if (logHistory.size >= 250) logHistory.removeAt(0)
                logHistory.add(text)
            }
            onLog?.invoke(text)
        }

        fun getLogHistory(): List<String> = synchronized(logHistory) { ArrayList(logHistory) }

        fun status(msg: String, isError: Boolean = false, isSuccess: Boolean = false) {
            onStatus?.invoke(msg, isError, isSuccess)
        }

        private var activeConfig: Config? = null

        fun updateProgress(target: Int) {
            processHandle?.progress(successCount.get(), target)
            onProgress?.invoke(successCount.get(), target, activeThreads.get(), failedCount.get())
        }

        fun start(config: Config, customScope: CoroutineScope? = null) {
            if (isRunning.getAndSet(true)) return
            activeConfig = config
            cancelled.set(false)
            isPaused.set(false)
            waitingForInternet.set(false)
            successCount.set(0)
            failedCount.set(0)
            activeThreads.set(0)
            reservedSlots.clear()
            inUseProxies.clear()
            pendingActivations.clear()
            proxyCursor.set(0)

            saveConfig(config)

            processHandle = GgtgProcess.start(
                "Авторегистрация (${config.targetSuccessCount} акк.)",
                GgtgProcess.Kind.Registration
            )
            processHandle?.progress(0, config.targetSuccessCount)

            log("========================================")
            log("Запуск авторегистрации: цель ${config.targetSuccessCount} аккаунтов, потоков: ${config.threadsCount}")
            log("Пауза между шагами: ${config.stepDelayMin}–${config.stepDelayMax}с | Пауза между аккаунтами: ${config.accountDelayMin}–${config.accountDelayMax}с")
            if (config.tokenService != 0) log("Сервис токенов: ${tokenServiceName(config.tokenService)}")
            if (config.smsOperator.isNotBlank()) log("Оператор SMS: ${config.smsOperator}")
            if (config.maxPrice.isNotBlank()) log("Макс. цена номера: ${config.maxPrice}")
            log("========================================")

            updateProgress(config.targetSuccessCount)
            status("Регистрация запущена: цель ${config.targetSuccessCount} акк., потоков: ${config.threadsCount}")

            val runner = customScope ?: engineScope
            val threads = config.threadsCount.coerceIn(1, 10)
            for (threadIndex in 1..threads) {
                runner.launch(Dispatchers.IO) {
                    workerLoop(threadIndex, config)
                }
            }
        }

        fun stop(userRequested: Boolean = true) {
            if (!isRunning.getAndSet(false)) return
            cancelled.set(true)
            isPaused.set(false)
            waitingForInternet.set(false)
            onPauseChanged?.invoke(false)
            if (userRequested) {
                log("--- Регистрация остановлена пользователем ---")
                status("Остановлено. Успешно: ${successCount.get()}, Ошибок: ${failedCount.get()}")
                processHandle?.fail("Остановлено пользователем")
            } else {
                processHandle?.fail("Остановлено")
            }
            processHandle = null
            activeConfig?.let { cfg ->
                engineScope.launch {
                    val count = refundPendingActivations(cfg)
                    if (count > 0) {
                        log("Отправлен запрос на отмену и возврат средств для $count номеров.")
                    }
                }
            }
            onFinished?.invoke(successCount.get(), failedCount.get())
        }

        private suspend fun workerLoop(threadId: Int, config: Config) {
            activeThreads.incrementAndGet()
            updateProgress(config.targetSuccessCount)

            try {
                while (!cancelled.get() && successCount.get() < config.targetSuccessCount) {
                    if (!waitIfPausedOrNoNetwork()) break
                    val target = config.targetSuccessCount
                    if (successCount.get() >= target) break

                    val success = runSingleRegistration(threadId, config)
                    if (success) {
                        val current = successCount.incrementAndGet()
                        log("[Поток $threadId] Успешно завершён! Прогресс: $current из $target аккаунтов")
                    } else {
                        val fails = failedCount.incrementAndGet()
                        log("[Поток $threadId] Попытка не удалась. Всего ошибок: $fails. Запуск следующего номера...")
                    }
                    updateProgress(target)

                    if (!cancelled.get() && successCount.get() < target) {
                        accountDelay(threadId, config)
                    }
                }
            } finally {
                val remaining = activeThreads.decrementAndGet()
                updateProgress(config.targetSuccessCount)
                if (remaining == 0 && isRunning.getAndSet(false)) {
                    val s = successCount.get()
                    val f = failedCount.get()
                    val summary = "Регистрация завершена! Успешно: $s из ${config.targetSuccessCount}. Ошибок: $f."
                    log("========================================")
                    log(summary)
                    log("========================================")
                    status(summary, isSuccess = true)
                    processHandle?.succeed(summary)
                    processHandle = null
                    onFinished?.invoke(s, f)
                }
            }
        }

        private suspend fun runSingleRegistration(threadId: Int, config: Config): Boolean {
            if (cancelled.get()) return false

            // 1. Claim a free account slot
            val slot = (0 until UserConfig.MAX_ACCOUNT_COUNT).firstOrNull { s ->
                !UserConfig.getInstance(s).isClientActivated && reservedSlots.add(s)
            }
            if (slot == null) {
                log("[Поток $threadId] Ошибка: нет свободных слотов для аккаунтов в приложении")
                return false
            }

            var proxyEntry: GgtgProxyPoolStore.Entry? = null
            var activationId: String? = null
            var phone: String? = null
            var mobileToken = ""
            var tokenSpent = false

            try {
                // 2. Assign proxy from pool if configured
                if (config.proxyListName.isNotEmpty()) {
                    val entries = GgtgProxyPoolStore.getListEntries(config.proxyListName)
                    if (entries.isNotEmpty()) {
                        val total = entries.size
                        synchronized(inUseProxies) {
                            for (i in 0 until total) {
                                val idx = Math.floorMod(proxyCursor.get() + i, total)
                                val candidate = entries[idx]
                                val key = "${candidate.host}:${candidate.port}"
                                if (inUseProxies.add(key)) {
                                    proxyCursor.set(Math.floorMod(idx + 1, total))
                                    proxyEntry = candidate
                                    break
                                }
                            }
                            if (proxyEntry == null) {
                                val idx = Math.floorMod(proxyCursor.getAndIncrement(), total)
                                proxyEntry = entries[idx]
                            }
                        }
                    }
                    if (proxyEntry != null) {
                        log("[Поток $threadId] Прокси: ${proxyEntry.host}:${proxyEntry.port}")
                        if (config.precheckProxy) {
                            val reachable = withContext(Dispatchers.IO) {
                                try {
                                    val sock = java.net.Socket()
                                    sock.connect(java.net.InetSocketAddress(proxyEntry.host, proxyEntry.port), 6000)
                                    sock.close()
                                    true
                                } catch (_: Throwable) {
                                    false
                                }
                            }
                            if (!reachable) {
                                log("[Поток $threadId] Прокси ${proxyEntry.host}:${proxyEntry.port} не отвечает! Пропуск во избежание списания средств за номер.")
                                return false
                            }
                        }
                        ConnectionsManager.setProxySettingsForAccount(
                            slot, true, proxyEntry.host, proxyEntry.port,
                            proxyEntry.username, proxyEntry.password, ""
                        )
                        UserConfig.getInstance(slot).apply {
                            ncgProxyEnabled = true
                            ncgProxyAddress = proxyEntry.host
                            ncgProxyPort = proxyEntry.port
                            ncgProxyUsername = proxyEntry.username
                            ncgProxyPassword = proxyEntry.password
                            saveConfig(false)
                        }
                    }
                }

                // 3. Android device profile with country-matched timezone and language
                val countryItem = COUNTRIES.firstOrNull { it.id == config.countryId }
                val initialGeo = GgtgGeo.matchForCountry(countryItem?.isoCode ?: "US")
                val deviceProfile = GgtgAndroidProfiles.random()
                GgtgDevice.save(
                    slot, deviceProfile.deviceModel, deviceProfile.systemVersion,
                    initialGeo.systemLang, initialGeo.cloudLang, initialGeo.tzOffset,
                    deviceProfile.appVersion, "android", 6, "eb06d4abfb49dc3eeb1aeb98ae0f581e"
                )
                log("[Поток $threadId] Устройство: ${deviceProfile.deviceModel} | Гео: ${initialGeo.country} (UTC${GgtgDevice.formatOffset(initialGeo.tzOffset)})")

                // Step delay before ordering number
                stepDelay(threadId, "Пауза перед заказом номера в SMS-сервисе", config)

                // 4. Order number from SMS service
                log("[Поток $threadId] Запрос номера у SMS-сервиса...")
                val orderRes = orderNumber(config, config.countryId)
                if (orderRes.isFailure) {
                    val err = orderRes.exceptionOrNull()?.message ?: "Не удалось заказать номер"
                    log("[Поток $threadId] Ошибка заказа номера: $err")
                    return false
                }
                val (id, num) = orderRes.getOrThrow()
                activationId = id
                val rawDigits = num.filter(Char::isDigit)
                val digits = cleanPhoneForTelegram(rawDigits)
                phone = digits
                val orderedAt = System.currentTimeMillis()
                pendingActivations[activationId] = phone
                orderTimestamps[activationId] = orderedAt
                if (digits != rawDigits) {
                    log("[Поток $threadId] Получен номер: +$rawDigits (скорректирован для Telegram: +$digits, ID: $activationId)")
                } else {
                    log("[Поток $threadId] Получен номер: +$phone (ID: $activationId)")
                }
                val phoneGeo = GgtgGeo.matchForPhone(phone)
                GgtgDevice.save(
                    slot, deviceProfile.deviceModel, deviceProfile.systemVersion,
                    phoneGeo.systemLang, phoneGeo.cloudLang, phoneGeo.tzOffset,
                    deviceProfile.appVersion, "android", 6, "eb06d4abfb49dc3eeb1aeb98ae0f581e"
                )
                log("[Поток $threadId] Согласовано гео по номеру: ${phoneGeo.country} → язык: ${phoneGeo.systemLang}, часовой пояс: UTC${GgtgDevice.formatOffset(phoneGeo.tzOffset)}")

                if (config.filterNumbers && isBadNumber(digits)) {
                    log("[Поток $threadId] Номер +$phone уже не подошёл раньше (занят, заблокирован или неверный). Отмена, средства будут возвращены.")
                    enqueueRefund(config, activationId, phone, orderedAt)
                    return false
                }

                // 4.5 Fetch Play Integrity / SafetyNet token (if configured)
                mobileToken = ""
                tokenSpent = false
                if (config.tokenService != 0 && config.tokenApiKey.isNotBlank()) {
                    val reserved = popReservedToken()
                    if (!reserved.isNullOrBlank()) {
                        mobileToken = reserved
                        log("[Поток $threadId] Взят токен Play Integrity из резерва (в резерве: ${tokenReserve.size})")
                    } else {
                        log("[Поток $threadId] Запрос токена в ${tokenServiceName(config.tokenService)}...")
                        val tokenRes = fetchMobileToken(config.tokenService, config.tokenApiKey, digits, config.antiSafetyAid)
                        if (tokenRes.isFailure) {
                            log("[Поток $threadId] Предупреждение сервиса токенов: ${tokenRes.exceptionOrNull()?.message}")
                        } else {
                            mobileToken = tokenRes.getOrDefault("")
                            if (mobileToken.isNotEmpty()) {
                                log("[Поток $threadId] Токен Play Integrity получен (длина: ${mobileToken.length})")
                            }
                        }
                    }
                }

                // Step delay before requesting code from Telegram
                stepDelay(threadId, "Пауза перед отправкой запроса кода в Telegram", config)

                // 5. Send code via MTProto (with flood token replacement support)
                val cm = ConnectionsManager.getInstance(slot)
                val flags = ConnectionsManager.RequestFlagWithoutLogin or ConnectionsManager.RequestFlagFailOnServerErrors or
                    ConnectionsManager.RequestFlagTryDifferentDc or ConnectionsManager.RequestFlagEnableUnauthorized

                var floodRetries = 0
                var captchaRetries = 0
                var sent: TLObject? = null
                var sendErr: TLRPC.TL_error? = null

                while (floodRetries <= 2 && captchaRetries <= 2 && !cancelled.get()) {
                    log("[Поток $threadId] Отправка запроса кода в Telegram (MTPauth_SendCode)...")
                    val res = cm.sendSuspend(TLRPC.TL_auth_sendCode().apply {
                        phone_number = digits
                        api_id = BuildVars.APP_ID
                        api_hash = BuildVars.APP_HASH
                        settings = TLRPC.TL_codeSettings().apply {
                            allow_firebase = true
                            if (mobileToken.isNotEmpty()) {
                                token = mobileToken
                                this.flags = this.flags or (1 shl 8)
                            }
                        }
                    }, flags, ConnectionsManager.DEFAULT_DATACENTER_ID, "sendCode", 25_000)

                    if (res == null) {
                        log("[Поток $threadId] ⚠️ Telegram не ответил на запрос кода за 25 сек (таймаут сети/прокси). Отменяю номер и возвращаю средства...")
                        enqueueRefund(config, activationId, phone, orderedAt)
                        return false
                    }

                    sent = res.first
                    sendErr = res.second

                    if (sendErr != null && sendErr.text?.startsWith("PHONE_NUMBER_FLOOD") == true
                        && config.replaceTokenOnFlood && config.tokenService != 0 && config.tokenApiKey.isNotBlank() && floodRetries < 2) {
                        floodRetries++
                        log("[Поток $threadId] Флуд на токене: заменяю токен новым (попытка $floodRetries из 2)...")
                        delay(2000)
                        val tokenRes = fetchMobileToken(config.tokenService, config.tokenApiKey, digits, config.antiSafetyAid)
                        mobileToken = tokenRes.getOrDefault("")
                        continue
                    }

                    // Check for Telegram reCAPTCHA challenge
                    val errText = sendErr?.text.orEmpty()
                    val captchaRegex = Regex("RECAPTCHA_CHECK_([a-zA-Z0-9]+)__([a-zA-Z0-9_\\-]+)")
                    val match = captchaRegex.find(errText)
                    if (match != null && captchaRetries < 2) {
                        captchaRetries++
                        val action = match.groupValues[1]
                        val keyId = match.groupValues[2]
                        log("[Поток $threadId] Telegram запросил reCAPTCHA Enterprise ($action). Решение через сервис...")
                        val capRes = solveAppCaptcha(config, action, keyId)
                        val capToken = capRes.getOrNull()
                        if (capToken.isNullOrEmpty()) {
                            log("[Поток $threadId] Не удалось решить капчу Telegram: ${capRes.exceptionOrNull()?.message}")
                            enqueueRefund(config, activationId, phone, orderedAt)
                            return false
                        }
                        log("[Поток $threadId] ✓ Токен reCAPTCHA получен! Отправка решения...")
                        delay(1000)
                        continue
                    }
                    break
                }

                if (cancelled.get()) return false

                if (sendErr != null) {
                    val errText = sendErr.text ?: "Ошибка sendCode"
                    log("[Поток $threadId] Ошибка Telegram: $errText")
                    if (errText.startsWith("PHONE_NUMBER_BANNED") || errText.startsWith("PHONE_NUMBER_INVALID")) addBadNumber(digits)
                    enqueueRefund(config, activationId, phone, orderedAt)
                    return false
                }
                if (sent is TLRPC.TL_auth_sentCodePaymentRequired) {
                    addBadNumber(digits)
                    log("[Поток $threadId] Telegram требует платную отправку SMS / Premium для номера +$digits. Отмена, возврат средств.")
                    enqueueRefund(config, activationId, phone, orderedAt)
                    return false
                }
                val sentCode = sent as? TLRPC.TL_auth_sentCode ?: run {
                    log("[Поток $threadId] Неизвестный ответ Telegram на sendCode (${sent?.javaClass?.simpleName}). Отмена, возврат средств.")
                    enqueueRefund(config, activationId, phone, orderedAt)
                    return false
                }
                var phoneCodeHash = sentCode.phone_code_hash
                tokenSpent = true

                if (sentCode.type is TLRPC.TL_auth_sentCodeTypeSms) {
                    log("[Поток $threadId] ✓ Telegram подтвердил отправку SMS на номер +$digits! Ожидаем поступления SMS от сервиса...")
                } else if (sentCode.type is TLRPC.TL_auth_sentCodeTypeApp) {
                    log("[Поток $threadId] ⚠️ Telegram отправил код в активное приложение на этом номере, а не в SMS: номер уже кем-то используется.")
                } else if (sentCode.type is TLRPC.TL_auth_sentCodeTypeCall) {
                    log("[Поток $threadId] Telegram отправляет звонок на номер +$digits")
                } else if (sentCode.type is TLRPC.TL_auth_sentCodeTypeFlashCall) {
                    log("[Поток $threadId] Telegram выполняет Flash-call на номер +$digits")
                } else if (sentCode.type is TLRPC.TL_auth_sentCodeTypeMissedCall) {
                    log("[Поток $threadId] Telegram выполняет обратный звонок на номер +$digits")
                } else {
                    log("[Поток $threadId] Telegram подтвердил запрос кода. Ожидаем код...")
                }

                // Firebase SMS support if requested by Telegram backend
                if (sentCode.type is TLRPC.TL_auth_sentCodeTypeFirebaseSms && mobileToken.isNotEmpty()) {
                    log("[Поток $threadId] Telegram запросил Firebase SMS, отправка токена Play Integrity...")
                    val req = TLRPC.TL_auth_requestFirebaseSms().apply {
                        phone_number = digits
                        phone_code_hash = phoneCodeHash
                        play_integrity_token = mobileToken
                        this.flags = this.flags or 4
                    }
                    cm.sendSuspend(req, flags, ConnectionsManager.DEFAULT_DATACENTER_ID, "requestFirebaseSms")
                }
                if (sentCode.type is TLRPC.TL_auth_sentCodeTypeApp) {
                    addBadNumber(digits)
                    if (config.skipAppCode) {
                        log("[Поток $threadId] Telegram отправил код в приложение (номер уже зарегистрирован). Отмена, запрос возврата средств.")
                        enqueueRefund(config, activationId, phone, orderedAt)
                        return false
                    }
                    log("[Поток $threadId] Telegram отправил код в приложение, а не SMS: номер, возможно, уже зарегистрирован.")
                }

                // Handle email setup requirement if Telegram demands email verification
                if (sentCode.type is TLRPC.TL_auth_sentCodeTypeSetUpEmailRequired) {
                    log("[Поток $threadId] Telegram требует подтверждения Email для номера +$digits")
                    if (!config.autoBuyEmail) {
                        log("[Поток $threadId] Покупка email отключена в настройках. Отмена, запрос возврата средств.")
                        enqueueRefund(config, activationId, phone, orderedAt)
                        return false
                    }
                    log("[Поток $threadId] Запрос временной почты в сервисе...")
                    val emailOrder = orderEmail(config)
                    if (emailOrder.isFailure) {
                        log("[Поток $threadId] Не удалось получить почту: ${emailOrder.exceptionOrNull()?.message}")
                        enqueueRefund(config, activationId, phone, orderedAt)
                        return false
                    }
                    val (mailId, email) = emailOrder.getOrThrow()
                    log("[Поток $threadId] Получена почта: $email (ID: $mailId). Отправка в Telegram...")
                    val sendVerifyReq = TL_account.sendVerifyEmailCode().apply {
                        this.purpose = TLRPC.TL_emailVerifyPurposeLoginSetup().apply {
                            this.phone_number = digits
                            this.phone_code_hash = phoneCodeHash
                        }
                        this.email = email
                    }
                    val (_, sendMailErr) = cm.sendSuspend(sendVerifyReq, flags, ConnectionsManager.DEFAULT_DATACENTER_ID, "sendVerifyEmailCode") ?: (null to null)
                    if (sendMailErr != null) {
                        log("[Поток $threadId] Ошибка Telegram при отправке email: ${sendMailErr.text}")
                        cancelEmail(config, mailId)
                        enqueueRefund(config, activationId, phone, orderedAt)
                        return false
                    }
                    log("[Поток $threadId] Telegram отправил код на почту $email. Ожидание кода...")
                    var emailCode: String? = null
                    val emailDeadline = System.currentTimeMillis() + 120_000L
                    while (System.currentTimeMillis() < emailDeadline && !cancelled.get()) {
                        delay(3000)
                        val (st, code, _) = pollEmailCode(config, mailId)
                        if (st == "ok" && !code.isNullOrEmpty()) {
                            emailCode = code
                            break
                        }
                    }
                    if (emailCode == null) {
                        log("[Поток $threadId] Время ожидания кода из почты истекло (120 сек).")
                        cancelEmail(config, mailId)
                        enqueueRefund(config, activationId, phone, orderedAt)
                        return false
                    }
                    log("[Поток $threadId] Код из почты получен: $emailCode. Верификация email в Telegram...")
                    val verifyReq = TL_account.verifyEmail().apply {
                        this.purpose = TLRPC.TL_emailVerifyPurposeLoginSetup().apply {
                            this.phone_number = digits
                            this.phone_code_hash = phoneCodeHash
                        }
                        this.verification = TLRPC.TL_emailVerificationCode().apply {
                            this.code = emailCode
                        }
                    }
                    val (verifyResp, verifyErr) = cm.sendSuspend(verifyReq, flags, ConnectionsManager.DEFAULT_DATACENTER_ID, "verifyEmail") ?: (null to null)
                    if (verifyErr != null) {
                        log("[Поток $threadId] Ошибка верификации email: ${verifyErr.text}")
                        cancelEmail(config, mailId)
                        enqueueRefund(config, activationId, phone, orderedAt)
                        return false
                    }
                    log("[Поток $threadId] Email $email успешно подтверждён!")

                    if (verifyResp is TL_account.TL_emailVerifiedLogin) {
                        val loginSentCode = verifyResp.sent_code
                        if (loginSentCode is TLRPC.TL_auth_sentCode) {
                            phoneCodeHash = loginSentCode.phone_code_hash
                            if (loginSentCode.type is TLRPC.TL_auth_sentCodeTypeApp) {
                                addBadNumber(digits)
                                if (config.skipAppCode) {
                                    log("[Поток $threadId] Telegram отправил код в активное приложение на номере (траст, номер уже занят). Отмена, средства возвращены.")
                                    enqueueRefund(config, activationId, phone, orderedAt)
                                    return false
                                }
                                log("[Поток $threadId] ⚠️ Telegram отправил код в активное приложение на этом номере, а не в SMS: номер уже кем-то используется.")
                            } else if (loginSentCode.type is TLRPC.TL_auth_sentCodeTypeSms) {
                                log("[Поток $threadId] ✓ Telegram подтвердил отправку SMS на номер +$digits! Ожидаем код от сервиса...")
                            } else if (loginSentCode.type is TLRPC.TL_auth_sentCodeTypeCall) {
                                log("[Поток $threadId] ✓ Telegram отправляет звонок на номер +$digits")
                            } else if (loginSentCode.type is TLRPC.TL_auth_sentCodeTypeFlashCall) {
                                log("[Поток $threadId] ✓ Telegram выполняет Flash-call на номер +$digits")
                            } else if (loginSentCode.type is TLRPC.TL_auth_sentCodeTypeMissedCall) {
                                log("[Поток $threadId] ✓ Telegram выполняет обратный звонок на номер +$digits")
                            } else {
                                log("[Поток $threadId] ✓ Telegram подтвердил запрос кода для номера +$digits")
                            }
                        } else if (loginSentCode is TLRPC.TL_auth_sentCodePaymentRequired) {
                            addBadNumber(digits)
                            log("[Поток $threadId] Telegram требует платную отправку SMS / Premium для номера +$digits. Отмена, возврат средств.")
                            enqueueRefund(config, activationId, phone, orderedAt)
                            return false
                        }
                    }
                }

                // Mark SMS status 1 (code requested)
                setSmsStatus(config, activationId, 1)
                log("[Поток $threadId] Ожидаем SMS-код от сервиса (макс. ${config.smsWaitSeconds} сек)...")

                // 6. Poll for SMS code (config.smsWaitSeconds)
                var smsCode: String? = null
                val deadline = System.currentTimeMillis() + config.smsWaitSeconds * 1000L
                while (System.currentTimeMillis() < deadline && !cancelled.get()) {
                    if (!waitIfPausedOrNoNetwork()) break
                    delay(3000)
                    val (st, code, pollErr) = pollSmsCode(config, activationId)
                    if (st == "ok" && !code.isNullOrEmpty()) {
                        smsCode = code
                        break
                    } else if (st == "cancel") {
                        log("[Поток $threadId] Активация отменена SMS-сервисом")
                        return false
                    }
                }

                if (cancelled.get() || smsCode == null) {
                    log("[Поток $threadId] SMS не пришло за ${config.smsWaitSeconds} сек. Автоматический возврат средств...")
                    enqueueRefund(config, activationId, phone, orderedAt)
                    return false
                }
                log("[Поток $threadId] SMS-код получен: $smsCode")

                // 7. Reading & typing delay
                stepDelay(threadId, "Пауза перед вводом SMS", config)

                // 8. Sign In
                log("[Поток $threadId] Авторизация в Telegram...")
                val (signed, signErr) = cm.sendSuspend(TLRPC.TL_auth_signIn().apply {
                    this.flags = this.flags or 1
                    phone_number = digits
                    phone_code_hash = phoneCodeHash
                    phone_code = smsCode
                }, flags, ConnectionsManager.DEFAULT_DATACENTER_ID, "signIn") ?: (null to null)

                if (cancelled.get()) return false

                var auth = signed as? TLRPC.TL_auth_authorization

                if (signErr != null) {
                    when (signErr.text) {
                        "PHONE_NUMBER_UNOCCUPIED" -> {
                            // New user registration — name generation or fixed name for all
                            val fName: String
                            val lName: String
                            if (config.singleNameForAll || !config.autoRandomNames) {
                                fName = config.customFirstName.trim().ifEmpty { generateRandomFirstName(config.countryId, config.gender) }
                                lName = config.customLastName.trim().ifEmpty { generateRandomLastName(config.countryId, config.gender) }
                            } else {
                                fName = readRandomLineFromFile(config.firstNameFile)
                                    ?: generateRandomFirstName(config.countryId, config.gender)
                                lName = readRandomLineFromFile(config.lastNameFile)
                                    ?: generateRandomLastName(config.countryId, config.gender)
                            }

                            stepDelay(threadId, "Пауза перед созданием профиля ($fName $lName)", config)

                            log("[Поток $threadId] Регистрация нового аккаунта: $fName $lName...")
                            val (signedUp, signUpErr) = cm.sendSuspend(TLRPC.TL_auth_signUp().apply {
                                phone_number = digits
                                phone_code_hash = phoneCodeHash
                                first_name = fName
                                last_name = lName
                            }, flags, ConnectionsManager.DEFAULT_DATACENTER_ID, "signUp") ?: (null to null)

                            if (signUpErr != null) {
                                log("[Поток $threadId] Ошибка регистрации: ${signUpErr.text}")
                                return false
                            }
                            auth = signedUp as? TLRPC.TL_auth_authorization
                        }
                        "SESSION_PASSWORD_NEEDED" -> {
                            if (config.reset2faOnOccupied) {
                                log("[Поток $threadId] На номере установлен 2FA. Отправка запроса сброса пароля (resetPassword)...")
                                val (resetRes, resetErr) = cm.sendSuspend(
                                    TL_account.resetPassword(),
                                    flags, ConnectionsManager.DEFAULT_DATACENTER_ID, "resetPassword"
                                ) ?: (null to null)
                                if (resetErr != null) {
                                    log("[Поток $threadId] Сброс 2FA не удался: ${resetErr.text}")
                                } else {
                                    val waitMsg = when (resetRes) {
                                        is TL_account.resetPasswordRequestedWait -> "Запрос сброса 2FA принят (ожидание ${resetRes.until_date} сек)."
                                        is TL_account.resetPasswordFailedWait -> "Сброс 2FA заблокирован Telegram до ${resetRes.retry_date} сек."
                                        is TL_account.resetPasswordOk -> "2FA пароль успешно сброшен!"
                                        else -> "Запрос сброса 2FA отправлен."
                                    }
                                    log("[Поток $threadId] $waitMsg")
                                }
                            } else {
                                log("[Поток $threadId] На номере установлен чужой 2FA пароль. Номер отменён.")
                            }
                            addBadNumber(digits)
                            enqueueRefund(config, activationId, phone, orderedAt)
                            return false
                        }
                        else -> {
                            log("[Поток $threadId] Ошибка входа: ${signErr.text}")
                            return false
                        }
                    }
                }

                if (auth == null) {
                    log("[Поток $threadId] Не удалось завершить авторизацию Telegram")
                    return false
                }

                val user = auth.user
                cm.setUserId(user.id)
                val dcId = cm.currentDatacenterId
                if (dcId != ConnectionsManager.DEFAULT_DATACENTER_ID) {
                    cm.setDefaultDatacenterId(dcId)
                }

                // Step delay before 2FA setup
                stepDelay(threadId, "Пауза перед настройкой 2FA", config)

                // 11. 2FA Password
                val final2fa = if (config.single2faForAll) {
                    config.twoFactorPassword.trim()
                } else {
                    generateStrongPassword(18)
                }
                if (final2fa.isNotEmpty()) {
                    UserConfig.getInstance(slot).ncgTwoFactor = final2fa
                    GgtgTwoFaStore.save(user.id, final2fa)
                    log("[Поток $threadId] Пароль 2FA установлен: $final2fa (сохранён в хранилище приложения)")
                }

                // 12. Folder Tag
                if (config.folderTag.isNotEmpty()) {
                    UserConfig.getInstance(slot).ncgFolder = config.folderTag
                }

                // 13. Finish SMS activation (status 6)
                setSmsStatus(config, activationId, 6)
                pendingActivations.remove(activationId)
                orderTimestamps.remove(activationId)

                // 13.5 Auto Bio & Username setup (TG-Giant style)
                if (config.autoSetBio) {
                    val bioText = if (config.customBio.isNotBlank()) {
                        GgtgSms.render(config.customBio, user.first_name.orEmpty(), user.last_name.orEmpty(), "", "").trim()
                    } else {
                        val presets = listOf(
                            "Hey there! I am using Telegram.",
                            "Busy, text only.",
                            "Available on Telegram.",
                            "Digital creator 🚀",
                            "Tech & crypto enthusiast"
                        )
                        presets.random()
                    }
                    if (bioText.isNotEmpty()) {
                        val (bioOk, _) = GgtgBulk.updateProfile(slot, null, null, bioText.take(70))
                        if (bioOk) log("[Поток $threadId] ✓ Установлен профиль Bio: «$bioText»")
                    }
                }
                if (config.autoSetUsername) {
                    val cleanFirst = user.first_name.orEmpty().filter { (it in 'a'..'z') || (it in 'A'..'Z') || (it in '0'..'9') }
                    val cleanLast = user.last_name.orEmpty().filter { (it in 'a'..'z') || (it in 'A'..'Z') || (it in '0'..'9') }
                    val baseName = (cleanFirst + "_" + cleanLast).trim('_').ifEmpty { "user" }
                    val candidate = (baseName + "_" + Random.nextInt(100, 9999)).take(32)
                    val (userOk, userRes) = GgtgBulk.updateUsername(slot, candidate)
                    if (userOk) log("[Поток $threadId] ✓ Установлен @username: $userRes")
                }

                // 14. Reset other sessions if configured
                if (config.resetSessionsAfterReg) {
                    log("[Поток $threadId] Сброс других сессий (ResetAuthorizations)...")
                    val (_, resetErr) = cm.sendSuspend(
                        TLRPC.TL_auth_resetAuthorizations(),
                        ConnectionsManager.RequestFlagFailOnServerErrors, ConnectionsManager.DEFAULT_DATACENTER_ID, "resetAuthorizations"
                    ) ?: (null to null)
                    if (resetErr != null) {
                        log("[Поток $threadId] Предупреждение: сброс сессий не удался: ${resetErr.text}")
                    } else {
                        log("[Поток $threadId] Другие сессии сброшены.")
                    }
                }

                // 16. Activate slot in GGTG Android - directly start using this session!
                val activateRes = GgtgImportSupport.activateSlot(slot, user)
                if (activateRes.isFailure) {
                    log("[Поток $threadId] Ошибка активации слота: ${activateRes.exceptionOrNull()?.message}")
                    return false
                }

                // 17. Proxy consumed only upon confirmed successful registration
                if (proxyEntry != null && config.proxyListName.isNotEmpty()) {
                    GgtgProxyPoolStore.removeEntry(config.proxyListName, proxyEntry.host, proxyEntry.port)
                    log("[Поток $threadId] ✓ Прокси ${proxyEntry.host}:${proxyEntry.port} закреплён за аккаунтом и удалён из пула.")
                }

                // Make the account directly available and active in Android without any desktop duplication
                AndroidUtilities.runOnUIThread {
                    val users = ArrayList<TLRPC.User>()
                    users.add(user)
                    MessagesStorage.getInstance(slot).putUsersAndChats(users, null, true, true)
                    MessagesController.getInstance(slot).putUser(user, false)
                    MessagesController.getInstance(slot).checkPromoInfo(true)
                    MessagesController.getInstance(slot).loadAppConfig()
                    ConnectionsManager.getInstance(slot).updateDcSettings()
                    GgtgImportSupport.selectImportedSlotIfIdle(slot)
                    NotificationCenter.getInstance(slot).postNotificationName(NotificationCenter.mainUserInfoChanged)
                    NotificationCenter.getGlobalInstance().postNotificationName(NotificationCenter.mainUserInfoChanged)
                    NotificationCenter.getInstance(slot).postNotificationName(NotificationCenter.dialogsNeedReload)
                }

                log("[Поток $threadId] ✓ Аккаунт +$phone (${user.first_name}) готов к использованию в приложении Android (слот #$slot)!")
                return true

            } catch (t: Throwable) {
                log("[Поток $threadId] Исключение: ${t.message ?: t.javaClass.simpleName}")
                return false
            } finally {
                // Reserve token if acquired but never consumed by Telegram
                if (mobileToken.isNotEmpty() && !tokenSpent) {
                    reserveToken(mobileToken)
                    log("[Поток $threadId] Неиспользованный токен сохранён в резерв (в резерве: ${tokenReserve.size})")
                }
                // Cancel SMS if failed to refund
                if (activationId != null && !UserConfig.getInstance(slot).isClientActivated) {
                    val ordered = orderTimestamps[activationId] ?: System.currentTimeMillis()
                    enqueueRefund(config, activationId, phone ?: "", ordered)
                }
                // Release proxy from in-use set
                if (proxyEntry != null) {
                    inUseProxies.remove("${proxyEntry.host}:${proxyEntry.port}")
                }
                // If not activated, clean up slot
                if (!UserConfig.getInstance(slot).isClientActivated) {
                    ConnectionsManager.getInstance(slot).cleanup(true)
                    UserConfig.getInstance(slot).clearConfig()
                    reservedSlots.remove(slot)
                }
            }
        }
    }
}
