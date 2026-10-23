/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "settings/sections/settings_processes.h"

#include "core/application.h"
#include "core/process_manager.h"
#include "settings/process_box.h"
#include "settings/settings_common.h"
#include "settings/settings_common_session.h"
#include "ui/wrap/vertical_layout.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/buttons.h"
#include "ui/vertical_list.h"
#include "ui/ui_utility.h"
#include "window/window_session_controller.h"
#include "styles/style_settings.h"
#include "styles/style_layers.h"

namespace Settings {
namespace {

[[nodiscard]] QString StatusLabel(const Core::ProcessSnapshot &process) {
	switch (process.status) {
	case Core::ProcessStatus::Succeeded:
		return u"Готово"_q;
	case Core::ProcessStatus::Failed:
		return u"Ошибка"_q;
	case Core::ProcessStatus::Running:
		break;
	}
	return (process.progressTotal > 0)
		? u"%1 из %2"_q
			.arg(process.progressCurrent)
			.arg(process.progressTotal)
		: u"Выполняется..."_q;
}

// No kMeta/BuildHelper here on purpose - unlike the other sections in this
// directory, this list is entirely dynamic (rebuilt every time a process
// starts, logs, or finishes) and has nothing meaningful to contribute to
// the static search index that BuildHelper feeds. Type navigation itself
// (Section<Processes>::Id(), used below and from settings_main.cpp) works
// independently of that registration.
class Processes final : public Section<Processes> {
public:
	Processes(
		QWidget *parent,
		not_null<Window::SessionController*> controller);

	[[nodiscard]] rpl::producer<QString> title() override;

private:
	void setupContent();
	void rebuild();

	const not_null<Ui::VerticalLayout*> _container;
	QPointer<Ui::VerticalLayout> _wrap;

};

Processes::Processes(
	QWidget *parent,
	not_null<Window::SessionController*> controller)
: Section(parent, controller)
, _container(Ui::CreateChild<Ui::VerticalLayout>(this)) {
	setupContent();
}

rpl::producer<QString> Processes::title() {
	return rpl::single(u"Процессы"_q);
}

void Processes::setupContent() {
	rebuild();

	Core::App().processes().changes(
	) | rpl::on_next([=] {
		rebuild();
	}, lifetime());

	Ui::ResizeFitChild(this, _container);
}

void Processes::rebuild() {
	// Same "destroy and reinsert" shape AccountsList::rebuild() uses just
	// above in settings_information.cpp - simplest way to keep a
	// dynamically-changing list of rows in sync with its source of truth
	// without hand-rolling per-row diffing for what's, in practice, a
	// short list that changes rarely.
	delete _wrap.data();
	const auto wrap = _container->add(
		object_ptr<Ui::VerticalLayout>(_container));
	_wrap = wrap;

	const auto &list = Core::App().processes().list();
	if (list.empty()) {
		Ui::AddSkip(wrap);
		wrap->add(object_ptr<Ui::FlatLabel>(
			wrap,
			u"Пока нет ни одного процесса."_q,
			st::boxLabel));
		Ui::AddSkip(wrap);
		return;
	}

	auto running = std::vector<const Core::ProcessSnapshot*>();
	auto finished = std::vector<const Core::ProcessSnapshot*>();
	for (const auto &process : list) {
		if (process.status == Core::ProcessStatus::Running) {
			running.push_back(&process);
		} else {
			finished.push_back(&process);
		}
	}
	// Most-recent-first among finished runs, per the plan; running ones
	// keep insertion order (there's rarely more than one at a time).
	std::reverse(finished.begin(), finished.end());

	const auto controller = this->controller();
	const auto addRow = [=](const Core::ProcessSnapshot &process) {
		const auto id = process.id;
		const auto reopen = process.reopen;
		const auto button = AddButtonWithLabel(
			wrap,
			rpl::single(process.title),
			rpl::single(StatusLabel(process)),
			st::settingsButton);
		button->addClickHandler([=] {
			if (reopen) {
				reopen();
			} else {
				ShowProcessBox(controller, id);
			}
		});
	};
	for (const auto *process : running) {
		addRow(*process);
	}
	for (const auto *process : finished) {
		addRow(*process);
	}
}

} // namespace

Type ProcessesId() {
	return Processes::Id();
}

} // namespace Settings
