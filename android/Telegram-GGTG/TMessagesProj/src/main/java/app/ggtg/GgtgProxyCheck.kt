package app.ggtg

import java.net.Authenticator
import java.net.HttpURLConnection
import java.net.InetSocketAddress
import java.net.PasswordAuthentication
import java.net.Proxy
import java.net.URL

// ng: "Проверка прокси" from GGTG Desktop's proxy list editor - fetches the exit IP through the
// SOCKS5 proxy (api.ipify.org, then ip-api.com for the country) so a dead or wrong proxy shows up
// before it is handed to an account.
object GgtgProxyCheck {

    data class Result(val ok: Boolean, val text: String)

    private val lock = Any()

    fun check(host: String, port: Int, user: String, password: String): Result = synchronized(lock) {
        try {
            Authenticator.setDefault(object : Authenticator() {
                override fun getPasswordAuthentication(): PasswordAuthentication? =
                    if (user.isNotEmpty()) PasswordAuthentication(user, password.toCharArray()) else null
            })
            val proxy = Proxy(Proxy.Type.SOCKS, InetSocketAddress.createUnresolved(host, port))
            val ip = fetch("https://api.ipify.org", proxy).trim()
            if (ip.isEmpty()) return Result(false, "Прокси не работает: пустой ответ")
            val country = try {
                Regex("\"country\":\"([^\"]*)\"").find(fetch("http://ip-api.com/json/$ip?fields=country", proxy))?.groupValues?.get(1)
            } catch (t: Throwable) { null }
            Result(true, "Прокси работает. Выходной IP: $ip" + if (country.isNullOrEmpty()) "" else " ($country)")
        } catch (t: Throwable) {
            Result(false, "Прокси не работает: ${t.message ?: t.javaClass.simpleName}")
        } finally {
            Authenticator.setDefault(null)
        }
    }

    private fun fetch(url: String, proxy: Proxy): String {
        val conn = URL(url).openConnection(proxy) as HttpURLConnection
        conn.connectTimeout = 12_000
        conn.readTimeout = 12_000
        return try { conn.inputStream.bufferedReader().readText() } finally { conn.disconnect() }
    }
}
