package org.telegram.ui.Components;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.text.TextUtils;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.TextView;

import androidx.annotation.NonNull;

import org.telegram.messenger.AndroidUtilities;
import org.telegram.messenger.LocaleController;
import org.telegram.messenger.MessagesStorage;
import org.telegram.messenger.R;
import org.telegram.messenger.UserConfig;
import org.telegram.messenger.UserObject;
import org.telegram.tgnet.ConnectionsManager;
import org.telegram.tgnet.TLRPC;
import org.telegram.ui.ActionBar.Theme;

import java.util.List;
import java.util.Objects;

import static org.telegram.messenger.AndroidUtilities.dp;

// ng: UItem-based counterparts to AccountSwitcherView's AccountRow/FolderRow, for the folder-aware
// "Аккаунты" section on the Edit Profile screen (UserInfoActivity). Same visuals/business logic
// (via AccountFolderLogic), rendered through the UItem/UniversalAdapter framework that screen uses
// instead of the popup's own hand-rolled RecyclerView.Adapter.
public class AccountFolderCells {

    private static final int ROW_HEIGHT_DP = 48;
    private static final int INDENT_DP = 22;

    public static class AccountFolderRowCell extends LinearLayout {
        final FrameLayout avatarContainer;
        final BackupImageView avatarView;
        final TextView textView;
        final TextView unreadCountView;
        final ImageView pinView;
        final ImageView handleView;
        boolean noProxy;

        public AccountFolderRowCell(Context context) {
            super(context);
            setOrientation(HORIZONTAL);
            setBackground(Theme.createRadSelectorDrawable(Theme.getColor(Theme.key_listSelector), 0, 0));

            avatarContainer = new FrameLayout(context) {
                private final Paint noProxyPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
                private final Paint noProxyBorderPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
                @Override
                protected void dispatchDraw(@NonNull Canvas canvas) {
                    super.dispatchDraw(canvas);
                    float cx = getWidth() - dp(3);
                    float cy = getHeight() - dp(3);
                    noProxyBorderPaint.setColor(Theme.getColor(Theme.key_windowBackgroundWhite));
                    canvas.drawCircle(cx, cy, dp(5.5f), noProxyBorderPaint);
                    noProxyPaint.setColor(noProxy ? 0xFFF44336 : 0xFF4CAF50);
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

        void bind(int account, boolean indented) {
            noProxy = !ConnectionsManager.ncgAccountHasProxy(account);
            UserConfig uc = UserConfig.getInstance(account);
            TLRPC.User user = uc.getCurrentUser();

            ((LinearLayout.LayoutParams) avatarContainer.getLayoutParams()).leftMargin = dp(indented ? 12 + INDENT_DP : 12);

            AvatarDrawable avatarDrawable = new AvatarDrawable();
            avatarDrawable.setInfo(user);
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
            pinView.setVisibility(uc.ncgPinned ? VISIBLE : GONE);
        }

        @Override
        protected void onMeasure(int widthMeasureSpec, int heightMeasureSpec) {
            super.onMeasure(
                    MeasureSpec.makeMeasureSpec(MeasureSpec.getSize(widthMeasureSpec), MeasureSpec.EXACTLY),
                    MeasureSpec.makeMeasureSpec(dp(ROW_HEIGHT_DP), MeasureSpec.EXACTLY)
            );
        }

        public static class Factory extends UItem.UItemFactory<AccountFolderRowCell> {
            static { setup(new Factory()); }

            @Override
            public AccountFolderRowCell createView(Context context, RecyclerListView listView, int currentAccount, int classGuid, Theme.ResourcesProvider resourcesProvider) {
                return new AccountFolderRowCell(context);
            }

            @Override
            public void bindView(View view, UItem item, boolean divider, UniversalAdapter adapter, UniversalRecyclerView listView) {
                ((AccountFolderRowCell) view).bind(item.intValue, item.checked);
                ((AccountFolderRowCell) view).handleView.setOnTouchListener((v, event) -> {
                    if (event.getActionMasked() == android.view.MotionEvent.ACTION_DOWN && listView.itemTouchHelper != null) {
                        androidx.recyclerview.widget.RecyclerView.ViewHolder holder = listView.findContainingViewHolder(view);
                        if (holder != null) listView.itemTouchHelper.startDrag(holder);
                    }
                    return false;
                });
            }

            public static UItem of(int account, boolean indented) {
                UItem item = UItem.ofFactory(AccountFolderRowCell.Factory.class);
                item.intValue = account;
                item.checked = indented;
                return item;
            }

            @Override
            public boolean equals(UItem a, UItem b) {
                return a.intValue == b.intValue && a.checked == b.checked;
            }

            @Override
            public boolean contentsEquals(UItem a, UItem b) {
                return false;
            }
        }
    }

    public static class FolderHeaderRowCell extends LinearLayout {
        final FrameLayout avatarContainer;
        final TextView avatarLetters;
        final ImageView chevron;
        final TextView textView;
        final TextView unreadCountView;
        final ImageView pinView;
        final ImageView handleView;

        public FolderHeaderRowCell(Context context) {
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

        void bind(String name, List<Integer> members) {
            String letters = name.length() >= 2 ? name.substring(0, 2) : name.substring(0, 1);
            avatarLetters.setText(letters.toUpperCase());
            textView.setText(name);
            chevron.setRotation(AccountFolderStore.isExpanded(name) ? 90f : 0f);
            int unreadCount = AccountFolderLogic.folderUnreadCount(members);
            if (unreadCount > 0) {
                unreadCountView.setVisibility(VISIBLE);
                unreadCountView.setText(LocaleController.formatNumber(unreadCount, ','));
            } else {
                unreadCountView.setVisibility(GONE);
            }
            pinView.setVisibility(AccountFolderStore.isPinned(name) ? VISIBLE : GONE);
        }

        @Override
        protected void onMeasure(int widthMeasureSpec, int heightMeasureSpec) {
            super.onMeasure(
                    MeasureSpec.makeMeasureSpec(MeasureSpec.getSize(widthMeasureSpec), MeasureSpec.EXACTLY),
                    MeasureSpec.makeMeasureSpec(dp(ROW_HEIGHT_DP), MeasureSpec.EXACTLY)
            );
        }

        public static class Factory extends UItem.UItemFactory<FolderHeaderRowCell> {
            static { setup(new Factory()); }

            @Override
            public FolderHeaderRowCell createView(Context context, RecyclerListView listView, int currentAccount, int classGuid, Theme.ResourcesProvider resourcesProvider) {
                return new FolderHeaderRowCell(context);
            }

            @Override
            @SuppressWarnings("unchecked")
            public void bindView(View view, UItem item, boolean divider, UniversalAdapter adapter, UniversalRecyclerView listView) {
                ((FolderHeaderRowCell) view).bind((String) item.object, (List<Integer>) item.object2);
                ((FolderHeaderRowCell) view).handleView.setOnTouchListener((v, event) -> {
                    if (event.getActionMasked() == android.view.MotionEvent.ACTION_DOWN && listView.itemTouchHelper != null) {
                        androidx.recyclerview.widget.RecyclerView.ViewHolder holder = listView.findContainingViewHolder(view);
                        if (holder != null) listView.itemTouchHelper.startDrag(holder);
                    }
                    return false;
                });
            }

            public static UItem of(String folderName, List<Integer> members) {
                UItem item = UItem.ofFactory(FolderHeaderRowCell.Factory.class);
                item.object = folderName;
                item.object2 = members;
                return item;
            }

            @Override
            public boolean equals(UItem a, UItem b) {
                return Objects.equals(a.object, b.object);
            }

            @Override
            public boolean contentsEquals(UItem a, UItem b) {
                return false;
            }
        }
    }
}
