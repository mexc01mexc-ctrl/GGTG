/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtCore/QString>

#include <optional>
#include <vector>

namespace Data {

// Saved, reusable "Отправка SMS" message texts (see
// settings/sms_campaign.cpp's SetupTextSection), kept in the same
// "tdata/ggtg_db/" directory as the bases/sent-tracking (see
// scraped_members_db.h) - the local equivalent of tg-web-panel's own
// per-owner "sms-prompts" list, minus the owner id (this app only ever
// has one local user).
struct SmsPrompt {
	uint64 id = 0;
	QString name;
	QString text;
};

[[nodiscard]] std::vector<SmsPrompt> LoadSmsPrompts();

// Creates a new prompt or overwrites an existing one in place (same
// `id`, new name/text).
void SaveSmsPrompt(uint64 id, const QString &name, const QString &text);
void DeleteSmsPrompt(uint64 id);
void SaveAllSmsPrompts(const std::vector<SmsPrompt> &prompts);

// A fresh id for a new prompt - collisions are astronomically unlikely
// for a personal list of a few dozen entries, so unlike
// scraped_members_db.h's GenerateManualBaseId this doesn't need to dodge
// any other id space.
[[nodiscard]] uint64 GenerateSmsPromptId();

// The "Отправка SMS" setup box's last-used values, so hiding or closing it
// (or restarting the app) doesn't reset the accounts, limits, behavior
// toggles and text to their defaults. Field values are kept as the raw
// text the user typed (the box parses them with its own fallbacks).
struct SmsFormSettings {
	bool saved = false; // false: nothing stored yet, use defaults
	std::vector<uint64> accountUserIds;
	std::vector<uint64> selectedBaseIds;
	// Every base id that existed when the form was last saved - a base not
	// in here is new since then and defaults to selected, while one that is
	// in here but missing from selectedBaseIds was deliberately unchecked.
	std::vector<uint64> knownBaseIds;
	QString limitPerAccount;
	QString limitThreads;
	QString limitSkipSpamBlock;
	QString pauseMin;
	QString pauseMax;
	// "Пауза между аккаунтами": the cooldown before a thread picks up the
	// next account once the current one has sent its per-account quota.
	QString pauseAccountMin;
	QString pauseAccountMax;
	// "Снимать аккаунт после отказов" (empty: the preset's value).
	QString limitRefusals;
	// The "Режим" picked last: "normal", "premium" or "custom".
	QString limitMode;
	// Saved prompts (see SmsPrompt) the campaign rotates through instead of
	// the message field; empty = rotation off.
	std::vector<uint64> rotationPromptIds;
	// ИИ-Защита аккаунтов: 0 = Консервативный, 1 = Оптимальный, 2 = Скоростной
	int protectionMode = 1;
	bool nightSleep = true;
	bool simulateTypos = true;
	bool filterByGgr = false;
	bool skipSuspectedFakes = true;
	bool warmup = false;
	bool greetingSticker = false;
	QString text;
};

[[nodiscard]] SmsFormSettings LoadSmsForm();
void SaveSmsForm(const SmsFormSettings &form);

// Everything needed to start a campaign again - written when one starts,
// removed once it has fully finished, so an interrupted one (quit, crash,
// stopped by hand, accounts ran out) can be resumed. What each recipient
// already got lives in the send registry (see scraped_members_db.h), so
// resuming is just running the same recipients again: finished ones are
// skipped by the campaign's own pre-pass.
struct SmsJob {
	QStringList usernames;
	// Lowercase username -> the base it came from (see SmsSendRecord).
	std::vector<std::pair<QString, uint64>> baseOf;
	QStringList templates;
	std::vector<uint64> accountUserIds;
	int perAccountQuota = 1;
	int threadCount = 1;
	int spamTolerance = 0;
	// Telegram's refusals in a row after which the account is taken off
	// the campaign (0: never for that reason).
	int refusalLimit = 3;
	int protectionMode = 1;
	bool nightSleep = true;
	bool simulateTypos = true;
	bool filterByGgr = false;
	bool skipSuspectedFakes = true;
	bool warmup = false;
	bool greetingSticker = false;
	int pauseMinSeconds = 40;
	int pauseMaxSeconds = 60;
	int pauseAccountMinSeconds = 60;
	int pauseAccountMaxSeconds = 90;
};

[[nodiscard]] std::optional<SmsJob> LoadSmsJob();
void SaveSmsJob(const SmsJob &job);
void ClearSmsJob();

} // namespace Data
