/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and exceptions, see https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "window/ggtg_widgets.h"
#include "ui/abstract_button.h"
#include "ui/painter.h"
#include "ui/rp_widget.h"
#include "ui/widgets/buttons.h"
#include "ui/wrap/vertical_layout.h"
#include "styles/style_boxes.h"
#include "styles/style_widgets.h"

#include <QtGui/QLinearGradient>

namespace Ggtg {

// The dashboard of "Главная": a greeting with the one thing to do, tiles
// with the numbers, and cards for what is going on. Everything is drawn in
// the colors of the theme (dark or light), nothing is hard-coded.

// A soft rounded card with a column inside it. `tint` (when set) puts a
// colored stripe on its left edge: a hot lead, a warning.
class CardBox final : public Ui::RpWidget {
public:
	CardBox(
		not_null<QWidget*> parent,
		style::margins padding = style::margins(14, 12, 14, 12),
		std::optional<QColor> tint = std::nullopt)
	: RpWidget(parent.get())
	, _padding(
		style::ConvertScale(padding.left()),
		style::ConvertScale(padding.top()),
		style::ConvertScale(padding.right()),
		style::ConvertScale(padding.bottom()))
	, _tint(tint) {
		_inner = Ui::CreateChild<Ui::VerticalLayout>(this);
		widthValue(
		) | rpl::on_next([=](int width) {
			_inner->resizeToWidth(
				std::max(width - _padding.left() - _padding.right(), 0));
			_inner->move(_padding.left(), _padding.top());
		}, lifetime());
		_inner->heightValue(
		) | rpl::on_next([=](int height) {
			resize(width(), height + _padding.top() + _padding.bottom());
		}, lifetime());
	}

	[[nodiscard]] not_null<Ui::VerticalLayout*> inner() const {
		return _inner;
	}

	// A tap anywhere on the card (the labels inside let it through).
	void setClickedCallback(Fn<void()> callback) {
		_clicked = std::move(callback);
		setCursor(_clicked ? style::cur_pointer : style::cur_default);
	}

protected:
	void mousePressEvent(QMouseEvent *e) override {
		_pressed = _clicked && (e->button() == Qt::LeftButton);
	}
	void mouseReleaseEvent(QMouseEvent *e) override {
		const auto fire = _pressed
			&& (e->button() == Qt::LeftButton)
			&& rect().contains(e->pos());
		_pressed = false;
		if (fire && _clicked) {
			_clicked();
		}
	}
	void paintEvent(QPaintEvent *e) override {
		auto p = QPainter(this);
		auto hq = PainterHighQualityEnabler(p);
		const auto radius = style::ConvertScale(12);
		p.setPen(Qt::NoPen);
		p.setBrush(st::windowBgOver);
		p.drawRoundedRect(rect(), radius, radius);
		if (_tint) {
			p.setBrush(*_tint);
			const auto stripe = style::ConvertScale(4);
			p.drawRoundedRect(
				QRect(0, radius / 2, stripe, height() - radius),
				stripe / 2.,
				stripe / 2.);
		}
	}

private:
	style::margins _padding;
	std::optional<QColor> _tint;
	Ui::VerticalLayout *_inner = nullptr;
	Fn<void()> _clicked;
	bool _pressed = false;

};

// A number with a caption: one thing about the program at a glance. A tap
// goes to the section it is about.
class StatTile final : public Ui::AbstractButton {
public:
	StatTile(
		not_null<QWidget*> parent,
		const QString &label,
		const QString &value,
		const QString &sub,
		QColor valueColor)
	: AbstractButton(parent.get())
	, _label(label)
	, _value(value)
	, _sub(sub)
	, _valueColor(valueColor)
	, _valueFont(style::font(
		style::ConvertScale(22),
		style::FontFlag::Bold,
		0)) {
		setCursor(style::cur_pointer);
	}

	[[nodiscard]] int heightFor(int width) const {
		const auto pad = style::ConvertScale(14);
		const auto lines = int(WrapText(
			st::normalFont,
			_sub,
			std::max(width - 2 * pad, 0)).size());
		return pad
			+ st::normalFont->height
			+ style::ConvertScale(4)
			+ _valueFont->height
			+ (lines
				? (style::ConvertScale(2) + lines * st::normalFont->height)
				: 0)
			+ pad;
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		auto p = QPainter(this);
		auto hq = PainterHighQualityEnabler(p);
		const auto radius = style::ConvertScale(12);
		p.setPen(Qt::NoPen);
		p.setBrush(isOver() ? st::windowBgRipple : st::windowBgOver);
		p.drawRoundedRect(rect(), radius, radius);
		const auto pad = style::ConvertScale(14);
		const auto available = std::max(width() - 2 * pad, 0);
		auto top = pad;
		p.setPen(st::windowSubTextFg);
		p.setFont(st::normalFont);
		p.drawText(
			pad,
			top + st::normalFont->ascent,
			st::normalFont->elided(_label, available));
		top += st::normalFont->height + style::ConvertScale(4);
		p.setPen(_valueColor);
		p.setFont(_valueFont);
		p.drawText(
			pad,
			top + _valueFont->ascent,
			_valueFont->elided(_value, available));
		top += _valueFont->height + style::ConvertScale(2);
		p.setPen(st::windowSubTextFg);
		p.setFont(st::normalFont);
		for (const auto &line : WrapText(st::normalFont, _sub, available)) {
			p.drawText(pad, top + st::normalFont->ascent, line);
			top += st::normalFont->height;
		}
	}

private:
	QString _label;
	QString _value;
	QString _sub;
	QColor _valueColor;
	style::font _valueFont;

};

// Tiles in two columns, as tall as the tallest of a pair.
class TileGrid final : public Ui::RpWidget {
public:
	explicit TileGrid(not_null<QWidget*> parent)
	: RpWidget(parent.get()) {
		widthValue(
		) | rpl::on_next([=](int width) {
			layout(width);
		}, lifetime());
	}

	StatTile *add(
			const QString &label,
			const QString &value,
			const QString &sub,
			QColor valueColor,
			Fn<void()> onClick) {
		const auto tile = Ui::CreateChild<StatTile>(
			this,
			label,
			value,
			sub,
			valueColor);
		tile->setClickedCallback(std::move(onClick));
		tile->show();
		_tiles.push_back(tile);
		layout(width());
		return tile;
	}

private:
	void layout(int width) {
		if (width <= 0 || _tiles.empty()) {
			return;
		}
		const auto gap = style::ConvertScale(10);
		const auto column = (width - gap) / 2;
		auto y = 0;
		for (auto i = 0; i < int(_tiles.size()); i += 2) {
			const auto pair = std::min(2, int(_tiles.size()) - i);
			auto height = 0;
			for (auto j = 0; j != pair; ++j) {
				height = std::max(height, _tiles[i + j]->heightFor(column));
			}
			for (auto j = 0; j != pair; ++j) {
				_tiles[i + j]->setGeometry(
					j * (column + gap),
					y,
					(pair == 1) ? width : column,
					height);
			}
			y += height + gap;
		}
		resize(width, std::max(y - gap, 0));
	}

	std::vector<StatTile*> _tiles;

};

// A suggestion of what to ask: a short title and a line under it, on a
// rounded card that lights up under the cursor.
class SuggestTile final : public Ui::AbstractButton {
public:
	SuggestTile(
		not_null<QWidget*> parent,
		const QString &title,
		const QString &sub)
	: AbstractButton(parent.get())
	, _title(title)
	, _sub(sub) {
		setCursor(style::cur_pointer);
	}

	[[nodiscard]] int heightFor(int width) const {
		const auto pad = style::ConvertScale(14);
		const auto available = std::max(width - 2 * pad, 0);
		return pad
			+ int(WrapText(st::semiboldFont, _title, available).size())
				* st::semiboldFont->height
			+ style::ConvertScale(3)
			+ int(WrapText(st::normalFont, _sub, available).size())
				* st::normalFont->height
			+ pad;
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		auto p = QPainter(this);
		auto hq = PainterHighQualityEnabler(p);
		const auto radius = style::ConvertScale(12);
		p.setPen(Qt::NoPen);
		p.setBrush(isOver() ? st::windowBgRipple : st::windowBgOver);
		p.drawRoundedRect(rect(), radius, radius);
		const auto pad = style::ConvertScale(14);
		const auto available = std::max(width() - 2 * pad, 0);
		auto top = pad;
		p.setPen(st::windowBoldFg);
		p.setFont(st::semiboldFont);
		for (const auto &line : WrapText(st::semiboldFont, _title, available)) {
			p.drawText(pad, top + st::semiboldFont->ascent, line);
			top += st::semiboldFont->height;
		}
		top += style::ConvertScale(3);
		p.setPen(st::windowSubTextFg);
		p.setFont(st::normalFont);
		for (const auto &line : WrapText(st::normalFont, _sub, available)) {
			p.drawText(pad, top + st::normalFont->ascent, line);
			top += st::normalFont->height;
		}
	}

private:
	QString _title;
	QString _sub;

};

// Suggestions in two columns, as tall as the tallest of a pair.
class SuggestGrid final : public Ui::RpWidget {
public:
	explicit SuggestGrid(not_null<QWidget*> parent)
	: RpWidget(parent.get()) {
		widthValue(
		) | rpl::on_next([=](int width) {
			layout(width);
		}, lifetime());
	}

	void add(const QString &title, const QString &sub, Fn<void()> onClick) {
		const auto tile = Ui::CreateChild<SuggestTile>(this, title, sub);
		tile->setClickedCallback(std::move(onClick));
		tile->show();
		_tiles.push_back(tile);
		layout(width());
	}

private:
	void layout(int width) {
		if (width <= 0 || _tiles.empty()) {
			return;
		}
		const auto gap = style::ConvertScale(10);
		const auto column = (width - gap) / 2;
		auto y = 0;
		for (auto i = 0; i < int(_tiles.size()); i += 2) {
			const auto pair = std::min(2, int(_tiles.size()) - i);
			auto height = 0;
			for (auto j = 0; j != pair; ++j) {
				height = std::max(height, _tiles[i + j]->heightFor(column));
			}
			for (auto j = 0; j != pair; ++j) {
				_tiles[i + j]->setGeometry(
					j * (column + gap),
					y,
					(pair == 1) ? width : column,
					height);
			}
			y += height + gap;
		}
		resize(width, std::max(y - gap, 0));
	}

	std::vector<SuggestTile*> _tiles;

};

// A thin bar: how far a running thing has got.
class ThinProgress final : public Ui::RpWidget {
public:
	ThinProgress(not_null<QWidget*> parent, double ratio)
	: RpWidget(parent.get())
	, _ratio(std::clamp(ratio, 0., 1.)) {
		resize(width(), style::ConvertScale(6));
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		auto p = QPainter(this);
		auto hq = PainterHighQualityEnabler(p);
		const auto radius = height() / 2.;
		p.setPen(Qt::NoPen);
		auto back = QColor(st::windowSubTextFg->c);
		back.setAlphaF(0.2);
		p.setBrush(back);
		p.drawRoundedRect(rect(), radius, radius);
		p.setBrush(st::windowActiveTextFg);
		p.drawRoundedRect(
			QRectF(0, 0, std::max(width() * _ratio, double(height())), height()),
			radius,
			radius);
	}

private:
	double _ratio = 0.;

};

// The first thing on the screen: a greeting, one line of what matters now
// and the button that does it.
class HeroCard final : public Ui::RpWidget {
public:
	HeroCard(
		not_null<QWidget*> parent,
		const QString &title,
		const QString &text,
		const QString &buttonText,
		Fn<void()> onClick)
	: RpWidget(parent.get())
	, _title(title)
	, _text(text)
	, _titleFont(style::font(
		style::ConvertScale(20),
		style::FontFlag::Bold,
		0)) {
		if (!buttonText.isEmpty()) {
			_button = Ui::CreateChild<Ui::RoundButton>(
				this,
				rpl::single(buttonText),
				st::defaultActiveButton);
			_button->setClickedCallback(std::move(onClick));
		}
		widthValue(
		) | rpl::on_next([=](int width) {
			relayout(width);
		}, lifetime());
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		auto p = QPainter(this);
		auto hq = PainterHighQualityEnabler(p);
		const auto radius = style::ConvertScale(16);
		auto from = QColor(st::windowActiveTextFg->c);
		from.setAlphaF(0.28);
		auto to = QColor(st::windowBgOver->c);
		auto gradient = QLinearGradient(0, 0, width(), height());
		gradient.setColorAt(0., from);
		gradient.setColorAt(1., to);
		p.setPen(Qt::NoPen);
		p.setBrush(gradient);
		p.drawRoundedRect(rect(), radius, radius);
		const auto pad = style::ConvertScale(18);
		const auto available = std::max(width() - 2 * pad, 0);
		auto top = pad;
		p.setPen(st::windowBoldFg);
		p.setFont(_titleFont);
		p.drawText(
			pad,
			top + _titleFont->ascent,
			_titleFont->elided(_title, available));
		top += _titleFont->height + style::ConvertScale(6);
		p.setPen(st::windowFg);
		p.setFont(st::normalFont);
		for (const auto &line : WrapText(st::normalFont, _text, available)) {
			p.drawText(pad, top + st::normalFont->ascent, line);
			top += st::normalFont->height;
		}
	}

private:
	void relayout(int width) {
		if (width <= 0) {
			return;
		}
		const auto pad = style::ConvertScale(18);
		const auto available = std::max(width - 2 * pad, 0);
		auto height = pad
			+ _titleFont->height
			+ style::ConvertScale(6)
			+ int(WrapText(st::normalFont, _text, available).size())
				* st::normalFont->height;
		if (_button) {
			height += style::ConvertScale(12);
			_button->moveToLeft(pad, height);
			height += _button->height();
		}
		height += pad;
		resize(width, height);
	}

	QString _title;
	QString _text;
	style::font _titleFont;
	Ui::RoundButton *_button = nullptr;

};

} // namespace Ggtg
