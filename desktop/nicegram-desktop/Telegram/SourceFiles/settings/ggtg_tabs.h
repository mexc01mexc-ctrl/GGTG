/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and exceptions, see https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "ui/rp_widget.h"
#include "ui/painter.h"
#include "window/ggtg_widgets.h"
#include "styles/style_basic.h"
#include "styles/style_layers.h"
#include "styles/style_widgets.h"
#include "styles/style_window_main_menu.h" // ggtgTokenButton

#include <rpl/event_stream.h>
#include <QtGui/QColor>
#include <QtGui/QMouseEvent>

namespace Settings {

// A segmented control: a rounded strip with one pill per tab, the open one
// filled with the accent colour.
class GgtgTabs final : public Ui::RpWidget {
public:
	GgtgTabs(not_null<QWidget*> parent, QStringList names, int active)
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

} // namespace Settings
