package app.ggtg

import android.app.Activity
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.view.Gravity
import android.view.View
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import android.widget.Toast
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.telegram.messenger.AndroidUtilities
import org.telegram.messenger.ApplicationLoader
import org.telegram.messenger.R
import org.telegram.ui.ActionBar.ActionBar.ActionBarMenuOnItemClick
import org.telegram.ui.ActionBar.AlertDialog
import org.telegram.ui.ActionBar.BaseFragment
import org.telegram.ui.ActionBar.Theme
import org.telegram.ui.Cells.TextCell
import org.telegram.ui.Components.GgtgProxyChooser
import org.telegram.ui.Components.LayoutHelper
import java.io.File

// ng: "Импорт .ggtg" - installs a pack written by GGTG Desktop's "Экспорт" section (a zip with
// manifest.json holding each account's own brand-new session, folder, 2FA and proxy). Accounts
// whose own proxy can't be used pull one from a chosen pool instead.
class GgtgImportActivity : BaseFragment() {

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main)

    override fun createView(context: Context): View {
        actionBar.setBackButtonImage(R.drawable.ic_ab_back)
        actionBar.setAllowOverlayTitle(true)
        actionBar.setTitle("Импорт .ggtg")
        actionBar.setActionBarMenuOnItemClick(object : ActionBarMenuOnItemClick() {
            override fun onItemClick(id: Int) { if (id == -1) finishFragment() }
        })

        val frame = FrameLayout(context)
        frame.setBackgroundColor(Theme.getColor(Theme.key_windowBackgroundGray))
        fragmentView = frame
        val root = GgtgUi.page(context, frame)

        GgtgUi.addCard(root, GgtgUi.infoCard(context, "", "Что это",
            "Файл .ggtg создаётся в GGTG Desktop (раздел «Экспорт»): каждому аккаунту там выдаётся новая отдельная сессия, папка, прокси и пароль 2FA. Здесь он ставится напрямую, без повторного дублирования."), top = 10f)
        GgtgUi.addCard(root, GgtgUi.actionCard(context, "", "Выбрать файл .ggtg", "Откроется выбор файла на телефоне") {
            val intent = Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
                addCategory(Intent.CATEGORY_OPENABLE)
                type = "*/*"
            }
            startActivityForResult(intent, PICK_REQUEST)
        })
        return frame
    }

    override fun onActivityResultFragment(requestCode: Int, resultCode: Int, data: Intent?) {
        if (requestCode != PICK_REQUEST || resultCode != Activity.RESULT_OK) return
        val uri = data?.data ?: return
        val context = context ?: return
        val copy = File(ApplicationLoader.applicationContext.cacheDir, "ggtg_import_temp.ggtg")
        try {
            context.contentResolver.openInputStream(uri)?.use { input -> copy.outputStream().use { input.copyTo(it) } }
        } catch (t: Throwable) {
            toast("Не удалось прочитать файл")
            return
        }
        if (!GgtgPackImporter.isPack(copy)) {
            copy.delete()
            toast("Это не экспорт GGTG (.ggtg)")
            return
        }
        val needsPool = GgtgPackImporter.needsProxyPool(copy)
        GgtgImportSupport.askCreateBackupDialog(context) { createBackup ->
            if (needsPool) {
                GgtgProxyChooser.show(context) { source -> runImport(copy, source, createBackup) }
            } else {
                runImport(copy, null, createBackup)
            }
        }
    }

    private fun runImport(file: File, source: GgtgProxySource?, createBackup: Boolean) {
        val activity = parentActivity ?: return
        val progress = app.ggtg.ui.TdataImportProgressDialog(activity)
        progress.show()
        scope.launch {
            try {
                val result = withContext(Dispatchers.IO) { GgtgPackImporter.import(file, source, progress, createBackup) }
                progress.dismiss()
                AlertDialog.Builder(activity).setTitle("Импорт .ggtg")
                    .setMessage(GgtgImportSupport.buildResultMessage(result))
                    .setPositiveButton("Закрыть", null).show()
                if (result.imported.isNotEmpty()) finishFragment()
            } catch (t: Throwable) {
                progress.dismiss()
                toast("Ошибка импорта: ${t.message ?: t.javaClass.simpleName}")
            } finally {
                withContext(Dispatchers.IO) { file.delete() }
            }
        }
    }

    private fun toast(text: String) { context?.let { Toast.makeText(it, text, Toast.LENGTH_LONG).show() } }

    private companion object { const val PICK_REQUEST = 4713 }
}
