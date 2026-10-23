/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "settings/process_box.h"

#include "base/flat_set.h"
#include "base/timer.h"
#include "base/unixtime.h"
#include "core/application.h"
#include "core/ggtg_ai.h"
#include "core/ggtg_log.h"
#include "window/ggtg_widgets.h"
#include "styles/style_menu_icons.h"
#include <QtCore/QDateTime>
#include <QtCore/QFileInfo>
#include <QtCore/QUrl>
#include <QtGui/QDesktopServices>
#include "core/process_manager.h"
#include "ui/abstract_button.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"
#include "ui/ui_utility.h"
#include "styles/palette.h"
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"

namespace Settings {
namespace {

[[nodiscard]] QString ProgressText(const Core::ProcessSnapshot &process) {
	switch (process.status) {
	case Core::ProcessStatus::Succeeded:
		return u"Готово."_q;
	case Core::ProcessStatus::Failed:
		return u"Завершено с ошибкой."_q;
	case Core::ProcessStatus::Running:
		break;
	}
	return (process.progressTotal > 0)
		? u"Выполняется: %1 из %2"_q
			.arg(process.progressCurrent)
			.arg(process.progressTotal)
		: u"Выполняется..."_q;
}

[[nodiscard]] QColor ProcessColor(Core::ProcessStatus status) {
	switch (status) {
	case Core::ProcessStatus::Succeeded: return QColor(76, 217, 100);
	case Core::ProcessStatus::Failed: return QColor(255, 69, 58);
	case Core::ProcessStatus::Running: break;
	}
	return QColor(56, 189, 248);
}

// "Сводка ИИ": the model reads the end of a process's log and says in a few
// sentences what happened, what went wrong and what to do about it.
void ShowAiSummaryBox(
		std::shared_ptr<Ui::Show> show,
		const QString &title,
		const QStringList &lines) {
	show->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"Сводка ИИ"_q));
		box->setWidth(st::boxWideWidth);
		const auto label = box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			u"Читаю журнал..."_q,
			st::boxLabel));
		box->addButton(rpl::single(u"Закрыть"_q), [=] {
			box->closeBox();
		});
		// The last stretch of the log is what matters, and the fast models
		// that take bulk jobs have the biggest limits for short questions.
		auto text = lines.mid(std::max(int(lines.size()) - 150, 0)).join('\n');
		if (text.size() > 6000) {
			text = text.right(6000);
		}
		const auto weak = QPointer<Ui::FlatLabel>(label);
		Core::GgtgAi::Ask(
			u"Ты разбираешь журнал массовой операции программы GGTG "
			"(рассылка сообщений, массовые действия, сбор баз). По журналу "
			"коротко, по-русски и по делу ответь: что произошло, чем "
			"закончилось, какие были ошибки и что делать дальше. "
			"Не больше 8 строк, без вступлений. Ничего не выдумывай - только "
			"то, что есть в журнале."_q,
			u"Операция: "_q + title + u"\n\nЖурнал:\n"_q + text,
			crl::guard(box, [=](QString result, QString error) {
				if (const auto strong = weak.data()) {
					strong->setText(error.isEmpty()
						? result
						: (u"Не вышло: "_q + error));
				}
			}),
			0.3);
	}));
}

// One card's clickable header: a status glyph, the title, a colored status
// line and a chevron; expands/collapses the log below it.
class ProcessHeader final : public Ui::AbstractButton {
public:
	ProcessHeader(
		not_null<QWidget*> parent,
		const Core::ProcessSnapshot &process,
		bool expanded,
		Fn<void()> onToggle,
		Fn<void()> onReopen)
	: AbstractButton(parent.get())
	, _title(process.title)
	, _color(ProcessColor(process.status))
	, _expanded(expanded) {
		resize(width(), style::ConvertScale(52));
		setClickedCallback(std::move(onToggle));
		switch (process.status) {
		case Core::ProcessStatus::Running: _glyph = u"↻"_q; break;
		case Core::ProcessStatus::Succeeded: _glyph = u"✓"_q; break;
		case Core::ProcessStatus::Failed: _glyph = u"✗"_q; break;
		}
		const auto startedAt = process.startedAt
			? base::unixtime::parse(process.startedAt)
			: QDateTime();
		const auto started = process.startedAt
			? startedAt.toString(startedAt.date() == QDate::currentDate()
				? u"HH:mm:ss"_q
				: u"dd.MM HH:mm"_q)
			: QString();
		const auto since = started.isEmpty()
			? QString()
			: (u" · начато "_q + started);
		_subtitle = (process.status == Core::ProcessStatus::Running)
			? ((process.progressTotal > 0)
				? (u"Обработано: %1 / %2"_q
					.arg(process.progressCurrent)
					.arg(process.progressTotal) + since)
				: (u"Выполняется..."_q + since))
			: (process.status == Core::ProcessStatus::Succeeded)
			? (u"Готово"_q + since)
			: (u"Завершено с ошибкой"_q + since);
		if (onReopen) {
			_reopen = Ui::CreateChild<Ui::LinkButton>(this, u"Открыть"_q);
			_reopen->setClickedCallback(std::move(onReopen));
		}
		// The header is as tall as its wrapped title / status need.
		widthValue(
		) | rpl::on_next([=](int) {
			refit();
		}, lifetime());
	}

protected:
	void resizeEvent(QResizeEvent *e) override {
		if (_reopen) {
			_reopen->moveToRight(
				style::ConvertScale(40),
				(height() - _reopen->height()) / 2,
				width());
		}
	}

	void paintEvent(QPaintEvent *e) override {
		auto p = QPainter(this);
		auto hq = PainterHighQualityEnabler(p);
		auto fill = st::windowBoldFg->c;
		fill.setAlphaF(isOver() ? 0.12 : 0.07);
		p.setPen(Qt::NoPen);
		p.setBrush(fill);
		const auto radius = style::ConvertScale(8);
		p.drawRoundedRect(rect(), radius, radius);

		const auto pad = style::ConvertScale(14);
		const auto glyphWidth = style::ConvertScale(22);
		p.setFont(st::semiboldFont);
		p.setPen(_color);
		p.drawText(
			QRect(pad, 0, glyphWidth, height()),
			Qt::AlignCenter,
			_glyph);
		const auto textLeft = pad + glyphWidth + pad / 2;
		const auto textWidth = textAreaWidth();
		const auto titleLines = Ggtg::WrapText(
			st::semiboldFont,
			_title,
			textWidth);
		const auto subLines = Ggtg::WrapText(
			st::normalFont,
			_subtitle,
			textWidth);
		auto top = (height()
			- st::semiboldFont->height * int(titleLines.size())
			- st::normalFont->height * int(subLines.size())) / 2;
		p.setFont(st::semiboldFont);
		p.setPen(st::windowBoldFg);
		for (const auto &line : titleLines) {
			p.drawText(textLeft, top + st::semiboldFont->ascent, line);
			top += st::semiboldFont->height;
		}
		p.setFont(st::normalFont);
		p.setPen(_color);
		for (const auto &line : subLines) {
			p.drawText(textLeft, top + st::normalFont->ascent, line);
			top += st::normalFont->height;
		}
		p.setFont(st::semiboldFont);
		p.setPen(st::windowSubTextFg);
		p.drawText(
			QRect(width() - pad - glyphWidth, 0, glyphWidth, height()),
			Qt::AlignCenter,
			_expanded ? u"▾"_q : u"▸"_q);
	}

private:
	[[nodiscard]] int textAreaWidth() const {
		const auto pad = style::ConvertScale(14);
		const auto textLeft = pad + style::ConvertScale(22) + pad / 2;
		const auto reserved = (_reopen ? _reopen->width() : 0)
			+ style::ConvertScale(56);
		return std::max(width() - textLeft - reserved, 0);
	}

	void refit() {
		if (width() <= 0) {
			return;
		}
		const auto textWidth = textAreaWidth();
		const auto titleLines = Ggtg::WrapText(
			st::semiboldFont,
			_title,
			textWidth);
		const auto subLines = Ggtg::WrapText(
			st::normalFont,
			_subtitle,
			textWidth);
		const auto wanted = std::max(
			style::ConvertScale(52),
			st::semiboldFont->height * int(titleLines.size())
				+ st::normalFont->height * int(subLines.size())
				+ 2 * style::ConvertScale(10));
		if (wanted != height()) {
			resize(width(), wanted);
		}
	}

	const QString _title;
	const QColor _color;
	const bool _expanded;
	QString _glyph;
	QString _subtitle;
	Ui::LinkButton *_reopen = nullptr;

};

} // namespace

object_ptr<Ui::BoxContent> MakeProcessesBox() {
	return Box([=](not_null<Ui::GenericBox*> box) {
		box->setWidth(st::accountsBoxWidth);
		box->setTitle(rpl::single(u"Процессы"_q));

		struct State {
			base::flat_set<uint64> expanded;
			base::Timer refreshTimer;
		};
		const auto state = box->lifetime().make_state<State>();

		Ui::AddSkip(box->verticalLayout());
		box->addRow(object_ptr<Ggtg::ActionRow>(
			box,
			rpl::single(u"Открыть журнал GGTG (ggtg.log)"_q),
			u"↗"_q,
			&st::menuIconArticle)
		)->setClickedCallback([=] {
			const auto path = Core::GgtgLog::Path();
			if (!QFileInfo::exists(path)) {
				Ggtg::Notice(box->uiShow(), u"Журнал пока пуст"_q);
				return;
			}
			QDesktopServices::openUrl(QUrl::fromLocalFile(path));
		});
		box->addRow(object_ptr<Ggtg::ActionRow>(
			box,
			rpl::single(u"Очистить завершённые"_q),
			u"✕"_q,
			&st::menuIconClear)
		)->setClickedCallback([=] {
			Core::App().processes().clearFinished();
		});
		Ui::AddSkip(box->verticalLayout());
		const auto list = box->addRow(
			object_ptr<Ui::VerticalLayout>(box),
			style::margins());

		const auto rebuild = [=] {
			const Ggtg::ScrollKeeper keepScroll(list);
			list->clear();
			auto shown = Core::App().processes().list();
			if (shown.empty()) {
				list->add(object_ptr<Ggtg::EmptyState>(
					list,
					u"Операций пока не было. Здесь появятся рассылки, "
						"массовые действия и сбор баз."_q));
				return;
			}
			// Running first, then finished; newest first within each.
			std::stable_sort(shown.begin(), shown.end(), [](
					const Core::ProcessSnapshot &a,
					const Core::ProcessSnapshot &b) {
				const auto aRunning = (a.status == Core::ProcessStatus::Running);
				const auto bRunning = (b.status == Core::ProcessStatus::Running);
				if (aRunning != bRunning) {
					return aRunning;
				}
				return a.id > b.id;
			});
			for (const auto &process : shown) {
				const auto id = process.id;
				const auto open = state->expanded.contains(id);
				list->add(
					object_ptr<ProcessHeader>(
						list,
						process,
						open,
						[=] {
							if (state->expanded.contains(id)) {
								state->expanded.remove(id);
							} else {
								state->expanded.emplace(id);
							}
							// Not in place: this runs from inside the very
							// header being rebuilt.
							Ui::PostponeCall(box, [=] {
								state->refreshTimer.callOnce(1);
							});
						},
						process.reopen),
					st::boxRowPadding);
				if (open) {
					auto lines = QStringList();
					const auto from = (process.log.size() > 200)
						? (process.log.size() - 200)
						: size_t(0);
					for (auto i = from; i != process.log.size(); ++i) {
						lines.push_back(
							QDateTime::fromSecsSinceEpoch(process.log[i].at)
								.toString(u"HH:mm:ss"_q)
							+ u"  "_q + process.log[i].text);
					}
					list->add(
						object_ptr<Ui::FlatLabel>(
							list,
							lines.isEmpty()
								? u"Лог пока пуст."_q
								: lines.join('\n'),
							st::boxDividerLabel),
						st::boxRowPadding);
					const auto ai = Core::GgtgAi::Load();
					if (ai.enabled && ai.summaries && !lines.isEmpty()) {
						const auto title = process.title;
						list->add(
							object_ptr<Ggtg::ActionRow>(
								list,
								rpl::single(u"Сводка ИИ"_q),
								u"✦"_q,
								&st::menuIconBot),
							st::boxRowPadding
						)->setClickedCallback([=] {
							ShowAiSummaryBox(box->uiShow(), title, lines);
						});
					}
				}
				Ui::AddSkip(list);
			}
		};
		state->refreshTimer.setCallback(rebuild);
		rebuild();

		// Coalesce bursts of log lines into one rebuild every ~400 ms.
		Core::App().processes().changes(
		) | rpl::on_next([=] {
			if (!state->refreshTimer.isActive()) {
				state->refreshTimer.callOnce(400);
			}
		}, box->lifetime());
	});
}

void ShowProcessBox(not_null<Window::SessionController*> window, uint64 id) {
	ShowProcessBox(window->uiShow(), id);
}

void ShowProcessBox(std::shared_ptr<Ui::Show> show, uint64 id) {
	show->show(Box([=](not_null<Ui::GenericBox*> box) {
		const auto initial = Core::App().processes().find(id);
		box->setTitle(rpl::single(initial ? initial->title : u"Процесс"_q));
		box->setMaxHeight(st::boxMaxListHeight);

		const auto progress = box->addRow(
			object_ptr<Ui::FlatLabel>(box, QString(), st::boxLabel));
		const auto log = box->addRow(object_ptr<Ui::VerticalLayout>(box));

		struct State {
			size_t shownLines = 0;
			rpl::variable<bool> running = true;
		};
		const auto state = box->lifetime().make_state<State>();

		// Grows the log in place as new lines arrive instead of rebuilding
		// it - same reasoning as the export flow's own log box this
		// replaces: nothing here should ever make the window jump or flash
		// while a run is live.
		const auto refresh = [=] {
			const auto current = Core::App().processes().find(id);
			if (!current) {
				return;
			}
			progress->setText(ProgressText(*current));
			for (; state->shownLines < current->log.size();
					++state->shownLines) {
				log->add(object_ptr<Ui::FlatLabel>(
					log,
					current->log[state->shownLines].text,
					st::boxLabel));
			}
			state->running = (current->status == Core::ProcessStatus::Running);
			Ui::PostponeCall(box, [=, boxWeak = QPointer<Ui::GenericBox>(box.get())] {
				if (const auto stillThere = boxWeak.data()) {
					stillThere->scrollToY(stillThere->scrollHeight());
				}
			});
		};
		refresh();

		Core::App().processes().changes(
		) | rpl::on_next(refresh, box->lifetime());

		// The cross and "Скрыть" only hide the box: the process itself, and its
		// log, keep living in Core::ProcessManager either way.
		Ggtg::AddProcessHideButton(box, [=](std::shared_ptr<Ui::Show> to) {
			ShowProcessBox(std::move(to), id);
		});
		Ggtg::AddCloseCross(box);
	}));
}

} // namespace Settings
