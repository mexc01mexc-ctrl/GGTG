package app.ggtg

import android.content.Context
import android.text.InputType
import android.view.Gravity
import android.view.View
import android.widget.EditText
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import org.telegram.messenger.AndroidUtilities
import org.telegram.messenger.R
import org.telegram.ui.ActionBar.ActionBar.ActionBarMenuOnItemClick
import org.telegram.ui.ActionBar.AlertDialog
import org.telegram.ui.ActionBar.BaseFragment
import org.telegram.ui.ActionBar.Theme
import org.telegram.ui.Cells.TextCell
import org.telegram.ui.Cells.TextDetailCell
import org.telegram.ui.Components.LayoutHelper
import org.telegram.ui.Components.GgtgProxyPoolStore

import org.telegram.ui.CountrySelectActivity

// ng: the "Прокси" screen - the named, consumable proxy pools (GgtgProxyPoolStore, reused by the
// .ggtg import flow to hand a distinct proxy to each imported account).
class GgtgProxyActivity : BaseFragment() {

    private var container: LinearLayout? = null

    override fun createView(context: Context): View {
        actionBar.setBackButtonImage(R.drawable.ic_ab_back)
        actionBar.setAllowOverlayTitle(true)
        actionBar.setTitle("Прокси")
        actionBar.setActionBarMenuOnItemClick(object : ActionBarMenuOnItemClick() {
            override fun onItemClick(id: Int) {
                when (id) {
                    -1 -> finishFragment()
                    1 -> showAddOptions()
                }
            }
        })
        actionBar.createMenu().addItem(1, R.drawable.msg_add)

        val frame = FrameLayout(context)
        frame.setBackgroundColor(Theme.getColor(Theme.key_windowBackgroundGray))
        fragmentView = frame
        val scroll = ScrollView(context)
        val list = LinearLayout(context)
        list.orientation = LinearLayout.VERTICAL
        scroll.addView(list, LayoutHelper.createScroll(LayoutHelper.MATCH_PARENT, LayoutHelper.WRAP_CONTENT, Gravity.TOP))
        frame.addView(scroll, LayoutHelper.createFrame(LayoutHelper.MATCH_PARENT, LayoutHelper.MATCH_PARENT.toFloat()))
        container = list
        rebuild()
        return frame
    }

    override fun onResume() { super.onResume(); rebuild() }

    private fun rebuild() {
        val list = container ?: return
        val context = list.context
        list.removeAllViews()

        val addCountryCard = GgtgUi.card(context) { selectCountryForList() }
        val addRow = LinearLayout(context).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
        addRow.addView(GgtgUi.badge(context, R.drawable.msg_language), LinearLayout.LayoutParams(GgtgUi.dp(40f), GgtgUi.dp(40f)).apply { rightMargin = GgtgUi.dp(12f) })
        val addTexts = LinearLayout(context).apply { orientation = LinearLayout.VERTICAL }
        addTexts.addView(GgtgUi.title(context, "Добавить список по стране…"))
        addTexts.addView(GgtgUi.body(context, "Выбор из всех стран Telegram для автоматического языка и времени"))
        addRow.addView(addTexts, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
        addCountryCard.addView(addRow)
        GgtgUi.addCard(list, addCountryCard, top = 10f)

        val names = GgtgProxyPoolStore.getListNames()
        if (names.isEmpty()) {
            list.addView(TextView(context).apply {
                text = "Списков прокси пока нет.\nНажмите карточку выше, чтобы выбрать страну и добавить список."
                gravity = Gravity.CENTER
                setTextColor(Theme.getColor(Theme.key_windowBackgroundWhiteGrayText))
                setPadding(AndroidUtilities.dp(24f), AndroidUtilities.dp(40f), AndroidUtilities.dp(24f), 0)
            })
            return
        }

        list.addView(GgtgUi.sectionLabel(context, "Списки прокси"))

        for (name in names) {
            val count = GgtgProxyPoolStore.getCount(name)
            val countryCode = GgtgProxyPoolStore.getCountry(name)
            val geo = if (countryCode.isNotEmpty()) {
                GgtgGeo.matchForCountry(countryCode)
            } else {
                GgtgGeo.matchForPhone(name)
            }

            val card = GgtgUi.card(context) { presentFragment(GgtgProxyListActivity(name)) }
            val row = LinearLayout(context).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
            row.addView(GgtgUi.badge(context, R.drawable.msg_language), LinearLayout.LayoutParams(GgtgUi.dp(40f), GgtgUi.dp(40f)).apply { rightMargin = GgtgUi.dp(12f) })
            val texts = LinearLayout(context).apply { orientation = LinearLayout.VERTICAL }
            texts.addView(GgtgUi.title(context, name.ifEmpty { "(без названия)" }))
            val subtext = buildString {
                if (count == 0) append("Пусто — добавьте прокси")
                else append("Прокси в списке: $count")
                if (geo.country.isNotEmpty()) {
                    append(" • ${geo.country} (${geo.systemLang}, UTC${GgtgDevice.formatOffset(geo.tzOffset)})")
                }
            }
            texts.addView(GgtgUi.body(context, subtext))
            row.addView(texts, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
            row.addView(GgtgUi.pill(context, count.toString(), 0xFFFFFFFF.toInt(),
                if (count == 0) Theme.getColor(Theme.key_windowBackgroundWhiteGrayText) else GgtgUi.accent))
            card.addView(row)
            GgtgUi.addCard(list, card)
        }
        list.addView(View(context), LinearLayout.LayoutParams(1, GgtgUi.dp(16f)))
    }

    private fun showAddOptions() {
        val context = context ?: return
        AlertDialog.Builder(context)
            .setTitle("Новый список прокси")
            .setItems(arrayOf("Выбрать страну Telegram…", "Ввести своё название…")) { _, which ->
                if (which == 0) {
                    selectCountryForList()
                } else {
                    showCustomNameDialog()
                }
            }
            .setNegativeButton("Отмена", null)
            .show()
    }

    private fun selectCountryForList() {
        val act = CountrySelectActivity(true)
        act.setGgtgPinning(true)
        act.setCountrySelectActivityDelegate { country ->
            val listName = "${country.name} (+${country.code})"
            GgtgProxyPoolStore.createList(listName, country.shortname)
            presentFragment(GgtgProxyListActivity(listName))
        }
        presentFragment(act)
    }

    private fun showCustomNameDialog() {
        val context = context ?: return
        val field = EditText(context).apply {
            hint = "Название (например, США)"; setSingleLine()
            setPadding(AndroidUtilities.dp(24f), AndroidUtilities.dp(8f), AndroidUtilities.dp(24f), AndroidUtilities.dp(8f))
        }
        AlertDialog.Builder(context).setTitle("Создать список").setView(field)
            .setPositiveButton("Создать") { _, _ ->
                val name = field.text.toString().trim()
                if (name.isEmpty()) return@setPositiveButton
                GgtgProxyPoolStore.createList(name)
                presentFragment(GgtgProxyListActivity(name))
            }
            .setNegativeButton("Отмена", null)
            .show()
    }
}
