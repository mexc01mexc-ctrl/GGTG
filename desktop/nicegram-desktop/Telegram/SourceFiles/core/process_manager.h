/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/timer.h"

namespace Ui {
class Show;
} // namespace Ui

namespace Core {

// What a running process is, for the "защита" below: some actions are
// refused while a process of certain kinds runs.
enum class ProcessKind : uchar {
	Other,
	Sms, // "Отправка SMS": reads bases, rewrites them as people get sent
	Bulk, // "Массовые действия" over the accounts
	Views, // "Накрутка": views / joining with the accounts
	Export, // accounts being exported to a folder
	Scrape, // collecting members / groups into bases
	Warmup, // "Прогрев": safe warmup of accounts
	Registration, // "Авторегистрация": creating new Telegram accounts
};

// Actions refused while some processes run (see BlockedReason).
enum class GuardedAction : uchar {
	EditBases, // edit, delete, split, import into "База"
	RemoveAccount, // log out or delete an account
	StartSms,
	StartBulk,
	StartViews,
	ReplaceProxy, // changes accounts' connections live
	ChangeSessions, // end / swap / back up / restore an account's sessions
};

// Why `action` can't be done now, or empty when it can.
[[nodiscard]] QString BlockedReason(GuardedAction action);

// BlockedReason as a toast on `show`: true when the action may go on.
[[nodiscard]] bool GuardAllows(
	const std::shared_ptr<Ui::Show> &show,
	GuardedAction action);

enum class ProcessStatus : uchar {
	Running,
	Succeeded,
	Failed,
};

struct ProcessLogLine {
	QString text;
	TimeId at = 0;
};

class ProcessManager;

// One tracked operation's current state, as seen by the "Процессы" list
// and the generic process viewer (settings/process_box.h) - a snapshot,
// not a live reference, so it's safe for UI code to copy/hold onto
// briefly while rebuilding a list.
struct ProcessSnapshot {
	uint64 id = 0;
	QString title;
	ProcessStatus status = ProcessStatus::Running;
	ProcessKind kind = ProcessKind::Other;
	std::vector<ProcessLogLine> log;
	// 0/0 means indeterminate - show "Выполняется..." rather than "0 из 0".
	int progressCurrent = 0;
	int progressTotal = 0;
	TimeId startedAt = 0;
	TimeId finishedAt = 0;
	// Set only by integrations that already have their own specialized
	// live box (SMS campaign, bulk account actions) and want the
	// "Процессы" list to reopen *that* on click instead of the generic
	// log viewer every other operation gets by default.
	Fn<void()> reopen;
};

// Handed to whatever code is actually running the operation - the only
// way to update a process once ProcessManager::start() has created it.
// Every call here is main-thread-only, same as the rest of the app.
class ProcessHandle {
public:
	// Needed by callers that open the generic viewer themselves right
	// after start() (ShowProcessBox(window, handle->id()) - see
	// settings_information.cpp's ExportByDuplicatingSessions) instead of
	// relying on the "Процессы" list's own click handling.
	[[nodiscard]] uint64 id() const {
		return _id;
	}

	void log(const QString &line);
	void progress(int current, int total);
	// Calling either finishes the process (further log()/progress() calls
	// are ignored) - `finalLine`, if not empty, is appended to the log
	// first so the reason for success/failure isn't lost once the run
	// stops being "live".
	void succeed(const QString &finalLine = QString());
	void fail(const QString &finalLine = QString());

private:
	friend class ProcessManager;
	ProcessHandle(not_null<ProcessManager*> owner, uint64 id);

	const not_null<ProcessManager*> _owner;
	const uint64 _id = 0;

};

// App-wide (Core::App().processes()), not per-session - these operations
// (export, SMS, bulk actions, proxy replace) all work across whichever
// accounts they're pointed at, not scoped to one particular session the
// way most other app state is.
class ProcessManager final {
public:
	// The list survives restarts: it is kept in tdata/ggtg_db/processes.json
	// (loaded here, written shortly after every change and on exit). A
	// process still running when the app closed comes back as failed.
	ProcessManager();
	~ProcessManager();

	// `reopen`, if given, is what a "Процессы" row calls instead of
	// opening the generic viewer - see ProcessSnapshot::reopen's own
	// comment for when that's appropriate.
	[[nodiscard]] std::shared_ptr<ProcessHandle> start(
		const QString &title,
		Fn<void()> reopen = nullptr,
		ProcessKind kind = ProcessKind::Other);
	// A running process of `kind`, or nullptr.
	[[nodiscard]] const ProcessSnapshot *running(ProcessKind kind) const;

	[[nodiscard]] const std::vector<ProcessSnapshot> &list() const;
	[[nodiscard]] const ProcessSnapshot *find(uint64 id) const;
	[[nodiscard]] rpl::producer<> changes() const;
	[[nodiscard]] rpl::producer<int> runningCountValue() const;

	// Drops every finished (succeeded or failed) process and its log from
	// the list; running ones are untouched.
	void clearFinished();

private:
	friend class ProcessHandle;

	// Called only through a ProcessHandle - see its own public methods.
	void appendLog(uint64 id, const QString &line);
	void setProgress(uint64 id, int current, int total);
	void finish(uint64 id, ProcessStatus status, const QString &finalLine);

	[[nodiscard]] ProcessSnapshot *findMutable(uint64 id);
	void recomputeRunningCount();
	void load();
	void save();
	void changed();

	// Insertion order - the "Процессы" list itself decides how to sort
	// running-vs-finished for display, this just needs stable ids.
	std::vector<ProcessSnapshot> _processes;
	rpl::event_stream<> _changes;
	rpl::variable<int> _runningCount = 0;
	uint64 _nextId = 1;
	base::Timer _saveTimer;

};

} // namespace Core
