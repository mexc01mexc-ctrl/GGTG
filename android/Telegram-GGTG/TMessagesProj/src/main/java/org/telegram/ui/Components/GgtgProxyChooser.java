package org.telegram.ui.Components;

import static org.telegram.messenger.AndroidUtilities.dp;

import android.app.AlertDialog;
import android.content.Context;
import android.text.InputType;
import android.util.TypedValue;
import android.view.Gravity;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.Toast;

import org.telegram.messenger.LocaleController;
import org.telegram.messenger.R;

import java.util.List;
import java.util.function.Consumer;

import app.ggtg.GgtgProxySource;

// ng: the mandatory "where do the proxies for this new account (or these new accounts) come from"
// prompt shown the moment an add-account flow starts. No "skip" option by design - matches the
// existing hard "never connect without a proxy" rule elsewhere in this app.
public class GgtgProxyChooser {

    public static void show(Context context, Consumer<GgtgProxySource> onChosen) {
        List<String> names = GgtgProxyPoolStore.getListNames();
        java.util.List<String> usable = new java.util.ArrayList<>();
        for (String name : names) {
            if (GgtgProxyPoolStore.getCount(name) > 0) usable.add(name);
        }

        CharSequence[] items = new CharSequence[usable.size() + 2];
        for (int i = 0; i < usable.size(); i++) {
            items[i] = LocaleController.formatString("NicegramProxyListRow", R.string.NicegramProxyListRow, usable.get(i), GgtgProxyPoolStore.getCount(usable.get(i)));
        }
        items[usable.size()] = LocaleController.getString(R.string.NicegramCreateProxyList);
        items[usable.size() + 1] = LocaleController.getString(R.string.NicegramEnterProxyManually);

        new AlertDialog.Builder(context)
                .setTitle(LocaleController.getString(R.string.NicegramChooseProxySource))
                .setItems(items, (dialog, which) -> {
                    if (which < usable.size()) {
                        onChosen.accept(new GgtgProxySource.NamedList(usable.get(which)));
                    } else if (which == usable.size()) {
                        promptCreateList(context, onChosen);
                    } else {
                        promptManualProxy(context, onChosen);
                    }
                })
                .setNegativeButton(LocaleController.getString(R.string.Cancel), null)
                .show();
    }

    private static void promptCreateList(Context context, Consumer<GgtgProxySource> onChosen) {
        LinearLayout layout = new LinearLayout(context);
        layout.setOrientation(LinearLayout.VERTICAL);

        EditText nameInput = new EditText(context);
        nameInput.setSingleLine(true);
        nameInput.setHint(LocaleController.getString(R.string.NicegramProxyListNamePlaceholder));
        nameInput.setPadding(dp(24), dp(8), dp(24), dp(8));
        layout.addView(nameInput, LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);

        EditText pasteInput = new EditText(context);
        pasteInput.setSingleLine(false);
        pasteInput.setMinLines(4);
        pasteInput.setGravity(Gravity.TOP);
        pasteInput.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_MULTI_LINE);
        pasteInput.setHint(LocaleController.getString(R.string.NicegramProxyListPastePlaceholder));
        pasteInput.setTextSize(TypedValue.COMPLEX_UNIT_DIP, 14);
        pasteInput.setPadding(dp(24), dp(8), dp(24), dp(8));
        layout.addView(pasteInput, LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);

        new AlertDialog.Builder(context)
                .setTitle(LocaleController.getString(R.string.NicegramCreateProxyList))
                .setView(layout)
                .setPositiveButton(LocaleController.getString(R.string.Done), (dialog, which) -> {
                    String name = nameInput.getText().toString().trim();
                    String pasted = pasteInput.getText().toString();
                    if (name.isEmpty()) return;
                    int before = GgtgProxyPoolStore.getCount(name);
                    GgtgProxyPoolStore.addProxiesToList(name, pasted);
                    if (GgtgProxyPoolStore.getCount(name) == before) {
                        Toast.makeText(context, LocaleController.getString(R.string.NicegramProxyListNoneParsed), Toast.LENGTH_LONG).show();
                        return;
                    }
                    onChosen.accept(new GgtgProxySource.NamedList(name));
                })
                .setNegativeButton(LocaleController.getString(R.string.Cancel), null)
                .show();
    }

    private static void promptManualProxy(Context context, Consumer<GgtgProxySource> onChosen) {
        EditText input = new EditText(context);
        input.setSingleLine(true);
        input.setHint(LocaleController.getString(R.string.NicegramProxyManualPlaceholder));
        input.setPadding(dp(24), dp(8), dp(24), dp(8));

        new AlertDialog.Builder(context)
                .setTitle(LocaleController.getString(R.string.NicegramEnterProxyManually))
                .setView(input)
                .setPositiveButton(LocaleController.getString(R.string.Done), (dialog, which) -> {
                    GgtgProxyPoolStore.Entry entry = GgtgProxyPoolStore.Entry.parse(input.getText().toString());
                    if (entry == null) {
                        Toast.makeText(context, LocaleController.getString(R.string.NicegramProxyInvalidFormat), Toast.LENGTH_LONG).show();
                        return;
                    }
                    onChosen.accept(new GgtgProxySource.Manual(entry));
                })
                .setNegativeButton(LocaleController.getString(R.string.Cancel), null)
                .show();
    }
}
