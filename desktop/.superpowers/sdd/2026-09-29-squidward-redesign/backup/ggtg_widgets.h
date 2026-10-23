/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and exceptions, see https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "ui/abstract_button.h"
#include "ui/painter.h"
#include "styles/style_boxes.h"
#include "styles/style_widgets.h"

#include "ui/rp_widget.h"

#include <QtGui/QPainter>
#include <QtGui/QTextOption>

namespace Ggtg {

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
			update();
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
		const auto total = titleFont->height + subFont->height
			+ style::ConvertScale(2);
		const auto top = (height() - total) / 2;
		p.setPen(st::windowBoldFg);
		p.setFont(titleFont);
		p.drawText(
			left,
			top + titleFont->ascent,
			titleFont->elided(_data.title, available));
		p.setPen(st::windowSubTextFg);
		p.setFont(subFont);
		p.drawText(
			left,
			top + titleFont->height + style::ConvertScale(2)
				+ subFont->ascent,
			subFont->elided(_data.subtitle, available));
	}

private:
	InfoRowData _data;
	int _textInset = 0;

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
	ActionRow(
		not_null<QWidget*> parent,
		rpl::producer<QString> text,
		const QString &glyph)
	: AbstractButton(parent.get())
	, _glyph(glyph) {
		resize(width(), style::ConvertScale(44));
		std::move(text) | rpl::on_next([=](QString value) {
			_text = std::move(value);
			update();
		}, lifetime());
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		auto p = QPainter(this);
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
		p.drawText(badgeRect, Qt::AlignCenter, _glyph);

		const auto chevronWidth = st::normalFont->width(u"›"_q);
		const auto textLeft = badgeRect.right() + style::ConvertScale(12);
		const auto available = std::max(
			width() - textLeft - chevronWidth - 2 * pad,
			0);
		p.setPen(st::windowBoldFg);
		p.setFont(st::normalFont);
		p.drawText(
			QRect(textLeft, 0, available, height()),
			Qt::AlignLeft | Qt::AlignVCenter,
			st::normalFont->elided(_text, available));
		p.setPen(st::windowSubTextFg);
		p.drawText(
			QRect(width() - pad - chevronWidth, 0, chevronWidth, height()),
			Qt::AlignCenter,
			u"›"_q);
	}

private:
	QString _text;
	QString _glyph;

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
		p.setPen(isOver() ? st::boxTextFgError : st::windowSubTextFg);
		p.setFont(st::semiboldFont);
		p.drawText(rect(), Qt::AlignCenter, u"✕"_q);
	}

};

// A small glyph button for a list row's own action (download, ...): muted
// until hovered, then in the accent colour. `hint` is shown as its tooltip.
class GlyphButton final : public Ui::AbstractButton {
public:
	GlyphButton(
		not_null<QWidget*> parent,
		const QString &glyph,
		const QString &hint)
	: AbstractButton(parent.get())
	, _glyph(glyph) {
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
		p.setPen(isOver() ? st::windowActiveTextFg : st::windowSubTextFg);
		p.setFont(st::semiboldFont);
		p.drawText(rect(), Qt::AlignCenter, _glyph);
	}

private:
	QString _glyph;

};

} // namespace Ggtg
