package app.ggtg

import org.telegram.ui.Components.GgtgProxyPoolStore

// ng: whatever the user picked in GgtgProxyChooser - either a named, persisted list (each pop
// permanently removes that proxy so it can never reach a second account) or a single manually
// typed proxy (usable exactly once).
sealed class GgtgProxySource {
    abstract fun peekFirst(): GgtgProxyPoolStore.Entry?
    abstract fun popNext(): GgtgProxyPoolStore.Entry?

    class NamedList(val name: String) : GgtgProxySource() {
        override fun peekFirst(): GgtgProxyPoolStore.Entry? = GgtgProxyPoolStore.peekFirst(name)
        override fun popNext(): GgtgProxyPoolStore.Entry? = GgtgProxyPoolStore.popFirst(name)
    }

    class Manual(entry: GgtgProxyPoolStore.Entry) : GgtgProxySource() {
        private var remaining: GgtgProxyPoolStore.Entry? = entry
        override fun peekFirst(): GgtgProxyPoolStore.Entry? = remaining
        override fun popNext(): GgtgProxyPoolStore.Entry? = remaining.also { remaining = null }
    }
}

// ng: plain (address, port, username, password, secret) tuple - what
// ConnectionsManager.setProxySettingsForAccount actually takes. Simpler than routing through
// SharedConfig.ProxyInfo/ProxySettings (a builder-based, type-tagged class meant for the app-wide
// proxy picker UI) just to immediately unpack it again for a per-account assignment.
data class GgtgProxyBits(val address: String, val port: Int, val username: String, val password: String, val secret: String = "")

fun GgtgProxyPoolStore.Entry.toProxyBits() = GgtgProxyBits(host, port, username, password)
