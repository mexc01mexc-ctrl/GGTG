/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "settings/sms_campaign.h"
#include "settings/sms_campaign_model.h"
#include "settings/sms_campaign_widgets.h"
#include "settings/ggtg_ai_cards.h"
#include "settings/ggtg_ai_translate.h"

#include "api/api_blocked_peers.h"
#include "api/api_common.h"
#include "api/api_premium.h"
#include "api/api_sending.h"
#include "apiwrap.h"
#include "base/call_delayed.h"
#include "base/random.h"
#include "base/unixtime.h"
#include "base/weak_ptr.h"
#include "core/application.h"
#include "core/ggtg_ai.h"
#include "core/ggtg_ai_feedback.h"
#include "core/ggtg_ai_profile.h"
#include "core/ggtg_sms_stats.h"
#include "core/ggtg_sync_transport.h"
#include "core/ggtg_fake_detector.h"
#include "countries/countries_instance.h"
#include "core/process_manager.h"
#include "data/account_spam_status.h"
#include "data/data_changes.h"
#include "data/data_document.h"
#include "api/api_send_progress.h"
#include "data/data_histories.h"
#include "data/data_peer_id.h"
#include "data/data_session.h"
#include "api/api_chat_participants.h"
#include "data/data_user.h"
#include "data/scraped_members_db.h"
#include "data/sms_prompts.h"
#include "data/stickers/data_stickers.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_item_helpers.h"
#include "history/view/history_view_element.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "mtproto/sender.h"
#include "settings/bulk_accounts_actions.h"
#include "ui/boxes/confirm_box.h"
#include "ui/effects/animations.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/ui_utility.h"
#include "ui/userpic_view.h"
#include "ui/vertical_list.h"
#include "ui/widgets/discrete_sliders.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"
#include "window/window_session_controller_link_info.h"
#include "window/window_controller.h"
#include "core/ggtg_log.h"
#include "window/ggtg_click.h"
#include "window/ggtg_widgets.h"
#include "settings/ggtg_ai_agent.h"
#include "settings/ggtg_ai_inbox_box.h"
#include "core/ggtg_paths.h"
#include "core/ggtg_ai_profile.h"
#include <QtCore/QJsonArray>
#include <QtCore/QSaveFile>
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>
#include "ui/widgets/popup_menu.h"
#include "ui/ui_utility.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_widgets.h"
#include "styles/style_window_main_menu.h" // ggtgInfoButton
#include "ui/widgets/buttons.h"
#include "ui/wrap/padding_wrap.h"

#include <cmath>
#include <map>
#include <QtCore/QDateTime>
#include <QtCore/QRegularExpression>
#include <QtGui/QCursor>
#include <QtGui/QMouseEvent>
#include <QtWidgets/QApplication>

namespace Settings {
namespace {

// Default gap between SMS sends (editable in "Режим").
constexpr auto kSmsPauseMin = crl::time(30'000);
constexpr auto kSmsPauseMax = crl::time(40'000);
// "Пауза между аккаунтами": default cooldown before a thread picks up the
// next account, whether the current one did its share or was taken off.
constexpr auto kAccountPauseMin = crl::time(30'000);
constexpr auto kAccountPauseMax = crl::time(40'000);

// Only for a lane with nothing to do: waiting for another lane's send to
// come back, or ending once the list is done. Every step with a recipient,
// skipped ones included, waits the full pause between messages.
constexpr auto kSmsSkipPause = crl::time(2'000);

// How long a single SendSmsTextMessage step (resolve + optional warmup +
// send) can go without calling its done() before it's treated as failed -
// mirrors bulk_accounts_actions.cpp's own kStepTimeout/GuardedDone for the
// same "a request that never calls back must not wedge the lane forever"
// reason, duplicated locally since SmsStepDone's outcome enum isn't the
// shared StepDone.
constexpr auto kSmsStepTimeout = crl::time(180'000);

// How long a queue row's status text takes to fade into its final color.
constexpr auto kSmsStatusFadeDuration = crl::time(240);

// How long CheckAccountSpamStatus waits for @spambot's reply before
// giving up and leaving the account's stored status untouched.
constexpr auto kSpamBotReplyTimeout = crl::time(15'000);


// "Отправить приветственный стикер": how long FetchGreetingSticker waits
// for the first stickers-by-emoji load before giving up on the sticker
// and falling back to text-only, and the gap between the sticker and the
// text that follows it (when there is one).
constexpr auto kGreetingStickerTimeout = crl::time(5'000);
// How long a translation may take before the template goes as it is.
constexpr auto kSmsTranslateTimeout = crl::time(120'000);
constexpr auto kGreetingTextPauseMin = crl::time(10'000);
constexpr auto kGreetingTextPauseMax = crl::time(15'000);
// "[SPLIT]": the short, human-like gap between two parts of one recipient's
// message when the text is split into several separate messages.
constexpr auto kSplitPartPauseMin = crl::time(2'000);
constexpr auto kSplitPartPauseMax = crl::time(5'000);

// The account's counters below are kept for the whole campaign: a success
// in between doesn't wipe them.
//
// This many "user not found" from one account almost always means Telegram
// is throttling *its* username lookups, not that this many people vanished:
// the account is taken off and the recipient gets the "аккаунт не смог"
// mark instead of being written off as not found.
constexpr auto kNotFoundStreakLimit = 5;

// This many failed sends that aren't the recipient's fault (timeouts,
// network, unknown errors) means the account (or its proxy) is broken.
constexpr auto kSmsErrorStreakLimit = 5;
// Telegram refusing this many recipients (PEER_FLOOD, account itself
// "free"): the default of "Снять аккаунт после отказов".
constexpr auto kSmsPeerRefusalStreakLimit = 3;

// The gap between two real sends, from the "Пауза между сообщениями"
// fields (kSmsPauseMin/kSmsPauseMax are only their defaults).
[[nodiscard]] crl::time NextSmsPause(crl::time min, crl::time max) {
	return min + crl::time(base::RandomIndex(int(max - min) + 1));
}

// "Лёгкий прогрев" differs from account to account, like people do: each
// account gets a habit for the whole campaign, and now and then a different
// one for a single recipient. It stays logical: a photo is looked at only
// after the profile is open (and only if there is one), the chat is always
// opened before anything is written, and nothing is skipped that the next
// step needs.
//
//   fast      - goes straight to the chat (found them by @username)
//   usual     - opens the profile, then the chat
//   curious   - opens the profile, looks at the photo, then the chat
struct WarmupPlan {
	bool profile = false;
	bool photo = false;
	double pace = 1.; // multiplies every pause of the warm-up
};

[[nodiscard]] std::map<quint64, int> &WarmupHabits() {
	static auto result = std::map<quint64, int>();
	return result;
}

// A new campaign gives every account a new habit.
inline void ResetWarmupHabits() {
	WarmupHabits().clear();
}

[[nodiscard]] WarmupPlan WarmupPlanFor(
		not_null<Main::Account*> account,
		int protectionMode = 1) {
	constexpr auto kFast = 0;
	constexpr auto kUsual = 1;
	constexpr auto kCurious = 2;
	const auto id = account->session().user()->id.value;
	auto &habits = WarmupHabits();
	auto i = habits.find(id);
	if (i == habits.end()) {
		// Probabilities per GramGPT-style AI protection mode:
		// Safe (0): 10% fast, 40% usual, 50% curious (90% profile view, 50% photo)
		// Fast (2): 70% fast, 15% usual, 15% curious (30% profile view, 15% photo)
		// Optimal (1): 30% fast, 35% usual, 35% curious (70% profile view, 35% photo)
		const auto roll = base::RandomIndex(100);
		const auto habit = (protectionMode == 0)
			? ((roll < 10) ? kFast : (roll < 50) ? kUsual : kCurious)
			: (protectionMode == 2)
			? ((roll < 70) ? kFast : (roll < 85) ? kUsual : kCurious)
			: ((roll < 30) ? kFast : (roll < 65) ? kUsual : kCurious);
		i = habits.emplace(id, habit).first;
	}
	auto style = i->second;
	// One recipient in five is handled differently from the habit, to the
	// neighbouring style (fast <-> usual <-> curious).
	if (base::RandomIndex(5) == 0) {
		style = (style == kUsual)
			? ((base::RandomIndex(2) == 0) ? kFast : kCurious)
			: kUsual;
	}
	auto result = WarmupPlan();
	result.profile = (style != kFast);
	result.photo = (style == kCurious);
	const auto basePace = (protectionMode == 0)
		? 1.35
		: (protectionMode == 2)
		? 0.7
		: 1.0;
	result.pace = basePace * ((style == kFast)
		? 0.7
		: (style == kUsual)
		? 1.
		: 1.25);
	return result;
}

[[nodiscard]] crl::time NextGreetingTextPause() {
	return kGreetingTextPauseMin + crl::time(base::RandomIndex(
		int(kGreetingTextPauseMax - kGreetingTextPauseMin) + 1));
}

[[nodiscard]] crl::time NextSplitPartPause() {
	return kSplitPartPauseMin + crl::time(base::RandomIndex(
		int(kSplitPartPauseMax - kSplitPartPauseMin) + 1));
}

// Reads a "Лимиты и потоки" number field (see SetupLimitsSection) as a
// non-negative int, falling back to `fallback` for empty or non-numeric
// text rather than rejecting the whole campaign over a typo.
[[nodiscard]] int ParseNonNegative(
		const QPointer<Ui::InputField> &field,
		int fallback) {
	if (!field) {
		return fallback;
	}
	auto ok = false;
	const auto value = field->getLastText().trimmed().toInt(&ok);
	return (ok && value >= 0) ? value : fallback;
}

// A "от-до" field holding a range of seconds: "40-60", "40 - 60" or a
// single "40" (min == max). Hyphen/en-dash/em-dash all split it. Missing
// or unparseable parts fall back to the given defaults, and the result is
// always ordered (min <= max) with min >= 1.
[[nodiscard]] std::pair<int, int> ParseSecondsRange(
		const QPointer<Ui::InputField> &field,
		int fallbackMin,
		int fallbackMax) {
	if (!field) {
		return { fallbackMin, fallbackMax };
	}
	const auto raw = field->getLastText().trimmed();
	static const auto re = QRegularExpression(u"[-–—]"_q);
	const auto parts = raw.split(re, Qt::SkipEmptyParts);
	const auto toInt = [](const QString &s, int fallback) {
		auto ok = false;
		const auto value = s.trimmed().toInt(&ok);
		return (ok && value >= 0) ? value : fallback;
	};
	auto mn = fallbackMin;
	auto mx = fallbackMax;
	if (parts.size() >= 2) {
		mn = toInt(parts[0], fallbackMin);
		mx = toInt(parts[1], fallbackMax);
	} else if (parts.size() == 1) {
		mn = mx = toInt(parts[0], fallbackMin);
	}
	mn = std::max(1, mn);
	mx = std::max(mn, mx);
	return { mn, mx };
}

enum class SmsErrorKind {
	Other, // unknown / network / timeout - counts toward the error streak
	FloodWait, // FLOOD_WAIT_x and friends - the account must back off
	AccountDead, // the session/account itself is gone or banned
	RecipientOnly, // this recipient can't be messaged; account is fine
	Text, // the text itself was refused (too long, bad markup): not the
	      // account's fault, so it doesn't count toward the error streak
};

// Seconds from FLOOD_WAIT_30 / FLOOD_PREMIUM_WAIT_30 / SLOWMODE_WAIT_30,
// or 0 when `type` isn't one of those.
[[nodiscard]] int FloodWaitSeconds(const QString &type) {
	static const auto re = QRegularExpression(
		u"^(?:FLOOD|FLOOD_PREMIUM|SLOWMODE)_WAIT_(\\d+)$"_q);
	const auto match = re.match(type);
	return match.hasMatch() ? std::max(1, match.captured(1).toInt()) : 0;
}

[[nodiscard]] SmsErrorKind ClassifySmsError(const QString &type) {
	if (FloodWaitSeconds(type) > 0) {
		return SmsErrorKind::FloodWait;
	}
	static const auto dead = {
		u"AUTH_KEY_UNREGISTERED"_q,
		u"AUTH_KEY_INVALID"_q,
		u"AUTH_KEY_DUPLICATED"_q,
		u"SESSION_REVOKED"_q,
		u"SESSION_EXPIRED"_q,
		u"USER_DEACTIVATED"_q,
		u"USER_DEACTIVATED_BAN"_q,
		u"PHONE_NUMBER_BANNED"_q,
		// A frozen account can only read; one asking for its 2FA password
		// has no working session.
		u"FROZEN_METHOD_INVALID"_q,
		u"FROZEN_PARTICIPANT_MISSING"_q,
		u"SESSION_PASSWORD_NEEDED"_q,
	};
	static const auto recipientOnly = {
		u"USER_PRIVACY_RESTRICTED"_q,
		u"USER_IS_BLOCKED"_q,
		u"YOU_BLOCKED_USER"_q,
		u"INPUT_USER_DEACTIVATED"_q,
		u"PEER_ID_INVALID"_q,
		u"USER_NOT_MUTUAL_CONTACT"_q,
		u"PRIVACY_PREMIUM_REQUIRED"_q,
	};
	static const auto text = {
		u"MESSAGE_TOO_LONG"_q,
		u"MESSAGE_EMPTY"_q,
		u"ENTITY_BOUNDS_INVALID"_q,
		u"ENTITY_MENTION_USER_INVALID"_q,
		u"ENTITIES_TOO_LONG"_q,
	};
	if (ranges::contains(dead, type)) {
		return SmsErrorKind::AccountDead;
	} else if (ranges::contains(recipientOnly, type)
		// The recipient takes Stars for messages (ALLOW_PAYMENT_REQUIRED_N).
		|| type.startsWith(u"ALLOW_PAYMENT_REQUIRED"_q)) {
		return SmsErrorKind::RecipientOnly;
	} else if (ranges::contains(text, type)) {
		return SmsErrorKind::Text;
	}
	return SmsErrorKind::Other;
}

// A failed send as the campaign window shows it: what Telegram's error
// means in plain words, with the raw code kept in brackets.
[[nodiscard]] QString HumanSmsError(const QString &type) {
	if (type.isEmpty()) {
		return u"неизвестная"_q;
	}
	const auto reason = [&]() -> QString {
		if (type == u"USER_PRIVACY_RESTRICTED"_q) {
			return u"получатель закрыл личные сообщения от незнакомых"_q;
		} else if (type == u"USER_NOT_MUTUAL_CONTACT"_q) {
			return u"писать можно только общим контактам"_q;
		} else if (type == u"PRIVACY_PREMIUM_REQUIRED"_q) {
			return u"получатель принимает сообщения только от Premium"_q;
		} else if (type == u"USER_IS_BLOCKED"_q
				|| type == u"YOU_BLOCKED_USER"_q) {
			return u"получатель заблокировал аккаунт (или он его)"_q;
		} else if (type == u"INPUT_USER_DEACTIVATED"_q) {
			return u"аккаунт получателя удалён"_q;
		} else if (type.startsWith(u"ALLOW_PAYMENT_REQUIRED"_q)) {
			return u"получатель берёт звёзды за сообщения"_q;
		} else if (type.startsWith(u"FROZEN_"_q)) {
			return u"аккаунт заморожен Telegram"_q;
		} else if (type == u"SESSION_PASSWORD_NEEDED"_q) {
			return u"сессии нужен пароль 2FA"_q;
		} else if (type == u"MESSAGE_TOO_LONG"_q) {
			return u"текст длиннее 4096 символов"_q;
		} else if (type == u"PEER_FLOOD"_q) {
			return u"Telegram не дал написать этому человеку (PEER_FLOOD)"_q;
		} else if (FloodWaitSeconds(type) > 0) {
			return u"слишком частые запросы, ждать "_q
				+ QString::number(FloodWaitSeconds(type)) + u" сек"_q;
		}
		return QString();
	}();
	return reason.isEmpty() ? type : (reason + u" ["_q + type + ']');
}

// The whole "Отправка SMS" screen's mutable state, owned by the box
// (box->lifetime().make_state<CampaignState>()) - every Setup*Section
// helper below both reads and writes into it.
struct CampaignState {
	// АККАУНТЫ
	base::flat_set<not_null<Main::Account*>> selectedAccounts;
	base::flat_set<QString> expandedFolders;
	QPointer<Ui::VerticalLayout> accountsAccordion;

	// One-line summaries of the five steps, shown on their folded cards.
	rpl::variable<QString> accountsSummary;
	rpl::variable<QString> recipientsSummary;
	rpl::variable<QString> textSummary;
	rpl::variable<QString> protectionSummary;
	rpl::variable<QString> limitsSummary;

	// SPAM-статус: at most one @spambot check (a single row's click, a
	// folder's "Проверить", or the top-level "Проверить статус всех") runs
	// at a time - see RunAccountSpamCheck/RunSpamCheckQueue. Every row and
	// header watches spamCheckActivityChanged to know when to redraw its
	// own "Проверка..." state, since a queued check can be probing an
	// account whose row wasn't the one that started it.
	std::shared_ptr<Ui::Show> show;
	bool spamCheckBusy = false;
	Main::Account *spamCheckingAccount = nullptr;
	rpl::event_stream<> spamCheckActivityChanged;
	rpl::variable<QString> checkAllButtonText = u"Проверить статус"_q;

	// ПОЛУЧАТЕЛИ
	std::vector<Data::ScrapedGroupMeta> basesIndex;
	base::flat_set<uint64> selectedBaseIds;
	QPointer<Ui::VerticalLayout> basesContainer;
	QPointer<Ui::Checkbox> allListsCheckbox;
	std::vector<QPointer<Ui::Checkbox>> baseCheckboxes;
	QPointer<Ui::FlatLabel> recipientsStatsLabel;

	// РЕЖИМ (read by the "Запустить" handler)
	QPointer<Ui::InputField> limitPerAccount;
	QPointer<Ui::InputField> limitThreads;
	QPointer<Ui::InputField> limitSkipSpamBlock;
	// Refusals after which an account is taken off (0 - never).
	QPointer<Ui::InputField> limitRefusals;
	// One "от-до" field each (e.g. "40-60"), parsed by ParseSecondsRange.
	QPointer<Ui::InputField> limitPauseRange;
	QPointer<Ui::InputField> limitAccountPauseRange;
	// "normal", "premium" or "custom" - see LimitsPresets().
	QString limitMode;
	// "Готово к запуску..." / what is still missing, refreshed by a timer
	// and right away by whatever changes it.
	QPointer<Ui::FlatLabel> preflight;
	Fn<void()> refreshPreflight;

	// ПОВЕДЕНИЕ ПРИ ОТПРАВКЕ (read by SendSmsTextMessage). "Лёгкий прогрев"
	// is always on now, so it has no checkbox; the greeting-sticker toggle
	// moved into the "3. Текст сообщения" section.
	QPointer<Ui::Checkbox> greetingStickerCheckbox;

	// ТЕКСТ СООБЩЕНИЯ
	QPointer<Ui::InputField> messageField;
	QPointer<Ui::FlatLabel> messagePreview;
	QPointer<Ui::FlatLabel> messageWarning;
	// Saved prompts to rotate through, in turn, instead of the message
	// field (empty = rotation off).
	std::vector<uint64> rotationPromptIds;
	QPointer<Ui::FlatLabel> rotationLabel;

	// ИИ-ЗАЩИТА АККАУНТОВ
	int protectionMode = 1;
	std::shared_ptr<Ui::RadiobuttonGroup> protectionGroup;
	QPointer<Ui::Checkbox> nightSleepCheckbox;
	QPointer<Ui::Checkbox> simulateTyposCheckbox;
	QPointer<Ui::Checkbox> filterByGgrCheckbox;
	QPointer<Ui::Checkbox> skipSuspectedFakesCheckbox;

	// Last saved values of this whole form (see SaveFormState) and the base
	// ids known at that time - lets a reopened box pick up where it was
	// left instead of resetting to defaults.
	Data::SmsFormSettings saved;
	base::flat_set<uint64> knownBaseIds;
	bool basesRefreshQueued = false;
};

// Pulls the persisted selection (accounts, checked lists) back into
// `state` before any section builds its widgets from it; text-field
// values are applied by the sections themselves, from `state->saved`.
void RestoreFormState(not_null<CampaignState*> state) {
	state->saved = Data::LoadSmsForm();
	if (!state->saved.saved) {
		return;
	}
	state->protectionMode = state->saved.protectionMode;
	const auto &ids = state->saved.accountUserIds;
	for (const auto &account : Core::App().domain().orderedAccounts()) {
		if (account->sessionExists()
			&& ranges::contains(ids, account->session().user()->id.value)) {
			state->selectedAccounts.emplace(account);
		}
	}
	for (const auto id : state->saved.selectedBaseIds) {
		state->selectedBaseIds.emplace(id);
	}
	for (const auto id : state->saved.knownBaseIds) {
		state->knownBaseIds.emplace(id);
	}
	state->rotationPromptIds = state->saved.rotationPromptIds;
}

void SaveFormState(not_null<CampaignState*> state) {
	auto form = Data::SmsFormSettings();
	form.saved = true;
	for (const auto &account : Main::LiveAccounts(state->selectedAccounts)) {
		form.accountUserIds.push_back(account->session().user()->id.value);
	}
	form.selectedBaseIds.assign(
		state->selectedBaseIds.begin(),
		state->selectedBaseIds.end());
	form.knownBaseIds.assign(
		state->knownBaseIds.begin(),
		state->knownBaseIds.end());
	const auto text = [](const QPointer<Ui::InputField> &field) {
		return field ? field->getLastText() : QString();
	};
	form.limitPerAccount = text(state->limitPerAccount);
	form.limitThreads = text(state->limitThreads);
	form.limitSkipSpamBlock = text(state->limitSkipSpamBlock);
	form.limitRefusals = text(state->limitRefusals);
	const auto [pauseMin, pauseMax] = ParseSecondsRange(
		state->limitPauseRange,
		int(kSmsPauseMin / 1000),
		int(kSmsPauseMax / 1000));
	form.pauseMin = QString::number(pauseMin);
	form.pauseMax = QString::number(pauseMax);
	const auto [accPauseMin, accPauseMax] = ParseSecondsRange(
		state->limitAccountPauseRange,
		int(kAccountPauseMin / 1000),
		int(kAccountPauseMax / 1000));
	form.pauseAccountMin = QString::number(accPauseMin);
	form.pauseAccountMax = QString::number(accPauseMax);
	form.limitMode = state->limitMode;
	form.rotationPromptIds = state->rotationPromptIds;
	form.protectionMode = state->protectionMode;
	form.nightSleep = state->nightSleepCheckbox
		? state->nightSleepCheckbox->checked()
		: true;
	form.simulateTypos = state->simulateTyposCheckbox
		? state->simulateTyposCheckbox->checked()
		: true;
	form.filterByGgr = state->filterByGgrCheckbox
		? state->filterByGgrCheckbox->checked()
		: false;
	form.skipSuspectedFakes = state->skipSuspectedFakesCheckbox
		? state->skipSuspectedFakesCheckbox->checked()
		: true;
	// "Лёгкий прогрев" is always on now (no checkbox) - see SetupTextSection.
	form.warmup = true;
	form.greetingSticker = state->greetingStickerCheckbox
		&& state->greetingStickerCheckbox->checked();
	form.text = text(state->messageField);
	Data::SaveSmsForm(form);
}

// ---- template rendering: [NAME]/[SURNAME]/[MYNAME]/[MYSURNAME]/[GROUP] + {a|b} ----

// Splits the inside of a {a|b|c} group on '|', but only outside [...], so a
// [NAME|fallback] token inside an option isn't torn apart.
[[nodiscard]] QStringList SplitSpintaxOptions(const QString &group) {
	auto result = QStringList();
	auto current = QString();
	auto depth = 0;
	for (const auto ch : group) {
		if (ch == '[') {
			++depth;
		} else if (ch == ']' && depth > 0) {
			--depth;
		}
		if (ch == '|' && depth == 0) {
			result.push_back(current);
			current.clear();
		} else {
			current += ch;
		}
	}
	result.push_back(current);
	return result;
}

// The actual rendering, on plain strings so the setup box's live preview
// can run the very same code with sample names. Besides the four tokens
// there is [NAME|fallback] (any token accepts one): the fallback is used
// when that name is empty, e.g. "Привет, [NAME|друг]!" for a recipient
// with no first name.
[[nodiscard]] QString RenderSmsTemplateText(
		const QString &textTemplate,
		const QString &recipientFirst,
		const QString &recipientLast,
		const QString &senderFirst,
		const QString &senderLast,
		const QString &groupName = QString()) {
	auto result = textTemplate;
	static const auto groupRe = QRegularExpression(u"\\{([^{}]*)\\}"_q);
	for (;;) {
		const auto match = groupRe.match(result);
		if (!match.hasMatch()) {
			break;
		}
		const auto options = SplitSpintaxOptions(match.captured(1));
		const auto chosen = options[base::RandomIndex(options.size())];
		result.replace(match.capturedStart(0), match.capturedLength(0), chosen);
	}
	// Tokens are substituted in a single pass over the group-free text, so
	// a name that itself contains "[NAME]" or "{a|b}" is inserted verbatim
	// instead of being interpreted.
	static const auto tokenRe = QRegularExpression(
		u"\\[(NAME|SURNAME|MYNAME|MYSURNAME|GROUP)(?:\\|([^\\]]*))?\\]"_q);
	// [NAME] is a name fit for a greeting: no emoji or symbols from the
	// profile name, and for an obviously not-a-name ("qqqq", one letter,
	// "asdf") - "друг" (in the language of the text) unless the template
	// gave its own [NAME|fallback].
	const auto greetingName = Core::GgtgAi::CleanPersonName(recipientFirst);
	auto out = QString();
	auto last = 0;
	auto blank = false;
	auto it = tokenRe.globalMatch(result);
	while (it.hasNext()) {
		const auto match = it.next();
		out += result.mid(last, match.capturedStart(0) - last);
		const auto which = match.captured(1);
		const auto &value = (which == u"NAME"_q)
			? greetingName
			: (which == u"SURNAME"_q)
			? recipientLast
			: (which == u"MYNAME"_q)
			? senderFirst
			: (which == u"GROUP"_q)
			? groupName
			: senderLast;
		auto fallback = match.captured(2);
		if (which == u"NAME"_q && value.isEmpty() && fallback.isEmpty()) {
			fallback = Core::GgtgAi::FriendWord(result);
		}
		if (value.isEmpty() && fallback.isEmpty()) {
			blank = true;
		}
		out += value.isEmpty() ? fallback : value;
		last = match.capturedEnd(0);
	}
	out += result.mid(last);
	static const auto spacesRe = QRegularExpression(u" {2,}"_q);
	out.replace(spacesRe, u" "_q);
	if (blank) {
		// "Привет , как дела" -> "Привет, как дела" after an empty name.
		static const auto beforePunctRe = QRegularExpression(
			u" +([,.!?;:])"_q);
		out.replace(beforePunctRe, u"\\1"_q);
	}
	return out.trimmed();
}

// `recipient` is null when the resolved peer isn't a user (shouldn't
// normally happen - these are all usernames typed/scraped as people) -
// the name placeholders just come out empty in that case.
[[nodiscard]] QString RenderSmsTemplate(
		const QString &textTemplate,
		UserData *recipient,
		not_null<UserData*> sender,
		const QString &groupName = QString()) {
	return RenderSmsTemplateText(
		textTemplate,
		recipient ? recipient->firstName : QString(),
		recipient ? recipient->lastName : QString(),
		sender->firstName,
		sender->lastName,
		groupName);
}

// "[SPLIT]": a divider the user drops into the message so one recipient
// gets several separate messages from the same account instead of one big
// block. Splits already-rendered text on [SPLIT], trims each piece and
// drops the empty ones. No [SPLIT] (or an empty text) yields a single
// element - possibly empty, which the callers treat as "nothing to send".
[[nodiscard]] QStringList SplitRenderedMessage(const QString &rendered) {
	auto result = QStringList();
	const auto parts = rendered.split(u"[SPLIT]"_q);
	for (const auto &part : parts) {
		const auto trimmed = part.trimmed();
		if (!trimmed.isEmpty()) {
			result.push_back(trimmed);
		}
	}
	if (result.isEmpty()) {
		result.push_back(rendered.trimmed());
	}
	return result;
}

// True when a parts list (see SplitRenderedMessage) carries no actual text
// - a sticker-only greeting, or a template that rendered empty.
[[nodiscard]] bool MessagePartsEmpty(const QStringList &parts) {
	return parts.isEmpty()
		|| (parts.size() == 1 && parts.front().isEmpty());
}

// Static checks for the message field, shown live under it and enforced on
// "Отправить" for the `error` case (a warning is only a heads-up: e.g. an
// unknown "[word]" might be meant literally).
struct SmsTemplateCheck {
	QString error;
	QString warning;
};

// Telegram's cap on a single message's text.
constexpr auto kSmsMaxTextLength = 4096;

[[nodiscard]] SmsTemplateCheck CheckSmsTemplate(
		const QString &text,
		int renderedLength) {
	auto result = SmsTemplateCheck();
	auto depth = 0;
	for (const auto ch : text) {
		if (ch == '{') {
			if (depth > 0) {
				result.error = u"вложенные {…} не поддерживаются"_q;
				break;
			}
			++depth;
		} else if (ch == '}') {
			if (depth == 0) {
				result.error = u"лишняя закрывающая }"_q;
				break;
			}
			--depth;
		}
	}
	if (result.error.isEmpty() && depth > 0) {
		result.error = u"не закрыта {"_q;
	}
	if (result.error.isEmpty() && renderedLength > kSmsMaxTextLength) {
		result.error = u"текст длиннее "_q + QString::number(kSmsMaxTextLength)
			+ u" символов (сейчас "_q + QString::number(renderedLength) + ')';
	}
	if (result.error.isEmpty()) {
		static const auto knownTokens = {
			u"NAME"_q, u"SURNAME"_q, u"MYNAME"_q, u"MYSURNAME"_q,
			u"GROUP"_q, u"SPLIT"_q,
		};
		static const auto wordRe = QRegularExpression(
			u"\\[([A-Za-z_]+)(?:\\|[^\\]]*)?\\]"_q);
		auto it = wordRe.globalMatch(text);
		while (it.hasNext()) {
			const auto word = it.next().captured(1);
			if (!ranges::contains(knownTokens, word)) {
				result.warning = u"[%1] - неизвестный шаблон, уйдёт как "
					"есть"_q.arg(word);
				break;
			}
		}
	}
	return result;
}

// ---- sending: resolve a username, send a message/template to it ----

// Mirrors Window::SessionNavigation::resolveUsername, but bound to an
// arbitrary (possibly inactive) account's own session/API instead of the
// currently active window - the whole point of picking an account from a
// folder first.
void ResolveUsernameForAccount(
		not_null<Main::Account*> account,
		QString username,
		Fn<void(PeerData *peer, QString error)> done) {
	const auto session = &account->session();
	if (const auto peer = session->data().peerByUsername(username)) {
		done(peer, QString());
		return;
	}
	const auto api = static_cast<MTP::Sender*>(&session->api());
	// The session's own ApiWrap sends it: it lives as long as the session
	// and cancels what is still pending when the session goes. A Sender
	// owned by the request's own handlers (as here before) was destroyed
	// from inside the MTP instance's destructor on quit and crashed there.
	// handleFloodErrors(): without it MTP silently waits out FLOOD_WAIT
	// itself, for up to hours - resolveUsername has a daily limit (about
	// 200 names per account), after which a base check or a campaign just
	// seemed to hang. The callers deal with FLOOD_WAIT on their own.
	api->request(MTPcontacts_ResolveUsername(
		MTP_flags(MTPcontacts_ResolveUsername::Flags(0)),
		MTP_string(username),
		MTP_string(QString())
	)).handleFloodErrors().done([=, api = api](
			const MTPcontacts_ResolvedPeer &result) {
		result.match([&](const MTPDcontacts_resolvedPeer &data) {
			session->data().processUsers(data.vusers());
			session->data().processChats(data.vchats());
			const auto peerId = peerFromMTP(data.vpeer());
			if (peerId) {
				done(session->data().peer(peerId), QString());
			} else {
				done(nullptr, QString());
			}
		});
	}).fail([=, api = api](const MTP::Error &error) {
		done(nullptr, error.type());
	}).send();
}

void SendTextToPeer(
		not_null<Main::Account*> account,
		not_null<PeerData*> peer,
		const QString &text,
		StepDone done) {
	const auto session = &account->session();
	const auto history = session->data().history(peer);
	const auto randomId = base::RandomValue<uint64>();
	// The message is put into the chat right away, as the app's own sending
	// does: for a text in a private chat the server answers only "sent,
	// id N" (updateShortSentMessage), which fills in a local message found
	// by its random id. Without one the text did go out, but was not seen
	// in this app until a restart - "the sticker is there, the text is not".
	const auto localId = FullMsgId(
		peer->id,
		session->data().nextLocalMessageId());
	session->data().registerMessageRandomId(randomId, localId);
	session->data().registerMessageSentData(randomId, peer->id, text);
	history->addNewLocalMessage({
		.id = localId.msg,
		.flags = NewMessageFlags(peer),
		.from = session->userPeerId(),
		.date = base::unixtime::now(),
	}, TextWithEntities{ text }, MTP_messageMediaEmpty());
	const auto dropLocal = [=] {
		session->data().unregisterMessageRandomId(randomId);
		if (const auto item = session->data().message(localId)) {
			item->destroy();
		}
	};
	history->owner().histories().sendPreparedMessage(
		history,
		FullReplyTo(),
		randomId,
		Data::Histories::PrepareMessage<MTPmessages_SendMessage>(
			MTP_flags(MTPmessages_SendMessage::Flags(0)),
			peer->input(),
			Data::Histories::ReplyToPlaceholder(),
			MTP_string(text),
			MTP_long(randomId),
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
		[=](const MTPUpdates &, const MTP::Response &) {
			done(true, QString());
		},
		[=](const MTP::Error &sendError, const MTP::Response &) {
			// Not left in the chat as if it were on its way.
			dropLocal();
			done(false, sendError.type());
		});
}

// How long the "печатает..." indicator is shown for a text: grows with its
// length, like a person composing it.
[[nodiscard]] crl::time TypingPause(const QString &text) {
	return std::clamp(
		crl::time(text.size()) * 50,
		crl::time(1'500),
		crl::time(7'000)) + crl::time(base::RandomIndex(1'000));
}

// What a send in progress is told and asks about: what it is doing now
// (the "Поток N" line), whether it may still go on (`alive`: not stopped,
// not timed out, the session still there) and that one more message
// actually reached the recipient (`delivered`).
struct SmsSendControl {
	Fn<void(QString)> stage;
	Fn<bool()> alive;
	Fn<void()> delivered;
	int protectionMode = 1;
	bool simulateTypos = true;
};

// What `done` gets when the send was called off before its next message.
[[nodiscard]] QString SmsAbortedError() {
	return u"GGTG_ABORTED"_q;
}

// Sends `parts` (see SplitRenderedMessage) to one recipient one after
// another, with a short human-like gap between them, reporting the first
// failure or the final success through `done`. Guarded by Core::App() (not
// the account's session) so a session torn down during a gap is reported
// as a failed step instead of silently dropping the rest.
//
// Every part is "typed" first: the real typing indicator is shown to the
// recipient for TypingPause(). Before every request it checks it may still
// go on.
void SendTextPartsToPeer(
		not_null<Main::Account*> account,
		not_null<PeerData*> peer,
		QStringList parts,
		int index,
		StepDone done,
		SmsSendControl control) {
	if (index >= int(parts.size())) {
		done(true, QString());
		return;
	}
	const auto say = [=](const QString &step) {
		if (control.stage) {
			control.stage(step);
		}
	};
	const auto stopped = [=] {
		return control.alive && !control.alive();
	};
	if (stopped()) {
		done(false, SmsAbortedError());
		return;
	}
	const auto session = &account->session();
	const auto history = session->data().history(peer);
	const auto count = int(parts.size());
	const auto which = (count > 1)
		? u" %1 из %2"_q.arg(index + 1).arg(count)
		: QString();

	const auto sendNow = [=] {
		if (!Main::AccountIsLive(account)) {
			say(u"текст НЕ ушёл: сессия закрылась"_q);
			done(false, u"сессия аккаунта закрылась во время набора"_q);
			return;
		} else if (stopped()) {
			done(false, SmsAbortedError());
			return;
		}
		say(u"отправляет текст"_q + which);
		SendTextToPeer(account, peer, parts[index], [=](
				bool success,
				QString message) {
			if (!success) {
				say(u"текст НЕ ушёл: "_q + message);
				done(false, message);
				return;
			}
			// Only now, with the server's answer in hand.
			say(u"отправил текст"_q + which);
			if (control.delivered) {
				control.delivered();
			}
			if (index + 1 >= count) {
				done(true, QString());
				return;
			}
			base::call_delayed(NextSplitPartPause(), &Core::App(), [=] {
				if (!Main::AccountIsLive(account)) {
					done(false, u"сессия аккаунта закрылась во время паузы"_q);
					return;
				}
				SendTextPartsToPeer(
					account,
					peer,
					parts,
					index + 1,
					done,
					control);
			});
		});
	};

	say(u"печатает текст"_q + which);
	session->sendProgressManager().update(
		history,
		Api::SendProgressType::Typing);

	const auto typoChance = (control.protectionMode == 0)
		? 8
		: (control.protectionMode == 2)
		? 2
		: 5;
	const auto simulateTypo = control.simulateTypos
		&& (base::RandomIndex(100) < typoChance);

	if (simulateTypo) {
		const auto firstPause = crl::time(1500 + base::RandomIndex(1000));
		base::call_delayed(firstPause, &Core::App(), [=] {
			if (!Main::AccountIsLive(account) || stopped()) {
				sendNow();
				return;
			}
			say(u"исправляет опечатку..."_q);
			const auto fixPause = crl::time(1200 + base::RandomIndex(1000));
			base::call_delayed(fixPause, &Core::App(), [=] {
				if (!Main::AccountIsLive(account) || stopped()) {
					sendNow();
					return;
				}
				say(u"печатает текст"_q + which);
				account->session().sendProgressManager().update(
					account->session().data().history(peer),
					Api::SendProgressType::Typing);
				const auto secondPause = crl::time(1500 + base::RandomIndex(1200));
				base::call_delayed(secondPause, &Core::App(), sendNow);
			});
		});
	} else {
		const auto pause = TypingPause(parts[index]);
		if (pause > 4'500) {
			// The indicator lives about 5 seconds on the server's side.
			base::call_delayed(4'000, &Core::App(), [=] {
				if (Main::AccountIsLive(account) && !stopped()) {
					account->session().sendProgressManager().update(
						account->session().data().history(peer),
						Api::SendProgressType::Typing);
				}
			});
		}
		base::call_delayed(pause, &Core::App(), sendNow);
	}
}

// Starts a bot conversation cold (no deep-link token). Neither method
// alone has held up: a plain "/start" text message (ApiWrap::sendBotStart's
// own token.isEmpty() branch) is what the official client itself sends
// here, but that's what the original report ("no chat with @spambot ever
// appears") was about on some accounts; calling messages.startBot
// directly with an empty start_param, tried next, was rejected by the
// server outright (START_PARAM_EMPTY, confirmed live) - the error name
// itself says an empty parameter specifically is the problem, not the
// method. So: try the plain text first (matches the official app, no
// reason to prefer the heavier API call when it works), and only if that
// fails, retry once via messages.startBot with a non-empty start_param.
void SendBotStart(
		not_null<Main::Account*> account,
		not_null<PeerData*> peer,
		StepDone done) {
	const auto bot = peer->asUser();
	if (!bot || !bot->isBot()) {
		done(false, u"это не бот"_q);
		return;
	}
	const auto session = &account->session();
	const auto startViaApi = [=] {
		const auto randomId = base::RandomValue<uint64>();
		session->api().request(MTPmessages_StartBot(
			bot->inputUser(),
			MTP_inputPeerEmpty(),
			MTP_long(randomId),
			MTP_string(u"start"_q)
		)).done([=](const MTPUpdates &result) {
			session->api().applyUpdates(result);
			done(true, QString());
		}).fail([=](const MTP::Error &error) {
			done(false, error.type());
		}).send();
	};
	const auto send = [=] {
		SendTextToPeer(account, peer, u"/start"_q, [=](
				bool success,
				QString message) {
			if (success) {
				done(true, QString());
			} else {
				startViaApi();
			}
		});
	};
	// An account that once blocked the bot gets YOU_BLOCKED_USER on every
	// message to it - unblock first (what pressing "Restart bot" does).
	if (bot->isBlocked()) {
		session->api().blockedPeers().unblock(bot, [=](bool) { send(); });
	} else {
		send();
	}
}

// Sends an existing sticker/document through the same prepared-message path
// as SendTextToPeer, so - unlike Api::SendExistingDocument, which is
// fire-and-forget - the server's answer (success, PEER_FLOOD, FLOOD_WAIT...)
// actually reaches `done`.
void SendStickerToPeer(
		not_null<Main::Account*> account,
		not_null<PeerData*> peer,
		not_null<DocumentData*> document,
		StepDone done) {
	const auto session = &account->session();
	const auto history = session->data().history(peer);
	const auto randomId = base::RandomValue<uint64>();
	history->owner().histories().sendPreparedMessage(
		history,
		FullReplyTo(),
		randomId,
		Data::Histories::PrepareMessage<MTPmessages_SendMedia>(
			MTP_flags(MTPmessages_SendMedia::Flags(0)),
			peer->input(),
			Data::Histories::ReplyToPlaceholder(),
			MTP_inputMediaDocument(
				MTP_flags(MTPDinputMediaDocument::Flags(0)),
				document->mtpInput(),
				MTPInputPhoto(), // video_cover
				MTPint(), // ttl_seconds
				MTPint(), // video_timestamp
				MTPstring()), // query
			MTPstring(),
			MTP_long(randomId),
			MTPReplyMarkup(),
			MTPvector<MTPMessageEntity>(),
			MTP_int(0),
			MTP_int(0),
			MTP_inputPeerEmpty(),
			MTPInputQuickReplyShortcut(),
			MTP_long(0),
			MTP_long(0),
			MTPSuggestedPost()),
		[=](const MTPUpdates &, const MTP::Response &) {
			done(true, QString());
		},
		[=](const MTP::Error &sendError, const MTP::Response &) {
			done(false, sendError.type());
		});
}

// The sticker Telegram itself offers in an empty chat ("No messages here
// yet... click on the greeting below", see history_view_about_view.cpp):
// one of the "hello" set from Api::Premium::helloStickers(). Waits (with a
// timeout) for that set to load the first time; a plain 👋 lookup is the
// fallback when it can't be had. `done` is always called exactly once, even
// if the session goes away meanwhile.
void FetchGreetingSticker(
		not_null<Main::Account*> account,
		Fn<void(DocumentData*)> done) {
	if (!Main::AccountIsLive(account)) {
		done(nullptr);
		return;
	}
	const auto session = &account->session();
	const auto pick = [](
			const std::vector<not_null<DocumentData*>> &list) -> DocumentData* {
		return list.empty()
			? nullptr
			: list[base::RandomIndex(int(list.size()))].get();
	};
	if (const auto document = pick(session->api().premium().helloStickers())) {
		done(document);
		return;
	}
	const auto settled = std::make_shared<bool>(false);
	const auto lifetime = std::make_shared<rpl::lifetime>();
	const auto finish = [=](DocumentData *document) {
		if (std::exchange(*settled, true)) {
			return;
		}
		lifetime->destroy();
		done(document);
	};
	session->api().premium().helloStickersUpdated(
	) | rpl::on_next([=] {
		if (const auto document = pick(
				session->api().premium().helloStickers())) {
			finish(document);
		}
	}, *lifetime);
	base::call_delayed(kGreetingStickerTimeout, &Core::App(), [=] {
		if (!Main::AccountIsLive(account)) {
			finish(nullptr);
			return;
		}
		const auto list = account->session().api().stickersByEmoji(
			u"👋"_q);
		finish(list ? pick(*list) : nullptr);
	});
}

// "Сначала приветственный стикер": the sticker always goes out first; an
// empty `parts` means the sticker *is* the whole message, so `done` fires
// as soon as the server accepts it. A sticker that fails to send
// (PEER_FLOOD, FLOOD_WAIT, a dead session...) is reported through `done`
// like any other failed send.
void SendGreetingStickerThenText(
		not_null<Main::Account*> account,
		not_null<PeerData*> peer,
		QStringList parts,
		StepDone done,
		SmsSendControl control) {
	// Every stage lands in ggtg.log: "sticker went out, the text didn't" is
	// otherwise impossible to tell apart from "the text was never tried".
	const auto who = peer->isUser()
		? (u"@"_q + peer->asUser()->username())
		: peer->name();
	const auto note = [=](const QString &text) {
		Core::GgtgLog::Info(u"Отправка SMS"_q, who + u": "_q + text);
	};
	const auto say = [=](const QString &step) {
		if (control.stage) {
			control.stage(step);
		}
	};
	const auto stopped = [=] {
		return control.alive && !control.alive();
	};
	say(u"выбирает стикер"_q);
	FetchGreetingSticker(account, [=](DocumentData *document) {
		if (stopped()) {
			done(false, SmsAbortedError());
			return;
		}
		if (!document) {
			note(u"приветственный стикер не найден, уйдёт только текст"_q);
			say(u"стикер не нашёлся, только текст"_q);
			if (MessagePartsEmpty(parts)) {
				// Nothing at all would go out: not a success.
				done(false, u"приветственный стикер не нашёлся"_q);
			} else {
				SendTextPartsToPeer(account, peer, parts, 0, done, control);
			}
			return;
		}
		// The real "choosing a sticker" indicator for a moment, as a person
		// leafing through the panel would show.
		account->session().sendProgressManager().update(
			account->session().data().history(peer),
			Api::SendProgressType::ChooseSticker);
		base::call_delayed(NextSmsPause(1'000, 2'500), &Core::App(), [=] {
			if (!Main::AccountIsLive(account)) {
				say(u"стикер не ушёл: сессия закрылась"_q);
				done(false, u"сессия аккаунта закрылась перед стикером"_q);
				return;
			} else if (stopped()) {
				done(false, SmsAbortedError());
				return;
			}
			SendStickerToPeer(account, peer, document, [=](
					bool success,
					QString message) {
				if (!success) {
					say(u"стикер не ушёл: "_q + message);
					note(u"стикер не ушёл ("_q + message + ')');
					done(false, message);
					return;
				}
				say(u"отправил стикер"_q);
				if (control.delivered) {
					control.delivered();
				}
				if (MessagePartsEmpty(parts)) {
					note(u"стикер отправлен"_q);
					done(true, QString());
					return;
				}
				note(u"стикер отправлен, текст уйдёт через несколько секунд"_q);
				// Guarded by Core::App(), not by the session: a session
				// guard would let the pending text silently vanish (`done`
				// never called) if the session went away during the wait.
				say(u"пауза перед текстом"_q);
				base::call_delayed(NextGreetingTextPause(), &Core::App(), [=] {
					if (!Main::AccountIsLive(account)) {
						note(u"сессия закрылась до отправки текста"_q);
						done(false, u"сессия аккаунта закрылась во время паузы"_q);
						return;
					}
					SendTextPartsToPeer(account, peer, parts, 0, [=](
							bool textSent,
							QString textError) {
						note(textSent
							? u"текст отправлен"_q
							: (u"текст не ушёл ("_q + textError + ')'));
						done(textSent, textError);
					}, control);
				});
			});
		});
	});
}

// ---- SendSmsTextMessage: resolve + optional warmup + send, categorized ----

// Unlike the shared bulk_accounts_actions.h StepDone (a plain success
// bool), the campaign send loop needs to tell apart a real send attempt
// from one that never reached the server at all - see RunSmsCampaign's
// handling of Skip below, and tg-web-panel/main.py's
// _run_one_sms_account, whose equivalent distinction (a duplicate/
// not-found recipient `continue`s straight to the next one, no pause;
// only a real attempt - success, PEER_FLOOD, or some other send error -
// spends the long inter-message pause) this mirrors.
enum class SmsStepOutcome {
	Sent, // a real message went out - counts toward the per-account quota
	// Part of it went out (the sticker, the first parts of a [SPLIT] text)
	// and then the rest failed with `message`: the person did get something,
	// so they count as written to - never written to a second time.
	Partial,
	SpamBlock, // PEER_FLOOD on a real attempt - counts toward spamTolerance
	Skip, // no real attempt was made (bad username, empty config, ...)
	NotFound, // the username didn't resolve - Skip, but counted in a streak
	FloodWait, // FLOOD_WAIT_x: nothing was sent, the account must back off
	AccountDead, // session revoked / account banned: nothing was sent
	Error, // a real attempt was made and failed for some other reason
	// The chat already holds a message of ours (sent by hand, from another
	// device...): nothing was sent, the person counts as written to.
	AlreadyWritten,
	// The step was called off ("Стоп", or it ran out of time) before
	// anything was sent: the recipient is left exactly as it was.
	Aborted,
};
using SmsStepDone = Fn<void(SmsStepOutcome outcome, QString message)>;

// Set when the step must not go any further: "Стоп" was pressed, or the
// step ran out of time and the lane has already moved on. Every stage of a
// send looks at it before its next request, so nothing goes out after that
// (a request already on its way can't be called back - its answer is still
// recorded, see GuardedSmsDone's `late`).
using SmsStepAbort = std::shared_ptr<bool>;

enum class SmsStepPhase {
	Waiting,
	Answered,
	TimedOut,
};

// Mirrors bulk_accounts_actions.h's GuardedDone/GuardedStep idiom for
// SmsStepDone's richer outcome instead of a plain bool.
//
// A step that times out is reported as Error right away so the lane isn't
// wedged, and `abort` is raised so the step doesn't start anything new. A
// request already in flight may still deliver the message: if the real
// answer (Sent, Partial or SpamBlock) arrives after the timeout it goes to
// `late` instead of being dropped - otherwise that recipient would never be
// recorded as written to and the next campaign would message them again.
[[nodiscard]] SmsStepDone GuardedSmsDone(
		SmsStepDone done,
		SmsStepDone late,
		SmsStepAbort abort,
		crl::time timeout = kSmsStepTimeout) {
	const auto phase = std::make_shared<SmsStepPhase>(SmsStepPhase::Waiting);
	const auto guard = [=](SmsStepOutcome outcome, QString message) {
		if (*phase == SmsStepPhase::Waiting) {
			*phase = SmsStepPhase::Answered;
			done(outcome, message);
		} else if (*phase == SmsStepPhase::TimedOut
				&& late
				&& (outcome == SmsStepOutcome::Sent
					|| outcome == SmsStepOutcome::Partial
					|| outcome == SmsStepOutcome::SpamBlock)) {
			*phase = SmsStepPhase::Answered;
			late(outcome, message);
		}
	};
	base::call_delayed(timeout, &Core::App(), [=] {
		if (*phase == SmsStepPhase::Waiting) {
			*phase = SmsStepPhase::TimedOut;
			*abort = true;
			done(
				SmsStepOutcome::Error,
				u"таймаут ожидания ответа сервера (сообщение могло уйти)"_q);
		}
	});
	return guard;
}

// `textTemplate` is rendered via RenderSmsTemplate once the recipient
// peer resolves (only then is their real name known) - the sending
// account's own name comes from `account` itself. `warmup` ("Лёгкий
// прогрев", now always on) and `greetingSticker` (the toggle at the top of
// "3. Текст сообщения") are independent of each other.
[[nodiscard]] int SeenBucket(not_null<UserData*> user);

// ---- finding the recipient through the group they were collected from ----
//
// A recipient taken from an open group is first looked for in that group's
// member list (the group is opened, the person is searched among its
// members), and only if they are not there is the username searched as
// usual. A group that turns out to be closed (or gone) is not tried again for
// the rest of the campaign, and a recipient whose group is not known (a
// hand-made list, members of several groups) is simply searched.

// The groups known to be unusable for finding people, by lower-case
// username, for this campaign.
[[nodiscard]] std::set<QString> &ClosedGroups() {
	static auto result = std::set<QString>();
	return result;
}

[[nodiscard]] std::map<uint64, QString> &OpenGroupOfBaseCache() {
	static auto result = std::map<uint64, QString>();
	return result;
}

inline void ResetGroupRoutes() {
	ClosedGroups().clear();
	OpenGroupOfBaseCache().clear();
}

// The public @username of the group a users base was collected from (the
// base is titled "Name (@group)"), or empty: a hand-made base, a base of
// several groups, a group with no public username.
[[nodiscard]] QString OpenGroupOfBase(uint64 baseId) {
	if (!baseId || (baseId >> 63)) {
		return QString(); // hand-made: no group behind it
	}
	auto &cache = OpenGroupOfBaseCache();
	if (cache.empty()) {
		static const auto bracket = QRegularExpression(
			u"\\(@?([A-Za-z0-9_]{4,32})\\)\\s*$"_q);
		for (const auto &meta : Data::LoadScrapedGroupsIndex()) {
			const auto match = bracket.match(meta.title.trimmed());
			cache[meta.id] = match.hasMatch()
				? match.captured(1)
				: QString();
		}
		cache[0]; // never empty again
	}
	const auto i = cache.find(baseId);
	return (i == cache.end()) ? QString() : i->second;
}

// The name of the group a users base was collected from, for [GROUP]: the
// base's title without the "(@username)" of the group. Empty for a list made
// by hand (there is no group behind it) - the template's own fallback word
// then stands ([GROUP|друзья]).
[[nodiscard]] QString GroupNameOfBase(uint64 baseId) {
	if (!baseId) {
		return QString();
	}
	static const auto handle = QRegularExpression(
		u"\\s*\\(@[A-Za-z0-9_]{4,32}\\)"_q);
	for (const auto &meta : Data::LoadScrapedGroupsIndex()) {
		if (meta.id != baseId) {
			continue;
		}
		const auto bare = (baseId >> 63) == 0;
		if (!bare && !handle.match(meta.title).hasMatch()) {
			return QString(); // hand-made, no group behind it
		}
		auto name = meta.title;
		name.remove(handle);
		return name.trimmed();
	}
	return QString();
}

// The gap between two lookups of one recipient: a person retyping the name
// somewhere else, never a burst of requests.
[[nodiscard]] crl::time NextLookupPause() {
	return NextSmsPause(1'500, 3'000);
}

// Fallback chain used when the global search found nothing: the scraped
// group's member list, then contacts.resolveUsername - one request at a
// time, with a pause before each. Gives up (with SmsAbortedError()) as soon
// as `alive` says the step was called off.
void ResolveRecipientFallback(
		not_null<Main::Account*> account,
		const QString &clean,
		const QString &group,
		Fn<void(QString)> say,
		Fn<bool()> alive,
		Fn<void(PeerData *peer, QString error)> done) {
	// The next request after a pause, unless the step was called off or the
	// session went away meanwhile.
	const auto then = [=](Fn<void()> request) {
		base::call_delayed(NextLookupPause(), &Core::App(), [=] {
			if (!alive()) {
				done(nullptr, SmsAbortedError());
			} else if (!Main::AccountIsLive(account)) {
				done(nullptr, u"AUTH_KEY_UNREGISTERED"_q);
			} else {
				request();
			}
		});
	};
	const auto session = &account->session();
	const auto direct = [=] {
		then([=] {
			say(u"ищет @"_q + clean);
			ResolveUsernameForAccount(account, clean, done);
		});
	};
	const auto key = group.toLower();
	if (group.isEmpty()
		|| ClosedGroups().contains(key)
		|| session->data().peerByUsername(clean)) {
		direct();
		return;
	}
	const auto close = [=](const QString &why) {
		ClosedGroups().insert(key);
		Core::GgtgLog::Info(
			u"Отправка SMS"_q,
			u"группа @"_q + group + u" не используется для поиска: "_q + why);
	};
	then([=] {
	say(u"заходит в группу @"_q + group);
	ResolveUsernameForAccount(account, group, [=](
			PeerData *peer,
			QString error) {
		if (FloodWaitSeconds(error) > 0
			|| ClassifySmsError(error) == SmsErrorKind::AccountDead) {
			// The account itself has to stop: no point trying more.
			done(nullptr, error);
			return;
		}
		const auto channel = peer ? peer->asChannel() : nullptr;
		if (!channel) {
			if (peer
				|| error == u"USERNAME_NOT_OCCUPIED"_q
				|| error == u"USERNAME_INVALID"_q) {
				close(u"это не группа или её нет"_q);
			}
			say(u"группа недоступна, ищет напрямую"_q);
			direct();
			return;
		}
		if (!channel->isMegagroup()) {
			close(u"канал: список участников не виден"_q);
			direct();
			return;
		}
		then([=] {
		say(u"ищет @"_q + clean + u" в группе"_q);
		const auto api = static_cast<MTP::Sender*>(&session->api());
		api->request(MTPchannels_GetParticipants(
			channel->inputChannel(),
			MTP_channelParticipantsSearch(MTP_string(clean)),
			MTP_int(0),
			MTP_int(20),
			MTP_long(0)
		)).handleFloodErrors().done([=](
				const MTPchannels_ChannelParticipants &result) {
			auto found = (UserData*)nullptr;
			result.match([&](const MTPDchannels_channelParticipants &data) {
				const auto parsed = Api::ChatParticipants::Parse(
					channel,
					data);
				for (const auto &participant : parsed.list) {
					const auto userId = participant.userId();
					const auto user = userId
						? session->data().userLoaded(userId)
						: nullptr;
					if (user
						&& user->username().compare(
							clean,
							Qt::CaseInsensitive) == 0) {
						found = user;
						break;
					}
				}
			}, [&](const MTPDchannels_channelParticipantsNotModified &) {
			});
			if (found) {
				say(u"нашёл @"_q + clean + u" в группе"_q);
				done(found, QString());
				return;
			}
			say(u"в группе не нашёл, ищет напрямую"_q);
			direct();
		}).fail([=](const MTP::Error &error) {
			const auto type = error.type();
			if (MTP::IsFloodError(error)
				|| ClassifySmsError(type) == SmsErrorKind::AccountDead) {
				done(nullptr, type);
				return;
			}
			if (type == u"CHANNEL_PRIVATE"_q
				|| type == u"CHAT_ADMIN_REQUIRED"_q
				|| type == u"CHANNEL_INVALID"_q
				|| type == u"CHAT_FORBIDDEN"_q
				|| type == u"CHANNEL_BANNED"_q
				|| type == u"USER_BANNED_IN_CHANNEL"_q) {
				close(type);
				say(u"группа закрыта, ищет напрямую"_q);
			}
			direct();
		}).send();
		});
	});
	});
}

// The recipient is looked up the way the official client does it: through
// the global search (contacts.search), a single request. Only if the exact
// username isn't among the results does the older chain (group members,
// then contacts.resolveUsername) run, one request at a time with a pause
// before each. `done` gets SmsAbortedError() when the step was called off.
void ResolveRecipient(
		not_null<Main::Account*> account,
		const QString &clean,
		const QString &group,
		Fn<void(QString)> say,
		Fn<bool()> alive,
		Fn<void(PeerData *peer, QString error)> done) {
	const auto session = &account->session();
	if (const auto known = session->data().peerByUsername(clean)) {
		done(known, QString());
		return;
	}
	say(u"ищет @"_q + clean + u" в поиске"_q);
	const auto api = static_cast<MTP::Sender*>(&session->api());
	api->request(MTPcontacts_Search(
		MTP_flags(0),
		MTP_string(clean),
		MTP_int(20)
	)).handleFloodErrors().done([=](const MTPcontacts_Found &result) {
		const auto &data = result.data();
		session->data().processUsers(data.vusers());
		session->data().processChats(data.vchats());
		const auto lists = {
			&data.vmy_results().v,
			&data.vresults().v,
		};
		for (const auto list : lists) {
			for (const auto &mtpPeer : *list) {
				const auto peer = session->data().peer(
					peerFromMTP(mtpPeer));
				if (peer->username().compare(
						clean,
						Qt::CaseInsensitive) == 0) {
					say(u"нашёл @"_q + clean + u" в поиске"_q);
					done(peer, QString());
					return;
				}
			}
		}
		say(u"в поиске нет, пробует иначе"_q);
		ResolveRecipientFallback(account, clean, group, say, alive, done);
	}).fail([=](const MTP::Error &error) {
		if (ClassifySmsError(error.type()) == SmsErrorKind::AccountDead) {
			done(nullptr, error.type());
			return;
		}
		// The search has limits of its own (a FLOOD_WAIT here says nothing
		// about the username lookup): try the other ways.
		ResolveRecipientFallback(account, clean, group, say, alive, done);
	}).send();
}

using SmsAttemptInfo = std::shared_ptr<Core::GgtgSmsStats::Attempt>;

// One recipient from start to finish, every stage one request after a
// pause, the way a person would do it:
//
//   1. find them: contacts.search, then the group they came from, then
//      contacts.resolveUsername (ResolveRecipient);
//   2. check them on the spot: a person (not a channel or a bot), not
//      deleted, not written to before (the registry, then the chat history
//      itself once the chat is opened), not taking Stars for messages, not
//      closed to non-Premium senders, not last seen long ago;
//   3. warm up: maybe the profile, maybe the photo, always the chat;
//   4. maybe translate the text into their language;
//   5. send: maybe the greeting sticker, then the text part by part.
//
// Ends in exactly one `done` with what came of it. Before every request it
// checks `abort` (raised by "Стоп" or by the step timing out) and the
// session: nothing new is started after that. Anything that already
// reached the person is never lost: a send cut short after the sticker or
// the first part ends as Partial, i.e. "written to".
void SendSmsTextMessage(
		not_null<Main::Account*> account,
		QString username,
		QString textTemplate,
		bool warmup,
		bool greetingSticker,
		Fn<void(QString)> stage,
		SmsStepDone done,
		SmsStepAbort abort,
		SmsStepDone late = nullptr,
		SmsAttemptInfo info = nullptr,
		QString groupUsername = QString(),
		QString groupName = QString(),
		int protectionMode = 1,
		bool simulateTypos = true,
		bool skipSuspectedFakes = true) {
	// What the account is doing right now, for the "Поток N" line.
	const auto say = [=](const QString &step) {
		if (stage) {
			stage(step);
		}
	};
	if (!Main::AccountIsLive(account)) {
		done(SmsStepOutcome::AccountDead, u"нет активной сессии"_q);
		return;
	}
	const auto clean = username.trimmed().startsWith('@')
		? username.trimmed().mid(1)
		: username.trimmed();
	if (clean.isEmpty()) {
		done(SmsStepOutcome::Skip, u"пустой username"_q);
		return;
	}
	// Telegram requires every bot's username to end in "bot" (enforced by
	// BotFather at creation) - a real person's username never does, so
	// this suffix reliably tells bots and users apart without a request.
	if (Data::IsBotUsername(clean)) {
		done(SmsStepOutcome::Skip, u"это бот, а не пользователь"_q);
		return;
	}
	// An empty template is only fine when the sticker itself will carry
	// the message - see SendGreetingStickerThenText.
	if (!greetingSticker && textTemplate.trimmed().isEmpty()) {
		done(SmsStepOutcome::Skip, u"нет текста сообщения"_q);
		return;
	}
	// How long the step may take before it counts as lost: the usual time,
	// plus the typing and the gaps of every [SPLIT] part and the sticker
	// with its pause, and three times as much with the AI translation (a
	// model to load, maybe two) - or a long but healthy send was taken for
	// a lost one.
	const auto aiNow = Core::GgtgAi::CachedSettings();
	const auto extraParts = int(textTemplate.count(u"[SPLIT]"_q));
	const auto budget = kSmsStepTimeout
		+ crl::time(extraParts) * crl::time(15'000)
		+ (greetingSticker ? crl::time(30'000) : crl::time(0));
	done = GuardedSmsDone(
		done,
		late,
		abort,
		aiNow.smsTranslate ? (2 * budget) : budget);
	const auto alive = [=] {
		return !*abort;
	};
	// Messages that actually reached the person so far (sticker, parts).
	const auto delivered = std::make_shared<int>(0);
	// The step stops here: nothing more is sent. Whatever already went out
	// still counts.
	const auto stopHere = [=] {
		if (*delivered > 0) {
			done(SmsStepOutcome::Partial, u"остановлено после начала отправки"_q);
		} else {
			done(SmsStepOutcome::Aborted, QString());
		}
	};
	// Runs `next` after `delay` unless the step was called off or the
	// session went away meanwhile.
	const auto after = [=](crl::time delay, Fn<void()> next) {
		base::call_delayed(delay, &Core::App(), [=] {
			if (!alive()) {
				stopHere();
			} else if (!Main::AccountIsLive(account)) {
				done(
					(*delivered > 0)
						? SmsStepOutcome::Partial
						: SmsStepOutcome::AccountDead,
					u"сессия аккаунта закрылась"_q);
			} else {
				next();
			}
		});
	};
	ResolveRecipient(account, clean, groupUsername, say, alive, [=](
			PeerData *peer,
			QString error) {
		if (error == SmsAbortedError() || !alive()) {
			stopHere();
			return;
		}
		if (!peer) {
			say(u"не найден"_q);
			switch (ClassifySmsError(error)) {
			case SmsErrorKind::FloodWait:
				done(SmsStepOutcome::FloodWait, error);
				return;
			case SmsErrorKind::AccountDead:
				done(SmsStepOutcome::AccountDead, error);
				return;
			default: break;
			}
			const auto notFound = error.isEmpty()
				|| (error == u"USERNAME_NOT_OCCUPIED"_q)
				|| (error == u"USERNAME_INVALID"_q);
			done(
				notFound ? SmsStepOutcome::NotFound : SmsStepOutcome::Error,
				error.isEmpty() ? u"пользователь не найден"_q : error);
			return;
		}
		if (!Main::AccountIsLive(account)) {
			done(SmsStepOutcome::AccountDead, u"сессия аккаунта закрылась"_q);
			return;
		}
		say(u"нашёл @"_q + clean);
		// Checked right here, per recipient, instead of a separate pass
		// over the whole base.
		const auto user = peer->asUser();
		if (!user) {
			say(u"это не человек, пропуск"_q);
			done(SmsStepOutcome::Skip, u"это не пользователь"_q);
			return;
		} else if (user->isInaccessible()) {
			// Marked like Telegram's own answer for it, so it isn't tried
			// again (and doesn't count as a failed lookup).
			say(u"аккаунт удалён, пропуск"_q);
			done(SmsStepOutcome::Error, u"INPUT_USER_DEACTIVATED"_q);
			return;
		} else if (user->isBot()) {
			say(u"бот, пропуск"_q);
			done(SmsStepOutcome::Skip, u"это бот, а не пользователь"_q);
			return;
		} else if (user->isSelf()) {
			say(u"это сам аккаунт, пропуск"_q);
			done(SmsStepOutcome::Skip, u"это сам отправляющий аккаунт"_q);
			return;
		}
		if (Data::FindSmsSendRecord(clean)) {
			say(u"ему уже писали, пропуск"_q);
			done(SmsStepOutcome::Skip, u"ему уже писали"_q);
			return;
		}
		if (user->starsPerMessage() > 0) {
			say(u"берёт звёзды за сообщения, пропуск"_q);
			done(SmsStepOutcome::Error, u"ALLOW_PAYMENT_REQUIRED"_q);
			return;
		}
		if (user->requiresPremiumToWrite()
				&& !account->session().premium()) {
			say(u"ЛС закрыт (только Premium), пропуск"_q);
			done(SmsStepOutcome::Error, u"PRIVACY_PREMIUM_REQUIRED"_q);
			return;
		}
		// Only Telegram's own "last seen a long time ago" is skipped; a
		// week, a month or a hidden last seen (shown as one of those) is
		// written to as usual.
		if (SeenBucket(user) == 4) {
			say(u"был(а) давно, пропуск"_q);
			done(SmsStepOutcome::Skip, u"был(а) в сети давно"_q);
			return;
		}
		if (skipSuspectedFakes) {
			const auto fakeAnalysis = Ggtg::FakeDetector::AnalyzeUserData(user);
			if (fakeAnalysis.fakeProbability >= 65) {
				say(u"предположительный фейк ("_q + QString::number(fakeAnalysis.fakeProbability) + u"%), пропуск"_q);
				done(
					SmsStepOutcome::Skip,
					u"предположительный фейк ("_q
						+ QString::number(fakeAnalysis.fakeProbability)
						+ u"%): "_q
						+ fakeAnalysis.reasons.join(u", "_q));
				return;
			}
		}
		// Who the person is, for "Разбор рассылок".
		if (info) {
			info->recipientPremium = user->isPremium();
			info->photo = user->hasUserpic();
			info->about = !peer->about().trimmed().isEmpty();
			info->seen = SeenBucket(user);
			const auto code = Core::GgtgAi::DetectLanguage(
				user->firstName + ' ' + user->lastName);
			info->language = (code == u"en"_q) ? u"lat"_q : code;
		}
		const auto deliver = [=](const QString &templateToSend) {
			if (!alive()) {
				stopHere();
				return;
			} else if (!Main::AccountIsLive(account)) {
				done(SmsStepOutcome::AccountDead, u"сессия аккаунта закрылась"_q);
				return;
			}
			auto text = RenderSmsTemplate(
				templateToSend,
				user,
				account->session().user(),
				groupName);
			// A translation that came out empty would leave only the sticker
			// to go out (counted as a success): the original text goes then.
			if (text.trimmed().isEmpty() && templateToSend != textTemplate) {
				Core::GgtgLog::Info(
					u"Отправка SMS"_q,
					u"@"_q + clean + u": перевод пустой, уйдёт исходный текст"_q);
				if (info) {
					info->translated.clear();
				}
				text = RenderSmsTemplate(
					textTemplate,
					user,
					account->session().user(),
					groupName);
			}
			// "[SPLIT]": one recipient may get several messages in a row.
			const auto parts = SplitRenderedMessage(text);
			if (info) {
				info->parts = MessagePartsEmpty(parts) ? 0 : int(parts.size());
			}
			// There is a text to send, but nothing came out of it: not a
			// sticker-only "success".
			if (MessagePartsEmpty(parts)
				&& !textTemplate.trimmed().isEmpty()) {
				done(SmsStepOutcome::Error, u"MESSAGE_EMPTY"_q);
				return;
			}
			const auto sent = [=](bool success, QString message) {
				if (success) {
					done(SmsStepOutcome::Sent, message);
				} else if (message == SmsAbortedError()) {
					stopHere();
				} else if (*delivered > 0) {
					// The person already got the sticker or the first
					// parts: written to, whatever happened to the rest.
					done(SmsStepOutcome::Partial, message);
				} else if (message == u"PEER_FLOOD"_q) {
					done(SmsStepOutcome::SpamBlock, message);
				} else {
					switch (ClassifySmsError(message)) {
					case SmsErrorKind::FloodWait:
						done(SmsStepOutcome::FloodWait, message);
						break;
					case SmsErrorKind::AccountDead:
						done(SmsStepOutcome::AccountDead, message);
						break;
					default:
						done(SmsStepOutcome::Error, message);
						break;
					}
				}
			};
			const auto control = SmsSendControl{
				.stage = stage,
				.alive = [=] { return alive(); },
				.delivered = [=] { ++*delivered; },
				.protectionMode = protectionMode,
				.simulateTypos = simulateTypos,
			};
			if (greetingSticker) {
				SendGreetingStickerThenText(account, peer, parts, sent, control);
			} else {
				SendTextPartsToPeer(account, peer, parts, 0, sent, control);
			}
		};
		// "Переводить на язык получателя": only when the letters themselves
		// say the person writes another language - the script of the name
		// (Korean, Chinese, Russian...), or failing that of the profile text.
		// A text in Latin letters to a Latin name is never translated.
		// Translated once per language for the whole run; when it fails or
		// takes too long, the template goes out as it is.
		const auto proceed = [=] {
			const auto ai = Core::GgtgAi::CachedSettings();
			if (!ai.smsTranslate) {
				deliver(textTemplate);
				return;
			}
			const auto own = Core::GgtgAi::DetectLanguage(textTemplate);
			auto code = QString();
			const auto preferredLang = ai.smsTranslateLang.trimmed();
			if (!preferredLang.isEmpty() && preferredLang != u"auto"_q) {
				code = preferredLang;
			} else {
				// Auto-detection based on recipient profile
				const auto foreign = [&](const QString &c) {
					return !c.isEmpty() && c != own;
				};
				auto byName = Core::GgtgAi::DetectLanguage(
					user->firstName + ' ' + user->lastName);
				if (foreign(byName) && byName != u"en"_q) {
					code = byName;
				} else {
					static const auto noiseRe = QRegularExpression(
						u"(https?://\\S+|www\\.\\S+|\\S+\\.(com|net|org|me|io|ru|kr|cn|jp)\\S*|[@#]\\w+)"_q,
						QRegularExpression::CaseInsensitiveOption);
					auto aboutText = peer->about();
					aboutText.replace(noiseRe, QString());
					const auto byAbout = Core::GgtgAi::DetectLanguage(aboutText);
					if (foreign(byAbout) && byAbout != u"en"_q) {
						code = byAbout;
					}
				}
				// If name or bio still didn't give a specific non-en language, check group/base title
				if (code.isEmpty() && !groupName.isEmpty()) {
					const auto gLower = groupName.toLower();
					if (gLower.contains(u"коре"_q) || gLower.contains(u"korea"_q) || gLower.contains(u"kr"_q)) {
						code = u"ko"_q;
					} else if (gLower.contains(u"кита"_q) || gLower.contains(u"china"_q) || gLower.contains(u"cn"_q)) {
						code = u"zh-CN"_q;
					} else if (gLower.contains(u"япон"_q) || gLower.contains(u"japan"_q) || gLower.contains(u"jp"_q)) {
						code = u"ja"_q;
					}
				}
				// If Latin-script text has distinct Spanish/German/French/Turkish words
				if (code.isEmpty()) {
					const auto latinLang = Core::GgtgAi::DetectLatinLanguage(peer->about());
					if (foreign(latinLang) && latinLang != u"en"_q) {
						code = latinLang;
					}
				}
			}

			if (code.isEmpty() || code == own) {
				deliver(textTemplate);
				return;
			}
			const auto langObj = Core::GgtgAi::LanguageByCode(code);
			const auto language = langObj.russian.isEmpty() ? code : langObj.russian;
			say(u"переводит на "_q + language);
			const auto settled = std::make_shared<bool>(false);
			const auto finish = [=](const QString &translated, QString why) {
				if (std::exchange(*settled, true)) {
					return;
				}
				if (!why.isEmpty() || translated.trimmed().isEmpty()) {
					say(u"перевод не вышел, текст как есть"_q);
					Core::GgtgLog::Info(
						u"Отправка SMS"_q,
						u"@"_q + clean + u": перевод не вышел ("_q + why
							+ u"), уйдёт исходный текст"_q);
					deliver(textTemplate);
					return;
				}
				say(u"перевёл на "_q + language);
				if (info) {
					info->translated = code;
				}
				deliver(translated);
			};
			base::call_delayed(kSmsTranslateTimeout, &Core::App(), [=] {
				finish(QString(), u"не успел за 2 минуты"_q);
			});
			Settings::TranslateTemplateFree(
				textTemplate,
				code,
				[=](QString translated, QString error) {
					finish(
						translated,
						!error.isEmpty()
							? error
							: translated.trimmed().isEmpty()
							? u"пустой ответ"_q
							: QString());
				});
		};
		// The chat is always opened before anything is written, and what it
		// holds is the last word on "written to before": a message of ours
		// already there (sent by hand, from another device, before this
		// app) means the person is not written to again.
		const auto openChat = [=] {
			say(u"открывает переписку"_q);
			const auto session = &account->session();
			session->api().request(MTPmessages_GetHistory(
				peer->input(),
				MTP_int(0), // offset_id
				MTP_int(0), // offset_date
				MTP_int(0), // add_offset
				MTP_int(20), // limit
				MTP_int(0), // max_id
				MTP_int(0), // min_id
				MTP_long(0) // hash
			)).done([=](const MTPmessages_Messages &result) {
				const auto ours = result.match([](
						const MTPDmessages_messagesNotModified &) {
					return false;
				}, [](const auto &data) {
					for (const auto &message : data.vmessages().v) {
						const auto out = message.match([](
								const MTPDmessage &data) {
							return data.is_out();
						}, [](const MTPDmessageService &data) {
							return data.is_out();
						}, [](const MTPDmessageEmpty &) {
							return false;
						});
						if (out) {
							return true;
						}
					}
					return false;
				});
				if (ours) {
					say(u"в чате уже есть ваше сообщение, пропуск"_q);
					done(
						SmsStepOutcome::AlreadyWritten,
						u"в чате уже есть ваше сообщение"_q);
					return;
				}
				after(NextSmsPause(1'000, 2'500), [=] {
					say(u"открыл переписку"_q);
					proceed();
				});
			}).fail([=](const MTP::Error &error) {
				const auto type = error.type();
				switch (ClassifySmsError(type)) {
				case SmsErrorKind::FloodWait:
					done(SmsStepOutcome::FloodWait, type);
					return;
				case SmsErrorKind::AccountDead:
					done(SmsStepOutcome::AccountDead, type);
					return;
				case SmsErrorKind::RecipientOnly:
					done(SmsStepOutcome::Error, type);
					return;
				default: break;
				}
				// The history is only a check: without it the send still
				// goes, the registry has already been asked.
				after(NextSmsPause(1'000, 2'500), proceed);
			}).handleFloodErrors().send();
		};
		if (!warmup) {
			openChat();
			return;
		}
		// "Лёгкий прогрев": what a person does before writing to a stranger,
		// each step a real request with a pause as if looking at it. Not
		// every account does all of it (see WarmupPlan): some open the
		// profile (and the photo, if there is one), some go straight to the
		// chat.
		const auto plan = WarmupPlanFor(account, protectionMode);
		const auto pause = [=](crl::time min, crl::time max) {
			return NextSmsPause(
				crl::time(double(min) * plan.pace),
				crl::time(double(max) * plan.pace));
		};
		if (!plan.profile) {
			openChat();
			return;
		}
		say(u"смотрит профиль"_q);
		account->session().api().requestFullPeer(peer);
		after(pause(3'000, 6'000), [=] {
			say(u"посмотрел профиль"_q);
			if (skipSuspectedFakes) {
				const auto fullFake = Ggtg::FakeDetector::AnalyzeUserData(user);
				if (fullFake.fakeProbability >= 65) {
					say(u"предположительный фейк ("_q + QString::number(fullFake.fakeProbability) + u"%), пропуск"_q);
					done(
						SmsStepOutcome::Skip,
						u"предположительный фейк ("_q
							+ QString::number(fullFake.fakeProbability)
							+ u"%): "_q
							+ fullFake.reasons.join(u", "_q));
					return;
				}
			}
			// The photo only after the profile is open, and only if there
			// is one.
			if (!plan.photo || !user->hasUserpic()) {
				openChat();
				return;
			}
			say(u"смотрит фото"_q);
			peer->loadUserpic();
			account->session().api().request(MTPphotos_GetUserPhotos(
				user->inputUser(),
				MTP_int(0), // offset
				MTP_long(0), // max_id
				MTP_int(1) // limit
			)).send();
			after(pause(1'500, 3'500), [=] {
				say(u"посмотрел фото"_q);
				openChat();
			});
		});
	});
}

// @spambot's own reply is not a documented API contract - its exact
// wording depends on the account's Telegram language and can change over
// time - so this is a best-effort parse: a recognized "no limits" phrase
// clears the status, a date/time after a limit sets a Temporary status with
// that expiry, a recognized "limited" phrase without a date is Permanent.
// A reply it doesn't understand at all changes nothing (std::nullopt): a
// free account wrongly taken for a banned one would be lost for every
// campaign, while a wrongly "unknown" limited one is still caught by
// Telegram's own refusals ("Снять аккаунт после отказов").
[[nodiscard]] std::optional<Data::AccountSpamStatus> ParseSpamBotReply(
		const QString &raw) {
	const auto lower = raw.toLower();
	const auto clearPhrases = {
		u"no limits"_q,
		u"free as a bird"_q,
		u"you're free"_q,
		u"you are free"_q,
		u"not limited"_q,
		u"нет ограничени"_q,
		u"ограничения не применяются"_q,
		u"свободны как птица"_q,
		u"свободен от"_q,
		u"не ограничен"_q,
	};
	for (const auto &phrase : clearPhrases) {
		if (lower.contains(phrase)) {
			return Data::AccountSpamStatus{
				.state = Data::AccountSpamState::Clear,
			};
		}
	}
	static const auto isoRe = QRegularExpression(
		u"(\\d{4}-\\d{2}-\\d{2})[ T](\\d{1,2}:\\d{2})"_q);
	static const auto humanRe = QRegularExpression(
		u"(\\d{1,2})\\s+([A-Za-z]{3,9})\\s+(\\d{4})[^\\d]{0,10}(\\d{1,2}:\\d{2})"_q);
	if (const auto match = isoRe.match(raw); match.hasMatch()) {
		const auto dt = QDateTime::fromString(
			match.captured(1) + ' ' + match.captured(2),
			u"yyyy-MM-dd h:mm"_q);
		if (dt.isValid()) {
			return Data::AccountSpamStatus{
				.state = Data::AccountSpamState::Temporary,
				.until = dt.toUTC().toSecsSinceEpoch(),
			};
		}
	}
	if (const auto match = humanRe.match(raw); match.hasMatch()) {
		const auto composed = match.captured(1) + ' '
			+ match.captured(2) + ' '
			+ match.captured(3) + ' '
			+ match.captured(4);
		auto dt = QDateTime::fromString(composed, u"d MMM yyyy h:mm"_q);
		if (!dt.isValid()) {
			dt = QDateTime::fromString(composed, u"d MMMM yyyy h:mm"_q);
		}
		if (dt.isValid()) {
			return Data::AccountSpamStatus{
				.state = Data::AccountSpamState::Temporary,
				.until = dt.toUTC().toSecsSinceEpoch(),
			};
		}
	}
	const auto limitedPhrases = {
		u"limited"_q,
		u"restricted"_q,
		u"annoying"_q,
		u"anti-spam"_q,
		u"ограничен"_q,
		u"антиспам"_q,
		u"жалоб"_q,
		u"нежелательн"_q,
	};
	for (const auto &phrase : limitedPhrases) {
		if (lower.contains(phrase)) {
			return Data::AccountSpamStatus{
				.state = Data::AccountSpamState::Permanent,
			};
		}
	}
	return std::nullopt;
}

// ---- "АККАУНТЫ": expandable folder accordion, multi-select ----

// An account @spambot has reported as limited: permanently, or temporarily
// with the limit not yet expired. Sending from it would only spend
// recipients on PEER_FLOOD, so "Выбрать всех" skips it and "Отправить"
// offers to leave it out.
[[nodiscard]] bool IsAccountSpamBlocked(not_null<Main::Account*> account) {
	if (!Main::AccountIsLive(account)) {
		return false;
	}
	const auto status = Data::FindAccountSpamStatus(
		account->session().user()->id.value);
	if (!status) {
		return false;
	}
	switch (status->state) {
	case Data::AccountSpamState::Permanent: return true;
	case Data::AccountSpamState::Temporary:
		return status->until > QDateTime::currentSecsSinceEpoch();
	case Data::AccountSpamState::Clear: return false;
	}
	return false;
}

// Higher first: a permanent block outranks a still-active temporary one,
// which outranks everything else (clear, or never checked) - used to sort
// each folder's account list so the accounts most worth looking at surface
// at the top instead of being buried among however many others are fine.
[[nodiscard]] int AccountSpamSeverityRank(not_null<Main::Account*> account) {
	if (!IsAccountSpamBlocked(account)) {
		return 0;
	}
	const auto status = Data::FindAccountSpamStatus(
		account->session().user()->id.value);
	return (status && status->state == Data::AccountSpamState::Permanent)
		? 2
		: 1;
}

// The cards' summaries and the line next to "Запустить" follow the
// selection at once instead of on the next timer tick.
void RefreshAccountsSelectionUi(not_null<CampaignState*> state) {
	if (state->refreshPreflight) {
		state->refreshPreflight();
	}
}

// Runs one @spambot check and always calls `next` exactly once when it has
// settled - whether it actually got a reply, failed outright (no @spambot,
// couldn't send "/start"), or simply timed out. CheckAccountSpamStatus's own
// `feedback` only covers the outright-failure cases (a bare timeout never
// calls it, since nothing ever went wrong from its point of view - @spambot
// might still reply late), which isn't enough to know it's safe to release
// spamCheckBusy or move on to the next account in a "check all" queue.
//
// Marks `account` as the one currently being probed for the duration (see
// spamCheckingAccount on CampaignState) so any visible row for it - whether
// or not that row is the one that started this check - can show its own
// "Проверка..." state.
void RunAccountSpamCheck(
		not_null<CampaignState*> state,
		not_null<Main::Account*> account,
		Fn<void()> next) {
	state->spamCheckingAccount = account.get();
	state->spamCheckActivityChanged.fire({});
	const auto finish = [=] {
		if (state->spamCheckingAccount == account.get()) {
			state->spamCheckingAccount = nullptr;
		}
		state->spamCheckActivityChanged.fire({});
		if (next) {
			next();
		}
	};
	if (!Main::AccountIsLive(account)) {
		finish();
		return;
	}
	const auto userId = account->session().user()->id.value;
	const auto settled = std::make_shared<bool>(false);
	const auto lifetime = std::make_shared<rpl::lifetime>();
	const auto onceSettled = [=] {
		if (std::exchange(*settled, true)) {
			return;
		}
		lifetime->destroy();
		finish();
	};
	Data::AccountSpamStatusChanges(
	) | rpl::filter([=](uint64 id) {
		return id == userId;
	}) | rpl::on_next([=](uint64) {
		onceSettled();
	}, *lifetime);
	base::call_delayed(
		kSpamBotReplyTimeout + crl::time(1000),
		&account->session(),
		[=] { onceSettled(); });
	CheckAccountSpamStatus(account, [=](QString reason) {
		// Only the outright-failure cases get a toast (couldn't resolve
		// @spambot, couldn't send it "/start") - a bare timeout is the
		// normal "no reply yet" outcome and would just be noise, especially
		// with a whole queue of accounts checked one after another.
		if (state->show && !reason.isEmpty()) {
			const auto name = account->sessionExists()
				? account->session().user()->name()
				: QString();
			Ggtg::Notice(state->show, (name.isEmpty()
				? QString()
				: (name + u" - "_q)) + u"не удалось проверить: "_q + reason);
		}
		onceSettled();
	});
}

// Checks every account in `queue`, one at a time (never in parallel - each
// one still goes out through its own account's own proxy, since that's
// simply whichever proxy that account's own session/connection already
// uses), in order, until the queue is exhausted or another check is already
// running (spamCheckBusy set by someone else in the meantime).
void RunSpamCheckQueue(
		not_null<CampaignState*> state,
		std::shared_ptr<std::vector<not_null<Main::Account*>>> queue,
		int index,
		Fn<void()> done) {
	if (index >= int(queue->size()) || state->spamCheckBusy) {
		if (done) {
			done();
		}
		return;
	}
	state->spamCheckBusy = true;
	RunAccountSpamCheck(state, (*queue)[index], [=] {
		state->spamCheckBusy = false;
		RunSpamCheckQueue(state, queue, index + 1, done);
	});
}

void AddCampaignAccountRow(
		not_null<Ui::VerticalLayout*> container,
		not_null<Main::Account*> account,
		not_null<CampaignState*> state) {
	// Every literal pixel offset in this row is scaled with
	// style::ConvertScale the same way st::defaultCheckbox's own
	// checkPosition/diameter already are - see AddAccountPickerRow's
	// identical comment for why an unscaled literal next to an
	// already-scaled style value drifts out of proportion at any UI scale
	// other than 100%.
	const auto userpicSize = style::ConvertScale(kPickerUserpicSize);
	const auto rowHeight = userpicSize + style::ConvertScale(8);
	auto rowOwned = object_ptr<Ui::RpWidget>(container);
	const auto row = rowOwned.data();
	row->resize(row->width(), rowHeight);

	const auto user = account->session().user();

	// Left to right, as asked: selection checkbox, then photo, then
	// name/phone (elided so it can never grow into the status badge on
	// the right - that overlap was the actual bug behind the account
	// rows reading as garbled/overlapping text in narrower windows).
	const auto checkbox = Ui::CreateChild<Ui::Checkbox>(row, QString(), state->selectedAccounts.contains(account));
	// st::defaultCheckbox's `width: -44px` bakes in room for a text label
	// via Checkbox::updateNaturalWidth() even when the label is empty (as
	// it always is here), so the widget's own hit-tested/painted bounding
	// box was ~74px wide - almost 2.5x the glyph's real ~30px footprint,
	// reaching well past where the userpic starts and overlapping it (the
	// actual source of the reported crooked/broken-looking checkbox -
	// ripple/hover painted across that whole overlap, not just the glyph).
	// Force the widget down to the glyph's own tight bounds instead.
	const auto checkboxVisualWidth = st::defaultCheckbox.checkPosition.x()
		+ st::defaultCheck.diameter;
	// See AddAccountPickerRow's identical comment: only the width needs
	// checkPosition.x()'s padding (that axis is left-aligned, not
	// centered) - the height only needs to fit the glyph itself, since
	// Ui::Checkbox centers it vertically regardless of the widget's own
	// height. A taller widget here would only grow the hit-tested (but
	// not actually drawn on) area above and below the visible glyph.
	checkbox->resize(checkboxVisualWidth, st::defaultCheck.diameter);
	const auto userpicLeft = checkboxVisualWidth + style::ConvertScale(8);
	const auto textLeft = userpicLeft + userpicSize + style::ConvertScale(12);
	// See AddAccountPickerRow's identical comment: geometric centering is
	// mathematically exact here, but a small square checkbox next to a
	// larger round photo still reads as sitting high without a small
	// optical nudge down.
	const auto checkboxOpticalNudge = style::ConvertScale(7);
	checkbox->moveToLeft(
		0, (row->height() - checkbox->height()) / 2 + checkboxOpticalNudge);

	const auto view = row->lifetime().make_state<Ui::PeerUserpicView>();
	const auto userpic = Ui::CreateChild<Ui::RpWidget>(row);
	userpic->setAttribute(Qt::WA_TransparentForMouseEvents);
	userpic->resize(userpicSize, userpicSize);
	userpic->moveToLeft(userpicLeft, (row->height() - userpicSize) / 2);
	userpic->paintRequest(
	) | rpl::on_next([=] {
		auto p = Painter(userpic);
		user->paintUserpicLeft(
			p, *view, 0, 0, userpicSize, userpicSize);
	}, userpic->lifetime());

	// Name on top, "@username · phone" under it: nothing is cut with "…" -
	// both wrap to the width left of the status badge, and the row grows
	// to fit them (see the widthValue handler below).
	const auto nameText = user->name().isEmpty() ? u"Аккаунт"_q : user->name();
	const auto userId = user->id.value;
	auto detailParts = QStringList();
	if (!user->username().isEmpty()) {
		detailParts.push_back(u"@"_q + user->username());
	}
	if (!user->phone().isEmpty()) {
		detailParts.push_back(user->phone());
	}
	const auto detailsText = detailParts.join(u"  ·  "_q);
	const auto nameLabel = Ui::CreateChild<Ui::FlatLabel>(
		row,
		nameText,
		st::boxLabel);
	nameLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
	const auto detailsLabel = Ui::CreateChild<Ui::FlatLabel>(
		row,
		detailsText,
		st::boxLabel);
	detailsLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
	detailsLabel->setTextColorOverride(st::windowSubTextFg->c);
	detailsLabel->setVisible(!detailsText.isEmpty());
	// Telegram's Premium star / emoji status right after the name.
	const auto premium = Ui::CreateChild<Ggtg::PremiumBadge>(
		row,
		user);
	// How many this account has sent in the last 24 hours, live while a
	// campaign runs; nothing at all while it is zero.
	const auto todayLabel = Ui::CreateChild<Ui::FlatLabel>(
		row,
		QString(),
		st::boxLabel);
	todayLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
	todayLabel->setTextColorOverride(st::windowActiveTextFg->c);
	const auto refreshToday = [=] {
		const auto sent = Data::CountSmsSentToday(user->id.value);
		todayLabel->setText(sent > 0
			? (u"за сутки отправлено: "_q + QString::number(sent))
			: QString());
		todayLabel->setVisible(sent > 0);
		row->resize(row->width() + 1, row->height());
		row->resize(row->width() - 1, row->height());
	};
	refreshToday();
	Data::SmsSentTodayChanges(
	) | rpl::filter([=](uint64 id) {
		return id == user->id.value;
	}) | rpl::on_next([=](uint64) {
		refreshToday();
	}, row->lifetime());

	// Clickable: a click here re-checks this one account's SPAM-status with
	// @spambot instead of toggling the row's selection - see startCheck
	// below, and the exclusion for it in the row's own click routing. Stays
	// transparent-for-mouse-events like the other row labels so the click
	// actually reaches row->events() below in the first place (the same
	// reason the checkbox itself is excluded there instead of relying on
	// its own native click handling).
	const auto badge = Ui::CreateChild<Ui::FlatLabel>(row, QString(), st::boxLabel);
	badge->setAttribute(Qt::WA_TransparentForMouseEvents);
	const auto refreshBadge = [=] {
		if (state->spamCheckingAccount == account.get()) {
			badge->setText(u"Проверка…"_q);
			badge->setTextColorOverride(st::windowSubTextFg->c);
			return;
		}
		badge->setText(AccountSpamStatusText(userId));
		badge->setTextColorOverride(AccountSpamStatusColor(userId));
	};
	refreshBadge();
	Data::AccountSpamStatusChanges(
	) | rpl::filter([=](uint64 id) {
		return id == userId;
	}) | rpl::on_next([=](uint64) {
		refreshBadge();
	}, row->lifetime());
	state->spamCheckActivityChanged.events(
	) | rpl::on_next([=] {
		refreshBadge();
	}, row->lifetime());

	const auto startCheck = [=] {
		if (state->spamCheckBusy || !Main::AccountIsLive(account)) {
			return;
		}
		state->spamCheckBusy = true;
		RunAccountSpamCheck(state, account, [=] {
			state->spamCheckBusy = false;
		});
	};

	row->widthValue(
	) | rpl::on_next([=](int width) {
		const auto gap = style::ConvertScale(16);
		const auto badgeWidth = premium->badgeWidth();
		const auto available = std::max(
			width - textLeft - badge->width() - gap - badgeWidth,
			style::ConvertScale(60));
		nameLabel->resizeToWidth(available);
		detailsLabel->resizeToWidth(available);
		todayLabel->resizeToWidth(available);
		const auto todayHeight = todayLabel->isHidden()
			? 0
			: todayLabel->height();
		const auto textHeight = nameLabel->height()
			+ (detailsText.isEmpty() ? 0 : detailsLabel->height())
			+ todayHeight;
		const auto height = std::max(
			rowHeight,
			textHeight + 2 * style::ConvertScale(6));
		if (row->height() != height) {
			row->resize(width, height);
		}
		checkbox->moveToLeft(
			0,
			(height - checkbox->height()) / 2 + checkboxOpticalNudge);
		userpic->moveToLeft(userpicLeft, (height - userpicSize) / 2);
		auto top = (height - textHeight) / 2;
		nameLabel->moveToLeft(textLeft, top, width);
		if (badgeWidth > 0) {
			premium->moveToLeft(
				textLeft + std::min(
					nameLabel->naturalWidth(),
					nameLabel->width()),
				top,
				width);
		}
		if (!detailsText.isEmpty()) {
			detailsLabel->moveToLeft(
				textLeft,
				top + nameLabel->height(),
				width);
		}
		todayLabel->moveToLeft(
			textLeft,
			top + nameLabel->height()
				+ (detailsText.isEmpty() ? 0 : detailsLabel->height()),
			width);
		badge->moveToRight(
			style::ConvertScale(8),
			(height - badge->height()) / 2,
			width);
	}, row->lifetime());

	// One click handler for the whole row (see Ggtg::SetupRowClicks): the
	// status badge starts a check, anywhere else toggles the checkbox.
	Ggtg::SetupRowClicks(row, { checkbox }, [=](QPoint pos) {
		if (badge->geometry().contains(pos)) {
			startCheck();
		} else {
			checkbox->setChecked(!checkbox->checked());
		}
	});

	checkbox->checkedChanges(
	) | rpl::on_next([=](bool checked) {
		if (checked) {
			state->selectedAccounts.emplace(account);
		} else {
			state->selectedAccounts.remove(account);
		}
		RefreshAccountsSelectionUi(state);
	}, row->lifetime());

	// See the same fix's comment in bulk_accounts_actions.cpp's
	// AddAccountPickerRow - without this margin these rows sat flush
	// against the box edge while every other row in the box (added via
	// box->addRow) was properly inset by st::boxRowPadding.
	container->add(std::move(rowOwned), st::accountsPickerRowPadding);
}

// Qt only re-evaluates which widget is "under" the mouse (and therefore
// which one's cursor/hover state to show) on an actual mouse-move event
// from the platform - it does not re-run that check just because the
// widget tree changed under an already-stationary pointer. Expanding a
// folder (or toggling a folder/"выбрать всех" selection) adds or removes
// whole rows exactly where the mouse may already be resting, so a
// checkbox that just appeared there won't show its pointer cursor, and a
// mouse-enter-driven hover highlight won't appear either, until the user
// nudges the mouse a pixel - reads as "the checkbox is dead" even though
// an actual click there is still hit-tested fresh and works fine. Posting
// a synthetic move at the pointer's own current position (not moving it)
// forces that re-evaluation immediately.
void RefreshHoverAfterRebuild(not_null<QWidget*> widget) {
	// A synthetic move through the window handle (the same helper lib_ui
	// uses), not a postEvent to the QWidget: only the former reaches Qt's
	// own "which child is under the pointer" tracking.
	Ui::SendSynteticMouseEvent(widget, QEvent::MouseMove, Qt::NoButton);
}

void RebuildAccountsAccordion(
		not_null<Ui::VerticalLayout*> accordion,
		not_null<CampaignState*> state) {
	RebuildAccountAccordion(accordion, AccountAccordionHooks{
		.selected = &state->selectedAccounts,
		.expandedFolders = &state->expandedFolders,
		.selectionChanged = [=] { RefreshAccountsSelectionUi(state); },
		// Worst-off accounts first: a permanent block, then a still-active
		// temporary one, then everything else - see AccountSpamSeverityRank.
		.sortFolder = [](Accounts &accounts) {
			ranges::stable_sort(accounts, [](
					not_null<Main::Account*> a,
					not_null<Main::Account*> b) {
				return AccountSpamSeverityRank(a)
					> AccountSpamSeverityRank(b);
			});
		},
		// Each row also shows the account's @spambot status and starts a
		// re-check when clicked.
		.addAccountRow = [=](
				not_null<Ui::VerticalLayout*> container,
				not_null<Main::Account*> account) {
			AddCampaignAccountRow(container, account, state);
		},
		// "Проверить": checks every account in this one folder, one after
		// another - see RunSpamCheckQueue. A real button that handles its
		// own click; the shared header ignores clicks on it.
		.addHeaderWidget = [=](
				not_null<Ui::RpWidget*> header,
				const Accounts &accounts) -> QWidget* {
			const auto checkFolder = Ui::CreateChild<Ui::LinkButton>(
				header.get(), u"Проверить"_q);
			checkFolder->setClickedCallback([=] {
				if (state->spamCheckBusy) {
					return;
				}
				auto queue = std::make_shared<
					std::vector<not_null<Main::Account*>>>();
				for (const auto &acc : accounts) {
					if (acc->sessionExists()) {
						queue->push_back(acc);
					}
				}
				RunSpamCheckQueue(state, queue, 0, nullptr);
			});
			header->widthValue(
			) | rpl::on_next([=](int width) {
				checkFolder->moveToRight(
					style::ConvertScale(8),
					(header->height() - checkFolder->height()) / 2,
					width);
			}, header->lifetime());
			return checkFolder;
		},
	});
}

void SetupAccountsSection(
		not_null<Ui::GenericBox*> box,
		not_null<CampaignState*> state,
		not_null<Ui::VerticalLayout*> content) {
	// Rows go into the step card's own column, not straight into the box.
	const auto addRow = [=](
			auto widget,
			const style::margins &margins = st::boxRowPadding) {
		return content->add(std::move(widget), margins);
	};
	const auto applyToAll = [=](bool select) {
		for (const auto &account : Core::App().domain().orderedAccounts()) {
			if (!Main::AccountIsLive(account)) {
				continue;
			}
			if (select) {
				if (IsAccountSpamBlocked(account)) {
					continue;
				}
				state->selectedAccounts.emplace(account);
			} else {
				state->selectedAccounts.remove(account);
			}
		}
		RebuildAccountsAccordion(state->accountsAccordion, state);
		RefreshHoverAfterRebuild(state->accountsAccordion);
		RefreshAccountsSelectionUi(state);
	};
	// Quiet links instead of full-width buttons: "Проверить статус" asks
	// @spambot about every known account, one after another (each folder
	// also has its own "Проверить" next to its name).
	const auto links = addRow(object_ptr<SmsWidgets::LinksRow>(box));
	links->add(u"Выбрать все"_q, [=] { applyToAll(true); });
	links->add(u"Снять выбор"_q, [=] { applyToAll(false); });
	const auto checkAll = links->add(
		state->checkAllButtonText.current(),
		[=] {
			if (state->spamCheckBusy) {
				return;
			}
			auto queue = std::make_shared<
				std::vector<not_null<Main::Account*>>>();
			for (const auto &account
					: Core::App().domain().orderedAccounts()) {
				if (account->sessionExists()) {
					queue->push_back(account);
				}
			}
			state->checkAllButtonText = u"Проверка…"_q;
			RunSpamCheckQueue(state, queue, 0, [=] {
				state->checkAllButtonText = u"Проверить статус"_q;
			});
		});
	state->checkAllButtonText.value(
	) | rpl::on_next([=](const QString &text) {
		checkAll->setText(text);
		links->relayout();
	}, links->lifetime());
	Ui::AddSkip(content);

	state->accountsAccordion = addRow(
		object_ptr<Ui::VerticalLayout>(box));
	RebuildAccountsAccordion(state->accountsAccordion, state);
}

// ---- "ЛИМИТЫ И ПОТОКИ": read by RunSmsCampaign, see ParseNonNegative ----

// A row of `widget` with a small "i" at its right end: the explanation that
// used to sit as a muted paragraph under the field opens on click instead.
template <typename Widget>
Widget *AddRowWithInfo(
		not_null<Ui::GenericBox*> box,
		not_null<Ui::VerticalLayout*> content,
		object_ptr<Widget> widget,
		const QString &hint) {
	const auto raw = widget.data();
	const auto height = std::max(raw->height(), st::ggtgInfoButton.height);
	const auto wrap = content->add(
		object_ptr<Ui::FixedHeightWidget>(content, height),
		st::boxRowPadding);
	raw->setParent(wrap);
	widget.release();
	raw->show();
	const auto info = Ui::CreateChild<Ui::IconButton>(
		wrap,
		st::ggtgInfoButton);
	info->setToolTip(u"Подсказка"_q);
	info->setClickedCallback([=] {
		box->uiShow()->showBox(Ui::MakeInformBox(hint));
	});
	wrap->widthValue(
	) | rpl::on_next([=](int width) {
		const auto room = std::max(width - info->width(), 0);
		// A checkbox paints its glyph checkPosition.x() in from its own
		// edge; pull it back so it lines up with the fields above.
		const auto shift = std::is_base_of_v<Ui::Checkbox, Widget>
			? st::defaultCheckbox.checkPosition.x()
			: 0;
		raw->resize(room + shift, raw->height());
		raw->move(-shift, (height - raw->height()) / 2);
		info->move(room, (height - info->height()) / 2);
	}, wrap->lifetime());
	return raw;
}

void UpdateRotationLabel(not_null<CampaignState*> state);

// "Мои наборы": the text and the numbers of a form saved under a name, to
// bring back later in one click (next to the ready-made modes).
struct UserPreset {
	QString name;
	QString text;
	QString perAccount, threads, spam, refusals, pause, accountPause;
	bool sticker = false;
	std::vector<uint64> rotation;
	int protectionMode = 1;
	bool nightSleep = true;
	bool simulateTypos = true;
	bool filterByGgr = false;
};

[[nodiscard]] QString UserPresetsPath() {
	return Core::GgtgPaths::DbDir() + u"sms_user_presets.json"_q;
}

[[nodiscard]] std::vector<UserPreset> LoadUserPresets() {
	auto result = std::vector<UserPreset>();
	auto file = QFile(UserPresetsPath());
	if (!file.open(QIODevice::ReadOnly)) {
		return result;
	}
	const auto array = QJsonDocument::fromJson(file.readAll()).array();
	for (const auto &value : array) {
		const auto o = value.toObject();
		auto p = UserPreset();
		p.name = o.value("name").toString();
		p.text = o.value("text").toString();
		p.perAccount = o.value("perAccount").toString();
		p.threads = o.value("threads").toString();
		p.spam = o.value("spam").toString();
		p.refusals = o.value("refusals").toString();
		p.pause = o.value("pause").toString();
		p.accountPause = o.value("accountPause").toString();
		p.sticker = o.value("sticker").toBool();
		p.protectionMode = o.contains("protectionMode")
			? o.value("protectionMode").toInt(1)
			: 1;
		p.nightSleep = o.contains("nightSleep")
			? o.value("nightSleep").toBool(true)
			: true;
		p.simulateTypos = o.contains("simulateTypos")
			? o.value("simulateTypos").toBool(true)
			: true;
		for (const auto &id : o.value("rotation").toArray()) {
			p.rotation.push_back(uint64(id.toString().toULongLong()));
		}
		if (!p.name.isEmpty()) {
			result.push_back(std::move(p));
		}
	}
	return result;
}

void SaveUserPresets(const std::vector<UserPreset> &list) {
	auto array = QJsonArray();
	for (const auto &p : list) {
		auto rotation = QJsonArray();
		for (const auto id : p.rotation) {
			rotation.push_back(QString::number(id));
		}
		array.push_back(QJsonObject{
			{ "name", p.name }, { "text", p.text },
			{ "perAccount", p.perAccount }, { "threads", p.threads },
			{ "spam", p.spam }, { "refusals", p.refusals },
			{ "pause", p.pause }, { "accountPause", p.accountPause },
			{ "sticker", p.sticker }, { "rotation", rotation },
			{ "protectionMode", p.protectionMode },
			{ "nightSleep", p.nightSleep },
			{ "simulateTypos", p.simulateTypos },
		});
	}
	auto file = QSaveFile(UserPresetsPath());
	if (file.open(QIODevice::WriteOnly)) {
		file.write(QJsonDocument(array).toJson(QJsonDocument::Compact));
		file.commit();
	}
}

// The ready-made modes of "Режим"; "Свой" is whatever doesn't match one.
struct LimitsPreset {
	QString key;
	QString name;
	QString perAccount, threads, spam, refusals, pause, accountPause;
};

[[nodiscard]] const std::vector<LimitsPreset> &LimitsPresets() {
	static const auto result = std::vector<LimitsPreset>{
		{ u"normal"_q, u"Нормально"_q,
			u"2"_q, u"3"_q, u"1"_q, u"3"_q, u"30-40"_q, u"30-40"_q },
		{ u"premium"_q, u"Премиум"_q,
			u"40"_q, u"1"_q, u"2"_q, u"3"_q, u"30-40"_q, u"30-40"_q },
	};
	return result;
}

constexpr auto kCustomLimitsKey = "custom";

void SetupLimitsSection(
		not_null<Ui::GenericBox*> box,
		not_null<CampaignState*> state,
		not_null<Ui::VerticalLayout*> content) {
	const auto addRow = [=](
			auto widget,
			const style::margins &margins = st::boxRowPadding) {
		return content->add(std::move(widget), margins);
	};
	const auto &presets = LimitsPresets();
	const auto field = [](const QPointer<Ui::InputField> &f) {
		return f ? f->getLastText().trimmed() : QString();
	};
	// Which mode the numbers in the fields are: a ready-made one when all
	// of them match it, "Свой" otherwise.
	const auto detectMode = [=] {
		for (auto i = 0; i != int(presets.size()); ++i) {
			const auto &p = presets[i];
			if (field(state->limitPerAccount) == p.perAccount
				&& field(state->limitThreads) == p.threads
				&& field(state->limitSkipSpamBlock) == p.spam
				&& field(state->limitRefusals) == p.refusals
				&& field(state->limitPauseRange) == p.pause
				&& field(state->limitAccountPauseRange) == p.accountPause) {
				return i;
			}
		}
		return int(presets.size());
	};

	auto names = QStringList();
	for (const auto &p : presets) {
		names.push_back(p.name);
	}
	names.push_back(u"Свой"_q);
	const auto tabs = addRow(
		object_ptr<AiTabs>(box, names, 0),
		style::margins(
			st::boxRowPadding.left(),
			0,
			st::boxRowPadding.right(),
			st::defaultVerticalListSkip));
	// "2 сообщения с аккаунта · 3 потока · пауза 30-40 с ...".
	const auto summary = addRow(object_ptr<Ui::FlatLabel>(
		box,
		QString(),
		st::boxDividerLabel));
	Ui::AddSkip(content);

	const auto links = addRow(object_ptr<SmsWidgets::LinksRow>(box));
	const auto manual = addRow(
		object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			box,
			object_ptr<Ui::VerticalLayout>(box)),
		style::margins());
	manual->hide(anim::type::instant);
	const auto manualContent = manual->entity();
	Ui::AddSkip(manualContent);

	// A field's last-used text when the form was saved before, the
	// "Нормально" value otherwise (an empty saved value falls back too, so a
	// cleared field doesn't come back as a blank, unparseable one).
	const auto initial = [=](const QString &saved, const QString &fallback) {
		return (state->saved.saved && !saved.isEmpty()) ? saved : fallback;
	};
	const auto rangeInitial = [=](
			const QString &savedMin,
			const QString &savedMax,
			const QString &fallback) {
		return (state->saved.saved && !savedMin.isEmpty())
			? ((savedMax.isEmpty() || savedMax == savedMin)
				? savedMin
				: (savedMin + '-' + savedMax))
			: fallback;
	};
	// Each field carries its own explanation behind an "i" at its end.
	const auto addField = [=](
			const QString &placeholder,
			const QString &value,
			const QString &hint) {
		const auto result = AddRowWithInfo(
			box,
			manualContent,
			object_ptr<Ui::InputField>(
				box,
				st::ggtgCardInputField,
				rpl::single(placeholder),
				value),
			hint);
		Ui::AddSkip(manualContent);
		return result;
	};
	const auto &normal = presets.front();
	state->limitPerAccount = addField(
		u"Сообщений с одного аккаунта"_q,
		initial(state->saved.limitPerAccount, normal.perAccount),
		u"Сколько успешных отправок делает аккаунт, после чего поток берёт "
			"следующий. Пропущенные и неудачные попытки не считаются. 0 - "
			"без лимита: пишет, пока не кончатся получатели."_q);
	state->limitThreads = addField(
		u"Потоков (одновременно)"_q,
		initial(state->saved.limitThreads, normal.threads),
		u"Сколько аккаунтов пишут одновременно разным получателям, "
			"без пересечений."_q);
	state->limitSkipSpamBlock = addField(
		u"Отказов Telegram до проверки в @SpamBot"_q,
		initial(state->saved.limitSkipSpamBlock, normal.spam),
		u"Сколько отказов Telegram (PEER_FLOOD) у аккаунта за сегодня можно "
			"пропустить, прежде чем спросить @SpamBot, не в спам-блоке ли он. "
			"Свободен - пишет дальше (отказ считается проблемой пары "
			"«аккаунт - человек»), в спам-блоке - снимается. 0 - спрашивать "
			"после первого отказа."_q);
	state->limitRefusals = addField(
		u"Снять аккаунт после отказов"_q,
		initial(state->saved.limitRefusals, normal.refusals),
		u"Сколько отказов Telegram (PEER_FLOOD при свободном по @SpamBot "
			"аккаунте) аккаунт терпит за рассылку - успешные отправки счёт "
			"не обнуляют. Дошёл до этого числа - снимается. Получатели, "
			"которым отказали, получают пометку «Telegram не дал написать»; "
			"снять её можно в «Пометках» у получателей. 0 - по отказам не "
			"снимать."_q);
	state->limitPauseRange = addField(
		u"Пауза между сообщениями, сек"_q,
		rangeInitial(state->saved.pauseMin, state->saved.pauseMax, normal.pause),
		u"Случайная пауза после каждого шага потока - и после отправки, и "
			"после пропуска получателя. Формат «от-до», например 30-40, или "
			"одно число."_q);
	state->limitAccountPauseRange = addField(
		u"Пауза между аккаунтами, сек"_q,
		rangeInitial(
			state->saved.pauseAccountMin,
			state->saved.pauseAccountMax,
			normal.accountPause),
		u"Случайная пауза перед тем, как поток возьмёт следующий аккаунт - и "
			"когда текущий отправил свою норму, и когда его сняли. Формат "
			"«от-до», например 30-40."_q);
	manualContent->add(
		object_ptr<Ui::FlatLabel>(
			manualContent,
			u"Аккаунт снимается с рассылки сразу при FLOOD_WAIT, спам-блоке "
				"или недействительной сессии, а ещё после "_q
				+ QString::number(kSmsErrorStreakLimit)
				+ u" сбоев, "_q + QString::number(kNotFoundStreakLimit)
				+ u" «не найден», заданного числа отказов или своего лимита "
					"сообщений. Получатель, до которого аккаунт не смог "
					"достучаться, получает пометку «аккаунт не смог написать»."_q,
			st::boxDividerLabel),
		st::boxRowPadding);
	Ui::AddSkip(manualContent);

	const auto refreshSummary = [=] {
		const auto [pauseMin, pauseMax] = ParseSecondsRange(
			state->limitPauseRange,
			int(kSmsPauseMin / 1000),
			int(kSmsPauseMax / 1000));
		const auto quota = ParseNonNegative(state->limitPerAccount, 1);
		const auto threads = std::max(
			ParseNonNegative(state->limitThreads, 1),
			1);
		const auto refusals = ParseNonNegative(
			state->limitRefusals,
			kSmsPeerRefusalStreakLimit);
		const auto pause = (pauseMin == pauseMax)
			? QString::number(pauseMin)
			: (QString::number(pauseMin) + u"–"_q + QString::number(pauseMax));
		auto parts = QStringList{
			(quota > 0)
				? (QString::number(quota) + u" сообщ. с аккаунта"_q)
				: u"без лимита на аккаунт"_q,
			QString::number(threads)
				+ (threads == 1 ? u" поток"_q : u" потока"_q),
			u"пауза "_q + pause + u" с"_q,
		};
		if (refusals > 0) {
			parts.push_back(u"снять после "_q
				+ QString::number(refusals) + u" отказов"_q);
		}
		const auto line = parts.join(u" · "_q);
		summary->setText(line);
		const auto mode = tabs->active();
		state->limitsSummary = names[mode] + u" · "_q
			+ parts.mid(0, 2).join(u" · "_q);
		state->limitMode = (mode < int(presets.size()))
			? presets[mode].key
			: QString(kCustomLimitsKey);
		if (state->refreshPreflight) {
			state->refreshPreflight();
		}
	};
	// Any edit by hand picks the mode the numbers now match ("Свой" when
	// none).
	for (const auto &f : {
			state->limitPerAccount,
			state->limitThreads,
			state->limitSkipSpamBlock,
			state->limitRefusals,
			state->limitPauseRange,
			state->limitAccountPauseRange }) {
		f->changes(
		) | rpl::on_next([=] {
			tabs->setActive(detectMode());
			refreshSummary();
		}, f->lifetime());
	}

	const auto manualLink = links->add(u"Настроить вручную ▾"_q, [] {});
	const auto setManualShown = [=](bool shown) {
		manual->toggle(shown, anim::type::normal);
		manualLink->setText(shown
			? u"Скрыть настройки ▴"_q
			: u"Настроить вручную ▾"_q);
		links->relayout();
	};
	manualLink->setClickedCallback([=] {
		setManualShown(manual->toggled() ? false : true);
	});

	const auto apply = [=](const LimitsPreset &p) {
		const auto set = [](QPointer<Ui::InputField> f, const QString &v) {
			if (f && f->getLastText() != v) {
				f->setText(v);
			}
		};
		set(state->limitPerAccount, p.perAccount);
		set(state->limitThreads, p.threads);
		set(state->limitSkipSpamBlock, p.spam);
		set(state->limitRefusals, p.refusals);
		set(state->limitPauseRange, p.pause);
		set(state->limitAccountPauseRange, p.accountPause);
	};
	tabs->activeChanges(
	) | rpl::on_next([=](int index) {
		if (index < int(presets.size())) {
			apply(presets[index]);
			tabs->setActive(index);
		} else {
			setManualShown(true);
		}
		refreshSummary();
	}, tabs->lifetime());

	const auto mineMenu = box->lifetime().make_state<
		base::unique_qptr<Ui::PopupMenu>>();
	links->add(u"Мои наборы ▾"_q, [=] {
		*mineMenu = base::make_unique_q<Ui::PopupMenu>(
			box,
			st::popupMenuWithIcons);
		(*mineMenu)->addAction(u"Сохранить текущие настройки"_q, [=] {
			auto list = LoadUserPresets();
			auto p = UserPreset();
			p.text = state->messageField
				? state->messageField->getLastText()
				: QString();
			p.perAccount = field(state->limitPerAccount);
			p.threads = field(state->limitThreads);
			p.spam = field(state->limitSkipSpamBlock);
			p.refusals = field(state->limitRefusals);
			p.pause = field(state->limitPauseRange);
			p.accountPause = field(state->limitAccountPauseRange);
			p.sticker = state->greetingStickerCheckbox
				&& state->greetingStickerCheckbox->checked();
			p.rotation = state->rotationPromptIds;
			p.protectionMode = state->protectionMode;
			p.nightSleep = state->nightSleepCheckbox
				? state->nightSleepCheckbox->checked()
				: true;
			p.simulateTypos = state->simulateTyposCheckbox
				? state->simulateTyposCheckbox->checked()
				: true;
			p.name = QDateTime::currentDateTime().toString(u"dd.MM HH:mm"_q)
				+ u" · "_q + p.text.simplified().left(24);
			list.push_back(std::move(p));
			SaveUserPresets(list);
			Ggtg::Notice(state->show, u"Набор сохранён"_q);
		}, &st::menuIconDownload);
		for (const auto &saved : LoadUserPresets()) {
			(*mineMenu)->addAction(saved.name, [=] {
				const auto set = [](QPointer<Ui::InputField> f, QString v) {
					if (f && !v.isEmpty()) {
						f->setText(v);
					}
				};
				if (state->messageField) {
					state->messageField->setText(saved.text);
				}
				set(state->limitPerAccount, saved.perAccount);
				set(state->limitThreads, saved.threads);
				set(state->limitSkipSpamBlock, saved.spam);
				set(state->limitRefusals, saved.refusals);
				set(state->limitPauseRange, saved.pause);
				set(state->limitAccountPauseRange, saved.accountPause);
				if (state->greetingStickerCheckbox) {
					state->greetingStickerCheckbox->setChecked(saved.sticker);
				}
				if (state->protectionGroup) {
					state->protectionGroup->setValue(saved.protectionMode);
				}
				state->protectionMode = saved.protectionMode;
				if (state->nightSleepCheckbox) {
					state->nightSleepCheckbox->setChecked(saved.nightSleep);
				}
				if (state->simulateTyposCheckbox) {
					state->simulateTyposCheckbox->setChecked(saved.simulateTypos);
				}
				state->rotationPromptIds = saved.rotation;
				UpdateRotationLabel(state);
				Ggtg::Notice(state->show, u"Набор загружен"_q);
			}, &st::menuIconSelect);
			(*mineMenu)->addAction(u"   Удалить: "_q + saved.name, [=] {
				auto list = LoadUserPresets();
				list.erase(
					std::remove_if(list.begin(), list.end(), [&](
							const UserPreset &p) {
						return p.name == saved.name;
					}),
					list.end());
				SaveUserPresets(list);
				Ggtg::Notice(state->show, u"Набор удалён"_q);
			}, &st::menuIconDelete);
		}
		(*mineMenu)->popup(QCursor::pos());
	});

	// The mode saved last wins over guessing it from the numbers, so a
	// "Свой" that happens to equal a preset stays "Свой".
	auto start = detectMode();
	if (state->saved.limitMode == QString(kCustomLimitsKey)) {
		start = int(presets.size());
	}
	tabs->setActive(start);
	refreshSummary();
}

// ---- "ПОЛУЧАТЕЛИ": multi-list selection + merged preview ----

// The selected bases merged into one duplicate-free list. `baseOf`, when
// given, gets each username's (lowercase) source base - the first selected
// one that contains it - so the send registry can remember where every
// recipient came from.
[[nodiscard]] QStringList MergedRecipients(
		not_null<CampaignState*> state,
		base::flat_map<QString, uint64> *baseOf = nullptr) {
	auto seen = base::flat_set<QString>();
	auto result = QStringList();
	for (const auto &meta : state->basesIndex) {
		if (!state->selectedBaseIds.contains(meta.id)) {
			continue;
		}
		for (const auto &username : Data::LoadScrapedGroupUsernames(meta.id)) {
			const auto key = username.toLower();
			if (seen.emplace(key).second) {
				result.push_back(username);
				if (baseOf) {
					baseOf->emplace(key, meta.id);
				}
			}
		}
	}
	return result;
}

void RefreshRecipientsPreview(not_null<CampaignState*> state) {
	const auto stats = Data::ComputeBaseStats(MergedRecipients(state));
	if (const auto label = state->recipientsStatsLabel.data()) {
		const auto skipped = SmsModel::SkipBreakdown(
			stats.sent,
			stats.peerLimit,
			stats.restricted,
			stats.notFound,
			stats.spamAcc,
			stats.accountFail,
			stats.bots);
		label->setText(skipped.isEmpty()
			? QString()
			: (u"Пропущено: "_q + skipped));
		label->setVisible(!skipped.isEmpty());
	}
	if (state->refreshPreflight) {
		state->refreshPreflight();
	}
}

// Rebuilds just the "Все списки" + per-base checkboxes into `container` (a
// child of the ПОЛУЧАТЕЛИ section - see SetupRecipientsSection) without
// touching the rest of the "Отправка SMS" box. Used both for the
// section's initial fill and to pick up a base added/renamed/deleted via
// "Управлять списками получателей..." once that box closes (see the
// button's boxClosing() subscription below) - previously that list only
// loaded once when the "Отправка SMS" box itself was first opened, so a
// list added while it was already open never showed up until the whole
// box was closed and reopened from scratch.
//
// A base already known before this call keeps whatever checked state the
// user gave it; only a genuinely new id defaults to checked (matching
// the very first fill, where everything starts selected).
void RebuildBasesContainer(
		not_null<Ui::VerticalLayout*> container,
		not_null<CampaignState*> state) {
	const Ggtg::ScrollKeeper keepScroll(container);
	container->clear();
	state->baseCheckboxes.clear();

	auto previousIds = base::flat_set<uint64>();
	for (const auto &meta : state->basesIndex) {
		previousIds.emplace(meta.id);
	}
	state->basesIndex = Data::LoadScrapedGroupsIndex();
	// Recipients of an SMS campaign are people, so only user bases are
	// offered here - group bases (collected channels/chats) hold group
	// usernames, not members, and would only ever resolve to bots or fail.
	state->basesIndex.erase(
		ranges::remove_if(
			state->basesIndex,
			[](const Data::ScrapedGroupMeta &meta) {
				return meta.kind != Data::BaseKind::Users;
			}),
		state->basesIndex.end());
	for (const auto &meta : state->basesIndex) {
		// A list is never ticked for the user: a campaign goes out to every
		// ticked base, and a fresh (or forgotten) selection silently
		// covering hundreds of people is exactly the mistake to avoid. Only
		// what was explicitly ticked (and saved) stays ticked.
		state->knownBaseIds.emplace(meta.id);
	}

	const auto allSelected = [=] {
		return ranges::all_of(state->basesIndex, [=](
				const Data::ScrapedGroupMeta &meta) {
			return state->selectedBaseIds.contains(meta.id);
		});
	};
	state->allListsCheckbox = container->add(
		object_ptr<Ui::Checkbox>(
			container,
			u"Все списки"_q,
			allSelected()),
		style::margins(0, st::defaultCheckbox.margin.top() / 2, 0, st::defaultCheckbox.margin.bottom() / 2));
	// "Все списки" only makes sense with something to choose between.
	state->allListsCheckbox->setVisible(state->basesIndex.size() > 1);

	if (state->basesIndex.empty()) {
		container->add(object_ptr<Ui::FlatLabel>(
			container,
			u"Баз пока нет. Добавьте их в «Списки»."_q,
			st::boxDividerLabel));
	}
	for (const auto &meta : state->basesIndex) {
		const auto id = meta.id;
		// The checkbox with an eye at the end of its row: opens that list
		// to look through and edit it.
		// A normal checkbox with its own text - the very same widget and the
		// same margins as "Все списки" above, so the glyph and the title line
		// up exactly. The eye is a child of the checkbox itself, so it always
		// sits on the checkbox's own middle line, right after the title.
		// The same numbers as in "База": who is still left to write to, out
		// of everyone in the list (the stored size alone says nothing about
		// the ones already written or refused).
		const auto stats = Data::ComputeBaseStats(meta.id);
		const auto title = meta.title + u"  ·  "_q + (stats.remaining > 0
			? (QString::number(stats.remaining)
				+ u" из "_q + QString::number(stats.total))
			: u"некому писать"_q);
		const auto checkbox = container->add(
			object_ptr<Ui::Checkbox>(
				container,
				title,
				state->selectedBaseIds.contains(meta.id)),
			style::margins(
				0,
				st::defaultCheckbox.margin.top() / 2,
				0,
				st::defaultCheckbox.margin.bottom() / 2));
		checkbox->setAllowTextLines(0);
		const auto eye = Ui::CreateChild<Ui::IconButton>(
			checkbox,
			st::ggtgEyeButton);
		eye->show();
		eye->setToolTip(u"Открыть список"_q);
		eye->setClickedCallback([=] {
			if (const auto show = state->show) {
				ShowSavedBase(show, id);
			}
		});
		checkbox->sizeValue(
		) | rpl::on_next([=](QSize size) {
			const auto textLeft = st::defaultCheckbox.checkPosition.x()
				+ st::defaultCheck.diameter
				+ st::defaultCheckbox.textPosition.x();
			const auto textWidth = st::defaultCheckbox.style.font->width(
				title);
			const auto room = std::max(
				size.width() - eye->width() - textLeft,
				0);
			eye->moveToLeft(
				textLeft + std::min(textWidth, room)
					+ style::ConvertScale(2),
				(size.height() - eye->height()) / 2,
				size.width());
		}, eye->lifetime());
		state->baseCheckboxes.push_back(checkbox);
		checkbox->checkedChanges(
		) | rpl::on_next([=](bool checked) {
			if (checked) {
				state->selectedBaseIds.emplace(id);
			} else {
				state->selectedBaseIds.remove(id);
			}
			if (const auto all = state->allListsCheckbox.data()) {
				const auto nowAllSelected = allSelected();
				if (all->checked() != nowAllSelected) {
					all->setChecked(
						nowAllSelected,
						Ui::Checkbox::NotifyAboutChange::DontNotify);
				}
			}
			RefreshRecipientsPreview(state);
			// Saved right away: switching GGTG sections destroys this box
			// without a normal close, so the selection made here would
			// otherwise be lost.
			SaveFormState(state);
		}, checkbox->lifetime());
	}

	state->allListsCheckbox->checkedChanges(
	) | rpl::on_next([=](bool checked) {
		state->selectedBaseIds.clear();
		for (auto i = 0; i != int(state->basesIndex.size()); ++i) {
			if (checked) {
				state->selectedBaseIds.emplace(state->basesIndex[i].id);
			}
			if (const auto checkbox = state->baseCheckboxes[i].data()) {
				checkbox->setChecked(
					checked,
					Ui::Checkbox::NotifyAboutChange::DontNotify);
			}
		}
		RefreshRecipientsPreview(state);
		SaveFormState(state);
	}, state->allListsCheckbox->lifetime());

	RefreshRecipientsPreview(state);
}

void SetupRecipientsSection(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller,
		not_null<CampaignState*> state,
		not_null<Ui::VerticalLayout*> content) {
	// Rows go into the step card's own column, not straight into the box.
	const auto addRow = [=](
			auto widget,
			const style::margins &margins = st::boxRowPadding) {
		return content->add(std::move(widget), margins);
	};
	state->basesContainer = addRow(
		object_ptr<Ui::VerticalLayout>(box));
	RebuildBasesContainer(state->basesContainer, state);

	// Who is not going to be written to and why, only when there is any.
	state->recipientsStatsLabel = addRow(
		object_ptr<Ui::FlatLabel>(box, QString(), st::boxDividerLabel),
		style::margins(
			st::boxRowPadding.left(),
			st::defaultVerticalListSkip,
			st::boxRowPadding.right(),
			0));
	Ui::AddSkip(content);

	const auto links = addRow(object_ptr<SmsWidgets::LinksRow>(box));
	// The bases above refresh themselves on any change to them (see
	// Data::BasesChanged, subscribed in MakeSmsCampaignSetupBox).
	links->add(u"Списки"_q, [=] {
		controller->show(MakeScrapedGroupsDbBox(controller->uiShow()));
	});
	// Each kind of mark comes back into the queue on its own (a person
	// Telegram would not let an account write to, an account that was in a
	// spam block or couldn't write at all, not found, closed to messages),
	// or all of them at once.
	const auto marksMenu = box->lifetime().make_state<
		base::unique_qptr<Ui::PopupMenu>>();
	links->add(u"Пометки ▾"_q, [=] {
		if (Data::CountSmsResettable() == 0) {
			Ggtg::Notice(controller->uiShow(), u"Пометок нет"_q);
			return;
		}
		*marksMenu = base::make_unique_q<Ui::PopupMenu>(
			box,
			st::popupMenuWithIcons);
		for (const auto status : Data::SmsMarkKinds()) {
			const auto count = Data::CountSmsStatus(status);
			if (!count) {
				continue;
			}
			(*marksMenu)->addAction(
				u"Снять «"_q + Data::SmsMarkTitle(status) + u"» ("_q
					+ QString::number(count) + ')',
				[=] {
					const auto removed = Data::ClearSmsStatus(status);
					Ggtg::Notice(controller->uiShow(),
						u"Снова в очереди: "_q + QString::number(removed));
					RefreshRecipientsPreview(state);
				},
				&st::menuIconRestore);
		}
		(*marksMenu)->addAction(
			u"Снять все пометки ("_q
				+ QString::number(Data::CountSmsResettable()) + ')',
			[=] {
				Data::ClearAllSmsResettable();
				Ggtg::Notice(controller->uiShow(),
					u"Все пометки сняты, получатели снова в очереди"_q);
				RefreshRecipientsPreview(state);
			},
			&st::menuIconRestore);
		(*marksMenu)->popup(QCursor::pos());
	});
	links->add(u"Почистить базы"_q, [=] {
		controller->uiShow()->show(Ui::MakeConfirmBox({
			.text = u"Убрать из выбранных баз тех, кому уже написали, "
				"а также «не найден» и «закрыл ЛС»? Базы изменятся "
				"насовсем. Те, где виноват аккаунт (спам-блок, «не "
				"пустило», «аккаунт не смог»), останутся в базе."_q,
			.confirmed = [=](Fn<void()> &&close) {
				close();
				if (!Core::GuardAllows(
						controller->uiShow(),
						Core::GuardedAction::EditBases)) {
					return;
				}
				auto removed = 0;
				for (const auto &meta : state->basesIndex) {
					if (!state->selectedBaseIds.contains(meta.id)) {
						continue;
					}
					auto usernames = Data::LoadScrapedGroupUsernames(meta.id);
					const auto before = usernames.size();
					usernames.erase(
						std::remove_if(
							usernames.begin(),
							usernames.end(),
							[](const QString &username) {
								using Status = Data::SmsSendStatus;
								const auto record
									= Data::FindSmsSendRecord(username);
								return record
									&& (record->status != Status::SpamAcc)
									&& (record->status != Status::PeerLimit)
									&& (record->status
										!= Status::AccountFail);
							}),
						usernames.end());
					removed += before - usernames.size();
					Data::SaveScrapedGroupUsernames(
						meta.id, meta.title, usernames);
				}
				Ggtg::Notice(controller->uiShow(),
					u"Убрано из баз: "_q + QString::number(removed));
				RefreshRecipientsPreview(state);
			},
			.confirmText = u"Убрать"_q,
		}));
	});

	RefreshRecipientsPreview(state);
}

// ---- "ПОВЕДЕНИЕ ПРИ ОТПРАВКЕ": read by SendSmsTextMessage ----

// ---- "ТЕКСТ СООБЩЕНИЯ": template field + placeholder/spintax helpers ----

// Local equivalent of tg-web-panel's own sms-prompts editor: rename/edit
// a saved template's text, or create a brand new one (isNew, empty
// name/text) - same Save/Delete/Назад shape as ShowEditBaseBox in
// bulk_accounts_actions.cpp. "Использовать" (existing prompts only) is
// the one thing that box doesn't have: it copies this prompt's text
// straight into the still-open "Отправка SMS" box's own message field
// (`messageField` - safe even though several boxes are stacked on top of
// it by then, since that field belongs to a box further down the same
// stack, not to this one) and closes both this box and the picker above
// it in one step.
void ShowEditSmsPromptBox(
		not_null<Window::SessionController*> controller,
		QPointer<Ui::InputField> messageField,
		uint64 id,
		const QString &name,
		const QString &text,
		bool isNew) {
	struct State {
		QPointer<Ui::InputField> name;
		QPointer<Ui::InputField> text;
	};
	const auto state = std::make_shared<State>();

	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setWidth(st::accountsBoxWidth);
		box->setTitle(rpl::single(
			isNew ? u"Новый шаблон"_q : u"Шаблон сообщения"_q));
		state->name = box->addRow(object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			rpl::single(u"Название"_q),
			name));
		state->text = box->addRow(object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			Ui::InputField::Mode::MultiLine,
			rpl::single(QString()),
			text));
		box->setFocusCallback([=] { state->name->setFocusFast(); });
		if (!isNew) {
			box->addButton(rpl::single(u"Использовать"_q), [=] {
				if (const auto field = messageField.data()) {
					field->setText(state->text->getLastText());
				}
				box->closeBox();
			});
		}
		box->addButton(rpl::single(u"Сохранить"_q), [=] {
			const auto newName = state->name->getLastText().trimmed();
			if (newName.isEmpty()) {
				Ggtg::Notice(box->uiShow(), u"Введите название"_q);
				return;
			}
			Data::SaveSmsPrompt(
				isNew ? Data::GenerateSmsPromptId() : id,
				newName,
				state->text->getLastText());
			box->closeBox();
		});
		if (!isNew) {
			box->addButton(rpl::single(u"Удалить"_q), [=] {
				Data::DeleteSmsPrompt(id);
				box->closeBox();
			});
		}
		Ggtg::AddBackButton(box);
	}));
}

// Picker/manager for saved message templates - opened from "Выбрать
// шаблон..." below. Each row opens that prompt's own ShowEditSmsPromptBox
// rather than applying it directly, so a template can be reviewed (and
// tweaked) before it overwrites whatever's currently in the message
// field - "Использовать" there is the one-click apply.
object_ptr<Ui::BoxContent> MakeSmsPromptsBox(
		not_null<Window::SessionController*> controller,
		QPointer<Ui::InputField> messageField) {
	return Box([=](not_null<Ui::GenericBox*> box) {
		box->setWidth(st::accountsBoxWidth);
		box->setTitle(rpl::single(u"Шаблоны сообщений"_q));

		box->addRow(object_ptr<Ui::SettingsButton>(
			box, rpl::single(u"+ Новый шаблон"_q))
		)->setClickedCallback([=] {
			ShowEditSmsPromptBox(
				controller, messageField, 0, QString(), QString(), true);
		});

		const auto prompts = Data::LoadSmsPrompts();
		if (prompts.empty()) {
			box->addRow(object_ptr<Ui::FlatLabel>(
				box,
				u"Пока пусто - сохраните текущий текст кнопкой \"Сохранить "
					"как шаблон...\", или создайте новый выше."_q,
				st::boxLabel));
		}
		for (const auto &prompt : prompts) {
			const auto id = prompt.id;
			const auto name = prompt.name;
			const auto text = prompt.text;
			box->addRow(object_ptr<Ggtg::TextRow>(box, rpl::single(name))
			)->setClickedCallback([=] {
				ShowEditSmsPromptBox(
					controller, messageField, id, name, text, false);
			});
		}

		Ggtg::AddCloseCross(box);
	});
}

[[nodiscard]] QString RotationSummary(not_null<CampaignState*> state) {
	if (state->rotationPromptIds.empty()) {
		return QString();
	}
	auto names = QStringList();
	for (const auto &prompt : Data::LoadSmsPrompts()) {
		if (ranges::contains(state->rotationPromptIds, prompt.id)) {
			names.push_back(prompt.name);
		}
	}
	if (names.isEmpty()) {
		return QString();
	}
	return u"По очереди "_q + QString::number(names.size())
		+ u" шаблона: "_q + names.join(u", "_q)
		+ u". Текст в поле не используется."_q;
}

void UpdateRotationLabel(not_null<CampaignState*> state) {
	if (const auto label = state->rotationLabel.data()) {
		const auto text = RotationSummary(state);
		label->setText(text);
		label->setVisible(!text.isEmpty());
	}
	if (state->refreshPreflight) {
		state->refreshPreflight();
	}
}

// Which saved prompts a campaign cycles through, one per message in turn -
// the same account sending the same text to everyone is what gets a
// campaign noticed first.
void ShowRotationPicker(
		not_null<Window::SessionController*> controller,
		not_null<CampaignState*> state) {
	struct Item {
		uint64 id = 0;
		QPointer<Ui::Checkbox> checkbox;
	};
	const auto items = std::make_shared<std::vector<Item>>();
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setWidth(st::accountsBoxWidth);
		box->setTitle(rpl::single(u"Чередование шаблонов"_q));
		Ui::AddDividerText(
			box->verticalLayout(),
			rpl::single(
				u"Отмеченные шаблоны чередуются по кругу от сообщения к "
					"сообщению. Пока отмечен хотя бы один, текст в поле "
					"не используется; ни одного - обычная рассылка одним "
					"текстом."_q));
		const auto prompts = Data::LoadSmsPrompts();
		if (prompts.empty()) {
			box->addRow(object_ptr<Ui::FlatLabel>(
				box,
				u"Шаблонов пока нет - сохраните текст кнопкой \"Сохранить "
					"как шаблон...\"."_q,
				st::boxLabel));
		}
		for (const auto &prompt : prompts) {
			items->push_back({
				prompt.id,
				box->addRow(object_ptr<Ui::Checkbox>(
					box,
					prompt.name,
					ranges::contains(state->rotationPromptIds, prompt.id)), st::ggtgChoiceRowPadding),
			});
		}
		box->addButton(rpl::single(u"Готово"_q), [=] {
			state->rotationPromptIds.clear();
			for (const auto &item : *items) {
				if (item.checkbox && item.checkbox->checked()) {
					state->rotationPromptIds.push_back(item.id);
				}
			}
			UpdateRotationLabel(state);
			box->closeBox();
		});
		box->addButton(rpl::single(u"Отмена"_q), [=] { box->closeBox(); });
	}));
}

// "ИИ-помощник": a local model rewrites, shortens or translates the message
// template on request. The result is shown first and only goes into the
// message field when "Вставить" is pressed - nothing is replaced silently.
// Placeholders ([NAME], {a|b}, [SPLIT]...) are kept as they are.
void ShowAiTemplateAssistant(
		not_null<Window::SessionController*> controller,
		QPointer<Ui::InputField> target) {
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"ИИ-помощник"_q));
		box->setWidth(st::boxWideWidth);
		const auto settings = Core::GgtgAi::Load();
		if (!settings.enabled || !settings.templateAssistant) {
			box->addRow(object_ptr<Ui::FlatLabel>(
				box,
				u"ИИ-помощник выключен. Подключите модель и включите его в "
					"разделе «ИИ» окна GGTG."_q,
				st::boxLabel));
			box->addButton(rpl::single(u"Закрыть"_q), [=] {
				box->closeBox();
			});
			return;
		}
		struct Preset {
			QString label;
			QString instruction;
			QString translateTo;
			bool picked = false;
			Ui::RoundButton *plain = nullptr;
			Ui::RoundButton *active = nullptr;
		};
		struct State {
			QString result;
			bool busy = false;
			std::vector<Preset> presets;
			QPointer<Ui::InputField> instruction;
			QPointer<Ui::FlatLabel> output;
		};
		const auto state = box->lifetime().make_state<State>();
		const auto source = target ? target->getLastText() : QString();
		if (source.trimmed().isEmpty()) {
			box->addRow(object_ptr<Ui::FlatLabel>(
				box,
				u"Сначала впишите текст сообщения - помощник будет с ним "
					"работать."_q,
				st::boxLabel));
			box->addButton(rpl::single(u"Закрыть"_q), [=] {
				box->closeBox();
			});
			return;
		}

		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			u"Выберите одну или несколько команд, например «Короче» и «На "
				"английский». Перевод выбирается один."_q,
			st::boxDividerLabel));
		// The commands: a tap picks or unpicks one (the picked look is a
		// second, accent-coloured button over the plain one).
		auto presetsOwned = object_ptr<Ui::RpWidget>(box);
		const auto presets = presetsOwned.data();
		const auto addPreset = [=](
				const QString &label,
				const QString &instruction,
				const QString &translateTo = QString()) {
			state->presets.push_back({
				.label = label,
				.instruction = instruction,
				.translateTo = translateTo,
			});
		};
		addPreset(u"Улучшить"_q, u"Улучшить текст: естественнее и яснее"_q);
		addPreset(u"Короче"_q, u"Сделать короче, сохранив смысл"_q);
		addPreset(u"Вежливее"_q, u"Сделать вежливее и дружелюбнее"_q);
		addPreset(
			u"Варианты {a|b}"_q,
			u"Добавить к приветствию и концовке несколько вариантов в "
				"формате {вариант1|вариант2|вариант3}"_q);
		// Translation goes to the translator model, after everything else.
		for (const auto &language : Core::GgtgAi::Languages()) {
			if (ranges::contains(
					std::array{
						u"en"_q, u"ko"_q, u"zh"_q, u"ja"_q, u"ru"_q, u"es"_q,
					},
					language.code)) {
				addPreset(
					u"На "_q + language.russian,
					u"Перевести на "_q + language.russian,
					language.code);
			}
		}
		state->presets.front().picked = true;
		const auto refresh = [=] {
			for (const auto &preset : state->presets) {
				preset.active->setVisible(preset.picked);
				preset.plain->setVisible(!preset.picked);
			}
		};
		for (auto i = 0; i != int(state->presets.size()); ++i) {
			auto &preset = state->presets[i];
			const auto toggle = [=] {
				auto &chosen = state->presets[i];
				chosen.picked = !chosen.picked;
				if (chosen.picked && !chosen.translateTo.isEmpty()) {
					for (auto &other : state->presets) {
						if (&other != &chosen && !other.translateTo.isEmpty()) {
							other.picked = false;
						}
					}
				}
				refresh();
			};
			preset.plain = Ui::CreateChild<Ui::RoundButton>(
				presets,
				rpl::single(preset.label),
				st::ggtgTokenButton);
			preset.active = Ui::CreateChild<Ui::RoundButton>(
				presets,
				rpl::single(preset.label),
				st::ggtgTokenButtonActive);
			preset.plain->setClickedCallback(toggle);
			preset.active->setClickedCallback(toggle);
		}
		refresh();
		presets->widthValue(
		) | rpl::on_next([=](int width) {
			const auto gap = style::ConvertScale(6);
			auto x = 0;
			auto y = 0;
			auto lineHeight = 0;
			for (const auto &preset : state->presets) {
				const auto w = std::max(
					preset.plain->width(),
					preset.active->width());
				if (x > 0 && x + w > width) {
					x = 0;
					y += lineHeight + gap;
				}
				preset.plain->moveToLeft(x, y, width);
				preset.active->moveToLeft(x, y, width);
				x += w + gap;
				lineHeight = std::max(lineHeight, preset.plain->height());
			}
			presets->resize(width, y + lineHeight);
		}, presets->lifetime());
		box->addRow(
			std::move(presetsOwned),
			style::margins(
				st::boxRowPadding.left(),
				st::defaultVerticalListSkip,
				st::boxRowPadding.right(),
				st::defaultVerticalListSkip));

		state->instruction = box->addRow(object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			rpl::single(u"Своя команда (необязательно)"_q)));

		state->output = box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			u"Результат появится здесь."_q,
			st::boxLabel));
		state->output->setSelectable(true);

		const auto run = [=] {
			if (state->busy) {
				return;
			}
			auto instructions = QStringList();
			auto translateTo = QString();
			for (const auto &preset : state->presets) {
				if (!preset.picked) {
					continue;
				} else if (preset.translateTo.isEmpty()) {
					instructions.push_back(preset.instruction);
				} else {
					translateTo = preset.translateTo;
				}
			}
			const auto own = state->instruction
				? state->instruction->getLastText().trimmed()
				: QString();
			if (!own.isEmpty()) {
				instructions.push_back(own);
			}
			if (instructions.isEmpty() && translateTo.isEmpty()) {
				Ggtg::Notice(box->uiShow(), u"Выберите, что сделать с текстом"_q);
				return;
			}
			state->busy = true;
			if (const auto label = state->output.data()) {
				label->setText(u"Думаю..."_q);
			}
			const auto finished = crl::guard(box, [=](
					QString result,
					QString error) {
				state->busy = false;
				if (!error.isEmpty()) {
					if (const auto label = state->output.data()) {
						label->setText(u"Не вышло: "_q + error);
					}
					return;
				}
				state->result = result;
				if (const auto label = state->output.data()) {
					label->setText(result);
				}
			});
			// The edits first, then the translation of the edited text by the
			// translator model (words only, placeholders untouched).
			const auto translate = [=](QString text) {
				if (translateTo.isEmpty()) {
					finished(text, QString());
					return;
				}
				if (const auto label = state->output.data()) {
					label->setText(u"Перевожу..."_q);
				}
				Core::GgtgAi::TranslateTemplate(text, translateTo, finished);
			};
			if (instructions.isEmpty()) {
				translate(source);
				return;
			}
			Core::GgtgAi::Ask(
				u"Ты помощник по написанию коротких личных сообщений. "
				"Тебе дают шаблон сообщения и инструкции. Ты ОБЯЗАН "
				"реально выполнить ВСЕ инструкции: итоговый текст должен "
				"заметно отличаться от исходного формулировками. Верни "
				"ТОЛЬКО готовый шаблон: без пояснений, без кавычек, без "
				"комментариев и заголовков. Служебные части оставь "
				"символ в символ: [NAME], [SURNAME], [MYNAME], [MYSURNAME], "
				"[GROUP], [NAME|слово], [SPLIT] и скобки вариантов {a|b|c}. Менять "
				"можно только обычные слова вокруг них и сами слова внутри "
				"вариантов. Не добавляй ссылок, цен и обещаний, которых нет "
				"в исходном тексте."_q
				+ [] {
					const auto profile = Core::GgtgAi::Profile::Block(1500);
					return profile.isEmpty()
						? QString()
						: (u"\n\nО пользователе и его деле (его профиль; "
							"текст должен подходить под это дело, цель и "
							"аудиторию, соблюдай правила и запреты):\n"_q
							+ profile);
				}()
				+ [] {
					const auto insights = Core::GgtgSmsStats::LoadInsights();
					return insights.isEmpty()
						? QString()
						: (u"\n\nВыводы из статистики прошлых рассылок "
							"(учитывай, если подходят к инструкции):\n"_q
							+ insights.left(1500));
				}(),
				u"Инструкции:\n- "_q + instructions.join(u"\n- "_q)
					+ u"\n\nШаблон:\n"_q + source,
				crl::guard(box, [=](QString result, QString error) {
					if (!error.isEmpty() || result.trimmed().isEmpty()) {
						finished(
							QString(),
							error.isEmpty() ? u"пустой ответ"_q : error);
						return;
					}
					translate(result.trimmed());
				}),
				0.6,
				800,
				Core::GgtgAi::Role::General,
				QStringList(),
				true); // The first message people read: the cleverest model.
		};
		state->instruction->submits(
		) | rpl::on_next(run, state->instruction->lifetime());
		// Every button is always there: a hidden one still takes its place
		// in the row, which pushed "Закрыть" under the left-hand button.
		box->addButton(rpl::single(u"Выполнить"_q), run);
		box->addButton(rpl::single(u"Вставить"_q), [=] {
			if (state->result.isEmpty()) {
				Ggtg::Notice(box->uiShow(), u"Сначала нажмите «Выполнить»"_q);
				return;
			}
			if (const auto field = target.data()) {
				field->setText(state->result);
			}
			box->closeBox();
		});
		box->addButton(rpl::single(u"Закрыть"_q), [=] {
			box->closeBox();
		});
	}));
}

void SetupTextSection(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller,
		not_null<CampaignState*> state,
		not_null<Ui::VerticalLayout*> content) {
	// Rows go into the step card's own column, not straight into the box.
	const auto addRow = [=](
			auto widget,
			const style::margins &margins = st::boxRowPadding) {
		return content->add(std::move(widget), margins);
	};
	state->messageField = addRow(object_ptr<Ui::InputField>(
		box,
		st::ggtgCardInputField,
		Ui::InputField::Mode::MultiLine,
		rpl::single(u"Текст, например: {Привет|Здравствуйте}, [NAME]!"_q),
		state->saved.saved ? state->saved.text : QString()));

	// What one message would look like (sample names, one random pick from
	// each {a|b} group) plus the static checks - a broken template is far
	// cheaper to notice here than after a campaign has sent it to hundreds.
	state->messagePreview = addRow(
		object_ptr<Ui::FlatLabel>(box, QString(), st::boxDividerLabel),
		style::margins(
			st::boxRowPadding.left(),
			st::defaultVerticalListSkip,
			st::boxRowPadding.right(),
			0));
	state->messageWarning = addRow(
		object_ptr<Ui::FlatLabel>(box, QString(), st::boxDividerLabel),
		style::margins(
			st::boxRowPadding.left(),
			st::defaultVerticalListSkip / 2,
			st::boxRowPadding.right(),
			0));
	const auto refreshPreview = [=] {
		const auto field = state->messageField.data();
		if (!field) {
			return;
		}
		const auto text = field->getLastText();
		const auto sample = RenderSmsTemplateText(
			text,
			u"Иван"_q,
			u"Петров"_q,
			u"Анна"_q,
			u"Смирнова"_q,
			u"Название группы"_q);
		const auto check = CheckSmsTemplate(text, int(sample.size()));
		if (const auto label = state->messagePreview.data()) {
			// "[SPLIT]" -> each resulting message on its own numbered line,
			// so it is clear how many separate messages will go out.
			const auto parts = SplitRenderedMessage(sample);
			auto preview = QString();
			if (parts.size() > 1) {
				for (auto i = 0; i != int(parts.size()); ++i) {
					preview += u"\n"_q + QString::number(i + 1) + u". "_q
						+ parts[i];
				}
			} else {
				preview = u" «"_q + sample + u"»"_q;
			}
			label->setText(u"Пример для Ивана Петрова:"_q + preview);
			label->setVisible(!text.trimmed().isEmpty());
		}
		if (const auto label = state->messageWarning.data()) {
			label->setText(!check.error.isEmpty()
				? check.error
				: check.warning);
			label->setTextColorOverride(check.error.isEmpty()
				? std::optional<QColor>(st::windowSubTextFg->c)
				: std::optional<QColor>(st::boxTextFgError->c));
			label->setVisible(!check.error.isEmpty()
				|| !check.warning.isEmpty());
		}
		if (state->refreshPreflight) {
			state->refreshPreflight();
		}
	};
	state->messageField->changes(
	) | rpl::on_next(refreshPreview, state->messageField->lifetime());
	refreshPreview();
	Ui::AddSkip(content);

	// Placeholders, saved texts and the AI as three quiet links instead of
	// seven token buttons and two full-width rows.
	const auto links = addRow(object_ptr<SmsWidgets::LinksRow>(box));
	const auto insertMenu = box->lifetime().make_state<
		base::unique_qptr<Ui::PopupMenu>>();
	links->add(u"Вставить ▾"_q, [=] {
		*insertMenu = base::make_unique_q<Ui::PopupMenu>(
			box,
			st::popupMenuWithIcons);
		const auto add = [=](const QString &title, const QString &token) {
			(*insertMenu)->addAction(title, [=] {
				if (const auto field = state->messageField.data()) {
					field->setFocus();
					field->insertTag(token);
				}
			});
		};
		add(u"[NAME] - имя получателя"_q, u"[NAME]"_q);
		add(u"[NAME|друг] - имя или запасное слово"_q, u"[NAME|друг]"_q);
		add(u"[SURNAME] - фамилия получателя"_q, u"[SURNAME]"_q);
		add(u"[MYNAME] - ваше имя"_q, u"[MYNAME]"_q);
		add(u"[MYSURNAME] - ваша фамилия"_q, u"[MYSURNAME]"_q);
		add(u"[GROUP] - группа, где нашли получателя"_q, u"[GROUP]"_q);
		add(u"{a|b} - случайный вариант"_q, u"{|}"_q);
		add(u"[SPLIT] - следующее сообщение"_q, u"[SPLIT]"_q);
		(*insertMenu)->popup(QCursor::pos());
	});
	const auto templatesMenu = box->lifetime().make_state<
		base::unique_qptr<Ui::PopupMenu>>();
	links->add(u"Шаблоны ▾"_q, [=] {
		*templatesMenu = base::make_unique_q<Ui::PopupMenu>(
			box,
			st::popupMenuWithIcons);
		(*templatesMenu)->addAction(u"Выбрать шаблон"_q, [=] {
			controller->show(
				MakeSmsPromptsBox(controller, state->messageField));
		}, &st::menuIconSelect);
		(*templatesMenu)->addAction(u"Сохранить текст как шаблон"_q, [=] {
			const auto text = state->messageField
				? state->messageField->getLastText()
				: QString();
			if (text.trimmed().isEmpty()) {
				Ggtg::Notice(controller->uiShow(),
					u"Текст пуст - нечего сохранять"_q);
				return;
			}
			ShowEditSmsPromptBox(
				controller, state->messageField, 0, QString(), text, true);
		}, &st::menuIconDownload);
		(*templatesMenu)->addAction(
			u"Чередовать несколько шаблонов"_q,
			[=] { ShowRotationPicker(controller, state); },
			&st::menuIconRestore);
		(*templatesMenu)->popup(QCursor::pos());
	});
	links->add(u"ИИ-помощник"_q, [=] {
		ShowAiTemplateAssistant(controller, state->messageField);
	});
	// Shown only while several saved texts take turns instead of the field.
	state->rotationLabel = addRow(
		object_ptr<Ui::FlatLabel>(box, QString(), st::boxDividerLabel),
		style::margins(
			st::boxRowPadding.left(),
			st::defaultVerticalListSkip,
			st::boxRowPadding.right(),
			0));
	UpdateRotationLabel(state);
	Ui::AddSkip(content);

	// "Лёгкий прогрев" is no longer an option - it is always on.
	state->greetingStickerCheckbox = AddRowWithInfo(
		box,
		content,
		object_ptr<Ui::Checkbox>(
			box,
			u"Сначала приветственный стикер"_q,
			state->saved.saved && state->saved.greetingSticker,
			st::defaultCheckbox,
			st::defaultToggle),
		u"Если текст пустой, уйдёт только стикер; если текст есть, "
			"сначала стикер, а через 10-15 секунд текст."_q);
	state->greetingStickerCheckbox->checkedChanges(
	) | rpl::on_next([=] {
		if (state->refreshPreflight) {
			state->refreshPreflight();
		}
	}, state->greetingStickerCheckbox->lifetime());
	Ui::AddSkip(content);

	// Each recipient gets the template in their own language (Free Google Translate).
	const auto aiSettings = Core::GgtgAi::Load();
	const auto translateCheckbox = AddRowWithInfo(
		box,
		content,
		object_ptr<Ui::Checkbox>(
			box,
			u"Переводить сообщения (бесплатный переводчик)"_q,
			aiSettings.smsTranslate,
			st::defaultCheckbox,
			st::defaultToggle),
		u"Бесплатный перевод шаблона на язык получателя через Google Translate: "
			"корейцу по-корейски, китайцу по-китайски. Работает быстро, надёжно и "
			"без расхода ключей ИИ. [NAME], [GROUP], [SPLIT] и {a|b} сохраняются без изменений."_q);

	struct LangOption {
		QString code;
		QString title;
	};
	const auto langOptions = std::vector<LangOption>{
		{ u"auto"_q, u"Авто (по языку получателя / базы)"_q },
		{ u"ko"_q, u"Корейский (한국어)"_q },
		{ u"en"_q, u"Английский (English)"_q },
		{ u"zh-CN"_q, u"Китайский (中文)"_q },
		{ u"ru"_q, u"Русский (Русский)"_q },
		{ u"es"_q, u"Испанский (Español)"_q },
		{ u"tr"_q, u"Турецкий (Türkçe)"_q },
		{ u"de"_q, u"Немецкий (Deutsch)"_q },
		{ u"fr"_q, u"Французский (Français)"_q },
		{ u"ja"_q, u"Японский (日本語)"_q },
		{ u"ar"_q, u"Арабский (العربية)"_q },
	};

	const auto initialLang = aiSettings.smsTranslateLang.isEmpty() ? u"auto"_q : aiSettings.smsTranslateLang;
	auto initialLangIdx = 0;
	for (size_t i = 0; i < langOptions.size(); ++i) {
		if (langOptions[i].code == initialLang) {
			initialLangIdx = int(i);
			break;
		}
	}

	const auto langWrap = content->add(
		object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			content,
			object_ptr<Ui::VerticalLayout>(content)));
	const auto langInner = langWrap->entity();

	langInner->add(
		object_ptr<Ui::FlatLabel>(
			langInner,
			rpl::single(u"Язык перевода сообщений:"_q),
			st::boxDividerLabel),
		style::margins(style::ConvertScale(24), style::ConvertScale(4), style::ConvertScale(24), style::ConvertScale(2)));

	const auto langGroup = std::make_shared<Ui::RadiobuttonGroup>(initialLangIdx);
	for (size_t i = 0; i < langOptions.size(); ++i) {
		langInner->add(
			object_ptr<Ui::Radiobutton>(
				langInner,
				langGroup,
				int(i),
				langOptions[i].title,
				st::defaultCheckbox),
			style::margins(style::ConvertScale(24), style::ConvertScale(2), style::ConvertScale(24), style::ConvertScale(2)));
	}

	langWrap->toggle(aiSettings.smsTranslate, anim::type::instant);

	langGroup->setChangedCallback([=](int val) {
		if (val >= 0 && val < int(langOptions.size())) {
			auto s = Core::GgtgAi::Load();
			s.smsTranslateLang = langOptions[val].code;
			Core::GgtgAi::Save(s);
		}
	});

	translateCheckbox->checkedChanges(
	) | rpl::on_next([=](bool checked) {
		auto settings = Core::GgtgAi::Load();
		settings.smsTranslate = checked;
		Core::GgtgAi::Save(settings);
		langWrap->toggle(checked, anim::type::normal);
	}, translateCheckbox->lifetime());
	Ui::AddSkip(content);
}

// ---- 4. "ИИ-ЗАЩИТА АККАУНТОВ": 3 режима, ночной сон по гео, эмуляция опечаток ----

void SetupAiProtectionSection(
		not_null<Ui::GenericBox*> box,
		not_null<CampaignState*> state,
		not_null<Ui::VerticalLayout*> content) {
	const auto initialMode = state->saved.saved
		? std::clamp(state->saved.protectionMode, 0, 2)
		: 1;
	state->protectionMode = initialMode;
	state->protectionGroup = std::make_shared<Ui::RadiobuttonGroup>(initialMode);

	struct ProtectionOption {
		int mode = 0;
		QString title;
		QString desc;
	};
	const auto options = std::vector<ProtectionOption>{
		{
			0,
			u"Консервативный"_q,
			u"Задержки x1.5 · Просмотр анкет 90% · Опечатки 8% · Ночной сон 01:00–07:00"_q,
		},
		{
			1,
			u"Оптимальный"_q,
			u"Задержки x1.0 · Просмотр анкет 70% · Опечатки 5% · Ночной сон 02:00–06:00"_q,
		},
		{
			2,
			u"Скоростной"_q,
			u"Задержки x0.7 · Просмотр анкет 30% · Опечатки 2% · Без ночного сна"_q,
		},
	};

	for (const auto &opt : options) {
		content->add(
			object_ptr<Ui::Radiobutton>(
				content,
				state->protectionGroup,
				opt.mode,
				opt.title,
				st::defaultCheckbox),
			st::ggtgChoiceRowPadding);
		content->add(
			object_ptr<Ui::FlatLabel>(
				content,
				rpl::single(opt.desc),
				st::boxDividerLabel),
			style::margins(
				style::ConvertScale(48),
				0,
				style::ConvertScale(24),
				style::ConvertScale(8)));
	}

	const auto refreshSummary = [=] {
		auto parts = QStringList();
		switch (state->protectionMode) {
		case 0:
			parts.push_back(u"Консервативный (x1.5)"_q);
			break;
		case 2:
			parts.push_back(u"Скоростной (x0.7)"_q);
			break;
		case 1:
		default:
			parts.push_back(u"Оптимальный (x1.0)"_q);
			break;
		}
		if (state->nightSleepCheckbox && state->nightSleepCheckbox->checked()) {
			parts.push_back(state->protectionMode == 0
				? u"сон 01–07"_q
				: state->protectionMode == 1
				? u"сон 02–06"_q
				: u"сон по гео"_q);
		} else {
			parts.push_back(u"без сна"_q);
		}
		if (state->simulateTyposCheckbox && state->simulateTyposCheckbox->checked()) {
			parts.push_back(u"опечатки"_q);
		}
		if (state->filterByGgrCheckbox && state->filterByGgrCheckbox->checked()) {
			parts.push_back(u"GGR ≥ 7.0"_q);
		}
		state->protectionSummary = parts.join(u" · "_q);
		if (state->refreshPreflight) {
			state->refreshPreflight();
		}
	};

	state->protectionGroup->setChangedCallback([=](int val) {
		state->protectionMode = std::clamp(val, 0, 2);
		refreshSummary();
	});

	Ui::AddSkip(content);

	const auto initialSleep = state->saved.saved ? state->saved.nightSleep : true;
	state->nightSleepCheckbox = AddRowWithInfo(
		box,
		content,
		object_ptr<Ui::Checkbox>(
			box,
			u"Ночной сон по гео-таймзоне"_q,
			initialSleep,
			st::defaultCheckbox,
			st::defaultToggle),
		u"Если включено, аккаунт не отправляет сообщения в ночные часы "
			"по местному часовому поясу своего прокси или телефонного номера. "
			"Это предотвращает подозрительную ночную активность и баны антифрода."_q);
	state->nightSleepCheckbox->checkedChanges(
	) | rpl::on_next([=] {
		refreshSummary();
	}, state->nightSleepCheckbox->lifetime());

	Ui::AddSkip(content);

	const auto initialTypos = state->saved.saved ? state->saved.simulateTypos : true;
	state->simulateTyposCheckbox = AddRowWithInfo(
		box,
		content,
		object_ptr<Ui::Checkbox>(
			box,
			u"Эмуляция опечаток и правок"_q,
			initialTypos,
			st::defaultCheckbox,
			st::defaultToggle),
		u"В процессе набора сообщения с вероятностью выбранного режима имитирует "
			"ошибку, стирание символов через Backspace и повторный набор, "
			"создавая естественные паузы и статус «печатает…» у получателя."_q);
	state->simulateTyposCheckbox->checkedChanges(
	) | rpl::on_next([=] {
		refreshSummary();
	}, state->simulateTyposCheckbox->lifetime());

	Ui::AddSkip(content);

	const auto initialFilterGgr = state->saved.saved ? state->saved.filterByGgr : false;
	state->filterByGgrCheckbox = AddRowWithInfo(
		box,
		content,
		object_ptr<Ui::Checkbox>(
			box,
			u"Только аккаунты с высоким трастом (GGR ≥ 7.0)"_q,
			initialFilterGgr,
			st::defaultCheckbox,
			st::defaultToggle),
		u"Автоматически отсеивает молодые сессии и аккаунты с риском блокировки. "
			"Рейтинг GGR (1.0–10.0) рассчитывается по возрасту User ID, статусу @SpamBot, "
			"наличию персонального прокси и истории прогрева."_q);
	state->filterByGgrCheckbox->checkedChanges(
	) | rpl::on_next([=] {
		refreshSummary();
	}, state->filterByGgrCheckbox->lifetime());

	Ui::AddSkip(content);

	const auto initialSkipFakes = state->saved.saved ? state->saved.skipSuspectedFakes : true;
	state->skipSuspectedFakesCheckbox = AddRowWithInfo(
		box,
		content,
		object_ptr<Ui::Checkbox>(
			box,
			u"Не писать предположительным фейкам (проверка профиля)"_q,
			initialSkipFakes,
			st::defaultCheckbox,
			st::defaultToggle),
		u"Перед отправкой сообщения анализирует профиль, дату и интервалы добавления "
			"фотографий и ключевые слова. При обнаружении бота аккаунт пропускается и помечается "
			"в базе для последующей ручной проверки."_q);
	state->skipSuspectedFakesCheckbox->checkedChanges(
	) | rpl::on_next([=] {
		refreshSummary();
	}, state->skipSuspectedFakesCheckbox->lifetime());

	Ui::AddSkip(content);
	refreshSummary();
}

// ---- the progress window's own widgets: a card per lane and the summary ----

// What one lane shows: the account it works with, and either the steps it
// has taken with the current recipient (the last one is what it does right
// now) or, between recipients, a single status line ("пауза 42 сек...").
// A lane step that has ended: "✓ отправил текст", "✗ текст НЕ ушёл: ...".
// Any other step is what is going on right now.
[[nodiscard]] bool IsSettledStep(const QString &step) {
	return step.startsWith(u""_q) || step.startsWith(u""_q);
}

// ---- persistent campaign state: survives "Закрыть", supports reopen ----

// Everything the send loop needs to keep going, and everything a
// reopened progress box needs to redraw itself identically - stored
// outside any box so "Закрыть" (which only hides the current layer, see
// sms_campaign.h) never loses a running or just-finished campaign. Only
// one campaign can exist at a time (see CurrentCampaign() below).
struct CampaignRunState {
	std::vector<SmsModel::Recipient> recipients;
	// Indices of recipients worked on, newest first: what the list in the
	// progress window shows. Capped at kMaxOrder; older ones still count in
	// `counts` and stay in the "Процессы" log.
	std::vector<int> order;
	base::flat_map<QString, int> indexOf; // lowercase username -> index
	SmsModel::Counts counts{};
	std::vector<SmsModel::Account> accountViews;
	base::flat_map<Main::Account*, int> accountIndex;
	// What each account did, line by line with the time (the same index as
	// accountViews): opened with a click on the account's row.
	std::vector<QStringList> accountLog;
	// What each lane is doing right now, shown on its account's row.
	std::vector<QString> laneLive;
	// Seconds a lane spent per recipient lately: the time estimate.
	std::vector<int> recentDurations;
	std::vector<crl::time> laneLastSettle;
	int lanes = 1;
	int goal = 0;
	QString summaryText; // for "Процессы" when the run ends
	rpl::variable<SmsModel::Filter> filter = SmsModel::Filter::All;
	// First indices of the folded groups the user opened.
	base::flat_set<int> expanded;
	// Set by the open progress window, empty while it is closed.
	Fn<void()> refreshUi;
	Fn<void()> refreshAccounts;

	// Every recipient of this campaign, in order - kept so the finished
	// progress box can turn whoever never got through into a new base.
	QStringList usernames;

	rpl::variable<QString> buttonText
		= rpl::variable<QString>(u"Стоп"_q);
	std::shared_ptr<bool> cancelled = std::make_shared<bool>(false);
	bool finished = false;

	// "Пауза"/"Продолжить": lanes finish whatever send is already in
	// flight, then hold at the top of their next pass until resumed.
	bool paused = false;
	rpl::variable<QString> pauseText
		= rpl::variable<QString>(u"Пауза"_q);
	// False once the campaign has finished or been stopped - hides the
	// pause button, which has nothing left to pause.
	rpl::variable<bool> running = rpl::variable<bool>(true);
	// "Стоп" was pressed (as opposed to running out of recipients or
	// accounts on its own).
	bool stopped = false;
	// Recipients that never got an attempt - stopped, or every account was
	// retired first. Only counted (they stay in the base untouched, with no
	// mark), filled in by `finalize`.
	int unprocessed = 0;
	Fn<void()> finalize;

	[[nodiscard]] SmsModel::Phase phase() const {
		if (!finished) {
			return paused ? SmsModel::Phase::Paused : SmsModel::Phase::Running;
		} else if (stopped) {
			return SmsModel::Phase::Stopped;
		}
		return (unprocessed > 0)
			? SmsModel::Phase::AccountsOut
			: SmsModel::Phase::Done;
	}

	std::vector<int> pendingIndices;
	int nextPending = 0;
	// Sends currently awaiting an answer.
	int inFlight = 0;
	// Which template the next send uses (round-robin across the campaign).
	int templateIndex = 0;
	// Per lane, for its current account, over the whole campaign (never
	// reset by a success): sends, Telegram's refusals, failures, failed
	// lookups - see SmsModel::AfterStep.
	std::vector<SmsModel::LaneCounters> laneCounters;
	// The recipients Telegram refused this account (name, recipient index):
	// marked with the account's removal if it comes to that.
	std::vector<std::vector<std::pair<QString, int>>> laneRefusals;
	// Usernames a lane's account failed to resolve, not yet written to the
	// registry: they're only recorded as NotFound once that same account
	// gets a real send through afterwards (proof its lookups weren't just
	// throttled) - dropped if the account is retired first.
	std::vector<std::vector<QString>> laneNotFoundPending;
	int nextAccount = 0;
	std::vector<Main::Account*> laneAccount;
	// The abort flag of the step each lane has in flight (see
	// SmsStepAbort): "Стоп" raises them all, so nothing new goes out.
	std::vector<std::shared_ptr<bool>> laneAbort;
	std::vector<bool> laneFinished;
	std::vector<std::shared_ptr<Fn<void()>>> laneRunners;

	// Settled before the run started - see SmsSummaryData::earlier.
	int earlier = 0;
	base::flat_set<not_null<Main::Account*>> spamChecked;
	// Recipients Telegram refused while their account looked free: if the
	// @spambot check then says the account IS blocked, they are rewritten
	// as the account's fault (see PEER_FLOOD handling in RunSmsCampaign).
	base::flat_map<
		Main::Account*,
		std::vector<std::pair<QString, uint64>>> peerLimited;

	// Mirrors this campaign into the shared "Процессы" list - see
	// core/process_manager.h. Left null-safe (guarded at every call site)
	// rather than required, so a campaign started before this wiring
	// existed (impossible in practice, but matches the style of the rest
	// of this struct) would still run fine without it.
	std::shared_ptr<Core::ProcessHandle> process;
	int protectionMode = 1;
	bool nightSleep = true;
	bool simulateTypos = true;
	bool filterByGgr = false;
};

constexpr auto kMaxOrder = 1000;
constexpr auto kMaxRecipientRows = 200;
constexpr auto kRecentDurations = 20;

// Recipient `i` is being worked on: it goes to the top of the list.
void PushToOrder(not_null<CampaignRunState*> state, int i) {
	auto &order = state->order;
	order.erase(ranges::remove(order, i), order.end());
	order.insert(order.begin(), i);
	if (int(order.size()) > kMaxOrder) {
		order.resize(kMaxOrder);
	}
}

[[nodiscard]] rpl::variable<SmsCampaignIndicator> &IndicatorState() {
	static auto result = rpl::variable<SmsCampaignIndicator>(
		SmsCampaignIndicator::None);
	return result;
}

// Backs SmsCampaignProgressValue() - see RunSmsCampaign's updateSummary,
// the only place that assigns into this.
[[nodiscard]] rpl::variable<QString> &ProgressState() {
	static auto result = rpl::variable<QString>(QString());
	return result;
}

[[nodiscard]] std::shared_ptr<CampaignRunState> &CurrentCampaign() {
	static auto result = std::shared_ptr<CampaignRunState>();
	return result;
}

void FinishCampaign(not_null<CampaignRunState*> state) {
	if (state->finished) {
		return;
	}
	state->finished = true;
	state->running = false;
	if (state->refreshUi) {
		state->refreshUi();
	}
	// Bases are kept current as messages go out (see Data::MarkSmsSent);
	// make sure the last of those changes is on disk.
	Data::FlushBasePrunes();
	Core::GgtgSync::SyncSoon();
	if (state->finalize) {
		state->finalize();
	}
	// Everyone was dealt with (not stopped by hand, not cut short by the
	// accounts running out): nothing left to resume.
	if (!state->stopped && state->unprocessed == 0) {
		Data::ClearSmsJob();
	}
	state->buttonText = u"Готово"_q;
	IndicatorState() = SmsCampaignIndicator::Done;
	if (state->process) {
		state->process->succeed(
			SmsModel::PhaseTitle(state->phase()) + u". "_q
				+ state->summaryText);
	}
}

void MarkLaneFinished(not_null<CampaignRunState*> state, int lane) {
	if (state->laneFinished[lane]) {
		return;
	}
	state->laneFinished[lane] = true;
	if (ranges::all_of(state->laneFinished, [](bool v) { return v; })) {
		FinishCampaign(state);
	}
}

[[nodiscard]] object_ptr<Ui::BoxContent> MakeSmsCampaignSetupBox(
	not_null<Window::SessionController*> controller,
	uint64 presetBaseId = 0,
	bool withBack = false);

// Builds (or rebuilds, on reopen) the live progress box around an
// existing CampaignRunState - every row/lane/summary label is
// initialized straight from the state's already-current text/color
// instead of a fresh "в очереди" placeholder, so hiding and reopening
// the box looks exactly like it never went away.
void BuildCampaignProgressBox(
		std::shared_ptr<Ui::Show> show,
		std::shared_ptr<CampaignRunState> state) {
	show->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setWidth(st::accountsBoxWidth);
		box->setTitle(rpl::single(u"Рассылка"_q));
		Ggtg::AddProcessHideButton(box, [=](std::shared_ptr<Ui::Show> to) {
			BuildCampaignProgressBox(std::move(to), state);
		});
		const auto layout = box->verticalLayout();
		Ui::AddSkip(layout);
		const auto status = box->addRow(
			object_ptr<SmsWidgets::StatusCard>(box));
		Ui::AddSkip(layout);
		const auto tiles = box->addRow(
			object_ptr<SmsWidgets::OutcomeTiles>(box, true));

		// "Аккаунты · 23 выбрано · 2 в работе ..." - rewritten by refresh.
		const auto headline
			= box->lifetime().make_state<rpl::variable<QString>>();
		Ui::AddSubsectionTitle(layout, headline->value());
		const auto accountsWrap = box->addRow(
			object_ptr<Ui::VerticalLayout>(box),
			style::margins(
				st::boxRowPadding.left() - style::ConvertScale(12),
				0,
				st::boxRowPadding.right() - style::ConvertScale(12),
				0));
		const auto showAllAccounts = std::make_shared<bool>(false);

		Ui::AddSubsectionTitle(layout, rpl::single(u"Получатели"_q));
		const auto chips = box->addRow(
			object_ptr<SmsWidgets::FilterChips>(box));
		Ui::AddSkip(layout, style::ConvertScale(6));
		// No scroll area of its own: the box already scrolls, and a small
		// nested one showed only a few rows.
		const auto rowsArea = box->addRow(
			object_ptr<Ui::VerticalLayout>(box),
			style::margins(
				st::boxRowPadding.left() - style::ConvertScale(10),
				0,
				st::boxRowPadding.right() - style::ConvertScale(10),
				0));
		const auto emptyRows = box->addRow(
			object_ptr<Ui::FlatLabel>(
				box,
				u"Пока никого: первые получатели появятся здесь."_q,
				st::boxDividerLabel));

		// Rebuilding the list shrinks the box for a moment and the scroll
		// jumps to the top: after a click the recipients are brought back
		// into view (the list first, then its filter chips above it).
		const auto revealRecipients = [=] {
			Ui::PostponeCall(box, [=] {
				box->scrollToWidget(rowsArea);
				box->scrollToWidget(chips);
			});
		};

		// The recipients and accounts are rebuilt on every event of a running
		// campaign: clearing them shrinks the box for a moment and the scroll
		// would jump to the top. The position is put back after the rebuild
		// (and once more when the layout has settled).
		const auto keepScroll = [=](const auto &work) {
			const auto top = box->scrollTop();
			work();
			box->scrollToY(top);
			Ui::PostponeCall(box, [=] {
				box->scrollToY(top);
			});
		};

		// The whole log of one account: what it did, line by line, newest
		// first, with the result and the errors. It follows the campaign
		// while it is open.
		const auto showAccountLog = [=](int index) {
			box->uiShow()->show(Box([=](not_null<Ui::GenericBox*> logBox) {
				logBox->setTitle(rpl::single(
					state->accountViews[index].label));
				logBox->setWidth(st::boxWideWidth);
				const auto summary = logBox->addRow(
					object_ptr<Ui::FlatLabel>(
						logBox,
						QString(),
						st::boxLabel));
				const auto text = logBox->addRow(
					object_ptr<Ui::FlatLabel>(
						logBox,
						QString(),
						st::boxLabel),
					style::margins(
						st::boxRowPadding.left(),
						st::defaultVerticalListSkip,
						st::boxRowPadding.right(),
						st::defaultVerticalListSkip));
				text->setSelectable(true);
				const auto lines = [=] {
					return (state->accountLog.size() > std::size_t(index))
						? state->accountLog[index]
						: QStringList();
				};
				const auto fill = [=] {
					const auto &view = state->accountViews[index];
					summary->setText(SmsModel::StateTitle(view.state)
						+ u" · "_q + SmsModel::AccountCounters(view)
						+ (view.info.isEmpty()
							? QString()
							: (u"\n"_q + view.info))
						+ (view.note.isEmpty()
							? QString()
							: (u"\n"_q + view.note)));
					auto newest = lines();
					std::reverse(newest.begin(), newest.end());
					text->setText(newest.isEmpty()
						? u"Пока ничего не происходило: аккаунт ещё не "
							"начал работу."_q
						: newest.mid(0, 300).join(u"\n"_q));
				};
				fill();
				const auto timer = Ui::CreateChild<QTimer>(logBox.get());
				timer->setInterval(1000);
				QObject::connect(timer, &QTimer::timeout, logBox, fill);
				timer->start();
				logBox->addButton(
					rpl::single(u"Закрыть"_q),
					[=] { logBox->closeBox(); });
				logBox->addLeftButton(
					rpl::single(u"Копировать"_q),
					[=] {
						QGuiApplication::clipboard()->setText(
							lines().join(u"\n"_q));
						Ggtg::Notice(
							logBox->uiShow(),
							u"Журнал скопирован"_q);
					});
			}));
		};

		const auto rebuildAccountsRaw = [=] {
			accountsWrap->clear();
			const auto order = SmsModel::SortedAccounts(state->accountViews);
			const auto limit = *showAllAccounts
				? int(order.size())
				: std::min(int(order.size()), 8);
			for (auto k = 0; k != limit; ++k) {
				const auto index = order[k];
				auto live = QString();
				const auto laneCount = std::min(
					int(state->laneAccount.size()),
					int(state->laneLive.size()));
				for (auto lane = 0; lane != laneCount; ++lane) {
					const auto account = state->laneAccount[lane];
					const auto it = account
						? state->accountIndex.find(account)
						: state->accountIndex.end();
					if (it != state->accountIndex.end()
						&& it->second == index) {
						live = state->laneLive[lane];
					}
				}
				const auto row = accountsWrap->add(
					object_ptr<SmsWidgets::AccountRow>(accountsWrap));
				row->setData(state->accountViews[index], live);
				row->setClickedCallback([=] {
					showAccountLog(index);
				});
				Ggtg::SetWidgetContextMenu(row, [=] {
					auto entries = std::vector<Ggtg::MenuEntry>();
					entries.push_back({
						u"Журнал действий"_q,
						[=] { showAccountLog(index); },
						&st::menuIconShowInChat,
					});
					entries.push_back({
						u"Копировать название"_q,
						[=] {
							QGuiApplication::clipboard()->setText(
								state->accountViews[index].label);
							box->uiShow()->showToast(u"Скопировано"_q);
						},
						&st::menuIconCopy,
					});
					return entries;
				});
			}
			if (limit < int(order.size())) {
				const auto more = accountsWrap->add(
					object_ptr<Ui::LinkButton>(
						accountsWrap,
						u"ещё "_q + QString::number(order.size() - limit)
							+ u" аккаунтов"_q),
					style::margins(style::ConvertScale(12), 4, 0, 8));
				more->setClickedCallback([=] {
					*showAllAccounts = true;
					// Rebuilding deletes this very button: after the click.
					Ui::PostponeCall(box, [=] {
						if (state->refreshAccounts) {
							state->refreshAccounts();
						}
					});
				});
			}
			accountsWrap->resizeToWidth(accountsWrap->width());
		};

		const auto rebuildAccounts = [=] { keepScroll(rebuildAccountsRaw); };

		const auto rebuildRowsRaw = [=] {
			rowsArea->clear();
			const auto rows = SmsModel::Fold(
				state->recipients,
				state->order,
				state->filter.current(),
				kMaxRecipientRows);
			for (const auto &row : rows) {
				const auto first = row.indices.front();
				const auto expanded = state->expanded.contains(first);
				const auto widget = rowsArea->add(
					object_ptr<SmsWidgets::RecipientRow>(rowsArea));
				widget->setData(row, state->recipients, expanded);
				if (row.indices.size() < 2) {
					continue;
				}
				widget->setClickedCallback([=] {
					if (!state->expanded.remove(first)) {
						state->expanded.emplace(first);
					}
					// Rebuilding deletes this very row: after the click.
					Ui::PostponeCall(box, [=] {
						if (state->refreshUi) {
							state->refreshUi();
						}
						revealRecipients();
					});
				});
				if (!expanded) {
					continue;
				}
				for (const auto index : row.indices) {
					const auto single = rowsArea->add(
						object_ptr<SmsWidgets::RecipientRow>(rowsArea),
						style::margins(style::ConvertScale(20), 0, 0, 0));
					single->setData(
						SmsModel::DisplayRow{
							.indices = { index },
							.outcome = row.outcome,
							.account = row.account,
						},
						state->recipients,
						false);
				}
			}
			emptyRows->setVisible(rows.empty());
			rowsArea->resizeToWidth(rowsArea->width());
		};

		const auto rebuildRows = [=] { keepScroll(rebuildRowsRaw); };

		const auto refresh = [=] {
			// "2 / 2" is sent out of the goal (which counts sends too):
			// skipped or refused recipients don't move it.
			const auto processed = state->counts[int(SmsModel::Outcome::Sent)];
			const auto phase = state->phase();
			auto subtitle = QString::number(state->lanes)
				+ (state->lanes == 1 ? u" поток"_q : u" потока"_q);
			const auto eta = SmsModel::EstimateSeconds(
				state->recentDurations,
				std::max(state->goal - processed, 0),
				state->lanes);
			if (phase == SmsModel::Phase::Running && eta > 0) {
				subtitle += u" · осталось "_q + SmsModel::DurationText(eta);
			}
			status->setData({
				.title = SmsModel::PhaseTitle(phase),
				.subtitle = subtitle,
				.processed = processed,
				.goal = state->goal,
				.counts = state->counts,
				.finishLine = state->finished
					? SmsModel::FinishLine(state->counts)
					: QString(),
				.earlierLine = state->earlier
					? (u"Пропущены заранее (уже писали, боты): "_q
						+ QString::number(state->earlier))
					: QString(),
			});
			tiles->setCounts(state->counts);
			chips->setCounts(state->counts);
			*headline = SmsModel::AccountsHeadline(state->accountViews);
			rebuildAccounts();
			rebuildRows();
		};
		state->refreshUi = refresh;
		state->refreshAccounts = rebuildAccounts;
		// Closed (or hidden): the run goes on, the window is rebuilt from
		// `state` the next time it opens.
		box->lifetime().add([=] {
			state->refreshUi = nullptr;
			state->refreshAccounts = nullptr;
		});

		state->filter.value(
		) | rpl::on_next([=](SmsModel::Filter filter) {
			tiles->setFilter(filter);
			chips->setFilter(filter);
			rebuildRows();
		}, box->lifetime());
		rpl::merge(
			tiles->filterRequests(),
			chips->filterRequests()
		) | rpl::on_next([=](SmsModel::Filter filter) {
			state->filter = filter;
			revealRecipients();
		}, box->lifetime());
		refresh();

		const auto dismiss = [=] {
			if (CurrentCampaign() == state) {
				CurrentCampaign() = nullptr;
				IndicatorState() = SmsCampaignIndicator::None;
				ProgressState() = QString();
			}
		};
		box->addButton(state->buttonText.value(), [=] {
			if (!state->finished) {
				// Stopping keeps the box open on the final results (with
				// the button turning into "Готово") instead of closing it
				// and throwing them away.
				*state->cancelled = true;
				state->stopped = true;
				for (const auto &abort : state->laneAbort) {
					if (abort) {
						*abort = true;
					}
				}
				FinishCampaign(state.get());
				return;
			}
			dismiss();
			box->closeBox();
		});
		const auto pauseButton = box->addButton(
			state->pauseText.value(),
			[=] {
				if (state->finished) {
					return;
				}
				state->paused = !state->paused;
				state->pauseText = state->paused
					? u"Продолжить"_q
					: u"Пауза"_q;
				if (state->process) {
					state->process->log(state->paused
						? u"Пауза"_q
						: u"Продолжение"_q);
				}
				refresh();
			});
		// The window has room for one button on the left: "Скрыть" while the
		// run goes on (it only closes the window, the run continues and
		// comes back from "Процессы"), "Результаты" once it has ended.
		box->addLeftButton(
			state->running.value() | rpl::map([](bool running) {
				return running ? u"Скрыть"_q : u"Результаты"_q;
			}),
			[=] {
				if (state->running.current()) {
					box->closeBox();
				} else {
					box->uiShow()->show(MakeSmsResultsBoxStandalone());
				}
			});
		state->running.value(
		) | rpl::on_next([=](bool running) {
			if (const auto button = pauseButton.data()) {
				button->setVisible(running);
			}
		}, box->lifetime());
	}));
}

// ---- sending: one account at a time, full recipient list each ----

[[nodiscard]] bool IsAccountSleeping(
		not_null<Main::Account*> account,
		int protectionMode,
		bool nightSleepEnabled,
		int *outWakeHour = nullptr,
		QString *outLocalTime = nullptr) {
	if (!nightSleepEnabled || protectionMode == 2) {
		return false;
	}
	const auto tzOffset = account->deviceOverride().tzOffset.value_or(0);
	const auto local = QDateTime::currentDateTimeUtc().addSecs(tzOffset).time();
	const auto hour = local.hour();
	const auto sleepStart = (protectionMode == 0) ? 1 : 2;
	const auto sleepEnd = (protectionMode == 0) ? 7 : 6;
	if (outWakeHour) {
		*outWakeHour = sleepEnd;
	}
	if (outLocalTime) {
		*outLocalTime = local.toString(u"HH:mm"_q);
	}
	return (hour >= sleepStart && hour < sleepEnd);
}

// Live progress box + `threadCount` concurrent lanes sharing one
// recipient queue and one account pool. One row per recipient (status
// text/color updated in place, labelled with which account handled it);
// one lane label per thread showing its current account or, between
// sends, a live "пауза N сек..." countdown - see tg-web-panel/main.py's
// _sms_job_log pause line, which this mirrors.
void RunSmsCampaignNow(
		not_null<Window::SessionController*> controller,
		Accounts accounts,
		QStringList usernames,
		QStringList templates,
		int perAccountQuota,
		int threadCount,
		int spamTolerance,
		int refusalLimit,
		bool warmup,
		bool greetingSticker,
		crl::time pauseMin,
		crl::time pauseMax,
		crl::time pauseAccountMin,
		crl::time pauseAccountMax,
		base::flat_map<QString, uint64> baseOf,
		int protectionMode = 1,
		bool nightSleep = true,
		bool simulateTypos = true,
		bool filterByGgr = false,
		bool skipSuspectedFakes = true) {
	Expects(!accounts.empty());
	Expects(!usernames.empty());
	Expects(!templates.isEmpty());

	if (!Core::GuardAllows(
			controller->uiShow(),
			Core::GuardedAction::StartSms)) {
		return;
	}
	// One campaign at a time: two at once could write to the same person
	// (neither has recorded its in-flight recipients yet) or from the same
	// account in parallel.
	if (const auto running = CurrentCampaign();
			running && !running->finished) {
		Ggtg::Notice(
			controller->uiShow(),
			u"Рассылка уже идёт: остановите её или дождитесь конца"_q);
		BuildCampaignProgressBox(controller->uiShow(), running);
		return;
	}
	// A new campaign: every account gets a new warm-up habit, and the groups
	// found closed are forgotten.
	ResetWarmupHabits();
	ResetGroupRoutes();
	// Accounts from the «Корзина» sleep without a connection - wake the
	// ones this run uses.
	for (const auto &account : accounts) {
		account->wake();
	}
	const auto baseIdOf = [baseOf](const QString &username) {
		const auto it = baseOf.find(username.toLower());
		return (it != baseOf.end()) ? it->second : uint64(0);
	};
	// No more lanes than accounts: a lane without one would only show up as
	// a "поток" doing nothing.
	const auto lanes = std::clamp(threadCount, 1, int(accounts.size()));
	const auto total = int(usernames.size());
	// "Цель" (the denominator in "Обработано: X / цель") is the campaign's
	// realistic capacity - how many accounts were chosen times how many
	// messages each one sends - not the raw size of the recipient list,
	// which is usually far larger than what this run can actually cover.
	// A per-account quota of 0 means "unlimited", so the whole list is the
	// goal then; otherwise it is capped by the list (you never send more
	// than there are recipients).
	// Only the recipients this run will really try: the ones settled by an
	// earlier run (and bots) are counted apart, not in this run's numbers.
	auto fresh = 0;
	for (const auto &username : usernames) {
		if (!Data::IsBotUsername(username)
			&& !Data::FindSmsSendRecord(username)) {
			++fresh;
		}
	}
	const auto goal = (perAccountQuota > 0)
		? std::min(int(accounts.size()) * perAccountQuota, fresh)
		: fresh;
	const auto runId = qint64(base::unixtime::now());

	const auto paceMultiplier = (protectionMode == 0)
		? 1.5
		: (protectionMode == 2)
		? 0.7
		: 1.0;
	pauseMin = crl::time(double(pauseMin) * paceMultiplier);
	pauseMax = crl::time(double(pauseMax) * paceMultiplier);
	pauseAccountMin = crl::time(double(pauseAccountMin) * paceMultiplier);
	pauseAccountMax = crl::time(double(pauseAccountMax) * paceMultiplier);

	const auto state = std::make_shared<CampaignRunState>();
	state->protectionMode = protectionMode;
	state->nightSleep = nightSleep;
	state->simulateTypos = simulateTypos;
	state->filterByGgr = filterByGgr;
	state->usernames = usernames;

	// Written now, removed by FinishCampaign once everyone has been dealt
	// with: a campaign interrupted before that (quit, crash, stopped by hand,
	// accounts ran out) can then be offered for resuming - see
	// OpenSmsCampaign.
	{
		auto job = Data::SmsJob();
		job.usernames = usernames;
		for (const auto &[username, baseId] : baseOf) {
			job.baseOf.emplace_back(username, baseId);
		}
		job.templates = templates;
		for (const auto &account : accounts) {
			if (account->sessionExists()) {
				job.accountUserIds.push_back(
					account->session().user()->id.value);
			}
		}
		job.perAccountQuota = perAccountQuota;
		job.threadCount = threadCount;
		job.spamTolerance = spamTolerance;
		job.refusalLimit = refusalLimit;
		job.protectionMode = protectionMode;
		job.nightSleep = nightSleep;
		job.simulateTypos = simulateTypos;
		job.filterByGgr = filterByGgr;
		job.warmup = warmup;
		job.greetingSticker = greetingSticker;
		job.pauseMinSeconds = int(pauseMin / 1000);
		job.pauseMaxSeconds = int(pauseMax / 1000);
		job.pauseAccountMinSeconds = int(pauseAccountMin / 1000);
		job.pauseAccountMaxSeconds = int(pauseAccountMax / 1000);
		Data::SaveSmsJob(job);
	}
	state->lanes = lanes;
	state->goal = goal;
	state->recipients = std::vector<SmsModel::Recipient>(total);
	for (auto i = 0; i != total; ++i) {
		state->recipients[i].username = usernames[i];
		state->indexOf.emplace(usernames[i].toLower(), i);
	}
	for (auto k = 0; k != int(accounts.size()); ++k) {
		state->accountIndex.emplace(accounts[k].get(), k);
		// The name alone is the title, the phone and @username under it
		// tell apart accounts that share a name.
		auto view = SmsModel::Account{ .label = AccountLabel(accounts[k]) };
		if (accounts[k]->sessionExists()) {
			const auto user = accounts[k]->session().user();
			if (!user->name().isEmpty()) {
				view.label = user->name();
			}
			auto parts = QStringList();
			if (!user->phone().isEmpty()) {
				parts.push_back('+' + user->phone());
			}
			if (!user->username().isEmpty()) {
				parts.push_back('@' + user->username());
			}
			view.info = parts.join(u" · "_q);
		}
		state->accountViews.push_back(std::move(view));
	}
	state->laneLive = std::vector<QString>(lanes);
	state->laneLastSettle = std::vector<crl::time>(lanes, 0);
	state->laneAccount = std::vector<Main::Account*>(lanes, nullptr);
	state->laneAbort = std::vector<std::shared_ptr<bool>>(lanes);
	state->laneFinished = std::vector<bool>(lanes, false);
	state->laneCounters = std::vector<SmsModel::LaneCounters>(lanes);
	state->laneRefusals.assign(lanes, {});
	state->laneNotFoundPending
		= std::vector<std::vector<QString>>(lanes);
	state->laneRunners.resize(lanes);

	const auto refreshUi = [=] {
		if (state->refreshUi) {
			state->refreshUi();
		}
	};
	const auto refreshAccounts = [=] {
		if (state->refreshAccounts) {
			state->refreshAccounts();
		}
	};
	const auto accountView = [=](not_null<Main::Account*> account)
	-> SmsModel::Account& {
		return state->accountViews[state->accountIndex[account.get()]];
	};
	// A line in the account's own log (newest last, the oldest dropped).
	const auto logAccount = [=](Main::Account *account, const QString &text) {
		if (!account) {
			return;
		}
		const auto it = state->accountIndex.find(account);
		if (it == state->accountIndex.end()) {
			return;
		}
		if (state->accountLog.size() <= std::size_t(it->second)) {
			state->accountLog.resize(it->second + 1);
		}
		auto &log = state->accountLog[it->second];
		log.push_back(QTime::currentTime().toString(u"HH:mm:ss"_q)
			+ u"  "_q + text);
		if (log.size() > 800) {
			log.erase(log.begin(), log.begin() + 200);
		}
	};
	// A settled recipient: counted, shown, logged, its time recorded for the
	// estimate. `lane` is -1 outside a lane.
	const auto settle = [=](
			int i,
			int lane,
			SmsModel::Outcome outcome,
			const QString &reason,
			Main::Account *account) {
		SmsModel::Settle(state->recipients, state->counts, i, outcome, reason);
		auto &r = state->recipients[i];
		if (account) {
			r.account = AccountLabel(account);
			logAccount(account, (outcome == SmsModel::Outcome::Sent
				? u"@"_q
				: u"@"_q) + r.username + u" - итог: "_q
				+ SmsModel::Title(outcome)
				+ (reason.isEmpty() ? QString() : (u": "_q + reason)));
			auto &view = accountView(account);
			if (outcome == SmsModel::Outcome::Sent) {
				++view.sent;
			} else if (outcome == SmsModel::Outcome::Peer) {
				++view.refused;
			} else if (outcome == SmsModel::Outcome::Fail) {
				++view.failed;
			}
		}
		if (lane >= 0) {
			const auto now = crl::now();
			if (const auto last = state->laneLastSettle[lane]) {
				state->recentDurations.push_back(int((now - last) / 1000));
				if (int(state->recentDurations.size()) > kRecentDurations) {
					state->recentDurations.erase(
						state->recentDurations.begin());
				}
			}
			state->laneLastSettle[lane] = now;
		}
		if (state->process) {
			state->process->log('@' + r.username + u" — "_q
				+ SmsModel::Title(outcome)
				+ (reason.isEmpty() ? QString() : (u": "_q + reason))
				+ (r.account.isEmpty()
					? QString()
					: (u" ("_q + r.account + ')')));
		}
		refreshUi();
	};
	const auto setInProgress = [=](
			int i,
			not_null<Main::Account*> account) {
		auto &r = state->recipients[i];
		r.inProgress = true;
		r.outcome = std::nullopt;
		r.account = AccountLabel(account);
		logAccount(account, u"→ взял @"_q + r.username);
		PushToOrder(state.get(), i);
		refreshUi();
	};
	// What the lane is doing now ("пауза 42 сек", "открывает профиль @x").
	const auto setLaneLive = [=](int lane, const QString &text) {
		state->laneLive[lane] = text;
		refreshAccounts();
	};
	const auto setAccountState = [=](
			not_null<Main::Account*> account,
			SmsModel::AccountState accountState,
			const QString &note) {
		auto &view = accountView(account);
		view.state = accountState;
		view.note = note;
		logAccount(account, u"Состояние: "_q
			+ SmsModel::StateTitle(accountState)
			+ (note.isEmpty() ? QString() : (u" - "_q + note)));
		refreshUi();
	};

	const auto updateSummary = [=] {
		const auto processed = state->counts[int(SmsModel::Outcome::Sent)];
		state->summaryText = SmsModel::FinishLine(state->counts);
		ProgressState() = QString::number(processed) + '/'
			+ QString::number(goal);
		if (state->process) {
			state->process->progress(processed, goal);
		}
		refreshUi();
	};

	// Live "пауза N сек..." countdown on the lane's own label between two
	// real sends - see tg-web-panel/main.py's per-second pause loop.
	const auto scheduleNext = [=](int lane, crl::time delay) {
		if (*state->cancelled) {
			return;
		}
		const auto remaining = std::make_shared<int>(int(delay / 1000));
		if (*remaining > 0) {
			logAccount(
				state->laneAccount[lane],
				u"пауза "_q + QString::number(*remaining) + u" сек"_q);
		}
		const auto tick = std::make_shared<Fn<void()>>();
		*tick = [=] {
			if (*state->cancelled) {
				return;
			}
			if (*remaining <= 0) {
				(*state->laneRunners[lane])();
				return;
			}
			setLaneLive(
				lane,
				u"пауза "_q + QString::number(*remaining) + u" сек"_q);
			--(*remaining);
			base::call_delayed(crl::time(1000), &Core::App(), *tick);
		};
		(*tick)();
	};

	// Pre-pass: settle (without touching the server) every recipient that
	// already has an outcome from an earlier run, and collect the rest as
	// the shared queue the lanes below claim from one at a time.
	for (auto i = 0; i != total; ++i) {
		const auto &username = usernames[i];
		if (Data::IsBotUsername(username)) {
			// Never worth an account's attempt - see SendSmsTextMessage.
			++state->earlier;
			continue;
		}
		if (!Data::FindSmsSendRecord(username)) {
			state->pendingIndices.push_back(i);
			continue;
		}
		// Not this run's doing: kept out of its counters.
		++state->earlier;
	}
	updateSummary();

	CurrentCampaign() = state;
	IndicatorState() = SmsCampaignIndicator::Running;

	// Weak, not the controller itself - reopen() can fire long after this
	// call returns (whenever the user clicks this campaign's row in
	// "Процессы"), potentially from a window that's since closed.
	const auto weakController = base::make_weak(controller);
	// "Рассылка: база X · 178 получателей" - several runs of the same size
	// stay tellable apart in "Процессы".
	auto processTitle = u"Рассылка"_q;
	{
		auto names = QStringList();
		auto seen = base::flat_set<uint64>();
		for (const auto &[username, baseId] : baseOf) {
			if (seen.emplace(baseId).second) {
				for (const auto &meta : Data::LoadScrapedGroupsIndex()) {
					if (meta.id == baseId) {
						names.push_back(meta.title);
						break;
					}
				}
			}
		}
		if (!names.isEmpty()) {
			processTitle += u": "_q + names.mid(0, 2).join(u", "_q)
				+ (names.size() > 2 ? u" и ещё "_q
					+ QString::number(names.size() - 2) : QString());
		}
		processTitle += u" · "_q + QString::number(total) + u" получ."_q;
	}
	state->process = Core::App().processes().start(
		processTitle,
		[=] {
			if (const auto alive = CurrentSessionController(weakController.get())) {
				OpenSmsCampaign(alive);
			}
		},
		Core::ProcessKind::Sms);

	BuildCampaignProgressBox(controller->uiShow(), state);

	// The account couldn't get to this recipient at all (FLOOD_WAIT, a dead
	// session, throttled lookups): nothing was sent and it's not the
	// recipient's fault. Not handed to another account - marked instead, so
	// the mark can be taken off later ("Пометки" in the form).
	const auto markAccountFail = [=](
			int i,
			int lane,
			const QString &username,
			const QString &why,
			not_null<Main::Account*> account) {
		Data::MarkSmsStatus(
			username,
			Data::SmsSendStatus::AccountFail,
			AccountLabel(account),
			baseIdOf(username));
		Data::SetSmsNote(username, why);
		settle(i, lane, SmsModel::Outcome::Fail, why, account);
		updateSummary();
	};

	// Takes the lane's account out of this campaign; the runner claims the
	// next one from the pool on its following pass.
	const auto retireAccount = [=](
			int lane,
			not_null<Main::Account*> account,
			SmsModel::AccountState accountState,
			const QString &reason) {
		state->laneAccount[lane] = nullptr;
		state->laneNotFoundPending[lane].clear();
		state->laneLive[lane] = QString();
		setAccountState(account, accountState, reason);
		updateSummary();
		if (state->process) {
			state->process->log(AccountLabel(account)
				+ (accountState == SmsModel::AccountState::Done
					? u" отработал: "_q
					: u" снят с рассылки: "_q)
				+ reason);
		}
	};

	// Runs once when the campaign ends (see FinishCampaign): whoever never
	// got an attempt (stopped, or every account was used up first) is only
	// counted - no mark, they stay in the base for the next run.
	state->finalize = [=] {
		state->unprocessed = std::max(
			int(state->pendingIndices.size()) - state->nextPending,
			0);
		updateSummary();
	};

	for (auto lane = 0; lane != lanes; ++lane) {
		state->laneRunners[lane] = std::make_shared<Fn<void()>>();
		*state->laneRunners[lane] = [=] {
			if (*state->cancelled) {
				return;
			}
			if (state->paused) {
				setLaneLive(lane, u"на паузе"_q);
				base::call_delayed(crl::time(1000), &Core::App(), [=] {
					(*state->laneRunners[lane])();
				});
				return;
			}
			// Claim a fresh account for this lane if it doesn't have one
			// (first run, or the previous one was just retired below).
			while (!state->laneAccount[lane]) {
				if (state->nextAccount >= int(accounts.size())) {
					auto hasSleeping = false;
					int minWakeHour = 24;
					for (const auto &acc : accounts) {
						if (Main::AccountIsLive(acc)) {
							int wake = 6;
							if (IsAccountSleeping(acc, protectionMode, nightSleep, &wake)) {
								hasSleeping = true;
								minWakeHour = std::min(minWakeHour, wake);
							}
						}
					}
					if (hasSleeping) {
						setLaneLive(lane, u"ночной сон до %1:00 (гео-прокси)"_q.arg(minWakeHour));
						base::call_delayed(crl::time(60'000), &Core::App(), [=] {
							if (!*state->cancelled) {
								state->nextAccount = 0;
								(*state->laneRunners[lane])();
							}
						});
						return;
					}
					setLaneLive(lane, QString());
					MarkLaneFinished(state.get(), lane);
					return;
				}
				const auto candidate = accounts[state->nextAccount++];
				if (!Main::AccountIsLive(candidate)) {
					// Logged out or removed since the run started: never
					// given a recipient.
					setAccountState(
						candidate,
						SmsModel::AccountState::Retired,
						u"сессия недоступна"_q);
					if (state->process) {
						state->process->log(AccountLabel(candidate)
							+ u": сессия недоступна - пропущен"_q);
					}
					continue;
				}
				int wakeHour = 6;
				QString localTime;
				if (IsAccountSleeping(candidate, protectionMode, nightSleep, &wakeHour, &localTime)) {
					setAccountState(
						candidate,
						SmsModel::AccountState::Waiting,
						u"спит до %1:00 (ночь по гео: %2)"_q.arg(wakeHour).arg(localTime));
					if (state->process) {
						state->process->log(AccountLabel(candidate)
							+ u": ночной сон по гео (местное время " + localTime
							+ u", до " + QString::number(wakeHour) + u":00)");
					}
					continue;
				}
				state->laneAccount[lane] = candidate;
				// Everything below is per account, not per lane - carrying
				// the previous account's counts over made every account
				// after the first on a lane hit its quota after a single
				// message.
				state->laneCounters[lane] = SmsModel::LaneCounters();
				state->laneRefusals[lane].clear();
				state->laneNotFoundPending[lane].clear();
				setAccountState(
					candidate,
					SmsModel::AccountState::Working,
					QString());
				setLaneLive(lane, QString());
				updateSummary();
			}
			// The session went away between two recipients: the account is
			// taken off before it can spend anyone.
			if (const auto current = state->laneAccount[lane];
					current && !Main::AccountIsLive(current)) {
				retireAccount(
					lane,
					not_null<Main::Account*>(current),
					SmsModel::AccountState::Retired,
					u"сессия закрылась"_q);
				scheduleNext(lane, kSmsSkipPause);
				return;
			}
			if (const auto current = state->laneAccount[lane]) {
				int wakeHour = 6;
				QString localTime;
				if (IsAccountSleeping(not_null<Main::Account*>(current), protectionMode, nightSleep, &wakeHour, &localTime)) {
					logAccount(current, u"ночной сон по гео (местное время %1): спит до %2:00"_q
						.arg(localTime).arg(wakeHour));
					setAccountState(
						not_null<Main::Account*>(current),
						SmsModel::AccountState::Waiting,
						u"спит до %1:00 (ночь по гео)"_q.arg(wakeHour));
					state->laneAccount[lane] = nullptr;
					scheduleNext(lane, kSmsSkipPause);
					return;
				}
				if (state->filterByGgr) {
					const auto ggr = CalculateAccountGgr(not_null<Main::Account*>(current));
					if (ggr.score < 7.0f) {
						logAccount(current, u"GGR %1 (%2) < 7.0: аккаунт отсеян фильтром траста"_q
							.arg(QString::number(ggr.score, 'f', 1), ggr.tier));
						setAccountState(
							not_null<Main::Account*>(current),
							SmsModel::AccountState::Retired,
							u"GGR %1 < 7.0: отсеян"_q.arg(QString::number(ggr.score, 'f', 1)));
						state->laneAccount[lane] = nullptr;
						scheduleNext(lane, kSmsSkipPause);
						return;
					}
				}
			}
			auto i = -1;
			if (state->nextPending < int(state->pendingIndices.size())) {
				i = state->pendingIndices[state->nextPending++];
			} else {
				if (const auto left = state->laneAccount[lane]) {
					setAccountState(
						left,
						SmsModel::AccountState::Done,
						u"получатели кончились"_q);
					state->laneAccount[lane] = nullptr;
				}
				setLaneLive(lane, QString());
				MarkLaneFinished(state.get(), lane);
				return;
			}
			const auto account = not_null<Main::Account*>(
				state->laneAccount[lane]);
			const auto accountLabel = AccountLabel(account);
			const auto username = usernames[i];
			++state->inFlight;
			setLaneLive(lane, QString());
			setInProgress(i, account);
			// "Поток 1: Аккаунт: ищет @user → открывает профиль → ...": the
			// steps the account has taken with this recipient so far, in
			// order, ending with what it is doing right now.
			const auto trail = std::make_shared<QStringList>();
			const auto templateIndex = state->templateIndex++
				% int(templates.size());
			// Everything about this attempt for "Разбор рассылок".
			const auto info = std::make_shared<Core::GgtgSmsStats::Attempt>();
			info->at = base::unixtime::now();
			info->run = runId;
			info->sender = accountLabel;
			if (account->sessionExists()) {
				const auto self = account->session().user();
				info->senderId = self->id.value;
				info->senderPremium = self->isPremium();
				info->senderCountry = Countries::Instance()
					.countryISO2ByPhone(self->phone());
				info->senderSentToday = Data::CountSmsSentToday(
					info->senderId);
			}
			info->recipient = username;
			info->templateIndex = templateIndex;
			info->templateText = Core::GgtgSmsStats::TemplatePreview(
				templates[templateIndex]);
			info->hour = QTime::currentTime().hour();
			const auto record = [=](
					SmsStepOutcome outcome,
					const QString &message) {
				using Outcome = SmsStepOutcome;
				if (outcome == Outcome::Aborted) {
					return; // nothing happened to this recipient
				}
				const auto sent = (outcome == Outcome::Sent)
					|| (outcome == Outcome::Partial);
				info->outcome = sent
					? u"sent"_q
					: (outcome == Outcome::SpamBlock)
					? (IsAccountSpamBlocked(account)
						? u"spam"_q
						: u"peer"_q)
					: (outcome == Outcome::Skip
						|| outcome == Outcome::AlreadyWritten)
					? u"skip"_q
					: (outcome == Outcome::NotFound)
					? u"notfound"_q
					: (outcome == Outcome::FloodWait)
					? u"flood"_q
					: (outcome == Outcome::AccountDead)
					? u"dead"_q
					: (ClassifySmsError(message)
						== SmsErrorKind::RecipientOnly)
					? u"closed"_q
					: u"error"_q;
				info->error = (outcome == Outcome::Sent) ? QString() : message;
				const auto has = [&](const QString &prefix) {
					return ranges::any_of(*trail, [&](const QString &step) {
						return step.startsWith(prefix);
					});
				};
				const auto stickerOut = has(u"отправил стикер"_q);
				info->sticker = !greetingSticker
					? u"off"_q
					: stickerOut
					? u"sent"_q
					: has(u"стикер не ушёл"_q)
					? u"failed"_q
					: u"none"_q;
				info->text = (sent && info->parts > 0)
					? u"sent"_q
					: (has(u"текст НЕ ушёл"_q)
						|| has(u"печатает текст"_q)
						|| has(u"отправляет текст"_q))
					? u"failed"_q
					: u"none"_q;
				info->seconds = int(base::unixtime::now() - info->at);
				Core::GgtgSmsStats::Append(*info);
				if (stickerOut && !sent && info->parts > 0) {
					Core::GgtgLog::Info(
						u"Отправка SMS"_q,
						u"@"_q + username + u": стикер ушёл, текст нет ("_q
							+ message + u")"_q);
				}
			};
			const auto abort = std::make_shared<bool>(false);
			state->laneAbort[lane] = abort;
			SendSmsTextMessage(
				account,
				username,
				templates[templateIndex],
				warmup,
				greetingSticker,
				[=](QString step) {
					// "✓ ..." / "✗ ..." is how a step ended, plain text is
					// what is going on now: a new step takes the place of an
					// unfinished one, so only real results stay in the list.
					if (!trail->isEmpty() && !IsSettledStep(trail->back())) {
						trail->back() = step;
					} else {
						trail->push_back(step);
					}
					logAccount(account, u"@"_q + username + u": "_q + step);
					setLaneLive(
						lane,
						trail->isEmpty() ? QString() : trail->back());
				},
				[=](SmsStepOutcome outcome, QString message) {
				--state->inFlight;
				if (state->laneAbort[lane] == abort) {
					state->laneAbort[lane] = nullptr;
				}
				record(outcome, message);
				// One pause after every step: the pause between messages
				// while the lane keeps its account, the pause between
				// accounts once it was taken off or did its share.
				const auto continueAfter = [=](crl::time delay) {
					if (*state->cancelled) {
						return;
					}
					// Nobody left to write to: no point waiting out a pause
					// before ending.
					const auto nothingLeft = (state->nextPending
							>= int(state->pendingIndices.size()))
						&& !state->inFlight;
					scheduleNext(lane, nothingLeft ? kSmsSkipPause : delay);
				};
				// The account found this recipient, so its lookups work: the
				// earlier "not found" ones from it were genuine - written to
				// the registry now.
				const auto confirmNotFound = [=] {
					for (const auto &missing
							: state->laneNotFoundPending[lane]) {
						Data::MarkSmsStatus(
							missing,
							Data::SmsSendStatus::NotFound,
							accountLabel,
							baseIdOf(missing));
					}
					state->laneNotFoundPending[lane].clear();
				};
				using Kind = SmsModel::StepKind;
				const auto errorKind = ClassifySmsError(message);
				const auto accountBlocked
					= (outcome == SmsStepOutcome::SpamBlock)
					&& IsAccountSpamBlocked(account);
				// What the step was, as far as the account is concerned.
				const auto kind = [&] {
					switch (outcome) {
					case SmsStepOutcome::Sent: return Kind::Sent;
					case SmsStepOutcome::Partial: return Kind::Partial;
					case SmsStepOutcome::SpamBlock:
						return accountBlocked
							? Kind::SpamBlocked
							: Kind::Refused;
					case SmsStepOutcome::Skip:
					case SmsStepOutcome::AlreadyWritten: return Kind::Skipped;
					case SmsStepOutcome::NotFound: return Kind::NotFound;
					case SmsStepOutcome::FloodWait: return Kind::FloodWait;
					case SmsStepOutcome::AccountDead: return Kind::AccountDead;
					case SmsStepOutcome::Aborted: return Kind::Aborted;
					case SmsStepOutcome::Error: break;
					}
					switch (errorKind) {
					case SmsErrorKind::RecipientOnly: return Kind::Closed;
					case SmsErrorKind::Text: return Kind::TextError;
					case SmsErrorKind::FloodWait: return Kind::FloodWait;
					case SmsErrorKind::AccountDead: return Kind::AccountDead;
					case SmsErrorKind::Other: break;
					}
					return Kind::Failed;
				}();
				// What cut a Partial send short.
				const auto cause = (errorKind == SmsErrorKind::FloodWait)
					? Kind::FloodWait
					: (errorKind == SmsErrorKind::AccountDead)
					? Kind::AccountDead
					: Kind::Failed;
				const auto verdict = SmsModel::AfterStep(
					kind,
					state->laneCounters[lane],
					SmsModel::LaneLimits{
						.quota = perAccountQuota,
						.refusals = refusalLimit,
						.errors = kSmsErrorStreakLimit,
						.notFound = kNotFoundStreakLimit,
					},
					cause);

				// What happens to the recipient: the row, the registry.
				// A PEER_FLOOD may still have to wait for @SpamBot's word
				// before the lane goes on (see below).
				auto verdictPending = false;
				const auto verdictDone = std::make_shared<bool>(false);
				const auto holdProceed = std::make_shared<Fn<void()>>();
				switch (kind) {
				case Kind::Aborted: {
					// "Стоп" came before anything was sent: the recipient is
					// left as it was, nothing is recorded.
					auto &r = state->recipients[i];
					r.inProgress = false;
					r.outcome = std::nullopt;
					refreshUi();
				} break;
				case Kind::Skipped:
					if (outcome == SmsStepOutcome::AlreadyWritten) {
						// The chat already had a message of ours: written
						// to, never again - but nothing was sent now.
						confirmNotFound();
						Data::MarkSmsSent(
							username,
							accountLabel,
							baseIdOf(username));
						Data::SetSmsNote(username, message);
					} else if (message.startsWith(u"предположительный фейк"_q) || message.startsWith(u"пропущен: фейк"_q)) {
						confirmNotFound();
						Data::MarkSmsStatus(
							username,
							Data::SmsSendStatus::SuspectedFake,
							accountLabel,
							baseIdOf(username));
						Data::SetSmsNote(username, message);
					}
					settle(i, lane, SmsModel::Outcome::Skipped, message, account);
					break;
				case Kind::NotFound:
					if (verdict.retire == SmsModel::RetireReason::NotFound) {
						// Likely Telegram throttling this account's lookups,
						// not this many deleted users: the recipient gets a
						// mark, the earlier ones are not written off.
						state->laneNotFoundPending[lane].clear();
						markAccountFail(
							i,
							lane,
							username,
							u"поиск ограничен у "_q + accountLabel,
							account);
					} else {
						state->laneNotFoundPending[lane].push_back(username);
						settle(
							i,
							lane,
							SmsModel::Outcome::NotFound,
							QString(),
							account);
					}
					break;
				case Kind::FloodWait:
					markAccountFail(
						i,
						lane,
						username,
						u"FLOOD_WAIT "_q
							+ QString::number(FloodWaitSeconds(message))
							+ u" сек у "_q + accountLabel,
						account);
					break;
				case Kind::AccountDead:
					markAccountFail(
						i,
						lane,
						username,
						u"сессия аккаунта недействительна: "_q
							+ HumanSmsError(message),
						account);
					break;
				case Kind::Sent:
				case Kind::Partial:
					// Partial too: the person got something, so they are
					// written to and never messaged a second time.
					confirmNotFound();
					Data::MarkSmsSent(username, accountLabel, baseIdOf(username));
					if (account->sessionExists()) {
						// Still counted, for "Разбор рассылок".
						Data::RegisterSmsSentToday(
							account->session().user()->id.value);
					}
					settle(
						i,
						lane,
						SmsModel::Outcome::Sent,
						(kind == Kind::Partial)
							? (u"дошло не всё: "_q + HumanSmsError(message))
							: QString(),
						account);
					break;
				case Kind::SpamBlocked:
					confirmNotFound();
					Data::MarkSmsStatus(
						username,
						Data::SmsSendStatus::SpamAcc,
						accountLabel,
						baseIdOf(username));
					settle(i, lane, SmsModel::Outcome::Spam, QString(), account);
					break;
				case Kind::Refused: {
					// PEER_FLOOD while the account looks free: Telegram
					// refused this pair (another country, a stranger...).
					confirmNotFound();
					Data::MarkSmsStatus(
						username,
						Data::SmsSendStatus::PeerLimit,
						accountLabel,
						baseIdOf(username));
					state->laneRefusals[lane].emplace_back(username, i);
					state->peerLimited[account.get()].emplace_back(
						username,
						baseIdOf(username));
					settle(i, lane, SmsModel::Outcome::Peer, QString(), account);
					const auto userId = account->session().user()->id.value;
					const auto warnings = Data::RegisterSpamWarning(userId);
					if (warnings <= spamTolerance) {
						break;
					}
					// Past "Отказов Telegram до проверки": a refusal alone
					// proves nothing about the account, so @SpamBot is asked
					// (unless it answered a few minutes ago) and the lane
					// waits for its word. Free -> the account keeps
					// writing. Blocked -> the account stops and its refused
					// recipients are rewritten as its spam block.
					const auto known = Data::FindAccountSpamStatus(userId);
					const auto fresh = known
						&& (QDateTime::currentSecsSinceEpoch()
							- known->checkedAt) < 300;
					if (fresh) {
						Data::RegisterSpamWarningReset(userId);
						break;
					}
					verdictPending = true;
					const auto life = std::make_shared<rpl::lifetime>();
					const auto spamVerdict = [=] {
						if (std::exchange(*verdictDone, true)) {
							return;
						}
						life->destroy();
						if (IsAccountSpamBlocked(account)) {
							auto indices = std::vector<int>();
							for (const auto &[name, base]
									: state->peerLimited[account]) {
								Data::MarkSmsStatus(
									name,
									Data::SmsSendStatus::SpamAcc,
									accountLabel,
									base);
								const auto it = state->indexOf.find(
									name.toLower());
								if (it != state->indexOf.end()) {
									indices.push_back(it->second);
								}
							}
							SmsModel::RewritePeerToSpam(
								state->recipients,
								state->counts,
								indices);
							accountView(account).refused = 0;
							if (state->laneAccount[lane] == account) {
								retireAccount(
									lane,
									account,
									SmsModel::AccountState::SpamBlocked,
									AccountSpamStatusText(userId));
							} else {
								// Already taken off for its refusals: the
								// real reason is shown instead.
								setAccountState(
									account,
									SmsModel::AccountState::SpamBlocked,
									AccountSpamStatusText(userId));
								updateSummary();
							}
						} else {
							Data::RegisterSpamWarningReset(userId);
						}
						state->peerLimited.erase(account);
						if (*holdProceed) {
							(*holdProceed)();
						}
					};
					Data::AccountSpamStatusChanges(
					) | rpl::filter([=](uint64 id) {
						return id == userId;
					}) | rpl::on_next([=](uint64) {
						spamVerdict();
					}, *life);
					// No answer from @SpamBot: carry on as if free.
					base::call_delayed(
						kSpamBotReplyTimeout + 3000,
						&Core::App(),
						spamVerdict);
					CheckAccountSpamStatus(account);
				} break;
				case Kind::Closed:
					// Nobody can write there: says nothing about the account.
					confirmNotFound();
					Data::MarkSmsStatus(
						username,
						Data::SmsSendStatus::Restricted,
						accountLabel,
						baseIdOf(username));
					settle(
						i,
						lane,
						SmsModel::Outcome::Closed,
						HumanSmsError(message),
						account);
					break;
				case Kind::TextError:
				case Kind::Failed:
					settle(
						i,
						lane,
						SmsModel::Outcome::Fail,
						HumanSmsError(message),
						account);
					break;
				}
				updateSummary();

				// What happens to the account.
				if (state->laneAccount[lane] == account.get()) {
					using Reason = SmsModel::RetireReason;
					switch (verdict.retire) {
					case Reason::None: break;
					case Reason::Quota:
						retireAccount(
							lane,
							account,
							SmsModel::AccountState::Done,
							u"лимит "_q + QString::number(perAccountQuota)
								+ u" сообщений"_q);
						break;
					case Reason::Refusals: {
						// The refused people are not the recipients' fault
						// and stay in this account's name: marked with why,
						// so they can be told apart and brought back.
						const auto note = u"аккаунт "_q + accountLabel
							+ u" снят после "_q + QString::number(refusalLimit)
							+ u" отказов"_q;
						for (const auto &[name, index]
								: state->laneRefusals[lane]) {
							Data::SetSmsNote(name, note);
							state->recipients[index].reason = note;
						}
						state->laneRefusals[lane].clear();
						retireAccount(
							lane,
							account,
							SmsModel::AccountState::Retired,
							u"Telegram "_q + QString::number(refusalLimit)
								+ u" раз не пустил к получателям"_q);
					} break;
					case Reason::Errors:
						retireAccount(
							lane,
							account,
							SmsModel::AccountState::Retired,
							QString::number(kSmsErrorStreakLimit)
								+ u" сбоев за рассылку"_q);
						break;
					case Reason::NotFound:
						retireAccount(
							lane,
							account,
							SmsModel::AccountState::Retired,
							QString::number(kNotFoundStreakLimit)
								+ u" раз «не найден»: похоже, Telegram "
									"ограничил поиск по username"_q);
						break;
					case Reason::FloodWait:
						retireAccount(
							lane,
							account,
							SmsModel::AccountState::Retired,
							u"FLOOD_WAIT "_q
								+ QString::number(FloodWaitSeconds(message))
								+ u" сек"_q);
						break;
					case Reason::AccountDead:
						retireAccount(
							lane,
							account,
							SmsModel::AccountState::Retired,
							u"сессия недействительна ("_q + message + ')');
						break;
					case Reason::SpamBlock:
						retireAccount(
							lane,
							account,
							SmsModel::AccountState::SpamBlocked,
							AccountSpamStatusText(
								account->session().user()->id.value));
						break;
					}
				}

				// And the lane: the pause between messages while it keeps
				// its account, the pause between accounts once the account
				// is off (decided only now: @SpamBot may still take it off).
				if (verdict.pause == SmsModel::PauseKind::None) {
					continueAfter(kSmsSkipPause);
					return;
				}
				const auto proceed = [=] {
					continueAfter(state->laneAccount[lane]
						? NextSmsPause(pauseMin, pauseMax)
						: (state->nextAccount < int(accounts.size()))
						? NextSmsPause(pauseAccountMin, pauseAccountMax)
						// Nobody to hand over to: nothing to wait for.
						: kSmsSkipPause);
				};
				if (verdictPending && !*verdictDone) {
					*holdProceed = proceed;
				} else {
					proceed();
				}
			}, abort, [=](SmsStepOutcome outcome, QString) {
				// The step was already reported as a timeout error, but the
				// server did answer afterwards - the message really went
				// out (or hit PEER_FLOOD), so record it (a duplicate on the
				// next campaign otherwise) and correct the row and counters.
				const auto spamAcc = (outcome == SmsStepOutcome::SpamBlock);
				// The timeout was counted as this account's failure: it
				// wasn't one.
				if (state->laneAccount[lane] == account.get()
						&& state->laneCounters[lane].errors > 0) {
					--state->laneCounters[lane].errors;
				}
				if (spamAcc) {
					Data::MarkSmsStatus(
						username,
						IsAccountSpamBlocked(account)
							? Data::SmsSendStatus::SpamAcc
							: Data::SmsSendStatus::PeerLimit,
						accountLabel,
						baseIdOf(username));
				} else {
					Data::MarkSmsSent(
						username,
						accountLabel,
						baseIdOf(username));
					if (account->sessionExists()) {
						Data::RegisterSmsSentToday(
							account->session().user()->id.value);
					}
				}
				SmsModel::CorrectLateOutcome(
					state->recipients,
					state->counts,
					i,
					spamAcc
						? (IsAccountSpamBlocked(account)
							? SmsModel::Outcome::Spam
							: SmsModel::Outcome::Peer)
						: SmsModel::Outcome::Sent);
				auto &view = accountView(account);
				if (view.failed > 0) {
					--view.failed;
				}
				if (spamAcc) {
					++view.refused;
				} else {
					++view.sent;
				}
				updateSummary();
			}, info, OpenGroupOfBase(baseIdOf(username)),
				GroupNameOfBase(baseIdOf(username)),
				protectionMode,
				simulateTypos,
				skipSuspectedFakes);
		};
	}
	// The first lane starts at once, every next one a little later: the
	// accounts don't all make their first requests in the same second.
	(*state->laneRunners[0])();
	for (auto lane = 1; lane != lanes; ++lane) {
		scheduleNext(
			lane,
			NextSmsPause(crl::time(lane) * 5'000, crl::time(lane) * 10'000));
	}
}

// Takes in what the other device (GGTG on the phone) sent first, so that
// nobody it already wrote to gets a message from here as well.
void RunSmsCampaign(
		not_null<Window::SessionController*> controller,
		Accounts accounts,
		QStringList usernames,
		QStringList templates,
		int perAccountQuota,
		int threadCount,
		int spamTolerance,
		int refusalLimit,
		bool warmup,
		bool greetingSticker,
		crl::time pauseMin,
		crl::time pauseMax,
		crl::time pauseAccountMin,
		crl::time pauseAccountMax,
		base::flat_map<QString, uint64> baseOf,
		int protectionMode = 1,
		bool nightSleep = true,
		bool simulateTypos = true,
		bool filterByGgr = false,
		bool skipSuspectedFakes = true) {
	const auto weak = base::make_weak(controller);
	auto weakAccounts = std::vector<base::weak_ptr<::Main::Account>>();
	for (const auto &account : accounts) {
		weakAccounts.push_back(base::make_weak(account.get()));
	}
	Core::GgtgSync::SyncBeforeMailing([=] {
		const auto strong = weak.get();
		if (!strong) {
			return;
		}
		auto alive = Accounts();
		for (const auto &account : weakAccounts) {
			if (const auto strongAccount = account.get()) {
				alive.push_back(strongAccount);
			}
		}
		if (alive.empty()) {
			return;
		}
		RunSmsCampaignNow(
			strong,
			std::move(alive),
			usernames,
			templates,
			perAccountQuota,
			threadCount,
			spamTolerance,
			refusalLimit,
			warmup,
			greetingSticker,
			pauseMin,
			pauseMax,
			pauseAccountMin,
			pauseAccountMax,
			baseOf,
			protectionMode,
			nightSleep,
			simulateTypos,
			filterByGgr,
			skipSuspectedFakes);
	});
}

object_ptr<Ui::BoxContent> MakeSmsCampaignSetupBox(
		not_null<Window::SessionController*> controller,
		uint64 presetBaseId,
		bool withBack) {
	return Box([=](not_null<Ui::GenericBox*> box) {
		box->setWidth(st::accountsBoxWidth);
		box->setTitle(rpl::single(u"Рассылка"_q));
		if (withBack) {
			Ggtg::AddBackButton(box);
		}

		const auto state = box->lifetime().make_state<CampaignState>();
		state->show = controller->uiShow();
		RestoreFormState(state);
		if (presetBaseId) {
			// "Отправить SMS по этой базе": only that one checked, and every
			// other base counts as already seen so none defaults back on.
			state->selectedBaseIds.clear();
			state->selectedBaseIds.emplace(presetBaseId);
			for (const auto &meta : Data::LoadScrapedGroupsIndex()) {
				state->knownBaseIds.emplace(meta.id);
			}
		}
		// Any change to the bases (created, edited, deleted, or statuses
		// moving as a campaign writes them) refreshes the checkbox list and
		// its numbers. Postponed and coalesced: a single edit can fire
		// several times, and rebuilding must not happen mid-handler.
		Data::BasesChanged(
		) | rpl::on_next([=] {
			if (std::exchange(state->basesRefreshQueued, true)) {
				return;
			}
			Ui::PostponeCall(box, [=] {
				state->basesRefreshQueued = false;
				if (const auto container = state->basesContainer.data()) {
					RebuildBasesContainer(container, state);
				}
			});
		}, box->lifetime());
		// Hiding, sending and closing all end up here: remember the form so
		// the next open (or the next app launch) starts where this one left
		// off.
		box->boxClosing(
		) | rpl::on_next([=] {
			SaveFormState(state);
		}, box->lifetime());

		// In the order the user actually fills the form in: who writes,
		// to whom, what, then how carefully.
		// Each step is a card that folds to one line of what is chosen.
		// Which ones are folded is kept while the app runs.
		static auto stepExpanded = std::array<bool, 5>{
			true, true, true, true, true };
		const auto step = [&](
				int index,
				const QString &title,
				rpl::producer<QString> summary) {
			const auto card = box->addRow(
				object_ptr<SmsWidgets::StepCard>(
					box,
					title,
					std::move(summary),
					stepExpanded[index],
					index + 1),
				style::margins(
					st::boxRowPadding.left() - style::ConvertScale(12),
					style::ConvertScale(6),
					st::boxRowPadding.right() - style::ConvertScale(12),
					style::ConvertScale(6)));
			card->expandedValue(
			) | rpl::on_next([index](bool expanded) {
				stepExpanded[index] = expanded;
			}, card->lifetime());
			return card->content();
		};
		// A campaign cut short (quit, crash, "Стоп", accounts ran out) can
		// be picked up where it stopped - said right here, not only when
		// the section is opened from the menu.
		if (const auto job = Data::LoadSmsJob();
				job && !CurrentCampaign()) {
			const auto stats = Data::ComputeBaseStats(job->usernames);
			if (stats.remaining > 0) {
				const auto wrap = box->addRow(
					object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
						box,
						object_ptr<Ui::VerticalLayout>(box)),
					style::margins());
				const auto banner = wrap->entity();
				banner->add(
					object_ptr<Ui::FlatLabel>(
						banner,
						u"Есть незавершённая рассылка: осталось "_q
							+ QString::number(stats.remaining) + u" из "_q
							+ QString::number(stats.total) + '.',
						st::boxLabel),
					st::boxRowPadding);
				Ui::AddSkip(banner);
				const auto links = banner->add(
					object_ptr<SmsWidgets::LinksRow>(banner),
					st::boxRowPadding);
				links->add(u"Продолжить"_q, [=] {
					OpenSmsCampaign(controller);
				});
				links->add(u"Забыть"_q, [=] {
					Data::ClearSmsJob();
					wrap->hide(anim::type::normal);
				});
				Ui::AddSkip(banner);
				Ui::AddDivider(banner);
			}
		}

		SetupAccountsSection(
			box,
			state,
			step(0, u"Аккаунты"_q, state->accountsSummary.value()));
		SetupRecipientsSection(
			box,
			controller,
			state,
			step(1, u"Получатели"_q, state->recipientsSummary.value()));
		SetupTextSection(
			box,
			controller,
			state,
			step(2, u"Сообщение"_q, state->textSummary.value()));
		SetupAiProtectionSection(
			box,
			state,
			step(3, u"ИИ-Защита"_q, state->protectionSummary.value()));
		SetupLimitsSection(
			box,
			state,
			step(4, u"Режим"_q, state->limitsSummary.value()));

		Ui::AddDivider(box->verticalLayout());
		state->preflight = box->addRow(
			object_ptr<Ui::FlatLabel>(box, QString(), st::boxDividerLabel),
			style::margins(
				st::boxRowPadding.left(),
				st::defaultVerticalListSkip * 2,
				st::boxRowPadding.right(),
				st::defaultVerticalListSkip * 2));
		const auto basesVersion = box->lifetime().make_state<int>(0);
		const auto leftSignature = box->lifetime().make_state<QString>();
		const auto leftCount = box->lifetime().make_state<int>(0);
		const auto leftEmpty = box->lifetime().make_state<bool>(true);
		const auto leftTotal = box->lifetime().make_state<int>(0);
		Data::BasesChanged(
		) | rpl::on_next([=] {
			++*basesVersion;
		}, box->lifetime());
		const auto refreshPreflight = [=] {
			const auto label = state->preflight.data();
			if (!label) {
				return;
			}
			const auto accounts = Main::LiveAccounts(
				state->selectedAccounts);
			// Counting who is left reads every selected base: redo it only
			// when the selection or the bases themselves changed.
			auto signature = QString::number(*basesVersion) + ':';
			for (const auto id : state->selectedBaseIds) {
				signature += QString::number(id) + ',';
			}
			if (*leftSignature != signature) {
				*leftSignature = signature;
				const auto merged = MergedRecipients(state);
				*leftEmpty = merged.isEmpty();
				const auto stats = Data::ComputeBaseStats(merged);
				*leftCount = stats.remaining;
				*leftTotal = stats.total;
			}
			const auto left = *leftCount;
			auto blocked = 0;
			for (const auto &account : accounts) {
				if (IsAccountSpamBlocked(account)) {
					++blocked;
				}
			}
			// The folded cards' one-line summaries.
			state->accountsSummary = accounts.empty()
				? u"не выбраны"_q
				: (u"выбрано "_q + QString::number(accounts.size())
					+ (blocked
						? (u" · в спам-блоке "_q + QString::number(blocked))
						: u" · все свободны"_q));
			state->recipientsSummary = *leftEmpty
				? u"база не выбрана"_q
				: (u"осталось "_q + QString::number(left)
					+ u" из "_q + QString::number(*leftTotal));
			const auto templates = int(state->rotationPromptIds.size());
			const auto hasText = state->messageField
				&& !state->messageField->getLastText().trimmed().isEmpty();
			const auto hasSticker = state->greetingStickerCheckbox
				&& state->greetingStickerCheckbox->checked();
			state->textSummary = templates
				? (QString::number(templates) + u" шаблона по очереди"_q)
				: hasText
				? (hasSticker ? u"стикер и текст"_q : u"текст задан"_q)
				: hasSticker
				? u"только стикер"_q
				: u"текст не задан"_q;

			auto problems = QStringList();
			if (accounts.empty()) {
				problems.push_back(u"выберите аккаунт"_q);
			}
			if (left <= 0) {
				problems.push_back(*leftEmpty
					? u"выберите базу"_q
					: u"в выбранных базах некому писать"_q);
			}
			if (!hasText && !hasSticker && !templates) {
				problems.push_back(u"введите текст"_q);
			}
			if (!problems.isEmpty()) {
				auto text = problems.join(u", "_q);
				text[0] = text[0].toUpper();
				label->setText(text + '.');
				label->setTextColorOverride(st::windowSubTextFg->c);
				return;
			}
			const auto threads = std::max(
				1,
				ParseNonNegative(state->limitThreads, 1));
			const auto [pauseMin, pauseMax] = ParseSecondsRange(
				state->limitPauseRange,
				int(kSmsPauseMin / 1000),
				int(kSmsPauseMax / 1000));
			const auto [accPauseMin, accPauseMax] = ParseSecondsRange(
				state->limitAccountPauseRange,
				int(kAccountPauseMin / 1000),
				int(kAccountPauseMax / 1000));
			// What the run will really send: the accounts together cannot
			// write more than "сообщений с аккаунта" allows, however long
			// the list is.
			const auto quota = ParseNonNegative(state->limitPerAccount, 1);
			const auto usable = std::max(int(accounts.size()) - blocked, 0);
			const auto accountCount = std::max(usable, 1);
			auto planned = qint64(left);
			if (quota > 0) {
				planned = std::min(planned, qint64(accountCount) * quota);
			}
			const auto lanes = std::min(threads, accountCount);
			const auto accountsPerLane = (accountCount + lanes - 1) / lanes;
			const auto seconds
				= planned * ((pauseMin + pauseMax) / 2) / lanes
				+ qint64(accountsPerLane - 1)
					* ((accPauseMin + accPauseMax) / 2);
			label->setText(QString::number(accountCount)
				+ u" акк. → "_q + QString::number(planned)
				+ u" сообщ."_q
				+ ((planned < left)
					? (u" из "_q + QString::number(left))
					: QString())
				+ u" · "_q + SmsModel::DurationText(int(seconds))
				+ (blocked
					? (u" · спам-блок у "_q + QString::number(blocked)
						+ u", их можно исключить"_q)
					: QString()));
			label->setTextColorOverride(st::windowFg->c);
		};
		state->refreshPreflight = refreshPreflight;
		const auto preflightTimer = box->lifetime().make_state<base::Timer>(
			refreshPreflight);
		preflightTimer->callEach(2000);
		refreshPreflight();

		box->addButton(rpl::single(u"Запустить"_q), [=] {
			auto accounts = Main::LiveAccounts(state->selectedAccounts);
			if (accounts.empty()) {
				Ggtg::Notice(controller->uiShow(), 
					u"Выберите хотя бы один аккаунт"_q);
				return;
			}
			auto baseOf = base::flat_map<QString, uint64>();
			const auto recipients = MergedRecipients(state, &baseOf);
			if (recipients.isEmpty()) {
				Ggtg::Notice(controller->uiShow(), u"Список получателей пуст"_q);
				return;
			}
			const auto text = state->messageField
				? state->messageField->getLastText()
				: QString();
			// "Переводить сообщения" uses the free online translator (no AI keys needed).
			// "Лёгкий прогрев" is always on now (no checkbox).
			const auto warmup = true;
			const auto greetingSticker = state->greetingStickerCheckbox
				&& state->greetingStickerCheckbox->checked();
			// What goes out: the picked prompts in rotation, or the message
			// field alone.
			auto templates = QStringList();
			auto templateNames = QStringList();
			if (!state->rotationPromptIds.empty()) {
				for (const auto &prompt : Data::LoadSmsPrompts()) {
					if (ranges::contains(
							state->rotationPromptIds,
							prompt.id)
						&& !prompt.text.trimmed().isEmpty()) {
						templates.push_back(prompt.text);
						templateNames.push_back(prompt.name);
					}
				}
			}
			if (templates.isEmpty()) {
				// Without a sticker, empty text has nothing to send at
				// all; with one, the sticker alone is a valid message.
				if (!greetingSticker && text.trimmed().isEmpty()) {
					Ggtg::Notice(controller->uiShow(), 
						u"Введите текст сообщения"_q);
					return;
				}
				templates.push_back(text);
			}
			for (auto i = 0; i != int(templates.size()); ++i) {
				if (templates[i].trimmed().isEmpty()) {
					continue;
				}
				const auto sample = RenderSmsTemplateText(
					templates[i],
					u"Иван"_q,
					u"Петров"_q,
					u"Анна"_q,
					u"Смирнова"_q,
					u"Название группы"_q);
				const auto check = CheckSmsTemplate(
					templates[i],
					int(sample.size()));
				if (!check.error.isEmpty()) {
					Ggtg::Notice(controller->uiShow(), 
						(templateNames.isEmpty()
							? u"Текст сообщения: "_q
							: (u"Шаблон \""_q + templateNames[i]
								+ u"\": "_q))
							+ check.error);
					return;
				}
			}
			const auto perAccountQuota = ParseNonNegative(
				state->limitPerAccount, 1);
			const auto [pauseMinSeconds, pauseMaxSeconds] = ParseSecondsRange(
				state->limitPauseRange,
				int(kSmsPauseMin / 1000),
				int(kSmsPauseMax / 1000));
			const auto pauseMin = crl::time(pauseMinSeconds) * 1000;
			const auto pauseMax = crl::time(pauseMaxSeconds) * 1000;
			const auto [accPauseSecMin, accPauseSecMax] = ParseSecondsRange(
				state->limitAccountPauseRange,
				int(kAccountPauseMin / 1000),
				int(kAccountPauseMax / 1000));
			const auto pauseAccountMin = crl::time(accPauseSecMin) * 1000;
			const auto pauseAccountMax = crl::time(accPauseSecMax) * 1000;
			const auto threadCount = std::max(
				1,
				ParseNonNegative(state->limitThreads, 1));
			const auto spamTolerance = ParseNonNegative(
				state->limitSkipSpamBlock, 0);
			const auto refusalLimit = ParseNonNegative(
				state->limitRefusals,
				kSmsPeerRefusalStreakLimit);
			const auto launch = [=](Accounts list) {
				if (list.empty()) {
					Ggtg::Notice(controller->uiShow(), 
						u"Все выбранные аккаунты со спам-блоком"_q);
					return;
				}
				// The setup form stays where it is (it's the section's own
				// content); the progress box opens on top of it.
				const auto protectionMode = state->protectionMode;
				const auto nightSleep = state->nightSleepCheckbox
					? state->nightSleepCheckbox->checked()
					: true;
				const auto simulateTypos = state->simulateTyposCheckbox
					? state->simulateTyposCheckbox->checked()
					: true;
				const auto filterByGgr = state->filterByGgrCheckbox
					&& state->filterByGgrCheckbox->checked();
				const auto skipSuspectedFakes = state->skipSuspectedFakesCheckbox
					? state->skipSuspectedFakesCheckbox->checked()
					: true;
				RunSmsCampaign(
					controller,
					list,
					recipients,
					templates,
					perAccountQuota,
					threadCount,
					spamTolerance,
					refusalLimit,
					warmup,
					greetingSticker,
					pauseMin,
					pauseMax,
					pauseAccountMin,
					pauseAccountMax,
					baseOf,
					protectionMode,
					nightSleep,
					simulateTypos,
					filterByGgr,
			skipSuspectedFakes);
			};
			auto usable = Accounts();
			auto excludedNames = QStringList();
			const auto filterGgr = state->filterByGgrCheckbox
				&& state->filterByGgrCheckbox->checked();
			for (const auto &account : accounts) {
				if (IsAccountSpamBlocked(account)) {
					excludedNames.push_back(AccountLabel(account) + u" (спам-блок)"_q);
				} else if (filterGgr && CalculateAccountGgr(account).score < 7.0f) {
					excludedNames.push_back(AccountLabel(account) + u" (GGR < 7.0)"_q);
				} else {
					usable.push_back(account);
				}
			}
			if (excludedNames.isEmpty()) {
				launch(accounts);
				return;
			}
			const auto shown = excludedNames.mid(0, 5).join(u", "_q)
				+ (excludedNames.size() > 5
					? (u" и ещё "_q + QString::number(excludedNames.size() - 5))
					: QString());
			controller->uiShow()->show(Ui::MakeConfirmBox({
				.text = u"Среди выбранных аккаунтов требуют внимания: "_q
					+ QString::number(excludedNames.size()) + u" ("_q
					+ shown + u"). Исключить их из рассылки?"_q,
				.confirmed = [=](Fn<void()> &&close) {
					close();
					launch(usable);
				},
				.cancelled = [=](Fn<void()> &&close) {
					close();
					launch(accounts);
				},
				.confirmText = u"Исключить"_q,
				.cancelText = u"Оставить"_q,
				.strictCancel = true,
			}));
		}, st::defaultActiveButton);
	});
}

} // namespace

// No entry at all means nobody has ever asked @spambot about this account -
// distinct from an entry recording an explicit Clear verdict (see the
// comment on SetAccountSpamStatus in data/account_spam_status.cpp). The
// clickable badge in the "Отправка SMS" accounts list, and the compact hint
// next to the name in the main account switcher (see
// settings/sections/settings_information.cpp), both show that difference
// as a gray, clickable/absent "Статус" versus a green "Свободен" - sharing
// this one function instead of each keeping its own copy is what keeps
// them saying the same thing.
QString AccountSpamStatusText(uint64 userId) {
	const auto status = Data::FindAccountSpamStatus(userId);
	if (!status) {
		return u"Статус"_q;
	} else if (status->state == Data::AccountSpamState::Clear
		|| (status->state == Data::AccountSpamState::Temporary
			&& status->until <= QDateTime::currentSecsSinceEpoch())) {
		// A limit whose time has passed is over: the same as everywhere
		// else (the account manager counts it so too).
		return u"Свободен"_q;
	} else if (status->state == Data::AccountSpamState::Permanent) {
		return u"Вечный блок"_q;
	}
	return u"Блок до "_q
		+ QDateTime::fromSecsSinceEpoch(status->until).toString(u"dd.MM HH:mm"_q);
}

QColor AccountSpamStatusColor(uint64 userId) {
	const auto status = Data::FindAccountSpamStatus(userId);
	if (!status) {
		return st::windowSubTextFg->c;
	} else if (status->state == Data::AccountSpamState::Permanent) {
		return st::boxTextFgError->c;
	} else if (status->state == Data::AccountSpamState::Temporary
		&& status->until > QDateTime::currentSecsSinceEpoch()) {
		return st::historyPeer8NameFg->c;
	}
	// Not st::boxTextFgGood: this app's own bundled night theme (see
	// Telegram/Resources/night.tdesktop-theme) overrides that token to a
	// blue (#5598db), not a green - fine for its original "accepted box
	// text" use, wrong for a traffic-light "all clear" that has to stay
	// green regardless of theme. A plain literal, same idea as the
	// connection-status dot's identical choice a few dozen lines up in
	// settings/sections/settings_information.cpp.
	return QColor(76, 217, 100);
}

AccountGgrInfo CalculateAccountGgr(not_null<Main::Account*> account) {
	if (!Main::AccountIsLive(account)) {
		return { 1.0f, u"Не активен"_q };
	}
	const auto userId = account->session().user()->id.value;
	float score = 1.0f;

	// 1. Account age by User ID
	if (userId < 1'000'000'000ULL) {
		score += 4.0f; // Antique account (pre-2020)
	} else if (userId < 3'000'000'000ULL) {
		score += 3.5f; // 2020-2021
	} else if (userId < 5'500'000'000ULL) {
		score += 2.8f; // 2022-2023
	} else if (userId < 7'500'000'000ULL) {
		score += 1.8f; // 2024
	} else {
		score += 0.8f; // 2025-2026 fresh session
	}

	// 2. Proxy isolation
	const auto proxy = account->proxyOverride();
	if (proxy.type != MTP::ProxyData::Type::None) {
		score += 1.5f;
	} else {
		score += 0.4f;
	}

	// 3. Activity / Warmup
	const auto sent = Data::CountSmsSentToday(userId);
	if (sent > 0) {
		score += 1.0f;
	} else {
		score += 0.5f;
	}

	// 4. SpamBot status
	const auto spam = Data::FindAccountSpamStatus(userId);
	if (spam.has_value()) {
		if (spam->state == Data::AccountSpamState::Clear) {
			score += 2.5f;
		} else if (spam->state == Data::AccountSpamState::Temporary) {
			score = std::min(score, 2.5f);
		} else if (spam->state == Data::AccountSpamState::Permanent) {
			score = 1.0f;
		}
	} else {
		score += 1.5f;
	}

	score = std::clamp(score, 1.0f, 10.0f);
	const auto tier = (score >= 7.5f)
		? u"Высокий"_q
		: (score >= 5.0f)
		? u"Средний"_q
		: u"Низкий"_q;
	return { score, tier };
}

void AutoAppealSpamBot(
		not_null<Main::Account*> account,
		Fn<void(bool success, QString message)> done) {
	if (!Main::AccountIsLive(account)) {
		if (done) {
			done(false, u"нет активной сессии"_q);
		}
		return;
	}
	account->wake();
	ResolveUsernameForAccount(account, u"spambot"_q, [=](
			PeerData *peer,
			QString error) {
		if (!peer) {
			if (done) {
				done(false, u"не удалось найти @spambot"_q
					+ (error.isEmpty() ? QString() : (u": "_q + error)));
			}
			return;
		}
		SendBotStart(account, peer, [=](bool success, QString message) {
			if (!success) {
				if (done) {
					done(false, u"ошибка /start: "_q + message);
				}
				return;
			}
			base::call_delayed(crl::time(2500), &account->session(), [=] {
				SendTextToPeer(account, peer, u"Это ошибка"_q, [=](bool, QString) {
					base::call_delayed(crl::time(2500), &account->session(), [=] {
						SendTextToPeer(account, peer, u"Да"_q, [=](bool, QString) {
							base::call_delayed(crl::time(2500), &account->session(), [=] {
								SendTextToPeer(account, peer, u"Нет, ничего подобного не было"_q, [=](bool, QString) {
									base::call_delayed(crl::time(2500), &account->session(), [=] {
										const auto appealText = u"Здравствуйте! Мой аккаунт был ограничен по ошибке. Я не отправлял спам и не нарушал правил Telegram. Пожалуйста, снимите ограничения. Спасибо!"_q;
										SendTextToPeer(account, peer, appealText, [=](bool s4, QString m4) {
											if (s4) {
												Core::GgtgLog::Info(
													u"SpamBot"_q,
													u"Авто-апелляция успешно отправлена модераторам"_q);
												if (done) {
													done(true, u"Апелляция отправлена"_q);
												}
											} else {
												if (done) {
													done(false, m4);
												}
											}
										});
									});
								});
							});
						});
					});
				});
			});
		});
	});
}

// Messages @spambot with "/start" from this account and waits (with a
// timeout) for its reply, then saves what ParseSpamBotReply makes of it
// via Data::SetAccountSpamStatus - triggered from RunSmsCampaign above
// the moment a send comes back with PEER_FLOOD, since that error means
// the *account*, not the recipient, hit Telegram's own limit.
void CheckAccountSpamStatus(
		not_null<Main::Account*> account,
		Fn<void(QString reason)> feedback) {
	if (!Main::AccountIsLive(account)) {
		if (feedback) {
			feedback(u"нет активной сессии"_q);
		}
		return;
	}
	account->wake();
	const auto session = &account->session();
	const auto userId = session->user()->id.value;
	ResolveUsernameForAccount(account, u"spambot"_q, [=](
			PeerData *peer,
			QString error) {
		if (!peer) {
			if (feedback) {
				feedback(u"не удалось найти @spambot"_q
					+ (error.isEmpty() ? QString() : (u": "_q + error)));
			}
			return;
		}
		const auto settled = std::make_shared<bool>(false);
		const auto lifetime = std::make_shared<rpl::lifetime>();
		const auto finish = [=](const QString &replyText) {
			if (std::exchange(*settled, true)) {
				return;
			}
			lifetime->destroy();
			if (replyText.isEmpty()) {
				return;
			} else if (const auto parsed = ParseSpamBotReply(replyText)) {
				Data::SetAccountSpamStatus(userId, *parsed);
			} else {
				Core::GgtgLog::Info(
					u"Спам-статус"_q,
					u"непонятный ответ @SpamBot, статус не изменён: "_q
						+ replyText.left(200));
			}
		};
		session->changes().messageUpdates(
			Data::MessageUpdate::Flag::NewAdded
		) | rpl::filter([=](const Data::MessageUpdate &update) {
			return !update.item->out()
				&& (update.item->history()->peer == peer);
		}) | rpl::on_next([=](const Data::MessageUpdate &update) {
			finish(update.item->originalText().text);
		}, *lifetime);
		base::call_delayed(
			kSpamBotReplyTimeout,
			session,
			[=] { finish(QString()); });
		SendBotStart(account, peer, [=](
				bool success,
				QString message) {
			if (!success && feedback && !std::exchange(*settled, true)) {
				lifetime->destroy();
				feedback(u"не удалось отправить /start"_q
					+ (message.isEmpty() ? QString() : (u": "_q + message)));
			}
		});
	});
}

void TrackSpamBotReplies(not_null<Main::Session*> session) {
	// What @SpamBot answers is taken as the account's status however the
	// question was asked - by the program or by hand in the chat.
	const auto userId = session->user()->id.value;
	session->data().newItemAdded(
	) | rpl::on_next([=](not_null<HistoryItem*> item) {
		if (item->out()
			|| item->isService()
			|| base::unixtime::now() - item->date() > 600) {
			return;
		}
		const auto user = item->history()->peer->asUser();
		if (!user
			|| !user->isBot()
			|| user->username().compare(u"spambot"_q, Qt::CaseInsensitive)) {
			return;
		}
		if (const auto parsed = ParseSpamBotReply(
				item->originalText().text)) {
			Data::SetAccountSpamStatus(userId, *parsed);
			Core::GgtgLog::Info(
				u"Спам-статус"_q,
				u"ответ @SpamBot принят: статус аккаунта обновлён"_q);
		}
	}, session->lifetime());
}

rpl::producer<SmsCampaignIndicator> SmsCampaignIndicatorValue() {
	return IndicatorState().value();
}

rpl::producer<QString> SmsCampaignProgressValue() {
	return ProgressState().value();
}

QString RenderSmsTemplateForPeer(
		const QString &textTemplate,
		not_null<PeerData*> peer) {
	const auto self = peer->session().user();
	const auto user = peer->asUser();
	return RenderSmsTemplateText(
		textTemplate,
		user ? user->firstName : QString(),
		user ? user->lastName : QString(),
		self->firstName,
		self->lastName);
}

void OpenSmsCampaign(
		not_null<Window::SessionController*> controller,
		uint64 presetBaseId,
		std::shared_ptr<Ui::Show> backTo) {
	if (const auto state = CurrentCampaign()) {
		if (backTo) {
			backTo->hideLayer();
		}
		BuildCampaignProgressBox(controller->uiShow(), state);
		if (presetBaseId) {
			Ggtg::Notice(controller->uiShow(), 
				u"Рассылка уже идёт - завершите её, чтобы запустить новую"_q);
		}
		return;
	}
	// An interrupted campaign (see the SmsJob written by RunSmsCampaign) is
	// offered for resuming - unless the user came here to start a fresh one
	// for a specific base.
	if (!presetBaseId) {
		if (const auto job = Data::LoadSmsJob()) {
			auto accounts = Accounts();
			for (const auto id : job->accountUserIds) {
				for (const auto &account
						: Core::App().domain().orderedAccounts()) {
					if (account->sessionExists()
						&& account->session().user()->id.value == id) {
						accounts.push_back(account);
						break;
					}
				}
			}
			const auto stats = Data::ComputeBaseStats(job->usernames);
			if (accounts.empty() || stats.remaining == 0) {
				// Nothing left to do, or none of its accounts are here
				// any more - a stale record, drop it quietly.
				Data::ClearSmsJob();
			} else {
				const auto saved = *job;
				controller->uiShow()->show(Ui::MakeConfirmBox({
					.text = u"Найдена незавершённая рассылка: получателей "_q
						+ QString::number(saved.usernames.size())
						+ u", осталось "_q + QString::number(stats.remaining)
						+ u", аккаунтов "_q + QString::number(accounts.size())
						+ u". Продолжить её?"_q,
					.confirmed = [=](Fn<void()> &&close) {
						close();
						auto baseOf = base::flat_map<QString, uint64>();
						for (const auto &[username, baseId] : saved.baseOf) {
							baseOf.emplace(username, baseId);
						}
						RunSmsCampaign(
							controller,
							accounts,
							saved.usernames,
							saved.templates,
							saved.perAccountQuota,
							saved.threadCount,
							saved.spamTolerance,
							saved.refusalLimit,
							true, // warmup is always on now
							saved.greetingSticker,
							crl::time(saved.pauseMinSeconds) * 1000,
							crl::time(saved.pauseMaxSeconds) * 1000,
							crl::time(saved.pauseAccountMinSeconds) * 1000,
							crl::time(saved.pauseAccountMaxSeconds) * 1000,
							std::move(baseOf),
							saved.protectionMode,
							saved.nightSleep,
							saved.simulateTypos,
							saved.filterByGgr,
							saved.skipSuspectedFakes);
					},
					.cancelled = [=](Fn<void()> &&close) {
						close();
						Data::ClearSmsJob();
						controller->show(MakeSmsCampaignSetupBox(controller));
					},
					.confirmText = u"Продолжить"_q,
					.cancelText = u"Настроить заново"_q,
					.strictCancel = true,
				}));
				return;
			}
		}
	}
	if (backTo) {
		backTo->showBox(
			MakeSmsCampaignSetupBox(controller, presetBaseId, true),
			Ui::LayerOption::KeepOther);
		return;
	}
	controller->show(MakeSmsCampaignSetupBox(controller, presetBaseId));
}

object_ptr<Ui::BoxContent> MakeSmsCampaignSetupBoxFresh(
		not_null<Window::SessionController*> controller,
		uint64 presetBaseId) {
	return MakeSmsCampaignSetupBox(controller, presetBaseId);
}

// ---- helpers shared by the campaign and the stats ----

namespace {

[[nodiscard]] int SeenBucket(not_null<UserData*> user) {
	const auto status = user->lastseen();
	const auto now = base::unixtime::now();
	if (status.isOnline(now) || status.isRecently()) {
		return 1;
	} else if (status.isWithinWeek()) {
		return 2;
	} else if (status.isWithinMonth()) {
		return 3;
	} else if (status.isLongAgo()) {
		return 4;
	} else if (const auto till = status.onlineTill()) {
		const auto age = now - till;
		// Only Telegram's own «был(а) давно» counts as long ago: an exact
		// last-seen time, however old, is not.
		return (age < 3 * 86400)
			? 1
			: (age < 7 * 86400)
			? 2
			: 3;
	}
	return 0;
}

[[nodiscard]] QString LanguageLabel(const QString &code) {
	if (code.isEmpty()) {
		return u"не определён"_q;
	} else if (code == u"lat"_q) {
		return u"латиница"_q;
	}
	return Core::GgtgAi::LanguageByCode(code).russian;
}

// Did the person write back, and what did they say first? Judged by what
// this app already has of that chat on the account that wrote (no requests).
// Nothing - no answer; an empty text - there is one, but its words are not
// at hand.
[[nodiscard]] std::optional<QString> FindReplyText(
		const Core::GgtgSmsStats::Attempt &a,
		const base::flat_map<uint64, Main::Account*> &accountOf) {
	if (const auto i = accountOf.find(a.senderId); i != accountOf.end()) {
		const auto account = i->second;
		const auto &data = account->session().data();
		const auto peer = data.peerByUsername(a.recipient);
		if (!peer) {
			return std::nullopt;
		}
		const auto history = data.historyLoaded(peer->id);
		if (!history) {
			return std::nullopt;
		}
		for (const auto &block : history->blocks) {
			for (const auto &view : block->messages) {
				const auto item = view->data();
				if (!item->out() && !item->isService()) {
					const auto text = item->originalText().text.simplified();
					return text.isEmpty()
						? u"[вложение или стикер]"_q
						: text;
				}
			}
		}
		if (history->unreadCount() > 0) {
			return QString();
		}
		if (const auto last = history->lastMessage()) {
			if (!last->out() && !last->isService()) {
				return QString();
			}
		}
		return std::nullopt;
	}
	return std::nullopt;
}

// Who answered, what the answer was about (known from earlier sorting) and
// what the user marked as a success. The first words of each answer are kept
// for the sorting, so they outlive the chat itself.
void FillReplies(std::vector<Core::GgtgSmsStats::Attempt> &attempts) {
	namespace Feedback = Core::GgtgAi::Feedback;
	auto replies = Feedback::LoadReplies();
	const auto successes = Feedback::LoadSuccesses();
	auto changed = false;
	// One pass over the accounts instead of one per attempt.
	auto accountOf = base::flat_map<uint64, Main::Account*>();
	for (const auto &[index, account] : Core::App().domain().accounts()) {
		if (Main::AccountIsLive(account.get())) {
			accountOf.emplace(
				account->session().user()->id.value,
				account.get());
		}
	}
	for (auto &a : attempts) {
		if (a.outcome != u"sent"_q) {
			continue;
		}
		const auto key = Feedback::ReplyKey(a.senderId, a.recipient);
		const auto found = FindReplyText(a, accountOf);
		const auto known = replies.find(key);
		const auto saved = (known != replies.end()
			&& !known->second.text.isEmpty());
		a.replied = found.has_value() || saved;
		if (found && !found->isEmpty() && !saved) {
			auto &reply = replies[key];
			reply.text = found->left(300);
			reply.at = base::unixtime::now();
			changed = true;
		}
		if (successes.contains(a.recipient.toLower())) {
			a.replied = true;
			a.replyKind = u"success"_q;
		} else if (a.replied) {
			const auto i = replies.find(key);
			if (i != replies.end()) {
				a.replyKind = i->second.kind;
			}
		}
	}
	if (changed) {
		Feedback::SaveReplies(replies);
	}
}

void ShowSmsStatsBox(std::shared_ptr<Ui::Show> show) {
	show->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"Разбор рассылок"_q));
		box->setWidth(st::boxWideWidth);
		auto attempts = Core::GgtgSmsStats::Load();
		FillReplies(attempts);
		const auto report = Core::GgtgSmsStats::Report(
			attempts,
			[](QString code) { return LanguageLabel(code); },
			[](QString iso) {
				const auto name = iso.isEmpty()
					? QString()
					: Countries::Instance().countryNameByISO2(iso);
				return name.isEmpty() ? u"не определена"_q : name;
			});
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			u"Всё, что записано о каждой отправке: страна номера аккаунта, "
				"Premium у вас и у получателя, язык имени, когда человек "
				"был в сети, шаблон, перевод, стикер, что ушло, а что нет, "
				"и кто ответил (по уже загруженным перепискам)."_q,
			st::boxDividerLabel));
		// Answers sorted by meaning (interest, a question, a refusal,
		// hostility): the share of good answers, not just of answers, is
		// what a mailing is judged by.
		const auto unsorted = Core::GgtgAi::Feedback::UnsortedReplies();
		if (!unsorted.empty()) {
			const auto sorting = std::make_shared<bool>(false);
			const auto sort = box->addRow(
				object_ptr<Ui::LinkButton>(
					box,
					u"Определить типы ответов (ИИ): "_q
						+ QString::number(unsorted.size()) + u" шт."_q),
				st::boxRowPadding);
			sort->setClickedCallback([=] {
				if (std::exchange(*sorting, true)) {
					return;
				}
				Ggtg::Notice(box->uiShow(), 
					u"ИИ читает ответы и определяет, интерес это, вопрос "
					"или отказ..."_q);
				Core::GgtgAi::Feedback::ClassifyReplies(
					unsorted,
					crl::guard(box, [=](int sorted, QString error) {
						*sorting = false;
						if (!error.isEmpty() && !sorted) {
							Ggtg::Notice(box->uiShow(), 
								u"Не вышло: "_q + error);
							return;
						}
						const auto show = box->uiShow();
						box->closeBox();
						ShowSmsStatsBox(show);
					}));
			});
		}
		if (report.isEmpty()) {
			box->addRow(object_ptr<Ggtg::EmptyState>(
				box,
				u"Пока нет данных: они копятся с каждой рассылкой."_q));
		} else {
			// The top of the text report (totals, answer types) stays as a
			// few lines; the groups below it become cards.
			const auto splitAt = report.indexOf(u"\n\n"_q);
			const auto head = box->addRow(object_ptr<Ui::FlatLabel>(
				box,
				(splitAt > 0) ? report.left(splitAt) : report,
				st::boxLabel));
			head->setSelectable(true);
			const auto sections = Core::GgtgSmsStats::Sections(
				attempts,
				[](QString code) { return LanguageLabel(code); },
				[](QString iso) {
					const auto name = iso.isEmpty()
						? QString()
						: Countries::Instance().countryNameByISO2(iso);
					return name.isEmpty() ? u"не определена"_q : name;
				});
			using Core::GgtgSmsStats::details::Percent;
			for (const auto &section : sections) {
				if (section.rows.empty()) {
					continue;
				}
				Ui::AddSubsectionTitle(
					box->verticalLayout(),
					rpl::single(section.title));
				for (const auto &[key, b] : section.rows) {
					box->addRow(
						object_ptr<Ggtg::InfoRow>(
							box,
							rpl::single(Ggtg::InfoRowData{
								.title = key.isEmpty() ? u"?"_q : key,
								.subtitle = u"попыток "_q
									+ QString::number(b.total)
									+ u" · ушло "_q + Percent(b.sent, b.total)
									+ u" · отказ Telegram "_q
									+ QString::number(b.spam)
									+ u" · ответили "_q
									+ Percent(b.replied, b.sent),
								.chip = Percent(b.positive, b.sent),
								.chipGood = b.positive > 0,
							})),
						st::boxRowPadding);
				}
			}
			const auto errorsAt = report.indexOf(u"Ошибки:\n"_q);
			if (errorsAt >= 0) {
				const auto errors = box->addRow(object_ptr<Ui::FlatLabel>(
					box,
					report.mid(errorsAt),
					st::boxLabel));
				errors->setSelectable(true);
			}
		}
		const auto insights = box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			[] {
				const auto saved = Core::GgtgSmsStats::LoadInsights();
				return saved.isEmpty()
					? QString()
					: (u"Прошлые выводы ИИ:\n"_q + saved);
			}(),
			st::boxLabel));
		insights->setSelectable(true);
		const auto busy = std::make_shared<bool>(false);
		box->addButton(rpl::single(u"Выводы ИИ"_q), [=] {
			if (report.isEmpty()) {
				Ggtg::Notice(box->uiShow(), u"Пока нечего разбирать"_q);
				return;
			} else if (std::exchange(*busy, true)) {
				return;
			}
			insights->setText(u"ИИ разбирает статистику..."_q);
			Core::GgtgAi::Ask(
				u"Ты аналитик рассылок личных сообщений в Telegram. Тебе "
				"дают статистику отправок. Дай 5-8 коротких конкретных "
				"выводов и советов по-русски, списком через «- »: что "
				"повышает доставку и ответы, что ведёт к отказам Telegram (PEER_FLOOD) и спам-блоку "
				"аккаунтов, какие страны номеров, аккаунты, языки, время, "
				"шаблоны, перевод и стикер работают лучше или хуже. "
				"Опирайся только на цифры. Если в группе меньше 10 попыток, "
				"так и скажи, что данных мало. Без вступлений."_q,
				report.left(12000),
				crl::guard(box, [=](QString result, QString error) {
					*busy = false;
					if (!error.isEmpty()) {
						insights->setText(u"Не вышло: "_q + error);
						return;
					}
					Core::GgtgSmsStats::SaveInsights(result);
					insights->setText(u"Выводы ИИ (запомнены, помощник "
						"текста их учитывает):\n"_q + result.trimmed());
				}),
				0.3,
				900);
		});
		box->addButton(rpl::single(u"Закрыть"_q), [=] { box->closeBox(); });
		box->addLeftButton(rpl::single(u"Копировать отчёт"_q), [=] {
			QGuiApplication::clipboard()->setText(report);
			Ggtg::Notice(box->uiShow(), u"Отчёт скопирован"_q);
		});
		box->addLeftButton(rpl::single(u"Очистить"_q), [=] {
			box->uiShow()->show(Ui::MakeConfirmBox({
				.text = u"Удалить всю статистику рассылок и выводы ИИ?"_q,
				.confirmed = [=](Fn<void()> &&close) {
					close();
					Core::GgtgSmsStats::Clear();
					Core::GgtgSmsStats::SaveInsights(QString());
					box->closeBox();
				},
				.confirmText = u"Удалить"_q,
			}));
		});
	}));
}

} // namespace

void OpenSmsStats(std::shared_ptr<Ui::Show> show) {
	ShowSmsStatsBox(std::move(show));
}

QString SmsStatsReportText(bool lastRunOnly) {
	auto attempts = Core::GgtgSmsStats::Load();
	if (attempts.empty()) {
		return QString();
	}
	if (lastRunOnly) {
		auto last = qint64(0);
		for (const auto &attempt : attempts) {
			last = std::max(last, attempt.run);
		}
		attempts.erase(
			std::remove_if(attempts.begin(), attempts.end(), [=](
					const Core::GgtgSmsStats::Attempt &attempt) {
				return attempt.run != last;
			}),
			attempts.end());
	}
	FillReplies(attempts);
	auto result = Core::GgtgSmsStats::Report(
		attempts,
		[](QString code) { return LanguageLabel(code); },
		[](QString iso) {
			const auto name = iso.isEmpty()
				? QString()
				: Countries::Instance().countryNameByISO2(iso);
			return name.isEmpty() ? u"не определена"_q : name;
		});
	if (lastRunOnly && !result.isEmpty()) {
		result = u"(Только последняя рассылка, запущена "_q
			+ QDateTime::fromSecsSinceEpoch(attempts.front().run).toString(
				u"dd.MM.yyyy HH:mm"_q)
			+ u")\n"_q + result;
	}
	const auto insights = Core::GgtgSmsStats::LoadInsights();
	if (!lastRunOnly && !insights.isEmpty()) {
		result += u"\n\nВыводы прошлого разбора рассылок:\n"_q + insights;
	}
	return result;
}

QString SmsLastRunSummary() {
	auto attempts = Core::GgtgSmsStats::Load();
	if (attempts.empty()) {
		return QString();
	}
	auto last = qint64(0);
	for (const auto &a : attempts) {
		last = std::max(last, a.run);
	}
	attempts.erase(
		std::remove_if(attempts.begin(), attempts.end(), [&](const auto &a) {
			return a.run != last;
		}),
		attempts.end());
	FillReplies(attempts);
	auto sent = 0, replied = 0, positive = 0;
	for (const auto &a : attempts) {
		if (a.outcome != u"sent"_q) {
			continue;
		}
		++sent;
		if (a.replied) {
			++replied;
			if (Core::GgtgSmsStats::IsPositiveKind(a.replyKind)) {
				++positive;
			}
		}
	}
	return QDateTime::fromSecsSinceEpoch(last).toString(u"dd.MM HH:mm"_q)
		+ u": отправлено "_q + QString::number(sent)
		+ u" · ответили "_q + QString::number(replied)
		+ u" · положительных "_q + QString::number(positive);
}

namespace {

// What happened to the messages of one group (a run, a text, an account).
struct Funnel {
	SmsModel::Counts counts{};
	int replied = 0;
	int known = 0; // answers whose kind is known
	int positive = 0;

	[[nodiscard]] int sent() const {
		return counts[int(SmsModel::Outcome::Sent)];
	}
	[[nodiscard]] int attempts() const {
		return SmsModel::Total(counts);
	}
};

void AddToFunnel(Funnel &f, const Core::GgtgSmsStats::Attempt &a) {
	const auto outcome = SmsModel::FromStatsKey(a.outcome);
	if (!outcome) {
		return;
	}
	++f.counts[int(*outcome)];
	if (*outcome == SmsModel::Outcome::Sent && a.replied) {
		++f.replied;
		if (!a.replyKind.isEmpty()) {
			++f.known;
		}
		if (Core::GgtgSmsStats::IsPositiveKind(a.replyKind)) {
			++f.positive;
		}
	}
}

[[nodiscard]] QString Percent(int part, int whole) {
	return whole
		? (QString::number(int(part * 100. / whole + 0.5)) + '%')
		: u"-"_q;
}

[[nodiscard]] QString FunnelLine(const Funnel &f) {
	return u"отправлено "_q + QString::number(f.sent())
		+ u" · ответили "_q + QString::number(f.replied)
		+ u" ("_q + Percent(f.replied, f.sent()) + u") · положительных "_q
		+ QString::number(f.positive);
}

// "Определить типы ответов (ИИ): N шт." - shown while some answers are
// not sorted yet; `reload` rebuilds the screen once they are.
void AddSortRepliesLink(
		not_null<Ui::GenericBox*> box,
		Fn<void()> reload) {
	const auto unsorted = Core::GgtgAi::Feedback::UnsortedReplies();
	if (unsorted.empty()) {
		return;
	}
	const auto sorting = std::make_shared<bool>(false);
	const auto sort = box->addRow(
		object_ptr<Ui::LinkButton>(
			box,
			u"Определить типы ответов (ИИ): "_q
				+ QString::number(unsorted.size()) + u" шт."_q),
		st::boxRowPadding);
	sort->setClickedCallback([=] {
		if (std::exchange(*sorting, true)) {
			return;
		}
		Ggtg::Notice(box->uiShow(), 
			u"ИИ читает ответы и определяет, интерес это, вопрос "
			"или отказ..."_q);
		Core::GgtgAi::Feedback::ClassifyReplies(
			unsorted,
			crl::guard(box, [=](int sorted, QString error) {
				*sorting = false;
				if (!error.isEmpty() && !sorted) {
					Ggtg::Notice(box->uiShow(), u"Не вышло: "_q + error);
					return;
				}
				reload();
			}));
	});
}

} // namespace

object_ptr<Ui::BoxContent> MakeSmsResultsBox(
		not_null<Window::SessionController*> controller) {
	return MakeSmsResultsBoxStandalone();
}

object_ptr<Ui::BoxContent> MakeSmsResultsBoxStandalone() {
	return Box([=](not_null<Ui::GenericBox*> box) {
		box->setWidth(st::accountsBoxWidth);
		box->setTitle(rpl::single(u"Результаты"_q));
		const auto rebuild = std::make_shared<Fn<void()>>();
		// 0 - the last run, 1 - 7 days, 2 - all time. Remembered while the
		// app runs.
		static auto period = 0;
		const auto runsShown = std::make_shared<int>(8);
		const auto slider = box->addRow(
			object_ptr<Ui::SettingsSlider>(box, st::defaultTabsSlider),
			st::boxRowPadding);
		slider->addSection(u"Последняя"_q);
		slider->addSection(u"7 дней"_q);
		slider->addSection(u"Всё время"_q);
		slider->setActiveSectionFast(period);
		slider->sectionActivated(
		) | rpl::on_next([=](int section) {
			period = section;
			(*rebuild)();
		}, box->lifetime());
		AddSortRepliesLink(box, [=] { (*rebuild)(); });
		const auto wrap = box->addRow(
			object_ptr<Ui::VerticalLayout>(box),
			style::margins());
		*rebuild = [=] {
			const Ggtg::ScrollKeeper keepScroll(wrap);
			wrap->clear();
			auto attempts = Core::GgtgSmsStats::Load();
			FillReplies(attempts);
			auto lastRun = qint64(0);
			for (const auto &a : attempts) {
				lastRun = std::max(lastRun, qint64(a.run));
			}
			const auto since = qint64(base::unixtime::now()) - 7 * 86400;
			attempts.erase(std::remove_if(
				attempts.begin(),
				attempts.end(),
				[&](const Core::GgtgSmsStats::Attempt &a) {
					return (period == 0)
						? (a.run != lastRun)
						: (period == 1)
						? (a.at < since)
						: false;
				}), attempts.end());
			if (attempts.empty()) {
				wrap->add(object_ptr<Ggtg::EmptyState>(
					wrap,
					u"Данных пока нет: они появятся после первой "
					"рассылки."_q));
				return;
			}
			const auto row = [&](
					const QString &title,
					const QString &subtitle,
					const QString &chip,
					bool good) {
				wrap->add(
					object_ptr<Ggtg::InfoRow>(
						wrap,
						rpl::single(Ggtg::InfoRowData{
							.title = title,
							.subtitle = subtitle,
							.chip = chip,
							.chipGood = good,
						})),
					st::boxRowPadding);
			};
			auto total = Funnel();
			auto runs = std::map<qint64, Funnel>();
			auto texts = std::map<QString, Funnel>();
			auto accounts = std::map<QString, Funnel>();
			for (const auto &a : attempts) {
				AddToFunnel(total, a);
				AddToFunnel(runs[a.run], a);
				AddToFunnel(
					texts[a.templateText.isEmpty()
						? (u"Шаблон №"_q
							+ QString::number(a.templateIndex + 1)
							+ u" (старые записи)"_q)
						: a.templateText],
					a);
				AddToFunnel(accounts[a.sender], a);
			}
			Ui::AddSkip(wrap);
			const auto tiles = wrap->add(
				object_ptr<SmsWidgets::OutcomeTiles>(wrap, false),
				st::boxRowPadding);
			tiles->setCounts(total.counts);
			wrap->add(
				object_ptr<Ui::FlatLabel>(
					wrap,
					FunnelLine(total),
					st::boxDividerLabel),
				st::boxRowPadding);

			Ui::AddSubsectionTitle(wrap, rpl::single(u"Рассылки"_q));
			auto shown = 0;
			for (auto i = runs.rbegin();
					i != runs.rend() && shown < *runsShown;
					++i) {
				++shown;
				row(
					QDateTime::fromSecsSinceEpoch(i->first).toString(
						u"dd.MM.yyyy HH:mm"_q),
					FunnelLine(i->second),
					Percent(i->second.positive, i->second.sent()),
					i->second.positive > 0);
				const auto bar = wrap->add(
					object_ptr<SmsWidgets::SegmentBar>(wrap),
					style::margins(
						st::boxRowPadding.left() + style::ConvertScale(14),
						0,
						st::boxRowPadding.right() + style::ConvertScale(14),
						style::ConvertScale(8)));
				bar->setCounts(i->second.counts, 0);
			}
			if (int(runs.size()) > *runsShown) {
				const auto more = wrap->add(
					object_ptr<Ui::LinkButton>(
						wrap,
						u"Показать ещё"_q),
					st::boxRowPadding);
				more->setClickedCallback([=] {
					*runsShown = int(runs.size());
					(*rebuild)();
				});
			}

			Ui::AddSubsectionTitle(
				wrap,
				rpl::single(u"Тексты (что лучше отвечают)"_q));
			for (const auto &[text, f] : texts) {
				if (!f.sent()) {
					continue;
				}
				row(
					text.left(70) + (text.size() > 70 ? u"…"_q : QString()),
					FunnelLine(f),
					Percent(f.positive, f.sent()),
					f.positive > 0);
			}
			const auto verdict = SmsAbReportText();
			if (!verdict.isEmpty()) {
				const auto label = wrap->add(
					object_ptr<Ui::FlatLabel>(
						wrap,
						verdict,
						st::boxDividerLabel),
					st::boxRowPadding);
				label->setSelectable(true);
			}

			Ui::AddSubsectionTitle(wrap, rpl::single(u"Аккаунты"_q));
			for (const auto &[name, f] : accounts) {
				const auto refused = f.counts[int(SmsModel::Outcome::Peer)];
				const auto spam = f.counts[int(SmsModel::Outcome::Spam)];
				const auto failed = f.counts[int(SmsModel::Outcome::Fail)];
				auto subtitle = FunnelLine(f);
				if (refused) {
					subtitle += u" · отказ Telegram "_q
						+ QString::number(refused);
				}
				if (failed) {
					subtitle += u" · сбой "_q + QString::number(failed);
				}
				row(
					name,
					subtitle,
					spam
						? u"спам-блок"_q
						: SmsModel::OftenRefused(refused, f.attempts())
						? u"часто отказ"_q
						: QString(),
					false);
			}

			Ui::AddSkip(wrap);
			const auto full = wrap->add(
				object_ptr<Ui::LinkButton>(
					wrap,
					u"Полный отчёт по всем признакам"_q),
				st::boxRowPadding);
			full->setClickedCallback([=] {
				ShowSmsStatsBox(box->uiShow());
			});
			const auto ask = wrap->add(
				object_ptr<Ui::LinkButton>(
					wrap,
					u"Разобрать с ИИ-агентом..."_q),
				st::boxRowPadding);
			ask->setClickedCallback([=] {
				ShowAiAgent(box->uiShow());
			});
			Ui::AddSkip(wrap);
		};
		(*rebuild)();
	});
}

// "Предложить ответ (ИИ)": a draft answer to what the person wrote,
// in the user's own style and with what the AI knows about their business.
void ShowAiReplyDraft(
		std::shared_ptr<Ui::Show> show,
		const QString &username,
		const QString &theirText) {
	show->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"Ответ для @"_q + username));
		box->setWidth(st::boxWideWidth);
		const auto label = box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			u"ИИ пишет ответ..."_q,
			st::boxLabel));
		label->setSelectable(true);
		const auto advice = Core::GgtgAi::Feedback::SuggestedAction(
			Core::GgtgAi::Feedback::ReplyOfUsername(username).kind);
		if (!advice.isEmpty()) {
			box->addRow(
				object_ptr<Ui::FlatLabel>(box, advice, st::boxDividerLabel),
				st::boxRowPadding);
		}
		const auto draft = std::make_shared<QString>();
		box->addButton(rpl::single(u"Копировать"_q), [=] {
			if (draft->isEmpty()) {
				return;
			}
			QGuiApplication::clipboard()->setText(*draft);
			Ggtg::Notice(box->uiShow(), u"Скопировано"_q);
		}, st::defaultActiveButton);
		box->addButton(rpl::single(u"Закрыть"_q), [=] { box->closeBox(); });
		Core::GgtgAi::Profile::WarmFor(
			theirText,
			[=] {
				Core::GgtgAi::Ask(
				u"Ты помогаешь продавцу ответить человеку, который ответил на "
				"его рассылку в Telegram. Напиши короткий живой ответ "
				"(1-3 предложения) на языке собеседника, по делу, без "
				"давления; если он задал вопрос - ответь на него, если "
				"интересуется - предложи следующий шаг. Только текст ответа. "
				"Что известно о продавце и его деле:\n"_q
					+ Core::GgtgAi::Profile::Block(1500, theirText)
					+ [&] {
						const auto materials
							= Core::GgtgAi::Knowledge::Block(2500, theirText);
						return materials.isEmpty()
							? QString()
							: (u"\n\nИз его материалов (опирайся как на факты, "
								"ничего не выдумывай сверх них):\n"_q
								+ materials);
					}(),
				u"Человек написал: "_q + theirText,
				crl::guard(box, [=](QString result, QString error) {
					if (!error.isEmpty()) {
						label->setText(u"Не вышло: "_q + error);
						return;
					}
					*draft = result.trimmed();
					// Nothing is invented here: a link, name or number that is
					// neither in their message nor in what the assistant knows
					// of the seller is pointed out before it is sent.
					const auto invented = Core::GgtgAi::UngroundedFacts(
						*draft,
						theirText
							+ Core::GgtgAi::Profile::Block(4000)
							+ Core::GgtgAi::Knowledge::AllText());
					label->setText(invented.isEmpty()
						? *draft
						: (*draft + u"\n\nПроверьте, этого нет в вашем "
							"профиле и в сообщении человека: "_q
							+ invented.join(u", "_q)));
				}),
				0.6,
				400,
				Core::GgtgAi::Role::General,
				QStringList(),
				true);
			});
	}));
}

object_ptr<Ui::BoxContent> MakeSmsRepliesBox(
		not_null<Window::SessionController*> controller) {
	return Box([=](not_null<Ui::GenericBox*> box) {
		box->setWidth(st::accountsBoxWidth);
		box->setTitle(rpl::single(u"Ответы"_q));
		const auto rebuild = std::make_shared<Fn<void()>>();
		AddSortRepliesLink(box, [=] { (*rebuild)(); });
		const auto wrap = box->addRow(
			object_ptr<Ui::VerticalLayout>(box),
			style::margins());
		const auto menu = box->lifetime().make_state<
			base::unique_qptr<Ui::PopupMenu>>();
		*rebuild = [=] {
			const Ggtg::ScrollKeeper keepScroll(wrap);
			wrap->clear();
			namespace Feedback = Core::GgtgAi::Feedback;
			auto attempts = Core::GgtgSmsStats::Load();
			FillReplies(attempts);
			const auto replies = Feedback::LoadReplies();
			auto list = std::vector<Core::GgtgSmsStats::Attempt>();
			auto seen = base::flat_set<QString>();
			std::sort(attempts.begin(), attempts.end(), [](
					const auto &a,
					const auto &b) {
				return a.at > b.at;
			});
			for (const auto &a : attempts) {
				if (a.outcome == u"sent"_q
					&& a.replied
					&& seen.emplace(
						Feedback::ReplyKey(a.senderId, a.recipient)).second) {
					list.push_back(a);
				}
			}
			if (list.empty()) {
				wrap->add(object_ptr<Ggtg::EmptyState>(
					wrap,
					u"Ответов пока нет. Они подтягиваются из чатов, "
					"которые уже загружены в программе."_q));
				return;
			}
			for (const auto &a : list) {
				const auto key = Feedback::ReplyKey(a.senderId, a.recipient);
				const auto found = replies.find(key);
				auto text = (found != replies.end())
					? found->second.text.simplified()
					: QString();
				if (text.isEmpty()) {
					text = u"[текст недоступен]"_q;
				}
				const auto success = (a.replyKind == u"success"_q);
				const auto kind = a.replyKind.isEmpty()
					? u"не определён"_q
					: success
					? u"успех"_q
					: Core::GgtgSmsStats::ReplyKindTitle(a.replyKind);
				const auto username = a.recipient;
				const auto senderId = a.senderId;
				wrap->add(
					object_ptr<Ggtg::InfoRow>(
						wrap,
						rpl::single(Ggtg::InfoRowData{
							.title = u"@"_q + username,
							.subtitle = text.left(140) + u" · от "_q
								+ a.sender
								+ (Data::SmsSourceOf(a.recipient).isEmpty()
									? QString()
									: (u" · из «"_q
										+ Data::SmsSourceOf(a.recipient)
										+ u"»"_q)),
							.chip = kind,
							.chipGood = success
								|| Core::GgtgSmsStats::IsPositiveKind(
									a.replyKind),
						})),
					st::boxRowPadding
				)->setClickedCallback([=] {
					*menu = base::make_unique_q<Ui::PopupMenu>(
						box,
						st::popupMenuWithIcons);
					(*menu)->addAction(
						u"Открыть @"_q + username,
						[=] {
							// From the account that wrote: its chat is the
							// one with the conversation in it.
							for (const auto &[index, account]
									: Core::App().domain().accounts()) {
								if (Main::AccountIsLive(account.get())
									&& account->session().user()->id.value
										== senderId) {
									Core::App().domain().activate(
										account.get());
									break;
								}
							}
							base::call_delayed(700, &Core::App(), [=] {
								const auto window = Core::App().activeWindow();
								const auto now = window
									? window->sessionController()
									: nullptr;
								if (!now) {
									return;
								}
								now->showPeerByLink(Window::PeerByLinkInfo{
									.usernameOrId = username,
									.resolveType
										= Window::ResolveType::Mention,
								});
								window->activate();
							});
						},
						&st::menuIconProfile);
					(*menu)->addAction(
						u"Предложить ответ (ИИ)"_q,
						[=] {
							ShowAiReplyDraft(box->uiShow(), username, text);
						},
						&st::menuIconBot);
					(*menu)->addAction(
						success
							? u"Снять отметку «Успех»"_q
							: u"Отметить как успех"_q,
						[=] {
							Feedback::SetSuccess(username, !success);
							Ui::PostponeCall(box, [=] { (*rebuild)(); });
						},
						&st::menuIconSelect);
					(*menu)->popup(QCursor::pos());
				});
			}
		};
		(*rebuild)();
	});
}

QString SmsAbReportText() {
	constexpr auto kNeed = 30; // sent per text for a verdict
	auto attempts = Core::GgtgSmsStats::Load();
	FillReplies(attempts);
	struct Group {
		int sent = 0;
		int replied = 0;
		int known = 0;
		int positive = 0;
		qint64 last = 0;
	};
	auto groups = std::map<QString, Group>();
	for (const auto &a : attempts) {
		if (a.outcome != u"sent"_q || a.templateText.isEmpty()) {
			continue;
		}
		auto &group = groups[a.templateText];
		++group.sent;
		group.last = std::max(group.last, a.at);
		if (a.replied) {
			++group.replied;
			if (!a.replyKind.isEmpty()) {
				++group.known;
				if (Core::GgtgSmsStats::IsPositiveKind(a.replyKind)) {
					++group.positive;
				}
			}
		}
	}
	if (groups.size() < 2) {
		return u"Для сравнения нужно хотя бы два разных текста, по которым "
			"уже были отправки. Сейчас такой текст один или записей с "
			"текстами ещё нет (они копятся с рассылок после обновления). "
			"Чтобы проверить два варианта, включите «Чередовать несколько "
			"шаблонов» в «Рассылке»."_q;
	}
	// Good answers are compared when most answers are sorted; otherwise any
	// answer counts and the report says so.
	auto sorted = 0, answers = 0;
	for (const auto &[text, group] : groups) {
		sorted += group.known;
		answers += group.replied;
	}
	const auto byPositive = answers && (sorted * 2 >= answers);
	auto result = byPositive
		? u"Сравнение по положительным ответам (интерес, вопросы, успех).\n"_q
		: u"Типы ответов определены не у большинства, поэтому сравнение по "
			"любым ответам (это грубо: отказ тоже ответ). Лучше сначала "
			"нажать «Определить типы ответов» в «Разборе рассылок».\n"_q;
	auto best = QString();
	auto bestRate = -1.;
	auto bestGroup = Group();
	auto enough = true;
	auto rows = QString();
	for (const auto &[text, group] : groups) {
		const auto good = byPositive ? group.positive : group.replied;
		const auto rate = group.sent ? (double(good) / group.sent) : 0.;
		rows += u"«"_q + text + u"»: ушло "_q + QString::number(group.sent)
			+ (byPositive
				? (u", положительных "_q + QString::number(good))
				: (u", ответили "_q + QString::number(good)))
			+ u" ("_q + QString::number(int(rate * 100 + 0.5)) + u"%)"_q
			+ (group.sent < kNeed
				? (u" - мало данных, нужно ещё около "_q
					+ QString::number(kNeed - group.sent))
				: QString())
			+ '\n';
		enough = enough && (group.sent >= kNeed);
		if (rate > bestRate) {
			bestRate = rate;
			best = text;
			bestGroup = group;
		}
	}
	result += rows;
	if (!enough) {
		return result + u"Вывода пока нет: на каждый текст нужно минимум "_q
			+ QString::number(kNeed) + u" отправок, иначе разницу не "
			"отличить от случайности."_q;
	}
	// The leader against the runner-up: is the gap bigger than luck?
	auto second = -1.;
	auto secondGroup = Group();
	for (const auto &[text, group] : groups) {
		if (text == best) {
			continue;
		}
		const auto good = byPositive ? group.positive : group.replied;
		const auto rate = group.sent ? (double(good) / group.sent) : 0.;
		if (rate > second) {
			second = rate;
			secondGroup = group;
		}
	}
	const auto goodA = byPositive ? bestGroup.positive : bestGroup.replied;
	const auto goodB = byPositive ? secondGroup.positive : secondGroup.replied;
	const auto n1 = double(bestGroup.sent);
	const auto n2 = double(secondGroup.sent);
	const auto pooled = double(goodA + goodB) / (n1 + n2);
	const auto error = std::sqrt(pooled * (1. - pooled) * (1. / n1 + 1. / n2));
	const auto z = error > 0. ? ((bestRate - second) / error) : 0.;
	if (std::abs(z) >= 1.96) {
		result += u"Лидер: «"_q + best + u"». Разница достоверна (вряд ли "
			"случайность)."_q;
	} else {
		result += u"Лидер пока «"_q + best + u"», но разница может быть "
			"случайной (ещё не достоверна): отправок мало или тексты почти "
			"равны."_q;
	}
	return result;
}

} // namespace Settings
