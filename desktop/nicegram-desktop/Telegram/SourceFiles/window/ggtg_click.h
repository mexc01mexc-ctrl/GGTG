/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and exceptions, see https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "ui/rp_widget.h"

#include <QtGui/QMouseEvent>

namespace Ggtg {

// One reliable click for a GGTG list row (a checkbox row, a folder header).
//
// Rows used to filter Press/Release themselves and toggle a child checkbox
// by hand, while the checkbox could ALSO react natively - two handlers on
// one gesture, and Qt turns a quick second press into a DblClick event
// that the filter never saw, so a fast "on, then off" was swallowed (the
// checkbox "doesn't release"). Here:
//  - every mouse event on the row goes through one place;
//  - a DblClick counts as an ordinary press, so no click is ever lost;
//  - the click fires on release only if the pointer is still over the row,
//    so a press dragged away from the row does nothing;
//  - `children` (checkboxes, labels) are made transparent for the mouse,
//    so they can't compete with the row for the same click.
//
// `clicked` gets the click's position in the row's own coordinates and
// decides what it hit (checkbox area, badge, plain row...).
inline void SetupRowClicks(
		not_null<Ui::RpWidget*> row,
		std::vector<not_null<QWidget*>> children,
		Fn<void(QPoint)> clicked) {
	for (const auto child : children) {
		child->setAttribute(Qt::WA_TransparentForMouseEvents);
	}
	row->setCursor(style::cur_pointer);
	const auto pending = row->lifetime().make_state<bool>(false);
	row->events(
	) | rpl::filter([](not_null<QEvent*> e) {
		const auto type = e->type();
		return (type == QEvent::MouseButtonPress)
			|| (type == QEvent::MouseButtonDblClick)
			|| (type == QEvent::MouseButtonRelease);
	}) | rpl::on_next([=](not_null<QEvent*> e) {
		const auto mouse = static_cast<QMouseEvent*>(e.get());
		if (mouse->button() != Qt::LeftButton) {
			return;
		}
		if (e->type() != QEvent::MouseButtonRelease) {
			*pending = true;
			return;
		}
		if (std::exchange(*pending, false)
			&& row->rect().contains(mouse->pos())) {
			clicked(mouse->pos());
		}
	}, row->lifetime());
}

} // namespace Ggtg
