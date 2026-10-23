/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/object_ptr.h"

namespace Window {
class SessionController;
} // namespace Window

namespace Ui {
class BoxContent;
class Show;
} // namespace Ui

namespace Settings {

// Generic live viewer for a Core::ProcessManager-tracked operation - the
// default "Процессы" row opens this unless the operation registered its
// own `reopen` (see core/process_manager.h's ProcessSnapshot::reopen).
// Safe to call for an id that no longer exists (already dropped, which
// this manager never actually does, or simply mistyped) - shows an empty
// box rather than crashing.
void ShowProcessBox(not_null<Window::SessionController*> window, uint64 id);
void ShowProcessBox(std::shared_ptr<Ui::Show> show, uint64 id);

// "Процессы" as a page of the GGTG window (window/window_main_menu.cpp),
// laid out like the reference web panel's list: one collapsible card per
// operation - running ones first, then finished, newest first - with a
// status icon, its title, a colored status line (progress, or "Готово" /
// "Завершено с ошибкой" plus when it started), a "Открыть" link for the
// ones that have their own live box (SMS campaign, bulk action), and its
// full log when expanded. Updates live while operations run.
[[nodiscard]] object_ptr<Ui::BoxContent> MakeProcessesBox();

} // namespace Settings
