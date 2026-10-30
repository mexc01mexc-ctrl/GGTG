package app.ggtg

import android.content.Context
import android.view.Gravity
import android.view.View
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import android.widget.Toast
import org.telegram.messenger.AndroidUtilities
import org.telegram.messenger.R
import org.telegram.ui.ActionBar.ActionBar.ActionBarMenuOnItemClick
import org.telegram.ui.ActionBar.AlertDialog
import org.telegram.ui.ActionBar.BaseFragment
import org.telegram.ui.ActionBar.Theme
import org.telegram.ui.Cells.TextCell
import org.telegram.ui.Components.GgtgProxyChooser
import org.telegram.ui.Components.LayoutHelper

// ng: "Импорт tdata" - a real second Android login duplicated from a Telegram Desktop session,
// either a picked tdata folder or a zip/rar archive holding several. Always asks for a proxy first
// (GgtgProxyChooser: a named pool from "Прокси", or one typed in by hand) since duplicating a
// Desktop session must never touch the network unprotected - see TdataImportHelper.
class GgtgTdataImportActivity : BaseFragment() {

    override fun createView(context: Context): View {
        actionBar.setBackButtonImage(R.drawable.ic_ab_back)
        actionBar.setAllowOverlayTitle(true)
        actionBar.setTitle("Импорт tdata")
        actionBar.setActionBarMenuOnItemClick(object : ActionBarMenuOnItemClick() {
            override fun onItemClick(id: Int) { if (id == -1) finishFragment() }
        })

        val frame = FrameLayout(context)
        frame.setBackgroundColor(Theme.getColor(Theme.key_windowBackgroundGray))
        fragmentView = frame
        val root = GgtgUi.page(context, frame)

        GgtgUi.addCard(root, GgtgUi.infoCard(context, org.telegram.messenger.R.drawable.msg_folders, "Что такое tdata",
            "Папка tdata — это сессия Telegram Desktop (Windows: AppData\\Roaming\\Telegram Desktop\\tdata). Отсюда она дублируется в новый, независимый вход на этом телефоне — сама сессия Desktop не трогается и не выходит из аккаунта. Прокси выбирается перед импортом."), top = 10f)
        GgtgUi.addCard(root, GgtgUi.actionCard(context, org.telegram.messenger.R.drawable.msg_openin, "Импорт папки tdata", "Выбрать папку tdata на телефоне") { pickAndImport(folder = true) })
        GgtgUi.addCard(root, GgtgUi.actionCard(context, org.telegram.messenger.R.drawable.msg_archive, "Импорт из архива", "zip / rar / .ggtg — можно сразу несколько аккаунтов") { pickAndImport(folder = false) })
        return frame
    }

    private fun pickAndImport(folder: Boolean) {
        val context = context ?: return
        val activity = parentActivity as? androidx.fragment.app.FragmentActivity ?: return
        GgtgImportSupport.askCreateBackupDialog(context) { createBackup ->
            if (folder) {
                // a tdata folder never contains a self-sufficient .ggtg pack, so a proxy source is
                // always required up front
                GgtgProxyChooser.show(context) { source ->
                    TdataImportHelper.pickFolderAndImport(activity, source, createBackup) { result, error -> onDone(result, error) }
                }
            } else {
                // an archive might turn out to be a .ggtg pack whose accounts already carry their own
                // proxy - pickArchiveSmart only prompts if the picked file actually needs one
                TdataImportHelper.pickArchiveSmart(activity, { chosen ->
                    GgtgProxyChooser.show(context) { source -> chosen(source) }
                }, createBackup) { result, error -> onDone(result, error) }
            }
        }
    }

    private fun onDone(result: GgtgImportSupport.ImportResult?, error: Throwable?) {
        val context = context ?: return
        if (result == null) {
            toast("Ошибка импорта: ${error?.message ?: error?.javaClass?.simpleName ?: "?"}")
            return
        }
        AlertDialog.Builder(context).setTitle("Импорт tdata")
            .setMessage(GgtgImportSupport.buildResultMessage(result))
            .setPositiveButton("Закрыть", null).show()
        if (result.imported.isNotEmpty()) finishFragment()
    }

    private fun toast(text: String) { context?.let { Toast.makeText(it, text, Toast.LENGTH_LONG).show() } }
}
