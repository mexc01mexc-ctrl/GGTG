/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and exceptions, see https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

// How a mailing looks: outcome colors and icons, the status card, the tiles,
// the account and recipient rows, the folding step cards of the "Новая"
// form. Drawing only - every number comes in from settings/sms_campaign.cpp.

#include "settings/sms_campaign_model.h"
#include "window/ggtg_widgets.h"
#include "ui/abstract_button.h"
#include "ui/widgets/buttons.h"
#include "ui/rp_widget.h"
#include "ui/painter.h"
#include "ui/wrap/vertical_layout.h"
#include "ui/wrap/slide_wrap.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_widgets.h"

#include <QtGui/QMouseEvent>
#include <QtGui/QPainterPath>

namespace Settings::SmsWidgets {

using SmsModel::Outcome;

[[nodiscard]] inline QColor OutcomeColor(Outcome outcome) {
	switch (outcome) {
	case Outcome::Sent: return QColor(76, 217, 100);
	case Outcome::Peer: return QColor(255, 159, 10);
	case Outcome::Closed:
	case Outcome::NotFound:
	case Outcome::Skipped: return st::windowSubTextFg->c;
	case Outcome::Spam:
	case Outcome::Fail: return QColor(255, 69, 58);
	}
	return st::windowSubTextFg->c;
}

[[nodiscard]] inline QColor Tinted(QColor color, double alpha) {
	color.setAlphaF(alpha);
	return color;
}

// A small line icon in `rect`, drawn in the outcome's color; nullopt is "in
// progress" (an open ring in the accent color).
inline void PaintOutcomeIcon(
		QPainter &p,
		QRect rect,
		std::optional<Outcome> outcome) {
	auto hq = PainterHighQualityEnabler(p);
	const auto color = outcome
		? OutcomeColor(*outcome)
		: st::windowActiveTextFg->c;
	auto pen = QPen(color, style::ConvertScale(1.6));
	pen.setCapStyle(Qt::RoundCap);
	pen.setJoinStyle(Qt::RoundJoin);
	p.setPen(pen);
	p.setBrush(Qt::NoBrush);
	const auto r = QRectF(rect).adjusted(2, 2, -2, -2);
	const auto x = [&](double k) { return r.x() + r.width() * k; };
	const auto y = [&](double k) { return r.y() + r.height() * k; };
	if (!outcome) {
		p.drawArc(r, 90 * 16, -270 * 16);
		return;
	}
	switch (*outcome) {
	case Outcome::Sent:
		p.drawPolyline(QPolygonF(QVector<QPointF>{
			QPointF(x(0.15), y(0.55)),
			QPointF(x(0.42), y(0.8)),
			QPointF(x(0.88), y(0.22)) }));
		break;
	case Outcome::Peer:
		p.drawEllipse(r);
		p.drawLine(QPointF(x(0.28), y(0.5)), QPointF(x(0.72), y(0.5)));
		break;
	case Outcome::Closed:
		p.drawRoundedRect(
			QRectF(x(0.18), y(0.45), r.width() * 0.64, r.height() * 0.48),
			2,
			2);
		p.drawArc(
			QRectF(x(0.3), y(0.08), r.width() * 0.4, r.height() * 0.6),
			0,
			180 * 16);
		break;
	case Outcome::NotFound:
		p.drawEllipse(r);
		p.setFont(st::semiboldFont);
		p.drawText(r, Qt::AlignCenter, u"?"_q);
		break;
	case Outcome::Spam: {
		auto path = QPainterPath();
		path.moveTo(x(0.5), y(0.05));
		path.lineTo(x(0.9), y(0.2));
		path.lineTo(x(0.85), y(0.6));
		path.lineTo(x(0.5), y(0.95));
		path.lineTo(x(0.15), y(0.6));
		path.lineTo(x(0.1), y(0.2));
		path.closeSubpath();
		p.drawPath(path);
	} break;
	case Outcome::Fail:
		p.drawPolygon(QPolygonF(QVector<QPointF>{
			QPointF(x(0.5), y(0.08)),
			QPointF(x(0.95), y(0.9)),
			QPointF(x(0.05), y(0.9)) }));
		p.drawLine(QPointF(x(0.5), y(0.38)), QPointF(x(0.5), y(0.62)));
		p.drawPoint(QPointF(x(0.5), y(0.76)));
		break;
	case Outcome::Skipped:
		p.drawLine(QPointF(x(0.1), y(0.5)), QPointF(x(0.8), y(0.5)));
		p.drawPolyline(QPolygonF(QVector<QPointF>{
			QPointF(x(0.55), y(0.25)),
			QPointF(x(0.85), y(0.5)),
			QPointF(x(0.55), y(0.75)) }));
		break;
	}
}

inline void PaintCardBackground(QPainter &p, QRect rect) {
	auto hq = PainterHighQualityEnabler(p);
	const auto radius = style::ConvertScale(10);
	p.setPen(Qt::NoPen);
	p.setBrush(Tinted(st::windowBoldFg->c, 0.06));
	p.drawRoundedRect(rect, radius, radius);
}

// The progress line split into the outcomes' colors, over a muted track.
inline void PaintSegments(
		QPainter &p,
		QRectF rect,
		const SmsModel::Counts &counts,
		int goal) {
	auto hq = PainterHighQualityEnabler(p);
	const auto radius = rect.height() / 2.;
	p.setPen(Qt::NoPen);
	p.setBrush(Tinted(st::windowBoldFg->c, 0.12));
	p.drawRoundedRect(rect, radius, radius);
	const auto total = std::max({ goal, SmsModel::Total(counts), 1 });
	auto clip = QPainterPath();
	clip.addRoundedRect(rect, radius, radius);
	p.save();
	p.setClipPath(clip);
	auto left = rect.x();
	for (auto i = 0; i != SmsModel::kOutcomeCount; ++i) {
		if (!counts[i]) {
			continue;
		}
		const auto width = rect.width() * counts[i] / total;
		p.setBrush(OutcomeColor(Outcome(i)));
		p.drawRect(QRectF(left, rect.y(), width, rect.height()));
		left += width;
	}
	p.restore();
}

class SegmentBar final : public Ui::RpWidget {
public:
	explicit SegmentBar(not_null<QWidget*> parent)
	: RpWidget(parent.get()) {
		resize(width(), style::ConvertScale(6));
	}

	void setCounts(const SmsModel::Counts &counts, int goal) {
		_counts = counts;
		_goal = goal;
		update();
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		auto p = QPainter(this);
		PaintSegments(p, QRectF(rect()), _counts, _goal);
	}

private:
	SmsModel::Counts _counts{};
	int _goal = 0;

};

// Six tiles (every outcome but Skipped): the number in the outcome's
// color, the title under it. With
// `clickable` a tile asks to filter the list by its outcome (again - to drop
// the filter); the owner answers with setFilter().
class OutcomeTiles final : public Ui::RpWidget {
public:
	OutcomeTiles(not_null<QWidget*> parent, bool clickable)
	: RpWidget(parent.get())
	, _clickable(clickable) {
		if (_clickable) {
			setCursor(style::cur_pointer);
		}
		widthValue(
		) | rpl::on_next([=](int) {
			relayout();
		}, lifetime());
	}

	void setCounts(const SmsModel::Counts &counts) {
		_counts = counts;
		update();
	}
	void setFilter(SmsModel::Filter filter) {
		_filter = filter;
		update();
	}
	[[nodiscard]] rpl::producer<SmsModel::Filter> filterRequests() const {
		return _requests.events();
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		auto p = QPainter(this);
		auto hq = PainterHighQualityEnabler(p);
		const auto radius = style::ConvertScale(8);
		for (auto i = 0; i != SmsModel::kTileOutcomeCount; ++i) {
			const auto outcome = Outcome(i);
			const auto r = tileRect(i);
			const auto selected = (_filter != SmsModel::Filter::All)
				&& (SmsModel::FilterFor(outcome) == _filter);
			p.setBrush(Tinted(st::windowBoldFg->c, selected ? 0.12 : 0.05));
			p.setPen(selected
				? QPen(OutcomeColor(outcome), style::ConvertScale(1))
				: QPen(Qt::NoPen));
			p.drawRoundedRect(r, radius, radius);
			const auto pad = style::ConvertScale(8);
			const auto inner = r.adjusted(pad, pad, -pad, -pad);
			const auto &big = st::boxTitle.style.font;
			p.setFont(big);
			p.setPen(_counts[i]
				? OutcomeColor(outcome)
				: st::windowSubTextFg->c);
			p.drawText(
				inner.x(),
				inner.y() + big->ascent,
				QString::number(_counts[i]));
			p.setFont(st::normalFont);
			p.setPen(st::windowSubTextFg);
			p.drawText(
				QRect(
					inner.x(),
					inner.y() + big->height,
					inner.width(),
					st::normalFont->height),
				Qt::AlignLeft | Qt::AlignVCenter,
				st::normalFont->elided(
					SmsModel::TileTitle(outcome),
					inner.width()));
		}
	}

	void mouseReleaseEvent(QMouseEvent *e) override {
		if (!_clickable) {
			return;
		}
		for (auto i = 0; i != SmsModel::kTileOutcomeCount; ++i) {
			if (tileRect(i).contains(e->pos())) {
				const auto filter = SmsModel::FilterFor(Outcome(i));
				_requests.fire((filter == _filter)
					? SmsModel::Filter::All
					: filter);
				return;
			}
		}
	}

private:
	[[nodiscard]] static int tileHeight() {
		return st::boxTitle.style.font->height
			+ st::normalFont->height
			+ 2 * style::ConvertScale(8);
	}
	[[nodiscard]] int columns() const {
		return (width() < style::ConvertScale(520)) ? 3 : 6;
	}
	[[nodiscard]] QRect tileRect(int i) const {
		const auto gap = style::ConvertScale(6);
		const auto count = columns();
		const auto w = (width() - gap * (count - 1)) / count;
		const auto h = tileHeight();
		return QRect(
			(i % count) * (w + gap),
			(i / count) * (h + gap),
			w,
			h);
	}
	void relayout() {
		const auto gap = style::ConvertScale(6);
		const auto rows = SmsModel::kTileOutcomeCount / columns();
		const auto wanted = rows * tileHeight() + (rows - 1) * gap;
		if (height() != wanted) {
			resize(width(), wanted);
		}
		update();
	}

	const bool _clickable = false;
	SmsModel::Counts _counts{};
	SmsModel::Filter _filter = SmsModel::Filter::All;
	rpl::event_stream<SmsModel::Filter> _requests;

};

// "Все 134 · Отправлено 86 · Отказ Telegram 27 · Закрыты 14 · Проблемы 3"
// as pill chips above the recipient list.
class FilterChips final : public Ui::RpWidget {
public:
	explicit FilterChips(not_null<QWidget*> parent)
	: RpWidget(parent.get()) {
		setCursor(style::cur_pointer);
		widthValue(
		) | rpl::on_next([=](int) {
			relayout();
		}, lifetime());
	}

	void setCounts(const SmsModel::Counts &counts) {
		_counts = counts;
		relayout();
		update();
	}
	void setFilter(SmsModel::Filter filter) {
		_filter = filter;
		update();
	}
	[[nodiscard]] rpl::producer<SmsModel::Filter> filterRequests() const {
		return _requests.events();
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		auto p = QPainter(this);
		auto hq = PainterHighQualityEnabler(p);
		for (const auto &chip : _chips) {
			const auto selected = (chip.filter == _filter);
			p.setPen(selected
				? QPen(Qt::NoPen)
				: QPen(Tinted(st::windowBoldFg->c, 0.18), 1));
			p.setBrush(selected
				? Tinted(st::windowActiveTextFg->c, 0.18)
				: QColor(Qt::transparent));
			const auto radius = chip.rect.height() / 2.;
			p.drawRoundedRect(chip.rect, radius, radius);
			p.setFont(selected ? st::semiboldFont : st::normalFont);
			p.setPen(selected ? st::windowActiveTextFg : st::windowFg);
			p.drawText(chip.rect, Qt::AlignCenter, chip.text);
		}
	}

	void mouseReleaseEvent(QMouseEvent *e) override {
		for (const auto &chip : _chips) {
			if (chip.rect.contains(e->pos())) {
				_requests.fire_copy(chip.filter);
				return;
			}
		}
	}

private:
	struct Chip {
		SmsModel::Filter filter = SmsModel::Filter::All;
		QString text;
		QRect rect;
	};

	void relayout() {
		_chips.clear();
		const auto gap = style::ConvertScale(6);
		const auto padX = style::ConvertScale(12);
		const auto h = st::semiboldFont->height + style::ConvertScale(8);
		auto x = 0;
		auto y = 0;
		for (const auto filter : {
				SmsModel::Filter::All,
				SmsModel::Filter::Sent,
				SmsModel::Filter::Peer,
				SmsModel::Filter::Closed,
				SmsModel::Filter::Problems }) {
			const auto count = SmsModel::CountFor(_counts, filter);
			if (!count && filter != SmsModel::Filter::All) {
				continue;
			}
			const auto text = SmsModel::FilterTitle(filter)
				+ ' ' + QString::number(count);
			const auto w = st::semiboldFont->width(text) + 2 * padX;
			if (x > 0 && x + w > width()) {
				x = 0;
				y += h + gap;
			}
			_chips.push_back({ filter, text, QRect(x, y, w, h) });
			x += w + gap;
		}
		const auto wanted = y + h;
		if (height() != wanted) {
			resize(width(), wanted);
		}
		update();
	}

	SmsModel::Counts _counts{};
	SmsModel::Filter _filter = SmsModel::Filter::All;
	std::vector<Chip> _chips;
	rpl::event_stream<SmsModel::Filter> _requests;

};

struct StatusData {
	QString title; // "Идёт рассылка", "Аккаунты закончились"...
	QString subtitle; // "2 потока · осталось ≈ 40 мин"
	int processed = 0;
	int goal = 0;
	SmsModel::Counts counts{};
	QString finishLine; // set once finished, replaces the subtitle
	QString earlierLine; // "Пропущены заранее (уже писали, боты): 40"
};

// The top of the progress window: what is happening in one phrase, the
// processed / goal count, the progress line in the outcomes' colors.
class StatusCard final : public Ui::RpWidget {
public:
	explicit StatusCard(not_null<QWidget*> parent)
	: RpWidget(parent.get()) {
		widthValue(
		) | rpl::on_next([=](int) {
			refit();
		}, lifetime());
	}

	void setData(StatusData data) {
		_data = std::move(data);
		refit();
		update();
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		auto p = QPainter(this);
		PaintCardBackground(p, rect());
		const auto pad = style::ConvertScale(14);
		const auto inner = width() - 2 * pad;
		auto y = pad;
		const auto &big = st::boxTitle.style.font;
		const auto count = QString::number(_data.processed)
			+ u" / "_q + QString::number(_data.goal);
		const auto countWidth = big->width(count);
		p.setFont(st::semiboldFont);
		p.setPen(st::windowBoldFg);
		p.drawText(
			pad,
			y + st::semiboldFont->ascent,
			st::semiboldFont->elided(
				_data.title,
				std::max(inner - countWidth - pad, 1)));
		p.setFont(big);
		p.setPen(st::windowFg);
		p.drawText(
			QRect(pad, y, inner, big->height),
			Qt::AlignRight | Qt::AlignTop,
			count);
		y += std::max(big->height, st::semiboldFont->height)
			+ style::ConvertScale(4);
		p.setFont(st::normalFont);
		p.setPen(st::windowSubTextFg);
		for (const auto &line : _lines) {
			p.drawText(pad, y + st::normalFont->ascent, line);
			y += st::normalFont->height;
		}
		y += style::ConvertScale(8);
		PaintSegments(
			p,
			QRectF(pad, y, inner, style::ConvertScale(6)),
			_data.counts,
			_data.goal);
	}

private:
	void refit() {
		const auto pad = style::ConvertScale(14);
		const auto inner = std::max(width() - 2 * pad, 1);
		_lines.clear();
		const auto sub = _data.finishLine.isEmpty()
			? _data.subtitle
			: _data.finishLine;
		for (const auto &text : { sub, _data.earlierLine }) {
			if (!text.isEmpty()) {
				_lines += Ggtg::WrapText(st::normalFont, text, inner);
			}
		}
		const auto wanted = 2 * pad
			+ std::max(
				st::boxTitle.style.font->height,
				st::semiboldFont->height)
			+ style::ConvertScale(4)
			+ int(_lines.size()) * st::normalFont->height
			+ style::ConvertScale(8)
			+ style::ConvertScale(6);
		if (height() != wanted) {
			resize(width(), wanted);
		}
	}

	StatusData _data;
	QStringList _lines;

};

// One account: a state chip, the name, one line of what it does or why it
// stopped, and its counters on the right.
class AccountRow final : public Ui::RpWidget {
public:
	explicit AccountRow(not_null<QWidget*> parent)
	: RpWidget(parent.get()) {
		resize(width(), style::ConvertScale(44));
		widthValue(
		) | rpl::on_next([=](int) {
			refit();
		}, lifetime());
	}

	void setClickedCallback(Fn<void()> callback) {
		_clicked = std::move(callback);
		setCursor(_clicked ? style::cur_pointer : style::cur_default);
	}

	void setData(const SmsModel::Account &account, const QString &live) {
		_account = account;
		_live = live;
		refit();
		update();
	}

protected:
	void mousePressEvent(QMouseEvent *e) override {
		_pressed = (e->button() == Qt::LeftButton);
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
		const auto pad = style::ConvertScale(12);
		const auto chipText = SmsModel::StateTitle(_account.state);
		const auto color = stateColor();
		const auto chipH = st::normalFont->height + style::ConvertScale(4);
		const auto chip = QRect(
			pad,
			(std::max(height(), style::ConvertScale(44)) - chipH) / 2,
			chipWidth(),
			chipH);
		p.setPen(Qt::NoPen);
		p.setBrush(Tinted(color, 0.16));
		p.drawRoundedRect(chip, chipH / 2., chipH / 2.);
		p.setFont(st::normalFont);
		p.setPen(color);
		p.drawText(chip, Qt::AlignCenter, chipText);

		const auto counters = SmsModel::AccountCounters(_account);
		const auto countersW = st::normalFont->width(counters);
		const auto right = width() - pad;
		p.setPen(st::windowSubTextFg);
		p.drawText(
			QRect(right - countersW, 0, countersW, chip.bottom() + chip.top()),
			Qt::AlignRight | Qt::AlignVCenter,
			counters);

		const auto left = textLeft();
		const auto available = textWidth();
		const auto nameH = st::semiboldFont->height;
		const auto infoH = _account.info.isEmpty()
			? 0
			: st::normalFont->height;
		auto y = (height() - nameH - infoH
			- int(_lines.size()) * st::normalFont->height) / 2;
		p.setFont(st::semiboldFont);
		p.setPen(st::windowBoldFg);
		p.drawText(
			left,
			y + st::semiboldFont->ascent,
			st::semiboldFont->elided(_account.label, available));
		y += nameH;
		p.setFont(st::normalFont);
		p.setPen(st::windowSubTextFg);
		if (infoH) {
			p.drawText(
				left,
				y + st::normalFont->ascent,
				st::normalFont->elided(_account.info, available));
			y += infoH;
		}
		for (const auto &line : _lines) {
			p.drawText(left, y + st::normalFont->ascent, line);
			y += st::normalFont->height;
		}
	}

private:
	[[nodiscard]] int chipWidth() const {
		return std::max(
			st::normalFont->width(SmsModel::StateTitle(_account.state))
				+ 2 * style::ConvertScale(8),
			style::ConvertScale(84));
	}
	[[nodiscard]] int textLeft() const {
		return style::ConvertScale(12)
			+ chipWidth()
			+ style::ConvertScale(10);
	}
	[[nodiscard]] int textWidth() const {
		const auto countersW = st::normalFont->width(
			SmsModel::AccountCounters(_account));
		return std::max(
			width() - style::ConvertScale(12) - countersW
				- style::ConvertScale(12) - textLeft(),
			1);
	}
	// The reason an account was taken out matters: it wraps (two lines at
	// most) instead of being cut short.
	void refit() {
		_lines.clear();
		const auto detail = !_live.isEmpty() ? _live : _account.note;
		if (!detail.isEmpty() && width() > 0) {
			_lines = Ggtg::WrapText(st::normalFont, detail, textWidth());
			if (_lines.size() > 2) {
				_lines = _lines.mid(0, 2);
				_lines.back() = st::normalFont->elided(
					_lines.back() + u"…"_q,
					textWidth());
			}
		}
		const auto wanted = std::max(
			style::ConvertScale(44),
			st::semiboldFont->height
				+ (_account.info.isEmpty() ? 0 : st::normalFont->height)
				+ int(_lines.size()) * st::normalFont->height
				+ 2 * style::ConvertScale(8));
		if (height() != wanted) {
			resize(width(), wanted);
		}
	}

	[[nodiscard]] QColor stateColor() const {
		switch (_account.state) {
		case SmsModel::AccountState::Working:
			return st::windowActiveTextFg->c;
		case SmsModel::AccountState::Done:
			return OutcomeColor(Outcome::Sent);
		case SmsModel::AccountState::Retired:
			return OutcomeColor(Outcome::Peer);
		case SmsModel::AccountState::SpamBlocked:
			return OutcomeColor(Outcome::Spam);
		case SmsModel::AccountState::Waiting:
			return st::windowSubTextFg->c;
		}
		return st::windowSubTextFg->c;
	}

	SmsModel::Account _account;
	QString _live;
	QStringList _lines;
	Fn<void()> _clicked;
	bool _pressed = false;

};

// One recipient line, or a folded run: "Отказ Telegram × 14 подряд · Daniel".
// The owner sets a click callback on folded rows to expand them.
class RecipientRow final : public Ui::AbstractButton {
public:
	explicit RecipientRow(not_null<QWidget*> parent)
	: AbstractButton(parent.get()) {
		resize(width(), style::ConvertScale(34));
	}

	void setData(
			const SmsModel::DisplayRow &row,
			const std::vector<SmsModel::Recipient> &recipients,
			bool expanded) {
		_row = row;
		const auto folded = (row.indices.size() > 1);
		setCursor(folded ? style::cur_pointer : style::cur_default);
		const auto &first = recipients[row.indices.front()];
		if (folded) {
			_title = SmsModel::Title(*row.outcome)
				+ u" × "_q + QString::number(row.indices.size())
				+ u" подряд"_q;
			_detail = row.account + (expanded ? u"  ▴"_q : u"  ▾"_q);
		} else {
			_title = '@' + first.username;
			auto parts = QStringList();
			if (row.inProgress) {
				parts.push_back(u"в работе"_q);
			} else if (row.outcome && *row.outcome != Outcome::Sent) {
				parts.push_back(first.reason.isEmpty()
					? SmsModel::InlineTitle(*row.outcome)
					: first.reason);
			}
			if (!first.account.isEmpty()) {
				parts.push_back(first.account);
			}
			_detail = parts.join(u" · "_q);
		}
		update();
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		auto p = QPainter(this);
		auto hq = PainterHighQualityEnabler(p);
		const auto folded = (_row.indices.size() > 1);
		const auto radius = style::ConvertScale(8);
		if (folded) {
			p.setPen(Qt::NoPen);
			p.setBrush(Tinted(OutcomeColor(*_row.outcome), 0.1));
			p.drawRoundedRect(rect(), radius, radius);
		} else if (isOver()) {
			p.setPen(Qt::NoPen);
			p.setBrush(st::windowBgOver);
			p.drawRoundedRect(rect(), radius, radius);
		}
		const auto pad = style::ConvertScale(10);
		const auto icon = style::ConvertScale(16);
		PaintOutcomeIcon(
			p,
			QRect(pad, (height() - icon) / 2, icon, icon),
			_row.inProgress ? std::nullopt : _row.outcome);
		const auto left = pad + icon + style::ConvertScale(10);
		const auto detailW = std::min(
			st::normalFont->width(_detail),
			(width() - left - pad) / 2);
		p.setFont(st::normalFont);
		p.setPen(folded
			? OutcomeColor(*_row.outcome)
			: st::windowSubTextFg->c);
		p.drawText(
			QRect(width() - pad - detailW, 0, detailW, height()),
			Qt::AlignRight | Qt::AlignVCenter,
			st::normalFont->elided(_detail, detailW));
		const auto &font = folded ? st::semiboldFont : st::normalFont;
		p.setFont(font);
		p.setPen(folded ? OutcomeColor(*_row.outcome) : st::windowFg->c);
		const auto titleW = std::max(width() - left - detailW - 2 * pad, 1);
		p.drawText(
			QRect(left, 0, titleW, height()),
			Qt::AlignLeft | Qt::AlignVCenter,
			font->elided(_title, titleW));
	}

private:
	SmsModel::DisplayRow _row;
	QString _title;
	QString _detail;

};

// A step of the "Новая" form: a header with the step's title and a one-line
// summary ("Аккаунты: 23 · 2 в спам-блоке"); a click folds the body away.
// A row of quiet text links ("Выбрать все   Снять   Проверить статус"):
// small, rarely used actions that don't each deserve a full-width button.
// Wraps onto a new line when the links don't fit.
class LinksRow final : public Ui::RpWidget {
public:
	explicit LinksRow(not_null<QWidget*> parent)
	: RpWidget(parent.get()) {
		widthValue(
		) | rpl::on_next([=](int) {
			relayout();
		}, lifetime());
	}

	not_null<Ui::LinkButton*> add(const QString &text, Fn<void()> callback) {
		const auto link = Ui::CreateChild<Ui::LinkButton>(this, text);
		link->setClickedCallback(std::move(callback));
		link->show();
		_links.push_back(link);
		relayout();
		return link;
	}

	// After a link's text or visibility changed.
	void relayout() {
		const auto gap = style::ConvertScale(18);
		const auto available = std::max(width(), 1);
		auto x = 0;
		auto y = 0;
		auto line = 0;
		for (const auto &link : _links) {
			if (link->isHidden()) {
				continue;
			}
			if (x > 0 && x + link->width() > available) {
				x = 0;
				y += line + style::ConvertScale(6);
				line = 0;
			}
			link->moveToLeft(x, y);
			x += link->width() + gap;
			line = std::max(line, link->height());
		}
		const auto height = y + line;
		if (this->height() != height) {
			resize(width(), height);
		}
	}

private:
	std::vector<not_null<Ui::LinkButton*>> _links;

};

class StepCard final : public Ui::RpWidget {
public:
	StepCard(
		not_null<QWidget*> parent,
		const QString &title,
		rpl::producer<QString> summary,
		bool expanded,
		int number = 0)
	: RpWidget(parent.get())
	, _layout(this)
	, _header(_layout->add(object_ptr<Header>(
		_layout.data(),
		title,
		number)))
	, _body(_layout->add(object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
		_layout.data(),
		object_ptr<Ui::VerticalLayout>(_layout.data())))) {
		_expanded = expanded;
		_body->toggle(expanded, anim::type::instant);
		_header->setExpanded(expanded);
		std::move(summary) | rpl::on_next([=](QString text) {
			_header->setSummary(text);
		}, lifetime());
		_header->setClickedCallback([=] {
			_expanded = !_expanded.current();
			_body->toggle(_expanded.current(), anim::type::normal);
			_header->setExpanded(_expanded.current());
		});
		widthValue(
		) | rpl::on_next([=](int width) {
			_layout->resizeToWidth(width);
		}, lifetime());
		_layout->heightValue(
		) | rpl::on_next([=](int height) {
			resize(width(), height);
		}, lifetime());
	}

	[[nodiscard]] not_null<Ui::VerticalLayout*> content() const {
		return _body->entity();
	}
	[[nodiscard]] rpl::producer<bool> expandedValue() const {
		return _expanded.value();
	}

protected:
	// A step is a card of its own: a thin frame and a soft fill set it off
	// from the next one.
	void paintEvent(QPaintEvent *e) override {
		auto p = QPainter(this);
		auto hq = PainterHighQualityEnabler(p);
		const auto radius = style::ConvertScale(12);
		auto fill = st::windowBoldFg->c;
		fill.setAlphaF(0.035);
		auto line = st::windowBoldFg->c;
		line.setAlphaF(0.12);
		p.setPen(QPen(line, style::ConvertScale(1)));
		p.setBrush(fill);
		p.drawRoundedRect(
			QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5),
			radius,
			radius);
	}

private:
	class Header final : public Ui::AbstractButton {
	public:
		Header(
			not_null<QWidget*> parent,
			const QString &title,
			int number)
		: AbstractButton(parent.get())
		, _title(title)
		, _number(number) {
			resize(width(), style::ConvertScale(60));
		}
		void setSummary(const QString &text) {
			_summary = text;
			update();
		}
		void setExpanded(bool expanded) {
			_expandedState = expanded;
			update();
		}

	protected:
		void paintEvent(QPaintEvent *e) override {
			auto p = QPainter(this);
			auto hq = PainterHighQualityEnabler(p);
			if (isOver()) {
				p.setPen(Qt::NoPen);
				p.setBrush(st::windowBgOver);
				p.drawRoundedRect(
					rect(),
					style::ConvertScale(10),
					style::ConvertScale(10));
			}
			const auto pad = st::boxRowPadding.left();
			// The number of the step in a round accent badge.
			const auto badge = style::ConvertScale(30);
			auto textLeft = pad;
			if (_number > 0) {
				const auto badgeRect = QRect(
					pad - style::ConvertScale(4),
					(height() - badge) / 2,
					badge,
					badge);
				p.setPen(Qt::NoPen);
				p.setBrush(st::windowBgActive);
				p.drawEllipse(badgeRect);
				p.setFont(st::semiboldFont);
				p.setPen(st::windowFgActive);
				p.drawText(
					badgeRect,
					Qt::AlignCenter,
					QString::number(_number));
				textLeft = badgeRect.right() + style::ConvertScale(12);
			}
			const auto arrow = _expandedState ? u"▴"_q : u"▾"_q;
			const auto arrowW = st::normalFont->width(arrow);
			p.setFont(st::normalFont);
			p.setPen(st::windowSubTextFg);
			p.drawText(
				QRect(width() - pad - arrowW, 0, arrowW, height()),
				Qt::AlignCenter,
				arrow);
			const auto available = std::max(
				width() - textLeft - 2 * pad - arrowW,
				1);
			const auto top = (height()
				- st::semiboldFont->height
				- st::normalFont->height) / 2;
			p.setFont(st::semiboldFont);
			p.setPen(st::windowBoldFg);
			p.drawText(
				textLeft,
				top + st::semiboldFont->ascent,
				st::semiboldFont->elided(_title.toUpper(), available));
			p.setFont(st::normalFont);
			p.setPen(st::windowSubTextFg);
			p.drawText(
				textLeft,
				top + st::semiboldFont->height + st::normalFont->ascent,
				st::normalFont->elided(_summary, available));
		}

	private:
		QString _title;
		QString _summary;
		int _number = 0;
		bool _expandedState = false;

	};

	object_ptr<Ui::VerticalLayout> _layout;
	not_null<Header*> _header;
	not_null<Ui::SlideWrap<Ui::VerticalLayout>*> _body;
	rpl::variable<bool> _expanded = false;

};

} // namespace Settings::SmsWidgets
