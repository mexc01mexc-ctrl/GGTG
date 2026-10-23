// GGTG: Smart Account Warmup (Прогрев аккаунтов) for maximum Trust Score before campaigns.
// 2 Modes:
// 1. Browsing International Channels & Groups (human pacing, reading posts, reactions, joining)
// 2. Inter-Account Mutual Conversation (natural English dialogue, typing simulation, typos, reactions)
#pragma once

#include "apiwrap.h"
#include "boxes/abstract_box.h"
#include "boxes/connection_box.h"
#include "core/application.h"
#include "core/file_utilities.h"
#include "core/ggtg_paths.h"
#include "core/ggtg_log.h"
#include "core/ggtg_ai.h"
#include "core/process_manager.h"
#include "data/data_histories.h"
#include "data/data_session.h"
#include "history/history.h"
#include "ui/vertical_list.h"
#include "data/data_user.h"
#include "data/data_channel.h"
#include "lang/lang_keys.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "mtproto/sender.h"
#include "ui/boxes/confirm_box.h"
#include "ui/layers/generic_box.h"
#include "ui/layers/show.h"
#include "ui/text/text_entity.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/scroll_area.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"
#include "settings/bulk_accounts_actions.h"
#include "styles/style_boxes.h"
#include "styles/style_window_main_menu.h"

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QRandomGenerator>
#include <QtCore/QRegularExpression>
#include <QtCore/QTimer>

namespace Settings {

enum class WarmupMode {
	ChannelsAndGroups = 0, // Mode 1: Просмотр зарубежных каналов и групп
	MutualDialogue = 1,    // Mode 2: Взаимная переписка между аккаунтами
};

enum class WarmupIndicator {
	None,
	Running,
	Done,
};

struct WarmupStats {
	int trustScore = 20; // 0..100
	QString lastWarmup;  // formatted date string
	int postsRead = 0;
	int reactionsGiven = 0;
	int pollsVoted = 0;
	int p2pDialogues = 0;
	int channelsJoined = 0;
};

// -----------------------------------------------------------------------------
// Warmup Trust History Persistence
// -----------------------------------------------------------------------------

inline QString WarmupHistoryFilePath() {
	return Core::GgtgPaths::DbDir() + u"warmup_history.json"_q;
}

inline QMap<QString, WarmupStats> LoadWarmupHistory() {
	auto result = QMap<QString, WarmupStats>();
	auto file = QFile(WarmupHistoryFilePath());
	if (!file.open(QIODevice::ReadOnly)) {
		return result;
	}
	const auto doc = QJsonDocument::fromJson(file.readAll());
	if (!doc.isObject()) {
		return result;
	}
	const auto root = doc.object();
	for (auto it = root.constBegin(); it != root.constEnd(); ++it) {
		const auto obj = it.value().toObject();
		auto st = WarmupStats();
		st.trustScore = obj[u"trustScore"_q].toInt(20);
		st.lastWarmup = obj[u"lastWarmup"_q].toString();
		st.postsRead = obj[u"postsRead"_q].toInt();
		st.reactionsGiven = obj[u"reactionsGiven"_q].toInt();
		st.pollsVoted = obj[u"pollsVoted"_q].toInt();
		st.p2pDialogues = obj[u"p2pDialogues"_q].toInt();
		st.channelsJoined = obj[u"channelsJoined"_q].toInt();
		result.insert(it.key(), st);
	}
	return result;
}

inline void SaveWarmupHistory(const QMap<QString, WarmupStats> &history) {
	auto root = QJsonObject();
	for (auto it = history.constBegin(); it != history.constEnd(); ++it) {
		auto obj = QJsonObject();
		obj[u"trustScore"_q] = it.value().trustScore;
		obj[u"lastWarmup"_q] = it.value().lastWarmup;
		obj[u"postsRead"_q] = it.value().postsRead;
		obj[u"reactionsGiven"_q] = it.value().reactionsGiven;
		obj[u"pollsVoted"_q] = it.value().pollsVoted;
		obj[u"p2pDialogues"_q] = it.value().p2pDialogues;
		obj[u"channelsJoined"_q] = it.value().channelsJoined;
		root[it.key()] = obj;
	}
	QDir().mkpath(Core::GgtgPaths::DbDir());
	auto file = QFile(WarmupHistoryFilePath());
	if (file.open(QIODevice::WriteOnly)) {
		file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
	}
}

inline QString AccountWarmupKey(not_null<Main::Account*> account) {
	if (account->sessionExists() && account->session().user()) {
		const auto phone = account->session().user()->phone();
		if (!phone.isEmpty()) return phone;
		return QString::number(account->session().userId().bare);
	}
	return u"session"_q;
}

inline WarmupStats GetAccountWarmupStats(not_null<Main::Account*> account) {
	const auto key = AccountWarmupKey(account);
	auto hist = LoadWarmupHistory();
	if (hist.contains(key)) {
		return hist.value(key);
	}
	auto fresh = WarmupStats();
	fresh.trustScore = 25;
	return fresh;
}

inline void UpdateAccountWarmupStats(
		not_null<Main::Account*> account,
		int postsDelta,
		int reactionsDelta,
		int pollsDelta,
		int p2pDelta,
		int joinsDelta) {
	const auto key = AccountWarmupKey(account);
	auto hist = LoadWarmupHistory();
	auto st = hist.value(key, WarmupStats());
	st.postsRead += postsDelta;
	st.reactionsGiven += reactionsDelta;
	st.pollsVoted += pollsDelta;
	st.p2pDialogues += p2pDelta;
	st.channelsJoined += joinsDelta;
	st.lastWarmup = QDateTime::currentDateTime().toString(u"dd.MM.yyyy hh:mm"_q);

	int calculated = 25
		+ std::min(30, st.postsRead / 2)
		+ std::min(15, st.reactionsGiven * 2)
		+ std::min(10, st.pollsVoted * 3)
		+ std::min(20, st.p2pDialogues * 5)
		+ std::min(10, st.channelsJoined * 3);
	st.trustScore = std::clamp(calculated, 10, 100);

	hist.insert(key, st);
	SaveWarmupHistory(hist);
}

// -----------------------------------------------------------------------------
// Safe Whitelist Catalog: Strictly International (NO CIS / NO Russian)
// -----------------------------------------------------------------------------

inline QStringList SafeCatalogChannelsEn() {
	return {
		u"techcrunch"_q,
		u"nature"_q,
		u"bbcearth"_q,
		u"theeconomist"_q,
		u"bloomberg"_q,
		u"discovery"_q,
		u"sciencedaily"_q,
		u"worldnews"_q,
		u"wired"_q,
		u"nationalgeographic"_q,
		u"theverge"_q,
		u"nasa"_q,
		u"tedtalks"_q,
	};
}

// -----------------------------------------------------------------------------
// Natural English Dialogue Scenarios (Mode 2)
// -----------------------------------------------------------------------------

struct DialogueTurn {
	QString textA;
	QString textB;
	QString reactionA;
	QString reactionB;
};

struct DialogueScript {
	std::vector<DialogueTurn> turns;
};

inline std::vector<DialogueScript> NaturalDialogueScripts() {
	return {
		// Script 1: Work & Coffee Break
		{
			{
				{
					u"Hey! How is your day going? 😊"_q,
					u"Hey there! Pretty busy, just finishing up a few tasks. How about you?"_q,
					u"☕"_q,
					u"👍"_q,
				},
				{
					u"Same here, had quite a few meetings today! Taking a quick coffee break now ☕"_q,
					u"Haha sounds familiar! Enjoy your coffee, talk to you in a bit 👌"_q,
					QString(),
					u"👍"_q,
				},
			}
		},
		// Script 2: Weekend & Outdoor Plans
		{
			{
				{
					u"Hello! Any fun plans for this weekend?"_q,
					u"Hey! Thinking of heading out for a short road trip if the weather stays nice 🚗 You?"_q,
					u"❤️"_q,
					u"✨"_q,
				},
				{
					u"That sounds awesome! Just planning to chill at home and catch up on some reading 📚"_q,
					u"Nice, relaxing weekends are the best! Have a great one! 😊"_q,
					QString(),
					u"🎉"_q,
				},
			}
		},
		// Script 3: Tech & News Updates
		{
			{
				{
					u"Hey mate, did you see the latest tech news today?"_q,
					u"Hi! Not yet, was heads down in work. Anything interesting happen?"_q,
					u"🔥"_q,
					u"👍"_q,
				},
				{
					u"Yeah, some pretty cool AI updates and hardware announcements released this morning 🚀"_q,
					u"Oh nice, thanks for the heads up! Will definitely check it out tonight 👌"_q,
					QString(),
					u"🔥"_q,
				},
			}
		},
		// Script 4: Project Collaboration & Check-in
		{
			{
				{
					u"Good afternoon! Hope you're having a productive week 💪"_q,
					u"Thanks! Going well so far, trying to stay on top of everything. Hope all is good on your end too!"_q,
					u"👏"_q,
					u"👍"_q,
				},
				{
					u"All good here, thank you! Let me know if you need any help with the shared docs."_q,
					u"Appreciate it a lot! Will ping you if anything comes up 😊"_q,
					QString(),
					u"👌"_q,
				},
			}
		},
		// Script 5: Hobbies & Photography
		{
			{
				{
					u"Hey! Have you had a chance to try out that new camera feature yet?"_q,
					u"Hey! Yes, took a few test shots yesterday. The quality is honestly impressive 📸"_q,
					u"🔥"_q,
					u"✨"_q,
				},
				{
					u"Awesome, can't wait to see the shots! Catch you later 👍"_q,
					u"Will share some soon! Have a great afternoon 😊"_q,
					QString(),
					u"👍"_q,
				},
			}
		},
	};
}

// -----------------------------------------------------------------------------
// Warmup Configuration
// -----------------------------------------------------------------------------

struct WarmupConfig {
	WarmupMode mode = WarmupMode::ChannelsAndGroups;
	int delayMinSec = 4;
	int delayMaxSec = 8;

	// Mode 1: Channels and Groups
	bool joinChannels = true;
	int maxJoinsPerAccount = 2;
	int readPostsPerChannel = 15;
	bool enableReactions = true;
	int reactionChance = 70; // %
	bool useSafeCatalog = true;
	QString customChannels;

	// Mode 2: Mutual Dialogue
	int dialogueTurns = 2; // turns per pair (2 turns = 4 messages)
	bool enableDialogueReactions = true;
};

// -----------------------------------------------------------------------------
// State & Indicator
// -----------------------------------------------------------------------------

inline rpl::variable<WarmupIndicator> &WarmupIndicatorVariable() {
	static auto variable = rpl::variable<WarmupIndicator>(WarmupIndicator::None);
	return variable;
}

[[nodiscard]] inline rpl::producer<WarmupIndicator> WarmupIndicatorValue() {
	return WarmupIndicatorVariable().value();
}

inline rpl::producer<QString> WarmupIndicatorLabel() {
	return WarmupIndicatorValue() | rpl::map([](WarmupIndicator ind) {
		switch (ind) {
		case WarmupIndicator::Running:
			return u"Прогрев (идёт)"_q;
		case WarmupIndicator::Done:
			return u"Прогрев (завершён)"_q;
		default:
			return u"Прогрев"_q;
		}
	});
}

// -----------------------------------------------------------------------------
// Active Warmup Runner (Crash-Proof with std::enable_shared_from_this)
// -----------------------------------------------------------------------------

class WarmupRunner final
	: public std::enable_shared_from_this<WarmupRunner>
	, public QObject {
public:
	WarmupRunner(
		std::vector<not_null<Main::Account*>> accounts,
		WarmupConfig config,
		Fn<void(QString)> logCb,
		Fn<void(int current, int total)> progressCb,
		Fn<void(bool ok)> doneCb)
	: _accounts(std::move(accounts))
	, _config(std::move(config))
	, _logCb(std::move(logCb))
	, _progressCb(std::move(progressCb))
	, _doneCb(std::move(doneCb)) {
	}

	void start() {
		if (_accounts.empty()) {
			finish(true);
			return;
		}
		_totalSteps = int(_accounts.size());
		_currentStep = 0;
		_processHandle = Core::App().processes().start(
			u"Прогрев аккаунтов"_q,
			nullptr,
			Core::ProcessKind::Warmup);
		WarmupIndicatorVariable() = WarmupIndicator::Running;

		log(u"=== Старт прогрева GGTG (%1 аккаунтов, режим: %2) ==="_q
			.arg(_accounts.size())
			.arg(ModeName(_config.mode)));

		if (_config.mode == WarmupMode::MutualDialogue) {
			startMutualDialogues();
		} else {
			processNextAccount();
		}
	}

	void cancel() {
		if (_cancelled) return;
		_cancelled = true;
		log(u"Прогрев остановлен пользователем."_q);
		finish(false);
	}

	[[nodiscard]] bool isCancelled() const { return _cancelled; }

private:
	static QString ModeName(WarmupMode mode) {
		switch (mode) {
		case WarmupMode::ChannelsAndGroups:
			return u"Просмотр зарубежных каналов и групп"_q;
		case WarmupMode::MutualDialogue:
			return u"Взаимная переписка между аккаунтами (English)"_q;
		}
		return u"Прогрев"_q;
	}

	void log(const QString &text) {
		const auto line = QDateTime::currentDateTime().toString(u"hh:mm:ss"_q) + u" | "_q + text;
		if (_logCb) _logCb(line);
		if (_processHandle) _processHandle->log(line);
		LOG((line));
	}

	template <typename Callback>
	void delayCall(int ms, Callback &&cb) {
		if (_cancelled) return;
		auto weak = weak_from_this();
		QTimer::singleShot(ms, this, [weak, fn = std::forward<Callback>(cb)] {
			auto self = weak.lock();
			if (!self || self->isCancelled()) return;
			fn();
		});
	}

	// -------------------------------------------------------------------------
	// MODE 1: CHANNELS & GROUPS (International Whitelist, Human Pacing)
	// -------------------------------------------------------------------------

	void processNextAccount() {
		if (_cancelled) return;
		if (_currentStep >= int(_accounts.size())) {
			log(u"✓ Все аккаунты успешно прогреты!"_q);
			finish(true);
			return;
		}

		const auto account = _accounts[_currentStep];
		_currentStep++;
		if (_progressCb) _progressCb(_currentStep, _totalSteps);
		if (_processHandle) _processHandle->progress(_currentStep, _totalSteps);

		const auto name = account->sessionExists()
			? account->session().user()->name()
			: u"Аккаунт %1"_q.arg(account->session().userId().bare);
		log(u"--- Прогрев: %1 (%2 из %3) ---"_q.arg(name).arg(_currentStep).arg(_totalSteps));

		if (!account->sessionExists()) {
			log(u"Аккаунт не авторизован, пропуск."_q);
			delayCall(1000, [this] { processNextAccount(); });
			return;
		}

		warmupSingleAccount(account, [this] {
			if (_cancelled) return;
			const auto delay = QRandomGenerator::global()->bounded(
				_config.delayMinSec * 1000,
				_config.delayMaxSec * 1000 + 1);
			delayCall(delay, [this] {
				processNextAccount();
			});
		});
	}

	void warmupSingleAccount(not_null<Main::Account*> account, Fn<void()> done) {
		if (_cancelled) { done(); return; }

		// International channels only
		auto channels = QStringList();
		if (_config.useSafeCatalog) {
			channels.append(SafeCatalogChannelsEn());
		}
		if (!_config.customChannels.trimmed().isEmpty()) {
			for (const auto &c : _config.customChannels.split(QRegularExpression(u"[\\s,;\\n]+"_q), Qt::SkipEmptyParts)) {
				auto clean = c.trimmed();
				if (clean.startsWith('@')) clean.remove(0, 1);
				if (!clean.isEmpty()) channels.append(clean);
			}
		}

		if (channels.isEmpty()) {
			channels.append(SafeCatalogChannelsEn());
		}

		std::shuffle(channels.begin(), channels.end(), *QRandomGenerator::global());
		const auto countToVisit = std::min(channels.size(), qsizetype(2));

		auto visitIndex = std::make_shared<int>(0);
		auto postsReadAcc = std::make_shared<int>(0);
		auto reactionsAcc = std::make_shared<int>(0);
		auto joinsAcc = std::make_shared<int>(0);

		auto visitNext = std::make_shared<Fn<void()>>();
		*visitNext = [=, this]() {
			if (_cancelled || *visitIndex >= countToVisit) {
				UpdateAccountWarmupStats(
					account,
					*postsReadAcc,
					*reactionsAcc,
					0,
					0,
					*joinsAcc);
				const auto st = GetAccountWarmupStats(account);
				log(u"✓ Аккаунт прогрет. Новый Траст: %1% (Прочитано постов: %2, Реакций: %3, Подписок: %4)"_q
					.arg(st.trustScore)
					.arg(st.postsRead)
					.arg(st.reactionsGiven)
					.arg(st.channelsJoined));
				done();
				return;
			}

			const auto chUsername = channels[*visitIndex];
			(*visitIndex)++;

			// Human pause before opening each channel (3-6 seconds)
			const auto openDelay = QRandomGenerator::global()->bounded(3000, 6000);
			delayCall(openDelay, [=, this] {
				if (_cancelled) { done(); return; }
				log(u"Открытие канала @%1..."_q.arg(chUsername));

				const auto api = static_cast<MTP::Sender*>(&account->session().api());
				auto weak = weak_from_this();

				api->request(MTPcontacts_ResolveUsername(
					MTP_flags(MTPcontacts_ResolveUsername::Flags(0)),
					MTP_string(chUsername),
					MTP_string(QString())
				)).done([=, this](const MTPcontacts_ResolvedPeer &res) {
					auto self = weak.lock();
					if (!self || self->isCancelled()) return;

					res.match([&](const MTPDcontacts_resolvedPeer &data) {
						account->session().data().processUsers(data.vusers());
						account->session().data().processChats(data.vchats());
						const auto peerId = peerFromMTP(data.vpeer());
						const auto peer = peerId ? account->session().data().peer(peerId).get() : nullptr;
						if (!peer) {
							log(u"Канал @%1 не найден, переход к следующему..."_q.arg(chUsername));
							delayCall(3000, [=] { (*visitNext)(); });
							return;
						}

						// Human pause inspecting channel header (2-4 seconds)
						const auto headerPause = QRandomGenerator::global()->bounded(2000, 4000);
						delayCall(headerPause, [=, this] {
							auto self2 = weak.lock();
							if (!self2 || self2->isCancelled()) return;

							// Optional Join Channel
							const auto afterJoin = [=, this] {
								// Read posts delay (3-6 seconds)
								const auto readDelay = QRandomGenerator::global()->bounded(3000, 6000);
								delayCall(readDelay, [=, this] {
									auto self3 = weak.lock();
									if (!self3 || self3->isCancelled()) return;

									api->request(MTPmessages_GetHistory(
										peer->input(),
										MTP_int(0),
										MTP_int(0),
										MTP_int(0),
										MTP_int(_config.readPostsPerChannel),
										MTP_int(0),
										MTP_int(0),
										MTP_long(0)
									)).done([=, this](const MTPmessages_Messages &msgsRes) {
										auto self4 = weak.lock();
										if (!self4 || self4->isCancelled()) return;

										auto msgList = QVector<MTPMessage>();
										msgsRes.match(
											[&](const MTPDmessages_messages &d) { msgList = d.vmessages().v; },
											[&](const MTPDmessages_messagesSlice &d) { msgList = d.vmessages().v; },
											[&](const MTPDmessages_channelMessages &d) { msgList = d.vmessages().v; },
											[&](const MTPDmessages_messagesNotModified &) {}
										);

										if (msgList.isEmpty()) {
											delayCall(3000, [=] { (*visitNext)(); });
											return;
										}

										auto msgIds = QVector<MTPint>();
										int highestId = 0;
										for (const auto &m : msgList) {
											m.match([&](const MTPDmessage &msg) {
												msgIds.push_back(msg.vid());
												if (msg.vid().v > highestId) highestId = msg.vid().v;
											}, [](auto &) {});
										}

										// Mark read and get views
										if (highestId > 0) {
											api->request(MTPmessages_ReadHistory(
												peer->input(),
												MTP_int(highestId)
											)).send();
										}
										if (!msgIds.isEmpty()) {
											api->request(MTPmessages_GetMessagesViews(
												peer->input(),
												MTP_vector<MTPint>(msgIds),
												MTP_bool(true)
											)).send();
											*postsReadAcc += int(msgIds.size());
											log(u"Просмотрено %1 постов в @%2"_q.arg(msgIds.size()).arg(chUsername));
										}

										// Realistic reaction with pause
										if (_config.enableReactions && highestId > 0) {
											const auto roll = QRandomGenerator::global()->bounded(100);
											if (roll < _config.reactionChance) {
												const auto emojis = QStringList{ u"👍"_q, u"🔥"_q, u"❤️"_q, u"👏"_q, u"🎉"_q };
												const auto pickedEmoji = emojis[QRandomGenerator::global()->bounded(int(emojis.size()))];
												const auto reactPause = QRandomGenerator::global()->bounded(2000, 4000);
												delayCall(reactPause, [=, this] {
													auto self5 = weak.lock();
													if (!self5 || self5->isCancelled()) return;

													api->request(MTPmessages_SendReaction(
														MTP_flags(0),
														peer->input(),
														MTP_int(highestId),
														MTP_vector<MTPReaction>(1, MTP_reactionEmoji(MTP_string(pickedEmoji)))
													)).done([=, this](const MTPUpdates &) {
														auto self6 = weak.lock();
														if (!self6 || self6->isCancelled()) return;
														log(u"✓ Поставлена реакция %1 на пост в @%2"_q.arg(pickedEmoji).arg(chUsername));
														(*reactionsAcc)++;
													}).send();
												});
											}
										}

										// Pause before next channel
										const auto nextPause = QRandomGenerator::global()->bounded(
											_config.delayMinSec * 1000,
											_config.delayMaxSec * 1000 + 1);
										delayCall(nextPause, [=] { (*visitNext)(); });
									}).fail([=, this](const MTP::Error &) {
										auto self4 = weak.lock();
										if (!self4 || self4->isCancelled()) return;
										delayCall(3000, [=] { (*visitNext)(); });
									}).send();
								});
							};

							if (_config.joinChannels && *joinsAcc < _config.maxJoinsPerAccount) {
								if (const auto ch = peer->asChannel()) {
									if (!ch->amIn()) {
										api->request(MTPchannels_JoinChannel(
											ch->inputChannel()
										)).done([=, this](const MTPmessages_ChatInviteJoinResult &) {
											auto self3 = weak.lock();
											if (!self3 || self3->isCancelled()) return;
											log(u"✓ Подписка на канал @%1"_q.arg(chUsername));
											(*joinsAcc)++;
											delayCall(2000, afterJoin);
										}).fail([=, this](const MTP::Error &) {
											auto self3 = weak.lock();
											if (!self3 || self3->isCancelled()) return;
											afterJoin();
										}).send();
										return;
									}
								}
							}
							afterJoin();
						});
					});
				}).fail([=, this](const MTP::Error &) {
					auto self = weak.lock();
					if (!self || self->isCancelled()) return;
					log(u"Канал @%1 недоступен, пауза..."_q.arg(chUsername));
					delayCall(4000, [=] { (*visitNext)(); });
				}).send();
			});
		};

		(*visitNext)();
	}

	// -------------------------------------------------------------------------
	// MODE 2: MUTUAL ENGLISH DIALOGUE (Realistic Human Pacing, Typos, Reactions)
	// -------------------------------------------------------------------------

	void startMutualDialogues() {
		if (_accounts.size() < 2) {
			log(u"Для взаимной переписки требуется как минимум 2 аккаунта."_q);
			finish(false);
			return;
		}

		log(u"=== Запуск модуля взаимных P2P диалогов на английском языке ==="_q);
		auto pairs = std::vector<std::pair<not_null<Main::Account*>, not_null<Main::Account*>>>();
		for (size_t i = 0; i + 1 < _accounts.size(); i += 2) {
			pairs.push_back({ _accounts[i], _accounts[i + 1] });
		}
		if (_accounts.size() % 2 == 1) {
			// Odd account pairs with the first
			pairs.push_back({ _accounts.back(), _accounts.front() });
		}

		auto pairIdx = std::make_shared<size_t>(0);
		auto runNextPair = std::make_shared<Fn<void()>>();
		*runNextPair = [=, this]() {
			if (_cancelled || *pairIdx >= pairs.size()) {
				log(u"✓ Все взаимные P2P диалоги завершены. Траст аккаунтов на максимуме!"_q);
				finish(true);
				return;
			}
			const auto pair = pairs[*pairIdx];
			(*pairIdx)++;
			if (_progressCb) _progressCb(int(*pairIdx), int(pairs.size()));
			if (_processHandle) _processHandle->progress(int(*pairIdx), int(pairs.size()));

			runP2pPair(pair.first, pair.second, [=, this] {
				const auto p = QRandomGenerator::global()->bounded(4000, 8000);
				delayCall(p, [=] { (*runNextPair)(); });
			});
		};
		(*runNextPair)();
	}

	void runP2pPair(not_null<Main::Account*> a1, not_null<Main::Account*> a2, Fn<void()> done) {
		if (_cancelled) { done(); return; }
		const auto u1 = a1->session().user();
		const auto u2 = a2->session().user();
		if (!u1 || !u2) { done(); return; }

		const auto name1 = u1->name();
		const auto name2 = u2->name();
		log(u"--- Диалог: [%1] ↔ [%2] ---"_q.arg(name1).arg(name2));

		const auto api1 = static_cast<MTP::Sender*>(&a1->session().api());
		const auto api2 = static_cast<MTP::Sender*>(&a2->session().api());
		auto weak = weak_from_this();

		// Step 1: Account 1 views Account 2's profile photo & bio
		log(u"[%1] Просмотр профиля [%2]..."_q.arg(name1).arg(name2));
		api1->request(MTPusers_GetFullUser(u2->inputUser())).send();

		const auto inspectPause = QRandomGenerator::global()->bounded(3000, 5000);
		delayCall(inspectPause, [=, this] {
			auto self = weak.lock();
			if (!self || self->isCancelled()) return;

			// Pick a natural English conversation scenario
			const auto scripts = NaturalDialogueScripts();
			const auto script = scripts[QRandomGenerator::global()->bounded(int(scripts.size()))];

			auto turnIndex = std::make_shared<size_t>(0);
			const auto maxTurns = std::min(script.turns.size(), size_t(std::clamp(_config.dialogueTurns, 1, 4)));

			auto runTurn = std::make_shared<Fn<void()>>();
			*runTurn = [=, this]() {
				if (_cancelled || *turnIndex >= maxTurns) {
					UpdateAccountWarmupStats(a1, 0, 1, 0, 1, 0);
					UpdateAccountWarmupStats(a2, 0, 1, 0, 1, 0);
					done();
					return;
				}

				const auto &turn = script.turns[*turnIndex];
				(*turnIndex)++;

				// 1. Account 1 simulates typing with human delays & typos
				log(u"[%1] набирает сообщение..."_q.arg(name1));
				api1->request(MTPmessages_SetTyping(
					MTP_flags(MTPmessages_SetTyping::Flags(0)),
					u2->input(),
					MTP_int(0),
					MTP_sendMessageTypingAction()
				)).send();

				const auto typingDuration1 = QRandomGenerator::global()->bounded(3500, 6000);
				delayCall(typingDuration1, [=, this] {
					auto self2 = weak.lock();
					if (!self2 || self2->isCancelled()) return;

					// Send message from A1 to A2
					const auto h1 = a1->session().data().history(u2);
					const auto randomId1 = QRandomGenerator::global()->generate64();
					h1->owner().histories().sendPreparedMessage(
						h1,
						FullReplyTo(),
						randomId1,
						Data::Histories::PrepareMessage<MTPmessages_SendMessage>(
							MTP_flags(MTPmessages_SendMessage::Flags(0)),
							u2->input(),
							Data::Histories::ReplyToPlaceholder(),
							MTP_string(turn.textA),
							MTP_long(randomId1),
							MTPReplyMarkup(),
							MTPVector<MTPMessageEntity>(),
							MTP_int(0),
							MTP_int(0),
							MTP_inputPeerEmpty(),
							MTPInputQuickReplyShortcut(),
							MTP_long(0),
							MTP_long(0),
							MTPSuggestedPost(),
							MTPInputRichMessage()),
						[=, this](const MTPUpdates &updates, const MTP::Response &) {
							auto self3 = weak.lock();
							if (!self3 || self3->isCancelled()) return;
							log(u"[%1] -> [%2]: «%3»"_q.arg(name1).arg(name2).arg(turn.textA));

							// 2. Account 2 reading delay (3-6 sec)
							const auto readPause = QRandomGenerator::global()->bounded(3000, 6000);
							delayCall(readPause, [=, this] {
								auto self4 = weak.lock();
								if (!self4 || self4->isCancelled()) return;

								// Optional reaction from B on A's message
								if (_config.enableDialogueReactions && !turn.reactionB.isEmpty()) {
									// Extract sent message id if available from updates
									int sentMsgId = 0;
									updates.match([&](const MTPDupdates &d) {
										for (const auto &u : d.vupdates().v) {
											u.match([&](const MTPDupdateNewMessage &unm) {
												unm.vmessage().match([&](const MTPDmessage &m) {
													sentMsgId = m.vid().v;
												}, [](auto &) {});
											}, [](auto &) {});
										}
									}, [](auto &) {});

									if (sentMsgId > 0) {
										api2->request(MTPmessages_SendReaction(
											MTP_flags(0),
											u1->input(),
											MTP_int(sentMsgId),
											MTP_vector<MTPReaction>(1, MTP_reactionEmoji(MTP_string(turn.reactionB)))
										)).done([=, this](const MTPUpdates &) {
											auto self5 = weak.lock();
											if (!self5 || self5->isCancelled()) return;
											log(u"[%1] поставил реакцию %2 на сообщение"_q.arg(name2).arg(turn.reactionB));
										}).send();
									}
								}

								// 3. Account 2 simulates typing reply (3-5 sec)
								log(u"[%1] набирает ответ..."_q.arg(name2));
								api2->request(MTPmessages_SetTyping(
									MTP_flags(MTPmessages_SetTyping::Flags(0)),
									u1->input(),
									MTP_int(0),
									MTP_sendMessageTypingAction()
								)).send();

								const auto typingDuration2 = QRandomGenerator::global()->bounded(3000, 5500);
								delayCall(typingDuration2, [=, this] {
									auto self6 = weak.lock();
									if (!self6 || self6->isCancelled()) return;

									// Send reply from A2 to A1
									const auto h2 = a2->session().data().history(u1);
									const auto randomId2 = QRandomGenerator::global()->generate64();
									h2->owner().histories().sendPreparedMessage(
										h2,
										FullReplyTo(),
										randomId2,
										Data::Histories::PrepareMessage<MTPmessages_SendMessage>(
											MTP_flags(MTPmessages_SendMessage::Flags(0)),
											u1->input(),
											Data::Histories::ReplyToPlaceholder(),
											MTP_string(turn.textB),
											MTP_long(randomId2),
											MTPReplyMarkup(),
											MTPVector<MTPMessageEntity>(),
											MTP_int(0),
											MTP_int(0),
											MTP_inputPeerEmpty(),
											MTPInputQuickReplyShortcut(),
											MTP_long(0),
											MTP_long(0),
											MTPSuggestedPost(),
											MTPInputRichMessage()),
										[=, this](const MTPUpdates &updates2, const MTP::Response &) {
											auto self7 = weak.lock();
											if (!self7 || self7->isCancelled()) return;
											log(u"[%1] -> [%2]: «%3»"_q.arg(name2).arg(name1).arg(turn.textB));

											// Optional reaction from A on B's message
											if (_config.enableDialogueReactions && !turn.reactionA.isEmpty()) {
												int sentReplyId = 0;
												updates2.match([&](const MTPDupdates &d) {
													for (const auto &u : d.vupdates().v) {
														u.match([&](const MTPDupdateNewMessage &unm) {
															unm.vmessage().match([&](const MTPDmessage &m) {
																sentReplyId = m.vid().v;
															}, [](auto &) {});
														}, [](auto &) {});
													}
												}, [](auto &) {});

												if (sentReplyId > 0) {
													api1->request(MTPmessages_SendReaction(
														MTP_flags(0),
														u2->input(),
														MTP_int(sentReplyId),
														MTP_vector<MTPReaction>(1, MTP_reactionEmoji(MTP_string(turn.reactionA)))
													)).done([=, this](const MTPUpdates &) {
														auto self8 = weak.lock();
														if (!self8 || self8->isCancelled()) return;
														log(u"[%1] поставил реакцию %2 на ответ"_q.arg(name1).arg(turn.reactionA));
													}).send();
												}
											}

											// Pause before next turn
											const auto turnPause = QRandomGenerator::global()->bounded(4000, 7000);
											delayCall(turnPause, [=] { (*runTurn)(); });
										},
										[=, this](const MTP::Error &err, const MTP::Response &) {
											auto self7 = weak.lock();
											if (!self7 || self7->isCancelled()) return;
											log(u"Ошибка отправки сообщения: "_q + err.type());
											done();
										});
								});
							});
						},
						[=, this](const MTP::Error &err, const MTP::Response &) {
							auto self3 = weak.lock();
							if (!self3 || self3->isCancelled()) return;
							log(u"Ошибка отправки P2P: "_q + err.type());
							done();
						});
				});
			};
			(*runTurn)();
		});
	}

	void finish(bool ok) {
		WarmupIndicatorVariable() = WarmupIndicator::Done;
		if (_processHandle) {
			if (ok) {
				_processHandle->succeed();
			} else {
				_processHandle->fail();
			}
		}
		if (_doneCb) {
			auto cb = std::move(_doneCb);
			cb(ok);
		}
	}

	std::vector<not_null<Main::Account*>> _accounts;
	WarmupConfig _config;
	Fn<void(QString)> _logCb;
	Fn<void(int, int)> _progressCb;
	Fn<void(bool)> _doneCb;

	std::shared_ptr<Core::ProcessHandle> _processHandle;
	bool _cancelled = false;
	int _totalSteps = 0;
	int _currentStep = 0;
};

inline std::shared_ptr<WarmupRunner> &CurrentWarmupRunner() {
	static auto runner = std::shared_ptr<WarmupRunner>();
	return runner;
}

inline not_null<Ui::ScrollArea*> AddWarmupScrollArea(
		not_null<Ui::VerticalLayout*> container,
		int height) {
	auto wrapOwned = object_ptr<Ui::RpWidget>(container);
	const auto wrap = wrapOwned.data();
	const auto scroll = Ui::CreateChild<Ui::ScrollArea>(
		wrap, st::defaultScrollArea);
	wrap->sizeValue(
	) | rpl::on_next([=](QSize size) {
		scroll->setGeometry(0, 0, size.width(), size.height());
	}, wrap->lifetime());
	wrap->resize(wrap->width(), height);
	container->add(std::move(wrapOwned), style::margins(0, 0, 0, style::ConvertScale(8)));
	return scroll;
}

// -----------------------------------------------------------------------------
// UI: MakeWarmupBox (Strictly 2 Modes, Accordion Picker, Safe Stop)
// -----------------------------------------------------------------------------

struct WarmupFormState : AccountPickerState {
	WarmupMode mode = WarmupMode::ChannelsAndGroups;
	WarmupConfig config;
};

inline object_ptr<Ui::BoxContent> MakeWarmupBox(
		not_null<Window::SessionController*> controller,
		std::vector<not_null<Main::Account*>> preselected = {}) {
	return Box([=](not_null<Ui::GenericBox*> box) {
		box->setWidth(st::accountsBoxWidth);
		box->setTitle(rpl::single(u"Прогрев аккаунтов (Бустер траста)"_q));

		const auto content = box->verticalLayout();
		Ui::AddSkip(content, st::defaultVerticalListSkip);

		// If a warmup is already active, show the live progress screen directly
		if (CurrentWarmupRunner() && !CurrentWarmupRunner()->isCancelled()) {
			box->addRow(object_ptr<Ui::FlatLabel>(
				box,
				rpl::single(u"В данный момент выполняется прогрев аккаунтов..."_q),
				st::boxDividerLabel));
			box->addButton(rpl::single(u"Остановить прогрев"_q), [=] {
				if (const auto runner = CurrentWarmupRunner()) {
					runner->cancel();
				}
				box->closeBox();
			});
			box->addButton(tr::lng_close(), [=] { box->closeBox(); });
			return;
		}

		const auto state = box->lifetime().make_state<WarmupFormState>();
		if (!preselected.empty()) {
			for (const auto acc : preselected) {
				state->selected.emplace(acc);
			}
		} else {
			for (const auto &acc : Core::App().domain().orderedAccounts()) {
				if (acc->sessionExists()) {
					state->selected.emplace(acc);
				}
			}
		}

		// 1. MODE SELECTOR (Strictly 2 Modes)
		Ui::AddSubsectionTitle(content, rpl::single(u"1. Выберите режим прогрева"_q));

		const auto modeGroup = std::make_shared<Ui::RadiobuttonGroup>(0);

		struct ModeCard {
			WarmupMode id;
			QString title;
			QString desc;
		};

		const auto modes = std::vector<ModeCard>{
			{
				WarmupMode::ChannelsAndGroups,
				u"1. Просмотр зарубежных каналов и групп"_q,
				u"Аккаунты последовательно просматривают международные каналы, читают посты с человеческой задержкой, ставят реакции и вступают."_q,
			},
			{
				WarmupMode::MutualDialogue,
				u"2. Взаимная переписка между аккаунтами (English)"_q,
				u"Выбранные аккаунты переписываются друг с другом на живом английском языке: просмотр профилей, естественные паузы, реалистичный набор текста с опечатками, реакции и стикеры."_q,
			},
		};

		for (const auto &m : modes) {
			box->addRow(
				object_ptr<Ui::Radiobutton>(
					box,
					modeGroup,
					static_cast<int>(m.id),
					m.title,
					st::defaultCheckbox),
				st::ggtgChoiceRowPadding);

			content->add(
				object_ptr<Ui::FlatLabel>(
					content,
					rpl::single(m.desc),
					st::boxDividerLabel),
				style::margins(style::ConvertScale(48), 0, style::ConvertScale(24), style::ConvertScale(8)));
		}

		// Expandable parameters for Mode 1 (Channels)
		const auto channelsWrap = content->add(
			object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
				content,
				object_ptr<Ui::VerticalLayout>(content)));
		const auto channelsInner = channelsWrap->entity();

		Ui::AddSkip(channelsInner, st::defaultVerticalListSkip);
		Ui::AddSubsectionTitle(channelsInner, rpl::single(u"Параметры просмотра каналов"_q));

		// Join channels
		const auto joinCb = channelsInner->add(
			object_ptr<Ui::Checkbox>(
				channelsInner,
				u"Подписываться на каналы (1–2 на аккаунт)"_q,
				state->config.joinChannels),
			style::margins(style::ConvertScale(24), style::ConvertScale(4), style::ConvertScale(24), style::ConvertScale(2)));

		// Read posts
		const auto postsInput = channelsInner->add(
			object_ptr<Ui::InputField>(
				channelsInner,
				st::defaultInputField,
				rpl::single(u"Просмотров постов в канале (10–25)"_q),
				QString::number(state->config.readPostsPerChannel)),
			style::margins(style::ConvertScale(24), style::ConvertScale(4), style::ConvertScale(24), style::ConvertScale(2)));

		// Reaction chance
		const auto reactsInput = channelsInner->add(
			object_ptr<Ui::InputField>(
				channelsInner,
				st::defaultInputField,
				rpl::single(u"Вероятность реакции (0–100%)"_q),
				QString::number(state->config.reactionChance)),
			style::margins(style::ConvertScale(24), style::ConvertScale(4), style::ConvertScale(24), style::ConvertScale(2)));

		// Safe catalog checkbox
		const auto safeCatalogCb = channelsInner->add(
			object_ptr<Ui::Checkbox>(
				channelsInner,
				u"Использовать безопасный международный каталог (TechCrunch, Nature, BBC...)"_q,
				state->config.useSafeCatalog),
			style::margins(style::ConvertScale(24), style::ConvertScale(4), style::ConvertScale(24), style::ConvertScale(2)));

		// Custom channels
		const auto customInput = channelsInner->add(
			object_ptr<Ui::InputField>(
				channelsInner,
				st::defaultInputField,
				rpl::single(u"Свои каналы (необязательно, через пробел)"_q),
				state->config.customChannels),
			style::margins(style::ConvertScale(24), style::ConvertScale(4), style::ConvertScale(24), style::ConvertScale(6)));

		// Expandable parameters for Mode 2 (Mutual Dialogue)
		const auto dialogueWrap = content->add(
			object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
				content,
				object_ptr<Ui::VerticalLayout>(content)));
		const auto dialogueInner = dialogueWrap->entity();

		Ui::AddSkip(dialogueInner, st::defaultVerticalListSkip);
		Ui::AddSubsectionTitle(dialogueInner, rpl::single(u"Параметры взаимного диалога"_q));

		const auto turnsInput = dialogueInner->add(
			object_ptr<Ui::InputField>(
				dialogueInner,
				st::defaultInputField,
				rpl::single(u"Раундов переписки на пару (1–4)"_q),
				QString::number(state->config.dialogueTurns)),
			style::margins(style::ConvertScale(24), style::ConvertScale(4), style::ConvertScale(24), style::ConvertScale(2)));
		dialogueInner->add(
			object_ptr<Ui::FlatLabel>(
				dialogueInner,
				rpl::single(u"Каждый раунд — это отправка сообщения и ответ собеседника с реалистичной задержкой и набором."_q),
				st::boxDividerLabel),
			style::margins(style::ConvertScale(24), 0, style::ConvertScale(24), style::ConvertScale(6)));

		const auto dialogueReactsCb = dialogueInner->add(
			object_ptr<Ui::Checkbox>(
				dialogueInner,
				u"Ставить реакции на сообщения собеседника"_q,
				state->config.enableDialogueReactions),
			style::margins(style::ConvertScale(24), style::ConvertScale(4), style::ConvertScale(24), style::ConvertScale(2)));

		channelsWrap->toggle(true, anim::type::instant);
		dialogueWrap->toggle(false, anim::type::instant);

		modeGroup->setChangedCallback([=](int val) {
			state->mode = static_cast<WarmupMode>(val);
			channelsWrap->toggle(state->mode == WarmupMode::ChannelsAndGroups, anim::type::normal);
			dialogueWrap->toggle(state->mode == WarmupMode::MutualDialogue, anim::type::normal);
		});

		joinCb->checkedChanges() | rpl::on_next([=](bool v) {
			state->config.joinChannels = v;
		}, joinCb->lifetime());

		safeCatalogCb->checkedChanges() | rpl::on_next([=](bool v) {
			state->config.useSafeCatalog = v;
		}, safeCatalogCb->lifetime());

		dialogueReactsCb->checkedChanges() | rpl::on_next([=](bool v) {
			state->config.enableDialogueReactions = v;
		}, dialogueReactsCb->lifetime());

		Ui::AddSkip(content, st::defaultVerticalListSkip);
		Ui::AddDivider(content);
		Ui::AddSkip(content, st::defaultVerticalListSkip);

		// 2. ACCOUNTS SELECTION WITH ACCORDION
		Ui::AddSubsectionTitle(content, rpl::single(u"2. Аккаунты"_q));
		AddAccountPicker(box, state);

		Ui::AddSkip(content, st::defaultVerticalListSkip);

		// 3. START BUTTON & LIVE OVERLAY
		const auto startBtn = box->addButton(
			rpl::single(u"Начать прогрев"_q),
			[=] {
				if (state->selected.empty()) {
					controller->uiShow()->showToast(u"Выберите хотя бы один аккаунт"_q);
					return;
				}
				if (state->mode == WarmupMode::MutualDialogue && state->selected.size() < 2) {
					controller->uiShow()->showToast(u"Для взаимной переписки выберите как минимум 2 аккаунта"_q);
					return;
				}

				bool ok = false;
				if (int v = postsInput->getLastText().trimmed().toInt(&ok); ok && v > 0) {
					state->config.readPostsPerChannel = v;
				}
				if (int v = reactsInput->getLastText().trimmed().toInt(&ok); ok && v >= 0 && v <= 100) {
					state->config.reactionChance = v;
					state->config.enableReactions = (v > 0);
				}
				if (int v = turnsInput->getLastText().trimmed().toInt(&ok); ok && v > 0) {
					state->config.dialogueTurns = std::clamp(v, 1, 4);
				}
				state->config.customChannels = customInput->getLastText().trimmed();
				state->config.mode = state->mode;

				auto finalCfg = state->config;
				auto selectedAccounts = std::vector<not_null<Main::Account*>>(
					state->selected.begin(),
					state->selected.end());

				controller->uiShow()->showBox(Box([=](not_null<Ui::GenericBox*> progBox) {
					progBox->setWidth(st::accountsBoxWidth);
					progBox->setTitle(rpl::single(u"Прогрев аккаунтов в процессе..."_q));

					const auto pCont = progBox->verticalLayout();
					Ui::AddSkip(pCont, st::defaultVerticalListSkip);

					const auto statusLabel = pCont->add(
						object_ptr<Ui::FlatLabel>(
							pCont,
							rpl::single(u"Инициализация прогрева..."_q),
							st::defaultFlatLabel));

					Ui::AddSkip(pCont, st::defaultVerticalListSkip);

					const auto logScroll = AddWarmupScrollArea(pCont, style::ConvertScale(220));
					const auto logInner = logScroll->setOwnedWidget(object_ptr<Ui::VerticalLayout>(logScroll));

					const auto appendLog = [=](const QString &line) {
						if (!logInner) return;
						logInner->add(
							object_ptr<Ui::FlatLabel>(
								logInner,
								rpl::single(line),
								st::boxDividerLabel),
							style::margins(style::ConvertScale(6), style::ConvertScale(2), style::ConvertScale(6), style::ConvertScale(2)));
						Ui::PostponeCall(logScroll, [=] {
							if (logScroll) logScroll->scrollToY(logScroll->scrollTopMax());
						});
					};

					CurrentWarmupRunner() = std::make_shared<WarmupRunner>(
						selectedAccounts,
						finalCfg,
						appendLog,
						[=](int cur, int tot) {
							if (statusLabel) {
								statusLabel->setText(u"Прогрев: шаг %1 из %2"_q.arg(cur).arg(tot));
							}
						},
						[=](bool ok) {
							if (statusLabel) {
								statusLabel->setText(ok
									? u"Прогрев успешно завершён. Траст всех аккаунтов повышен."_q
									: u"Прогрев завершён или остановлен."_q);
							}
							Ui::PostponeCall([=] {
								CurrentWarmupRunner() = nullptr;
							});
						});

					progBox->addButton(rpl::single(u"Остановить"_q), [=] {
						if (const auto runner = CurrentWarmupRunner()) {
							runner->cancel();
						}
						progBox->closeBox();
					});

					progBox->addButton(tr::lng_close(), [=] {
						progBox->closeBox();
					});

					CurrentWarmupRunner()->start();
				}));

				box->closeBox();
			},
			st::defaultActiveButton);

		box->addButton(tr::lng_close(), [=] { box->closeBox(); });
	});
}

} // namespace Settings
