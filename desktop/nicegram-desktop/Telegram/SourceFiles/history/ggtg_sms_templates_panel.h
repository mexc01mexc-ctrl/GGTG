/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "ui/rp_widget.h"
#include "ui/widgets/scroll_area.h"
#include "ui/wrap/vertical_layout.h"
#include "ui/wrap/vertical_layout_reorder.h"
#include "ui/widgets/popup_menu.h"
#include "data/sms_prompts.h"
#include "base/timer.h"
#include "base/algorithm.h"
#include "base/event_filter.h"
#include "styles/style_widgets.h"
#include "styles/style_chat_helpers.h"
#include "styles/style_menu_icons.h"

#include <QtGui/QPainter>
#include <QtGui/QMouseEvent>
#include <QtGui/QKeyEvent>
#include <QtGui/QGuiApplication>
#include <QtWidgets/QApplication>

namespace HistoryView {

class PromptRowHandle final : public Ui::RpWidget {
public:
	PromptRowHandle(
		QWidget *parent,
		Fn<void(bool pressed)> onPressed)
	: RpWidget(parent)
	, _onPressed(std::move(onPressed)) {
		setCursor(Qt::OpenHandCursor);
		setToolTip(u"Перетащите для изменения порядка"_q);
		setMouseTracking(true);
	}

	~PromptRowHandle() {
		restoreCursor();
	}

	void setRowHovered(bool hovered) {
		if (_rowHovered != hovered) {
			_rowHovered = hovered;
			update();
		}
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		auto p = QPainter(this);
		if (_rowHovered || _hovered || _pressed) {
			p.fillRect(rect(), st::defaultMenu.itemBgOver);
		}
		p.setRenderHint(QPainter::Antialiasing);
		p.setPen(Qt::NoPen);

		const auto stripeFg = (_hovered || _pressed)
			? st::menuIconFgOver
			: (_rowHovered ? st::menuIconFgOver : st::menuIconFg);
		p.setBrush(stripeFg);

		// Exactly 3 horizontal bars for reordering
		const auto lineWidth = style::ConvertScale(14);
		const auto lineHeight = style::ConvertScale(2);
		const auto lineGap = style::ConvertScale(2);
		const auto totalH = 3 * lineHeight + 2 * lineGap;
		const auto startX = (width() - lineWidth) / 2;
		const auto startY = (height() - totalH) / 2;
		const auto radius = style::ConvertScale(1);

		for (int i = 0; i < 3; ++i) {
			const auto curY = startY + i * (lineHeight + lineGap);
			p.drawRoundedRect(
				QRect(startX, curY, lineWidth, lineHeight),
				radius,
				radius);
		}
	}

	void enterEventHook(QEnterEvent *e) override {
		_hovered = true;
		if (!_pressed) {
			setCursor(Qt::OpenHandCursor);
		}
		update();
	}

	void leaveEventHook(QEvent *e) override {
		_hovered = false;
		update();
	}

	void mousePressEvent(QMouseEvent *e) override {
		if (e->button() == Qt::LeftButton) {
			_pressed = true;
			if (!_cursorOverridden) {
				_cursorOverridden = true;
				QGuiApplication::setOverrideCursor(Qt::ClosedHandCursor);
			}
			if (_onPressed) {
				_onPressed(true);
			}
			update();
			e->accept();
		} else if (e->button() == Qt::RightButton) {
			// Allow right-click context menu to bubble to row
			e->ignore();
		}
	}

	void mouseReleaseEvent(QMouseEvent *e) override {
		if (e->button() == Qt::LeftButton) {
			_pressed = false;
			restoreCursor();
			if (_onPressed) {
				_onPressed(false);
			}
			update();
			e->accept();
		}
	}

	void hideEvent(QHideEvent *e) override {
		RpWidget::hideEvent(e);
		restoreCursor();
	}

private:
	void restoreCursor() {
		if (_cursorOverridden) {
			_cursorOverridden = false;
			QGuiApplication::restoreOverrideCursor();
		}
		if (_pressed) {
			_pressed = false;
			if (_onPressed) {
				_onPressed(false);
			}
		}
	}

	Fn<void(bool pressed)> _onPressed;
	bool _rowHovered = false;
	bool _hovered = false;
	bool _pressed = false;
	bool _cursorOverridden = false;
};

class PromptRow final : public Ui::RpWidget {
public:
	PromptRow(
		QWidget *parent,
		const Data::SmsPrompt &prompt,
		Fn<void(const QString &text)> onSelect,
		Fn<void(const Data::SmsPrompt &prompt, QPoint globalPos)> onContextMenu,
		Fn<void(bool pressed)> onHandlePressed)
	: RpWidget(parent)
	, _prompt(prompt)
	, _onSelect(std::move(onSelect))
	, _onContextMenu(std::move(onContextMenu)) {
		const auto rowHeight = style::ConvertScale(34);
		resize(parent ? parent->width() : style::ConvertScale(300), rowHeight);
		setMouseTracking(true);
		setCursor(Qt::PointingHandCursor);

		_handle = Ui::CreateChild<PromptRowHandle>(this, std::move(onHandlePressed));
		const auto handleWidth = style::ConvertScale(32);
		_handle->resize(handleWidth, rowHeight);
		_handle->show();
	}

	int resizeGetHeight(int newWidth) override {
		return style::ConvertScale(34);
	}

	[[nodiscard]] not_null<Ui::RpWidget*> handle() const {
		return _handle;
	}

	[[nodiscard]] const Data::SmsPrompt &prompt() const {
		return _prompt;
	}

protected:
	void resizeEvent(QResizeEvent *e) override {
		RpWidget::resizeEvent(e);
		if (_handle) {
			_handle->moveToRight(0, 0, width());
		}
	}

	void paintEvent(QPaintEvent *e) override {
		auto p = QPainter(this);
		if (_hovered) {
			p.fillRect(rect(), st::defaultMenu.itemBgOver);
		}
		p.setFont(st::defaultMenu.itemStyle.font);
		p.setPen(_hovered ? st::defaultMenu.itemFgOver : st::defaultMenu.itemFg);

		const auto padLeft = style::ConvertScale(12);
		const auto padRight = _handle
			? _handle->width() + style::ConvertScale(4)
			: style::ConvertScale(36);
		const auto textWidth = width() - padLeft - padRight;
		if (textWidth > 0) {
			const auto elided = st::defaultMenu.itemStyle.font->elided(
				_prompt.name,
				textWidth);
			const auto textRect = QRect(
				padLeft,
				0,
				textWidth,
				height());
			p.drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, elided);
		}
	}

	void enterEventHook(QEnterEvent *e) override {
		_hovered = true;
		update();
		if (_handle) _handle->setRowHovered(true);
	}

	void leaveEventHook(QEvent *e) override {
		if (!rect().contains(mapFromGlobal(QCursor::pos()))) {
			_hovered = false;
			update();
			if (_handle) _handle->setRowHovered(false);
		}
	}

	void mousePressEvent(QMouseEvent *e) override {
		if (_handle && _handle->geometry().contains(e->pos())) {
			e->accept();
			return;
		}
		if (e->button() == Qt::LeftButton) {
			_pressed = true;
			e->accept();
		} else if (e->button() == Qt::RightButton) {
			if (_onContextMenu) {
				_onContextMenu(_prompt, e->globalPos());
			}
			e->accept();
		}
	}

	void mouseReleaseEvent(QMouseEvent *e) override {
		if (_handle && _handle->geometry().contains(e->pos())) {
			e->accept();
			return;
		}
		if (e->button() == Qt::LeftButton && _pressed) {
			_pressed = false;
			if (rect().contains(e->pos())) {
				if (_onSelect) {
					_onSelect(_prompt.text);
				}
			}
			e->accept();
		}
	}

	void contextMenuEvent(QContextMenuEvent *e) override {
		if (_onContextMenu) {
			_onContextMenu(_prompt, e->globalPos());
		}
	}

private:
	Data::SmsPrompt _prompt;
	Fn<void(const QString &text)> _onSelect;
	Fn<void(const Data::SmsPrompt &prompt, QPoint globalPos)> _onContextMenu;
	PromptRowHandle *_handle = nullptr;
	bool _hovered = false;
	bool _pressed = false;
};

class SmsTemplatesPanel final : public Ui::RpWidget {
public:
	SmsTemplatesPanel(
		QWidget *parent,
		QWidget *toggleButton,
		Fn<void(const QString &text)> onSelect,
		Fn<void(const Data::SmsPrompt &prompt, QPoint globalPos)> onContextMenu,
		Fn<void()> onSaveField)
	: RpWidget(parent)
	, _toggleButton(toggleButton)
	, _onSelect(std::move(onSelect))
	, _onContextMenu(std::move(onContextMenu))
	, _onSaveField(std::move(onSaveField)) {
		hide();
		setMouseTracking(true);

		_hideTimer.setCallback([=] {
			if (_reordering == 0
				&& !_handlePressed
				&& !underMouse()
				&& (!_toggleButton || !_toggleButton->underMouse())) {
				hidePanel();
			}
		});

		rebuild();
	}

	void rebuild() {
		_reorder = nullptr;
		_rows.clear();
		_prompts = Data::LoadSmsPrompts();

		const auto width = style::ConvertScale(320);
		const auto rowHeight = style::ConvertScale(34);
		const auto maxListHeight = style::ConvertScale(280);
		const auto padding = style::ConvertScale(6);
		const auto bottomHeight = style::ConvertScale(36);
		const auto contentWidth = width - padding * 2;

		if (!_scroll) {
			_scroll = Ui::CreateChild<Ui::ScrollArea>(
				this,
				st::defaultScrollArea);
			_scroll->setFrameShape(QFrame::NoFrame);
		}
		_promptsLayout = _scroll->setOwnedWidget(
			object_ptr<Ui::VerticalLayout>(_scroll));

		if (_prompts.empty()) {
			const auto emptyWrap = _promptsLayout->add(
				object_ptr<Ui::RpWidget>(_promptsLayout),
				style::al_justify);
			emptyWrap->resize(contentWidth, rowHeight);
			emptyWrap->paintRequest(
			) | rpl::on_next([=] {
				auto p = QPainter(emptyWrap);
				p.setFont(st::defaultMenu.itemStyle.font);
				p.setPen(st::defaultMenu.itemFgDisabled);
				p.drawText(
					emptyWrap->rect(),
					Qt::AlignCenter,
					u"Шаблонов пока нет"_q);
			}, emptyWrap->lifetime());
		} else {
			for (const auto &prompt : _prompts) {
				const auto row = _promptsLayout->add(
					object_ptr<PromptRow>(
						_promptsLayout,
						prompt,
						[=](const QString &text) {
							hidePanel();
							if (_onSelect) {
								_onSelect(text);
							}
						},
						[=](const Data::SmsPrompt &p, QPoint pos) {
							if (_onContextMenu) {
								_onContextMenu(p, pos);
							}
						},
						[=](bool pressed) {
							_handlePressed = pressed;
							if (pressed) {
								_hideTimer.cancel();
							}
						}),
					style::al_justify);
				row->show();
				_rows.push_back(row);
			}
		}

		_promptsLayout->resizeToWidth(contentWidth);

		const auto listHeight = _prompts.empty()
			? rowHeight
			: std::min(_promptsLayout->height(), maxListHeight);

		_scroll->setGeometry(
			padding,
			padding,
			contentWidth,
			listHeight);
		_scroll->show();
		_promptsLayout->show();

		if (!_bottomWrap) {
			_bottomWrap = Ui::CreateChild<Ui::RpWidget>(this);
			_bottomWrap->setMouseTracking(true);

			_bottomWrap->paintRequest(
			) | rpl::on_next([=] {
				auto p = QPainter(_bottomWrap);
				// 1px top border
				p.fillRect(
					0,
					0,
					_bottomWrap->width(),
					1,
					st::windowBgOver);
				if (_bottomHovered) {
					p.fillRect(
						0,
						1,
						_bottomWrap->width(),
						_bottomWrap->height() - 1,
						st::defaultMenu.itemBgOver);
				}
				p.setFont(st::defaultMenu.itemStyle.font);
				p.setPen(_bottomHovered ? st::defaultMenu.itemFgOver : st::defaultMenu.itemFg);
				const auto textRect = QRect(
					style::ConvertScale(12),
					0,
					_bottomWrap->width() - style::ConvertScale(24),
					_bottomWrap->height());
				p.drawText(
					textRect,
					Qt::AlignLeft | Qt::AlignVCenter,
					u"Сохранить текст поля как шаблон..."_q);
			}, _bottomWrap->lifetime());

			_bottomWrap->events(
			) | rpl::on_next([=](not_null<QEvent*> e) {
				if (e->type() == QEvent::Enter) {
					_bottomHovered = true;
					_bottomWrap->update();
				} else if (e->type() == QEvent::Leave) {
					_bottomHovered = false;
					_bottomWrap->update();
				} else if (e->type() == QEvent::MouseButtonPress) {
					if (static_cast<QMouseEvent*>(e.get())->button() == Qt::LeftButton) {
						hidePanel();
						if (_onSaveField) {
							_onSaveField();
						}
					}
				}
			}, _bottomWrap->lifetime());
		}

		_bottomWrap->setGeometry(
			0,
			padding + listHeight,
			width,
			bottomHeight);
		_bottomWrap->show();

		const auto totalHeight = padding + listHeight + bottomHeight + padding;
		resize(width, totalHeight);

		setupReorder();
	}

	void setupReorder() {
		if (_rows.size() < 2) {
			return;
		}
		_reorder = std::make_unique<Ui::VerticalLayoutReorder>(
			_promptsLayout,
			_scroll);
		_reorder->setMouseEventProxy([=](int i) -> not_null<Ui::RpWidget*> {
			if (i >= 0 && i < int(_rows.size())) {
				return _rows[i]->handle();
			}
			return _promptsLayout->widgetAt(i);
		});
		_reorder->updates(
		) | rpl::on_next([=](Ui::VerticalLayoutReorder::Single data) {
			using State = Ui::VerticalLayoutReorder::State;
			if (data.state == State::Started) {
				++_reordering;
				_hideTimer.cancel();
			} else {
				Ui::PostponeCall(_promptsLayout, [=] {
					--_reordering;
				});
				if (data.state == State::Applied) {
					base::reorder(
						_prompts,
						data.oldPosition,
						data.newPosition);
					base::reorder(
						_rows,
						data.oldPosition,
						data.newPosition);
					Data::SaveAllSmsPrompts(_prompts);
				}
			}
		}, lifetime());
		_reorder->start();
	}

	void showPanel() {
		rebuild();
		raise();
		show();
		if (_scroll) _scroll->show();
		if (_promptsLayout) _promptsLayout->show();
		if (_bottomWrap) _bottomWrap->show();
		_hideTimer.cancel();
	}

	void hidePanel() {
		if (_reordering > 0 || _handlePressed) {
			return;
		}
		hide();
	}

	void startHideTimer() {
		_hideTimer.callOnce(300);
	}

	void cancelHideTimer() {
		_hideTimer.cancel();
	}

	[[nodiscard]] bool isReordering() const {
		return _reordering > 0 || _handlePressed;
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		auto p = QPainter(this);
		p.setRenderHint(QPainter::Antialiasing);
		const auto radius = style::ConvertScale(8);

		p.setPen(st::windowBgOver);
		p.setBrush(st::defaultMenu.itemBg);
		p.drawRoundedRect(rect().adjusted(0, 0, -1, -1), radius, radius);
	}

	void enterEventHook(QEnterEvent *e) override {
		cancelHideTimer();
	}

	void leaveEventHook(QEvent *e) override {
		if (_reordering == 0 && !_handlePressed) {
			startHideTimer();
		}
	}

	void showEvent(QShowEvent *e) override {
		RpWidget::showEvent(e);
		_clickFilterLifetime.destroy();
		base::install_event_filter(qApp, [=](not_null<QEvent*> ev) {
			if (ev->type() == QEvent::MouseButtonPress) {
				const auto me = static_cast<QMouseEvent*>(ev.get());
				const auto gPos = me->globalPos();
				const auto pRect = QRect(mapToGlobal(QPoint(0, 0)), size());
				const auto tRect = _toggleButton
					? QRect(_toggleButton->mapToGlobal(QPoint(0, 0)), _toggleButton->size())
					: QRect();
				if (!pRect.contains(gPos) && !tRect.contains(gPos)) {
					Ui::PostponeCall(this, [=] { hidePanel(); });
				}
			} else if (ev->type() == QEvent::KeyPress) {
				const auto ke = static_cast<QKeyEvent*>(ev.get());
				if (ke->key() == Qt::Key_Escape) {
					hidePanel();
					return base::EventFilterResult::Cancel;
				}
			}
			return base::EventFilterResult::Continue;
		}, _clickFilterLifetime);
	}

	void hideEvent(QHideEvent *e) override {
		RpWidget::hideEvent(e);
		_clickFilterLifetime.destroy();
	}

private:
	QWidget *_toggleButton = nullptr;
	Fn<void(const QString &text)> _onSelect;
	Fn<void(const Data::SmsPrompt &prompt, QPoint globalPos)> _onContextMenu;
	Fn<void()> _onSaveField;

	std::vector<Data::SmsPrompt> _prompts;
	std::vector<PromptRow*> _rows;

	Ui::ScrollArea *_scroll = nullptr;
	Ui::VerticalLayout *_promptsLayout = nullptr;
	Ui::RpWidget *_bottomWrap = nullptr;
	bool _bottomHovered = false;

	std::unique_ptr<Ui::VerticalLayoutReorder> _reorder;
	int _reordering = 0;
	bool _handlePressed = false;

	base::Timer _hideTimer;
	rpl::lifetime _clickFilterLifetime;
};

} // namespace HistoryView
