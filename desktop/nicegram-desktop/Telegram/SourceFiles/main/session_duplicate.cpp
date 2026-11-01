/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "main/session_duplicate.h"

#include "base/weak_ptr.h"
#include "apiwrap.h"
#include "main/main_account.h"
#include "main/main_session.h"
#include "data/data_user.h"
#include "core/application.h"
#include "core/core_cloud_password.h"
#include "mtproto/mtp_instance.h"
#include "mtproto/mtproto_config.h"
#include "core/ggtg_api_override.h"
#include "mtproto/mtproto_proxy_data.h"
#include "mtproto/proxy_pool.h"
#include "mtproto/sender.h"
#include "base/platform/base_platform_info.h"
#include "settings/bulk_accounts_actions.h"
#include "settings/ggtg_android_profiles.h"
#include "settings/ggtg_ios_profiles.h"
#include "core/ggtg_sessions.h"
#include "core/ggtg_geo.h"
#include "base/unixtime.h"
#include "config.h"

#include "data/data_session.h"
#include "data/data_peer.h"
#include "history/history.h"
#include "history/history_item.h"
#include "rpl/filter.h"

#include <QtCore/QTimer>
#include <QtCore/QRegularExpression>

namespace Main {
namespace {

// How many times to re-poll auth.exportLoginToken on the throwaway side
// after auth.acceptLoginToken has already succeeded on the real account's
// side, waiting for the server to reflect that - the accept call's own
// response already confirms the server processed it before we ever start
// polling, so this is a small safety margin against propagation lag, not
// a real wait loop (intro_qr.cpp's own equivalent is push-driven, but that
// needs a UI-less throwaway instance's update stream wired up for no real
// benefit here - a few short retries covers the same case).
constexpr auto kMaxRetriesAfterAccept = 5;
constexpr auto kRetryDelayMs = 300;

// GGTG Android's own BuildVars.APP_ID / APP_HASH and version name (code)
// (ggtg-Android/Telegram-GGTG), sent for sessions minted for that app.
// Keep the version equal to APP_VERSION_NAME (APP_VERSION_CODE) in
// ggtg-Android/Telegram-GGTG/gradle.properties.
constexpr auto kAndroidApiId = 4;
constexpr auto kAndroidApiHash = "014b35b6184100b085b0d0572f9b5103";
// ggtg: a real Android build string is versionName (versionCode * 10 + ABI digit), e.g. 71129 for
// 12.10.6; the bare 7112 made the session list show an impossible build.
constexpr auto kAndroidAppVersion = "12.10.6 (71129)";

struct DuplicateContext {
	// base::weak_ptr<>, not a raw/not_null<> pointer - Account derives from
	// base::has_weak_ptr specifically so code like this can hold a
	// reference across an async gap without risking a use-after-free if
	// the account is logged out/deleted or the app starts quitting while
	// a duplication is still in flight (see AGENTS.md's own warning about
	// exactly this pattern with api().request() callbacks). Every use
	// below re-checks it rather than assuming it's still alive.
	base::weak_ptr<Account> account;
	UserId userId;
	Fn<void(Fn<void(QString password)> submit, Fn<void()> cancel)>
		requestPassword;
	Fn<void(QString error, QByteArray serializedAuth)> done;
	std::unique_ptr<MTP::Instance> instance;
	std::unique_ptr<MTP::Sender> throwawaySender;
	std::unique_ptr<MTP::Sender> realSender;
	QByteArray token;
	bool accepted = false;
	int retriesAfterAccept = 0;
	bool finished = false;
	int exportApiId = 0;
	QString exportApiHash;

	// For Android code-based duplication:
	bool asAndroid = false;
	QString phoneDigits;
	QString phoneCodeHash;
	QString pendingCode;
	bool codeCaptured = false;
	int64 codeStartTime = 0;
	rpl::lifetime codeLifetime;
};

void ExportToken(const std::shared_ptr<DuplicateContext> &ctx);
void HandleExportResult(
	const std::shared_ptr<DuplicateContext> &ctx,
	const MTPauth_LoginToken &result);
void StartAndroidCodeDuplication(const std::shared_ptr<DuplicateContext> &ctx);
void PollCode(const std::weak_ptr<DuplicateContext> &weakCtx, int attempt);
void OnCodeReceived(const std::shared_ptr<DuplicateContext> &ctx, const QString &code);
void SignInWithCode(const std::shared_ptr<DuplicateContext> &ctx, const QString &code);

[[nodiscard]] QString DigitsOnly(QString value) {
	static const auto RegExp = QRegularExpression(u"[^0-9]"_q);
	return value.replace(RegExp, QString());
}

void Finish(
		const std::shared_ptr<DuplicateContext> &ctx,
		QString error,
		QByteArray serializedAuth) {
	if (ctx->finished) {
		return;
	}
	ctx->finished = true;
	ctx->codeLifetime.destroy();
	// Direct reset, not deferred - Account::destroyMtpKeys() in
	// main_account.cpp already tears down its own second, parallel
	// MTP::Instance this same way (`_mtpForKeysDestroy = nullptr;`) from
	// inside one of that instance's own emitted events, so this is a
	// established-safe pattern for this specific type in this codebase.
	ctx->throwawaySender = nullptr;
	ctx->realSender = nullptr;
	ctx->instance = nullptr;
	const auto done = std::exchange(ctx->done, nullptr);
	if (done) {
		done(error, serializedAuth);
	}
}

void FinishWithAuthorization(const std::shared_ptr<DuplicateContext> &ctx) {
	const auto serialized = SerializeMtpAuthorization(
		ctx->userId,
		ctx->instance->mainDcId(),
		ctx->instance->getKeysForWrite(),
		{});
	Finish(ctx, QString(), serialized);
}

void RequestPasswordInteractively(const std::shared_ptr<DuplicateContext> &ctx);

// Tries one specific password (whatever was already stored, or one the
// person operating this app just typed via RequestPasswordInteractively()
// below) against the throwaway instance. A wrong password surfaces the
// same way a missing one does - MTPauth_CheckPassword's own failure (wrong
// SRP proof) or ComputeCloudPasswordCheck() itself refusing to build one -
// both fall through to asking interactively instead of giving up outright,
// so a typo doesn't need restarting the whole export.
void AttemptPassword(
		const std::shared_ptr<DuplicateContext> &ctx,
		const QString &password) {
	ctx->throwawaySender->request(MTPaccount_GetPassword(
	)).done([=](const MTPaccount_Password &result) {
		result.match([&](const MTPDaccount_password &data) {
			const auto state = Core::ParseCloudPasswordState(data);
			if (!state.mtp.request) {
				Finish(ctx, u"Не удалось получить параметры пароля."_q, {});
				return;
			}
			const auto passwordUtf8 = password.toUtf8();
			const auto hash = Core::ComputeCloudPasswordHash(
				state.mtp.request.algo,
				bytes::make_span(passwordUtf8));
			const auto check = Core::ComputeCloudPasswordCheck(
				state.mtp.request,
				hash);
			if (!check) {
				// A local computation failure (e.g. an algorithm this
				// build can't handle) - a different password wouldn't fix
				// it, so this isn't a "try again" case.
				Finish(ctx, u"Не удалось проверить пароль 2FA."_q, {});
				return;
			}
			ctx->throwawaySender->request(MTPauth_CheckPassword(
				check.result
			)).done([=](const MTPauth_Authorization &result) {
				// The password just proved correct - keep it with the
				// source account, however it got here (typed by hand
				// included), so it lands in the export's 2fa.txt and any
				// later run doesn't have to ask again.
				if (const auto source = ctx->account.get()) {
					if (source->twoFactorPassword() != password) {
						source->setTwoFactorPassword(password);
					}
				}
				result.match([&](const MTPDauth_authorization &auth) {
					auth.vuser().match([&](const MTPDuser &user) {
						ctx->userId = UserId(user.vid().v);
					}, [](const auto &) {});
				}, [](const auto &) {});
				FinishWithAuthorization(ctx);
			}).fail([=](const MTP::Error &error) {
				// PASSWORD_HASH_INVALID and similar - the password itself
				// was wrong, worth asking again. Anything else (FLOOD_WAIT,
				// a network hiccup, ...) looping back to the same prompt
				// would just repeat the identical failure with no visible
				// cause, so surface it as a real error instead.
				if (error.type().startsWith(u"PASSWORD"_q)) {
					RequestPasswordInteractively(ctx);
				} else {
					Finish(ctx, u"checkPassword: "_q + error.type(), {});
				}
			}).send();
		});
	}).fail([=](const MTP::Error &error) {
		Finish(ctx, u"getPassword: "_q + error.type(), {});
	}).send();
}

void RequestPasswordInteractively(const std::shared_ptr<DuplicateContext> &ctx) {
	if (!ctx->requestPassword) {
		Finish(ctx, u"У аккаунта включён пароль 2FA, а сохранённого "
			u"локально пароля нет - задайте его через \"Пароль 2FA\" и "
			u"повторите."_q, {});
		return;
	}
	ctx->requestPassword([ctx](QString password) {
		if (!ctx->finished) {
			AttemptPassword(ctx, password);
		}
	}, [ctx] {
		Finish(ctx, u"Пропущено: нужен пароль 2FA."_q, {});
	});
}

void StartPasswordCheck(const std::shared_ptr<DuplicateContext> &ctx) {
	const auto account = ctx->account.get();
	if (!account) {
		Finish(ctx, u"Исходный аккаунт больше не доступен."_q, {});
		return;
	}
	auto stored = account->twoFactorPassword();
	if (stored.isEmpty() && account->sessionExists()) {
		const auto rawId = account->session().userId().bare;
		for (const auto &s : Core::GgtgSessions::StoredForUser(Core::GgtgSessions::LoadVault(), QString::number(rawId))) {
			if (!s.twoFactor.isEmpty()) {
				stored = s.twoFactor;
				account->setTwoFactorPassword(stored);
				break;
			}
		}
	}
	if (stored.isEmpty()) {
		RequestPasswordInteractively(ctx);
		return;
	}
	AttemptPassword(ctx, stored);
}

// Shared by ExportToken() and MigrateAndRetry() below - both are export/
// poll-style calls on the throwaway side, and either can be the one that
// finds out the account needs its password confirmed (SESSION_PASSWORD_
// NEEDED only ever shows up once accept has already gone through - see
// this function's own accepted-guard).
void HandleExportOrImportFailure(
		const std::shared_ptr<DuplicateContext> &ctx,
		const QString &label,
		const MTP::Error &error) {
	if (ctx->accepted && error.type() == u"SESSION_PASSWORD_NEEDED"_q) {
		StartPasswordCheck(ctx);
		return;
	}
	Finish(ctx, label + u": "_q + error.type(), {});
}

void MigrateAndRetry(
		const std::shared_ptr<DuplicateContext> &ctx,
		MTP::DcId dcId,
		const QByteArray &token) {
	// Mirrors Intro::details::QrWidget::importTo() (intro_qr.cpp) - despite
	// the name, this is the *unauthenticated* side resending its own token
	// against the DC the server says it actually belongs on, not a real
	// "import" of someone else's login.
	ctx->instance->setMainDcId(dcId);
	ctx->throwawaySender->request(MTPauth_ImportLoginToken(
		MTP_bytes(token)
	)).done([=](const MTPauth_LoginToken &result) {
		HandleExportResult(ctx, result);
	}).fail([=](const MTP::Error &error) {
		HandleExportOrImportFailure(ctx, u"importLoginToken"_q, error);
	}).toDC(dcId).send();
}

void AcceptOnRealAccount(const std::shared_ptr<DuplicateContext> &ctx) {
	if (!ctx->account) {
		Finish(ctx, u"Исходный аккаунт больше не доступен."_q, {});
		return;
	}
	// auth.acceptLoginToken - the "phone scans the QR" role, played here by
	// the account's own already-authorized session instead of a second
	// device. No existing call site in this codebase to model the DC
	// routing on (see session_duplicate.h's own comment) - sent over the
	// account's normal/default connection rather than pinned to any
	// particular DC, since it's the token bytes themselves (already tied
	// to a DC server-side) doing the addressing, not this call. ctx->
	// realSender itself still wraps that account's Instance directly
	// (built once, back in DuplicateSession() while it was definitely
	// alive) - the null-check just above only rules out firing a *new*
	// request against an account already known to be gone by now; it
	// can't protect this specific in-flight request if the account dies
	// *while* it's outstanding, since Instance teardown mid-request isn't
	// something this codebase has an existing pattern to guard against
	// here (see session_duplicate.h's own risk note).
	ctx->realSender->request(MTPauth_AcceptLoginToken(
		MTP_bytes(ctx->token)
	)).done([=](const MTPAuthorization &) {
		ctx->accepted = true;
		ExportToken(ctx);
	}).fail([=](const MTP::Error &error) {
		Finish(ctx, u"acceptLoginToken: "_q + error.type(), {});
	}).send();
}

void HandleExportResult(
		const std::shared_ptr<DuplicateContext> &ctx,
		const MTPauth_LoginToken &result) {
	result.match([&](const MTPDauth_loginToken &data) {
		if (!ctx->accepted) {
			ctx->token = data.vtoken().v;
			AcceptOnRealAccount(ctx);
			return;
		}
		// Accepted already, but this re-poll still says "not yet" - the
		// server hasn't caught up. Short bounded retry, not a real wait
		// loop - see kMaxRetriesAfterAccept's own comment.
		if (ctx->retriesAfterAccept >= kMaxRetriesAfterAccept) {
			Finish(ctx, u"Сервер не подтвердил принятие сессии вовремя."_q,
				{});
			return;
		}
		++ctx->retriesAfterAccept;
		QTimer::singleShot(kRetryDelayMs, [ctx] {
			if (!ctx->finished) {
				ExportToken(ctx);
			}
		});
	}, [&](const MTPDauth_loginTokenMigrateTo &data) {
		MigrateAndRetry(ctx, data.vdc_id().v, data.vtoken().v);
	}, [&](const MTPDauth_loginTokenSuccess &) {
		FinishWithAuthorization(ctx);
	});
}

void ExportToken(const std::shared_ptr<DuplicateContext> &ctx) {
	ctx->throwawaySender->request(MTPauth_ExportLoginToken(
		MTP_int(ctx->exportApiId),
		MTP_string(ctx->exportApiHash),
		MTP_vector<MTPlong>(0)
	)).done([=](const MTPauth_LoginToken &result) {
		HandleExportResult(ctx, result);
	}).fail([=](const MTP::Error &error) {
		HandleExportOrImportFailure(ctx, u"exportLoginToken"_q, error);
	}).send();
}

void SignInWithCode(const std::shared_ptr<DuplicateContext> &ctx, const QString &code) {
	if (ctx->finished) {
		return;
	}
	ctx->throwawaySender->request(MTPauth_SignIn(
		MTP_flags(MTPauth_SignIn::Flag::f_phone_code),
		MTP_string(ctx->phoneDigits),
		MTP_string(ctx->phoneCodeHash),
		MTP_string(code),
		MTP_emailVerificationCode(MTPstring())
	)).done([ctx](const MTPauth_Authorization &result) {
		result.match([&](const MTPDauth_authorization &auth) {
			auth.vuser().match([&](const MTPDuser &user) {
				ctx->userId = UserId(user.vid().v);
			}, [](const auto &) {});
		}, [](const auto &) {});
		FinishWithAuthorization(ctx);
	}).fail([ctx](const MTP::Error &error) {
		if (error.type() == u"SESSION_PASSWORD_NEEDED"_q) {
			StartPasswordCheck(ctx);
			return;
		}
		Finish(ctx, u"signIn: "_q + error.type(), {});
	}).send();
}

void OnCodeReceived(const std::shared_ptr<DuplicateContext> &ctx, const QString &code) {
	if (ctx->finished || ctx->codeCaptured) {
		return;
	}
	ctx->codeCaptured = true;
	ctx->codeLifetime.destroy();

	if (ctx->phoneCodeHash.isEmpty()) {
		ctx->pendingCode = code;
		return;
	}
	SignInWithCode(ctx, code);
}

void PollCode(const std::weak_ptr<DuplicateContext> &weakCtx, int attempt) {
	const auto ctx = weakCtx.lock();
	if (!ctx || ctx->finished || ctx->codeCaptured) {
		return;
	}
	if (attempt >= 40) { // ~60 seconds timeout
		if (!ctx->phoneCodeHash.isEmpty()) {
			ctx->throwawaySender->request(MTPauth_CancelCode(
				MTP_string(ctx->phoneDigits),
				MTP_string(ctx->phoneCodeHash)
			)).send();
		}
		Finish(ctx, u"Таймаут ожидания кода подтверждения в Telegram (60 сек)."_q, {});
		return;
	}
	const auto account = ctx->account.get();
	if (!account || !account->sessionExists()) {
		Finish(ctx, u"Исходный аккаунт больше не доступен."_q, {});
		return;
	}

	const auto peer = account->session().data().peer(PeerData::kServiceNotificationsId);
	const auto inputPeer = peer ? peer->input() : MTP_inputPeerUser(MTP_long(777000), MTP_long(0));

	account->session().api().request(MTPmessages_GetHistory(
		inputPeer,
		MTP_int(0), // offset_id
		MTP_int(0), // offset_date
		MTP_int(0), // add_offset
		MTP_int(10), // limit
		MTP_int(0), // max_id
		MTP_int(0), // min_id
		MTP_long(0) // hash
	)).done([weakCtx, attempt](const MTPmessages_Messages &result) {
		const auto ctx = weakCtx.lock();
		if (!ctx || ctx->finished || ctx->codeCaptured) {
			return;
		}
		static const auto CodePattern = QRegularExpression(u"(?<!\\d)(\\d{5,6})(?!\\d)"_q);
		auto foundCode = QString();
		result.match([&](const MTPDmessages_messagesNotModified &) {
		}, [&](const auto &data) {
			for (const auto &message : data.vmessages().v) {
				message.match([&](const MTPDmessage &msg) {
					if (msg.vdate().v >= ctx->codeStartTime - 5 && !msg.is_out()) {
						const auto text = qs(msg.vmessage());
						const auto match = CodePattern.match(text);
						if (match.hasMatch()) {
							foundCode = match.captured(1);
						}
					}
				}, [](const auto &) {});
				if (!foundCode.isEmpty()) break;
			}
		});
		if (!foundCode.isEmpty()) {
			OnCodeReceived(ctx, foundCode);
		} else {
			QTimer::singleShot(1500, [weakCtx, attempt] {
				PollCode(weakCtx, attempt + 1);
			});
		}
	}).fail([weakCtx, attempt](const MTP::Error &) {
		const auto ctx = weakCtx.lock();
		if (ctx && !ctx->finished && !ctx->codeCaptured) {
			QTimer::singleShot(1500, [weakCtx, attempt] {
				PollCode(weakCtx, attempt + 1);
			});
		}
	}).send();
}

void StartAndroidCodeDuplication(const std::shared_ptr<DuplicateContext> &ctx) {
	const auto account = ctx->account.get();
	if (!account || !account->sessionExists()) {
		Finish(ctx, u"Исходный аккаунт не в сети."_q, {});
		return;
	}
	if (ctx->phoneDigits.isEmpty()) {
		Finish(ctx, u"У аккаунта отсутствует номер телефона."_q, {});
		return;
	}

	ctx->codeStartTime = base::unixtime::now();

	// 1. Hook incoming messages on the active account in real-time
	static const auto CodePattern = QRegularExpression(u"(?<!\\d)(\\d{5,6})(?!\\d)"_q);

	const auto weakCtx = std::weak_ptr<DuplicateContext>(ctx);
	account->session().data().newItemAdded(
	) | rpl::filter([weakCtx](not_null<HistoryItem*> item) {
		const auto ctx = weakCtx.lock();
		if (!ctx || ctx->finished || ctx->codeCaptured) {
			return false;
		}
		return (item->history()->peer->id == PeerData::kServiceNotificationsId)
			&& (item->date() >= ctx->codeStartTime - 5)
			&& !item->out();
	}) | rpl::on_next([weakCtx](not_null<HistoryItem*> item) {
		const auto ctx = weakCtx.lock();
		if (!ctx || ctx->finished || ctx->codeCaptured) {
			return;
		}
		static const auto CodePattern = QRegularExpression(u"(?<!\\d)(\\d{5,6})(?!\\d)"_q);
		const auto text = item->originalText().text;
		const auto match = CodePattern.match(text);
		if (match.hasMatch()) {
			OnCodeReceived(ctx, match.captured(1));
		}
	}, ctx->codeLifetime);

	// 2. Also check if the history already has a matching message (just in case)
	if (const auto history = account->session().data().history(PeerData::kServiceNotificationsId)) {
		if (const auto last = history->lastMessage()) {
			if (!last->out() && last->date() >= ctx->codeStartTime - 2) {
				const auto match = CodePattern.match(last->originalText().text);
				if (match.hasMatch()) {
					OnCodeReceived(ctx, match.captured(1));
				}
			}
		}
	}

	// 3. Send MTPauth_SendCode
	ctx->throwawaySender->request(MTPauth_SendCode(
		MTP_string(ctx->phoneDigits),
		MTP_int(ctx->exportApiId),
		MTP_string(ctx->exportApiHash),
		MTP_codeSettings(
			MTP_flags(0),
			MTPVector<MTPbytes>(),
			MTPstring(),
			MTPBool())
	)).done([weakCtx](const MTPauth_SentCode &result) {
		const auto ctx = weakCtx.lock();
		if (!ctx || ctx->finished) {
			return;
		}
		result.match([&](const MTPDauth_sentCode &data) {
			const auto isApp = (data.vtype().type() == mtpc_auth_sentCodeTypeApp);
			if (!isApp) {
				ctx->throwawaySender->request(MTPauth_CancelCode(
					MTP_string(ctx->phoneDigits),
					data.vphone_code_hash()
				)).send();
				Finish(ctx, u"Telegram отправил код не в приложение (SMS). Для создания Android-сессии требуется подтверждение в приложении."_q, {});
				return;
			}
			ctx->phoneCodeHash = qs(data.vphone_code_hash());
			if (!ctx->pendingCode.isEmpty()) {
				const auto code = std::exchange(ctx->pendingCode, QString());
				SignInWithCode(ctx, code);
			} else {
				PollCode(weakCtx, 1);
			}
		}, [&](const MTPDauth_sentCodeSuccess &) {
			FinishWithAuthorization(ctx);
		}, [&](const auto &) {
			Finish(ctx, u"Не удалось запросить код подтверждения для Android."_q, {});
		});
	}).fail([weakCtx](const MTP::Error &error) {
		const auto ctx = weakCtx.lock();
		if (ctx && !ctx->finished) {
			Finish(ctx, u"sendCode: "_q + error.type(), {});
		}
	}).send();
}

} // namespace

void DuplicateSession(
		not_null<Account*> account,
		Fn<void(Fn<void(QString password)> submit, Fn<void()> cancel)>
			requestPassword,
		Fn<void(QString error, QByteArray serializedAuth)> done,
		std::optional<MTP::DeviceOverride> device,
		bool asAndroid) {
	Expects(done != nullptr);

	if (!account->sessionExists()) {
		done(u"Аккаунт не в сети - дублировать нечего."_q, {});
		return;
	}

	if (!MTP::HasRealProxy(account->proxyOverride())) {
		done(u"У аккаунта не задан прокси - без него он не подключается."_q, {});
		return;
	}

	auto fields = MTP::Instance::Fields();
	fields.mainDcId = MTP::Instance::Fields::kNotSetMainDc;
	fields.config = std::make_unique<MTP::Config>(
		Core::App().fallbackProductionConfig());
	fields.deviceModel = Platform::DeviceModelPretty();
	fields.systemVersion = Platform::SystemVersionPretty();
	const auto phone = (account->sessionExists() && account->session().user())
		? account->session().user()->phone()
		: QString();
	if (!device) {
		fields.deviceOverride = asAndroid
			? Settings::RandomAndroidDeviceOverride(phone)
			: Settings::RandomDeviceOverrideForAccount(account);
	} else {
		fields.deviceOverride = *device;
	}
	if (!phone.isEmpty()) {
		const auto geo = Core::GgtgGeo::MatchForPhone(phone);
		fields.deviceOverride.systemLangCode = geo.systemLang;
		fields.deviceOverride.cloudLangCode = geo.cloudLang;
		fields.deviceOverride.tzOffset = geo.tzOffset;
	}
	const auto isIos = Settings::IsIosSession(fields.deviceOverride);
	const auto isAndroid = asAndroid || Settings::IsAndroidSession(fields.deviceOverride);
	const auto own = Core::GgtgApiOverride::Load();
	const auto exportApiId = own.valid()
		? own.apiId
		: isIos
		? Ggtg::IosProfiles::kAppId
		: isAndroid
		? Ggtg::AndroidProfiles::kAppId
		: ApiId;
	const auto exportApiHash = own.valid()
		? own.apiHash
		: isIos
		? QString::fromLatin1(Ggtg::IosProfiles::kAppHash)
		: isAndroid
		? QString::fromLatin1(Ggtg::AndroidProfiles::kAppHash)
		: QString::fromLatin1(ApiHash);
	const auto exportLangPack = isIos
		? QString::fromLatin1(Ggtg::IosProfiles::kLangPack)
		: isAndroid
		? QString::fromLatin1(Ggtg::AndroidProfiles::kLangPack)
		: QString::fromLatin1(Ggtg::DesktopProfiles::kLangPack);

	if (isIos) {
		fields.deviceModel = fields.deviceOverride.deviceModel;
		fields.systemVersion = fields.deviceOverride.systemVersion;
		fields.appVersion = !fields.deviceOverride.appVersion.isEmpty()
			? fields.deviceOverride.appVersion
			: Ggtg::IosProfiles::RandomVersion();
	} else if (isAndroid) {
		fields.deviceModel = fields.deviceOverride.deviceModel;
		fields.systemVersion = fields.deviceOverride.systemVersion;
		fields.appVersion = !fields.deviceOverride.appVersion.isEmpty()
			? fields.deviceOverride.appVersion
			: Ggtg::AndroidProfiles::RandomBuild();
	} else {
		fields.appVersion = !fields.deviceOverride.appVersion.isEmpty()
			? fields.deviceOverride.appVersion
			: Ggtg::DesktopProfiles::RandomDesktopVersion();
	}
	fields.apiId = exportApiId;
	fields.apiHash = exportApiHash;
	fields.deviceOverride.apiId = exportApiId;
	fields.deviceOverride.apiHash = exportApiHash;
	fields.deviceOverride.langPackName = exportLangPack;
	if (account->sessionExists()) {
		fields.mainDcId = account->session().mainDcId();
	}
	// The throwaway instance's own connection, not just the blob it ends
	// up producing - this account should never suddenly make even one
	// request from this machine's raw IP mid-duplication (see this
	// function's own header comment).
	const auto proxy = account->proxyOverride();
	if (proxy.type != MTP::ProxyData::Type::None) {
		fields.proxyOverride = proxy;
	}

	auto ctx = std::make_shared<DuplicateContext>();
	ctx->account = account;
	ctx->userId = account->session().userId();
	ctx->requestPassword = std::move(requestPassword);
	ctx->done = std::move(done);
	ctx->exportApiId = exportApiId;
	ctx->exportApiHash = exportApiHash;
	ctx->asAndroid = isAndroid;
	ctx->phoneDigits = DigitsOnly(phone);
	ctx->instance = std::make_unique<MTP::Instance>(
		MTP::Instance::Mode::Normal,
		std::move(fields));
	ctx->throwawaySender = std::make_unique<MTP::Sender>(ctx->instance.get());
	ctx->realSender = std::make_unique<MTP::Sender>(&account->mtp());

	if (isAndroid) {
		StartAndroidCodeDuplication(ctx);
	} else {
		ExportToken(ctx);
	}
}

void FetchSessionHashes(
		not_null<Account*> account,
		Fn<void(std::vector<uint64> hashes, QString error)> done) {
	if (!account->sessionExists()) {
		done({}, u"Аккаунт не в сети."_q);
		return;
	}
	const auto weak = base::make_weak(account.get());
	const auto sender = static_cast<MTP::Sender*>(&account->session().api());
	sender->request(MTPaccount_GetAuthorizations(
	)).done([=](const MTPaccount_Authorizations &result) {
		(void)sender;
		auto hashes = std::vector<uint64>();
		for (const auto &auth : result.data().vauthorizations().v) {
			hashes.push_back(uint64(auth.data().vhash().v));
		}
		if (weak) {
			done(std::move(hashes), QString());
		}
	}).fail([=](const MTP::Error &error) {
		(void)sender;
		if (weak) {
			done({}, error.type());
		}
	}).send();
}

void FetchSessionDescription(
		not_null<Account*> account,
		uint64 hash,
		Fn<void(QString description)> done) {
	if (!hash || !account->sessionExists()) {
		done(QString());
		return;
	}
	const auto weak = base::make_weak(account.get());
	const auto sender = static_cast<MTP::Sender*>(&account->session().api());
	sender->request(MTPaccount_GetAuthorizations(
	)).done([=](const MTPaccount_Authorizations &result) {
		(void)sender;
		auto text = QString();
		for (const auto &auth : result.data().vauthorizations().v) {
			const auto &data = auth.data();
			if (uint64(data.vhash().v) == hash) {
				text = qs(data.vdevice_model()) + u", "_q
					+ qs(data.vapp_name()) + ' ' + qs(data.vapp_version())
					+ u", "_q + qs(data.vsystem_version());
				break;
			}
		}
		if (weak) {
			done(text);
		}
	}).fail([=](const MTP::Error &) {
		(void)sender;
		if (weak) {
			done(QString());
		}
	}).send();
}

void DuplicateSessionTracked(
		not_null<Account*> account,
		Fn<void(Fn<void(QString password)> submit, Fn<void()> cancel)>
			requestPassword,
		Fn<void(QString error, QByteArray serializedAuth, uint64 hash)> done,
		std::optional<MTP::DeviceOverride> device,
		bool asAndroid) {
	Expects(done != nullptr);

	const auto weak = base::make_weak(account.get());
	FetchSessionHashes(account, [=](
			std::vector<uint64> before,
			QString beforeError) {
		if (!weak) {
			done(u"Аккаунт больше не доступен."_q, {}, 0);
			return;
		}
		// If the list could not be read the duplicate is still made, just
		// without its number.
		const auto known = beforeError.isEmpty();
		DuplicateSession(weak.get(), requestPassword, [=](
				QString error,
				QByteArray serialized) {
			if (!error.isEmpty() || !weak || !known) {
				done(error, serialized, 0);
				return;
			}
			const auto checkAfter = std::make_shared<Fn<void(int)>>();
			*checkAfter = [=](int attempt) {
				if (!weak) {
					done(u"Аккаунт больше не доступен."_q, serialized, 0);
					return;
				}
				FetchSessionHashes(weak.get(), [=](
						std::vector<uint64> after,
						QString afterError) {
					auto found = uint64(0);
					if (afterError.isEmpty()) {
						auto fresh = std::vector<uint64>();
						for (const auto hash : after) {
							if (hash && !ranges::contains(before, hash)) {
								fresh.push_back(hash);
							}
						}
						if (fresh.size() == 1) {
							found = fresh.front();
						}
					}
					if (found != 0 || attempt >= 3 || !weak) {
						done(QString(), serialized, found);
					} else {
						QTimer::singleShot(600 * attempt, [=] {
							if (weak) {
								(*checkAfter)(attempt + 1);
							} else {
								done(QString(), serialized, 0);
							}
						});
					}
				});
			};
			QTimer::singleShot(400, [=] {
				if (weak) {
					(*checkAfter)(1);
				} else {
					done(QString(), serialized, 0);
				}
			});
		}, device, asAndroid);
	});
}

} // namespace Main
