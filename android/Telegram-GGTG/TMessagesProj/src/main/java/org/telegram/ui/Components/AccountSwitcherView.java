package org.telegram.ui.Components;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.text.TextUtils;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.TextView;

import androidx.annotation.NonNull;
import androidx.recyclerview.widget.ItemTouchHelper;
import androidx.recyclerview.widget.RecyclerView;

import org.telegram.messenger.AndroidUtilities;
import org.telegram.messenger.LocaleController;
import org.telegram.messenger.MessagesStorage;
import org.telegram.messenger.R;
import org.telegram.messenger.UserConfig;
import org.telegram.messenger.UserObject;
import org.telegram.tgnet.ConnectionsManager;
import org.telegram.tgnet.TLRPC;
import org.telegram.ui.ActionBar.Theme;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Map;

import static org.telegram.messenger.AndroidUtilities.dp;

// ng: draggable, pinnable, folder-groupable account switcher list - mirrors NiceGram Desktop's
// folder/account pin + drag-reorder feature. Folders are a lightweight concept: an account just
// carries the name of the folder it belongs to (UserConfig.ncgFolder); a folder "exists" exactly
// when at least one account is assigned to it. Folder-level state that isn't owned by any one
// account (pinned, expanded, manual order) lives in AccountFolderStore instead.
//
// Gestures: swipe right = pin/unpin (account or folder); swipe left on a folder = delete it
// (members become ungrouped, never deleted); tap a folder header = expand/collapse (accordion -
// opening one collapses the rest); long-press an account = move it to a folder; long-press a
// folder header = rename or delete it; the small handle icon on the right drags to reorder.
public class AccountSwitcherView extends FrameLayout {

    public interface OnAccountSelected {
        void onAccountSelected(int account);
    }

    private static final int ROW_HEIGHT_DP = 48;
    private static final int MAX_VISIBLE_ROWS = 7;
    private static final int VIEW_TYPE_ACCOUNT = 0;
    private static final int VIEW_TYPE_FOLDER = 1;
    private static final int INDENT_DP = 22;

    private static abstract class Item {
    }

    private static class AccountItem extends Item {
        int account;
        boolean indented;
        String folder; // null if ungrouped
    }

    private static class FolderItem extends Item {
        String name;
        boolean pinned;
        boolean expanded;
        int unreadCount;
    }

    private final RecyclerListView listView;
    private final Adapter adapter;
    private final List<Integer> accounts;
    private final List<Item> items = new ArrayList<>();

    public AccountSwitcherView(Context context, List<Integer> accountNumbers, int currentAccount, OnAccountSelected listener) {
        super(context);

        accounts = new ArrayList<>(accountNumbers);
        rebuildItems();

        adapter = new Adapter(currentAccount);

        listView = new RecyclerListView(context);
        listView.setLayoutManager(new androidx.recyclerview.widget.LinearLayoutManager(context));
        listView.setAdapter(adapter);
        // RecyclerListView owns touch handling for its rows (also needed for the drag helper below
        // to coexist) - a plain View.OnClickListener set on the row inside onBindViewHolder never
        // actually fires clicks through it, so selection must go through its own listener API instead.
        listView.setOnItemClickListener((view, position) -> {
            if (position < 0 || position >= items.size()) return;
            Item item = items.get(position);
            if (item instanceof AccountItem) {
                if (listener != null) listener.onAccountSelected(((AccountItem) item).account);
            } else {
                toggleFolder(((FolderItem) item).name);
            }
        });
        listView.setOnItemLongClickListener((view, position) -> {
            if (position < 0 || position >= items.size()) return false;
            Item item = items.get(position);
            if (item instanceof AccountItem) {
                showFolderMenu(context, ((AccountItem) item).account);
            } else {
                showFolderHeaderMenu(context, ((FolderItem) item).name);
            }
            return true;
        });

        ItemTouchHelper touchHelper = new ItemTouchHelper(new TouchCallback());
        touchHelper.attachToRecyclerView(listView);
        adapter.touchHelper = touchHelper;

        addView(listView, LayoutHelper.createFrame(LayoutHelper.MATCH_PARENT, ROW_HEIGHT_DP * Math.min(items.size(), MAX_VISIBLE_ROWS)));
    }

    /** ng: the popup sizes itself once at construction time (its containing ItemOptions measures
     * this view's own height) - pin/folder-create/expand-collapse all change how many rows there
     * are afterward, so the container needs to be told to re-measure each time or extra rows just
     * render into space that was never allocated. */
    private void updateHeight() {
        ViewGroup.LayoutParams params = listView.getLayoutParams();
        params.height = dp(ROW_HEIGHT_DP) * Math.min(items.size(), MAX_VISIBLE_ROWS);
        listView.setLayoutParams(params);
        requestLayout();
    }

    // ---- item model ------------------------------------------------------------------------

    /** ng: rebuilds the flat, ordered display list from accounts + folder metadata. Pinned
     * top-level entities (ungrouped accounts and folders alike) sort first, then unpinned ones,
     * each bucket by its own manual order (falling back to login time for never-reordered
     * accounts). Folder members sort the same way, scoped to just that folder. */
    private void rebuildItems() {
        items.clear();

        Map<String, List<Integer>> byFolder = AccountFolderLogic.groupByFolder(accounts);
        List<Object> topLevel = AccountFolderLogic.buildTopLevelOrder(accounts);

        for (Object entity : topLevel) {
            if (entity instanceof Integer) {
                AccountItem item = new AccountItem();
                item.account = (Integer) entity;
                items.add(item);
            } else {
                String folder = (String) entity;
                FolderItem header = new FolderItem();
                header.name = folder;
                header.pinned = AccountFolderStore.isPinned(folder);
                header.expanded = AccountFolderStore.isExpanded(folder);
                header.unreadCount = AccountFolderLogic.folderUnreadCount(byFolder.get(folder));
                items.add(header);
                if (header.expanded) {
                    for (int acc : AccountFolderLogic.sortFolderMembers(byFolder.get(folder))) {
                        AccountItem item = new AccountItem();
                        item.account = acc;
                        item.indented = true;
                        item.folder = folder;
                        items.add(item);
                    }
                }
            }
        }
    }

    /** ng: re-derives manual order values from however the list currently reads, after a drag. */
    private void persistOrderFromItems() {
        int topOrder = 0;
        int folderOrder = 0;
        for (Item it : items) {
            if (it instanceof FolderItem) {
                AccountFolderStore.setOrder(((FolderItem) it).name, topOrder++);
                folderOrder = 0;
            } else {
                AccountItem ai = (AccountItem) it;
                UserConfig uc = UserConfig.getInstance(ai.account);
                uc.ncgOrder = ai.indented ? folderOrder++ : topOrder++;
                uc.saveConfig(false);
            }
        }
    }

    private void refresh() {
        rebuildItems();
        adapter.notifyDataSetChanged();
        updateHeight();
    }

    private void toggleFolder(String name) {
        AccountFolderLogic.toggleFolder(name, this::refresh);
    }

    private void deleteFolder(String name) {
        AccountFolderLogic.deleteFolder(name, this::refresh);
    }

    private void showFolderMenu(Context context, int account) {
        app.ggtg.GgtgAccountMenus.showAccountMenu(context, account, null, () -> { refresh(); return kotlin.Unit.INSTANCE; });
    }

    private void showFolderHeaderMenu(Context context, String folder) {
        app.ggtg.GgtgAccountMenus.showFolderMenu(context, folder, () -> { refresh(); return kotlin.Unit.INSTANCE; });
    }

    // ---- rows ------------------------------------------------------------------------------

    private class AccountRow extends LinearLayout {
        final FrameLayout avatarContainer;
        final BackupImageView avatarView;
        final TextView textView;
        final TextView unreadCountView;
        final ImageView pinView;
        final ImageView handleView;
        boolean selected;
        boolean noProxy;
        boolean isOffline;

        AccountRow(Context context) {
            super(context);
            setOrientation(HORIZONTAL);
            setBackground(Theme.createRadSelectorDrawable(Theme.getColor(Theme.key_listSelector), 0, 0));

            avatarContainer = new FrameLayout(context) {
                private final Paint selectedPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
                private final Paint noProxyPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
                private final Paint noProxyBorderPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
                @Override
                protected void dispatchDraw(@NonNull Canvas canvas) {
                    if (selected) {
                        selectedPaint.setStyle(Paint.Style.STROKE);
                        selectedPaint.setStrokeWidth(dp(1.33f));
                        selectedPaint.setColor(Theme.getColor(Theme.key_featuredStickers_addButton));
                        canvas.drawCircle(getWidth() / 2.0f, getHeight() / 2.0f, dp(16), selectedPaint);
                    }
                    super.dispatchDraw(canvas);
                    // ng: proxy protection tripped (red) vs genuinely connected (green) - matches
                    // the desktop app's per-account connection indicator
                    float cx = getWidth() - dp(3);
                    float cy = getHeight() - dp(3);
                    noProxyBorderPaint.setColor(Theme.getColor(Theme.key_windowBackgroundWhite));
                    canvas.drawCircle(cx, cy, dp(5.5f), noProxyBorderPaint);
                    noProxyPaint.setColor(isOffline ? 0xFF9E9E9E : (noProxy ? 0xFFF44336 : 0xFF4CAF50));
                    canvas.drawCircle(cx, cy, dp(4), noProxyPaint);
                }
            };
            addView(avatarContainer, LayoutHelper.createLinear(34, 34, Gravity.CENTER_VERTICAL, 12, 0, 0, 0));

            avatarView = new BackupImageView(context);
            avatarView.setRoundRadius(dp(16));
            avatarContainer.addView(avatarView, LayoutHelper.createLinear(32, 32, Gravity.CENTER, 1, 1, 1, 1));

            textView = new TextView(context);
            textView.setTextSize(TypedValue.COMPLEX_UNIT_DIP, 16);
            textView.setTextColor(Theme.getColor(Theme.key_dialogTextBlack));
            textView.setMaxLines(2);
            textView.setEllipsize(TextUtils.TruncateAt.END);
            addView(textView, LayoutHelper.createLinear(0, LayoutHelper.WRAP_CONTENT, 1f, Gravity.CENTER_VERTICAL, 13, 0, 4, 0));

            unreadCountView = new TextView(context);
            unreadCountView.setTextSize(TypedValue.COMPLEX_UNIT_DIP, 12);
            unreadCountView.setTextColor(Theme.getColor(Theme.key_chats_unreadCounterText));
            unreadCountView.setTypeface(AndroidUtilities.bold());
            unreadCountView.setGravity(Gravity.CENTER);
            unreadCountView.setMinWidth(dp(18));
            unreadCountView.setPadding(dp(5), 0, dp(5), 0);
            android.graphics.drawable.GradientDrawable badgeBg = new android.graphics.drawable.GradientDrawable();
            badgeBg.setShape(android.graphics.drawable.GradientDrawable.RECTANGLE);
            badgeBg.setCornerRadius(dp(9));
            badgeBg.setColor(Theme.getColor(Theme.key_chats_unreadCounter));
            unreadCountView.setBackground(badgeBg);
            addView(unreadCountView, LayoutHelper.createLinear(LayoutHelper.WRAP_CONTENT, 18, Gravity.CENTER_VERTICAL, 0, 0, 8, 0));

            pinView = new ImageView(context);
            pinView.setScaleType(ImageView.ScaleType.CENTER);
            pinView.setImageResource(R.drawable.msg_pin_filled);
            pinView.setColorFilter(Theme.getColor(Theme.key_featuredStickers_addButton));
            addView(pinView, LayoutHelper.createLinear(28, 40, Gravity.CENTER_VERTICAL, 0, 0, 0, 0));

            handleView = new ImageView(context);
            handleView.setScaleType(ImageView.ScaleType.CENTER);
            handleView.setImageResource(R.drawable.list_reorder);
            handleView.setColorFilter(Theme.getColor(Theme.key_stickers_menuSelector));
            addView(handleView, LayoutHelper.createLinear(32, 40, Gravity.CENTER_VERTICAL, 0, 0, 4, 0));
        }

        void bind(int account, boolean isSelected, boolean indented) {
            selected = isSelected;
            noProxy = !ConnectionsManager.ncgAccountHasProxy(account);
            UserConfig uc = UserConfig.getInstance(account);
            isOffline = uc.ncgOffline;
            TLRPC.User user = uc.getCurrentUser();

            ((LinearLayout.LayoutParams) avatarContainer.getLayoutParams()).leftMargin = dp(indented ? 12 + INDENT_DP : 12);

            AvatarDrawable avatarDrawable = new AvatarDrawable();
            avatarDrawable.setInfo(user);
            avatarView.setScaleX(selected ? 0.833f : 1f);
            avatarView.setScaleY(selected ? 0.833f : 1f);
            avatarView.getImageReceiver().setCurrentAccount(account);
            avatarView.setForUserOrChat(user, avatarDrawable);
            avatarContainer.invalidate();

            int unreadCount = MessagesStorage.getInstance(account).getMainUnreadCount();
            if (unreadCount > 0) {
                unreadCountView.setVisibility(VISIBLE);
                unreadCountView.setText(LocaleController.formatNumber(unreadCount, ','));
            } else {
                unreadCountView.setVisibility(GONE);
            }

            textView.setText(UserObject.getUserName(user));

            // ng: pin icon only shows once actually pinned - pinning itself happens via swipe-right
            pinView.setVisibility(uc.ncgPinned ? VISIBLE : GONE);
        }
    }

    private class FolderRow extends LinearLayout {
        final FrameLayout avatarContainer;
        final TextView avatarLetters;
        final ImageView chevron;
        final TextView textView;
        final TextView unreadCountView;
        final ImageView pinView;
        final ImageView handleView;
        private final Paint activeBorderPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private boolean containsCurrentAccount;

        @Override
        protected void dispatchDraw(@NonNull Canvas canvas) {
            super.dispatchDraw(canvas);
            if (containsCurrentAccount) {
                activeBorderPaint.setStyle(Paint.Style.STROKE);
                activeBorderPaint.setStrokeWidth(dp(1.5f));
                activeBorderPaint.setColor(Theme.getColor(Theme.key_featuredStickers_addButton));
                android.graphics.RectF rect = new android.graphics.RectF(dp(4), dp(2), getWidth() - dp(4), getHeight() - dp(2));
                canvas.drawRoundRect(rect, dp(8), dp(8), activeBorderPaint);
            }
        }

        FolderRow(Context context) {
            super(context);
            setOrientation(HORIZONTAL);
            setBackground(Theme.createRadSelectorDrawable(Theme.getColor(Theme.key_listSelector), 0, 0));

            avatarContainer = new FrameLayout(context);
            addView(avatarContainer, LayoutHelper.createLinear(34, 34, Gravity.CENTER_VERTICAL, 12, 0, 0, 0));

            avatarLetters = new TextView(context);
            avatarLetters.setGravity(Gravity.CENTER);
            avatarLetters.setTextColor(0xFFFFFFFF);
            avatarLetters.setTypeface(AndroidUtilities.bold());
            avatarLetters.setTextSize(TypedValue.COMPLEX_UNIT_DIP, 13);
            android.graphics.drawable.GradientDrawable bg = new android.graphics.drawable.GradientDrawable();
            bg.setShape(android.graphics.drawable.GradientDrawable.OVAL);
            bg.setColor(Theme.getColor(Theme.key_avatar_backgroundBlue));
            avatarLetters.setBackground(bg);
            avatarContainer.addView(avatarLetters, LayoutHelper.createFrame(32, 32, Gravity.CENTER));

            chevron = new ImageView(context);
            chevron.setScaleType(ImageView.ScaleType.CENTER);
            chevron.setImageResource(R.drawable.arrow_more);
            chevron.setColorFilter(Theme.getColor(Theme.key_stickers_menuSelector));
            addView(chevron, LayoutHelper.createLinear(24, 40, Gravity.CENTER_VERTICAL, 4, 0, 0, 0));

            textView = new TextView(context);
            textView.setTextSize(TypedValue.COMPLEX_UNIT_DIP, 16);
            textView.setTypeface(AndroidUtilities.bold());
            textView.setTextColor(Theme.getColor(Theme.key_dialogTextBlack));
            textView.setMaxLines(1);
            textView.setEllipsize(TextUtils.TruncateAt.END);
            addView(textView, LayoutHelper.createLinear(0, LayoutHelper.WRAP_CONTENT, 1f, Gravity.CENTER_VERTICAL, 8, 0, 4, 0));

            unreadCountView = new TextView(context);
            unreadCountView.setTextSize(TypedValue.COMPLEX_UNIT_DIP, 12);
            unreadCountView.setTextColor(Theme.getColor(Theme.key_chats_unreadCounterText));
            unreadCountView.setTypeface(AndroidUtilities.bold());
            unreadCountView.setGravity(Gravity.CENTER);
            unreadCountView.setMinWidth(dp(18));
            unreadCountView.setPadding(dp(5), 0, dp(5), 0);
            android.graphics.drawable.GradientDrawable folderBadgeBg = new android.graphics.drawable.GradientDrawable();
            folderBadgeBg.setShape(android.graphics.drawable.GradientDrawable.RECTANGLE);
            folderBadgeBg.setCornerRadius(dp(9));
            folderBadgeBg.setColor(Theme.getColor(Theme.key_chats_unreadCounter));
            unreadCountView.setBackground(folderBadgeBg);
            addView(unreadCountView, LayoutHelper.createLinear(LayoutHelper.WRAP_CONTENT, 18, Gravity.CENTER_VERTICAL, 0, 0, 8, 0));

            pinView = new ImageView(context);
            pinView.setScaleType(ImageView.ScaleType.CENTER);
            pinView.setImageResource(R.drawable.msg_pin_filled);
            pinView.setColorFilter(Theme.getColor(Theme.key_featuredStickers_addButton));
            addView(pinView, LayoutHelper.createLinear(28, 40, Gravity.CENTER_VERTICAL, 0, 0, 0, 0));

            handleView = new ImageView(context);
            handleView.setScaleType(ImageView.ScaleType.CENTER);
            handleView.setImageResource(R.drawable.list_reorder);
            handleView.setColorFilter(Theme.getColor(Theme.key_stickers_menuSelector));
            addView(handleView, LayoutHelper.createLinear(32, 40, Gravity.CENTER_VERTICAL, 0, 0, 4, 0));
        }

        void bind(FolderItem item) {
            String letters = item.name.length() >= 2 ? item.name.substring(0, 2) : item.name.substring(0, 1);
            avatarLetters.setText(letters.toUpperCase());
            textView.setText(item.name);
            chevron.setRotation(item.expanded ? 90f : 0f);
            if (item.unreadCount > 0) {
                unreadCountView.setVisibility(VISIBLE);
                unreadCountView.setText(LocaleController.formatNumber(item.unreadCount, ','));
            } else {
                unreadCountView.setVisibility(GONE);
            }
            pinView.setVisibility(item.pinned ? VISIBLE : GONE);
            String activeFolder = UserConfig.getInstance(UserConfig.selectedAccount).ncgFolder;
            containsCurrentAccount = !android.text.TextUtils.isEmpty(activeFolder) && activeFolder.equals(item.name);
            invalidate();
        }
    }

    private class AccountHolder extends RecyclerView.ViewHolder {
        AccountHolder(AccountRow row) {
            super(row);
        }
    }

    private class FolderHolder extends RecyclerView.ViewHolder {
        FolderHolder(FolderRow row) {
            super(row);
        }
    }

    private class Adapter extends RecyclerView.Adapter<RecyclerView.ViewHolder> {
        final int currentAccount;
        ItemTouchHelper touchHelper;

        Adapter(int currentAccount) {
            this.currentAccount = currentAccount;
        }

        @NonNull
        @Override
        public RecyclerView.ViewHolder onCreateViewHolder(@NonNull ViewGroup parent, int viewType) {
            if (viewType == VIEW_TYPE_FOLDER) {
                FolderRow row = new FolderRow(parent.getContext());
                row.setLayoutParams(new RecyclerView.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(ROW_HEIGHT_DP)));
                return new FolderHolder(row);
            }
            AccountRow row = new AccountRow(parent.getContext());
            row.setLayoutParams(new RecyclerView.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(ROW_HEIGHT_DP)));
            return new AccountHolder(row);
        }

        @SuppressWarnings("ClickableViewAccessibility")
        @Override
        public void onBindViewHolder(@NonNull RecyclerView.ViewHolder holder, int position) {
            Item item = items.get(position);
            if (item instanceof FolderItem) {
                FolderRow row = (FolderRow) holder.itemView;
                row.bind((FolderItem) item);
                row.handleView.setOnTouchListener((v, event) -> {
                    if (event.getActionMasked() == MotionEvent.ACTION_DOWN && touchHelper != null) {
                        touchHelper.startDrag(holder);
                    }
                    return false;
                });
            } else {
                AccountItem ai = (AccountItem) item;
                AccountRow row = (AccountRow) holder.itemView;
                row.bind(ai.account, ai.account == currentAccount, ai.indented);
                row.handleView.setOnTouchListener((v, event) -> {
                    if (event.getActionMasked() == MotionEvent.ACTION_DOWN && touchHelper != null) {
                        touchHelper.startDrag(holder);
                    }
                    return false;
                });
            }
        }

        @Override
        public int getItemViewType(int position) {
            return items.get(position) instanceof FolderItem ? VIEW_TYPE_FOLDER : VIEW_TYPE_ACCOUNT;
        }

        @Override
        public int getItemCount() {
            return items.size();
        }
    }

    // ---- drag + swipe ------------------------------------------------------------------------

    /** ng: whether two items can be swapped with each other while dragging - top-level entities
     * (ungrouped accounts and folder headers) are freely swappable with each other; accounts
     * inside a folder only swap with other members of that same folder. */
    private static boolean canSwap(Item a, Item b) {
        boolean aIndented = a instanceof AccountItem && ((AccountItem) a).indented;
        boolean bIndented = b instanceof AccountItem && ((AccountItem) b).indented;
        if (aIndented != bIndented) return false;
        if (aIndented) {
            return ((AccountItem) a).folder.equals(((AccountItem) b).folder);
        }
        return true;
    }

    private class TouchCallback extends ItemTouchHelper.Callback {
        @Override
        public boolean isLongPressDragEnabled() {
            // ng: long-press already opens the folder/rename menu - dragging starts only from the
            // dedicated handle icon (see handleView's OnTouchListener in the adapter).
            return false;
        }

        @Override
        public boolean isItemViewSwipeEnabled() {
            return true;
        }

        @Override
        public int getMovementFlags(@NonNull RecyclerView recyclerView, @NonNull RecyclerView.ViewHolder viewHolder) {
            int pos = viewHolder.getAdapterPosition();
            if (pos < 0 || pos >= items.size()) return 0;
            int swipeFlags = items.get(pos) instanceof FolderItem
                    ? ItemTouchHelper.RIGHT | ItemTouchHelper.LEFT
                    : ItemTouchHelper.RIGHT;
            return makeMovementFlags(ItemTouchHelper.UP | ItemTouchHelper.DOWN, swipeFlags);
        }

        @Override
        public boolean onMove(@NonNull RecyclerView recyclerView, @NonNull RecyclerView.ViewHolder source, @NonNull RecyclerView.ViewHolder target) {
            int fromPos = source.getAdapterPosition();
            int toPos = target.getAdapterPosition();
            if (fromPos < 0 || toPos < 0 || fromPos >= items.size() || toPos >= items.size()) return false;
            if (!canSwap(items.get(fromPos), items.get(toPos))) return false;
            Collections.swap(items, fromPos, toPos);
            adapter.notifyItemMoved(fromPos, toPos);
            return true;
        }

        @Override
        public void onSwiped(@NonNull RecyclerView.ViewHolder viewHolder, int direction) {
            int pos = viewHolder.getAdapterPosition();
            if (pos < 0 || pos >= items.size()) return;
            Item item = items.get(pos);
            if (item instanceof AccountItem) {
                UserConfig uc = UserConfig.getInstance(((AccountItem) item).account);
                uc.ncgPinned = !uc.ncgPinned;
                uc.saveConfig(false);
                refresh();
            } else {
                String folder = ((FolderItem) item).name;
                if (direction == ItemTouchHelper.RIGHT) {
                    AccountFolderStore.setPinned(folder, !AccountFolderStore.isPinned(folder));
                    refresh();
                } else {
                    deleteFolder(folder);
                }
            }
        }

        @Override
        public void onSelectedChanged(RecyclerView.ViewHolder viewHolder, int actionState) {
            super.onSelectedChanged(viewHolder, actionState);
            if (viewHolder != null) {
                viewHolder.itemView.setTag(R.id.dragging, actionState == ItemTouchHelper.ACTION_STATE_DRAG ? true : null);
            }
            if (actionState == ItemTouchHelper.ACTION_STATE_IDLE) {
                persistOrderFromItems();
            }
        }

        @Override
        public void clearView(@NonNull RecyclerView recyclerView, @NonNull RecyclerView.ViewHolder viewHolder) {
            super.clearView(recyclerView, viewHolder);
            viewHolder.itemView.setPressed(false);
            viewHolder.itemView.setTag(R.id.dragging, null);
        }
    }
}
