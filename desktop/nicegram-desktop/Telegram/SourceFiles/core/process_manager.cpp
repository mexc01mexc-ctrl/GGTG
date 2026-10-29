/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/process_manager.h"

#include "base/unixtime.h"
#include "core/application.h"
#include "core/ggtg_log.h"
#include "core/ggtg_paths.h"
#include "main/main_domain.h"
#include "ui/boxes/confirm_box.h"
#include "ui/layers/show.h"

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif // NOMINMAX
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif // WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif // Q_OS_WIN

#include <QtCore/QCoreApplication>
#include <QtCore/QProcess>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSaveFile>

namespace Core {
namespace {

// A process's in-memory log keeps its newest lines only: a campaign to
// thousands of recipients would otherwise grow it (and every rebuild of
// the viewer) without bound. The full history is in ggtg.log.
constexpr auto kMaxLogLines = 2000;

// Writes are batched: a campaign logs many lines a second.
constexpr auto kSaveDelay = crl::time(2000);

// While a process runs (a mailing, collecting, training...) the computer
// does not fall asleep, turn the screen off or lock because of idleness: a
// night run would otherwise stop when the laptop goes to sleep.
void SetKeepAwake(bool on) {
	static auto current = false;
	if (std::exchange(current, on) == on) {
		return;
	}
#ifdef Q_OS_WIN
	SetThreadExecutionState(on
		? (ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED)
		: ES_CONTINUOUS);
#elif defined Q_OS_MAC || defined Q_OS_LINUX
	static auto helper = (QProcess*)nullptr;
	if (helper) {
		helper->disconnect();
		helper->kill();
		helper->deleteLater();
		helper = nullptr;
	}
	if (!on) {
		return;
	}
	helper = new QProcess(qApp);
#ifdef Q_OS_MAC
	// Holds the display and the system awake for as long as the app lives.
	helper->start(u"/usr/bin/caffeinate"_q, QStringList{
		u"-d"_q,
		u"-i"_q,
		u"-s"_q,
		u"-w"_q,
		QString::number(QCoreApplication::applicationPid()) });
#else // Q_OS_MAC
	helper->start(u"systemd-inhibit"_q, QStringList{
		u"--what=idle:sleep"_q,
		u"--why=GGTG: идёт процесс"_q,
		u"sleep"_q,
		u"infinity"_q });
#endif // Q_OS_MAC
#endif // Q_OS_WIN
}

[[nodiscard]] QString StorePath() {
	return GgtgPaths::DbDir() + u"processes.json"_q;
}

} // namespace

ProcessManager::ProcessManager()
: _saveTimer([=] { save(); }) {
	load();
}

ProcessManager::~ProcessManager() {
	SetKeepAwake(false);
	if (_saveTimer.isActive()) {
		save();
	}
}

void ProcessManager::changed() {
	if (!_saveTimer.isActive()) {
		_saveTimer.callOnce(kSaveDelay);
	}
	_changes.fire({});
}

void ProcessManager::load() {
	auto file = QFile(StorePath());
	if (!file.open(QIODevice::ReadOnly)) {
		return;
	}
	const auto document = QJsonDocument::fromJson(file.readAll());
	auto interrupted = false;
	for (const auto &value : document.object().value(u"processes"_q).toArray()) {
		const auto object = value.toObject();
		auto process = ProcessSnapshot{
			.id = _nextId++,
			.title = object.value(u"title"_q).toString(),
			.status = ProcessStatus(object.value(u"status"_q).toInt()),
			.kind = ProcessKind(object.value(u"kind"_q).toInt()),
			.progressCurrent = object.value(u"current"_q).toInt(),
			.progressTotal = object.value(u"total"_q).toInt(),
			.startedAt = TimeId(object.value(u"started"_q).toDouble()),
			.finishedAt = TimeId(object.value(u"finished"_q).toDouble()),
		};
		for (const auto &line : object.value(u"log"_q).toArray()) {
			const auto entry = line.toObject();
			process.log.push_back({
				.text = entry.value(u"text"_q).toString(),
				.at = TimeId(entry.value(u"at"_q).toDouble()),
			});
		}
		if (process.status == ProcessStatus::Running) {
			process.status = ProcessStatus::Failed;
			process.finishedAt = process.log.empty()
				? process.startedAt
				: process.log.back().at;
			process.log.push_back({
				.text = u"Прервано: программа была закрыта"_q,
				.at = process.finishedAt,
			});
			interrupted = true;
		}
		_processes.push_back(std::move(process));
	}
	if (interrupted) {
		save();
	}
}

void ProcessManager::save() {
	_saveTimer.cancel();
	auto list = QJsonArray();
	for (const auto &process : _processes) {
		auto log = QJsonArray();
		for (const auto &line : process.log) {
			log.push_back(QJsonObject{
				{ u"text"_q, line.text },
				{ u"at"_q, qint64(line.at) },
			});
		}
		list.push_back(QJsonObject{
			{ u"title"_q, process.title },
			{ u"status"_q, int(process.status) },
			{ u"kind"_q, int(process.kind) },
			{ u"current"_q, process.progressCurrent },
			{ u"total"_q, process.progressTotal },
			{ u"started"_q, qint64(process.startedAt) },
			{ u"finished"_q, qint64(process.finishedAt) },
			{ u"log"_q, log },
		});
	}
	const auto path = StorePath();
	QDir().mkpath(QFileInfo(path).absolutePath());
	auto file = QSaveFile(path);
	if (!file.open(QIODevice::WriteOnly)) {
		GgtgLog::Error(u"Процессы"_q, u"Не удалось сохранить список"_q);
		return;
	}
	file.write(QJsonDocument(QJsonObject{
		{ u"processes"_q, list },
	}).toJson(QJsonDocument::Compact));
	if (!file.commit()) {
		GgtgLog::Error(u"Процессы"_q, u"Не удалось сохранить список"_q);
	}
}

ProcessHandle::ProcessHandle(not_null<ProcessManager*> owner, uint64 id)
: _owner(owner)
, _id(id) {
}

void ProcessHandle::log(const QString &line) {
	_owner->appendLog(_id, line);
}

void ProcessHandle::progress(int current, int total) {
	_owner->setProgress(_id, current, total);
}

void ProcessHandle::succeed(const QString &finalLine) {
	_owner->finish(_id, ProcessStatus::Succeeded, finalLine);
}

void ProcessHandle::fail(const QString &finalLine) {
	_owner->finish(_id, ProcessStatus::Failed, finalLine);
}

std::shared_ptr<ProcessHandle> ProcessManager::start(
		const QString &title,
		Fn<void()> reopen,
		ProcessKind kind) {
	const auto id = _nextId++;
	_processes.push_back(ProcessSnapshot{
		.id = id,
		.title = title,
		.status = ProcessStatus::Running,
		.kind = kind,
		.startedAt = base::unixtime::now(),
		.reopen = std::move(reopen),
	});
	recomputeRunningCount();
	GgtgLog::Info(title, u"Начало"_q);
	changed();

	// std::shared_ptr, not returned by value as a plain ProcessHandle -
	// callers thread this through several levels of async callbacks (see
	// settings_information.cpp's ExportAccountsSequential for the
	// reference shape), so it needs stable identity and a lifetime that
	// isn't tied to any particular stack frame.
	return std::shared_ptr<ProcessHandle>(new ProcessHandle(this, id));
}

const ProcessSnapshot *ProcessManager::running(ProcessKind kind) const {
	for (const auto &process : _processes) {
		if (process.kind == kind && process.status == ProcessStatus::Running) {
			return &process;
		}
	}
	return nullptr;
}

QString BlockedReason(GuardedAction action) {
	const auto &processes = Core::App().processes();
	const auto busy = [&](std::initializer_list<ProcessKind> kinds) {
		for (const auto kind : kinds) {
			if (const auto process = processes.running(kind)) {
				return process;
			}
		}
		return (const ProcessSnapshot*)nullptr;
	};
	const auto because = [](const ProcessSnapshot *process, QString what) {
		return u"Идёт «"_q + process->title + u"» — "_q + what
			+ u". Дождитесь окончания или остановите его в «Процессах»."_q;
	};
	using Kind = ProcessKind;
	const auto starting = [&] {
		const auto &domain = Core::App().domain();
		const auto pending = domain.pendingAccounts();
		if (!pending) {
			return QString();
		}
		const auto total = int(domain.accounts().size()) + pending;
		return u"Аккаунты ещё подключаются ("_q
			+ QString::number(total - pending) + u" из "_q
			+ QString::number(total)
			+ u") — запустите, когда подключатся все, иначе часть "
				"аккаунтов не попадёт в работу."_q;
	};
	switch (action) {
	case GuardedAction::StartSms:
	case GuardedAction::StartBulk:
	case GuardedAction::StartViews:
		if (const auto reason = starting(); !reason.isEmpty()) {
			return reason;
		}
		break;
	default:
		break;
	}
	switch (action) {
	case GuardedAction::EditBases:
		if (const auto p = busy({ Kind::Sms })) {
			return because(p, u"базы сейчас нельзя менять"_q);
		}
		break;
	case GuardedAction::RemoveAccount:
		if (const auto p = busy({ Kind::Sms, Kind::Bulk, Kind::Views, Kind::Export })) {
			return because(p, u"аккаунты заняты, удалять их нельзя"_q);
		}
		break;
	case GuardedAction::StartSms:
		if (const auto p = busy({ Kind::Sms, Kind::Bulk, Kind::Views })) {
			return because(p, u"аккаунты заняты, рассылку начать нельзя"_q);
		}
		break;
	case GuardedAction::StartBulk:
		if (const auto p = busy({ Kind::Sms, Kind::Views })) {
			return because(p, u"аккаунты заняты, массовое действие начать нельзя"_q);
		}
		break;
	case GuardedAction::StartViews:
		if (const auto p = busy({ Kind::Sms, Kind::Bulk })) {
			return because(p, u"аккаунты заняты, накрутку начать нельзя"_q);
		}
		break;
	case GuardedAction::ChangeSessions:
		if (const auto p = busy({ Kind::Sms, Kind::Bulk, Kind::Views, Kind::Export })) {
			return because(p, u"аккаунты заняты, сессии менять нельзя"_q);
		}
		break;
	case GuardedAction::ReplaceProxy:
		if (const auto p = busy({ Kind::Sms, Kind::Bulk, Kind::Views, Kind::Export })) {
			return because(p, u"смена прокси оборвёт соединения аккаунтов"_q);
		}
		break;
	}
	return QString();
}

bool GuardAllows(
		const std::shared_ptr<Ui::Show> &show,
		GuardedAction action) {
	const auto reason = BlockedReason(action);
	if (reason.isEmpty()) {
		return true;
	}
	GgtgLog::Info(u"Защита"_q, reason);
	if (show) {
		show->showBox(Ui::MakeInformBox(reason));
	}
	return false;
}

const std::vector<ProcessSnapshot> &ProcessManager::list() const {
	return _processes;
}

const ProcessSnapshot *ProcessManager::find(uint64 id) const {
	const auto i = ranges::find(_processes, id, &ProcessSnapshot::id);
	return (i != _processes.end()) ? &*i : nullptr;
}

ProcessSnapshot *ProcessManager::findMutable(uint64 id) {
	const auto i = ranges::find(_processes, id, &ProcessSnapshot::id);
	return (i != _processes.end()) ? &*i : nullptr;
}

rpl::producer<> ProcessManager::changes() const {
	return _changes.events();
}

rpl::producer<int> ProcessManager::runningCountValue() const {
	return _runningCount.value();
}

void ProcessManager::clearFinished() {
	const auto was = _processes.size();
	std::erase_if(_processes, [](const ProcessSnapshot &process) {
		return process.status != ProcessStatus::Running;
	});
	if (_processes.size() != was) {
		changed();
	}
}

void ProcessManager::appendLog(uint64 id, const QString &line) {
	const auto process = findMutable(id);
	if (!process || process->status != ProcessStatus::Running) {
		return;
	}
	process->log.push_back({ .text = line, .at = base::unixtime::now() });
	if (process->log.size() > kMaxLogLines) {
		process->log.erase(
			process->log.begin(),
			process->log.begin() + (process->log.size() - kMaxLogLines));
	}
	GgtgLog::Info(process->title, line);
	changed();
}

void ProcessManager::setProgress(uint64 id, int current, int total) {
	const auto process = findMutable(id);
	if (!process || process->status != ProcessStatus::Running) {
		return;
	}
	process->progressCurrent = current;
	process->progressTotal = total;
	changed();
}

void ProcessManager::finish(
		uint64 id,
		ProcessStatus status,
		const QString &finalLine) {
	const auto process = findMutable(id);
	if (!process || process->status != ProcessStatus::Running) {
		return;
	}
	if (!finalLine.isEmpty()) {
		process->log.push_back({
			.text = finalLine,
			.at = base::unixtime::now(),
		});
	}
	if (!finalLine.isEmpty()) {
		(status == ProcessStatus::Failed
			? GgtgLog::Error
			: GgtgLog::Info)(process->title, finalLine);
	}
	GgtgLog::Info(
		process->title,
		status == ProcessStatus::Failed ? u"Завершено с ошибкой"_q : u"Завершено"_q);
	process->status = status;
	process->finishedAt = base::unixtime::now();
	recomputeRunningCount();
	changed();
}

void ProcessManager::recomputeRunningCount() {
	_runningCount = int(ranges::count(
		_processes,
		ProcessStatus::Running,
		&ProcessSnapshot::status));
	SetKeepAwake(_runningCount.current() > 0);
}

} // namespace Core
