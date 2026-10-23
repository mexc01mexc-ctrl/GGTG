/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/object_ptr.h"

#include <QtGui/QColor>

class PeerData;

namespace Main {
class Account;
class Session;
} // namespace Main

namespace Ui {
class BoxContent;
class Show;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

namespace Settings {

enum class SmsCampaignIndicator {
	None,
	Running,
	Done,
};

// Drives the "Отправка SMS" menu row's spinner/checkmark suffix (see
// window_main_menu.cpp): Running while a campaign's send loop is active,
// Done once every lane has settled and nobody has looked at the result
// yet (closing the reopened progress box while Done clears it back to
// None), None otherwise.
[[nodiscard]] rpl::producer<SmsCampaignIndicator> SmsCampaignIndicatorValue();

// "processed/total" (e.g. "10/15") while a campaign is running, updated
// live as each recipient settles - see RunSmsCampaign's updateSummary.
// Empty whenever SmsCampaignIndicatorValue() is None.
[[nodiscard]] rpl::producer<QString> SmsCampaignProgressValue();

// Opens the "Отправка SMS" flow as a normal in-app box - the same
// show/hide mechanism as every other Settings box, no separate OS
// window. If a campaign is currently running or just finished, this
// reopens its live progress box instead of the setup box, picking up
// exactly where the last view of it left off (see CurrentCampaign() in
// sms_campaign.cpp) - that's the "hide and reopen" half of the flow;
// "Закрыть" on either box only hides it (Ui::Show::hideLayer), it never
// stops a running campaign.
//
// `presetBaseId`, when non-zero, opens the setup box with only that base
// checked ("Отправить SMS по этой базе" in "База"); ignored when a campaign
// is already running, since that reopens its progress box instead.
//
// `backTo`, when given (opened from a base), shows the setup box on top of
// that place with "Назад" back to it instead of replacing what is there.
void OpenSmsCampaign(
	not_null<Window::SessionController*> controller,
	uint64 presetBaseId = 0,
	std::shared_ptr<Ui::Show> backTo = nullptr);

// "Разбор рассылок": totals of every SMS attempt (country of the sending
// number, Premium, language, time, template, translation, sticker, what
// went and what did not, who wrote back) and the AI's conclusions from them.
void OpenSmsStats(std::shared_ptr<Ui::Show> show);

// The same totals as "Разбор рассылок" as plain text (who replied is read
// from the chats), plus the AI's last conclusions - for the "ИИ-агент".
// Empty when nothing was sent yet.
// With `lastRunOnly`, only the newest mailing is counted.
[[nodiscard]] QString SmsStatsReportText(bool lastRunOnly = false);
// One line about the latest run for the home screen (empty without data).
[[nodiscard]] QString SmsLastRunSummary();

// The "Результаты" and "Ответы" pages of the "Рассылка" section.
[[nodiscard]] object_ptr<Ui::BoxContent> MakeSmsResultsBox(
	not_null<Window::SessionController*> controller);

// The same screen without a controller: the finished progress window
// opens it.
[[nodiscard]] object_ptr<Ui::BoxContent> MakeSmsResultsBoxStandalone();
[[nodiscard]] object_ptr<Ui::BoxContent> MakeSmsRepliesBox(
	not_null<Window::SessionController*> controller);

// The texts that were in the rotation side by side, compared: how many each
// sent, how many answered well, who leads and whether the difference is
// real or may be luck (needs about 30 sent per text). For the "ИИ-агент".
[[nodiscard]] QString SmsAbReportText();

// The fresh setup box on its own, skipping OpenSmsCampaign's "reopen the
// running campaign's progress instead" branch - for a context that always
// wants the setup form specifically (see window_main_menu.cpp's "open in
// a floating window" - a currently-running campaign's own progress box
// still opens the normal, in-app way from there).
[[nodiscard]] object_ptr<Ui::BoxContent> MakeSmsCampaignSetupBoxFresh(
	not_null<Window::SessionController*> controller,
	uint64 presetBaseId = 0);

// Messages @spambot with "/start" from this account and, once (or if) it
// replies, saves what its reply says about the account's own anti-spam
// limit (see data/account_spam_status.h) - the same check the campaign
// send loop triggers automatically the moment a send comes back with
// PEER_FLOOD, exposed here too for a manual "check now" re-check (see the
// clickable status badge and "Проверить..." buttons in the "Отправка SMS"
// box's accounts accordion, sms_campaign.cpp).
//
// `feedback`, when given, reports the one specific way this attempt failed
// before ever getting a reply - resolving @spambot failed, or sending
// "/start" to it failed - so a user-initiated check can show *why* nothing
// happened instead of just going quiet. Left null for the automatic
// PEER_FLOOD-triggered call, where that would just be unexplained noise
// mid-campaign.
void CheckAccountSpamStatus(
	not_null<Main::Account*> account,
	Fn<void(QString reason)> feedback = nullptr);

// The short display text ("Статус"/"Свободен"/"Блок до dd.mm hh:mm"/
// "Вечный блок") and matching color for an account's current SPAM-status
// (data/account_spam_status.h) - shared by the "Отправка SMS" accounts
// list's clickable badge and the compact hint next to the name in the
// main account switcher (settings/sections/settings_information.cpp), so
// the two always agree instead of drifting into different wording or
// colors.
[[nodiscard]] QString AccountSpamStatusText(uint64 userId);
[[nodiscard]] QColor AccountSpamStatusColor(uint64 userId);

// GGR (GramGPT / GGTG Rating) - 1.0 to 10.0 account health and trust index
struct AccountGgrInfo {
	float score = 1.0f;
	QString tier;
};
[[nodiscard]] AccountGgrInfo CalculateAccountGgr(not_null<Main::Account*> account);

// Auto-appeals to @SpamBot for temporary/accidental limits.
void AutoAppealSpamBot(
	not_null<Main::Account*> account,
	Fn<void(bool success, QString message)> done = nullptr);

// Takes every answer of @SpamBot in this session's chat as the account's
// status - also when the user wrote to the bot by hand. Called once per
// session when it is created.
void TrackSpamBotReplies(not_null<Main::Session*> session);

// A saved "Отправка SMS" template rendered the way a campaign would for this
// chat's peer - {a|b} picks one variant, [NAME]/[SURNAME] are the peer's,
// [MYNAME]/[MYSURNAME] the account's own, [NAME|fallback] falls back when a
// name is empty - for inserting into the message field of that chat.
[[nodiscard]] QString RenderSmsTemplateForPeer(
	const QString &textTemplate,
	not_null<PeerData*> peer);

} // namespace Settings
