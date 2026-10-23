/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and exceptions, see https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "data/data_user.h"
#include "main/main_session.h"
#include "ui/abstract_button.h"
#include "ui/unread_badge.h"
#include "styles/style_dialogs.h"
#include "styles/style_menu_icons.h"
#include "ui/layers/generic_box.h"
#include "ui/layers/show.h"
#include "ui/widgets/buttons.h"
#include "styles/style_layers.h"
#include "ui/painter.h"
#include "styles/style_boxes.h"
#include "styles/style_widgets.h"

#include "ui/rp_widget.h"
#include "ui/widgets/scroll_area.h"
#include "ui/ui_utility.h"
#include "ui/boxes/confirm_box.h"
#include "ui/widgets/popup_menu.h"
#include "base/unique_qptr.h"

#include <QtCore/QPointer>
#include <QtGui/QPainter>
#include <algorithm>
#include <vector>
#include <QtGui/QTextOption>

namespace Ggtg {

// One line of a right-click menu.
struct MenuEntry {
	QString text;
	Fn<void()> callback;
	const style::icon *icon = nullptr;
	bool separatorBefore = false;
};

namespace details {

inline void ShowContextMenu(
		not_null<Ui::RpWidget*> widget,
		const std::shared_ptr<base::unique_qptr<Ui::PopupMenu>> &menu,
		std::vector<MenuEntry> list) {
	if (list.empty()) {
		return;
	}
	*menu = base::make_unique_q<Ui::PopupMenu>(
		widget,
		st::popupMenuWithIcons);
	for (auto &entry : list) {
		if (entry.separatorBefore) {
			(*menu)->addSeparator();
		}
		auto callback = std::move(entry.callback);
		(*menu)->addAction(entry.text, [=] {
			Ui::PostponeCall(widget, callback);
		}, entry.icon);
	}
	(*menu)->popup(QCursor::pos());
}

} // namespace details

// The secondary actions of a row live in its right-click menu, the way they
// do in Telegram itself (a left click keeps doing the main thing). `entries`
// is asked each time the menu opens, so it shows what is true right then; an
// empty list shows nothing. The action runs after the menu has closed, so it
// may rebuild the list the row sits in.
inline void SetContextMenu(
		not_null<Ui::AbstractButton*> button,
		Fn<std::vector<MenuEntry>()> entries) {
	button->setAcceptBoth(true);
	const auto menu = std::make_shared<base::unique_qptr<Ui::PopupMenu>>();
	button->clicks(
	) | rpl::on_next([=](Qt::MouseButton which) {
		if (which == Qt::RightButton) {
			details::ShowContextMenu(button, menu, entries());
		}
	}, button->lifetime());
}

// The same for a plain widget (a label, a block): a right click opens it.
inline void SetWidgetContextMenu(
		not_null<Ui::RpWidget*> widget,
		Fn<std::vector<MenuEntry>()> entries) {
	const auto menu = std::make_shared<base::unique_qptr<Ui::PopupMenu>>();
	widget->events(
	) | rpl::on_next([=](not_null<QEvent*> e) {
		if (e->type() != QEvent::MouseButtonPress) {
			return;
		}
		const auto mouse = static_cast<QMouseEvent*>(e.get());
		if (mouse->button() == Qt::RightButton) {
			details::ShowContextMenu(widget, menu, entries());
		}
	}, widget->lifetime());
}

// Greedy word wrap for rows that paint their own text: a line is never cut
// with "…" - a word longer than the line is broken by letters instead.
[[nodiscard]] inline QStringList WrapText(
		const style::font &font,
		const QString &text,
		int available) {
	auto result = QStringList();
	if (text.isEmpty()) {
		return result;
	}
	available = std::max(available, style::ConvertScale(40));
	auto line = QString();
	const auto flush = [&] {
		if (!line.isEmpty()) {
			result.push_back(line);
			line.clear();
		}
	};
	for (const auto &word : text.split(u' ', Qt::SkipEmptyParts)) {
		const auto candidate = line.isEmpty() ? word : (line + ' ' + word);
		if (font->width(candidate) <= available) {
			line = candidate;
			continue;
		}
		flush();
		if (font->width(word) <= available) {
			line = word;
			continue;
		}
		for (const auto ch : word) {
			if (!line.isEmpty() && font->width(line + ch) > available) {
				flush();
			}
			line += ch;
		}
	}
	flush();
	return result;
}

// Telegram's own marks next to an account's name - the Premium star or the
// emoji status (and verified/scam), exactly as the account list of Telegram
// shows them (Ui::PeerBadge). Hidden - taking no room - for an ordinary
// account or when the user switched the badges off in Telegram.
[[nodiscard]] inline bool ShowsPremiumBadge(not_null<UserData*> user) {
	return user->session().premiumBadgesShown()
		&& (user->isPremium() || user->isVerified());
}

// What a name needs to leave to the badge (0 when there is none).
[[nodiscard]] inline int PremiumBadgeWidth(not_null<UserData*> user) {
	return ShowsPremiumBadge(user)
		? (st::dialogsPremiumIcon.icon.width() + style::ConvertScale(2))
		: 0;
}

// Paints the badge for `user` at the end of a name that is `nameWidth`
// wide and starts at `rect`'s left edge (for rows that paint their own text).
inline int PaintPremiumBadge(
		Painter &p,
		Ui::PeerBadge &badge,
		not_null<UserData*> user,
		QRect rect,
		int nameWidth,
		int outerWidth,
		Fn<void()> repaint) {
	if (!ShowsPremiumBadge(user)) {
		return 0;
	}
	return badge.drawGetWidth(p, {
		.peer = user,
		.rectForName = rect,
		.nameWidth = nameWidth,
		.outerWidth = outerWidth,
		.verified = &st::dialogsVerifiedIcon,
		.premium = &st::dialogsPremiumIcon.icon,
		.scam = &st::dialogsScamFg,
		.direct = &st::windowSubTextFg,
		.premiumFg = &st::dialogsVerifiedIconBg,
		.customEmojiRepaint = std::move(repaint),
		.now = crl::now(),
	});
}

// A small widget that shows the badge of one account; place it right after
// the name's text.
class PremiumBadge final : public Ui::RpWidget {
public:
	PremiumBadge(not_null<QWidget*> parent, not_null<UserData*> user)
	: RpWidget(parent)
	, _user(user) {
		setAttribute(Qt::WA_TransparentForMouseEvents);
		resize(
			st::dialogsPremiumIcon.icon.width() + style::ConvertScale(2),
			std::max(st::dialogsPremiumIcon.icon.height(), st::emojiSize));
		setVisible(ShowsPremiumBadge(user));
	}

	[[nodiscard]] int badgeWidth() const {
		return isHidden() ? 0 : width();
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		auto p = Painter(this);
		PaintPremiumBadge(
			p,
			_badge,
			_user,
			QRect(0, 0, width(), height()),
			0,
			width(),
			[=] { update(); });
	}

private:
	const not_null<UserData*> _user;
	Ui::PeerBadge _badge;

};

// GGTG boxes everywhere: "Назад" (one box back) sits on the left of the
// bottom row, and closing is the cross in the top-right corner - or Esc.
inline void AddBackButton(not_null<Ui::GenericBox*> box) {
	box->addLeftButton(rpl::single(u"Назад"_q), [=] { box->closeBox(); });
}

// GGTG has no passing toasts: a toast lands on the main Telegram window
// (behind this one) and is gone before it can be read. A notice is a small
// window that stays until it is closed, and opens over whichever window the
// user is in (the main window's boxes are redirected into GGTG while it is
// up).
template <typename ShowPtr>
inline void Notice(const ShowPtr &show, const QString &text) {
	if (text.isEmpty()) {
		return;
	}
	show->showBox(Ui::MakeInformBox(Ui::ConfirmBoxArgs{
		.text = text,
		.confirmText = rpl::single(u"Закрыть"_q),
	}));
}

// Every window of a running process (SMS, bulk actions, views, the log
// viewer...) has "Скрыть" on the left of the bottom row: it only closes the
// window, the process keeps running and comes back from "Процессы".
inline void AddHideButton(not_null<Ui::GenericBox*> box) {
	box->addLeftButton(rpl::single(u"Скрыть"_q), [=] { box->closeBox(); });
}

// A process window that can be built again from nothing but a Ui::Show.
// The GGTG window drops its dialogs when the main window switches to
// another account (they belong to the old account's controller); these
// don't, so it builds them again over the new sections instead of losing a
// running process's window.
using ProcessBoxReopen = Fn<void(std::shared_ptr<Ui::Show>)>;

struct ProcessBoxEntry {
	QPointer<QWidget> box;
	ProcessBoxReopen reopen;
};

[[nodiscard]] inline std::vector<ProcessBoxEntry> &ProcessBoxes() {
	static auto result = std::vector<ProcessBoxEntry>();
	return result;
}

inline void AddProcessHideButton(
		not_null<Ui::GenericBox*> box,
		ProcessBoxReopen reopen) {
	AddHideButton(box);
	auto &all = ProcessBoxes();
	all.erase(std::remove_if(all.begin(), all.end(), [](
			const ProcessBoxEntry &entry) {
		return entry.box.isNull();
	}), all.end());
	all.push_back({ QPointer<QWidget>(box.get()), std::move(reopen) });
}

// The reopeners of the process windows currently open inside `window`.
[[nodiscard]] inline std::vector<ProcessBoxReopen> ProcessBoxesIn(
		not_null<QWidget*> window) {
	auto result = std::vector<ProcessBoxReopen>();
	for (const auto &entry : ProcessBoxes()) {
		if (entry.box && entry.box->window() == window.get()) {
			result.push_back(entry.reopen);
		}
	}
	return result;
}

// A dialog that must stay open when the account is switched (the open list of
// a base: the user goes on from the same place). The GGTG window leaves its
// dialogs alone while one of these is open in it.
[[nodiscard]] inline std::vector<QPointer<QWidget>> &KeptBoxes() {
	static auto result = std::vector<QPointer<QWidget>>();
	return result;
}

inline void KeepOpenOnAccountSwitch(not_null<QWidget*> box) {
	auto &all = KeptBoxes();
	all.erase(std::remove_if(all.begin(), all.end(), [](
			const QPointer<QWidget> &entry) {
		return entry.isNull();
	}), all.end());
	all.push_back(QPointer<QWidget>(box.get()));
}

[[nodiscard]] inline bool HasKeptBoxIn(not_null<QWidget*> window) {
	for (const auto &entry : KeptBoxes()) {
		if (entry && entry->isVisible() && entry->window() == window.get()) {
			return true;
		}
	}
	return false;
}

// The GGTG window's sections drop this cross: they have their own close.
inline constexpr auto kCloseCrossName = "ggtg-close-cross";

inline void AddCloseCross(not_null<Ui::GenericBox*> box) {
	// Named before it is handed over: the GGTG window checks the name in
	// its own addTopButton and drops the cross (it has its own close).
	auto cross = object_ptr<Ui::IconButton>(box, st::boxTitleClose);
	cross->setObjectName(QString::fromLatin1(kCloseCrossName));
	cross->setClickedCallback([=] {
		box->closeBox();
	});
	box->addTopButton(std::move(cross));
}

// What one two-line list row shows: a bold title, a muted second line and
// an optional status chip on the right ("осталось 186", "все написано").
struct InfoRowData {
	QString title;
	QString subtitle;
	QString chip;
	bool chipGood = false; // green (done) instead of the accent color
};

// A list row for GGTG screens (bases, records...): replaces a one-line
// "Название — 12 (дата)" button with a clearer title / details / status
// layout. `data` may keep producing values (live counters).
class InfoRow final : public Ui::AbstractButton {
public:
	// `textInset` shifts the texts right, to leave room for a leading icon
	// the caller places itself.
	InfoRow(
		not_null<QWidget*> parent,
		rpl::producer<InfoRowData> data,
		int textInset = 0)
	: AbstractButton(parent.get())
	, _textInset(textInset) {
		resize(width(), style::ConvertScale(56));
		std::move(data) | rpl::on_next([=](InfoRowData value) {
			_data = std::move(value);
			refit();
			update();
		}, lifetime());
		// The row is as tall as its wrapped text needs.
		widthValue(
		) | rpl::on_next([=](int) {
			refit();
		}, lifetime());
	}

	// A right click on the row (the left one stays the clicked callback).
	void setContextMenuCallback(Fn<void()> callback) {
		setAcceptBoth(true);
		if (_contextConnected) {
			_contextCallback = std::move(callback);
			return;
		}
		_contextConnected = true;
		_contextCallback = std::move(callback);
		clicks(
		) | rpl::on_next([=](Qt::MouseButton button) {
			if (button == Qt::RightButton && _contextCallback) {
				_contextCallback();
			}
		}, lifetime());
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		auto p = QPainter(this);
		auto hq = PainterHighQualityEnabler(p);
		const auto radius = style::ConvertScale(10);
		if (isOver()) {
			p.setPen(Qt::NoPen);
			p.setBrush(st::windowBgOver);
			p.drawRoundedRect(rect(), radius, radius);
		}
		const auto pad = style::ConvertScale(14);
		auto right = width() - pad;
		if (!_data.chip.isEmpty()) {
			const auto &font = st::semiboldFont;
			const auto chipPad = style::ConvertScale(10);
			const auto chipWidth = font->width(_data.chip) + 2 * chipPad;
			const auto chipHeight = font->height + style::ConvertScale(6);
			const auto chipRect = QRect(
				right - chipWidth,
				(height() - chipHeight) / 2,
				chipWidth,
				chipHeight);
			const auto color = _data.chipGood
				? st::boxTextFgGood->c
				: st::windowActiveTextFg->c;
			auto fill = color;
			fill.setAlphaF(0.16);
			p.setPen(Qt::NoPen);
			p.setBrush(fill);
			p.drawRoundedRect(chipRect, chipHeight / 2., chipHeight / 2.);
			p.setPen(color);
			p.setFont(font);
			p.drawText(chipRect, Qt::AlignCenter, _data.chip);
			right = chipRect.left() - pad;
		}
		const auto left = pad + _textInset;
		const auto available = std::max(right - left, 0);
		const auto titleFont = st::semiboldFont;
		const auto subFont = st::normalFont;
		const auto titleLines = WrapText(titleFont, _data.title, available);
		const auto subLines = WrapText(subFont, _data.subtitle, available);
		const auto gap = subLines.empty() ? 0 : style::ConvertScale(2);
		const auto total = titleFont->height * int(titleLines.size())
			+ subFont->height * int(subLines.size())
			+ gap;
		auto top = (height() - total) / 2;
		p.setPen(st::windowBoldFg);
		p.setFont(titleFont);
		for (const auto &line : titleLines) {
			p.drawText(left, top + titleFont->ascent, line);
			top += titleFont->height;
		}
		top += gap;
		p.setPen(st::windowSubTextFg);
		p.setFont(subFont);
		for (const auto &line : subLines) {
			p.drawText(left, top + subFont->ascent, line);
			top += subFont->height;
		}
	}

private:
	// The width the texts get: whatever the status chip leaves.
	[[nodiscard]] int textWidth() const {
		const auto pad = style::ConvertScale(14);
		auto right = width() - pad;
		if (!_data.chip.isEmpty()) {
			const auto chipPad = style::ConvertScale(10);
			right -= st::semiboldFont->width(_data.chip)
				+ 2 * chipPad
				+ pad;
		}
		return std::max(right - (pad + _textInset), 0);
	}

	void refit() {
		if (width() <= 0) {
			return;
		}
		const auto available = textWidth();
		const auto titleLines = WrapText(
			st::semiboldFont,
			_data.title,
			available);
		const auto subLines = WrapText(
			st::normalFont,
			_data.subtitle,
			available);
		const auto gap = subLines.empty() ? 0 : style::ConvertScale(2);
		const auto wanted = std::max(
			style::ConvertScale(56),
			st::semiboldFont->height * int(titleLines.size())
				+ st::normalFont->height * int(subLines.size())
				+ gap
				+ 2 * style::ConvertScale(9));
		if (wanted != height()) {
			resize(width(), wanted);
		}
	}

	InfoRowData _data;
	int _textInset = 0;
	Fn<void()> _contextCallback;
	bool _contextConnected = false;

};


// A calm "nothing here yet" block for empty lists: a muted, centered,
// wrapped message with breathing room, instead of a lone gray line jammed
// against the edge.
class EmptyState final : public Ui::RpWidget {
public:
	EmptyState(not_null<QWidget*> parent, const QString &text)
	: RpWidget(parent.get())
	, _text(text) {
		widthValue(
		) | rpl::on_next([=](int width) {
			resize(width, heightFor(width));
		}, lifetime());
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		auto p = QPainter(this);
		p.setPen(st::windowSubTextFg);
		p.setFont(st::normalFont);
		auto option = QTextOption(Qt::AlignHCenter | Qt::AlignVCenter);
		option.setWrapMode(QTextOption::WordWrap);
		p.drawText(rect().marginsRemoved(margins()), _text, option);
	}

private:
	[[nodiscard]] QMargins margins() const {
		const auto side = style::ConvertScale(24);
		const auto vertical = style::ConvertScale(18);
		return QMargins(side, vertical, side, vertical);
	}
	[[nodiscard]] int heightFor(int width) const {
		const auto inner = std::max(width - margins().left() - margins().right(), 1);
		const auto text = QFontMetrics(st::normalFont->f).boundingRect(
			QRect(0, 0, inner, 10000),
			Qt::TextWordWrap,
			_text);
		return text.height() + margins().top() + margins().bottom();
	}

	QString _text;

};


// A button-row for an action ("+ Новый список", "Импорт...", "Сбросить..."):
// an accent glyph badge on the left, the label, a chevron on the right and
// a rounded hover - reads as something to press, unlike a bare line of
// text. `text` may keep changing (counters, current sort mode).
class ActionRow final : public Ui::AbstractButton {
public:
	// `icon`, when given, is painted in the badge instead of `glyph`.
	ActionRow(
		not_null<QWidget*> parent,
		rpl::producer<QString> text,
		const QString &glyph,
		const style::icon *icon = nullptr,
		UserData *premiumUser = nullptr)
	: AbstractButton(parent.get())
	, _glyph(glyph)
	, _icon(icon)
	, _premiumUser(premiumUser) {
		resize(width(), style::ConvertScale(44));
		std::move(text) | rpl::on_next([=](QString value) {
			_text = std::move(value);
			refit();
			update();
		}, lifetime());
		widthValue(
		) | rpl::on_next([=](int) {
			refit();
		}, lifetime());
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		auto p = Painter(this);
		auto hq = PainterHighQualityEnabler(p);
		const auto radius = style::ConvertScale(10);
		if (isOver() || isDown()) {
			p.setPen(Qt::NoPen);
			p.setBrush(st::windowBgOver);
			p.drawRoundedRect(rect(), radius, radius);
		}
		const auto badge = style::ConvertScale(26);
		const auto pad = style::ConvertScale(10);
		const auto badgeRect = QRect(
			pad,
			(height() - badge) / 2,
			badge,
			badge);
		auto fill = st::windowActiveTextFg->c;
		fill.setAlphaF(0.16);
		p.setPen(Qt::NoPen);
		p.setBrush(fill);
		p.drawRoundedRect(badgeRect, badge / 2., badge / 2.);
		p.setPen(st::windowActiveTextFg);
		p.setFont(st::semiboldFont);
		if (_icon) {
			_icon->paintInCenter(p, badgeRect, st::windowActiveTextFg->c);
		} else {
			p.drawText(badgeRect, Qt::AlignCenter, _glyph);
		}

		const auto chevronWidth = st::normalFont->width(u"›"_q);
		const auto textLeft = badgeRect.right() + style::ConvertScale(12);
		const auto available = std::max(
			width() - textLeft - chevronWidth - 2 * pad,
			0);
		p.setPen(st::windowBoldFg);
		p.setFont(st::normalFont);
		const auto lines = WrapText(st::normalFont, _text, available);
		auto top = (height() - st::normalFont->height * int(lines.size())) / 2;
		auto lastTop = top;
		auto lastWidth = 0;
		for (const auto &line : lines) {
			p.drawText(textLeft, top + st::normalFont->ascent, line);
			lastTop = top;
			lastWidth = st::normalFont->width(line);
			top += st::normalFont->height;
		}
		if (_premiumUser && !lines.empty()) {
			PaintPremiumBadge(
				p,
				_premiumBadge,
				_premiumUser,
				QRect(textLeft, lastTop, available, st::normalFont->height),
				std::min(lastWidth, available - PremiumBadgeWidth(_premiumUser)),
				width(),
				[=] { update(); });
		}
		p.setPen(st::windowSubTextFg);
		p.drawText(
			QRect(width() - pad - chevronWidth, 0, chevronWidth, height()),
			Qt::AlignCenter,
			u"›"_q);
	}

private:
	void refit() {
		if (width() <= 0) {
			return;
		}
		const auto pad = style::ConvertScale(10);
		const auto badge = style::ConvertScale(26);
		const auto textLeft = pad + badge + style::ConvertScale(12);
		const auto available = std::max(
			width() - textLeft - st::normalFont->width(u"›"_q) - 2 * pad,
			0);
		const auto lines = WrapText(st::normalFont, _text, available);
		const auto wanted = std::max(
			style::ConvertScale(44),
			st::normalFont->height * int(lines.size())
				+ 2 * style::ConvertScale(12));
		if (wanted != height()) {
			resize(width(), wanted);
		}
	}

	QString _text;
	QString _glyph;
	const style::icon *_icon = nullptr;
	UserData *_premiumUser = nullptr;
	Ui::PeerBadge _premiumBadge;

};


// A clickable list row whose text WRAPS instead of being cut with "…" (a
// Ui::SettingsButton elides): for rows that carry names the user typed or
// long status lines. Hover highlight like the other GGTG rows.
class TextRow final : public Ui::AbstractButton {
public:
	TextRow(not_null<QWidget*> parent, rpl::producer<QString> text)
	: AbstractButton(parent.get()) {
		resize(width(), style::ConvertScale(40));
		setCursor(style::cur_pointer);
		std::move(text) | rpl::on_next([=](QString value) {
			_text = std::move(value);
			refit();
			update();
		}, lifetime());
		widthValue(
		) | rpl::on_next([=](int) {
			refit();
		}, lifetime());
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		auto p = QPainter(this);
		auto hq = PainterHighQualityEnabler(p);
		if (isOver() || isDown()) {
			p.setPen(Qt::NoPen);
			p.setBrush(st::windowBgOver);
			const auto radius = style::ConvertScale(10);
			p.drawRoundedRect(rect(), radius, radius);
		}
		p.setPen(st::windowBoldFg);
		p.setFont(st::normalFont);
		const auto lines = WrapText(st::normalFont, _text, textWidth());
		auto top = (height() - st::normalFont->height * int(lines.size())) / 2;
		for (const auto &line : lines) {
			p.drawText(padding(), top + st::normalFont->ascent, line);
			top += st::normalFont->height;
		}
	}

private:
	[[nodiscard]] int padding() const {
		return style::ConvertScale(14);
	}
	[[nodiscard]] int textWidth() const {
		return std::max(width() - 2 * padding(), 0);
	}
	void refit() {
		if (width() <= 0) {
			return;
		}
		const auto lines = WrapText(st::normalFont, _text, textWidth());
		const auto wanted = std::max(
			style::ConvertScale(40),
			st::normalFont->height * int(lines.size())
				+ 2 * style::ConvertScale(11));
		if (wanted != height()) {
			resize(width(), wanted);
		}
	}

	QString _text;

};

// A small "✕" for removing a list entry: muted until hovered, then red.
class RemoveButton final : public Ui::AbstractButton {
public:
	explicit RemoveButton(not_null<QWidget*> parent)
	: AbstractButton(parent.get()) {
		resize(style::ConvertScale(40), style::ConvertScale(40));
		setCursor(style::cur_pointer);
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		auto p = QPainter(this);
		auto hq = PainterHighQualityEnabler(p);
		if (isOver()) {
			auto fill = st::boxTextFgError->c;
			fill.setAlphaF(0.16);
			p.setPen(Qt::NoPen);
			p.setBrush(fill);
			p.drawEllipse(rect().marginsRemoved(QMargins(
				style::ConvertScale(6),
				style::ConvertScale(6),
				style::ConvertScale(6),
				style::ConvertScale(6))));
		}
		st::menuIconDelete.paintInCenter(
			p,
			rect(),
			isOver() ? st::boxTextFgError->c : st::windowSubTextFg->c);
	}

};

// A small glyph button for a list row's own action (download, ...): muted
// until hovered, then in the accent colour. `hint` is shown as its tooltip.
class GlyphButton final : public Ui::AbstractButton {
public:
	// `icon`, when given, replaces `glyph` (Telegram's own icon, not an emoji).
	GlyphButton(
		not_null<QWidget*> parent,
		const QString &glyph,
		const QString &hint,
		const style::icon *icon = nullptr)
	: AbstractButton(parent.get())
	, _glyph(glyph)
	, _icon(icon) {
		resize(style::ConvertScale(40), style::ConvertScale(40));
		setCursor(style::cur_pointer);
		setToolTip(hint);
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		auto p = QPainter(this);
		auto hq = PainterHighQualityEnabler(p);
		if (isOver()) {
			auto fill = st::windowActiveTextFg->c;
			fill.setAlphaF(0.16);
			p.setPen(Qt::NoPen);
			p.setBrush(fill);
			p.drawEllipse(rect().marginsRemoved(QMargins(
				style::ConvertScale(6),
				style::ConvertScale(6),
				style::ConvertScale(6),
				style::ConvertScale(6))));
		}
		const auto color = isOver()
			? st::windowActiveTextFg->c
			: st::windowSubTextFg->c;
		if (_icon) {
			_icon->paintInCenter(p, rect(), color);
			return;
		}
		p.setPen(color);
		p.setFont(st::semiboldFont);
		p.drawText(rect(), Qt::AlignCenter, _glyph);
	}

private:
	QString _glyph;
	const style::icon *_icon = nullptr;

};

// A thin rounded progress line.
class ProgressBar final : public Ui::RpWidget {
public:
	explicit ProgressBar(not_null<QWidget*> parent)
	: RpWidget(parent.get()) {
		resize(width(), style::ConvertScale(6));
	}

	void setValue(double value) {
		_value = std::clamp(value, 0., 1.);
		update();
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		auto p = QPainter(this);
		auto hq = PainterHighQualityEnabler(p);
		const auto radius = height() / 2.;
		p.setPen(Qt::NoPen);
		p.setBrush(st::windowBgOver);
		p.drawRoundedRect(rect(), radius, radius);
		if (_value > 0.) {
			p.setBrush(st::windowActiveTextFg);
			p.drawRoundedRect(
				QRectF(0, 0, width() * _value, height()),
				radius,
				radius);
		}
	}

private:
	double _value = 0.;

};


// Put one at the top of a function that clears a list and fills it again
// (on a timer, an event, a click): that shrinks the page for a moment and the
// scroll area it sits in would jump to the top. The position is put back when
// the function ends, and once more when the layout has settled.
class ScrollKeeper final {
public:
	explicit ScrollKeeper(not_null<QWidget*> widget) {
		for (auto parent = widget->parentWidget();
			parent;
			parent = parent->parentWidget()) {
			if (const auto area = dynamic_cast<Ui::ScrollArea*>(parent)) {
				_area = area;
				_top = area->scrollTop();
				break;
			}
		}
	}
	ScrollKeeper(const ScrollKeeper &) = delete;
	ScrollKeeper &operator=(const ScrollKeeper &) = delete;
	~ScrollKeeper() {
		const auto area = _area.data();
		if (!area || _top <= 0) {
			return;
		}
		const auto top = _top;
		area->scrollToY(top);
		const auto guard = _area;
		Ui::PostponeCall(area, [=] {
			if (const auto strong = guard.data()) {
				strong->scrollToY(top);
			}
		});
	}

private:
	QPointer<Ui::ScrollArea> _area;
	int _top = 0;

};

} // namespace Ggtg
