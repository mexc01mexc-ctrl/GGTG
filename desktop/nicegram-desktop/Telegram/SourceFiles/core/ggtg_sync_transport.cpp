/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/ggtg_sync_transport.h"

#include "core/ggtg_sync.h"

#include "api/api_common.h"
#include "apiwrap.h"
#include "base/timer.h"
#include "base/unixtime.h"
#include "core/application.h"
#include "core/ggtg_log.h"
#include "core/ggtg_paths.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "data/scraped_members_db.h"
#include "history/history.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "mtproto/facade.h"
#include "storage/localimageloader.h"
#include "storage/storage_media_prepare.h"
#include "ui/chat/attach/attach_prepare.h"
#include "styles/style_boxes.h"

#include <QtCore/QDir>
#include <QtCore/QFile>

namespace Core::GgtgSync {
namespace {

constexpr auto kChunk = 512 * 1024;
constexpr auto kPrefix = "ggtg-sync-";
constexpr auto kTag = "#ggtgsync";
constexpr auto kPeriodMs = 5 * 60 * 1000;
constexpr auto kAfterChangeMs = 90 * 1000;
constexpr auto kFirstDelayMs = 25 * 1000;
constexpr auto kSoonMs = 5 * 1000;
constexpr auto kBeforeMailingTimeoutMs = 30 * 1000;
constexpr auto kWaitStepMs = 500;
// A file of another device made with another phrase and not replaced for
// this long is of a device that is gone (an old computer): it is removed.
constexpr auto kForeignKeyTtl = TimeId(7 * 86400);

struct Round {
	base::weak_ptr<Main::Session> session;
	Settings settings;
	Fn<void(Report)> done;
	std::vector<Remote> peers; // the newest of every other device
	std::vector<qint64> staleOwn; // older snapshots of this device
	qint64 ownNewest = 0;
	MergeResult total;
	int merged = 0;
	int incoming = 0;
	QString problem;
	// Devices with a snapshot made with this phrase, and the phrase
	// fingerprints of the ones made with another one.
	int sameKeyDevices = 0;
	QStringList otherKeys;
};

bool &RunningFlag() {
	static auto result = false;
	return result;
}

[[nodiscard]] QString SelfFileName(
		const QString &deviceId,
		const QString &passphrase) {
	return QString::fromLatin1(kPrefix)
		+ deviceId
		+ '-'
		+ KeyId(passphrase)
		+ u".bin"_q;
}

[[nodiscard]] QString KeyNote(const std::shared_ptr<Round> &round) {
	if (round->merged || round->sameKeyDevices || round->otherKeys.isEmpty()) {
		return QString();
	}
	return u". Другое устройство шифрует другим ключом ("_q
		+ round->otherKeys.join(u", "_q)
		+ u", здесь "_q
		+ KeyId(round->settings.passphrase)
		+ u"): отсканируйте QR-код заново"_q;
}

void Finish(const std::shared_ptr<Round> &round, Report report) {
	RunningFlag() = false;
	auto settings = LoadSettings();
	settings.lastSyncAt = base::unixtime::now();
	settings.lastResult = report.text + KeyNote(round);
	if (round->incoming > 0) {
		settings.lastResult += u". Ждут аккаунты с другого устройства: "_q
			+ QString::number(round->incoming);
	}
	settings.seen = round->settings.seen;
	if (!round->settings.lastUploadedHash.isEmpty()) {
		settings.lastUploadedHash = round->settings.lastUploadedHash;
	}
	if (!round->problem.isEmpty()) {
		settings.lastResult += u". "_q + round->problem;
		report.ok = report.ok && round->merged > 0;
	}
	SaveSettings(settings);
	GgtgLog::Info(u"Синхронизация"_q, settings.lastResult);
	report.text = settings.lastResult;
	if (const auto done = round->done) {
		done(report);
	}
}

void Upload(const std::shared_ptr<Round> &round) {
	const auto session = round->session.get();
	if (!session) {
		Finish(round, { false, u"Аккаунт вышел"_q });
		return;
	}
	if (!round->staleOwn.empty()) {
		auto ids = QVector<MTPint>();
		for (const auto id : base::take(round->staleOwn)) {
			ids.push_back(MTP_int(int(id)));
		}
		session->api().request(MTPmessages_DeleteMessages(
			MTP_flags(MTPmessages_DeleteMessages::Flag::f_revoke),
			MTP_vector<MTPint>(ids)
		)).send();
	}
	const auto snapshot = BuildSnapshot(round->settings);
	if (snapshot.hash == round->settings.lastUploadedHash
		&& round->ownNewest) {
		Finish(round, {
			true,
			round->merged
				? (u"Получено: "_q + ResultText(round->total))
				: u"Всё актуально"_q });
		return;
	}
	const auto dir = QDir::tempPath() + u"/ggtg-sync/"_q;
	QDir().mkpath(dir);
	const auto path = dir + SelfFileName(
		round->settings.deviceId,
		round->settings.passphrase);
	auto file = QFile(path);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		Finish(round, { false, u"Не удалось подготовить файл"_q });
		return;
	}
	file.write(Pack(snapshot.json, round->settings.passphrase));
	file.close();

	auto list = Storage::PrepareMediaList(
		QStringList{ path },
		st::sendMediaPreviewSize,
		session->user()->isPremium());
	if (list.error != Ui::PreparedList::Error::None || list.files.empty()) {
		Finish(round, { false, u"Не удалось подготовить файл"_q });
		return;
	}
	list.files.front().caption = TextWithTags{ QString::fromLatin1(kTag) };
	const auto history = session->data().history(session->user());
	auto action = Api::SendAction(history);
	action.clearDraft = false;
	session->api().sendFiles(
		std::move(list),
		SendMediaType::File,
		nullptr,
		action);
	round->settings.lastUploadedHash = snapshot.hash;
	Finish(round, {
		true,
		round->merged
			? (u"Получено: "_q + ResultText(round->total)
				+ u". Свои данные отправлены"_q)
			: u"Свои данные отправлены"_q });
}

void MergeNext(const std::shared_ptr<Round> &round, int index);

void Download(
		const std::shared_ptr<Round> &round,
		int index,
		const std::shared_ptr<QByteArray> &data) {
	const auto session = round->session.get();
	if (!session) {
		Finish(round, { false, u"Аккаунт вышел"_q });
		return;
	}
	const auto &remote = round->peers[index];
	const auto offset = data->size();
	if (remote.size > 0 && offset >= remote.size) {
		const auto unpacked = Unpack(*data, round->settings.passphrase);
		if (!unpacked) {
			// An older file without the fingerprint, made with another
			// phrase: not an error, just not ours.
			round->otherKeys.push_back(u"?"_q);
		} else {
			const auto result = MergeSnapshot(*unpacked);
			if (!result.ok) {
				round->problem = result.error;
			} else {
				round->total.bases += result.bases;
				round->total.sentAdded += result.sentAdded;
				round->total.sentRemoved += result.sentRemoved;
				round->total.files += result.files;
				round->total.ok = true;
				++round->merged;
				round->settings.seen[remote.device] = remote.msgId;
			}
		}
		MergeNext(round, index + 1);
		return;
	}
	session->api().request(MTPupload_GetFile(
		MTP_flags(0),
		MTP_inputDocumentFileLocation(
			MTP_long(remote.docId),
			MTP_long(remote.accessHash),
			MTP_bytes(remote.reference),
			MTP_string()),
		MTP_long(offset),
		MTP_int(kChunk)
	)).done([=](const MTPupload_File &result) {
		result.match([&](const MTPDupload_file &file) {
			const auto &bytes = file.vbytes().v;
			if (bytes.isEmpty()) {
				round->problem = u"Файл другого устройства пришёл пустым"_q;
				MergeNext(round, index + 1);
				return;
			}
			data->append(bytes);
			Download(round, index, data);
		}, [&](const MTPDupload_fileCdnRedirect &) {
			round->problem = u"Файл недоступен для скачивания"_q;
			MergeNext(round, index + 1);
		});
	}).fail([=](const MTP::Error &error) {
		round->problem = u"Не скачался файл другого устройства: "_q
			+ error.type();
		MergeNext(round, index + 1);
	}).toDC(MTP::downloadDcId(remote.dcId, 0)).send();
}

void MergeNext(const std::shared_ptr<Round> &round, int index) {
	if (index >= int(round->peers.size())) {
		// This device's own data goes up even when another device's file
		// failed; the problem is told in the result.
		Upload(round);
		return;
	}
	Download(round, index, std::make_shared<QByteArray>());
}

[[nodiscard]] std::vector<Remote> ParseRemotes(
		const MTPmessages_Messages &result) {
	auto out = std::vector<Remote>();
	const auto handle = [&](const QVector<MTPMessage> &list) {
		for (const auto &message : list) {
			message.match([&](const MTPDmessage &data) {
				const auto media = data.vmedia();
				if (!media) {
					return;
				}
				media->match([&](const MTPDmessageMediaDocument &item) {
					const auto document = item.vdocument();
					if (!document) {
						return;
					}
					document->match([&](const MTPDdocument &doc) {
						auto name = QString();
						for (const auto &attribute : doc.vattributes().v) {
							attribute.match([&](
									const MTPDdocumentAttributeFilename &f) {
								name = qs(f.vfile_name());
							}, [](const auto &) {});
						}
						if (!name.endsWith(u".bin"_q)) {
							return;
						}
						auto remote = Remote();
						const auto stem = name.left(name.size() - 4);
						if (stem.startsWith(QString::fromLatin1(kPrefix))) {
							// "<device>-<key>", or just "<device>" before.
							remote.kind = u"sync"_q;
							const auto rest = stem.mid(qstrlen(kPrefix));
							const auto dash = rest.indexOf('-');
							remote.device = (dash < 0) ? rest : rest.left(dash);
							remote.key = (dash < 0)
								? QString()
								: rest.mid(dash + 1);
						} else if (stem.startsWith(u"ggtg-accounts-pack-"_q)) {
							remote.kind = u"pack"_q;
							remote.device = stem.mid(19);
						} else if (stem.startsWith(u"ggtg-accounts-keys-"_q)) {
							remote.kind = u"keys"_q;
							remote.device = stem.mid(19);
						} else {
							return;
						}
						remote.msgId = data.vid().v;
						remote.date = data.vdate().v;
						remote.docId = doc.vid().v;
						remote.accessHash = doc.vaccess_hash().v;
						remote.reference = doc.vfile_reference().v;
						remote.dcId = doc.vdc_id().v;
						remote.size = doc.vsize().v;
						out.push_back(std::move(remote));
					}, [](const auto &) {});
				}, [](const auto &) {});
			}, [](const auto &) {});
		}
	};
	result.match([&](const MTPDmessages_messagesNotModified &) {
	}, [&](const auto &data) {
		handle(data.vmessages().v);
	});
	return out;
}

} // namespace

Main::Session *MainSession() {
	const auto settings = LoadSettings();
	if (settings.mainUserId.isEmpty()) {
		return nullptr;
	}
	const auto wanted = settings.mainUserId.toULongLong();
	for (const auto &[index, account] : Core::App().domain().accounts()) {
		if (Main::AccountIsLive(account.get())
			&& account->sessionExists()
			&& account->session().user()->id.value == wanted) {
			return &account->session();
		}
	}
	return nullptr;
}

bool Running() {
	return RunningFlag();
}

void RunSync(Fn<void(Report)> done) {
	const auto reply = [&](Report report) {
		if (done) {
			done(std::move(report));
		}
	};
	if (RunningFlag()) {
		reply({ false, u"Синхронизация уже идёт"_q });
		return;
	}
	auto settings = LoadSettings();
	if (settings.passphrase.isEmpty()) {
		reply({ false, u"Задайте фразу-пароль для шифрования"_q });
		return;
	}
	const auto session = MainSession();
	if (!session) {
		reply({ false, u"Главный аккаунт не подключён в этом GGTG"_q });
		return;
	}
	RunningFlag() = true;
	auto round = std::make_shared<Round>();
	round->session = base::make_weak(session);
	round->settings = settings;
	round->done = std::move(done);

	using Flag = MTPmessages_Search::Flag;
	const auto search = [=](const QString &query, Fn<void()> none) {
		session->api().request(MTPmessages_Search(
			MTP_flags(Flag(0)),
			MTP_inputPeerSelf(),
			MTP_string(query),
			MTP_inputPeerEmpty(),
			MTPInputPeer(), // saved_peer_id
			MTPVector<MTPReaction>(), // saved_reaction
			MTPint(), // top_msg_id
			MTP_inputMessagesFilterDocument(),
			MTP_int(0), // min_date
			MTP_int(0), // max_date
			MTP_int(0), // offset_id
			MTP_int(0), // add_offset
			MTP_int(100),
			MTP_int(0), // max_id
			MTP_int(0), // min_id
			MTP_long(0) // hash
		)).done([=](const MTPmessages_Messages &result) {
			auto found = ParseRemotes(result);
			auto incoming = 0;
			for (const auto &remote : found) {
				if (remote.kind == u"keys"_q
					&& remote.device != round->settings.deviceId) {
					++incoming;
				}
			}
			round->incoming = incoming;
			found.erase(
				std::remove_if(found.begin(), found.end(), [](
						const Remote &remote) {
					return remote.kind != u"sync"_q;
				}),
				found.end());
			if (found.empty() && none) {
				none();
				return;
			}
			// The newest of each device made with this phrase (or an older
			// file without the fingerprint - tried, may not open). This
			// device's own other files go: older ones, and the ones made
			// with a previous phrase (then it puts its data up again).
			const auto key = KeyId(round->settings.passphrase);
			const auto now = base::unixtime::now();
			auto newest = std::map<QString, Remote>();
			auto otherKey = std::map<QString, Remote>();
			for (const auto &remote : found) {
				const auto own = (remote.device == round->settings.deviceId);
				if (own) {
					if (remote.key == key) {
						round->ownNewest = std::max(
							round->ownNewest,
							remote.msgId);
					}
					continue;
				} else if (!remote.key.isEmpty() && remote.key != key) {
					auto &other = otherKey[remote.device];
					if (remote.msgId > other.msgId) {
						other = remote;
					}
					continue;
				}
				auto &mine = newest[remote.device];
				if (remote.msgId > mine.msgId) {
					mine = remote;
				}
			}
			for (const auto &remote : found) {
				const auto own = (remote.device == round->settings.deviceId);
				if (own && (remote.key != key
					|| remote.msgId < round->ownNewest)) {
					round->staleOwn.push_back(remote.msgId);
				} else if (!own
					&& !remote.key.isEmpty()
					&& remote.key != key
					&& remote.date > 0
					&& now - remote.date > kForeignKeyTtl) {
					round->staleOwn.push_back(remote.msgId);
				}
			}
			for (const auto &[device, remote] : otherKey) {
				if (!newest.contains(device)) {
					round->otherKeys.push_back(remote.key);
				}
			}
			for (const auto &[device, remote] : newest) {
				if (!remote.key.isEmpty()) {
					++round->sameKeyDevices;
				}
				const auto seen = round->settings.seen.find(device);
				if (seen != round->settings.seen.end()
					&& seen->second >= remote.msgId) {
					continue;
				}
				round->peers.push_back(remote);
			}
			MergeNext(round, 0);
		}).fail([=](const MTP::Error &error) {
			Finish(round, {
				false,
				u"Не удалось прочитать «Избранное»: "_q + error.type() });
		}).send();
	};
	search(QString::fromLatin1(kTag), [=] { search(QString(), nullptr); });
}

namespace {

void DownloadRemote(
		base::weak_ptr<Main::Session> weak,
		Remote remote,
		std::shared_ptr<QByteArray> data,
		Fn<void(QString error)> done) {
	const auto session = weak.get();
	if (!session) {
		done(u"Аккаунт вышел"_q);
		return;
	}
	if (remote.size > 0 && data->size() >= remote.size) {
		done(QString());
		return;
	}
	session->api().request(MTPupload_GetFile(
		MTP_flags(0),
		MTP_inputDocumentFileLocation(
			MTP_long(remote.docId),
			MTP_long(remote.accessHash),
			MTP_bytes(remote.reference),
			MTP_string()),
		MTP_long(data->size()),
		MTP_int(kChunk)
	)).done([=](const MTPupload_File &result) {
		result.match([&](const MTPDupload_file &file) {
			const auto &bytes = file.vbytes().v;
			if (bytes.isEmpty()) {
				done(u"Файл пришёл пустым"_q);
				return;
			}
			data->append(bytes);
			DownloadRemote(weak, remote, data, done);
		}, [&](const MTPDupload_fileCdnRedirect &) {
			done(u"Файл недоступен для скачивания"_q);
		});
	}).fail([=](const MTP::Error &error) {
		done(u"Не скачался файл: "_q + error.type());
	}).toDC(MTP::downloadDcId(remote.dcId, 0)).send();
}

} // namespace

void FindIncoming(Fn<void(std::vector<Remote>, QString error)> done) {
	const auto session = MainSession();
	if (!session) {
		done({}, u"Главный аккаунт не подключён в этом GGTG"_q);
		return;
	}
	const auto settings = LoadSettings();
	using Flag = MTPmessages_Search::Flag;
	session->api().request(MTPmessages_Search(
		MTP_flags(Flag(0)),
		MTP_inputPeerSelf(),
		MTP_string(),
		MTP_inputPeerEmpty(),
		MTPInputPeer(), // saved_peer_id
		MTPVector<MTPReaction>(), // saved_reaction
		MTPint(), // top_msg_id
		MTP_inputMessagesFilterDocument(),
		MTP_int(0), // min_date
		MTP_int(0), // max_date
		MTP_int(0), // offset_id
		MTP_int(0), // add_offset
		MTP_int(100),
		MTP_int(0), // max_id
		MTP_int(0), // min_id
		MTP_long(0) // hash
	)).done([=](const MTPmessages_Messages &result) {
		auto newest = std::map<QString, Remote>();
		for (const auto &remote : ParseRemotes(result)) {
			if (remote.kind != u"keys"_q
				|| remote.device == settings.deviceId) {
				continue;
			}
			auto &mine = newest[remote.device];
			if (remote.msgId > mine.msgId) {
				mine = remote;
			}
		}
		auto list = std::vector<Remote>();
		for (const auto &[device, remote] : newest) {
			list.push_back(remote);
		}
		done(std::move(list), QString());
	}).fail([=](const MTP::Error &error) {
		done({}, u"Не удалось прочитать «Избранное»: "_q + error.type());
	}).send();
}

void SendAccounts(
		const QString &kind,
		const QByteArray &payload,
		Fn<void(bool ok, QString text)> done) {
	const auto settings = LoadSettings();
	if (settings.passphrase.isEmpty()) {
		done(false, u"Задайте фразу-пароль в «Синхронизации»"_q);
		return;
	}
	const auto session = MainSession();
	if (!session) {
		done(false, u"Главный аккаунт не подключён в этом GGTG"_q);
		return;
	}
	const auto dir = QDir::tempPath() + u"/ggtg-sync/"_q;
	QDir().mkpath(dir);
	const auto path = dir + u"ggtg-accounts-"_q + kind + '-'
		+ settings.deviceId + u".bin"_q;
	auto file = QFile(path);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		done(false, u"Не удалось подготовить файл"_q);
		return;
	}
	file.write(Pack(payload, settings.passphrase));
	file.close();
	auto list = Storage::PrepareMediaList(
		QStringList{ path },
		st::sendMediaPreviewSize,
		session->user()->isPremium());
	if (list.error != Ui::PreparedList::Error::None || list.files.empty()) {
		done(false, u"Не удалось подготовить файл"_q);
		return;
	}
	list.files.front().caption = TextWithTags{ QString::fromLatin1(kTag) };
	const auto history = session->data().history(session->user());
	auto action = Api::SendAction(history);
	action.clearDraft = false;
	session->api().sendFiles(
		std::move(list),
		SendMediaType::File,
		nullptr,
		action);
	done(true, u"Отправлено в «Избранное» главного аккаунта"_q);
}

void FetchIncoming(
		const Remote &remote,
		Fn<void(QByteArray payload, QString error)> done) {
	const auto session = MainSession();
	if (!session) {
		done({}, u"Главный аккаунт не подключён в этом GGTG"_q);
		return;
	}
	const auto settings = LoadSettings();
	const auto data = std::make_shared<QByteArray>();
	const auto weak = base::make_weak(session);
	DownloadRemote(weak, remote, data, [=](QString error) {
		if (!error.isEmpty()) {
			done({}, error);
			return;
		}
		const auto plain = Unpack(*data, settings.passphrase);
		if (!plain) {
			done({}, u"Не удалось расшифровать: проверьте, что фраза "
				"одинаковая на обоих устройствах"_q);
			return;
		}
		if (const auto alive = weak.get()) {
			alive->api().request(MTPmessages_DeleteMessages(
				MTP_flags(0),
				MTP_vector<MTPint>(QVector<MTPint>{
					MTP_int(int(remote.msgId)) })
			)).send();
		}
		done(*plain, QString());
	});
}

namespace {

struct Auto {
	base::Timer periodic;
	base::Timer afterChange;
	base::Timer first;
	rpl::lifetime lifetime;
	bool hooked = false;
};

[[nodiscard]] Auto &AutoState() {
	static auto result = Auto();
	return result;
}

} // namespace

void Restart() {
	auto &state = AutoState();
	state.periodic.cancel();
	state.afterChange.cancel();
	state.first.cancel();
	const auto settings = LoadSettings();
	if (!settings.enabled || settings.passphrase.isEmpty()) {
		return;
	}
	if (!state.hooked) {
		state.hooked = true;
		state.periodic.setCallback([] { RunSync(); });
		state.afterChange.setCallback([] { RunSync(); });
		state.first.setCallback([] { RunSync(); });
		Data::BasesChanged() | rpl::on_next([] {
			const auto settings = LoadSettings();
			if (!settings.enabled || Running() || !MainSession()) {
				return;
			}
			AutoState().afterChange.callOnce(kAfterChangeMs);
		}, state.lifetime);
	}
	state.periodic.callEach(kPeriodMs);
	state.first.callOnce(kFirstDelayMs);
}

void SessionStarted(not_null<Main::Session*> session) {
	Q_UNUSED(session);
	Restart();
}

void SyncSoon() {
	const auto settings = LoadSettings();
	if (!settings.enabled || settings.passphrase.isEmpty()) {
		return;
	}
	Restart();
	AutoState().afterChange.callOnce(kSoonMs);
}

void SyncBeforeMailing(Fn<void()> proceed) {
	const auto settings = LoadSettings();
	if (!settings.enabled
		|| settings.passphrase.isEmpty()
		|| !MainSession()) {
		proceed();
		return;
	}
	struct Wait {
		Fn<void()> proceed;
		base::Timer timeout;
		base::Timer poll;
		bool done = false;
	};
	const auto wait = std::make_shared<Wait>();
	wait->proceed = std::move(proceed);
	const auto finish = [=] {
		if (wait->done) {
			return;
		}
		wait->done = true;
		wait->timeout.cancel();
		wait->poll.cancel();
		const auto proceed = base::take(wait->proceed);
		// The timers' callbacks hold `wait`: drop them, but not from
		// inside one of them.
		crl::on_main([=] {
			wait->timeout.setCallback(nullptr);
			wait->poll.setCallback(nullptr);
		});
		proceed();
	};
	wait->timeout.setCallback(finish);
	wait->timeout.callOnce(kBeforeMailingTimeoutMs);
	// A round already going on may have downloaded before the other device
	// put its last records up: wait for it, then run a fresh one.
	const auto start = [=] {
		RunSync([=](Report) { finish(); });
	};
	if (Running()) {
		wait->poll.setCallback([=] {
			if (!Running()) {
				wait->poll.cancel();
				start();
			}
		});
		wait->poll.callEach(kWaitStepMs);
	} else {
		start();
	}
}

} // namespace Core::GgtgSync
