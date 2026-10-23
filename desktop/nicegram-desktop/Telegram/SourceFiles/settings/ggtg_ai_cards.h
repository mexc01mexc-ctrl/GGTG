/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and exceptions, see https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "ui/rp_widget.h"
#include "ui/painter.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "ui/text/text_entity.h"
#include "window/ggtg_widgets.h"
#include "styles/style_basic.h"
#include "styles/style_layers.h"
#include "styles/style_widgets.h"
#include "styles/style_window_main_menu.h" // ggtgTokenButton

#include <rpl/event_stream.h>
#include <QtGui/QColor>
#include <QtGui/QMouseEvent>

namespace Settings {

struct AiCardAction {
	QString text;
	Fn<void()> callback;
};

// What a card shows: a coloured dot and a title, a small chip with the state
// on the right, a few lines of details, an optional progress bar and a row
// of buttons - the look of the lane cards of the SMS progress window.
struct AiCardView {
	QColor dot;
	QString title;
	QString badge;
	QColor badgeColor;
	QString detail;
	double progress = -1.; // < 0: no bar
	std::vector<AiCardAction> actions;
};

class AiCard final : public Ui::RpWidget {
public:
	explicit AiCard(not_null<QWidget*> parent)
	: RpWidget(parent.get()) {
		widthValue(
		) | rpl::on_next([=](int width) {
			relayout(width);
		}, lifetime());
	}

	// Cheap to call every second: nothing is rebuilt while it looks the same.
	void setView(AiCardView view) {
		auto signature = view.title + '|' + view.badge + '|' + view.detail
			+ '|' + QString::number(int(view.progress * 1000));
		for (const auto &action : view.actions) {
			signature += '|' + action.text;
		}
		signature += '|' + view.dot.name() + view.badgeColor.name();
		const auto same = (signature == _signature);
		_signature = std::move(signature);
		_view = std::move(view);
		if (same) {
			// Only the callbacks may differ (they capture nothing that
			// changes between equal views).
			for (auto i = size_t(0); i != _buttons.size(); ++i) {
				if (i < _view.actions.size()) {
					_buttons[i]->setClickedCallback(
						_view.actions[i].callback);
				}
			}
			return;
		}
		rebuildButtons();
		relayout(width());
		update();
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		auto p = QPainter(this);
		auto hq = PainterHighQualityEnabler(p);
		const auto radius = style::ConvertScale(10);
		auto bg = st::windowBoldFg->c;
		bg.setAlphaF(0.06);
		p.setPen(Qt::NoPen);
		p.setBrush(bg);
		p.drawRoundedRect(rect(), radius, radius);

		const auto pad = padding();
		const auto dotSize = style::ConvertScale(12);
		const auto titleHeight = st::semiboldFont->height;
		p.setBrush(_view.dot);
		p.drawEllipse(QRect(
			pad,
			pad + (titleHeight - dotSize) / 2,
			dotSize,
			dotSize));

		// The state chip on the right.
		auto chipWidth = 0;
		if (!_view.badge.isEmpty()) {
			p.setFont(st::normalFont);
			const auto text = _view.badge;
			chipWidth = st::normalFont->width(text)
				+ 2 * style::ConvertScale(9);
			const auto chipHeight = st::normalFont->height
				+ 2 * style::ConvertScale(3);
			const auto chip = QRect(
				width() - pad - chipWidth,
				pad + (titleHeight - chipHeight) / 2,
				chipWidth,
				chipHeight);
			auto fill = _view.badgeColor;
			fill.setAlphaF(0.18);
			p.setPen(Qt::NoPen);
			p.setBrush(fill);
			p.drawRoundedRect(chip, chipHeight / 2., chipHeight / 2.);
			p.setPen(_view.badgeColor);
			p.drawText(chip, Qt::AlignCenter, text);
		}

		const auto left = textLeft();
		const auto titleWidth = std::max(
			width() - left - pad - chipWidth
				- (chipWidth ? style::ConvertScale(8) : 0),
			1);
		p.setPen(st::windowFg);
		p.setFont(st::semiboldFont);
		p.drawText(
			QRect(left, pad, titleWidth, titleHeight),
			Qt::AlignVCenter | Qt::AlignLeft,
			st::semiboldFont->elided(_view.title, titleWidth));

		p.setFont(st::normalFont);
		p.setPen(st::windowSubTextFg);
		auto y = pad + titleHeight + style::ConvertScale(6);
		for (const auto &line : _detailLines) {
			p.drawText(left, y + st::normalFont->ascent, line);
			y += st::normalFont->height;
		}

		if (_view.progress >= 0.) {
			const auto barHeight = style::ConvertScale(6);
			const auto barTop = y + style::ConvertScale(6);
			const auto barLeft = left;
			const auto barWidth = std::max(width() - left - pad, 1);
			p.setPen(Qt::NoPen);
			p.setBrush(st::windowBgOver);
			p.drawRoundedRect(
				QRectF(barLeft, barTop, barWidth, barHeight),
				barHeight / 2.,
				barHeight / 2.);
			p.setBrush(_view.dot);
			p.drawRoundedRect(
				QRectF(
					barLeft,
					barTop,
					barWidth * std::clamp(_view.progress, 0., 1.),
					barHeight),
				barHeight / 2.,
				barHeight / 2.);
		}
	}

private:
	[[nodiscard]] int padding() const {
		return style::ConvertScale(12);
	}
	[[nodiscard]] int textLeft() const {
		return padding() + style::ConvertScale(12) + style::ConvertScale(10);
	}

	void rebuildButtons() {
		for (const auto button : _buttons) {
			button->hide();
			button->deleteLater();
		}
		_buttons.clear();
		for (const auto &action : _view.actions) {
			const auto button = Ui::CreateChild<Ui::RoundButton>(
				this,
				rpl::single(action.text),
				st::ggtgTokenButton);
			button->setClickedCallback(action.callback);
			button->show();
			_buttons.push_back(button);
		}
	}

	void relayout(int width) {
		if (width <= 0) {
			return;
		}
		const auto pad = padding();
		const auto left = textLeft();
		const auto available = std::max(width - left - pad, 1);
		_detailLines = Ggtg::WrapText(
			st::normalFont,
			_view.detail,
			available);
		auto bottom = pad + st::semiboldFont->height;
		if (!_detailLines.isEmpty()) {
			bottom += style::ConvertScale(6)
				+ int(_detailLines.size()) * st::normalFont->height;
		}
		if (_view.progress >= 0.) {
			bottom += style::ConvertScale(6) + style::ConvertScale(6);
		}
		if (!_buttons.empty()) {
			bottom += style::ConvertScale(10);
			const auto gap = style::ConvertScale(8);
			auto x = left;
			auto y = bottom;
			auto rowHeight = 0;
			for (const auto button : _buttons) {
				if (x > left && x + button->width() > width - pad) {
					x = left;
					y += rowHeight + gap;
				}
				button->moveToLeft(x, y, width);
				x += button->width() + gap;
				rowHeight = std::max(rowHeight, button->height());
			}
			bottom = y + rowHeight;
		}
		const auto height = bottom + pad;
		if (this->height() != height) {
			resize(width, height);
		}
	}

	AiCardView _view;
	QString _signature;
	QStringList _detailLines;
	std::vector<Ui::RoundButton*> _buttons;

};

// A segmented control: a rounded strip with one pill per tab, the open one
// filled with the accent colour.
class AiTabs final : public Ui::RpWidget {
public:
	AiTabs(not_null<QWidget*> parent, QStringList names, int active)
	: RpWidget(parent.get())
	, _names(std::move(names))
	, _active(active) {
		resize(width(), style::ConvertScale(42));
		setMouseTracking(true);
		setCursor(style::cur_pointer);
	}

	[[nodiscard]] rpl::producer<int> activeChanges() const {
		return _changes.events();
	}
	[[nodiscard]] int active() const {
		return _active;
	}
	// Switches the open tab without reporting it in activeChanges().
	void setActive(int index) {
		if (_active != index) {
			_active = index;
			update();
		}
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		auto p = QPainter(this);
		auto hq = PainterHighQualityEnabler(p);
		const auto radius = height() / 2.;
		auto bg = st::windowBoldFg->c;
		bg.setAlphaF(0.07);
		p.setPen(Qt::NoPen);
		p.setBrush(bg);
		p.drawRoundedRect(rect(), radius, radius);
		const auto count = std::max(int(_names.size()), 1);
		const auto inset = style::ConvertScale(4);
		const auto segment = (width() - 2 * inset) / double(count);
		for (auto i = 0; i != int(_names.size()); ++i) {
			const auto rect = QRectF(
				inset + i * segment,
				inset,
				segment,
				height() - 2 * inset);
			const auto active = (i == _active);
			if (active) {
				p.setBrush(st::windowBgActive);
				p.drawRoundedRect(rect, rect.height() / 2., rect.height() / 2.);
			}
			p.setFont(active ? st::semiboldFont : st::normalFont);
			p.setPen(active
				? st::windowFgActive
				: (i == _over ? st::windowFg : st::windowSubTextFg));
			p.drawText(rect, Qt::AlignCenter, _names[i]);
		}
	}

	void mouseMoveEvent(QMouseEvent *e) override {
		const auto over = indexAt(e->pos().x());
		if (_over != over) {
			_over = over;
			update();
		}
	}
	void leaveEventHook(QEvent *e) override {
		if (_over != -1) {
			_over = -1;
			update();
		}
	}
	void mousePressEvent(QMouseEvent *e) override {
		if (e->button() != Qt::LeftButton) {
			return;
		}
		const auto index = indexAt(e->pos().x());
		if (index >= 0 && index != _active) {
			_active = index;
			update();
			_changes.fire_copy(index);
		}
	}

private:
	[[nodiscard]] int indexAt(int x) const {
		const auto count = std::max(int(_names.size()), 1);
		const auto index = int(x * count / std::max(width(), 1));
		return (index >= 0 && index < int(_names.size())) ? index : -1;
	}

	QStringList _names;
	int _active = 0;
	int _over = -1;
	rpl::event_stream<int> _changes;

};

// One message of a chat with the AI: the person's own on the right in a
// tinted bubble, the AI's on the left in a quiet one. The bubble is as wide
// as its text needs, at most four fifths of the row.
class AiBubble final : public Ui::RpWidget {
public:
	AiBubble(
		not_null<QWidget*> parent,
		const TextWithEntities &text,
		bool mine)
	: RpWidget(parent.get())
	, _mine(mine)
	, _label(Ui::CreateChild<Ui::FlatLabel>(
		this,
		QString(),
		st::boxLabel)) {
		_label->setMarkedText(text);
		_label->setSelectable(true);
		widthValue(
		) | rpl::on_next([=](int width) {
			layout(width);
		}, lifetime());
	}

	void setMarkedText(const TextWithEntities &text) {
		_label->setMarkedText(text);
		layout(width());
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		auto p = QPainter(this);
		auto hq = PainterHighQualityEnabler(p);
		auto bg = _mine
			? st::windowActiveTextFg->c
			: st::windowBoldFg->c;
		bg.setAlphaF(_mine ? 0.20 : 0.08);
		p.setPen(Qt::NoPen);
		p.setBrush(bg);
		const auto radius = style::ConvertScale(12);
		p.drawRoundedRect(_bubble, radius, radius);
	}

private:
	void layout(int width) {
		if (width <= 0) {
			return;
		}
		const auto pad = style::ConvertScale(10);
		const auto maxBubble = std::max(width * 4 / 5, style::ConvertScale(160));
		const auto natural = _label->naturalWidth() + 2 * pad;
		const auto bubbleWidth = std::min({ natural, maxBubble, width });
		_label->resizeToWidth(std::max(bubbleWidth - 2 * pad, 1));
		const auto left = _mine ? (width - bubbleWidth) : 0;
		_label->moveToLeft(left + pad, pad);
		_bubble = QRect(left, 0, bubbleWidth, _label->height() + 2 * pad);
		resize(width, _bubble.height());
		update();
	}

	const bool _mine;
	Ui::FlatLabel *_label = nullptr;
	QRect _bubble;

};

} // namespace Settings
