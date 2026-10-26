package app.ggtg

import android.Manifest
import android.content.Context
import android.content.pm.PackageManager
import android.os.Build
import android.widget.Toast
import android.text.Editable
import android.text.TextWatcher
import android.view.View
import android.widget.CheckBox
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.TextView
import org.telegram.messenger.AndroidUtilities
import org.telegram.messenger.R
import org.telegram.messenger.UserConfig
import org.telegram.messenger.UserObject
import org.telegram.ui.ActionBar.ActionBar.ActionBarMenuOnItemClick
import org.telegram.ui.ActionBar.BaseFragment
import org.telegram.ui.ActionBar.Theme
import org.telegram.ui.CameraScanActivity
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

// ng: "Синхронизация" - one main Telegram account carries GGTG's data (bases, who was written to, templates, the
// AI's memory) between this phone and GGTG Desktop. The phone joins by scanning the QR code of the desktop's
// "Подключить телефон" (the main account and the key); from then on it all goes by itself, and every mailing first
// takes in the desktop's records. The desktop's side is settings/ggtg_sync_box.h.
class GgtgSyncActivity : BaseFragment() {

    private var content: LinearLayout? = null
    private var status: TextView? = null
    // a failed "Синхронизировать сейчас", shown for a while over the usual state
    private var failure = ""
    private var failureAt = 0L
    private val ticker = object : Runnable {
        override fun run() {
            refreshStatus()
            AndroidUtilities.runOnUIThread(this, 1000)
        }
    }

    override fun createView(context: Context): View {
        actionBar.setBackButtonImage(R.drawable.ic_ab_back)
        actionBar.setAllowOverlayTitle(true)
        actionBar.setTitle("Синхронизация")
        actionBar.setActionBarMenuOnItemClick(object : ActionBarMenuOnItemClick() {
            override fun onItemClick(id: Int) {
                if (id == -1) finishFragment()
            }
        })
        val frame = FrameLayout(context)
        frame.setBackgroundColor(Theme.getColor(Theme.key_windowBackgroundGray))
        fragmentView = frame
        content = GgtgUi.page(context, frame)
        build(context)
        return frame
    }

    override fun onResume() {
        super.onResume()
        AndroidUtilities.cancelRunOnUIThread(ticker)
        AndroidUtilities.runOnUIThread(ticker)
    }

    override fun onPause() {
        super.onPause()
        AndroidUtilities.cancelRunOnUIThread(ticker)
    }

    override fun onFragmentDestroy() {
        AndroidUtilities.cancelRunOnUIThread(ticker)
        super.onFragmentDestroy()
    }

    private fun build(context: Context) {
        val column = content ?: return
        column.removeAllViews()
        val settings = GgtgSync.loadSettings()

        val intro = GgtgUi.card(context)
        intro.addView(GgtgUi.body(context, "Базы, «уже написали», шаблоны и память ИИ сами переходят между телефоном и Mac: при " +
            "запуске, после изменений и каждые 5 минут. Перед каждой рассылкой GGTG сначала забирает с Mac, кому там уже " +
            "написали, так что одному человеку не напишут дважды."))
        GgtgUi.addCard(column, intro)

        // ---- the state ----
        val state = GgtgUi.card(context)
        val stateText = GgtgUi.body(context, "", 14f)
        status = stateText
        state.addView(stateText)
        GgtgUi.addCard(column, state)
        refreshStatus()

        // ---- pairing ----
        GgtgUi.addCard(column, GgtgUi.actionCard(context, "", "Сканировать QR-код с Mac",
            "На Mac: «Синхронизация» → «Подключить телефон по QR-коду». Больше ничего вводить не нужно.") {
            scan()
        })

        // ---- the main account ----
        column.addView(GgtgUi.sectionLabel(context, "Главный аккаунт"))
        val accounts = GgtgUi.card(context)
        // one line with the chosen account; the whole list (there may be a hundred) opens on a tap, with a search
        val entries = ArrayList<Pair<String, String>>()
        for (slot in 0 until UserConfig.MAX_ACCOUNT_COUNT) {
            val config = UserConfig.getInstance(slot)
            if (!config.isClientActivated) continue
            val user = config.currentUser ?: continue
            entries.add(config.clientUserId.toString() to
                (UserObject.getUserName(user) + (user.username?.takeIf { it.isNotEmpty() }?.let { "  @$it" } ?: "")))
        }
        fun labelOf(id: String) = entries.firstOrNull { it.first == id }?.second ?: if (id.isEmpty()) "не выбран" else "id $id (не вошёл здесь)"
        val current = GgtgUi.title(context, labelOf(settings.mainUserId) + "  ▾", 15f)
        current.setPadding(0, GgtgUi.dp(8f), 0, GgtgUi.dp(8f))
        current.setOnClickListener {
            val box = LinearLayout(context).apply { orientation = LinearLayout.VERTICAL; setPadding(GgtgUi.dp(20f), 0, GgtgUi.dp(20f), 0) }
            val search = GgtgUi.input(context, "Поиск по имени или @username")
            box.addView(search)
            val list = LinearLayout(context).apply { orientation = LinearLayout.VERTICAL }
            val scroll = android.widget.ScrollView(context).apply { addView(list) }
            box.addView(scroll, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, GgtgUi.dp(320f)))
            val dialog = org.telegram.ui.ActionBar.AlertDialog.Builder(context).setTitle("Главный аккаунт").setView(box)
                .setNegativeButton("Закрыть", null).create()
            fun fill() {
                list.removeAllViews()
                val query = search.text.toString().trim().lowercase()
                var shown = 0
                val chosen = GgtgSync.loadSettings().mainUserId
                for ((id, label) in entries) {
                    if (query.isNotEmpty() && !label.lowercase().contains(query)) continue
                    if (++shown > 40) { list.addView(GgtgUi.body(context, "Показаны первые 40 - уточните поиск.")); break }
                    val row = GgtgUi.title(context, (if (id == chosen) "● " else "○ ") + label, 15f)
                    row.setPadding(0, GgtgUi.dp(10f), 0, GgtgUi.dp(10f))
                    row.setOnClickListener {
                        GgtgSync.saveSettings(GgtgSync.loadSettings().copy(mainUserId = id))
                        GgtgSyncTransport.restart()
                        current.text = label + "  ▾"
                        refreshStatus()
                        dialog.dismiss()
                    }
                    list.addView(row)
                }
            }
            search.addTextChangedListener(object : TextWatcher {
                override fun beforeTextChanged(s: CharSequence?, a: Int, b: Int, c: Int) {}
                override fun onTextChanged(s: CharSequence?, a: Int, b: Int, c: Int) {}
                override fun afterTextChanged(s: Editable?) { fill() }
            })
            fill()
            dialog.show()
        }
        accounts.addView(current)
        accounts.addView(GgtgUi.body(context, if (entries.isEmpty()) "Нет вошедших аккаунтов." else
            "Ставится сам при сканировании QR-кода. Через его «Избранное» идут данные.", 12f))
        GgtgUi.addCard(column, accounts)

        // ---- on / off ----
        val auto = GgtgUi.card(context)
        val check = CheckBox(context).apply {
            text = "Синхронизация включена"
            isChecked = settings.enabled && settings.passphrase.isNotEmpty()
            setTextColor(Theme.getColor(Theme.key_windowBackgroundWhiteBlackText))
            setOnCheckedChangeListener { _, checked ->
                GgtgSync.saveSettings(GgtgSync.loadSettings().copy(enabled = checked))
                GgtgSyncTransport.restart()
                refreshStatus()
            }
        }
        auto.addView(check)

        // ---- the key, for setting up by hand ----
        val manual = GgtgUi.body(context, "Ключ вручную (без QR-кода)  ▾", 14f)
        manual.setPadding(0, GgtgUi.dp(10f), 0, GgtgUi.dp(4f))
        val phraseBox = LinearLayout(context).apply { orientation = LinearLayout.VERTICAL; visibility = View.GONE }
        val phrase = GgtgUi.input(context, "Ключ (одинаковый на всех устройствах)", settings.passphrase)
        phrase.addTextChangedListener(object : TextWatcher {
            override fun beforeTextChanged(s: CharSequence?, a: Int, b: Int, c: Int) {}
            override fun onTextChanged(s: CharSequence?, a: Int, b: Int, c: Int) {}
            override fun afterTextChanged(s: Editable?) {
                GgtgSync.saveSettings(GgtgSync.loadSettings().copy(passphrase = s.toString().trim()))
                GgtgSyncTransport.restart()
            }
        })
        phraseBox.addView(phrase)
        phraseBox.addView(GgtgUi.body(context, "Ставится сам при сканировании QR-кода. Должен совпадать с ключом на Desktop.", 12f))
        manual.setOnClickListener { phraseBox.visibility = if (phraseBox.visibility == View.GONE) View.VISIBLE else View.GONE }
        auto.addView(manual)
        auto.addView(phraseBox)
        GgtgUi.addCard(column, auto)

        // ---- accounts between the devices ----
        GgtgUi.addCard(column, GgtgUi.actionCard(context, "", "Аккаунты между устройствами",
            "Принять аккаунты с Desktop или отправить свои на Desktop — новой отдельной сессией, без разрыва.") {
            presentFragment(GgtgSyncAccountsActivity())
        })

        GgtgUi.addCard(column, GgtgUi.actionCard(context, "", "Свой api_id",
            "Если Telegram отказывает в новой сессии (API_ID_PUBLISHED_FLOOD): как получить и куда ввести.") {
            GgtgApiOverride.showDialog(context)
        })

        // not needed for the sync to work: for when one can't wait
        GgtgUi.addButton(column, GgtgUi.button(context, "Синхронизировать сейчас") {
            if (GgtgSyncTransport.running) return@button
            GgtgSyncTransport.runSync { report ->
                if (!report.ok) { failure = "Не вышло: " + report.text; failureAt = System.currentTimeMillis() }
                refreshStatus()
            }
            refreshStatus()
        }, 14f)
        column.addView(GgtgUi.spacer(context, 24f))
    }

    private fun scan() {
        val activity = parentActivity ?: return
        if (Build.VERSION.SDK_INT >= 23 && activity.checkSelfPermission(Manifest.permission.CAMERA) != PackageManager.PERMISSION_GRANTED) {
            activity.requestPermissions(arrayOf(Manifest.permission.CAMERA), CAMERA_REQUEST)
            return
        }
        CameraScanActivity.showAsSheet(this, false, CameraScanActivity.TYPE_QR, object : CameraScanActivity.CameraScanActivityDelegate {
            override fun didFindQr(text: String) {
                AndroidUtilities.runOnUIThread { paired(text) }
            }
        })
    }

    override fun onRequestPermissionsResultFragment(requestCode: Int, permissions: Array<out String>?, grantResults: IntArray?) {
        if (requestCode != CAMERA_REQUEST) return
        if (grantResults != null && grantResults.isNotEmpty() && grantResults[0] == PackageManager.PERMISSION_GRANTED) {
            scan()
        } else {
            parentActivity?.let { Toast.makeText(it, "Нужен доступ к камере, чтобы отсканировать QR-код", Toast.LENGTH_LONG).show() }
        }
    }

    private fun paired(text: String) {
        val context = parentActivity ?: return
        val pairing = GgtgSync.parsePairing(text)
        if (pairing == null) {
            Toast.makeText(context, "Это не QR-код синхронизации GGTG", Toast.LENGTH_LONG).show()
            return
        }
        GgtgSync.saveSettings(GgtgSync.loadSettings().copy(
            enabled = true,
            mainUserId = pairing.mainUserId,
            passphrase = pairing.passphrase,
            // a new key: what was merged with the old one says nothing about the files made with this one
            lastUploadedHash = "",
            seen = emptyMap()))
        GgtgSyncTransport.restart()
        build(context)
        if (GgtgSyncTransport.mainSlot() == null) {
            org.telegram.ui.ActionBar.AlertDialog.Builder(context)
                .setTitle("Почти готово")
                .setMessage("Главный аккаунт с Desktop (id ${pairing.mainUserId}) не вошёл на этом телефоне. Войдите в него — " +
                    "или примите его через «Аккаунты между устройствами» — и синхронизация начнётся сама.")
                .setPositiveButton("Понятно", null)
                .show()
            return
        }
        Toast.makeText(context, "Телефон подключён к Desktop", Toast.LENGTH_LONG).show()
        GgtgSyncTransport.runSync { refreshStatus() }
        refreshStatus()
    }

    private fun refreshStatus() {
        val view = status ?: return
        val s = GgtgSync.loadSettings()
        val text = when {
            failure.isNotEmpty() && System.currentTimeMillis() - failureAt < 15_000 -> failure
            !s.enabled || s.passphrase.isEmpty() -> "○ Выключена. Отсканируйте QR-код с Desktop."
            GgtgSyncTransport.mainSlot() == null -> "Главный аккаунт не выбран или не вошёл на этом телефоне"
            GgtgSyncTransport.running -> "↻ Синхронизирую…"
            s.lastSyncAt > 0 -> "" + SimpleDateFormat("dd.MM HH:mm", Locale.getDefault()).format(Date(s.lastSyncAt * 1000)) +
                " · " + s.lastResult
            else -> "↻ Скоро синхронизирую…"
        } + (if (s.passphrase.isNotEmpty()) "\nКлюч: ${GgtgSync.keyId(s.passphrase)} — на Desktop должен быть такой же" else "")
        AndroidUtilities.runOnUIThread { if (view.text.toString() != text) view.text = text }
    }

    private companion object {
        const val CAMERA_REQUEST = 3471
    }
}
