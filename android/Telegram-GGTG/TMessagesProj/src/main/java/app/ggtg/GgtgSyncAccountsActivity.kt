package app.ggtg

import android.content.Context
import android.view.View
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.Toast
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.telegram.messenger.ApplicationLoader
import org.telegram.messenger.R
import org.telegram.ui.ActionBar.ActionBar.ActionBarMenuOnItemClick
import org.telegram.ui.ActionBar.AlertDialog
import org.telegram.ui.ActionBar.BaseFragment
import org.telegram.ui.ActionBar.Theme
import org.telegram.ui.Components.GgtgProxyChooser
import org.telegram.ui.Components.LayoutHelper
import java.io.File

// ng: accounts between this phone and GGTG Desktop through the sync's main account: every account gets a new,
// independent session (so no session is ever shared and none breaks), carried as an encrypted file in the main
// account's "Saved messages". Needs the sync set up first (GgtgSyncActivity).
class GgtgSyncAccountsActivity : BaseFragment() {

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main)
    private var picker: GgtgAccountPicker? = null

    override fun createView(context: Context): View {
        actionBar.setBackButtonImage(R.drawable.ic_ab_back)
        actionBar.setAllowOverlayTitle(true)
        actionBar.setTitle("Аккаунты Desktop ↔ телефон")
        actionBar.setActionBarMenuOnItemClick(object : ActionBarMenuOnItemClick() {
            override fun onItemClick(id: Int) { if (id == -1) finishFragment() }
        })
        val frame = FrameLayout(context)
        frame.setBackgroundColor(Theme.getColor(Theme.key_windowBackgroundGray))
        fragmentView = frame
        val root = GgtgUi.page(context, frame)

        GgtgUi.addCard(root, GgtgUi.infoCard(context, "", "Как это работает",
            "Каждому аккаунту выдаётся новая отдельная сессия, поэтому на телефоне и на Desktop аккаунт работает одновременно и сессии не рвутся. " +
                "Файл с сессиями идёт через «Избранное» главного аккаунта и зашифрован вашей фразой."), top = 10f)

        root.addView(GgtgUi.sectionLabel(context, "Принять с Desktop"))
        GgtgUi.addCard(root, GgtgUi.actionCard(context, "", "Принять аккаунты с Desktop",
            "На Desktop: «Синхронизация» → «Отправить аккаунты на Android…»") { receive() })

        root.addView(GgtgUi.sectionLabel(context, "Отправить на Desktop"))
        val accounts = GgtgUi.card(context)
        val p = GgtgAccountPicker(context)
        p.setBackgroundColor(android.graphics.Color.TRANSPARENT)
        accounts.addView(p, LinearLayout.LayoutParams(LayoutHelper.MATCH_PARENT, LayoutHelper.WRAP_CONTENT))
        GgtgUi.addCard(root, accounts)
        picker = p
        GgtgUi.addButton(root, GgtgUi.button(context, "Отправить выбранные на Desktop") { send() }, top = 14f)
        root.addView(GgtgUi.spacer(context))
        return frame
    }

    private fun toast(text: String) { context?.let { Toast.makeText(it, text, Toast.LENGTH_LONG).show() } }

    private fun inform(title: String, text: String) {
        val context = context ?: return
        GgtgApiOverride.inform(context, title, text)
    }

    private fun send() {
        val context = context ?: return
        val activity = parentActivity as? androidx.fragment.app.FragmentActivity ?: return
        val slots = picker?.selectedSlots().orEmpty()
        if (slots.isEmpty()) { toast("Выберите хотя бы один аккаунт"); return }
        if (GgtgSyncTransport.mainSlot() == null || GgtgSync.loadSettings().passphrase.isEmpty()) {
            inform("Синхронизация", "Сначала настройте синхронизацию: главный аккаунт и фраза-пароль.")
            return
        }
        // the new sessions are made through a throwaway slot that goes out through the account's own proxy
        TdataExportHelper.exportForDesktop(activity, slots) { manifest, summary ->
            if (manifest == null) {
                inform("Аккаунты на Desktop", summary)
            } else {
                GgtgSyncTransport.sendAccounts("keys", manifest) { ok, text ->
                    inform("Аккаунты на Desktop", if (ok) "$summary\n$text. На Desktop: «Синхронизация» → «Принять аккаунты с Android…»." else "Не отправилось: $text")
                }
            }
        }
    }

    private fun receive() {
        val context = context ?: return
        GgtgSyncTransport.findIncoming { list, error ->
            if (error.isNotEmpty()) { toast(error); return@findIncoming }
            val remote = list.firstOrNull()
            if (remote == null) { toast("С Desktop пока ничего не пришло"); return@findIncoming }
            GgtgSyncTransport.fetchIncoming(remote) { bytes, problem ->
                if (bytes == null) { toast(problem); return@fetchIncoming }
                val copy = File(ApplicationLoader.applicationContext.cacheDir, "ggtg_sync_accounts.ggtg")
                try { copy.writeBytes(bytes) } catch (t: Throwable) { toast("Не удалось сохранить файл"); return@fetchIncoming }
                if (!GgtgPackImporter.isPack(copy)) { copy.delete(); toast("Это не экспорт GGTG"); return@fetchIncoming }
                if (GgtgPackImporter.needsProxyPool(copy)) {
                    GgtgProxyChooser.show(context) { source -> runImport(copy, source) }
                } else {
                    runImport(copy, null)
                }
            }
        }
    }

    private fun runImport(file: File, source: GgtgProxySource?) {
        val activity = parentActivity ?: return
        val progress = app.ggtg.ui.TdataImportProgressDialog(activity)
        progress.show()
        scope.launch {
            try {
                val result = withContext(Dispatchers.IO) { GgtgPackImporter.import(file, source, progress) }
                try { progress.dismiss() } catch (_: Throwable) {}
                val act = parentActivity
                val msg = GgtgImportSupport.buildResultMessage(result)
                if (act != null && !act.isFinishing && !act.isDestroyed) {
                    try {
                        AlertDialog.Builder(act).setTitle("Аккаунты с Desktop")
                            .setMessage(msg)
                            .setPositiveButton("Закрыть", null).show()
                    } catch (t: Throwable) {
                        toast(msg)
                    }
                } else {
                    toast(msg)
                }
            } catch (t: Throwable) {
                try { progress.dismiss() } catch (_: Throwable) {}
                toast("Ошибка импорта: ${t.message ?: t.javaClass.simpleName}")
            } finally {
                withContext(Dispatchers.IO) { file.delete() }
            }
        }
    }
}
