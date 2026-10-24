package app.ggtg

import android.content.Context
import android.content.Intent
import android.net.Uri
import android.widget.LinearLayout
import android.widget.TextView
import android.widget.Toast
import org.json.JSONObject
import org.telegram.ui.ActionBar.AlertDialog
import java.io.File

// ng: the user's own api_id / api_hash (from my.telegram.org), used only when a new session is made for another
// device (auth.exportLoginToken): the shared public api_id is sometimes refused with API_ID_PUBLISHED_FLOOD.
// The same as core/ggtg_api_override.h on the desktop.
object GgtgApiOverride {

    data class Value(val apiId: Int, val apiHash: String) {
        val valid get() = apiId > 0 && apiHash.length == 32
    }

    private fun file() = File(GgtgDb.dir(), "api_override.json")

    fun load(): Value = try {
        val f = file()
        if (f.exists()) {
            val o = JSONObject(f.readText())
            Value(o.optString("apiId").toIntOrNull() ?: 0, o.optString("apiHash").trim())
        } else Value(0, "")
    } catch (t: Throwable) { Value(0, "") }

    fun save(value: Value) {
        file().writeText(JSONObject().put("apiId", value.apiId.toString()).put("apiHash", value.apiHash).toString())
    }

    /** The id and hash a login token is requested with: the user's own when they made one, else Telegram Desktop's.
     *  Not this app's own (Telegram Android's 4): Telegram refuses auth.exportLoginToken with it outright
     *  (API_ID_PUBLISHED_FLOOD, seen 2026-10-14 even from this genuine Android client) - the official Android app
     *  never signs in by a QR code itself, Telegram Desktop does, and its id still gets through (the desktop's
     *  duplication works with it). The new session is then recorded as Telegram Desktop's. */
    fun id(): Int = load().takeIf { it.valid }?.apiId ?: DESKTOP_API_ID
    fun hash(): String = load().takeIf { it.valid }?.apiHash ?: DESKTOP_API_HASH

    private const val DESKTOP_API_ID = 17349
    private const val DESKTOP_API_HASH = "344583e45741c457fe1862106095a5eb"

    fun isFlood(text: String?) = text?.contains("API_ID_PUBLISHED_FLOOD") == true || text?.contains("API_ID_INVALID") == true

    private const val HELP = "Telegram иногда отказывает в новой сессии с общим api_id (ошибка API_ID_PUBLISHED_FLOOD): им пользуются многие программы. " +
        "Свой api_id принадлежит только вам.\n\nКак получить:\n" +
        "1. Откройте my.telegram.org и войдите по номеру телефона (код придёт в Telegram).\n" +
        "2. Выберите «API development tools».\n" +
        "3. Заполните форму: App title - любое имя, Short name - латиницей (5-32 символа), платформа - Android. Остальное можно пустым. Нажмите «Create application».\n" +
        "4. Скопируйте «App api_id» (число) и «App api_hash» (32 символа) в поля ниже.\n\n" +
        "Не показывайте api_hash другим."

    fun showDialog(context: Context) {
        val saved = load()
        val column = LinearLayout(context).apply { orientation = LinearLayout.VERTICAL; setPadding(GgtgUi.dp(20f), 0, GgtgUi.dp(20f), 0) }
        val help = GgtgUi.body(context, HELP)
        val scroll = android.widget.ScrollView(context).apply { addView(LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            addView(help)
        }) }
        column.addView(scroll, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, GgtgUi.dp(190f)))
        val id = GgtgUi.input(context, "api_id (число)", if (saved.apiId > 0) saved.apiId.toString() else "", numeric = true)
        val hash = GgtgUi.input(context, "api_hash (32 символа)", saved.apiHash)
        column.addView(id, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply { topMargin = GgtgUi.dp(8f) })
        column.addView(hash, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply { topMargin = GgtgUi.dp(8f) })
        val dialog = AlertDialog.Builder(context).setTitle("Свой api_id").setView(column)
            .setPositiveButton("Сохранить", null)
            .setNeutralButton("Открыть сайт") { _, _ ->
                context.startActivity(Intent(Intent.ACTION_VIEW, Uri.parse("https://my.telegram.org/apps")))
            }
            .setNegativeButton("Закрыть", null).create()
        dialog.setOnShowListener {
            dialog.getButton(AlertDialog.BUTTON_POSITIVE)?.setOnClickListener {
                val idText = id.text.toString().trim()
                val hashText = hash.text.toString().trim()
                if (idText.isEmpty() && hashText.isEmpty()) {
                    save(Value(0, ""))
                    Toast.makeText(context, "Свой api_id убран", Toast.LENGTH_SHORT).show()
                    dialog.dismiss()
                } else {
                    val value = Value(idText.toIntOrNull() ?: 0, hashText)
                    if (!value.valid) {
                        Toast.makeText(context, "Проверьте: api_id - число, api_hash - 32 символа", Toast.LENGTH_LONG).show()
                    } else {
                        save(value)
                        Toast.makeText(context, "Сохранено. Повторите экспорт.", Toast.LENGTH_LONG).show()
                        dialog.dismiss()
                    }
                }
            }
        }
        dialog.show()
    }

    /** A result dialog that, for the flood error, also offers to set the user's own api_id. */
    fun inform(context: Context, title: String, text: String) {
        val builder = AlertDialog.Builder(context).setTitle(title).setMessage(text).setPositiveButton("Закрыть", null)
        if (isFlood(text)) builder.setNeutralButton("Свой api_id…") { _, _ -> showDialog(context) }
        builder.show()
    }
}
