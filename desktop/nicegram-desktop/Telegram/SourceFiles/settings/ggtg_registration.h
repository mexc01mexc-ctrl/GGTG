// GGTG: Auto-registration of Telegram accounts via SMS and Email activation services (Hero-SMS, SMSBower, SMS-Activate, 5SIM, Vak-SMS, SMSHub, Grizzly SMS) with Android mobile emulation (Base.db 3000 profiles, TokenFast, AntiSafety), 2FA vault, and session duplication/Android export.
/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtWidgets/QTextEdit>
#include <QtNetwork/QTcpSocket>
#include <QRandomGenerator>
#include "apiwrap.h"
#include "api/api_cloud_password.h"
#include "boxes/connection_box.h"
#include "core/application.h"
#include "core/file_utilities.h"
#include "core/ggtg_api_override.h"
#include "core/ggtg_geo.h"
#include "core/ggtg_paths.h"
#include "core/ggtg_log.h"
#include "core/ggtg_sessions.h"
#include "data/data_user.h"
#include "lang/lang_keys.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "main/main_session_settings.h"
#include "main/session_duplicate.h"
#include "mtproto/facade.h"
#include "mtproto/mtproto_proxy_data.h"
#include "mtproto/proxy_pool.h"
#include "mtproto/sender.h"
#include "settings/bulk_accounts_actions.h"
#include "settings/ggtg_android_profiles.h"
#include "settings/ggtg_export.h"
#include "storage/localstorage.h"
#include "storage/storage_tdata_export.h"
#include "ui/boxes/confirm_box.h"
#include "ui/layers/generic_box.h"
#include "ui/layers/show.h"
#include "ui/text/text_entity.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "window/ggtg_click.h"
#include "window/ggtg_widgets.h"
#include "window/ggtg_home_widgets.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_widgets.h"
#include "config.h"

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QPointer>
#include <QtCore/QRegularExpression>
#include <QtCore/QSaveFile>
#include <QtCore/QStandardPaths>
#include <QtCore/QTextStream>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtGui/QDesktopServices>
#include <QtCore/QUrlQuery>
#include <QtGui/QClipboard>
#include <QtGui/QDesktopServices>
#include <QtGui/QGuiApplication>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>
#include <dlfcn.h>

namespace Settings {
namespace Registration {

struct ServiceInfo {
	QString id;
	QString name;
	QString baseUrl;
	QString webUrl;
	bool supportsEmail = false;
};

inline const std::vector<ServiceInfo> &Services() {
	static const auto list = std::vector<ServiceInfo>{
		{ u"hero-sms"_q, u"Hero-SMS (SMS + Email)"_q, u"https://hero-sms.com/stubs/handler_api.php"_q, u"https://hero-sms.com"_q, true },
		{ u"smsbower"_q, u"SMSBower (SMS + Email)"_q, u"https://smsbower.page/stubs/handler_api.php"_q, u"https://smsbower.app"_q, true },
		{ u"onlinesim"_q, u"OnlineSim"_q, u"https://onlinesim.io/stubs/handler_api.php"_q, u"https://onlinesim.io"_q, false },
		{ u"sms-activate"_q, u"SMS-Activate"_q, u"https://api.sms-activate.org/stubs/handler_api.php"_q, u"https://sms-activate.org"_q, false },
		{ u"5sim"_q, u"5SIM"_q, u"https://5sim.net/stubs/handler_api.php"_q, u"https://5sim.net"_q, false },
		{ u"vak-sms"_q, u"Vak-SMS"_q, u"https://vak-sms.com/stubs/handler_api.php"_q, u"https://vak-sms.com"_q, false },
		{ u"smshub"_q, u"SMSHub"_q, u"https://smshub.org/stubs/handler_api.php"_q, u"https://smshub.org"_q, false },
		{ u"grizzly"_q, u"Grizzly SMS"_q, u"https://api.grizzlysms.com/stubs/handler_api.php"_q, u"https://grizzlysms.com"_q, false },
		{ u"sms-man"_q, u"SMS-MAN"_q, u"https://api.sms-man.com/stubs/handler_api.php"_q, u"https://sms-man.com"_q, false },
		{ u"custom"_q, u"Свой сервис (API URL)"_q, QString(), QString(), false },
	};
	return list;
}

struct CountryItem {
	int id = 0;
	QString flag;
	QString name;
	QString phoneCode;
};

inline const std::vector<CountryItem> &PopularCountries() {
	static const auto list = std::vector<CountryItem>{
		{ 16, u"🇬🇧"_q, u"Великобритания"_q, u"+44"_q },
		{ 187, u"🇺🇸"_q, u"США"_q, u"+1"_q },
		{ 6, u"🇮🇩"_q, u"Индонезия"_q, u"+62"_q },
		{ 2, u"🇰🇿"_q, u"Казахстан"_q, u"+77"_q },
		{ 22, u"🇮🇳"_q, u"Индия"_q, u"+91"_q },
		{ 73, u"🇧🇷"_q, u"Бразилия"_q, u"+55"_q },
		{ 10, u"🇻🇳"_q, u"Вьетнам"_q, u"+84"_q },
		{ 19, u"🇳🇬"_q, u"Нигерия"_q, u"+234"_q },
		{ 4, u"🇵🇭"_q, u"Филиппины"_q, u"+63"_q },
		{ 11, u"🇰🇬"_q, u"Кыргызстан"_q, u"+996"_q },
		{ 35, u"🇦🇿"_q, u"Азербайджан"_q, u"+994"_q },
		{ 40, u"🇺🇿"_q, u"Узбекистан"_q, u"+998"_q },
		{ 51, u"🇧🇾"_q, u"Беларусь"_q, u"+375"_q },
		{ 1, u"🇺🇦"_q, u"Украина"_q, u"+380"_q },
		{ 143, u"🇹🇯"_q, u"Таджикистан"_q, u"+992"_q },
		{ 128, u"🇬🇪"_q, u"Грузия"_q, u"+995"_q },
		{ 148, u"🇦🇲"_q, u"Армения"_q, u"+374"_q },
		{ 17, u"🇲🇩"_q, u"Молдова"_q, u"+373"_q },
		{ 0, u"🇷🇺"_q, u"Россия"_q, u"+7"_q },
	};
	return list;
}

inline QString CountryCodeById(int id) {
	for (const auto &item : PopularCountries()) {
		if (item.id == id) {
			const auto m = Core::GgtgGeo::MatchForPhone(item.phoneCode);
			return m.country;
		}
	}
	return u"US"_q;
}

enum class MobileTokenProvider {
	None = 0,
	TokenFast = 1,
	AntiSafety = 2,
};

[[nodiscard]] inline QString TokenProviderName(int provider) {
	switch (MobileTokenProvider(provider)) {
	case MobileTokenProvider::TokenFast: return u"TokenFast (TokenFast.pro)"_q;
	case MobileTokenProvider::AntiSafety: return u"AntiSafety (AntiSafety.net)"_q;
	case MobileTokenProvider::None:
	default: return u"Отключен"_q;
	}
}

// ggtg: Captcha solving providers for reCAPTCHA Enterprise
enum class CaptchaServiceProvider {
	Auto = 0,
	TokenFast = 1,
	AntiSafety = 2,
	NextCaptcha = 3,
};

[[nodiscard]] inline QString CaptchaProviderName(int provider) {
	switch (CaptchaServiceProvider(provider)) {
	case CaptchaServiceProvider::TokenFast: return u"TokenFast (api.tokenfast.shop)"_q;
	case CaptchaServiceProvider::AntiSafety: return u"AntiSafety (antisafety.net)"_q;
	case CaptchaServiceProvider::NextCaptcha: return u"NextCaptcha (nextcaptcha.com)"_q;
	case CaptchaServiceProvider::Auto:
	default: return u"Авто (из сервиса токенов)"_q;
	}
}

struct SettingsData {
	QString serviceId = u"hero-sms"_q;
	QString apiKey;
	QString customUrl;
	int countryId = -1;
	QString countryName; // title of the chosen country as the service named it
	QMap<QString, QString> apiKeys; // the API key of each SMS service, kept between runs
	QString proxyPoolName;
	QString folderTag = u"Регистрация"_q;
	QString twoFactorPassword;
	bool single2faForAll = true;
	bool singleNameForAll = false;
	bool autoRandomNames = true;
	QString customFirstName;
	QString customLastName;
	int mode = 0; // 0 = Auto SMS, 1 = Manual SIM

	// Multi-threading and target count (Point 5)
	int targetSuccessCount = 10;
	int threadsCount = 2; // 1 to 10 parallel threads
	int stepDelayMin = 10; // seconds between registration steps
	int stepDelayMax = 30;
	int accountDelayMin = 15; // seconds between accounts in a thread
	int accountDelayMax = 45;

	// Email settings (for Telegram setup email requirements)
	bool autoBuyEmail = true;
	QString emailService = u"auto"_q; // auto, hero-sms, smsbower
	QString emailApiKey;
	QString emailDomain = u"any"_q; // always any available domain

	// How long to wait for the SMS code before the number is cancelled and refunded
	int smsWaitSeconds = 300; // 120 is the lowest allowed: a number lives ~20 min on most services

	// Skip numbers Telegram would send the code to the app for (already registered)
	bool skipAppCode = true;
	// Reject numbers that already failed before (registered / banned / invalid), before asking for a code
	bool filterNumbers = true;
	// Reserve of unused Play Integrity tokens; replace the token when the number hits a flood limit
	bool replaceTokenOnFlood = true;

	// Mobile emulation settings (Point 1 & 2)
	int clientMode = 1; // Always Mobile (Android)
	QString baseDbPath = u"2026-10-16_15-42-19_Base.db"_q;
	int tokenService = 1; // 0 = None, 1 = TokenFast, 2 = AntiSafety
	QString tokenApiKey;

	// Captcha solving settings (reCAPTCHA Enterprise)
	int captchaService = 0; // 0 = Auto, 1 = TokenFast, 2 = AntiSafety, 3 = NextCaptcha
	QString captchaApiKey; // if empty, falls back to tokenApiKey

	// SMS order options (from TGEX: operator, maxPrice)
	QString smsOperator; // empty or "any" = any operator; "megafon", "mts" etc.
	QString maxPrice; // empty = no limit; "15.00" etc.

	// Name generation options (from TGEX: names from file, gender)
	int gender = 0; // 0 = random, 1 = male, 2 = female
	QString firstNameFile; // path to .txt with first names (one per line)
	QString lastNameFile; // path to .txt with last names (one per line)

	// Post-registration actions (from TGEX)
	bool resetSessionsAfterReg = false; // call ResetAuthorizations after registration
	bool reset2faOnOccupied = false; // try ResetPassword when number has 2FA already

	// Bio and username auto-setup (TG-Giant style)
	bool autoSetBio = false;
	QString customBio;
	bool autoSetUsername = false;
	bool precheckProxy = true;

	// GGTG Android Export (.ggtg with 2FA, proxy, and device)
	bool autoExportGgtg = true;
	bool singleGgtgPack = true;
	QString ggtgExportPath;
};

[[nodiscard]] inline QString SettingsFilePath() {
	return Core::GgtgPaths::DbDir() + u"registration_settings.json"_q;
}

inline SettingsData LoadSettings() {
	auto result = SettingsData();
	auto file = QFile(SettingsFilePath());
	if (!file.open(QIODevice::ReadOnly)) {
		return result;
	}
	const auto doc = QJsonDocument::fromJson(file.readAll());
	if (!doc.isObject()) {
		return result;
	}
	const auto obj = doc.object();
	if (obj.contains(u"serviceId"_q)) result.serviceId = obj[u"serviceId"_q].toString();
	if (obj.contains(u"apiKey"_q)) result.apiKey = obj[u"apiKey"_q].toString();
	if (obj.contains(u"customUrl"_q)) result.customUrl = obj[u"customUrl"_q].toString();
	if (obj.contains(u"countryId"_q)) result.countryId = obj[u"countryId"_q].toInt();
	if (obj.contains(u"countryName"_q)) result.countryName = obj[u"countryName"_q].toString();
	if (obj.contains(u"apiKeys"_q)) {
		const auto keys = obj[u"apiKeys"_q].toObject();
		for (auto it = keys.constBegin(); it != keys.constEnd(); ++it) {
			result.apiKeys.insert(it.key(), it.value().toString());
		}
	}
	if (!result.apiKey.isEmpty() && !result.apiKeys.contains(result.serviceId)) {
		result.apiKeys.insert(result.serviceId, result.apiKey);
	}
	if (obj.contains(u"proxyPoolName"_q)) result.proxyPoolName = obj[u"proxyPoolName"_q].toString();
	if (obj.contains(u"folderTag"_q)) result.folderTag = obj[u"folderTag"_q].toString();
	if (obj.contains(u"twoFactorPassword"_q)) result.twoFactorPassword = obj[u"twoFactorPassword"_q].toString();
	if (obj.contains(u"single2faForAll"_q)) result.single2faForAll = obj[u"single2faForAll"_q].toBool();
	if (obj.contains(u"singleNameForAll"_q)) result.singleNameForAll = obj[u"singleNameForAll"_q].toBool();
	if (obj.contains(u"autoRandomNames"_q)) result.autoRandomNames = obj[u"autoRandomNames"_q].toBool();
	if (obj.contains(u"customFirstName"_q)) result.customFirstName = obj[u"customFirstName"_q].toString();
	if (obj.contains(u"customLastName"_q)) result.customLastName = obj[u"customLastName"_q].toString();
	if (obj.contains(u"mode"_q)) result.mode = obj[u"mode"_q].toInt();

	if (obj.contains(u"targetSuccessCount"_q)) result.targetSuccessCount = std::max(1, obj[u"targetSuccessCount"_q].toInt());
	if (obj.contains(u"threadsCount"_q)) result.threadsCount = std::max(1, std::min(10, obj[u"threadsCount"_q].toInt()));
	if (obj.contains(u"stepDelayMin"_q)) result.stepDelayMin = std::max(0, obj[u"stepDelayMin"_q].toInt());
	if (obj.contains(u"stepDelayMax"_q)) result.stepDelayMax = std::max(result.stepDelayMin, obj[u"stepDelayMax"_q].toInt());
	if (obj.contains(u"accountDelayMin"_q)) result.accountDelayMin = std::max(0, obj[u"accountDelayMin"_q].toInt());
	if (obj.contains(u"accountDelayMax"_q)) result.accountDelayMax = std::max(result.accountDelayMin, obj[u"accountDelayMax"_q].toInt());

	if (obj.contains(u"autoBuyEmail"_q)) result.autoBuyEmail = obj[u"autoBuyEmail"_q].toBool();
	if (obj.contains(u"emailService"_q)) result.emailService = obj[u"emailService"_q].toString();
	if (obj.contains(u"emailApiKey"_q)) result.emailApiKey = obj[u"emailApiKey"_q].toString();
	result.emailDomain = u"any"_q;
	if (obj.contains(u"skipAppCode"_q)) result.skipAppCode = obj[u"skipAppCode"_q].toBool();
	if (obj.contains(u"filterNumbers"_q)) result.filterNumbers = obj[u"filterNumbers"_q].toBool();
	if (obj.contains(u"replaceTokenOnFlood"_q)) result.replaceTokenOnFlood = obj[u"replaceTokenOnFlood"_q].toBool();
	if (obj.contains(u"smsWaitSeconds"_q)) result.smsWaitSeconds = std::clamp(obj[u"smsWaitSeconds"_q].toInt(), 120, 1800);

	if (obj.contains(u"clientMode"_q)) result.clientMode = obj[u"clientMode"_q].toInt();
	if (obj.contains(u"baseDbPath"_q)) result.baseDbPath = obj[u"baseDbPath"_q].toString();
	if (obj.contains(u"tokenService"_q)) result.tokenService = obj[u"tokenService"_q].toInt();
	if (obj.contains(u"tokenApiKey"_q)) result.tokenApiKey = obj[u"tokenApiKey"_q].toString();
	if (obj.contains(u"captchaService"_q)) result.captchaService = obj[u"captchaService"_q].toInt();
	if (obj.contains(u"captchaApiKey"_q)) result.captchaApiKey = obj[u"captchaApiKey"_q].toString();

	if (obj.contains(u"smsOperator"_q)) result.smsOperator = obj[u"smsOperator"_q].toString();
	if (obj.contains(u"maxPrice"_q)) result.maxPrice = obj[u"maxPrice"_q].toString();
	if (obj.contains(u"gender"_q)) result.gender = std::clamp(obj[u"gender"_q].toInt(), 0, 2);
	if (obj.contains(u"firstNameFile"_q)) result.firstNameFile = obj[u"firstNameFile"_q].toString();
	if (obj.contains(u"lastNameFile"_q)) result.lastNameFile = obj[u"lastNameFile"_q].toString();
	if (obj.contains(u"resetSessionsAfterReg"_q)) result.resetSessionsAfterReg = obj[u"resetSessionsAfterReg"_q].toBool();
	if (obj.contains(u"reset2faOnOccupied"_q)) result.reset2faOnOccupied = obj[u"reset2faOnOccupied"_q].toBool();

	if (obj.contains(u"autoSetBio"_q)) result.autoSetBio = obj[u"autoSetBio"_q].toBool();
	if (obj.contains(u"customBio"_q)) result.customBio = obj[u"customBio"_q].toString();
	if (obj.contains(u"autoSetUsername"_q)) result.autoSetUsername = obj[u"autoSetUsername"_q].toBool();
	if (obj.contains(u"precheckProxy"_q)) result.precheckProxy = obj[u"precheckProxy"_q].toBool();

	if (obj.contains(u"autoExportGgtg"_q)) result.autoExportGgtg = obj[u"autoExportGgtg"_q].toBool();
	if (obj.contains(u"singleGgtgPack"_q)) result.singleGgtgPack = obj[u"singleGgtgPack"_q].toBool();
	if (obj.contains(u"ggtgExportPath"_q)) result.ggtgExportPath = obj[u"ggtgExportPath"_q].toString();
	return result;
}

inline void SaveSettings(const SettingsData &data) {
	QDir().mkpath(Core::GgtgPaths::DbDir());
	auto file = QSaveFile(SettingsFilePath());
	if (!file.open(QIODevice::WriteOnly)) {
		return;
	}
	auto obj = QJsonObject();
	obj[u"serviceId"_q] = data.serviceId;
	obj[u"apiKey"_q] = data.apiKey;
	obj[u"customUrl"_q] = data.customUrl;
	obj[u"countryId"_q] = data.countryId;
	obj[u"countryName"_q] = data.countryName;
	{
		auto keys = QJsonObject();
		for (auto it = data.apiKeys.constBegin(); it != data.apiKeys.constEnd(); ++it) {
			keys[it.key()] = it.value();
		}
		obj[u"apiKeys"_q] = keys;
	}
	obj[u"proxyPoolName"_q] = data.proxyPoolName;
	obj[u"folderTag"_q] = data.folderTag;
	obj[u"twoFactorPassword"_q] = data.twoFactorPassword;
	obj[u"single2faForAll"_q] = data.single2faForAll;
	obj[u"singleNameForAll"_q] = data.singleNameForAll;
	obj[u"autoRandomNames"_q] = data.autoRandomNames;
	obj[u"customFirstName"_q] = data.customFirstName;
	obj[u"customLastName"_q] = data.customLastName;
	obj[u"mode"_q] = data.mode;

	obj[u"targetSuccessCount"_q] = data.targetSuccessCount;
	obj[u"threadsCount"_q] = data.threadsCount;
	obj[u"stepDelayMin"_q] = data.stepDelayMin;
	obj[u"stepDelayMax"_q] = data.stepDelayMax;
	obj[u"accountDelayMin"_q] = data.accountDelayMin;
	obj[u"accountDelayMax"_q] = data.accountDelayMax;

	obj[u"autoBuyEmail"_q] = data.autoBuyEmail;
	obj[u"emailService"_q] = data.emailService;
	obj[u"emailApiKey"_q] = data.emailApiKey;
	obj[u"emailDomain"_q] = data.emailDomain;
	obj[u"smsWaitSeconds"_q] = data.smsWaitSeconds;
	obj[u"skipAppCode"_q] = data.skipAppCode;
	obj[u"filterNumbers"_q] = data.filterNumbers;
	obj[u"replaceTokenOnFlood"_q] = data.replaceTokenOnFlood;

	obj[u"clientMode"_q] = data.clientMode;
	obj[u"baseDbPath"_q] = data.baseDbPath;
	obj[u"tokenService"_q] = data.tokenService;
	obj[u"tokenApiKey"_q] = data.tokenApiKey;
	obj[u"captchaService"_q] = data.captchaService;
	obj[u"captchaApiKey"_q] = data.captchaApiKey;

	obj[u"smsOperator"_q] = data.smsOperator;
	obj[u"maxPrice"_q] = data.maxPrice;
	obj[u"gender"_q] = data.gender;
	obj[u"firstNameFile"_q] = data.firstNameFile;
	obj[u"lastNameFile"_q] = data.lastNameFile;
	obj[u"resetSessionsAfterReg"_q] = data.resetSessionsAfterReg;
	obj[u"reset2faOnOccupied"_q] = data.reset2faOnOccupied;

	obj[u"autoSetBio"_q] = data.autoSetBio;
	obj[u"customBio"_q] = data.customBio;
	obj[u"autoSetUsername"_q] = data.autoSetUsername;
	obj[u"precheckProxy"_q] = data.precheckProxy;

	obj[u"autoExportGgtg"_q] = data.autoExportGgtg;
	obj[u"singleGgtgPack"_q] = data.singleGgtgPack;
	obj[u"ggtgExportPath"_q] = data.ggtgExportPath;
	file.write(QJsonDocument(obj).toJson(QJsonDocument::Indented));
	file.commit();
}

// -----------------------------------------------------------------------------
// Mobile Device Profile (Point 1 & 2 from Base.db or procedural generator)
// -----------------------------------------------------------------------------
struct MobileDeviceProfile {
	int appId = 4;
	QString appHash = u"014b35b6184100b085b0d0572f9b5103"_q;
	QString sdk = u"SDK 30"_q;
	QString device = u"XiaomiRedmi 9C"_q;
	QString appVersion = u"12.10.1 (70382)"_q;
	QString langCode = u"ru"_q;
	QString systemLangCode = u"ru-ru"_q;
	QString langPack = u"android"_q;
	int tzOffset = 10800;
	int perfCat = 2;
};

inline QJsonObject ToDeviceJson(const MobileDeviceProfile &p) {
	return QJsonObject{
		{ "deviceModel", p.device },
		{ "systemVersion", p.sdk },
		{ "appVersion", p.appVersion },
		{ "systemLangCode", p.systemLangCode },
		{ "cloudLangCode", p.langCode },
		{ "langPackName", p.langPack },
		{ "tzOffset", p.tzOffset },
	};
}

inline QString FindBaseDbPath(const QString &customPath = QString()) {
	if (!customPath.trimmed().isEmpty() && QFile::exists(customPath.trimmed())) {
		return customPath.trimmed();
	}
	const auto candidates = std::vector<QString>{
		u"/Users/macbook/Desktop/GGTG/2026-10-16_15-42-19_Base.db"_q,
		Core::GgtgPaths::DbDir() + u"2026-10-16_15-42-19_Base.db"_q,
		Core::GgtgPaths::DbDir() + u"Base.db"_q,
		QDir::currentPath() + u"/2026-10-16_15-42-19_Base.db"_q,
		QCoreApplication::applicationDirPath() + u"/../Resources/2026-10-16_15-42-19_Base.db"_q,
		QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + u"/GGTG/2026-10-16_15-42-19_Base.db"_q,
		u"2026-10-16_15-42-19_Base.db"_q,
	};
	for (const auto &c : candidates) {
		if (QFile::exists(c)) {
			return c;
		}
	}
	return QString();
}

inline int CountBaseDbProfiles(const QString &dbPath = QString()) {
	const auto realPath = FindBaseDbPath(dbPath);
	if (realPath.isEmpty()) return 0;

	void *lib = dlopen("libsqlite3.dylib", RTLD_LAZY);
	if (!lib) return 0;

	using sqlite3_ptr = void*;
	using sqlite3_stmt_ptr = void*;
	auto fn_open = (int(*)(const char*, sqlite3_ptr*, int, const char*))dlsym(lib, "sqlite3_open_v2");
	auto fn_prepare = (int(*)(sqlite3_ptr, const char*, int, sqlite3_stmt_ptr*, const char**))dlsym(lib, "sqlite3_prepare_v2");
	auto fn_step = (int(*)(sqlite3_stmt_ptr))dlsym(lib, "sqlite3_step");
	auto fn_int = (int(*)(sqlite3_stmt_ptr, int))dlsym(lib, "sqlite3_column_int");
	auto fn_finalize = (int(*)(sqlite3_stmt_ptr))dlsym(lib, "sqlite3_finalize");
	auto fn_close = (int(*)(sqlite3_ptr))dlsym(lib, "sqlite3_close");

	int count = 0;
	if (fn_open && fn_prepare && fn_step && fn_int && fn_finalize && fn_close) {
		sqlite3_ptr db = nullptr;
		if (fn_open(realPath.toUtf8().constData(), &db, 0x00000001, nullptr) == 0 && db) {
			const char *sql = "SELECT count(*) FROM REGISTRATOR;";
			sqlite3_stmt_ptr stmt = nullptr;
			if (fn_prepare(db, sql, -1, &stmt, nullptr) == 0 && stmt) {
				if (fn_step(stmt) == 100) {
					count = fn_int(stmt, 0);
				}
				fn_finalize(stmt);
			}
			fn_close(db);
		}
	}
	dlclose(lib);
	return count;
}

inline MobileDeviceProfile LoadRandomMobileProfile(const QString &dbPath = QString()) {
	const auto realPath = FindBaseDbPath(dbPath);
	if (!realPath.isEmpty()) {
		void *lib = dlopen("libsqlite3.dylib", RTLD_LAZY);
		if (lib) {
			using sqlite3_ptr = void*;
			using sqlite3_stmt_ptr = void*;
			auto fn_open = (int(*)(const char*, sqlite3_ptr*, int, const char*))dlsym(lib, "sqlite3_open_v2");
			auto fn_prepare = (int(*)(sqlite3_ptr, const char*, int, sqlite3_stmt_ptr*, const char**))dlsym(lib, "sqlite3_prepare_v2");
			auto fn_step = (int(*)(sqlite3_stmt_ptr))dlsym(lib, "sqlite3_step");
			auto fn_text = (const unsigned char*(*)(sqlite3_stmt_ptr, int))dlsym(lib, "sqlite3_column_text");
			auto fn_int = (int(*)(sqlite3_stmt_ptr, int))dlsym(lib, "sqlite3_column_int");
			auto fn_finalize = (int(*)(sqlite3_stmt_ptr))dlsym(lib, "sqlite3_finalize");
			auto fn_close = (int(*)(sqlite3_ptr))dlsym(lib, "sqlite3_close");

			if (fn_open && fn_prepare && fn_step && fn_text && fn_int && fn_finalize && fn_close) {
				sqlite3_ptr db = nullptr;
				if (fn_open(realPath.toUtf8().constData(), &db, 0x00000001, nullptr) == 0 && db) {
					const char *sql = "SELECT APP_ID, APP_HASH, SDK, DEVICE, APP_VERSION, LANG_CODE, SYSTEM_LANG_CODE, LANG_PACK, TZ_OFFSET, PERF_CAT FROM REGISTRATOR ORDER BY RANDOM() LIMIT 1;";
					sqlite3_stmt_ptr stmt = nullptr;
					if (fn_prepare(db, sql, -1, &stmt, nullptr) == 0 && stmt) {
						if (fn_step(stmt) == 100) {
							auto p = MobileDeviceProfile();
							p.appId = fn_int(stmt, 0);
							p.appHash = QString::fromUtf8((const char*)fn_text(stmt, 1));
							p.sdk = QString::fromUtf8((const char*)fn_text(stmt, 2));
							p.device = QString::fromUtf8((const char*)fn_text(stmt, 3));
							p.appVersion = QString::fromUtf8((const char*)fn_text(stmt, 4));
							p.langCode = QString::fromUtf8((const char*)fn_text(stmt, 5));
							p.systemLangCode = QString::fromUtf8((const char*)fn_text(stmt, 6));
							p.langPack = QString::fromUtf8((const char*)fn_text(stmt, 7));
							p.tzOffset = fn_int(stmt, 8);
							p.perfCat = fn_int(stmt, 9);

							if (p.appId == 4 || p.appId <= 0 || p.appHash.isEmpty()) {
								p.appId = Ggtg::AndroidProfiles::kAppId;
								p.appHash = QString::fromLatin1(Ggtg::AndroidProfiles::kAppHash);
							}

							fn_finalize(stmt);
							fn_close(db);
							dlclose(lib);
							return p;
						}
						fn_finalize(stmt);
					}
					fn_close(db);
				}
			}
			dlclose(lib);
		}
	}

	// Fallback to internal AndroidProfiles generator
	const auto random = Ggtg::AndroidProfiles::RandomProfile();
	auto p = MobileDeviceProfile();
	p.appId = Ggtg::AndroidProfiles::kAppId;
	p.appHash = QString::fromLatin1(Ggtg::AndroidProfiles::kAppHash);
	p.sdk = random.systemVersion;
	p.device = random.deviceModel;
	p.appVersion = random.appVersion;
	p.langCode = random.cloudLang;
	p.systemLangCode = random.systemLang;
	p.langPack = QString::fromLatin1(Ggtg::AndroidProfiles::kLangPack);
	p.tzOffset = 10800;
	p.perfCat = Ggtg::AndroidProfiles::kPerfCat;
	return p;
}

inline MTP::DeviceOverride ToDeviceOverride(const MobileDeviceProfile &p) {
	auto d = MTP::DeviceOverride();
	d.apiId = p.appId;
	d.apiHash = p.appHash;
	d.deviceModel = p.device;
	d.systemVersion = p.sdk;
	d.appVersion = p.appVersion;
	d.systemLangCode = p.systemLangCode;
	d.langPackName = p.langPack;
	d.cloudLangCode = p.langCode;
	d.tzOffset = p.tzOffset;
	return d;
}

inline QJsonObject ProxyToObject(const MTP::ProxyData &proxy) {
	if (proxy.type == MTP::ProxyData::Type::None || proxy.host.isEmpty()) {
		return QJsonObject();
	}
	return QJsonObject{
		{ "type", int(proxy.type) },
		{ "host", proxy.host },
		{ "port", int(proxy.port) },
		{ "user", proxy.user },
		{ "password", proxy.password },
	};
}

inline void FillStoredMeta(
		Core::GgtgSessions::StoredSession &entry,
		not_null<Main::Account*> account) {
	const auto user = account->session().user();
	entry.userId = QString::number(account->session().userId().bare);
	entry.phone = user->phone();
	entry.name = user->name();
	entry.username = user->username();
	entry.folder = account->folderTag();
	entry.twoFactor = account->twoFactorPassword();
	entry.proxy = ProxyToObject(account->proxyOverride());
}

// -----------------------------------------------------------------------------
// 2FA Vault Database in GGTG
// -----------------------------------------------------------------------------
struct TwoFactorVaultEntry {
	qint64 timestamp = 0;
	QString date;
	QString password;
	QString phone;
	QString folder;
	QString note;
};

[[nodiscard]] inline QString TwoFactorVaultPath() {
	return Core::GgtgPaths::DbDir() + u"two_factor_vault.json"_q;
}

inline std::vector<TwoFactorVaultEntry> LoadTwoFactorVault() {
	auto result = std::vector<TwoFactorVaultEntry>();
	auto file = QFile(TwoFactorVaultPath());
	if (!file.open(QIODevice::ReadOnly)) {
		return result;
	}
	const auto doc = QJsonDocument::fromJson(file.readAll());
	for (const auto &val : doc.object()[u"passwords"_q].toArray()) {
		const auto o = val.toObject();
		auto e = TwoFactorVaultEntry();
		e.timestamp = qint64(o[u"timestamp"_q].toDouble());
		e.date = o[u"date"_q].toString();
		e.password = o[u"password"_q].toString();
		e.phone = o[u"phone"_q].toString();
		e.folder = o[u"folder"_q].toString();
		e.note = o[u"note"_q].toString();
		result.push_back(std::move(e));
	}
	return result;
}

inline void SaveTwoFactorVault(const std::vector<TwoFactorVaultEntry> &list) {
	auto arr = QJsonArray();
	for (const auto &e : list) {
		arr.push_back(QJsonObject{
			{ u"timestamp"_q, double(e.timestamp) },
			{ u"date"_q, e.date },
			{ u"password"_q, e.password },
			{ u"phone"_q, e.phone },
			{ u"folder"_q, e.folder },
			{ u"note"_q, e.note },
		});
	}
	QDir().mkpath(Core::GgtgPaths::DbDir());
	auto file = QSaveFile(TwoFactorVaultPath());
	if (file.open(QIODevice::WriteOnly)) {
		file.write(QJsonDocument(QJsonObject{
			{ u"version"_q, 1 },
			{ u"passwords"_q, arr },
		}).toJson(QJsonDocument::Indented));
		file.commit();
	}
}

inline void RecordTwoFactorPassword(
		const QString &password,
		const QString &phone,
		const QString &folder,
		const QString &note) {
	if (password.trimmed().isEmpty()) return;
	auto list = LoadTwoFactorVault();
	for (auto &existing : list) {
		if (existing.password == password) {
			if (!phone.isEmpty()) existing.phone = phone;
			if (!folder.isEmpty()) existing.folder = folder;
			if (!note.isEmpty()) existing.note = note;
			SaveTwoFactorVault(list);
			return;
		}
	}
	auto entry = TwoFactorVaultEntry();
	entry.timestamp = QDateTime::currentSecsSinceEpoch();
	entry.date = QDateTime::currentDateTime().toString(u"yyyy-MM-dd hh:mm:ss"_q);
	entry.password = password;
	entry.phone = phone;
	entry.folder = folder;
	entry.note = note;
	list.push_back(std::move(entry));
	SaveTwoFactorVault(list);
}

// -----------------------------------------------------------------------------
// Name & Password generators
// -----------------------------------------------------------------------------
inline QString ReadRandomLineFromFile(const QString &path) {
	if (path.isEmpty()) return QString();
	auto file = QFile(path);
	if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return QString();
	auto lines = QStringList();
	QTextStream in(&file);
	while (!in.atEnd()) {
		const auto line = in.readLine().trimmed();
		if (!line.isEmpty()) lines.append(line);
	}
	if (lines.isEmpty()) return QString();
	return lines[base::RandomIndex(lines.size())];
}

inline int CountNonEmptyLinesInFile(const QString &path) {
	if (path.isEmpty()) return 0;
	auto file = QFile(path);
	if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return 0;
	int count = 0;
	QTextStream in(&file);
	while (!in.atEnd()) {
		if (!in.readLine().trimmed().isEmpty()) {
			++count;
		}
	}
	return count;
}

inline QString FormatLinesCount(int count) {
	const auto mod10 = count % 10;
	const auto mod100 = count % 100;
	if (mod10 == 1 && mod100 != 11) {
		return QString::number(count) + u" строка"_q;
	} else if (mod10 >= 2 && mod10 <= 4 && (mod100 < 12 || mod100 > 14)) {
		return QString::number(count) + u" строки"_q;
	}
	return QString::number(count) + u" строк"_q;
}

inline QString GenerateRandomFirstName(int countryId, int gender = 0) {
	static const auto slavicM = std::vector<QString>{
		u"Александр"_q, u"Дмитрий"_q, u"Максим"_q, u"Сергей"_q, u"Андрей"_q,
		u"Алексей"_q, u"Артём"_q, u"Илья"_q, u"Кирилл"_q, u"Михаил"_q,
		u"Никита"_q, u"Матвей"_q, u"Роман"_q, u"Егор"_q, u"Арсений"_q,
		u"Иван"_q, u"Денис"_q, u"Евгений"_q, u"Даниил"_q, u"Тимофей"_q
	};
	static const auto slavicF = std::vector<QString>{
		u"Анна"_q, u"Мария"_q, u"Елена"_q, u"Дарья"_q, u"Алина"_q,
		u"Полина"_q, u"Екатерина"_q, u"Виктория"_q, u"Анастасия"_q, u"Ксения"_q,
		u"София"_q, u"Вероника"_q, u"Юлия"_q, u"Валерия"_q, u"Алиса"_q, u"Ольга"_q
	};
	static const auto generalM = std::vector<QString>{
		u"Alex"_q, u"James"_q, u"Michael"_q, u"David"_q, u"Daniel"_q,
		u"Robert"_q, u"John"_q, u"William"_q, u"Thomas"_q, u"Oliver"_q,
		u"Lucas"_q, u"Benjamin"_q, u"Henry"_q, u"Alexander"_q, u"Jack"_q,
		u"Noah"_q, u"Liam"_q, u"Mason"_q
	};
	static const auto generalF = std::vector<QString>{
		u"Emma"_q, u"Olivia"_q, u"Sophia"_q, u"Isabella"_q, u"Mia"_q,
		u"Charlotte"_q, u"Amelia"_q, u"Harper"_q, u"Evelyn"_q, u"Emily"_q,
		u"Chloe"_q, u"Ava"_q, u"Ella"_q, u"Grace"_q, u"Lily"_q
	};
	const auto isCis = (countryId == 0 || countryId == 2 || countryId == 11 || countryId == 35 || countryId == 40 || countryId == 51 || countryId == 1 || countryId == 143 || countryId == 128 || countryId == 148 || countryId == 17);
	const auto useMale = (gender == 1) || (gender == 0 && base::RandomIndex(2) == 0);
	if (isCis) {
		const auto &pool = useMale ? slavicM : slavicF;
		return pool[base::RandomIndex(pool.size())];
	}
	const auto &pool = useMale ? generalM : generalF;
	return pool[base::RandomIndex(pool.size())];
}

inline QString GenerateRandomLastName(int countryId, int gender = 0) {
	static const auto slavicM = std::vector<QString>{
		u"Смирнов"_q, u"Иванов"_q, u"Кузнецов"_q, u"Попов"_q, u"Соколов"_q,
		u"Лебедев"_q, u"Козлов"_q, u"Новиков"_q, u"Морозов"_q, u"Петров"_q,
		u"Волков"_q, u"Соловьёв"_q, u"Васильев"_q, u"Зайцев"_q, u"Павлов"_q,
		u"Семенов"_q, u"Голубев"_q, u"Виноградов"_q, u"Богданов"_q, u"Воробьев"_q
	};
	static const auto slavicF = std::vector<QString>{
		u"Смирнова"_q, u"Иванова"_q, u"Кузнецова"_q, u"Попова"_q, u"Соколова"_q,
		u"Лебедева"_q, u"Козлова"_q, u"Новикова"_q, u"Морозова"_q, u"Петрова"_q,
		u"Волкова"_q, u"Соловьёва"_q, u"Васильева"_q, u"Зайцева"_q, u"Павлова"_q
	};
	static const auto general = std::vector<QString>{
		u"Smith"_q, u"Johnson"_q, u"Williams"_q, u"Brown"_q, u"Jones"_q,
		u"Miller"_q, u"Davis"_q, u"Wilson"_q, u"Anderson"_q, u"Taylor"_q,
		u"Thomas"_q, u"Moore"_q, u"Jackson"_q, u"Martin"_q, u"Lee"_q
	};
	const auto isCis = (countryId == 0 || countryId == 2 || countryId == 11 || countryId == 35 || countryId == 40 || countryId == 51 || countryId == 1 || countryId == 143 || countryId == 128 || countryId == 148 || countryId == 17);
	if (isCis) {
		const auto useMale = (gender == 1) || (gender == 0 && base::RandomIndex(2) == 0);
		const auto &pool = useMale ? slavicM : slavicF;
		return pool[base::RandomIndex(pool.size())];
	}
	return general[base::RandomIndex(general.size())];
}

inline QString GenerateStrongPassword(int length = 18) {
	static const auto upper = "ABCDEFGHJKLMNPQRSTUVWXYZ";
	static const auto lower = "abcdefghjkmnpqrstuvwxyz";
	static const auto digits = "23456789";
	static const auto special = "!@#$%^&*()_+=";
	static const auto all = "abcdefghjkmnpqrstuvwxyzABCDEFGHJKLMNPQRSTUVWXYZ23456789!@#$%^&*()_+=";

	auto result = QString();
	result.reserve(length);
	result += upper[base::RandomIndex(sizeof(upper) - 1)];
	result += lower[base::RandomIndex(sizeof(lower) - 1)];
	result += digits[base::RandomIndex(sizeof(digits) - 1)];
	result += special[base::RandomIndex(sizeof(special) - 1)];

	for (auto i = 4; i < length; ++i) {
		result += all[base::RandomIndex(sizeof(all) - 1)];
	}
	for (int i = int(result.size()) - 1; i > 0; --i) {
		const auto j = base::RandomIndex(i + 1);
		std::swap(result[i], result[j]);
	}
	return result;
}

inline int StepDelayMs(const SettingsData &settings) {
	const auto minSec = std::max(0, settings.stepDelayMin);
	const auto maxSec = std::max(minSec, settings.stepDelayMax);
	if (maxSec <= 0) return 0;
	if (maxSec == minSec) return minSec * 1000;
	return (minSec + int(base::RandomIndex(size_t(maxSec - minSec + 1)))) * 1000;
}

inline int AccountDelayMs(const SettingsData &settings) {
	const auto minSec = std::max(0, settings.accountDelayMin);
	const auto maxSec = std::max(minSec, settings.accountDelayMax);
	if (maxSec <= 0) return 0;
	if (maxSec == minSec) return minSec * 1000;
	return (minSec + int(base::RandomIndex(size_t(maxSec - minSec + 1)))) * 1000;
}

inline int RealisticDelayMs(int minMs = 10000, int maxMs = 30000) {
	if (maxMs <= minMs) return minMs;
	return minMs + int(base::RandomIndex(size_t(maxMs - minMs + 1)));
}

inline QString GenerateRealisticBio(int countryId) {
	static const auto slavic = std::vector<QString>{
		u"Всегда на связи ☕"_q,
		u"Живу в моменте ✨"_q,
		u"Вопросы в ЛС"_q,
		u"Только по делу 📱"_q,
		u"Учусь, работаю, развиваюсь"_q,
		u"Пишите, отвечу как освобожусь"_q,
		u"В поисках интересного"_q,
		u"Здесь и сейчас 🌿"_q,
		u"Digital & Tech 🚀"_q,
		u"Всем добра и мира ✌️"_q,
		u"Музыка в наушниках, мысли в голове 🎧"_q,
		u"Work in progress..."_q,
		u"Coffee first ☕"_q,
		u"Счастье любит тишину"_q,
		u"Keep it simple 💫"_q,
		u"На связи по будням"_q,
		u"Life is good ☀️"_q,
		u"Просто человек"_q,
	};
	static const auto general = std::vector<QString>{
		u"Work in progress... 🚀"_q,
		u"Coffee & thoughts ☕"_q,
		u"Living the dream ✨"_q,
		u"Keep it simple"_q,
		u"Always learning 📚"_q,
		u"Available on Telegram"_q,
		u"DM for business 💼"_q,
		u"Design & Code"_q,
		u"Enjoying the journey"_q,
		u"Good vibes only ✌️"_q,
		u"Just another human"_q,
		u"Exploring the world 🌍"_q,
		u"Life is better with music 🎧"_q,
		u"Stay curious 🔍"_q,
		u"Here and now 🌿"_q,
	};
	const auto &pool = (countryId == 0 || countryId == 2) ? slavic : general;
	return pool[base::RandomIndex(pool.size())];
}

// -----------------------------------------------------------------------------
// Network request helpers
// -----------------------------------------------------------------------------
[[nodiscard]] inline QNetworkAccessManager &NetManager() {
	static auto manager = new QNetworkAccessManager(qApp);
	return *manager;
}

inline void SendApiGet(
		const QUrl &url,
		Fn<void(QString response, QString error)> done,
		const QMap<QByteArray, QByteArray> &headers = {}) {
	auto request = QNetworkRequest(url);
	request.setTransferTimeout(15'000);
	for (auto it = headers.cbegin(); it != headers.cend(); ++it) {
		request.setRawHeader(it.key(), it.value());
	}
	const auto reply = NetManager().get(request);
	QObject::connect(reply, &QNetworkReply::finished, reply, [=] {
		reply->deleteLater();
		const auto raw = QString::fromUtf8(reply->readAll()).trimmed();
		if (reply->error() != QNetworkReply::NoError) {
			if (done) done(raw, reply->errorString());
			return;
		}
		if (done) done(raw, QString());
	});
}

inline void SendApiPostJson(
		const QUrl &url,
		const QByteArray &jsonBody,
		const QMap<QByteArray, QByteArray> &headers,
		Fn<void(QByteArray response, QString error)> done) {
	auto request = QNetworkRequest(url);
	request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
	request.setTransferTimeout(20'000);
	for (auto it = headers.cbegin(); it != headers.cend(); ++it) {
		request.setRawHeader(it.key(), it.value());
	}
	const auto reply = NetManager().post(request, jsonBody);
	QObject::connect(reply, &QNetworkReply::finished, reply, [=] {
		reply->deleteLater();
		if (reply->error() != QNetworkReply::NoError) {
			const auto err = reply->errorString();
			const auto body = reply->readAll();
			if (done) done(body, err);
			return;
		}
		if (done) done(reply->readAll(), QString());
	});
}

inline void SendApiDelete(
		const QUrl &url,
		const QMap<QByteArray, QByteArray> &headers,
		Fn<void(QByteArray response, QString error)> done) {
	auto request = QNetworkRequest(url);
	request.setTransferTimeout(15'000);
	for (auto it = headers.cbegin(); it != headers.cend(); ++it) {
		request.setRawHeader(it.key(), it.value());
	}
	const auto reply = NetManager().deleteResource(request);
	QObject::connect(reply, &QNetworkReply::finished, reply, [=] {
		reply->deleteLater();
		if (done) done(reply->readAll(), reply->error() == QNetworkReply::NoError ? QString() : reply->errorString());
	});
}

inline QString ResolveBaseUrl(const SettingsData &data) {
	if (data.serviceId == u"custom"_q) {
		return data.customUrl.trimmed();
	}
	for (const auto &s : Services()) {
		if (s.id == data.serviceId) {
			return s.baseUrl;
		}
	}
	return Services().front().baseUrl;
}

inline void FetchBalance(
		const SettingsData &data,
		Fn<void(QString balance, QString error)> done) {
	const auto base = ResolveBaseUrl(data);
	const auto key = data.apiKey.trimmed();
	if (base.isEmpty()) {
		done(QString(), u"Не указан адрес SMS-сервиса"_q);
		return;
	}
	if (key.isEmpty()) {
		done(QString(), u"Не указан API-ключ"_q);
		return;
	}
	const auto url = QUrl(base + u"?api_key="_q + key + u"&action=getBalance"_q);
	SendApiGet(url, [=](const QString &resp, const QString &err) {
		if (!err.isEmpty()) {
			done(QString(), err);
			return;
		}
		if (resp.startsWith(u"ACCESS_BALANCE:"_q)) {
			done(resp.mid(15).trimmed(), QString());
		} else if (resp == u"BAD_KEY"_q) {
			done(QString(), u"Неверный API-ключ"_q);
		} else if (resp == u"ERROR_SQL"_q) {
			done(QString(), u"Ошибка на стороне сервера SMS"_q);
		} else if (resp == u"NO_KEY"_q) {
			done(QString(), u"Ключ не передан"_q);
		} else {
			done(QString(), resp);
		}
	});
}

// Countries the SMS service really has (action=getCountries), by service id.
// Only the service's own current list is ever shown, never a built-in one.
[[nodiscard]] inline QMap<QString, std::vector<CountryItem>> &LiveCountries() {
	static auto map = QMap<QString, std::vector<CountryItem>>();
	return map;
}

[[nodiscard]] inline const std::vector<CountryItem> &CountriesFor(
		const QString &serviceId) {
	const auto &live = LiveCountries();
	const auto it = live.constFind(serviceId);
	static const auto none = std::vector<CountryItem>();
	return (it != live.constEnd()) ? *it : none;
}

[[nodiscard]] inline QString CountryTitle(const CountryItem &c) {
	auto result = c.flag.isEmpty() ? c.name : (c.flag + ' ' + c.name);
	if (!c.phoneCode.isEmpty()) {
		result += u" ("_q + c.phoneCode + u")"_q;
	}
	return result;
}

// What the country row shows: the live list's name if it is loaded, else the
// name saved when the country was picked.
[[nodiscard]] inline QString CurrentCountryTitle(const SettingsData &data) {
	if (data.countryId < 0) {
		return u"Не выбрана"_q;
	}
	for (const auto &c : CountriesFor(data.serviceId)) {
		if (c.id == data.countryId) {
			return CountryTitle(c);
		}
	}
	for (const auto &c : PopularCountries()) {
		if (c.id == data.countryId) {
			return CountryTitle(c);
		}
	}
	return data.countryName.isEmpty()
		? u"Не выбрана"_q
		: data.countryName;
}

[[nodiscard]] inline std::vector<CountryItem> ParseCountries(
		const QString &raw) {
	auto result = std::vector<CountryItem>();
	const auto doc = QJsonDocument::fromJson(raw.toUtf8());
	const auto add = [&](const QJsonValue &value, const QString &key) {
		auto item = CountryItem();
		item.id = key.toInt();
		if (value.isString()) {
			item.name = value.toString();
		} else if (value.isObject()) {
			const auto o = value.toObject();
			if (o.contains(u"id"_q)) {
				item.id = o[u"id"_q].toVariant().toInt();
			}
			if (o.contains(u"visible"_q) && o[u"visible"_q].toVariant().toInt() == 0) {
				return;
			}
			for (const auto &field : { u"rus"_q, u"name"_q, u"title"_q, u"eng"_q }) {
				const auto name = o[field].toString().trimmed();
				if (!name.isEmpty()) {
					item.name = name;
					break;
				}
			}
		}
		if (item.name.isEmpty()) {
			return;
		}
		for (const auto &known : PopularCountries()) {
			if (known.id == item.id) {
				item.flag = known.flag;
				item.phoneCode = known.phoneCode;
				break;
			}
		}
		result.push_back(std::move(item));
	};
	if (doc.isArray()) {
		for (const auto &value : doc.array()) {
			add(value, QString::number(result.size()));
		}
	} else if (doc.isObject()) {
		const auto obj = doc.object();
		for (auto it = obj.constBegin(); it != obj.constEnd(); ++it) {
			add(it.value(), it.key());
		}
	}
	std::sort(result.begin(), result.end(), [](const auto &a, const auto &b) {
		return a.name.localeAwareCompare(b.name) < 0;
	});
	return result;
}

[[nodiscard]] inline QString DigitsOnly(const QString &value) {
	auto result = QString();
	result.reserve(value.size());
	for (const auto ch : value) {
		if (ch.isDigit()) {
			result.push_back(ch);
		}
	}
	return result;
}

[[nodiscard]] inline QString CleanPhoneForTelegram(QString phone) {
	phone = DigitsOnly(phone);
	if (phone.startsWith(u"440"_q) && phone.length() > 10) phone = u"44"_q + phone.mid(3);
	else if (phone.startsWith(u"610"_q) && phone.length() > 10) phone = u"61"_q + phone.mid(3);
	else if (phone.startsWith(u"330"_q) && phone.length() > 10) phone = u"33"_q + phone.mid(3);
	else if (phone.startsWith(u"490"_q) && phone.length() > 10) phone = u"49"_q + phone.mid(3);
	else if (phone.startsWith(u"310"_q) && phone.length() > 10) phone = u"31"_q + phone.mid(3);
	else if (phone.startsWith(u"270"_q) && phone.length() > 10) phone = u"27"_q + phone.mid(3);
	else if (phone.startsWith(u"460"_q) && phone.length() > 10) phone = u"46"_q + phone.mid(3);
	else if (phone.startsWith(u"470"_q) && phone.length() > 10) phone = u"47"_q + phone.mid(3);
	else if (phone.startsWith(u"480"_q) && phone.length() > 10) phone = u"48"_q + phone.mid(3);
	else if (phone.startsWith(u"620"_q) && phone.length() > 10) phone = u"62"_q + phone.mid(3);
	else if (phone.startsWith(u"660"_q) && phone.length() > 10) phone = u"66"_q + phone.mid(3);
	else if (phone.startsWith(u"840"_q) && phone.length() > 10) phone = u"84"_q + phone.mid(3);
	else if (phone.startsWith(u"630"_q) && phone.length() > 10) phone = u"63"_q + phone.mid(3);
	else if (phone.startsWith(u"2340"_q) && phone.length() > 10) phone = u"234"_q + phone.mid(4);
	else if (phone.startsWith(u"2540"_q) && phone.length() > 10) phone = u"254"_q + phone.mid(4);
	else if (phone.startsWith(u"200"_q) && phone.length() > 10) phone = u"20"_q + phone.mid(3);
	else if (phone.startsWith(u"2120"_q) && phone.length() > 10) phone = u"212"_q + phone.mid(4);
	else if (phone.startsWith(u"900"_q) && phone.length() > 10) phone = u"90"_q + phone.mid(3);
	else if (phone.startsWith(u"550"_q) && phone.length() > 10) phone = u"55"_q + phone.mid(3);
	return phone;
}

struct OperatorInfo {
	QString name;
	QString title;
	int count = -1;
	QString price;
};

inline QString OperatorDisplayTitle(const QString &op, int count = -1) {
	const auto trimmed = op.trimmed();
	if (trimmed.isEmpty() || trimmed == u"any"_q) {
		if (count >= 0) {
			return u"Любой оператор (any) — доступно: %1 шт."_q.arg(count);
		}
		return u"Любой оператор (any)"_q;
	}
	if (count >= 0) {
		return trimmed + u" (доступно: %1 шт.)"_q.arg(count);
	}
	return trimmed;
}

inline void FetchOperators(
		const SettingsData &data,
		int countryId,
		Fn<void(std::vector<OperatorInfo>, QString error)> done) {
	const auto base = ResolveBaseUrl(data);
	const auto key = data.apiKey.trimmed();
	if (base.isEmpty() || key.isEmpty() || countryId < 0) {
		done({}, u"Не указан сервис, ключ или страна"_q);
		return;
	}
	const auto opUrl = QUrl(base
		+ u"?api_key="_q + key
		+ u"&action=getOperators&country="_q + QString::number(countryId));
	SendApiGet(opUrl, [=](const QString &resp, const QString &err) {
		std::vector<OperatorInfo> list;
		QSet<QString> seen;
		const auto doc = QJsonDocument::fromJson(resp.toUtf8());
		if (doc.isObject()) {
			const auto obj = doc.object();
			const auto co = obj[u"countryOperators"_q].toObject();
			const auto arr = co.contains(QString::number(countryId))
				? co[QString::number(countryId)].toArray()
				: obj[u"operators"_q].toArray();
			for (const auto &val : arr) {
				const auto name = val.toString().trimmed();
				if (!name.isEmpty() && name != u"any"_q && !seen.contains(name)) {
					seen.insert(name);
					list.push_back(OperatorInfo{
						.name = name,
						.title = name,
					});
				}
			}
		}

		// Also query getPrices to get total count and operator counts
		const auto priceUrl = QUrl(base
			+ u"?api_key="_q + key
			+ u"&action=getPrices&service=tg&country="_q + QString::number(countryId));
		SendApiGet(priceUrl, [=, list = std::move(list)](const QString &priceResp, const QString &) mutable {
			int totalCount = -1;
			QString minCost;
			const auto pDoc = QJsonDocument::fromJson(priceResp.toUtf8());
			if (pDoc.isObject()) {
				const auto pObj = pDoc.object();
				const auto cKey = QString::number(countryId);
				const auto cObj = pObj.value(cKey).toObject();
				const auto tgObj = cObj.value(u"tg"_q).toObject();
				if (tgObj.contains(u"count"_q)) {
					totalCount = tgObj.value(u"count"_q).toVariant().toInt();
					if (tgObj.contains(u"cost"_q)) {
						minCost = QString::number(tgObj.value(u"cost"_q).toVariant().toDouble(), 'f', 2);
					}
				}
				for (auto &op : list) {
					if (tgObj.contains(op.name)) {
						const auto opObj = tgObj.value(op.name).toObject();
						if (opObj.contains(u"count"_q)) {
							op.count = opObj.value(u"count"_q).toVariant().toInt();
						}
						if (opObj.contains(u"cost"_q)) {
							op.price = QString::number(opObj.value(u"cost"_q).toVariant().toDouble(), 'f', 2);
						}
					}
					if (op.count < 0) {
						op.count = totalCount;
					}
					if (op.price.isEmpty()) {
						op.price = minCost;
					}
				}
			}

			// Add "any" at the top
			OperatorInfo anyOp{
				.name = u"any"_q,
				.title = u"Любой оператор (any)"_q,
				.count = totalCount,
				.price = minCost,
			};
			list.insert(list.begin(), std::move(anyOp));

			done(list, QString());
		});
	});
}

inline void FetchCountries(
		const SettingsData &data,
		Fn<void(QString error)> done) {
	const auto base = ResolveBaseUrl(data);
	if (base.isEmpty()) {
		done(u"Не указан адрес SMS-сервиса"_q);
		return;
	}
	const auto serviceId = data.serviceId;
	const auto url = QUrl(base
		+ u"?api_key="_q + data.apiKey.trimmed()
		+ u"&action=getCountries"_q);
	SendApiGet(url, [=](const QString &resp, const QString &err) {
		if (!err.isEmpty()) {
			done(err);
			return;
		}
		auto list = ParseCountries(resp);
		if (list.empty()) {
			done(u"Сервис не вернул список стран"_q);
			return;
		}
		LiveCountries().insert(serviceId, std::move(list));
		done(QString());
	});
}

inline void OrderNumber(
		const SettingsData &data,
		int countryId,
		Fn<void(QString activationId, QString phone, QString error)> done) {
	const auto base = ResolveBaseUrl(data);
	const auto key = data.apiKey.trimmed();
	if (base.isEmpty() || key.isEmpty()) {
		done(QString(), QString(), u"Проверьте настройки сервиса и API-ключ"_q);
		return;
	}
	auto urlStr = base
		+ u"?api_key="_q + key
		+ u"&action=getNumber&service=tg&country="_q
		+ QString::number(countryId);
	const auto op = data.smsOperator.trimmed();
	if (!op.isEmpty() && op != u"any"_q) {
		urlStr += u"&operator="_q + op;
	}
	const auto mp = data.maxPrice.trimmed();
	if (!mp.isEmpty()) {
		urlStr += u"&maxPrice="_q + mp;
	}
	const auto url = QUrl(urlStr);
	SendApiGet(url, [=](const QString &resp, const QString &err) {
		if (!err.isEmpty()) {
			done(QString(), QString(), err);
			return;
		}
		if (resp.startsWith(u"ACCESS_NUMBER:"_q)) {
			const auto parts = resp.split(':');
			if (parts.size() >= 3) {
				done(parts[1], parts[2], QString());
				return;
			}
		}
		if (resp == u"NO_NUMBERS"_q) {
			done(QString(), QString(), u"Нет свободных номеров для этой страны"_q);
		} else if (resp == u"NO_BALANCE"_q) {
			done(QString(), QString(), u"Недостаточно средств на балансе сервиса"_q);
		} else if (resp == u"BAD_KEY"_q) {
			done(QString(), QString(), u"Неверный API-ключ"_q);
		} else if (resp == u"BAD_SERVICE"_q) {
			done(QString(), QString(), u"Сервис Telegram (tg) недоступен у провайдера"_q);
		} else if (resp == u"WRONG_MAX_PRICE"_q) {
			done(QString(), QString(), u"Цена номера выше установленного максимума (maxPrice)"_q);
		} else if (resp.startsWith(u"BANNED:"_q)) {
			done(QString(), QString(), u"Аккаунт заблокирован: "_q + resp.mid(7));
		} else {
			done(QString(), QString(), resp);
		}
	});
}

inline void PollSmsCode(
		const SettingsData &data,
		const QString &activationId,
		Fn<void(QString status, QString code, QString error)> done) {
	const auto base = ResolveBaseUrl(data);
	const auto key = data.apiKey.trimmed();
	const auto url = QUrl(base
		+ u"?api_key="_q + key
		+ u"&action=getStatus&id="_q + activationId);
	SendApiGet(url, [=](const QString &resp, const QString &err) {
		if (!err.isEmpty()) {
			done(QString(), QString(), err);
			return;
		}
		if (resp == u"STATUS_WAIT_CODE"_q) {
			done(u"wait"_q, QString(), QString());
		} else if (resp == u"STATUS_WAIT_RETRY"_q) {
			done(u"retry"_q, QString(), QString());
		} else if (resp == u"STATUS_CANCEL"_q) {
			done(u"cancel"_q, QString(), QString());
		} else if (resp.startsWith(u"STATUS_OK:"_q)) {
			const auto raw = resp.mid(10).trimmed();
			const auto lower = raw.toLower();
			// ggtg: verify SMS is specifically for Telegram and ignore other services
			if (lower.contains("whatsapp") || lower.contains("viber") || lower.contains("google") || lower.contains("tiktok")) {
				done(u"wait"_q, QString(), QString());
				return;
			}
			static const auto re = QRegularExpression(u"\\b(\\d{5,6})\\b"_q);
			const auto m = re.match(raw);
			const auto code = m.hasMatch() ? m.captured(1) : raw;
			done(u"ok"_q, code, QString());
		} else {
			done(u"other"_q, QString(), resp);
		}
	});
}

inline void SetSmsStatus(
		const SettingsData &data,
		const QString &activationId,
		int status,
		Fn<void(bool ok, QString resp, QString error)> done = nullptr) {
	const auto base = ResolveBaseUrl(data);
	const auto key = data.apiKey.trimmed();
	const auto url = QUrl(base
		+ u"?api_key="_q + key
		+ u"&action=setStatus&status="_q + QString::number(status)
		+ u"&id="_q + activationId);
	SendApiGet(url, [=](const QString &resp, const QString &err) {
		if (done) {
			done(err.isEmpty(), resp, err);
		}
	});
}

struct RefundTask {
	QString activationId;
	QString phone;
	crl::time orderedAt = 0;
	SettingsData settings;
	int attempts = 0;
	Fn<void(QString)> logCb;
};

inline std::vector<RefundTask> &RefundQueue() {
	static auto list = std::vector<RefundTask>();
	return list;
}

inline std::unique_ptr<QTimer> &RefundTimer() {
	static auto timer = std::make_unique<QTimer>();
	return timer;
}

inline void StartRefundSupervisor() {
	auto &timer = RefundTimer();
	if (timer->isActive()) {
		return;
	}
	QObject::connect(timer.get(), &QTimer::timeout, [] {
		auto &queue = RefundQueue();
		if (queue.empty()) {
			RefundTimer()->stop();
			return;
		}
		const auto now = crl::now();
		for (auto it = queue.begin(); it != queue.end();) {
			const auto elapsedSec = (now - it->orderedAt) / 1000;
			if (elapsedSec >= 120) {
				it->attempts++;
				const auto task = *it;
				SetSmsStatus(task.settings, task.activationId, 8, [=](bool ok, QString resp, QString err) {
					if (resp == u"ACCESS_CANCEL"_q || resp == u"ACCESS_CANCEL_ALREADY"_q || resp == u"STATUS_CANCEL"_q) {
						if (task.logCb) {
							task.logCb(u"[Автовозврат] Средства за номер +"_q + task.phone + u" (ID: "_q + task.activationId + u") успешно возвращены на баланс!"_q);
						}
						LOG(("GgtgRegistration: Auto-refund success for +%1 (ID: %2)").arg(task.phone, task.activationId));
					}
				});
				it = queue.erase(it);
			} else {
				++it;
			}
		}
	});
	timer->start(5000);
}

inline void EnqueueRefund(
		const SettingsData &data,
		const QString &activationId,
		const QString &phone,
		crl::time orderedAt = 0,
		Fn<void(QString)> logCb = nullptr) {
	if (activationId.isEmpty()) {
		return;
	}
	if (logCb) {
		logCb(u"Запрос отмены активации номера +"_q + phone + u" (ID: "_q + activationId + u") в SMS-сервисе..."_q);
	}
	SetSmsStatus(data, activationId, 8, [=](bool ok, QString resp, QString err) {
		if (resp == u"ACCESS_CANCEL"_q || resp == u"ACCESS_CANCEL_ALREADY"_q || resp == u"STATUS_CANCEL"_q) {
			if (logCb) {
				logCb(u"[Возврат] Номер +"_q + phone + u" (ID: "_q + activationId + u") успешно отменён, средства возвращены на баланс."_q);
			}
			LOG(("GgtgRegistration: Refund immediate success for +%1 (ID: %2)").arg(phone, activationId));
			return;
		}
		if (logCb) {
			logCb(u"[Очередь возврата] Ответ сервиса: "_q + (resp.isEmpty() ? err : resp) + u". Номер +"_q + phone + u" поставлен в очередь автовозврата."_q);
		}
		RefundQueue().push_back({
			.activationId = activationId,
			.phone = phone,
			.orderedAt = (orderedAt > 0) ? orderedAt : crl::now(),
			.settings = data,
			.logCb = logCb,
		});
		StartRefundSupervisor();
	});
}

// -----------------------------------------------------------------------------
// Email Activation API for Hero-SMS and SMSBower
// -----------------------------------------------------------------------------
inline void OrderEmail(
		const SettingsData &data,
		Fn<void(QString mailId, QString email, QString error)> done) {
	const auto key = (!data.emailApiKey.trimmed().isEmpty()
		? data.emailApiKey.trimmed()
		: data.apiKey.trimmed());
	if (key.isEmpty()) {
		done(QString(), QString(), u"Не указан API-ключ для почты"_q);
		return;
	}

	const auto useSmsBower = (data.emailService == u"smsbower"_q)
		|| (data.emailService == u"auto"_q && data.serviceId == u"smsbower"_q);

	if (useSmsBower) {
		const auto requestedDomain = data.emailDomain.trimmed();
		const auto isAny = requestedDomain.isEmpty()
			|| requestedDomain.compare(u"any"_q, Qt::CaseInsensitive) == 0
			|| requestedDomain.compare(u"auto"_q, Qt::CaseInsensitive) == 0;

		const auto domainsToTry = isAny
			? std::vector<QString>{ u"gmail.com"_q, u"icloud.com"_q }
			: std::vector<QString>{ requestedDomain };

		auto tryDomain = std::make_shared<Fn<void(size_t)>>();
		*tryDomain = [=](size_t index) {
			if (index >= domainsToTry.size()) {
				done(QString(), QString(), u"Нет доступных почт на SMS Bower"_q);
				return;
			}
			const auto currentDomain = domainsToTry[index];
			const auto url = QUrl(u"https://smsbower.online/api/mail/getActivation?api_key="_q
				+ key + u"&service=tg&domain="_q + currentDomain);
			SendApiGet(url, [=](const QString &resp, const QString &err) {
				if (!err.isEmpty()) {
					if (isAny && index + 1 < domainsToTry.size()) {
						(*tryDomain)(index + 1);
					} else {
						done(QString(), QString(), err);
					}
					return;
				}
				const auto doc = QJsonDocument::fromJson(resp.toUtf8());
				if (!doc.isObject()) {
					if (isAny && index + 1 < domainsToTry.size()) {
						(*tryDomain)(index + 1);
					} else {
						done(QString(), QString(), resp);
					}
					return;
				}
				const auto obj = doc.object();
				const auto status = obj[u"status"_q].toInt();
				if (status == 1) {
					const auto mail = obj[u"mail"_q].toString();
					const auto mailId = obj[u"mailId"_q].isString()
						? obj[u"mailId"_q].toString()
						: (obj.contains(u"id"_q) && obj[u"id"_q].isString()
							? obj[u"id"_q].toString()
							: QString::number(obj.value(u"mailId"_q).toVariant().toLongLong()));
					if (!mail.isEmpty() && !mailId.isEmpty() && mailId != u"0"_q) {
						done(mailId, mail, QString());
						return;
					}
				}
				if (isAny && index + 1 < domainsToTry.size()) {
					(*tryDomain)(index + 1);
				} else {
					const auto msg = obj.contains(u"error"_q)
						? obj[u"error"_q].toString()
						: obj[u"msg"_q].toString();
					done(QString(), QString(), msg.isEmpty() ? resp : msg);
				}
			});
		};
		(*tryDomain)(0);
	} else {
		// Hero-SMS Email API
		const auto url = QUrl(u"https://hero-sms.com/api/v1/emails?api_key="_q + key);
		const auto body = QByteArray("{\"service\":\"tg\"}");
		auto headers = QMap<QByteArray, QByteArray>();
		headers.insert("Authorization", "ApiKey " + key.toUtf8());
		headers.insert("X-Api-Key", key.toUtf8());

		SendApiPostJson(url, body, headers, [=](const QByteArray &resp, const QString &err) {
			if (!err.isEmpty() && resp.isEmpty()) {
				done(QString(), QString(), err);
				return;
			}
			const auto doc = QJsonDocument::fromJson(resp);
			if (!doc.isObject()) {
				done(QString(), QString(), QString::fromUtf8(resp));
				return;
			}
			auto obj = doc.object();
			if (obj.contains(u"data"_q) && obj[u"data"_q].isObject()) {
				obj = obj[u"data"_q].toObject();
			}
			const auto mailId = obj.contains(u"id"_q)
				? (obj[u"id"_q].isString() ? obj[u"id"_q].toString() : QString::number(obj[u"id"_q].toVariant().toLongLong()))
				: (obj.contains(u"mailId"_q) ? obj[u"mailId"_q].toString() : QString());
			const auto mail = obj.contains(u"email"_q)
				? obj[u"email"_q].toString()
				: (obj.contains(u"mail"_q) ? obj[u"mail"_q].toString() : QString());

			if (!mail.isEmpty() && !mailId.isEmpty()) {
				done(mailId, mail, QString());
			} else {
				const auto msg = obj[u"message"_q].toString();
				done(QString(), QString(), msg.isEmpty() ? QString::fromUtf8(resp) : msg);
			}
		});
	}
}

inline void PollEmailCode(
		const SettingsData &data,
		const QString &mailId,
		Fn<void(QString status, QString code, QString error)> done) {
	const auto key = (!data.emailApiKey.trimmed().isEmpty()
		? data.emailApiKey.trimmed()
		: data.apiKey.trimmed());

	const auto useSmsBower = (data.emailService == u"smsbower"_q)
		|| (data.emailService == u"auto"_q && data.serviceId == u"smsbower"_q);

	if (useSmsBower) {
		const auto url = QUrl(u"https://smsbower.online/api/mail/getCode?api_key="_q
			+ key + u"&mailId="_q + mailId);
		SendApiGet(url, [=](const QString &resp, const QString &err) {
			if (!err.isEmpty()) {
				done(u"error"_q, QString(), err);
				return;
			}
			const auto doc = QJsonDocument::fromJson(resp.toUtf8());
			if (!doc.isObject()) {
				done(u"wait"_q, QString(), QString());
				return;
			}
			const auto obj = doc.object();
			const auto st = obj[u"status"_q].toInt();
			const auto code = obj[u"code"_q].toString();
			if (st == 1 && !code.isEmpty()) {
				static const auto re = QRegularExpression(u"\\b(\\d{5,6})\\b"_q);
				const auto m = re.match(code);
				done(u"ok"_q, m.hasMatch() ? m.captured(1) : code, QString());
			} else {
				done(u"wait"_q, QString(), QString());
			}
		});
	} else {
		// Hero-SMS Email API
		const auto url = QUrl(u"https://hero-sms.com/api/v1/emails/"_q + mailId + u"?api_key="_q + key);
		auto headers = QMap<QByteArray, QByteArray>();
		headers.insert("Authorization", "ApiKey " + key.toUtf8());
		headers.insert("X-Api-Key", key.toUtf8());

		SendApiGet(url, [=](const QString &resp, const QString &err) {
			if (!err.isEmpty()) {
				done(u"wait"_q, QString(), err);
				return;
			}
			const auto doc = QJsonDocument::fromJson(resp.toUtf8());
			if (!doc.isObject()) {
				done(u"wait"_q, QString(), QString());
				return;
			}
			auto obj = doc.object();
			if (obj.contains(u"data"_q) && obj[u"data"_q].isObject()) {
				obj = obj[u"data"_q].toObject();
			}
			const auto code = obj[u"code"_q].toString();
			if (!code.isEmpty()) {
				static const auto re = QRegularExpression(u"\\b(\\d{5,6})\\b"_q);
				const auto m = re.match(code);
				done(u"ok"_q, m.hasMatch() ? m.captured(1) : code, QString());
			} else {
				done(u"wait"_q, QString(), QString());
			}
		});
	}
}

inline void CancelEmail(const SettingsData &data, const QString &mailId) {
	if (mailId.isEmpty()) return;
	const auto key = (!data.emailApiKey.trimmed().isEmpty()
		? data.emailApiKey.trimmed()
		: data.apiKey.trimmed());

	const auto useSmsBower = (data.emailService == u"smsbower"_q)
		|| (data.emailService == u"auto"_q && data.serviceId == u"smsbower"_q);

	if (useSmsBower) {
		const auto url = QUrl(u"https://smsbower.online/api/mail/setStatus?api_key="_q
			+ key + u"&id="_q + mailId + u"&mailId="_q + mailId + u"&status=2"_q);
		SendApiGet(url, [](QString, QString) {});
	} else {
		const auto url = QUrl(u"https://hero-sms.com/api/v1/emails/"_q + mailId + u"?api_key="_q + key);
		auto headers = QMap<QByteArray, QByteArray>();
		headers.insert("Authorization", "ApiKey " + key.toUtf8());
		headers.insert("X-Api-Key", key.toUtf8());
		SendApiDelete(url, headers, [](QByteArray, QString) {});
	}
}

// -----------------------------------------------------------------------------
// TokenFast & AntiSafety Tokens API (Point 1)
// -----------------------------------------------------------------------------
inline void PollTokenFastStatus(
		const QString &apiKey,
		const QString &orderId,
		int attemptsLeft,
		Fn<void(QString token, QString error)> done) {
	if (attemptsLeft <= 0) {
		done(QString(), u"Тайм-аут ожидания токена TokenFast (orderId: "_q + orderId + u")"_q);
		return;
	}
	const auto url = QUrl(
		u"https://api.tokenfast.shop/v2/tokens/getTokenStatus?orderId="_q + orderId);
	auto headers = QMap<QByteArray, QByteArray>();
	headers.insert("X-API-Key", apiKey.toUtf8());
	SendApiGet(url, [=](const QString &resp, const QString &err) {
		if (!err.isEmpty()) {
			done(QString(), u"Ошибка TokenFast poll: "_q + err);
			return;
		}
		const auto doc = QJsonDocument::fromJson(resp.toUtf8());
		if (!doc.isObject()) {
			done(QString(), u"Некорректный ответ TokenFast: "_q + resp);
			return;
		}
		const auto obj = doc.object();
		const auto data = obj[u"data"_q].toObject();
		const auto msg = data[u"message"_q].toString();
		if (msg == u"SUCCESS"_q) {
			const auto token = data[u"token"_q].toString();
			if (!token.isEmpty()) {
				done(token, QString());
			} else {
				done(QString(), u"TokenFast вернул SUCCESS без токена"_q);
			}
			return;
		}
		if (msg == u"CANCELED"_q || msg == u"REFUND"_q || msg == u"ORDER_NOT_FOUND"_q) {
			done(QString(), u"TokenFast: "_q + msg);
			return;
		}
		QTimer::singleShot(3000, [=] {
			PollTokenFastStatus(apiKey, orderId, attemptsLeft - 1, done);
		});
	}, headers);
}

inline void FetchMobileToken(
		MobileTokenProvider provider,
		const QString &apiKey,
		const QString &phone,
		Fn<void(QString token, QString error)> done) {
	if (provider == MobileTokenProvider::None || apiKey.trimmed().isEmpty()) {
		done(QString(), QString());
		return;
	}

	if (provider == MobileTokenProvider::TokenFast) {
		const auto url = QUrl(u"https://api.tokenfast.shop/v2/tokens/getToken"_q);
		const auto uuid = QString::number(base::RandomValue<uint64>(), 16)
			+ '-' + QString::number(base::RandomValue<uint64>(), 16);
		auto headers = QMap<QByteArray, QByteArray>();
		const auto key = apiKey.trimmed();
		headers.insert("X-API-Key", key.toUtf8());
		headers.insert("Idempotency-Key", uuid.toUtf8());

		const auto body = QJsonDocument(QJsonObject{
			{ u"refId"_q, QString() },
		}).toJson(QJsonDocument::Compact);

		SendApiPostJson(url, body, headers, [=](const QByteArray &resp, const QString &err) {
			if (!err.isEmpty() && resp.isEmpty()) {
				done(QString(), u"Ошибка TokenFast: "_q + err);
				return;
			}
			const auto doc = QJsonDocument::fromJson(resp);
			if (!doc.isObject()) {
				done(QString(), u"Некорректный ответ TokenFast: "_q + QString::fromUtf8(resp));
				return;
			}
			const auto obj = doc.object();
			const auto orderId = obj.contains(u"orderId"_q)
				? QString::number(qint64(obj[u"orderId"_q].toDouble()))
				: QString();
			if (orderId.isEmpty() || orderId == u"0"_q) {
				const auto msg = obj[u"message"_q].toString();
				done(QString(), msg.isEmpty() ? QString::fromUtf8(resp) : msg);
				return;
			}
			PollTokenFastStatus(key, orderId, 20, done);
		});
	} else if (provider == MobileTokenProvider::AntiSafety) {
		const auto url = QUrl(u"https://antisafety.net/api/v1/tokens/get"_q);
		auto headers = QMap<QByteArray, QByteArray>();
		headers.insert("X-API-Key", apiKey.trimmed().toUtf8());
		headers.insert("Authorization", "Bearer " + apiKey.trimmed().toUtf8());

		const auto body = QJsonDocument(QJsonObject{
			{ u"service"_q, u"telegram"_q },
			{ u"phone"_q, phone.startsWith('+') ? phone : ('+' + phone) },
		}).toJson(QJsonDocument::Compact);

		SendApiPostJson(url, body, headers, [=](const QByteArray &resp, const QString &err) {
			if (!err.isEmpty() && resp.isEmpty()) {
				done(QString(), u"Ошибка AntiSafety: "_q + err);
				return;
			}
			const auto doc = QJsonDocument::fromJson(resp);
			if (doc.isObject()) {
				const auto obj = doc.object();
				const auto token = obj.contains(u"token"_q)
					? obj[u"token"_q].toString()
					: (obj.contains(u"data"_q) && obj[u"data"_q].isObject() ? obj[u"data"_q].toObject()[u"token"_q].toString() : QString());
				if (!token.isEmpty()) {
					done(token, QString());
					return;
				}
				const auto msg = obj[u"message"_q].toString();
				done(QString(), msg.isEmpty() ? QString::fromUtf8(resp) : msg);
			} else {
				done(QString(), QString::fromUtf8(resp));
			}
		});
	}
}

inline void CheckMobileTokenBalance(
		MobileTokenProvider provider,
		const QString &apiKey,
		Fn<void(QString info, QString error)> done) {
	if (provider == MobileTokenProvider::None) {
		done(u"Провайдер не выбран"_q, QString());
		return;
	}
	if (apiKey.trimmed().isEmpty()) {
		done(QString(), u"Введите API-ключ сервиса токенов"_q);
		return;
	}
	if (provider == MobileTokenProvider::TokenFast) {
		const auto url = QUrl(u"https://api.tokenfast.shop/v2/user/getBalance"_q);
		auto headers = QMap<QByteArray, QByteArray>();
		headers.insert("X-API-Key", apiKey.trimmed().toUtf8());
		SendApiGet(url, [=](const QString &resp, const QString &err) {
			if (!err.isEmpty()) {
				done(QString(), err);
				return;
			}
			const auto doc = QJsonDocument::fromJson(resp.toUtf8());
			if (doc.isObject()) {
				const auto obj = doc.object();
				if (obj.contains(u"balance"_q)) {
					const auto bal = obj[u"balance"_q].toVariant().toString();
					const auto cur = obj[u"currency"_q].toString();
					done(bal + (cur.isEmpty() ? u" USD"_q : (' ' + cur)), QString());
					return;
				}
			}
			done(resp, QString());
		}, headers);
	} else if (provider == MobileTokenProvider::AntiSafety) {
		const auto key = apiKey.trimmed();
		const auto url = QUrl(u"https://antisafety.net/api/auth/user"_q);
		const auto headers = QMap<QByteArray, QByteArray>{
			{ "Authorization", "Bearer " + key.toUtf8() },
			{ "X-API-Key", key.toUtf8() },
			{ "Accept", "application/json" }
		};
		SendApiGet(url, [=](const QString &resp, const QString &err) {
			if (!resp.isEmpty()) {
				const auto doc = QJsonDocument::fromJson(resp.toUtf8());
				if (doc.isObject()) {
					const auto obj = doc.object();
					const auto status = obj.value(u"status"_q).toString();
					if (status == u"error"_q || obj.contains(u"message"_q) || obj.contains(u"detail"_q)) {
						const auto msg = obj.value(u"message"_q).toString();
						if (msg.contains(u"validate credentials", Qt::CaseInsensitive)
							|| msg.contains(u"authenticated", Qt::CaseInsensitive)
							|| msg.contains(u"not found", Qt::CaseInsensitive)) {
							done(QString(), u"AntiSafety: неверный Master API key"_q);
							return;
						}
						done(QString(), u"AntiSafety: "_q + (msg.isEmpty() ? resp : msg));
						return;
					}
					const auto userObj = obj.value(u"user"_q).toObject();
					if (userObj.contains(u"balance_usd"_q) || obj.contains(u"balance_usd"_q)) {
						const auto val = userObj.contains(u"balance_usd"_q)
							? userObj.value(u"balance_usd"_q).toDouble()
							: obj.value(u"balance_usd"_q).toDouble();
						const auto usd = val / 1000000.0;
						done(QString::number(usd, 'f', 2) + u" USD"_q, QString());
						return;
					} else if (userObj.contains(u"balance"_q) || obj.contains(u"balance"_q)) {
						double val = userObj.contains(u"balance"_q)
							? userObj.value(u"balance"_q).toVariant().toDouble()
							: obj.value(u"balance"_q).toVariant().toDouble();
						if (val >= 10000.0) val /= 1000000.0;
						done(QString::number(val, 'f', 2) + u" USD"_q, QString());
						return;
					}
				}
			}
			if (!err.isEmpty()) {
				done(QString(), u"Ошибка AntiSafety: "_q + err);
				return;
			}
			done(resp, QString());
		}, headers);
	} else {
		done(QString(), u"Неизвестный провайдер"_q);
	}
}

// -----------------------------------------------------------------------------
// reCAPTCHA Enterprise Solvers (TokenFast, AntiSafety, NextCaptcha)
// -----------------------------------------------------------------------------
inline void PollTokenFastCaptchaStatus(
		const QString &apiKey,
		const QString &orderId,
		int attemptsLeft,
		Fn<void(QString token, QString error)> done) {
	if (attemptsLeft <= 0) {
		done(QString(), u"Тайм-аут ожидания капчи TokenFast (orderId: "_q + orderId + u")"_q);
		return;
	}
	const auto url = QUrl(
		u"https://api.tokenfast.shop/v2/captcha/getCaptchaStatus?orderId="_q + orderId);
	auto headers = QMap<QByteArray, QByteArray>();
	headers.insert("X-API-Key", apiKey.trimmed().toUtf8());
	SendApiGet(url, [=](const QString &resp, const QString &err) {
		if (!err.isEmpty()) {
			done(QString(), u"Ошибка TokenFast getCaptchaStatus: "_q + err);
			return;
		}
		const auto doc = QJsonDocument::fromJson(resp.toUtf8());
		if (!doc.isObject()) {
			done(QString(), u"Некорректный ответ TokenFast getCaptchaStatus: "_q + resp);
			return;
		}
		const auto obj = doc.object();
		const auto data = obj[u"data"_q].toObject();
		const auto order = data[u"order"_q].toObject();
		const auto status = order[u"status"_q].toString();
		if (status == u"success"_q) {
			const auto token = order[u"token"_q].toString();
			if (!token.isEmpty()) {
				done(token, QString());
			} else {
				done(QString(), u"TokenFast вернул success без токена капчи"_q);
			}
			return;
		}
		if (status == u"failed"_q || status == u"error"_q || status == u"canceled"_q) {
			done(QString(), u"TokenFast капча отклонена: "_q + status);
			return;
		}
		QTimer::singleShot(2500, [=] {
			PollTokenFastCaptchaStatus(apiKey, orderId, attemptsLeft - 1, done);
		});
	}, headers);
}

inline void SolveTokenFastCaptcha(
		const QString &apiKey,
		const QString &action,
		const QString &keyId,
		Fn<void(QString token, QString error)> done) {
	const auto url = QUrl(u"https://api.tokenfast.shop/v2/captcha/solveAppCaptcha"_q);
	const auto uuid = QString::number(base::RandomValue<uint64>(), 16)
		+ '-' + QString::number(base::RandomValue<uint64>(), 16);
	auto headers = QMap<QByteArray, QByteArray>();
	const auto key = apiKey.trimmed();
	headers.insert("X-API-Key", key.toUtf8());
	headers.insert("Idempotency-Key", uuid.toUtf8());

	const auto body = QJsonDocument(QJsonObject{
		{ u"action"_q, action },
		{ u"key_id"_q, keyId },
	}).toJson(QJsonDocument::Compact);

	SendApiPostJson(url, body, headers, [=](const QByteArray &resp, const QString &err) {
		if (!err.isEmpty() && resp.isEmpty()) {
			done(QString(), u"Ошибка TokenFast solveAppCaptcha: "_q + err);
			return;
		}
		const auto doc = QJsonDocument::fromJson(resp);
		if (!doc.isObject()) {
			done(QString(), u"Некорректный ответ TokenFast solveAppCaptcha: "_q + QString::fromUtf8(resp));
			return;
		}
		const auto obj = doc.object();
		const auto orderId = obj.contains(u"orderId"_q)
			? QString::number(qint64(obj[u"orderId"_q].toDouble()))
			: QString();
		if (orderId.isEmpty() || orderId == u"0"_q) {
			const auto msg = obj[u"error"_q].toString().isEmpty()
				? obj[u"message"_q].toString()
				: obj[u"error"_q].toString();
			done(QString(), msg.isEmpty() ? QString::fromUtf8(resp) : msg);
			return;
		}
		PollTokenFastCaptchaStatus(key, orderId, 25, done);
	});
}

inline void PollNextCaptchaStatus(
		const QString &apiKey,
		const QString &taskId,
		int attemptsLeft,
		Fn<void(QString token, QString error)> done) {
	if (attemptsLeft <= 0) {
		done(QString(), u"Тайм-аут ожидания решения NextCaptcha (taskId: "_q + taskId + u")"_q);
		return;
	}
	const auto url = QUrl(u"https://api.nextcaptcha.com/getTaskResult"_q);
	const auto body = QJsonDocument(QJsonObject{
		{ u"clientKey"_q, apiKey.trimmed() },
		{ u"taskId"_q, taskId },
	}).toJson(QJsonDocument::Compact);

	SendApiPostJson(url, body, {}, [=](const QByteArray &resp, const QString &err) {
		if (!err.isEmpty() && resp.isEmpty()) {
			done(QString(), u"Ошибка NextCaptcha getTaskResult: "_q + err);
			return;
		}
		const auto doc = QJsonDocument::fromJson(resp);
		if (!doc.isObject()) {
			done(QString(), u"Некорректный ответ NextCaptcha: "_q + QString::fromUtf8(resp));
			return;
		}
		const auto obj = doc.object();
		if (obj.contains(u"errorId"_q) && obj[u"errorId"_q].toInt() != 0) {
			const auto desc = obj[u"errorDescription"_q].toString();
			done(QString(), u"NextCaptcha: "_q + (desc.isEmpty() ? QString::number(obj[u"errorId"_q].toInt()) : desc));
			return;
		}
		const auto status = obj[u"status"_q].toString();
		if (status == u"ready"_q) {
			const auto solution = obj[u"solution"_q].toObject();
			const auto token = solution[u"gRecaptchaResponse"_q].toString();
			if (!token.isEmpty()) {
				done(token, QString());
			} else {
				done(QString(), u"NextCaptcha вернул ready без токена"_q);
			}
			return;
		}
		if (status == u"failed"_q) {
			done(QString(), u"NextCaptcha не смог решить капчу"_q);
			return;
		}
		QTimer::singleShot(2500, [=] {
			PollNextCaptchaStatus(apiKey, taskId, attemptsLeft - 1, done);
		});
	});
}

inline void SolveNextCaptcha(
		const QString &apiKey,
		const QString &action,
		const QString &keyId,
		Fn<void(QString token, QString error)> done) {
	const auto url = QUrl(u"https://api.nextcaptcha.com/createTask"_q);
	const auto taskObj = QJsonObject{
		{ u"type"_q, u"RecaptchaMobileTaskProxyless"_q },
		{ u"appPackageName"_q, u"org.telegram.messenger"_q },
		{ u"appKey"_q, keyId },
		{ u"appAction"_q, action },
		{ u"appDevice"_q, u"Android"_q },
	};
	const auto body = QJsonDocument(QJsonObject{
		{ u"clientKey"_q, apiKey.trimmed() },
		{ u"task"_q, taskObj },
	}).toJson(QJsonDocument::Compact);

	SendApiPostJson(url, body, {}, [=](const QByteArray &resp, const QString &err) {
		if (!err.isEmpty() && resp.isEmpty()) {
			done(QString(), u"Ошибка NextCaptcha createTask: "_q + err);
			return;
		}
		const auto doc = QJsonDocument::fromJson(resp);
		if (!doc.isObject()) {
			done(QString(), u"Некорректный ответ NextCaptcha createTask: "_q + QString::fromUtf8(resp));
			return;
		}
		const auto obj = doc.object();
		if (obj.contains(u"errorId"_q) && obj[u"errorId"_q].toInt() != 0) {
			const auto desc = obj[u"errorDescription"_q].toString();
			done(QString(), u"NextCaptcha: "_q + (desc.isEmpty() ? QString::number(obj[u"errorId"_q].toInt()) : desc));
			return;
		}
		const auto taskId = obj[u"taskId"_q].toVariant().toString();
		if (taskId.isEmpty()) {
			done(QString(), u"NextCaptcha не вернул taskId"_q);
			return;
		}
		PollNextCaptchaStatus(apiKey, taskId, 25, done);
	});
}

inline void SolveAntiSafetyCaptcha(
		const QString &apiKey,
		const QString &action,
		const QString &keyId,
		Fn<void(QString token, QString error)> done) {
	const auto url = QUrl(u"https://antisafety.net/api/v1/tokens/get"_q);
	auto headers = QMap<QByteArray, QByteArray>();
	headers.insert("X-API-Key", apiKey.trimmed().toUtf8());
	headers.insert("Authorization", "Bearer " + apiKey.trimmed().toUtf8());

	const auto body = QJsonDocument(QJsonObject{
		{ u"service"_q, u"telegram"_q },
		{ u"action"_q, action },
		{ u"key_id"_q, keyId },
		{ u"type"_q, u"recaptcha"_q },
	}).toJson(QJsonDocument::Compact);

	SendApiPostJson(url, body, headers, [=](const QByteArray &resp, const QString &err) {
		if (!err.isEmpty() && resp.isEmpty()) {
			done(QString(), u"Ошибка AntiSafety: "_q + err);
			return;
		}
		const auto doc = QJsonDocument::fromJson(resp);
		if (doc.isObject()) {
			const auto obj = doc.object();
			const auto token = obj.contains(u"token"_q)
				? obj[u"token"_q].toString()
				: (obj.contains(u"data"_q) && obj[u"data"_q].isObject() ? obj[u"data"_q].toObject()[u"token"_q].toString() : QString());
			if (!token.isEmpty()) {
				done(token, QString());
				return;
			}
			const auto msg = obj[u"message"_q].toString();
			done(QString(), msg.isEmpty() ? QString::fromUtf8(resp) : msg);
		} else {
			done(QString(), QString::fromUtf8(resp));
		}
	});
}

inline void SolveAppCaptcha(
		const SettingsData &settings,
		const QString &action,
		const QString &keyId,
		Fn<void(QString token, QString error)> done) {
	int service = settings.captchaService;
	QString key = settings.captchaApiKey.trimmed();
	if (service == 0) { // Auto
		if (settings.tokenService == int(MobileTokenProvider::TokenFast) && !settings.tokenApiKey.trimmed().isEmpty()) {
			service = 1;
			key = settings.tokenApiKey.trimmed();
		} else if (settings.tokenService == int(MobileTokenProvider::AntiSafety) && !settings.tokenApiKey.trimmed().isEmpty()) {
			service = 2;
			key = settings.tokenApiKey.trimmed();
		} else if (!settings.captchaApiKey.trimmed().isEmpty()) {
			service = 3;
			key = settings.captchaApiKey.trimmed();
		} else if (!settings.tokenApiKey.trimmed().isEmpty()) {
			service = 1;
			key = settings.tokenApiKey.trimmed();
		}
	} else if (key.isEmpty()) {
		key = settings.tokenApiKey.trimmed();
	}

	if (key.isEmpty()) {
		done(QString(), u"Не задан API-ключ для решения капчи (укажите в настройках токенов или капчи)"_q);
		return;
	}

	if (service == 3) {
		SolveNextCaptcha(key, action, keyId, done);
	} else if (service == 2) {
		SolveAntiSafetyCaptcha(key, action, keyId, done);
	} else {
		SolveTokenFastCaptcha(key, action, keyId, done);
	}
}

inline void CheckCaptchaBalance(
		int provider,
		const QString &apiKey,
		Fn<void(QString info, QString error)> done) {
	if (apiKey.trimmed().isEmpty()) {
		done(QString(), u"Введите API-ключ"_q);
		return;
	}
	if (provider == 3) { // NextCaptcha
		const auto url = QUrl(u"https://api.nextcaptcha.com/getBalance"_q);
		const auto body = QJsonDocument(QJsonObject{
			{ u"clientKey"_q, apiKey.trimmed() },
		}).toJson(QJsonDocument::Compact);
		SendApiPostJson(url, body, {}, [=](const QByteArray &resp, const QString &err) {
			if (!err.isEmpty() && resp.isEmpty()) {
				done(QString(), err);
				return;
			}
			const auto doc = QJsonDocument::fromJson(resp);
			if (doc.isObject()) {
				const auto obj = doc.object();
				if (obj.contains(u"errorId"_q) && obj[u"errorId"_q].toInt() != 0) {
					done(QString(), obj[u"errorDescription"_q].toString());
					return;
				}
				if (obj.contains(u"balance"_q)) {
					const auto bal = obj[u"balance"_q].toVariant().toString();
					done(bal + u" USD"_q, QString());
					return;
				}
			}
			done(QString::fromUtf8(resp), QString());
		});
	} else if (provider == 1) { // TokenFast
		CheckMobileTokenBalance(MobileTokenProvider::TokenFast, apiKey, done);
	} else {
		CheckMobileTokenBalance(MobileTokenProvider::AntiSafety, apiKey, done);
	}
}

// -----------------------------------------------------------------------------
// Numbers that already failed (registered, banned, invalid) - kept between runs
// so they are never paid for twice.
// -----------------------------------------------------------------------------
[[nodiscard]] inline QString BadNumbersPath() {
	return Core::GgtgPaths::DbDir() + u"registration_bad_numbers.json"_q;
}

[[nodiscard]] inline QSet<QString> &BadNumbers() {
	static auto set = [] {
		auto result = QSet<QString>();
		auto file = QFile(BadNumbersPath());
		if (file.open(QIODevice::ReadOnly)) {
			const auto doc = QJsonDocument::fromJson(file.readAll());
			for (const auto &value : doc.array()) {
				result.insert(value.toString());
			}
		}
		return result;
	}();
	return set;
}

[[nodiscard]] inline bool IsBadNumber(const QString &phone) {
	return BadNumbers().contains(phone);
}

inline void AddBadNumber(const QString &phone) {
	if (phone.isEmpty() || BadNumbers().contains(phone)) {
		return;
	}
	BadNumbers().insert(phone);
	QDir().mkpath(Core::GgtgPaths::DbDir());
	auto file = QSaveFile(BadNumbersPath());
	if (!file.open(QIODevice::WriteOnly)) {
		return;
	}
	auto array = QJsonArray();
	for (const auto &number : BadNumbers()) {
		array.push_back(number);
	}
	file.write(QJsonDocument(array).toJson(QJsonDocument::Compact));
	file.commit();
}

// Play Integrity tokens that were bought but not used (the registration was
// stopped or failed before Telegram got the request): the next number takes
// one from here instead of buying a new token.
[[nodiscard]] inline std::vector<QString> &TokenReserve() {
	static auto list = std::vector<QString>();
	return list;
}

[[nodiscard]] inline bool IsNetworkErrorString(const QString &err) {
	const auto l = err.toLower();
	return l.contains(u"host not found"_q)
		|| l.contains(u"connection refused"_q)
		|| l.contains(u"connection timed out"_q)
		|| l.contains(u"network error"_q)
		|| l.contains(u"network unreachable"_q)
		|| l.contains(u"socket error"_q)
		|| l.contains(u"temporary failure"_q)
		|| l.contains(u"name resolution"_q)
		|| l.contains(u"operation canceled"_q)
		|| l.contains(u"network session failed"_q)
		|| l.contains(u"timed out"_q);
}

// -----------------------------------------------------------------------------
// Single Registration Worker (Handles 1 registration from start to finish)
// -----------------------------------------------------------------------------
class SingleRegistrationWorker final : public QObject, public std::enable_shared_from_this<SingleRegistrationWorker> {
private:
	int _workerId = 0;
	SettingsData _settings;
	MTP::ProxyData _proxy;
	Fn<void(int, QString)> _logCb;
	Fn<void(int, QString, bool)> _statusCb;
	Fn<void(int, bool, QString)> _doneCb;
	Fn<void(QString)> _networkLossCb;

	bool _cancelled = false;
	crl::time _orderTime = 0;
	QString _activationId;
	QString _mailId;
	QString _email;
	QString _phone;
	QString _token; // Play Integrity token of this number
	bool _tokenSpent = false; // Telegram got the request: the token is used up
	int _floodRetries = 0;
	QString _captchaToken;
	int _captchaRetries = 0;
	Fn<void()> _sendCode;
	QByteArray _phoneCodeHash;
	QString _firstName;
	QString _lastName;
	MobileDeviceProfile _currentMobileProfile;
	Main::Account *_pendingAccount = nullptr;
	std::unique_ptr<MTP::Sender> _sender;

	QTimer _sendCodeTimer;
	int _sendCodeSeconds = 0;
	mtpRequestId _sendCodeRequestId = 0;

	QTimer _resendTimer;
	int _resendSecondsLeft = 0;
	mtpRequestId _resendRequestId = 0;

	QTimer _pollTimer;
	int _pollSecondsLeft = 0;

	QTimer _emailPollTimer;
	int _emailPollSecondsLeft = 120;
	QTimer _stepDelayTimer;

public:
	SingleRegistrationWorker(
		int workerId,
		const SettingsData &settings,
		const MTP::ProxyData &proxy,
		Fn<void(int workerId, QString msg)> logCb,
		Fn<void(int workerId, QString status, bool isError)> statusCb,
		Fn<void(int workerId, bool success, QString error)> doneCb,
		Fn<void(QString reason)> networkLossCb = nullptr)
	: _workerId(workerId)
	, _settings(settings)
	, _proxy(proxy)
	, _logCb(std::move(logCb))
	, _statusCb(std::move(statusCb))
	, _doneCb(std::move(doneCb))
	, _networkLossCb(std::move(networkLossCb))
	, _sendCodeTimer(this)
	, _resendTimer(this)
	, _pollTimer(this)
	, _pollSecondsLeft(_settings.smsWaitSeconds)
	, _emailPollTimer(this)
	, _stepDelayTimer(this) {
		connect(&_pollTimer, &QTimer::timeout, [this] { onPollTick(); });
		connect(&_emailPollTimer, &QTimer::timeout, [this] { onEmailPollTick(); });
	}

	~SingleRegistrationWorker() {
		cancel();
	}

	[[nodiscard]] int id() const { return _workerId; }
	[[nodiscard]] const MTP::ProxyData &proxy() const { return _proxy; }
	[[nodiscard]] QString phone() const { return _phone; }

	void log(const QString &msg) {
		if (_logCb) _logCb(_workerId, msg);
	}

	void status(const QString &msg, bool isError) {
		if (_statusCb) _statusCb(_workerId, msg, isError);
	}

	// A token that never reached Telegram goes to the reserve for the next number.
	void reserveUnusedToken() {
		if (_token.isEmpty() || _tokenSpent) {
			return;
		}
		TokenReserve().push_back(_token);
		log(u"Неиспользованный токен отправлен в резерв (в резерве: "_q
			+ QString::number(TokenReserve().size()) + u")"_q);
		_token.clear();
	}

	void cancel() {
		if (_cancelled) return;
		_cancelled = true;
		_sendCodeTimer.stop();
		_resendTimer.stop();
		_pollTimer.stop();
		_emailPollTimer.stop();
		_stepDelayTimer.stop();
		QObject::disconnect(&_sendCodeTimer, nullptr, nullptr, nullptr);
		QObject::disconnect(&_resendTimer, nullptr, nullptr, nullptr);
		QObject::disconnect(&_pollTimer, nullptr, nullptr, nullptr);
		QObject::disconnect(&_emailPollTimer, nullptr, nullptr, nullptr);
		QObject::disconnect(&_stepDelayTimer, nullptr, nullptr, nullptr);
		if (_sendCodeRequestId && _sender) {
			_sender->request(base::take(_sendCodeRequestId)).cancel();
		}
		if (_resendRequestId && _sender) {
			_sender->request(base::take(_resendRequestId)).cancel();
		}
		_sendCodeRequestId = 0;
		_resendRequestId = 0;
		_sender.reset();
		_sendCode = nullptr;
		reserveUnusedToken();
		if (!_activationId.isEmpty()) {
			const auto id = _activationId;
			const auto phone = _phone;
			const auto orderTime = _orderTime;
			const auto settings = _settings;
			const auto logCb = _logCb;
			const auto workerId = _workerId;
			_activationId.clear();
			log(u"Остановка потока: возврат средств за купленный номер +"_q + phone + u" (ID: "_q + id + u")..."_q);
			EnqueueRefund(settings, id, phone, orderTime, [=](QString msg) {
				if (logCb) logCb(workerId, msg);
			});
		}
		if (!_mailId.isEmpty()) {
			CancelEmail(_settings, _mailId);
			_mailId.clear();
		}
		_pendingAccount = nullptr;
	}

	void fail(const QString &err) {
		if (_cancelled) return;
		_sendCodeTimer.stop();
		_resendTimer.stop();
		_pollTimer.stop();
		_emailPollTimer.stop();
		_stepDelayTimer.stop();
		QObject::disconnect(&_sendCodeTimer, nullptr, nullptr, nullptr);
		QObject::disconnect(&_resendTimer, nullptr, nullptr, nullptr);
		QObject::disconnect(&_pollTimer, nullptr, nullptr, nullptr);
		QObject::disconnect(&_emailPollTimer, nullptr, nullptr, nullptr);
		QObject::disconnect(&_stepDelayTimer, nullptr, nullptr, nullptr);
		if (_sendCodeRequestId && _sender) {
			_sender->request(base::take(_sendCodeRequestId)).cancel();
		}
		if (_resendRequestId && _sender) {
			_sender->request(base::take(_resendRequestId)).cancel();
		}
		_sendCodeRequestId = 0;
		_resendRequestId = 0;
		_sender.reset();
		_sendCode = nullptr;
		log(u"ОШИБКА: "_q + err);
		reserveUnusedToken();
		if (!_activationId.isEmpty()) {
			const auto id = _activationId;
			const auto phone = _phone;
			const auto orderTime = _orderTime;
			const auto settings = _settings;
			const auto logCb = _logCb;
			const auto workerId = _workerId;
			_activationId.clear();
			log(u"Отмена активации и возврат средств за номер +"_q + phone + u" (ID: "_q + id + u")..."_q);
			EnqueueRefund(settings, id, phone, orderTime, [=](QString msg) {
				if (logCb) logCb(workerId, msg);
			});
		}
		if (!_mailId.isEmpty()) {
			CancelEmail(_settings, _mailId);
			_mailId.clear();
		}
		_pendingAccount = nullptr;
		if (_doneCb) {
			const auto done = _doneCb;
			const auto workerId = _workerId;
			crl::on_main([=] {
				done(workerId, false, err);
			});
		}
	}

	void onNetworkRestored() {
		if (_cancelled) return;
		log(u"Возобновление работы после восстановления сети..."_q);
		if (!_activationId.isEmpty() && !_phoneCodeHash.isEmpty()) {
			onPollTick();
		} else if (!_activationId.isEmpty()) {
			connectAndSendCode();
		} else {
			proceedOrderNumber();
		}
	}

	void start() {
		if (_cancelled) return;
		if (_settings.precheckProxy && _proxy.type != MTP::ProxyData::Type::None && !_proxy.host.isEmpty()) {
			status(u"Проверка работоспособности прокси перед заказом номера..."_q, false);
			log(u"Предварительная проверка прокси: "_q + _proxy.host + ':' + QString::number(_proxy.port));
			auto socket = new QTcpSocket(this);
			auto timeoutTimer = new QTimer(socket);
			timeoutTimer->setSingleShot(true);
			const auto weak = std::weak_ptr<SingleRegistrationWorker>(shared_from_this());
			auto doneCheck = [weak, socket, timeoutTimer](bool ok, const QString &err) {
				timeoutTimer->stop();
				socket->disconnect();
				socket->deleteLater();
				const auto strong = weak.lock();
				if (!strong || strong->_cancelled) return;
				if (!ok) {
					strong->fail(u"Ошибка прокси ("_q + strong->_proxy.host + ':' + QString::number(strong->_proxy.port) + u"): "_q + err + u". Номер не заказывался (баланс сохранён)."_q);
				} else {
					strong->log(u"Прокси успешно ответил. Переход к заказу номера..."_q);
					strong->proceedOrderNumber();
				}
			};
			connect(socket, &QTcpSocket::connected, [doneCheck] {
				doneCheck(true, QString());
			});
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
			connect(socket, &QAbstractSocket::errorOccurred, [socket, doneCheck](QAbstractSocket::SocketError) {
				doneCheck(false, socket->errorString());
			});
#else
			connect(socket, qOverload<QAbstractSocket::SocketError>(&QAbstractSocket::error), [socket, doneCheck](QAbstractSocket::SocketError) {
				doneCheck(false, socket->errorString());
			});
#endif
			connect(timeoutTimer, &QTimer::timeout, [doneCheck] {
				doneCheck(false, u"Превышено время ожидания (5 сек)"_q);
			});
			timeoutTimer->start(5000);
			socket->connectToHost(_proxy.host, _proxy.port);
			return;
		}
		proceedOrderNumber();
	}

	void proceedOrderNumber() {
		if (_cancelled) return;
		auto reqDesc = u"Запрос номера у SMS-сервиса (сервис: "_q + _settings.serviceId;
		if (_settings.countryId >= 0) {
			reqDesc += u", страна: "_q + CurrentCountryTitle(_settings);
		}
		if (!_settings.smsOperator.trimmed().isEmpty() && _settings.smsOperator.trimmed() != u"any"_q) {
			reqDesc += u", оператор: "_q + _settings.smsOperator.trimmed();
		}
		if (!_settings.maxPrice.trimmed().isEmpty()) {
			reqDesc += u", макс. цена: "_q + _settings.maxPrice.trimmed();
		}
		reqDesc += u")..."_q;
		log(reqDesc);
		status(u"Запрос номера в SMS-сервисе..."_q, false);

		const auto weak = std::weak_ptr<SingleRegistrationWorker>(shared_from_this());
		OrderNumber(_settings, _settings.countryId, [weak](QString id, QString phone, QString err) {
			const auto strong = weak.lock();
			if (!strong) return;
			if (strong->_cancelled) {
				if (!id.isEmpty()) {
					strong->log(u"Номер +"_q + phone + u" (ID: "_q + id + u") получен во время остановки. Оформляем возврат..."_q);
					const auto settings = strong->_settings;
					const auto logCb = strong->_logCb;
					const auto workerId = strong->_workerId;
					EnqueueRefund(settings, id, phone, crl::now(), [=](QString msg) {
						if (logCb) logCb(workerId, msg);
					});
				}
				return;
			}
			if (!err.isEmpty()) {
				if (strong->_networkLossCb && IsNetworkErrorString(err)) {
					strong->_networkLossCb(err);
					return;
				}
				strong->fail(u"Ошибка заказа номера: "_q + err);
				return;
			}
			strong->_activationId = id;
			const auto rawPhone = DigitsOnly(phone);
			strong->_phone = CleanPhoneForTelegram(rawPhone);
			strong->_orderTime = crl::now();
			if (strong->_phone != rawPhone) {
				strong->log(u"Номер куплен: +"_q + rawPhone + u" (скорректирован для Telegram: +"_q + strong->_phone + u", ID заказа: "_q + strong->_activationId + u")"_q);
			} else {
				strong->log(u"Номер куплен: +"_q + strong->_phone + u" (ID заказа: "_q + strong->_activationId + u")"_q);
			}
			if (strong->_settings.filterNumbers && IsBadNumber(strong->_phone)) {
				strong->fail(u"Номер +"_q + strong->_phone + u" уже не подошёл раньше (занят, заблокирован или неверный). Отмена, запрос возврата средств."_q);
				return;
			}
			strong->_captchaToken.clear();
			strong->_captchaRetries = 0;
			strong->status(u"Номер получен: +"_q + strong->_phone + u". Подключение к Telegram..."_q, false);
			const auto delayMs = StepDelayMs(strong->_settings);
			if (delayMs > 0) {
				strong->log(u"Пауза перед отправкой запроса кода в Telegram (%1 сек)..."_q.arg(delayMs / 1000.0, 0, 'f', 1));
				strong->status(u"Пауза перед запросом кода (%1 сек)..."_q.arg(delayMs / 1000.0, 0, 'f', 1), false);
				strong->_stepDelayTimer.stop();
				QObject::disconnect(&strong->_stepDelayTimer, nullptr, nullptr, nullptr);
				strong->connect(&strong->_stepDelayTimer, &QTimer::timeout, [weak] {
					if (const auto s = weak.lock()) {
						s->_stepDelayTimer.stop();
						if (!s->_cancelled) s->connectAndSendCode();
					}
				});
				strong->_stepDelayTimer.start(delayMs);
			} else {
				strong->connectAndSendCode();
			}
		});
	}

	void startManual(const QString &phone) {
		if (_cancelled) return;
		_phone = CleanPhoneForTelegram(phone);
		_phoneCodeHash.clear();
		_mailId.clear();
		_email.clear();
		_captchaToken.clear();
		_captchaRetries = 0;
		_pendingAccount = nullptr;

		log(u"Ручная регистрация номера +"_q + _phone);
		status(u"Подключение и запрос кода для +"_q + _phone + u"..."_q, false);
		connectAndSendCode();
	}

	void submitManualCode(const QString &code, const QString &firstName, const QString &lastName) {
		if (_cancelled || !_pendingAccount || _phoneCodeHash.isEmpty()) {
			status(u"Сначала запросите код в Telegram"_q, true);
			return;
		}
		_firstName = firstName.trimmed();
		_lastName = lastName.trimmed();
		if (_firstName.isEmpty()) _firstName = u"Пользователь"_q;
		submitTelegramCode(code.trimmed());
	}

private:
	void connectAndSendCode() {
		if (_cancelled) return;
		auto &domain = Core::App().domain();
		if (domain.accounts().size() >= domain.maxAccounts()) {
			fail(u"Достигнут лимит аккаунтов в приложении."_q);
			return;
		}

		_pendingAccount = domain.add(MTP::Environment::Production, _proxy);
		_pendingAccount->setFolderTag(_settings.folderTag);
		_pendingAccount->mtp().setUserPhone(_phone);

		_currentMobileProfile = LoadRandomMobileProfile(_settings.baseDbPath);
		auto dev = ToDeviceOverride(_currentMobileProfile);
		const auto geoMatch = !_phone.isEmpty()
			? Core::GgtgGeo::MatchForPhone(_phone)
			: Core::GgtgGeo::MatchForCountry(CountryCodeById(_settings.countryId));
		dev.tzOffset = geoMatch.tzOffset;
		dev.systemLangCode = geoMatch.systemLang;
		dev.cloudLangCode = geoMatch.cloudLang;
		_pendingAccount->setDeviceOverride(dev);
		const auto apiId = _currentMobileProfile.appId;
		const auto apiHash = _currentMobileProfile.appHash;

		if (_proxy.type != MTP::ProxyData::Type::None && !_proxy.host.isEmpty()) {
			log(u"Прокси: "_q + _proxy.host + ':' + QString::number(_proxy.port));
		}
		log(u"Устройство: "_q + _currentMobileProfile.device + u" ("_q + _currentMobileProfile.sdk + u") | Гео: "_q + geoMatch.country + u" → "_q + geoMatch.systemLang + u", UTC"_q + QString::number(geoMatch.tzOffset / 3600));

		_sender = std::make_unique<MTP::Sender>(&_pendingAccount->mtp());

		const auto weak = std::weak_ptr<SingleRegistrationWorker>(shared_from_this());
		_sendCode = [weak, apiId, apiHash] {
			const auto strong = weak.lock();
			if (!strong || strong->_cancelled) return;
			strong->log(u"Отправка запроса MTPauth_SendCode в Telegram для +"_q + strong->_phone + u"..."_q);
			strong->status(u"Запрос кода в Telegram для +"_q + strong->_phone + u" (ожидание ответа)..."_q, false);

			strong->_sendCodeTimer.stop();
			strong->_sendCodeSeconds = 0;

			MTPDcodeSettings::Flags flags;
			flags |= MTPDcodeSettings::Flag::f_allow_firebase;
			if (!strong->_token.isEmpty()) {
				flags |= MTPDcodeSettings::Flag::f_token;
			}

			auto sendQuery = MTPauth_SendCode(
				MTP_string(strong->_phone),
				MTP_int(apiId),
				MTP_string(apiHash),
				MTP_codeSettings(
					MTP_flags(flags),
					MTPVector<MTPbytes>(),
					MTP_string(strong->_token),
					MTP_boolFalse())
			);

			const auto handleDone = [weak](const MTPauth_SentCode &result) {
				const auto strong = weak.lock();
				if (!strong) return;
				strong->_sendCodeTimer.stop();
				strong->_sendCodeRequestId = 0;
				if (strong->_cancelled) return;
				strong->_captchaToken.clear();
				strong->_captchaRetries = 0;
				result.match([&](const MTPDauth_sentCode &data) {
					strong->processSentCode(data);
				}, [&](const MTPDauth_sentCodeSuccess &data) {
					strong->_sendCodeTimer.stop();
					strong->_sendCodeRequestId = 0;
					strong->finishRegistration(data.vauthorization());
				}, [&](const MTPDauth_sentCodePaymentRequired &) {
					strong->_sendCodeTimer.stop();
					strong->_sendCodeRequestId = 0;
					AddBadNumber(strong->_phone);
					strong->fail(u"Telegram требует платную отправку SMS / Premium для номера +"_q + strong->_phone + u". Номер отменён, средства возвращены."_q);
				}, [&](const auto &) {
					strong->_sendCodeTimer.stop();
					strong->_sendCodeRequestId = 0;
					AddBadNumber(strong->_phone);
					strong->fail(u"Неподдерживаемый ответ при запросе кода Telegram для номера +"_q + strong->_phone + u". Отмена номера, средства возвращены."_q);
				});
			};

			const auto handleFail = [weak](const MTP::Error &error) {
				const auto strong = weak.lock();
				if (!strong) return;
				strong->_sendCodeTimer.stop();
				strong->_sendCodeRequestId = 0;
				if (strong->_cancelled) return;
				const auto &err = error.type();
				const auto &desc = error.description();
				const auto fullErr = desc.isEmpty()
					? err
					: (err + u" ("_q + desc + u")"_q);
				strong->log(u"Telegram вернул ошибку: "_q + fullErr + u" [код "_q + QString::number(error.code()) + u"]"_q);
				if (strong->_networkLossCb && IsNetworkErrorString(err)) {
					strong->_networkLossCb(err);
					return;
				}

				// Check for Telegram reCAPTCHA challenge
				static const auto RecaptchaRegExp = QRegularExpression(
					u"RECAPTCHA_CHECK_([a-zA-Z0-9]+)__([a-zA-Z0-9_\\-]+)"_q);
				const auto captchaMatch = RecaptchaRegExp.match(fullErr);
				if (captchaMatch.hasMatch()) {
					const auto action = captchaMatch.captured(1);
					const auto keyId = captchaMatch.captured(2);
					if (strong->_captchaRetries < 2) {
						++strong->_captchaRetries;
						const auto sName = CaptchaProviderName(strong->_settings.captchaService);
						strong->log(u"Telegram запросил reCAPTCHA Enterprise ("_q + action + u"). Решение через "_q + sName + u"..."_q);
						strong->status(u"Решение reCAPTCHA ("_q + action + u")..."_q, false);
						SolveAppCaptcha(strong->_settings, action, keyId, [weak, action](QString token, QString solveErr) {
							const auto s = weak.lock();
							if (!s || s->_cancelled) return;
							if (!solveErr.isEmpty() || token.isEmpty()) {
								s->fail(u"Не удалось решить капчу Telegram: "_q + (solveErr.isEmpty() ? u"пустой токен"_q : solveErr));
								return;
							}
							s->log(u"Токен reCAPTCHA успешно получен! Повторная отправка запроса в Telegram..."_q);
							s->_captchaToken = token;
							s->_sendCode();
						});
						return;
					} else {
						strong->fail(u"Ошибка: не удалось пройти reCAPTCHA Telegram после 2 попыток."_q);
						return;
					}
				}

				if (err == u"PHONE_NUMBER_BANNED"_q || err == u"PHONE_NUMBER_INVALID"_q) {
					AddBadNumber(strong->_phone);
				}
				if (err == u"PHONE_NUMBER_FLOOD"_q
					&& strong->_settings.replaceTokenOnFlood
					&& strong->_settings.tokenService != 0
					&& !strong->_settings.tokenApiKey.trimmed().isEmpty()
					&& strong->_floodRetries < 2) {
					++strong->_floodRetries;
					strong->log(u"Флуд на токене: заменяю токен новым (попытка "_q
						+ QString::number(strong->_floodRetries) + u" из 2)..."_q);
					strong->_token.clear(); // the flooded token is dropped, not reserved
					strong->obtainTokenAndSend();
					return;
				}
				if (err == u"PHONE_NUMBER_BANNED"_q) {
					strong->fail(u"Номер +"_q + strong->_phone + u" заблокирован в Telegram. Средства возвращены."_q);
				} else if (err == u"PHONE_NUMBER_FLOOD"_q) {
					strong->fail(u"Флуд-лимит Telegram для номера +"_q + strong->_phone);
				} else if (err == u"PHONE_NUMBER_INVALID"_q) {
					strong->fail(u"Неверный номер телефона (PHONE_NUMBER_INVALID)."_q);
				} else {
					strong->fail(u"Ошибка Telegram: "_q + fullErr);
				}
			};

			if (!strong->_captchaToken.isEmpty()) {
				strong->_sendCodeRequestId = strong->_sender->request(
					MTPInvokeWithReCaptcha<MTPauth_SendCode>(
						MTP_string(strong->_captchaToken),
						std::move(sendQuery))
				).done(handleDone).fail(handleFail).send();
			} else {
				strong->_sendCodeRequestId = strong->_sender->request(
					std::move(sendQuery)
				).done(handleDone).fail(handleFail).send();
			}

			QObject::disconnect(&strong->_sendCodeTimer, nullptr, nullptr, nullptr);
			strong->connect(&strong->_sendCodeTimer, &QTimer::timeout, [weak] {
				const auto s = weak.lock();
				if (!s) return;
				if (s->_cancelled) {
					s->_sendCodeTimer.stop();
					return;
				}
				++s->_sendCodeSeconds;
				s->status(u"Запрос кода в Telegram для +"_q + s->_phone + u" (ожидание ответа: "_q + QString::number(s->_sendCodeSeconds) + u"/25с)..."_q, false);
				if (s->_sendCodeSeconds >= 25) {
					s->_sendCodeTimer.stop();
					if (s->_sendCodeRequestId && s->_sender) {
						s->_sender->request(base::take(s->_sendCodeRequestId)).cancel();
					}
					const auto dcState = s->_pendingAccount ? s->_pendingAccount->mtp().dcstate() : 0;
					if (dcState == 0 || dcState == 1) {
						s->fail(u"Тайм-аут подключения к серверам Telegram (25 сек). Прокси не отвечает по MTProto протоколу или соединение разорвано."_q);
					} else {
						s->fail(u"Тайм-аут: Telegram не ответил на MTPauth_SendCode за 25 сек (возможно, номер заблокирован или оператор отклонил SMS)."_q);
					}
				}
			});
			strong->_sendCodeTimer.start(1000);
		};

		obtainTokenAndSend();
	}

	// Takes a token from the reserve (or buys one) and asks Telegram for the code.
	void obtainTokenAndSend() {
		if (_cancelled) return;
		if (_settings.tokenService == 0 || _settings.tokenApiKey.trimmed().isEmpty()) {
			_sendCode();
			return;
		}
		if (!TokenReserve().empty()) {
			_token = TokenReserve().back();
			TokenReserve().pop_back();
			log(u"Токен взят из резерва (осталось в резерве: "_q
				+ QString::number(TokenReserve().size()) + u")"_q);
			_sendCode();
			return;
		}
		status(u"Запрос токена в сервисе токенов..."_q, false);
		log(u"Запрос Play Integrity токена ("_q + TokenProviderName(_settings.tokenService) + u") для +"_q + _phone + u"..."_q);
		const auto weak = std::weak_ptr<SingleRegistrationWorker>(shared_from_this());
		FetchMobileToken(
			MobileTokenProvider(_settings.tokenService),
			_settings.tokenApiKey,
			_phone,
			[weak](QString token, QString err) {
				const auto strong = weak.lock();
				if (!strong || strong->_cancelled) return;
				if (!err.isEmpty()) {
					strong->log(u"Предупреждение сервиса токенов: "_q + err + u" (продолжаем без токена)"_q);
				} else if (!token.isEmpty()) {
					strong->_token = token;
					strong->log(u"Токен Play Integrity успешно получен и привязан"_q);
				}
				strong->_sendCode();
			});
	}

	void handleEmailRequired() {
		if (_cancelled) return;
		if (!_settings.autoBuyEmail) {
			fail(u"Telegram запросил Email, но покупка почты выключена в настройках."_q);
			return;
		}
		status(u"Запрос временной почты в сервисе..."_q, false);
		log(u"Запрос временной почты в сервисе..."_q);

		const auto weak = std::weak_ptr<SingleRegistrationWorker>(shared_from_this());
		OrderEmail(_settings, [weak](QString mailId, QString email, QString err) {
			const auto strong = weak.lock();
			if (!strong || strong->_cancelled) return;
			if (!err.isEmpty() || email.isEmpty()) {
				strong->fail(u"Не удалось получить почту: "_q + (err.isEmpty() ? u"пустой ответ"_q : err));
				return;
			}
			strong->_mailId = mailId;
			strong->_email = email;
			strong->log(u"Временная почта получена: "_q + email + u" (ID: "_q + mailId + u")"_q);
			strong->sendEmailToTelegram(email);
		});
	}

	void sendEmailToTelegram(const QString &email) {
		if (_cancelled) return;
		log(u"Отправка MTPaccount_SendVerifyEmailCode для "_q + email + u"..."_q);
		const auto weak = std::weak_ptr<SingleRegistrationWorker>(shared_from_this());
		_sender->request(MTPaccount_SendVerifyEmailCode(
			MTP_emailVerifyPurposeLoginSetup(
				MTP_string(_phone),
				MTP_bytes(_phoneCodeHash)),
			MTP_string(email)
		)).done([weak](const MTPaccount_SentEmailCode &sent) {
			const auto strong = weak.lock();
			if (!strong || strong->_cancelled) return;
			const auto length = sent.data().vlength().v;
			const auto pattern = qs(sent.data().vemail_pattern());
			strong->log(u"Telegram отправил код подтверждения на почту (длина: "_q
				+ QString::number(length) + u", маска: "_q + pattern + u")"_q);
			strong->status(u"Ожидание кода из почты "_q + strong->_email + u" (120 сек)..."_q, false);

			strong->_emailPollSecondsLeft = 120;
			strong->_emailPollTimer.start(3000);
		}).fail([weak](const MTP::Error &error) {
			const auto strong = weak.lock();
			if (!strong || strong->_cancelled) return;
			strong->fail(u"Ошибка Telegram при привязке email: "_q + error.type());
		}).send();
	}

	void onEmailPollTick() {
		if (_cancelled || _mailId.isEmpty()) {
			_emailPollTimer.stop();
			return;
		}
		_emailPollSecondsLeft -= 3;
		if (_emailPollSecondsLeft <= 0) {
			_emailPollTimer.stop();
			CancelEmail(_settings, _mailId);
			_mailId.clear();
			fail(u"Время ожидания кода из почты истекло (120 сек)."_q);
			return;
		}
		status(u"Ожидание кода на почту ("_q + QString::number(_emailPollSecondsLeft) + u" сек)..."_q, false);

		const auto weak = std::weak_ptr<SingleRegistrationWorker>(shared_from_this());
		PollEmailCode(_settings, _mailId, [weak](QString st, QString code, QString err) {
			const auto strong = weak.lock();
			if (!strong || strong->_cancelled) return;
			if (st == u"ok"_q && !code.isEmpty()) {
				strong->_emailPollTimer.stop();
				strong->log(u"Код подтверждения из почты получен: "_q + code);
				strong->verifyEmailInTelegram(code);
			} else if (st == u"cancel"_q) {
				strong->_emailPollTimer.stop();
				strong->fail(u"Почтовая сессия отменена сервисом."_q);
			}
		});
	}

	void verifyEmailInTelegram(const QString &code) {
		if (_cancelled) return;
		log(u"Верификация почты кодом "_q + code + u" в Telegram..."_q);
		const auto weak = std::weak_ptr<SingleRegistrationWorker>(shared_from_this());
		_sender->request(MTPaccount_VerifyEmail(
			MTP_emailVerifyPurposeLoginSetup(
				MTP_string(_phone),
				MTP_bytes(_phoneCodeHash)),
			MTP_emailVerificationCode(MTP_string(code))
		)).done([weak](const MTPaccount_EmailVerified &result) {
			const auto strong = weak.lock();
			if (!strong || strong->_cancelled) return;
			result.match([&](const MTPDaccount_emailVerified &) {
				strong->log(u"Email подтверждён!"_q);
				strong->startSmsPolling();
			}, [&](const MTPDaccount_emailVerifiedLogin &login) {
				login.vsent_code().match([&](const MTPDauth_sentCode &sentData) {
					strong->processSentCode(sentData);
				}, [&](const MTPDauth_sentCodeSuccess &sentData) {
					strong->finishRegistration(sentData.vauthorization());
				}, [&](const MTPDauth_sentCodePaymentRequired &) {
					AddBadNumber(strong->_phone);
					strong->fail(u"Telegram требует платную отправку SMS / Premium для номера +"_q + strong->_phone + u". Номер отменён, средства возвращены."_q);
				}, [&](const auto &) {
					AddBadNumber(strong->_phone);
					strong->fail(u"Неподдерживаемый тип отправки кода Telegram для номера +"_q + strong->_phone + u". Номер отменён, средства возвращены."_q);
				});
			});
		}).fail([weak](const MTP::Error &error) {
			const auto strong = weak.lock();
			if (!strong || strong->_cancelled) return;
			strong->fail(u"Ошибка подтверждения email: "_q + error.type());
		}).send();
	}

	void processSentCode(const MTPDauth_sentCode &data) {
		if (_cancelled) return;
		_phoneCodeHash = qba(data.vphone_code_hash());
		const auto &codeType = data.vtype();
		if (codeType.type() == mtpc_auth_sentCodeTypeSetUpEmailRequired) {
			log(u"Telegram требует подтверждения Email для номера +"_q + _phone);
			handleEmailRequired();
			return;
		}
		_tokenSpent = true;
		if (codeType.type() == mtpc_auth_sentCodeTypeApp) {
			AddBadNumber(_phone);
			if (_settings.skipAppCode && !_activationId.isEmpty()) {
				log(u"Telegram отправил код в активное приложение на номере +"_q + _phone + u" (номер уже кем-то используется). Отклоняем номер и возвращаем средства..."_q);
				fail(u"Номер уже зарегистрирован в Telegram (код ушёл в приложение). Номер отменён, средства возвращены."_q);
				return;
			}
			if (!_settings.skipAppCode && !_activationId.isEmpty()) {
				const auto next = data.vnext_type();
				if (!next || next->type() != mtpc_auth_codeTypeSms) {
					log(u"Telegram отправил код в приложение и заблокировал отправку SMS для номера +"_q + _phone + u". Отмена номера и возврат средств..."_q);
					fail(u"Telegram не разрешает отправку SMS для занятого номера. Отмена, средства возвращены."_q);
					return;
				}
				const auto timeout = std::max(5, data.vtimeout().value_or(60));
				waitAndResendCode(timeout);
				return;
			}
			log(u"Telegram отправил код в приложение на номере +"_q + _phone + u". Введите код из приложения."_q);
			status(u"Код отправлен в приложение Telegram! Введите код."_q, false);
			return;
		} else if (codeType.type() == mtpc_auth_sentCodeTypeSms) {
			log(u"Telegram подтвердил отправку SMS на номер +"_q + _phone + u"! Ожидаем поступления SMS от сервиса..."_q);
		} else if (codeType.type() == mtpc_auth_sentCodeTypeCall) {
			log(u"Telegram отправляет звонок на номер +"_q + _phone);
		} else if (codeType.type() == mtpc_auth_sentCodeTypeFlashCall) {
			log(u"Telegram выполняет Flash-call на номер +"_q + _phone);
		} else if (codeType.type() == mtpc_auth_sentCodeTypeMissedCall) {
			log(u"Telegram выполняет обратный звонок (missed call) на номер +"_q + _phone);
		} else {
			log(u"Telegram подтвердил запрос кода. Ожидаем SMS..."_q);
		}

		startSmsPolling();
	}

	void startSmsPolling() {
		if (_cancelled) return;
		if (!_activationId.isEmpty()) {
			SetSmsStatus(_settings, _activationId, 1);
			status(u"Ожидание SMS-кода ("_q + QString::number(_settings.smsWaitSeconds) + u" сек)..."_q, false);
			_pollSecondsLeft = _settings.smsWaitSeconds;
			_pollTimer.start(3000);
		} else {
			status(u"Код отправлен Telegram! Введите код."_q, false);
		}
	}

	void waitAndResendCode(int timeout) {
		if (_cancelled) return;
		_resendSecondsLeft = timeout;
		log(u"Telegram отправил код в приложение. Ожидаем тайм-аут Telegram ("_q + QString::number(_resendSecondsLeft) + u" сек) для повторного запроса через SMS..."_q);
		status(u"Ожидание тайм-аута Telegram ("_q + QString::number(_resendSecondsLeft) + u" сек) для запроса SMS..."_q, false);

		_resendTimer.stop();
		QObject::disconnect(&_resendTimer, nullptr, nullptr, nullptr);
		const auto weak = std::weak_ptr<SingleRegistrationWorker>(shared_from_this());
		connect(&_resendTimer, &QTimer::timeout, [weak] {
			const auto strong = weak.lock();
			if (!strong || strong->_cancelled) return;
			strong->_resendSecondsLeft -= 1;
			if (strong->_resendSecondsLeft > 0) {
				strong->status(u"Ожидание тайм-аута Telegram ("_q + QString::number(strong->_resendSecondsLeft) + u" сек) для запроса SMS..."_q, false);
				return;
			}
			strong->_resendTimer.stop();
			strong->requestResendCode();
		});
		_resendTimer.start(1000);
	}

	void requestResendCode() {
		if (_cancelled || !_sender) return;
		log(u"Тайм-аут Telegram истёк. Отправка запроса MTPauth_ResendCode (SMS) для номера +"_q + _phone + u"..."_q);
		status(u"Запрос SMS в Telegram (MTPauth_ResendCode)..."_q, false);

		const auto weak = std::weak_ptr<SingleRegistrationWorker>(shared_from_this());
		_resendRequestId = _sender->request(MTPauth_ResendCode(
			MTP_flags(0),
			MTP_string(_phone),
			MTP_bytes(_phoneCodeHash),
			MTPstring() // reason
		)).done([weak](const MTPauth_SentCode &result) {
			const auto strong = weak.lock();
			if (!strong || strong->_cancelled) return;
			strong->_resendRequestId = 0;
			result.match([&](const MTPDauth_sentCode &sentData) {
				strong->_phoneCodeHash = qba(sentData.vphone_code_hash());
				const auto &cType = sentData.vtype();
				if (cType.type() == mtpc_auth_sentCodeTypeApp) {
					AddBadNumber(strong->_phone);
					strong->fail(u"Telegram повторно отправил код в приложение и отказал в отправке SMS. Номер отменён, средства возвращены."_q);
					return;
				} else if (cType.type() == mtpc_auth_sentCodeTypeSms) {
					strong->log(u"Telegram подтвердил отправку SMS на номер +"_q + strong->_phone + u"! Ожидаем поступления SMS..."_q);
				} else {
					strong->log(u"Telegram ответил на MTPauth_ResendCode (тип: "_q + QString::number(cType.type()) + u"). Ожидаем SMS..."_q);
				}
				strong->startSmsPolling();
			}, [&](const MTPDauth_sentCodeSuccess &sentData) {
				strong->finishRegistration(sentData.vauthorization());
			}, [&](const MTPDauth_sentCodePaymentRequired &) {
				AddBadNumber(strong->_phone);
				strong->fail(u"Telegram требует платную отправку SMS / Premium для номера +"_q + strong->_phone + u". Номер отменён, средства возвращены."_q);
			}, [&](const auto &) {
				AddBadNumber(strong->_phone);
				strong->fail(u"Не удалось запросить SMS повторно через Telegram. Номер отменён, средства возвращены."_q);
			});
		}).fail([weak](const MTP::Error &error) {
			const auto strong = weak.lock();
			if (!strong || strong->_cancelled) return;
			strong->_resendRequestId = 0;
			AddBadNumber(strong->_phone);
			strong->fail(u"Ошибка повторного запроса SMS в Telegram: "_q + error.type() + u". Номер отменён, средства возвращены."_q);
		}).send();
	}

	void onPollTick() {
		if (_cancelled || _activationId.isEmpty()) {
			_pollTimer.stop();
			return;
		}
		_pollSecondsLeft -= 3;
		if (_pollSecondsLeft <= 0) {
			_pollTimer.stop();
			log(u"SMS не получено за "_q + QString::number(_settings.smsWaitSeconds) + u" сек. Автоматический возврат средств (статус 8)..."_q);
			fail(u"Время ожидания SMS истекло ("_q + QString::number(_settings.smsWaitSeconds) + u" сек). Номер отменен, средства возвращены."_q);
			return;
		}

		status(u"Ожидание SMS-кода ("_q + QString::number(_pollSecondsLeft) + u" сек)..."_q, false);
		if (_pollSecondsLeft % 15 == 0 || _pollSecondsLeft == _settings.smsWaitSeconds - 3) {
			log(u"Ожидание SMS для +"_q + _phone + u"... (осталось "_q + QString::number(_pollSecondsLeft) + u" сек)"_q);
		}

		const auto weak = std::weak_ptr<SingleRegistrationWorker>(shared_from_this());
		PollSmsCode(_settings, _activationId, [weak](QString st, QString code, QString err) {
			const auto strong = weak.lock();
			if (!strong || strong->_cancelled) return;
			if (!err.isEmpty() && IsNetworkErrorString(err)) {
				if (strong->_networkLossCb) {
					strong->_networkLossCb(err);
					return;
				}
			}
			if (st == u"ok"_q && !code.isEmpty()) {
				strong->_pollTimer.stop();
				strong->log(u"SMS-код успешно получен: "_q + code);

				const auto delayMs = StepDelayMs(strong->_settings);
				if (delayMs > 0) {
					strong->log(u"Пауза между шагами перед вводом SMS (%1 сек)..."_q.arg(delayMs / 1000.0, 0, 'f', 1));
					strong->status(u"Имитация ввода SMS человеком ("_q + QString::number(delayMs / 1000.0, 'f', 1) + u" сек)..."_q, false);
					strong->_stepDelayTimer.stop();
					QObject::disconnect(&strong->_stepDelayTimer, nullptr, nullptr, nullptr);
					strong->connect(&strong->_stepDelayTimer, &QTimer::timeout, [weak, code] {
						if (const auto s = weak.lock()) {
							s->_stepDelayTimer.stop();
							if (!s->_cancelled) s->submitTelegramCode(code);
						}
					});
					strong->_stepDelayTimer.start(delayMs);
				} else {
					strong->submitTelegramCode(code);
				}
			} else if (st == u"cancel"_q) {
				strong->_pollTimer.stop();
				strong->fail(u"Активация была отменена сервисом."_q);
			}
		});
	}

	void submitTelegramCode(const QString &code) {
		if (_cancelled) return;
		log(u"Авторизация в Telegram с кодом "_q + code + u"...");
		status(u"Авторизация в Telegram..."_q, false);

		const auto weak = std::weak_ptr<SingleRegistrationWorker>(shared_from_this());
		_sender->request(MTPauth_SignIn(
			MTP_flags(MTPauth_SignIn::Flag::f_phone_code),
			MTP_string(_phone),
			MTP_bytes(_phoneCodeHash),
			MTP_string(code),
			MTP_emailVerificationCode(MTPstring())
		)).done([weak](const MTPauth_Authorization &result) {
			const auto strong = weak.lock();
			if (!strong || strong->_cancelled) return;
			result.match([&](const MTPDauth_authorization &auth) {
				strong->log(u"Номер уже зарегистрирован в Telegram. Вход выполнен!"_q);
				strong->finishRegistration(auth.vuser());
			}, [](const auto &) {
			});
		}).fail([weak](const MTP::Error &error) {
			const auto strong = weak.lock();
			if (!strong || strong->_cancelled) return;
			if (error.type() == u"PHONE_NUMBER_UNOCCUPIED"_q) {
				strong->handleSignUp();
			} else if (error.type() == u"EMAIL_UNCONFIRMED"_q) {
				strong->handleEmailRequired();
			} else if (error.type() == u"SESSION_PASSWORD_NEEDED"_q) {
				if (strong->_settings.reset2faOnOccupied) {
					strong->log(u"Номер с 2FA — отправляется запрос на сброс пароля (ResetPasswordRequest)..."_q);
					strong->_sender->request(MTPaccount_ResetPassword(
					)).done([weak](const MTPaccount_ResetPasswordResult &) {
						const auto s = weak.lock();
						if (!s || s->_cancelled) return;
						s->log(u"Запрос сброса 2FA отправлен. Номер отменён (средства возвращены)."_q);
						s->fail(u"Номер имеет 2FA. Запрос сброса пароля отправлен."_q);
					}).fail([weak](const MTP::Error &e2) {
						const auto s = weak.lock();
						if (!s || s->_cancelled) return;
						s->log(u"Не удалось отправить сброс 2FA: "_q + e2.type());
						s->fail(u"На номере установлен 2FA пароль, сброс не удался."_q);
					}).send();
				} else {
					strong->fail(u"На номере установлен чужой 2FA пароль. Номер отменен."_q);
				}
			} else if (error.type() == u"PHONE_CODE_INVALID"_q) {
				strong->fail(u"Неверный SMS-код."_q);
			} else if (error.type() == u"PHONE_CODE_EXPIRED"_q) {
				strong->fail(u"Срок действия SMS-кода истёк."_q);
			} else {
				const auto full = error.description().isEmpty() ? error.type() : (error.type() + u" ("_q + error.description() + u")"_q);
				strong->fail(u"Ошибка входа: "_q + full);
			}
		}).send();
	}

	void handleSignUp() {
		if (_cancelled) return;
		if (_settings.singleNameForAll) {
			_firstName = _settings.customFirstName.trimmed().isEmpty() ? GenerateRandomFirstName(_settings.countryId, _settings.gender) : _settings.customFirstName.trimmed();
			_lastName = _settings.customLastName.trimmed().isEmpty() ? GenerateRandomLastName(_settings.countryId, _settings.gender) : _settings.customLastName.trimmed();
		} else {
			_firstName = ReadRandomLineFromFile(_settings.firstNameFile);
			if (_firstName.isEmpty()) {
				_firstName = GenerateRandomFirstName(_settings.countryId, _settings.gender);
			}
			_lastName = ReadRandomLineFromFile(_settings.lastNameFile);
			if (_lastName.isEmpty()) {
				_lastName = GenerateRandomLastName(_settings.countryId, _settings.gender);
			}
		}

		const auto weak = std::weak_ptr<SingleRegistrationWorker>(shared_from_this());
		const auto performSignUp = [weak] {
			const auto strong = weak.lock();
			if (!strong || strong->_cancelled) return;
			strong->log(u"Регистрация нового пользователя ("_q + strong->_firstName + ' ' + strong->_lastName + u")..."_q);
			strong->status(u"Регистрация нового пользователя: "_q + strong->_firstName + ' ' + strong->_lastName + u"..."_q, false);

			strong->_sender->request(MTPauth_SignUp(
				MTP_flags(0),
				MTP_string(strong->_phone),
				MTP_bytes(strong->_phoneCodeHash),
				MTP_string(strong->_firstName),
				MTP_string(strong->_lastName)
			)).done([weak](const MTPauth_Authorization &result) {
				const auto s = weak.lock();
				if (!s || s->_cancelled) return;
				result.match([&](const MTPDauth_authorization &auth) {
					s->log(u"Новый аккаунт успешно создан в Telegram!"_q);
					s->finishRegistration(auth.vuser());
				}, [](const auto &) {
				});
			}).fail([weak](const MTP::Error &err) {
				const auto s = weak.lock();
				if (!s || s->_cancelled) return;
				if (err.type() == u"EMAIL_UNCONFIRMED"_q) {
					s->handleEmailRequired();
				} else {
					const auto full = err.description().isEmpty() ? err.type() : (err.type() + u" ("_q + err.description() + u")"_q);
					s->fail(u"Ошибка регистрации: "_q + full);
				}
			}).send();
		};

		const auto delayMs = StepDelayMs(_settings);
		if (delayMs > 0) {
			log(u"Пауза перед регистрацией профиля (%1 сек)..."_q.arg(delayMs / 1000.0, 0, 'f', 1));
			status(u"Имитация ввода имени (%1 сек)..."_q.arg(delayMs / 1000.0, 0, 'f', 1), false);
			_stepDelayTimer.stop();
			QObject::disconnect(&_stepDelayTimer, nullptr, nullptr, nullptr);
			connect(&_stepDelayTimer, &QTimer::timeout, [performSignUp, weak] {
				if (const auto s = weak.lock()) {
					s->_stepDelayTimer.stop();
					performSignUp();
				}
			});
			_stepDelayTimer.start(delayMs);
		} else {
			performSignUp();
		}
	}

	void finishRegistration(const MTPauth_Authorization &auth) {
		auth.match([this](const MTPDauth_authorization &a) {
			finishRegistration(a.vuser());
		}, [&](const auto &) {
			fail(u"Не удалось распознать авторизацию Telegram."_q);
		});
	}

	void finishRegistration(const MTPUser &user) {
		if (_cancelled) return;
		user.match([this, &user](const MTPDuser &u) {
			auto settings = std::make_unique<Main::SessionSettings>();
			settings->setDialogsFiltersEnabled(false);
			_pendingAccount->createSession(user, std::move(settings));

			if (!_settings.folderTag.isEmpty()) {
				_pendingAccount->setFolderTag(_settings.folderTag);
			}

			// 1. Установка 2FA пароля
			const auto actual2fa = _settings.single2faForAll
				? _settings.twoFactorPassword.trimmed()
				: GenerateStrongPassword(18);
			if (!actual2fa.isEmpty()) {
				_pendingAccount->setTwoFactorPassword(actual2fa);
				RecordTwoFactorPassword(
					actual2fa,
					_phone,
					_settings.folderTag,
					u"Привязан к аккаунту +"_q + _phone + u" ("_q + qs(u.vfirst_name().value_or_empty()) + u")"_q);

				if (_pendingAccount->sessionExists()) {
					_pendingAccount->session().api().cloudPassword().set(
						QString(),
						actual2fa,
						QString(),
						false,
						QString());
					log(u"Пароль 2FA установлен: "_q + actual2fa);
				}
			}

			if (_settings.resetSessionsAfterReg && _pendingAccount->sessionExists()) {
				_pendingAccount->session().api().request(MTPauth_ResetAuthorizations(
				)).done([](const MTPBool &) {
				}).fail([](const MTP::Error &) {
				}).send();
				log(u"Другие сессии сброшены (ResetAuthorizations)."_q);
			}

			if (!_activationId.isEmpty()) {
				SetSmsStatus(_settings, _activationId, 6); // Finished!
			}

			const auto firstName = qs(u.vfirst_name().value_or_empty());
			const auto lastName = qs(u.vlast_name().value_or_empty());
			const auto username = qs(u.vusername().value_or_empty());

			// 1.5 Auto Bio & Username setup (TG-Giant feature parity)
			const auto weak = std::weak_ptr<SingleRegistrationWorker>(shared_from_this());
			if (_settings.autoSetBio && _pendingAccount->sessionExists()) {
				const auto bioText = !_settings.customBio.trimmed().isEmpty()
					? _settings.customBio.trimmed().left(70)
					: u"Available on Telegram."_q;
				_pendingAccount->session().api().request(MTPaccount_UpdateProfile(
					MTP_flags(MTPaccount_UpdateProfile::Flag::f_about),
					MTPstring(),
					MTPstring(),
					MTP_string(bioText)
				)).done([weak, bioText](const MTPUser &) {
					if (const auto s = weak.lock()) {
						s->log(u"Установлен профиль Bio: «"_q + bioText + u"»"_q);
					}
				}).send();
			}
			if (_settings.autoSetUsername && _pendingAccount->sessionExists()) {
				const auto cleanFirst = firstName.toLower().remove(QRegularExpression(u"[^a-z0-9]"_q));
				const auto cleanLast = lastName.toLower().remove(QRegularExpression(u"[^a-z0-9]"_q));
				auto base = (cleanFirst + '_' + cleanLast).trimmed();
				if (base.isEmpty()) base = u"user"_q;
				const auto candidate = (base + '_' + QString::number(QRandomGenerator::global()->bounded(100, 9999))).left(32);
				_pendingAccount->session().api().request(MTPaccount_UpdateUsername(
					MTP_string(candidate)
				)).done([weak, candidate](const MTPUser &) {
					if (const auto s = weak.lock()) {
						s->log(u"Установлен @username: @"_q + candidate);
					}
				}).send();
			}

			// 2. Сохранение сессии в хранилище сессий (Session Vault)
			auto mobileStored = Core::GgtgSessions::StoredSession();
			FillStoredMeta(mobileStored, _pendingAccount);
			mobileStored.role = u"android"_q;
			mobileStored.source = u"mobile_registration"_q;
			mobileStored.note = u"Android: "_q + _currentMobileProfile.device + u" ("_q + _currentMobileProfile.sdk + u")"_q;
			mobileStored = Core::GgtgSessions::AddStored(mobileStored);

			auto exportErr = QString();
			(void)Storage::ExportRawAuthorizationAsTdata(
				_pendingAccount->serializeMtpAuthorization(),
				Core::GgtgSessions::VaultFolder(mobileStored),
				&exportErr);

			// 3. Экспорт в .ggtg для GGTG Android
			if (_settings.autoExportGgtg) {
				const auto docs = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
				const auto folder = _settings.folderTag.trimmed().isEmpty() ? u"Регистрация"_q : _settings.folderTag.trimmed();
				const auto targetDir = docs + u"/GGTG/"_q + folder;
				QDir().mkpath(targetDir);

				const auto packPath = targetDir + u"/accounts.ggtg"_q;
				auto packErr = QString();
				ExportAccountToGgtg(
					packPath,
					_pendingAccount->serializeMtpAuthorization(),
					_phone,
					firstName,
					lastName,
					username,
					actual2fa,
					_pendingAccount->proxyOverride(),
					ToDeviceJson(_currentMobileProfile),
					_settings.folderTag,
					true,
					&packErr);

				const auto singlePath = targetDir + u"/+"_q + _phone + u".ggtg"_q;
				auto singleErr = QString();
				ExportAccountToGgtg(
					singlePath,
					_pendingAccount->serializeMtpAuthorization(),
					_phone,
					firstName,
					lastName,
					username,
					actual2fa,
					_pendingAccount->proxyOverride(),
					ToDeviceJson(_currentMobileProfile),
					_settings.folderTag,
					false,
					&singleErr);

				log(u"Аккаунты экспортированы в .ggtg: "_q + packPath);
			}

			// 4. Замораживаем сетевую активность сессии в Desktop
			_pendingAccount->sleep();

			log(u"Аккаунт +"_q + _phone + u" успешно зарегистрирован и экспортирован!"_q);
			if (_doneCb) _doneCb(_workerId, true, QString());
		}, [&](const auto &) {
			fail(u"Не удалось распознать данные пользователя Telegram."_q);
		});
	}
};

// -----------------------------------------------------------------------------
// Multi-threaded Registration Controller & Queue Manager
// -----------------------------------------------------------------------------
class RegistrationController;
inline std::shared_ptr<RegistrationController> &CurrentRegistrationController();

class RegistrationController final : public QObject, public std::enable_shared_from_this<RegistrationController> {
public:
	RegistrationController(
		not_null<Window::SessionController*> controller,
		std::shared_ptr<Ui::Show> show)
	: _controller(controller)
	, _show(show)
	, _netCheckTimer(this)
	, _accountDelayTimer(this) {
	}

	~RegistrationController() {
		cancelRegistration(false);
	}

	void setLogCallback(Fn<void(QString)> cb) { _logCallback = std::move(cb); }
	void setStatusCallback(Fn<void(QString, bool isError, bool isSuccess)> cb) { _statusCallback = std::move(cb); }
	void setBalanceCallback(Fn<void(QString)> cb) { _balanceCallback = std::move(cb); }
	void setRunningCallback(Fn<void(bool)> cb) { _runningCallback = std::move(cb); }
	void setPauseCallback(Fn<void(bool)> cb) { _pauseCallback = std::move(cb); }
	[[nodiscard]] bool isPaused() const { return _isPaused; }
	[[nodiscard]] const QStringList &logHistory() const { return _logHistory; }
	void updateOverallStatus() {
		if (!_isRunning) return;
		if (_processHandle) {
			_processHandle->progress(_successCount, _targetSuccessCount);
		}
		if (_isPaused) {
			status(u"На паузе: Успешно %1 из %2 (Потоков: %3)"_q
				.arg(_successCount).arg(_targetSuccessCount).arg(_workers.size()), false, false);
		} else if (_waitingForInternet) {
			status(u"Ожидание интернет-соединения... (Успешно: %1 из %2)"_q
				.arg(_successCount).arg(_targetSuccessCount), true, false);
		} else {
			const auto summary = u"Успешно: %1 / %2 | Потоков: %3 | Ошибок: %4"_q
				.arg(_successCount)
				.arg(_targetSuccessCount)
				.arg(_workers.size())
				.arg(_failedCount);
			status(summary, false, false);
		}
	}

	void pauseRegistration() {
		if (!_isRunning || _isPaused) return;
		_isPaused = true;
		_accountDelayTimer.stop();
		log(u"Авторегистрация поставлена на паузу."_q);
		if (_pauseCallback) _pauseCallback(true);
		updateOverallStatus();
	}

	void resumeRegistration() {
		if (!_isRunning || !_isPaused) return;
		_isPaused = false;
		log(u"Авторегистрация возобновлена."_q);
		if (_pauseCallback) _pauseCallback(false);
		updateOverallStatus();
		while (_isRunning && !_isPaused && !_waitingForInternet && int(_workers.size()) < _threadsCount && (_successCount + int(_workers.size()) < _targetSuccessCount)) {
			spawnWorker();
		}
	}

	void onNetworkLoss(const QString &reason) {
		if (_waitingForInternet || !_isRunning) return;
		_waitingForInternet = true;
		log(u"Нет интернет-соединения ("_q + reason + u"). Ожидание восстановления сети..."_q);
		updateOverallStatus();

		_netCheckTimer.stop();
		QObject::disconnect(&_netCheckTimer, nullptr, nullptr, nullptr);
		const auto weak = std::weak_ptr<RegistrationController>(shared_from_this());
		connect(&_netCheckTimer, &QTimer::timeout, [weak] {
			const auto strong = weak.lock();
			if (!strong || !strong->_waitingForInternet || !strong->_isRunning) {
				if (strong) strong->_netCheckTimer.stop();
				return;
			}
			strong->checkInternetConnectivity();
		});
		_netCheckTimer.start(3500);
	}

	void checkInternetConnectivity() {
		const auto url = QUrl(u"https://api.telegram.org"_q);
		auto req = QNetworkRequest(url);
		req.setTransferTimeout(4000);
		const auto reply = NetManager().head(req);
		const auto weak = std::weak_ptr<RegistrationController>(shared_from_this());
		QObject::connect(reply, &QNetworkReply::finished, reply, [weak, reply] {
			reply->deleteLater();
			const auto strong = weak.lock();
			if (!strong) return;
			const auto err = reply->error();
			if (err == QNetworkReply::NoError || err == QNetworkReply::ProtocolUnknownError || err == QNetworkReply::ContentNotFoundError || err == QNetworkReply::AuthenticationRequiredError) {
				strong->onInternetRestored();
			}
		});
	}

	void onInternetRestored() {
		if (!_waitingForInternet || !_isRunning) return;
		_waitingForInternet = false;
		_netCheckTimer.stop();
		log(u"Интернет-соединение восстановлено! Продолжение авторегистрации..."_q);
		updateOverallStatus();
		if (!_isPaused) {
			for (auto &w : _workers) {
				if (w) w->onNetworkRestored();
			}
			while (_isRunning && !_isPaused && int(_workers.size()) < _threadsCount && (_successCount + int(_workers.size()) < _targetSuccessCount)) {
				spawnWorker();
			}
		}
	}

	void startAutoRegistration(const SettingsData &settings) {
		if (_isRunning) return;
		CurrentRegistrationController() = shared_from_this();
		_settings = settings;
		SaveSettings(_settings);
		_isRunning = true;
		_isPaused = false;
		_waitingForInternet = false;
		_successCount = 0;
		_failedCount = 0;
		_proxyCursor = 0;
		_nextWorkerId = 1;
		_targetSuccessCount = std::max(1, _settings.targetSuccessCount);
		_threadsCount = std::max(1, std::min(10, _settings.threadsCount));
		_workers.clear();
		_workerStatuses.clear();
		_inUseProxies.clear();

		if (_runningCallback) _runningCallback(true);
		if (_pauseCallback) _pauseCallback(false);

		_processHandle = Core::App().processes().start(
			u"Авторегистрация ("_q + QString::number(_targetSuccessCount) + u" акк.)"_q,
			nullptr,
			Core::ProcessKind::Registration);
		if (_processHandle) {
			_processHandle->progress(0, _targetSuccessCount);
		}

		log(u"========================================"_q);
		log(u"--- Запуск авторегистрации: цель %1 аккаунтов, потоков: %2 ---"_q
			.arg(_targetSuccessCount).arg(_threadsCount));
		log(u"Пауза между шагами: %1–%2 сек | Пауза между аккаунтами: %3–%4 сек"_q
			.arg(_settings.stepDelayMin).arg(_settings.stepDelayMax)
			.arg(_settings.accountDelayMin).arg(_settings.accountDelayMax));
		log(u"========================================"_q);

		updateOverallStatus();

		const auto initialWorkers = std::min(_threadsCount, _targetSuccessCount);
		for (int i = 0; i < initialWorkers; ++i) {
			spawnWorker();
		}
	}

	void startManualSendCode(const SettingsData &settings, const QString &phone) {
		if (_isRunning) return;
		CurrentRegistrationController() = shared_from_this();
		_settings = settings;
		SaveSettings(_settings);
		_isRunning = true;
		if (_runningCallback) _runningCallback(true);

		const auto proxy = pickProxyForWorker();
		const auto weak = std::weak_ptr<RegistrationController>(shared_from_this());
		_manualWorker = std::make_shared<SingleRegistrationWorker>(
			1,
			_settings,
			proxy,
			[weak](int, QString msg) {
				if (const auto strong = weak.lock()) strong->log(msg);
			},
			[weak](int, QString st, bool isErr) {
				if (const auto strong = weak.lock()) strong->status(st, isErr, false);
			},
			[weak, proxy](int, bool success, QString err) {
				if (const auto strong = weak.lock()) {
					strong->releaseProxyForWorker(proxy);
					strong->_isRunning = false;
					if (strong->_runningCallback) strong->_runningCallback(false);
					if (success) {
						if (!strong->_settings.proxyPoolName.isEmpty() && !proxy.host.isEmpty()) {
							MTP::GlobalProxyPool().removeMatching(proxy);
							strong->log(u"Прокси %1:%2 закреплён за аккаунтом и удалён из пула."_q
								.arg(proxy.host).arg(proxy.port));
						}
						strong->status(u"Аккаунт успешно зарегистрирован!"_q, false, true);
					} else {
						strong->status(err, true, false);
					}
					crl::on_main([weak] {
						if (const auto strong = weak.lock()) strong->_manualWorker.reset();
					});
				}
			}
		);
		_manualWorker->startManual(phone);
	}

	void submitManualCode(const QString &code, const QString &firstName, const QString &lastName) {
		if (_manualWorker) {
			_manualWorker->submitManualCode(code, firstName, lastName);
		}
	}

	void cancelRegistration(bool userRequested = true) {
		if (!_isRunning && _workers.empty() && !_manualWorker) return;
		_isRunning = false;
		_isPaused = false;
		_waitingForInternet = false;
		_netCheckTimer.stop();
		_accountDelayTimer.stop();
		QObject::disconnect(&_netCheckTimer, nullptr, nullptr, nullptr);
		QObject::disconnect(&_accountDelayTimer, nullptr, nullptr, nullptr);
		for (auto &w : _workers) {
			if (w) w->cancel();
		}
		_workers.clear();
		_workerStatuses.clear();
		_inUseProxies.clear();
		if (_manualWorker) {
			_manualWorker->cancel();
			_manualWorker.reset();
		}
		if (_processHandle) {
			_processHandle->fail(u"Остановлено пользователем"_q);
			_processHandle = nullptr;
		}
		crl::on_main(&Core::App(), [=] {
			if (Core::App().domain().started()) {
				Core::App().domain().removeRedundantAccounts();
			}
		});
		if (_runningCallback) _runningCallback(false);
		if (_pauseCallback) _pauseCallback(false);
		if (userRequested) {
			log(u"--- Регистрация остановлена пользователем ---"_q);
			status(u"Остановлено. Успешно: %1 из %2 (Ошибок: %3)"_q
				.arg(_successCount).arg(_targetSuccessCount).arg(_failedCount), false, false);
			CurrentRegistrationController() = nullptr;
		}
	}

	void requestAnotherSms() {
	}

	[[nodiscard]] bool isRunning() const { return _isRunning; }
	[[nodiscard]] int successCount() const { return _successCount; }
	[[nodiscard]] int failedCount() const { return _failedCount; }

private:
	void log(const QString &msg) {
		const auto time = QDateTime::currentDateTime().toString(u"hh:mm:ss"_q);
		const auto full = u"["_q + time + u"] "_q + msg;
		Core::GgtgLog::Info(u"Регистрация"_q, msg);
		_logHistory.push_back(full);
		if (_logHistory.size() > 200) {
			_logHistory.pop_front();
		}
		if (_processHandle) {
			_processHandle->log(msg);
		}
		if (_logCallback) _logCallback(full);
	}

	void status(const QString &msg, bool isError, bool isSuccess) {
		if (_statusCallback) _statusCallback(msg, isError, isSuccess);
	}

	MTP::ProxyData pickProxyForWorker() {
		if (_settings.proxyPoolName.isEmpty()) {
			return MTP::ProxyData();
		}
		const auto &lists = MTP::GlobalProxyPool().lists();
		for (const auto &list : lists) {
			if (list.name == _settings.proxyPoolName) {
				const auto total = int(list.entries.size());
				if (total == 0) {
					return MTP::ProxyData();
				}
				for (int i = 0; i < total; ++i) {
					const auto idx = (_proxyCursor + i) % total;
					const auto &candidate = list.entries[idx];
					const auto alreadyUsed = std::any_of(
						_inUseProxies.begin(),
						_inUseProxies.end(),
						[&](const MTP::ProxyData &p) {
							return p.host == candidate.host && p.port == candidate.port;
						});
					if (!alreadyUsed) {
						_inUseProxies.push_back(candidate);
						_proxyCursor = (idx + 1) % total;
						return candidate;
					}
				}
				const auto fallback = list.entries[_proxyCursor % total];
				_proxyCursor = (_proxyCursor + 1) % total;
				return fallback;
			}
		}
		return MTP::ProxyData();
	}

	void releaseProxyForWorker(const MTP::ProxyData &proxy) {
		_inUseProxies.erase(
			std::remove_if(_inUseProxies.begin(), _inUseProxies.end(), [&](const MTP::ProxyData &p) {
				return p.host == proxy.host && p.port == proxy.port;
			}),
			_inUseProxies.end());
	}

	void spawnWorker() {
		if (!_isRunning || _isPaused || _waitingForInternet) return;
		if (_successCount >= _targetSuccessCount) {
			if (_workers.empty()) {
				finishBatch();
			}
			return;
		}
		if (int(_workers.size()) >= _threadsCount) return;
		if (_successCount + int(_workers.size()) >= _targetSuccessCount) return;

		const auto workerId = _nextWorkerId++;
		const auto proxy = pickProxyForWorker();

		const auto weak = std::weak_ptr<RegistrationController>(shared_from_this());
		auto worker = std::make_shared<SingleRegistrationWorker>(
			workerId,
			_settings,
			proxy,
			[weak](int id, QString msg) {
				if (const auto strong = weak.lock()) {
					strong->log(u"[Поток %1] %2"_q.arg(id).arg(msg));
				}
			},
			[weak](int id, QString st, bool isErr) {
				if (const auto strong = weak.lock()) {
					strong->_workerStatuses[id] = st;
					strong->updateOverallStatus();
				}
			},
			[weak, proxy](int id, bool success, QString error) {
				if (const auto strong = weak.lock()) {
					strong->onWorkerFinished(id, proxy, success, error);
				}
			},
			[weak](QString reason) {
				if (const auto strong = weak.lock()) {
					strong->onNetworkLoss(reason);
				}
			}
		);

		_workers.push_back(worker);
		updateOverallStatus();
		worker->start();
	}

	void onWorkerFinished(int id, const MTP::ProxyData &proxy, bool success, QString error) {
		releaseProxyForWorker(proxy);
		_workerStatuses.remove(id);

		const auto weak = std::weak_ptr<RegistrationController>(shared_from_this());
		crl::on_main([weak, id, success, error, proxy] {
			const auto strong = weak.lock();
			if (!strong) return;

			strong->_workers.erase(
				std::remove_if(strong->_workers.begin(), strong->_workers.end(), [id](const std::shared_ptr<SingleRegistrationWorker> &w) {
					return w && w->id() == id;
				}),
				strong->_workers.end());

			if (success) {
				strong->_successCount++;
				if (!strong->_settings.proxyPoolName.isEmpty() && !proxy.host.isEmpty()) {
					MTP::GlobalProxyPool().removeMatching(proxy);
					strong->log(u"[Поток %1] Прокси %2:%3 закреплён за аккаунтом и удалён из пула."_q
						.arg(id).arg(proxy.host).arg(proxy.port));
				}
				strong->log(u"[Поток %1] Успешно завершён! Прогресс: %2 из %3 аккаунтов"_q
					.arg(id).arg(strong->_successCount).arg(strong->_targetSuccessCount));
			} else {
				strong->_failedCount++;
				strong->log(u"✗ [Поток %1] Завершился с ошибкой: %2. Всего ошибок: %3. Запускаем следующий номер..."_q
					.arg(id).arg(error.isEmpty() ? u"Неизвестная ошибка"_q : error).arg(strong->_failedCount));
			}

			strong->updateOverallStatus();

			if (strong->_isRunning && !strong->_isPaused && !strong->_waitingForInternet) {
				if (strong->_successCount < strong->_targetSuccessCount) {
					const auto delayMs = AccountDelayMs(strong->_settings);
					if (delayMs > 0) {
						strong->log(u"[Поток %1] Пауза между аккаунтами (%2 сек)..."_q.arg(id).arg(delayMs / 1000.0, 0, 'f', 1));
						strong->_accountDelayTimer.stop();
						QObject::disconnect(&strong->_accountDelayTimer, nullptr, nullptr, nullptr);
						strong->connect(&strong->_accountDelayTimer, &QTimer::timeout, [weak] {
							if (const auto s = weak.lock()) {
								s->_accountDelayTimer.stop();
								if (s->_isRunning && !s->_isPaused && !s->_waitingForInternet && s->_successCount < s->_targetSuccessCount) {
									while (s->_isRunning && !s->_isPaused && !s->_waitingForInternet && int(s->_workers.size()) < s->_threadsCount && (s->_successCount + int(s->_workers.size()) < s->_targetSuccessCount)) {
										s->spawnWorker();
									}
								}
							}
						});
						strong->_accountDelayTimer.start(delayMs);
					} else {
						while (strong->_isRunning && !strong->_isPaused && !strong->_waitingForInternet && int(strong->_workers.size()) < strong->_threadsCount && (strong->_successCount + int(strong->_workers.size()) < strong->_targetSuccessCount)) {
							strong->spawnWorker();
						}
					}
				} else if (strong->_workers.empty()) {
					strong->finishBatch();
				}
			}
		});
	}

	void finishBatch() {
		_isRunning = false;
		_isPaused = false;
		_waitingForInternet = false;
		_netCheckTimer.stop();
		_accountDelayTimer.stop();
		QObject::disconnect(&_netCheckTimer, nullptr, nullptr, nullptr);
		QObject::disconnect(&_accountDelayTimer, nullptr, nullptr, nullptr);
		if (_runningCallback) _runningCallback(false);
		if (_pauseCallback) _pauseCallback(false);
		const auto summary = u"Регистрация завершена! Успешно: %1 из %2 аккаунтов. Ошибок: %3."_q
			.arg(_successCount)
			.arg(_targetSuccessCount)
			.arg(_failedCount);
		if (_processHandle) {
			_processHandle->succeed(summary);
			_processHandle = nullptr;
		}
		status(summary, false, true);
		log(u"========================================"_q);
		log(summary);
		log(u"========================================"_q);
		CurrentRegistrationController() = nullptr;
	}

	not_null<Window::SessionController*> _controller;
	std::shared_ptr<Ui::Show> _show;
	SettingsData _settings;
	bool _isRunning = false;
	bool _isPaused = false;
	bool _waitingForInternet = false;
	QTimer _netCheckTimer;
	QTimer _accountDelayTimer;
	int _targetSuccessCount = 10;
	int _threadsCount = 2;
	int _successCount = 0;
	int _failedCount = 0;
	int _nextWorkerId = 1;
	int _proxyCursor = 0;

	std::vector<std::shared_ptr<SingleRegistrationWorker>> _workers;
	std::shared_ptr<SingleRegistrationWorker> _manualWorker;
	QMap<int, QString> _workerStatuses;
	std::vector<MTP::ProxyData> _inUseProxies;
	std::shared_ptr<Core::ProcessHandle> _processHandle;
	QStringList _logHistory;

	Fn<void(QString)> _logCallback;
	Fn<void(QString, bool isError, bool isSuccess)> _statusCallback;
	Fn<void(QString)> _balanceCallback;
	Fn<void(bool)> _runningCallback;
	Fn<void(bool)> _pauseCallback;
};

inline std::shared_ptr<RegistrationController> &CurrentRegistrationController() {
	static auto instance = std::shared_ptr<RegistrationController>();
	return instance;
}

// -----------------------------------------------------------------------------
// UI Builder for the Registration Box
// -----------------------------------------------------------------------------
// Input fields that sit inside the cards: the card color instead of the
// darker window color, so a field does not look like a hole in the card.
[[nodiscard]] inline const style::InputField &FieldStyle() {
	static const auto result = [] {
		auto st = st::defaultInputField;
		st.textBg = st::windowBgOver;
		st.textBgActive = st::windowBgOver;
		return st;
	}();
	return result;
}

// A small one-line number field (no floating caption), for "from / to / sec".
[[nodiscard]] inline const style::InputField &CompactFieldStyle() {
	static const auto result = [] {
		auto st = st::defaultInputField;
		st.textBg = st::windowBgOver;
		st.textBgActive = st::windowBgOver;
		st.textMargins = style::margins(
			style::ConvertScale(6),
			style::ConvertScale(7),
			style::ConvertScale(6),
			style::ConvertScale(7));
		st.heightMin = style::ConvertScale(36);
		return st;
	}();
	return result;
}

// Multiline log field for registration actions log
[[nodiscard]] inline const style::InputField &LogFieldStyle() {
	static const auto result = [] {
		auto st = st::defaultInputField;
		st.textBg = st::windowBgOver;
		st.textBgActive = st::windowBgOver;
		st.heightMin = style::ConvertScale(180);
		st.heightMax = style::ConvertScale(320);
		return st;
	}();
	return result;
}

// Section header of the registration form: round accent badge with an icon,
// a bold title and a muted wrapped hint (the look of the other GGTG rows).
class SectionHeader final : public Ui::RpWidget {
public:
	SectionHeader(
		not_null<QWidget*> parent,
		const QString &title,
		const QString &subtitle,
		const style::icon *icon)
	: RpWidget(parent.get())
	, _title(title)
	, _subtitle(subtitle)
	, _icon(icon) {
		resize(width(), style::ConvertScale(46));
		widthValue(
		) | rpl::on_next([=](int) {
			refit();
		}, lifetime());
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		auto p = QPainter(this);
		auto hq = PainterHighQualityEnabler(p);
		const auto badge = style::ConvertScale(32);
		const auto badgeRect = QRect(
			style::ConvertScale(4),
			(height() - badge) / 2,
			badge,
			badge);
		auto fill = st::windowActiveTextFg->c;
		fill.setAlphaF(0.16);
		p.setPen(Qt::NoPen);
		p.setBrush(fill);
		p.drawRoundedRect(badgeRect, badge / 2., badge / 2.);
		if (_icon) {
			_icon->paintInCenter(p, badgeRect, st::windowActiveTextFg->c);
		}
		const auto left = textLeft();
		const auto available = textWidth();
		const auto titleFont = st::semiboldFont;
		const auto subFont = st::normalFont;
		const auto subLines = Ggtg::WrapText(subFont, _subtitle, available);
		const auto gap = subLines.empty() ? 0 : style::ConvertScale(1);
		const auto total = titleFont->height
			+ gap
			+ subFont->height * int(subLines.size());
		auto top = (height() - total) / 2;
		p.setPen(st::windowBoldFg);
		p.setFont(titleFont);
		p.drawText(left, top + titleFont->ascent, _title);
		top += titleFont->height + gap;
		p.setPen(st::windowSubTextFg);
		p.setFont(subFont);
		for (const auto &line : subLines) {
			p.drawText(left, top + subFont->ascent, line);
			top += subFont->height;
		}
	}

private:
	[[nodiscard]] int textLeft() const {
		return style::ConvertScale(4 + 32 + 12);
	}
	[[nodiscard]] int textWidth() const {
		return std::max(width() - textLeft() - style::ConvertScale(4), 0);
	}
	void refit() {
		if (width() <= 0) {
			return;
		}
		const auto lines = Ggtg::WrapText(
			st::normalFont,
			_subtitle,
			textWidth());
		const auto wanted = std::max(
			style::ConvertScale(46),
			st::semiboldFont->height
				+ st::normalFont->height * int(lines.size())
				+ 2 * style::ConvertScale(8));
		if (wanted != height()) {
			resize(width(), wanted);
		}
	}

	QString _title;
	QString _subtitle;
	const style::icon *_icon = nullptr;

};

// A "label  value  ›" row in a soft rounded box: the picker that opens a
// choice box (replaces a text button with a "▾" arrow).
class PickerRow final : public Ui::AbstractButton {
public:
	PickerRow(
		not_null<QWidget*> parent,
		const QString &label,
		const QString &value)
	: AbstractButton(parent.get())
	, _label(label)
	, _value(value) {
		resize(width(), style::ConvertScale(42));
		setCursor(style::cur_pointer);
	}

	void setText(rpl::producer<QString> value) {
		std::move(value) | rpl::on_next([=](QString text) {
			_value = std::move(text);
			update();
		}, lifetime());
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		auto p = QPainter(this);
		auto hq = PainterHighQualityEnabler(p);
		const auto radius = style::ConvertScale(10);
		auto fill = st::windowActiveTextFg->c;
		fill.setAlphaF((isOver() || isDown()) ? 0.14 : 0.08);
		p.setPen(Qt::NoPen);
		p.setBrush(fill);
		p.drawRoundedRect(rect(), radius, radius);

		const auto pad = style::ConvertScale(14);
		const auto chevron = u"›"_q;
		const auto chevronWidth = st::normalFont->width(chevron);
		p.setFont(st::normalFont);
		p.setPen(st::windowSubTextFg);
		p.drawText(
			QRect(width() - pad - chevronWidth, 0, chevronWidth, height()),
			Qt::AlignCenter,
			chevron);

		const auto labelText = _label + u":"_q;
		const auto labelWidth = st::normalFont->width(labelText);
		p.drawText(
			pad,
			(height() - st::normalFont->height) / 2 + st::normalFont->ascent,
			labelText);

		const auto valueLeft = pad + labelWidth + style::ConvertScale(10);
		const auto available = std::max(
			width() - valueLeft - chevronWidth - 2 * pad,
			0);
		p.setFont(st::semiboldFont);
		p.setPen(st::windowBoldFg);
		p.drawText(
			QRect(valueLeft, 0, available, height()),
			Qt::AlignVCenter | Qt::AlignLeft,
			st::semiboldFont->elided(_value, available));
	}

private:
	QString _label;
	QString _value;

};

inline void FillRegistrationBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller) {
	const auto show = box->uiShow();
	box->setTitle(rpl::single(u"Регистрация аккаунтов"_q));
	box->setWidth(st::boxWideWidth + style::ConvertScale(120));

	const auto state = box->lifetime().make_state<SettingsData>(LoadSettings());
	if (!CurrentRegistrationController()) {
		CurrentRegistrationController() = std::make_shared<RegistrationController>(controller, show);
	}
	const auto regController = CurrentRegistrationController();
	box->lifetime().add([regController] {
		if (regController) {
			regController->setLogCallback(nullptr);
			regController->setStatusCallback(nullptr);
			regController->setBalanceCallback(nullptr);
			regController->setRunningCallback(nullptr);
			regController->setPauseCallback(nullptr);
		}
	});

	const auto layout = box->verticalLayout();

	const auto addSectionTo = [](
			not_null<Ui::VerticalLayout*> to,
			const QString &title,
			const QString &subtitle,
			const style::icon *icon) {
		Ui::AddSkip(to, style::ConvertScale(12));
		to->add(
			object_ptr<SectionHeader>(to, title, subtitle, icon),
			style::margins(0, 0, 0, style::ConvertScale(6)));
	};

	// =====================================================================
	//  ОСНОВНОЕ: Сервис, ключ, страна, количество, запуск
	// =====================================================================
	addSectionTo(layout,
		u"Регистрация"_q,
		u"SMS-сервис, страна, количество аккаунтов и запуск"_q,
		&st::menuIconPhone);

	const auto cardMain = layout->add(
		object_ptr<Ggtg::CardBox>(
			layout,
			style::margins(16, 14, 16, 14),
			std::optional<QColor>(st::windowActiveTextFg->c)),
		st::boxRowPadding);
	const auto inMain = cardMain->inner();

	// --- Сервис ---
	auto currentServiceName = Services().front().name;
	for (const auto &s : Services()) {
		if (s.id == state->serviceId) {
			currentServiceName = s.name;
			break;
		}
	}

	const auto serviceBtn = inMain->add(
		object_ptr<PickerRow>(inMain, u"Сервис активаций"_q, currentServiceName),
		style::margins(0, 0, 0, style::ConvertScale(4)));

	const auto webLinkBtn = inMain->add(
		object_ptr<Ui::LinkButton>(inMain, u"Открыть сайт сервиса"_q),
		style::margins(style::ConvertScale(4), 0, 0, style::ConvertScale(4)));

	const auto updateWebLink = [=] {
		for (const auto &s : Services()) {
			if (s.id == state->serviceId && !s.webUrl.isEmpty()) {
				webLinkBtn->show();
				webLinkBtn->setClickedCallback([url = s.webUrl] {
					QDesktopServices::openUrl(QUrl(url));
				});
				return;
			}
		}
		webLinkBtn->hide();
	};
	updateWebLink();

	const auto customWrap = inMain->add(
		object_ptr<Ui::SlideWrap<Ui::InputField>>(
			inMain,
			object_ptr<Ui::InputField>(
				inMain,
				FieldStyle(),
				rpl::single(u"Адрес API (например https://myservice.com/stubs/handler_api.php)"_q),
				state->customUrl)),
		style::margins(0, style::ConvertScale(4), 0, style::ConvertScale(4)));
	customWrap->toggle(state->serviceId == u"custom"_q, anim::type::instant);

	// --- API-ключ ---
	const auto apiKeyField = inMain->add(
		object_ptr<Ui::InputField>(
			inMain,
			FieldStyle(),
			rpl::single(u"API-ключ"_q),
			state->apiKey),
		style::margins(0, style::ConvertScale(6), 0, style::ConvertScale(6)));

	apiKeyField->changes(
	) | rpl::on_next([=] {
		state->apiKey = apiKeyField->getLastText().trimmed();
		state->apiKeys.insert(state->serviceId, state->apiKey);
		SaveSettings(*state);
	}, apiKeyField->lifetime());

	// --- Баланс ---
	const auto balanceRow = inMain->add(object_ptr<Ui::RpWidget>(inMain));
	balanceRow->resize(balanceRow->width(), style::ConvertScale(38));

	const auto checkBalanceBtn = Ui::CreateChild<Ui::RoundButton>(
		balanceRow,
		rpl::single(u"Проверить баланс"_q),
		st::defaultActiveButton);

	const auto balanceLabel = Ui::CreateChild<Ui::FlatLabel>(
		balanceRow,
		rpl::single(u"Баланс: —"_q),
		st::boxLabel);

	balanceRow->widthValue() | rpl::on_next([=](int w) {
		checkBalanceBtn->moveToLeft(0, style::ConvertScale(2));
		balanceLabel->moveToLeft(checkBalanceBtn->width() + style::ConvertScale(14), style::ConvertScale(8));
	}, balanceRow->lifetime());

	const auto countryRefresh = box->lifetime().make_state<Fn<void()>>();
	const auto weakBalanceLabel = QPointer<Ui::FlatLabel>(balanceLabel);

	const auto doCheckBalance = [=] {
		state->apiKey = apiKeyField->getLastText().trimmed();
		SaveSettings(*state);
		if (weakBalanceLabel) weakBalanceLabel->setText(u"Баланс: проверка..."_q);
		FetchBalance(*state, [=](QString bal, QString err) {
			if (weakBalanceLabel) {
				if (!err.isEmpty()) {
					weakBalanceLabel->setText(u"Ошибка баланса: "_q + err);
				} else {
					weakBalanceLabel->setText(u"Баланс: "_q + bal);
				}
			}
			FetchCountries(*state, [=](QString) {
				if (*countryRefresh) {
					(*countryRefresh)();
				}
			});
		});
	};
	checkBalanceBtn->setClickedCallback(doCheckBalance);

	regController->setBalanceCallback([weakBalanceLabel](QString bal) {
		if (weakBalanceLabel) {
			weakBalanceLabel->setText(u"Баланс: "_q + bal);
		}
	});

	// --- Диалог выбора сервиса ---
	serviceBtn->setClickedCallback([=] {
		show->show(Box([=](not_null<Ui::GenericBox*> pickBox) {
			pickBox->setTitle(rpl::single(u"Выберите SMS-сервис"_q));
			pickBox->setWidth(st::boxWideWidth);
			for (const auto &s : Services()) {
				const auto key = s.id;
				const auto name = s.name;
				pickBox->addRow(object_ptr<Ggtg::ActionRow>(
					pickBox,
					rpl::single(name),
					s.baseUrl.isEmpty() ? u"Свой endpoint"_q : s.baseUrl,
					&st::menuIconDiscussion)
				)->setClickedCallback([=] {
					state->apiKeys.insert(state->serviceId, apiKeyField->getLastText().trimmed());
					state->serviceId = key;
					state->countryName.clear();
					state->apiKey = state->apiKeys.value(key);
					apiKeyField->setText(state->apiKey);
					if (*countryRefresh) {
						(*countryRefresh)();
					}
					serviceBtn->setText(rpl::single(name));
					customWrap->toggle(key == u"custom"_q, anim::type::normal);
					updateWebLink();
					SaveSettings(*state);
					pickBox->closeBox();
					doCheckBalance();
				});
				Ui::AddSkip(pickBox->verticalLayout(), st::defaultVerticalListSkip);
			}
			pickBox->addButton(tr::lng_close(), [=] { pickBox->closeBox(); });
		}));
	});

	// --- Оператор holder для обновления из выбора страны ---
	const auto operatorBtnHolder = std::make_shared<QPointer<PickerRow>>();

	// --- Страна ---
	Ui::AddSkip(inMain, style::ConvertScale(8));
	const auto countryBtn = inMain->add(
		object_ptr<PickerRow>(
			inMain,
			u"Страна номера"_q,
			CurrentCountryTitle(*state)));
	const auto weakCountryBtn = QPointer<PickerRow>(countryBtn);

	*countryRefresh = [=] {
		if (const auto btn = weakCountryBtn.data()) {
			btn->setText(rpl::single(CurrentCountryTitle(*state)));
		}
	};
	if (!state->apiKey.trimmed().isEmpty()) {
		const auto weakBtn = weakCountryBtn;
		FetchCountries(*state, [=](QString) {
			if (const auto btn = weakBtn.data()) {
				btn->setText(rpl::single(CurrentCountryTitle(*state)));
			}
		});
	}

	countryBtn->setClickedCallback([=] {
		state->apiKey = apiKeyField->getLastText().trimmed();
		state->apiKeys.insert(state->serviceId, state->apiKey);
		SaveSettings(*state);
		show->showToast(u"Загружаю актуальный список стран..."_q);
		FetchCountries(*state, [=](QString err) {
			const auto &live = CountriesFor(state->serviceId);
			const auto &available = !live.empty() ? live : PopularCountries();
			if (available.empty()) {
				show->showToast(u"Не удалось получить список стран: "_q + err);
				return;
			}
			show->show(Box([=](not_null<Ui::GenericBox*> pickBox) {
				pickBox->setTitle(rpl::single(u"Выберите страну номера"_q));
				pickBox->setWidth(style::ConvertScale(520));

				const auto searchField = pickBox->addRow(
					object_ptr<Ui::InputField>(
						pickBox,
						st::defaultInputField,
						rpl::single(u"Поиск по названию или коду страны..."_q)),
					st::boxRowPadding);

				const auto listWrap = pickBox->addRow(
					object_ptr<Ui::VerticalLayout>(pickBox));
				const auto weakCountryListWrap = QPointer<Ui::VerticalLayout>(listWrap);

				const auto rebuildList = [=](const QString &query) {
					if (!weakCountryListWrap) return;
					weakCountryListWrap->clear();
					const auto q = query.trimmed().toLower();
					int count = 0;
					for (const auto &c : available) {
						const auto id = c.id;
						const auto title = CountryTitle(c);
						if (!q.isEmpty()) {
							const auto matchName = c.name.toLower().contains(q);
							const auto matchPhone = c.phoneCode.toLower().contains(q);
							const auto matchId = QString::number(id).contains(q);
							if (!matchName && !matchPhone && !matchId) {
								continue;
							}
						}
						count++;
						weakCountryListWrap->add(object_ptr<Ggtg::ActionRow>(
							weakCountryListWrap.data(),
							rpl::single(title),
							u"ID в сервисе: "_q + QString::number(id),
							&st::menuIconManage)
						)->setClickedCallback([=] {
							state->countryId = id;
							state->countryName = title;
							if (const auto btn = weakCountryBtn.data()) {
								btn->setText(rpl::single(title));
							}
							FetchOperators(*state, id, [=](std::vector<OperatorInfo> ops, QString) {
								for (const auto &op : ops) {
									if (op.name == state->smsOperator || (state->smsOperator.isEmpty() && op.name == u"any"_q)) {
										if (const auto opBtn = operatorBtnHolder ? operatorBtnHolder->data() : nullptr) {
											opBtn->setText(rpl::single(OperatorDisplayTitle(state->smsOperator, op.count)));
										}
										break;
									}
								}
							});
							SaveSettings(*state);
							pickBox->closeBox();
						});
						Ui::AddSkip(weakCountryListWrap.data(), st::defaultVerticalListSkip);
					}
					if (count == 0 && weakCountryListWrap) {
						weakCountryListWrap->add(object_ptr<Ui::FlatLabel>(
							weakCountryListWrap.data(),
							u"Ничего не найдено"_q,
							st::boxDividerLabel),
							st::boxRowPadding);
					}
				};

				searchField->changes() | rpl::on_next([=] {
					rebuildList(searchField->getLastText());
				}, searchField->lifetime());

				rebuildList(QString());

				pickBox->addButton(tr::lng_close(), [=] { pickBox->closeBox(); });
			}));
		});
	});

	// --- Оператор и максимальная цена (Быстрая настройка) ---
	Ui::AddSkip(inMain, style::ConvertScale(8));
	const auto operatorBtn = inMain->add(
		object_ptr<PickerRow>(
			inMain,
			u"Оператор связи"_q,
			OperatorDisplayTitle(state->smsOperator)),
		style::margins(0, 0, 0, style::ConvertScale(4)));
	*operatorBtnHolder = operatorBtn;
	const auto weakOperatorBtn = QPointer<PickerRow>(operatorBtn);

	const auto updateOperatorSubtitle = [=](int count = -1) {
		if (const auto btn = weakOperatorBtn.data()) {
			btn->setText(rpl::single(OperatorDisplayTitle(state->smsOperator, count)));
		}
	};

	if (state->countryId >= 0) {
		FetchOperators(*state, state->countryId, [=](std::vector<OperatorInfo> ops, QString) {
			for (const auto &op : ops) {
				if (op.name == state->smsOperator || (state->smsOperator.isEmpty() && op.name == u"any"_q)) {
					updateOperatorSubtitle(op.count);
					break;
				}
			}
		});
	}

	operatorBtn->setClickedCallback([=] {
		if (state->countryId < 0) {
			show->showToast(u"Сначала выберите страну"_q);
			return;
		}
		show->show(Box([=](not_null<Ui::GenericBox*> pickBox) {
			pickBox->setTitle(rpl::single(u"Выберите оператора связи"_q));
			pickBox->setWidth(style::ConvertScale(520));

			const auto searchField = pickBox->addRow(
				object_ptr<Ui::InputField>(
					pickBox,
					st::defaultInputField,
					rpl::single(u"Поиск оператора..."_q)),
				st::boxRowPadding);

			const auto listWrap = pickBox->addRow(
				object_ptr<Ui::VerticalLayout>(pickBox));
			const auto weakOpListWrap = QPointer<Ui::VerticalLayout>(listWrap);

			listWrap->add(
				object_ptr<Ui::FlatLabel>(
					listWrap,
					rpl::single(u"Загрузка операторов из сервиса..."_q),
					st::boxDividerLabel),
				st::boxRowPadding);

			FetchOperators(*state, state->countryId, [=](std::vector<OperatorInfo> ops, QString err) {
				if (!weakOpListWrap) return;
				const auto rebuildList = [=](const QString &filter) {
					if (!weakOpListWrap) return;
					weakOpListWrap->clear();
					const auto q = filter.trimmed().toLower();
					int count = 0;

					for (const auto &op : ops) {
						if (!q.isEmpty() && !op.name.toLower().contains(q) && !op.title.toLower().contains(q)) {
							continue;
						}
						count++;
						const auto isChosen = (state->smsOperator.isEmpty() && op.name == u"any"_q) || (state->smsOperator == op.name);
						const auto prefix = isChosen ? u"● "_q : u"○ "_q;

						auto rowText = prefix + op.title;
						if (op.count >= 0) {
							rowText += u" — доступно: %1 шт."_q.arg(op.count);
						}
						if (!op.price.isEmpty()) {
							rowText += u" (от %1$)"_q.arg(op.price);
						}

						weakOpListWrap->add(object_ptr<Ggtg::ActionRow>(
							weakOpListWrap.data(),
							rpl::single(rowText),
							QString(),
							&st::menuIconManage)
						)->setClickedCallback([=] {
							state->smsOperator = (op.name == u"any"_q) ? QString() : op.name;
							SaveSettings(*state);
							updateOperatorSubtitle(op.count);
							pickBox->closeBox();
						});
						Ui::AddSkip(weakOpListWrap.data(), st::defaultVerticalListSkip);
					}

					if (weakOpListWrap) {
						weakOpListWrap->add(object_ptr<Ggtg::ActionRow>(
							weakOpListWrap.data(),
							rpl::single(u"Ввести вручную..."_q),
							u"Указать произвольный код оператора"_q,
							&st::menuIconManage)
						)->setClickedCallback([=] {
							pickBox->closeBox();
							show->show(Box([=](not_null<Ui::GenericBox*> customBox) {
								customBox->setTitle(rpl::single(u"Оператор связи"_q));
								const auto input = customBox->addRow(
									object_ptr<Ui::InputField>(
										customBox,
										FieldStyle(),
										rpl::single(u"Код оператора (например: megafon, mts)"_q),
										state->smsOperator),
									st::boxRowPadding);
								customBox->addButton(tr::lng_settings_save(), [=] {
									state->smsOperator = input->getLastText().trimmed();
									SaveSettings(*state);
									updateOperatorSubtitle();
									customBox->closeBox();
								});
								customBox->addButton(tr::lng_cancel(), [=] { customBox->closeBox(); });
							}));
						});
					}

					if (count == 0 && weakOpListWrap) {
						weakOpListWrap->add(object_ptr<Ui::FlatLabel>(
							weakOpListWrap.data(),
							u"Ничего не найдено"_q,
							st::boxDividerLabel),
							st::boxRowPadding);
					}
				};

				searchField->changes() | rpl::on_next([=] {
					rebuildList(searchField->getLastText());
				}, searchField->lifetime());

				rebuildList(QString());
			});

			pickBox->addButton(tr::lng_close(), [=] { pickBox->closeBox(); });
		}));
	});

	const auto maxPriceField = inMain->add(
		object_ptr<Ui::InputField>(
			inMain,
			FieldStyle(),
			rpl::single(u"Макс. цена за номер (пусто = без лимита)"_q),
			state->maxPrice),
		style::margins(0, 0, 0, style::ConvertScale(6)));

	maxPriceField->changes() | rpl::on_next([=] {
		state->maxPrice = maxPriceField->getLastText().trimmed();
		SaveSettings(*state);
	}, maxPriceField->lifetime());

	// --- Сервис токенов (Play Integrity) в быстрой настройке ---
	Ui::AddSkip(inMain, style::ConvertScale(8));
	const auto tokenServiceBtn = inMain->add(
		object_ptr<PickerRow>(
			inMain,
			u"Сервис токенов (Play Integrity)"_q,
			TokenProviderName(state->tokenService)),
		style::margins(0, 0, 0, style::ConvertScale(4)));

	const auto tokenApiKeyField = inMain->add(
		object_ptr<Ui::InputField>(
			inMain,
			FieldStyle(),
			rpl::single(u"API-ключ сервиса токенов (TokenFast / AntiSafety)"_q),
			state->tokenApiKey),
		style::margins(0, style::ConvertScale(4), 0, style::ConvertScale(6)));

	tokenApiKeyField->changes() | rpl::on_next([=] {
		state->tokenApiKey = tokenApiKeyField->getLastText().trimmed();
		SaveSettings(*state);
	}, tokenApiKeyField->lifetime());

	const auto checkTokenRow = inMain->add(object_ptr<Ui::RpWidget>(inMain));
	checkTokenRow->resize(checkTokenRow->width(), style::ConvertScale(38));

	const auto checkTokenBtn = Ui::CreateChild<Ui::RoundButton>(
		checkTokenRow,
		rpl::single(u"Проверить баланс"_q),
		st::defaultActiveButton);

	const auto tokenStatusLabel = Ui::CreateChild<Ui::FlatLabel>(
		checkTokenRow,
		rpl::single(u"Баланс токенов: —"_q),
		st::boxLabel);

	checkTokenRow->widthValue() | rpl::on_next([=](int w) {
		checkTokenBtn->moveToLeft(0, style::ConvertScale(2));
		const auto avail = std::max(w - checkTokenBtn->width() - style::ConvertScale(20), style::ConvertScale(120));
		tokenStatusLabel->resizeToWidth(avail);
		tokenStatusLabel->moveToLeft(checkTokenBtn->width() + style::ConvertScale(14), style::ConvertScale(8));
	}, checkTokenRow->lifetime());

	const auto weakTokenStatusLabel = QPointer<Ui::FlatLabel>(tokenStatusLabel);
	checkTokenBtn->setClickedCallback([=] {
		const auto key = tokenApiKeyField->getLastText().trimmed();
		state->tokenApiKey = key;
		SaveSettings(*state);
		if (weakTokenStatusLabel) weakTokenStatusLabel->setText(u"Баланс: проверка..."_q);
		CheckMobileTokenBalance(
			MobileTokenProvider(state->tokenService),
			key,
			[weakTokenStatusLabel](QString bal, QString err) {
				if (!weakTokenStatusLabel) return;
				if (!err.isEmpty()) {
					weakTokenStatusLabel->setText(u"Ошибка: "_q + err);
				} else {
					weakTokenStatusLabel->setText(u"Баланс: "_q + bal);
				}
			});
	});

	tokenServiceBtn->setClickedCallback([=] {
		show->show(Box([=](not_null<Ui::GenericBox*> pickBox) {
			pickBox->setTitle(rpl::single(u"Выберите сервис токенов"_q));
			pickBox->setWidth(st::boxWideWidth);
			const auto options = std::vector<std::pair<int, QString>>{
				{ 0, u"Отключен"_q },
				{ 1, u"TokenFast (TokenFast.pro)"_q },
				{ 2, u"AntiSafety (AntiSafety.net)"_q },
			};
			for (const auto &[val, title] : options) {
				pickBox->addRow(object_ptr<Ggtg::ActionRow>(
					pickBox,
					rpl::single(title),
					val == 0 ? u"Регистрация без Play Integrity токенов"_q : u"Обход детекта эмулятора Telegram"_q,
					&st::menuIconManage)
				)->setClickedCallback([=] {
					state->tokenService = val;
					tokenServiceBtn->setText(rpl::single(title));
					SaveSettings(*state);
					pickBox->closeBox();
				});
				Ui::AddSkip(pickBox->verticalLayout(), st::defaultVerticalListSkip);
			}
			pickBox->addButton(tr::lng_close(), [=] { pickBox->closeBox(); });
		}));
	});

	// --- Количество аккаунтов ---
	Ui::AddSkip(inMain, style::ConvertScale(8));
	const auto targetSuccessField = inMain->add(
		object_ptr<Ui::InputField>(
			inMain,
			FieldStyle(),
			rpl::single(u"Количество аккаунтов"_q),
			QString::number(state->targetSuccessCount)),
		style::margins(0, 0, 0, style::ConvertScale(6)));

	targetSuccessField->changes() | rpl::on_next([=] {
		const auto val = targetSuccessField->getLastText().trimmed().toInt();
		if (val > 0) {
			state->targetSuccessCount = val;
			SaveSettings(*state);
		}
	}, targetSuccessField->lifetime());

	const auto queueSkipAppHolder = std::make_shared<QPointer<Ui::Checkbox>>();

	const auto skipAppCheck = inMain->add(
		object_ptr<Ui::Checkbox>(
			inMain,
			u"Отклонять номер, если код отправлен в приложение (номер занят)"_q,
			state->skipAppCode),
		style::margins(0, style::ConvertScale(8), 0, style::ConvertScale(2)));
	inMain->add(
		object_ptr<Ui::FlatLabel>(
			inMain,
			rpl::single(u"Рекомендуется включить (стандарт Telegram Expert). Если номер уже кем-то используется, Telegram отправляет код в приложение, а не в SMS. Опция мгновенно отменяет номер (статус 8) и возвращает 100% средств на баланс SMS-сервиса, не тратя 5 минут на пустое ожидание SMS."_q),
			st::boxDividerLabel),
		style::margins(0, 0, 0, style::ConvertScale(8)));

	skipAppCheck->checkedChanges(
	) | rpl::on_next([=](bool checked) {
		state->skipAppCode = checked;
		if (const auto qCheck = queueSkipAppHolder ? queueSkipAppHolder->data() : nullptr) {
			qCheck->setChecked(checked);
		}
		SaveSettings(*state);
	}, skipAppCheck->lifetime());

	// =====================================================================
	//  2. ИМЕНА И ФАМИЛИИ
	// =====================================================================
	addSectionTo(layout,
		u"Имена и фамилии"_q,
		u"Настройка имён для создаваемых аккаунтов"_q,
		&st::menuIconProfile);

	const auto cardNames = layout->add(
		object_ptr<Ggtg::CardBox>(layout, style::margins(16, 14, 16, 14)),
		st::boxRowPadding);
	const auto inNames = cardNames->inner();

	const auto singleNameCheck = inNames->add(
		object_ptr<Ui::Checkbox>(
			inNames,
			u"Одно имя и фамилия на все аккаунты"_q,
			state->singleNameForAll),
		style::margins(0, style::ConvertScale(4), 0, style::ConvertScale(2)));
	inNames->add(
		object_ptr<Ui::FlatLabel>(
			inNames,
			rpl::single(u"Если включено — все создаваемые аккаунты получат указанные имя и фамилию. Если выключено — имена берутся из файлов или генерируются автоматически."_q),
			st::boxDividerLabel),
		style::margins(0, 0, 0, style::ConvertScale(6)));

	const auto singleNameWrap = inNames->add(
		object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			inNames,
			object_ptr<Ui::VerticalLayout>(inNames)),
		style::margins());
	const auto singleNameLayout = singleNameWrap->entity();

	const auto firstNameField = singleNameLayout->add(
		object_ptr<Ui::InputField>(
			singleNameLayout,
			FieldStyle(),
			rpl::single(u"Имя для всех аккаунтов"_q),
			state->customFirstName),
		style::margins(0, style::ConvertScale(4), 0, style::ConvertScale(4)));
	firstNameField->changes() | rpl::on_next([=] {
		state->customFirstName = firstNameField->getLastText().trimmed();
		SaveSettings(*state);
	}, firstNameField->lifetime());

	const auto lastNameField = singleNameLayout->add(
		object_ptr<Ui::InputField>(
			singleNameLayout,
			FieldStyle(),
			rpl::single(u"Фамилия для всех аккаунтов"_q),
			state->customLastName),
		style::margins(0, style::ConvertScale(4), 0, style::ConvertScale(6)));
	lastNameField->changes() | rpl::on_next([=] {
		state->customLastName = lastNameField->getLastText().trimmed();
		SaveSettings(*state);
	}, lastNameField->lifetime());

	const auto randomNamesWrap = inNames->add(
		object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			inNames,
			object_ptr<Ui::VerticalLayout>(inNames)),
		style::margins());
	const auto randomNamesLayout = randomNamesWrap->entity();

	const auto genderBtn = randomNamesLayout->add(
		object_ptr<PickerRow>(
			randomNamesLayout,
			u"Пол"_q,
			state->gender == 1 ? u"Мужской"_q : (state->gender == 2 ? u"Женский"_q : u"Случайный (М/Ж)"_q)),
		style::margins(0, style::ConvertScale(2), 0, style::ConvertScale(6)));

	genderBtn->setClickedCallback([=] {
		show->show(Box([=](not_null<Ui::GenericBox*> pickBox) {
			pickBox->setTitle(rpl::single(u"Выберите пол для генерации имён"_q));
			struct GenderOpt { int val; QString label; };
			for (const auto &opt : std::vector<GenderOpt>{{0, u"Случайный (М/Ж)"_q}, {1, u"Мужской"_q}, {2, u"Женский"_q}}) {
				pickBox->addRow(object_ptr<Ggtg::ActionRow>(
					pickBox,
					rpl::single(opt.label),
					QString(),
					nullptr)
				)->setClickedCallback([=, v = opt.val, lbl = opt.label] {
					state->gender = v;
					genderBtn->setText(rpl::single(lbl));
					SaveSettings(*state);
					pickBox->closeBox();
				});
				Ui::AddSkip(pickBox->verticalLayout(), st::defaultVerticalListSkip);
			}
			pickBox->addButton(tr::lng_close(), [=] { pickBox->closeBox(); });
		}));
	});

	// Файл с именами
	const auto updateFnLabel = box->lifetime().make_state<Fn<void()>>();
	const auto fnStatusLabel = randomNamesLayout->add(
		object_ptr<Ui::FlatLabel>(
			randomNamesLayout,
			rpl::single(QString()),
			st::boxLabel),
		style::margins(0, style::ConvertScale(4), 0, style::ConvertScale(2)));

	const auto fnBtnsRow = randomNamesLayout->add(object_ptr<Ui::RpWidget>(randomNamesLayout));
	fnBtnsRow->resize(fnBtnsRow->width(), style::ConvertScale(38));

	const auto pickFnBtn = Ui::CreateChild<Ui::RoundButton>(
		fnBtnsRow,
		rpl::single(u"Выбрать файл с именами…"_q),
		st::defaultBoxButton);
	pickFnBtn->setTextTransform(Ui::RoundButtonTextTransform::NoTransform);

	const auto clearFnBtn = Ui::CreateChild<Ui::RoundButton>(
		fnBtnsRow,
		rpl::single(u"Очистить"_q),
		st::defaultBoxButton);
	clearFnBtn->setTextTransform(Ui::RoundButtonTextTransform::NoTransform);

	fnBtnsRow->widthValue() | rpl::on_next([=](int w) {
		pickFnBtn->moveToLeft(0, style::ConvertScale(2));
		clearFnBtn->moveToLeft(pickFnBtn->width() + style::ConvertScale(10), style::ConvertScale(2));
	}, fnBtnsRow->lifetime());

	*updateFnLabel = [=] {
		if (state->firstNameFile.isEmpty() || !QFile::exists(state->firstNameFile)) {
			fnStatusLabel->setText(u"Файл с именами: не выбран (случайные имена)"_q);
			clearFnBtn->hide();
		} else {
			const auto count = CountNonEmptyLinesInFile(state->firstNameFile);
			fnStatusLabel->setText(u"Файл с именами: "_q + QFileInfo(state->firstNameFile).fileName() + u" ("_q + FormatLinesCount(count) + u")"_q);
			clearFnBtn->show();
		}
	};
	(*updateFnLabel)();

	pickFnBtn->setClickedCallback([=] {
		FileDialog::GetOpenPath(
			QPointer<QWidget>(box.get()),
			u"Выберите файл с именами"_q,
			u"Текстовые файлы (*.txt);;Все файлы (*)"_q,
			[=](FileDialog::OpenResult &&result) {
				if (result.paths.isEmpty()) return;
				state->firstNameFile = result.paths.front();
				SaveSettings(*state);
				(*updateFnLabel)();
			});
	});

	clearFnBtn->setClickedCallback([=] {
		state->firstNameFile.clear();
		SaveSettings(*state);
		(*updateFnLabel)();
	});

	// Файл с фамилиями
	Ui::AddSkip(randomNamesLayout, style::ConvertScale(6));
	const auto updateLnLabel = box->lifetime().make_state<Fn<void()>>();
	const auto lnStatusLabel = randomNamesLayout->add(
		object_ptr<Ui::FlatLabel>(
			randomNamesLayout,
			rpl::single(QString()),
			st::boxLabel),
		style::margins(0, style::ConvertScale(4), 0, style::ConvertScale(2)));

	const auto lnBtnsRow = randomNamesLayout->add(object_ptr<Ui::RpWidget>(randomNamesLayout));
	lnBtnsRow->resize(lnBtnsRow->width(), style::ConvertScale(38));

	const auto pickLnBtn = Ui::CreateChild<Ui::RoundButton>(
		lnBtnsRow,
		rpl::single(u"Выбрать файл с фамилиями…"_q),
		st::defaultBoxButton);
	pickLnBtn->setTextTransform(Ui::RoundButtonTextTransform::NoTransform);

	const auto clearLnBtn = Ui::CreateChild<Ui::RoundButton>(
		lnBtnsRow,
		rpl::single(u"Очистить"_q),
		st::defaultBoxButton);
	clearLnBtn->setTextTransform(Ui::RoundButtonTextTransform::NoTransform);

	lnBtnsRow->widthValue() | rpl::on_next([=](int w) {
		pickLnBtn->moveToLeft(0, style::ConvertScale(2));
		clearLnBtn->moveToLeft(pickLnBtn->width() + style::ConvertScale(10), style::ConvertScale(2));
	}, lnBtnsRow->lifetime());

	*updateLnLabel = [=] {
		if (state->lastNameFile.isEmpty() || !QFile::exists(state->lastNameFile)) {
			lnStatusLabel->setText(u"Файл с фамилиями: не выбран (случайные фамилии)"_q);
			clearLnBtn->hide();
		} else {
			const auto count = CountNonEmptyLinesInFile(state->lastNameFile);
			lnStatusLabel->setText(u"Файл с фамилиями: "_q + QFileInfo(state->lastNameFile).fileName() + u" ("_q + FormatLinesCount(count) + u")"_q);
			clearLnBtn->show();
		}
	};
	(*updateLnLabel)();

	pickLnBtn->setClickedCallback([=] {
		FileDialog::GetOpenPath(
			QPointer<QWidget>(box.get()),
			u"Выберите файл с фамилиями"_q,
			u"Текстовые файлы (*.txt);;Все файлы (*)"_q,
			[=](FileDialog::OpenResult &&result) {
				if (result.paths.isEmpty()) return;
				state->lastNameFile = result.paths.front();
				SaveSettings(*state);
				(*updateLnLabel)();
			});
	});

	clearLnBtn->setClickedCallback([=] {
		state->lastNameFile.clear();
		SaveSettings(*state);
		(*updateLnLabel)();
	});

	singleNameWrap->toggle(state->singleNameForAll, anim::type::instant);
	randomNamesWrap->toggle(!state->singleNameForAll, anim::type::instant);

	singleNameCheck->checkedChanges(
	) | rpl::on_next([=](bool checked) {
		state->singleNameForAll = checked;
		state->autoRandomNames = !checked;
		singleNameWrap->toggle(checked, anim::type::normal);
		randomNamesWrap->toggle(!checked, anim::type::normal);
		SaveSettings(*state);
	}, singleNameCheck->lifetime());

	const auto usernameCheck = inNames->add(
		object_ptr<Ui::Checkbox>(
			inNames,
			u"Устанавливать @username при регистрации"_q,
			state->autoSetUsername),
		style::margins(0, 8, 0, 0));
	usernameCheck->checkedChanges(
	) | rpl::on_next([=](bool checked) {
		state->autoSetUsername = checked;
		SaveSettings(*state);
	}, usernameCheck->lifetime());

	const auto bioCheck = inNames->add(
		object_ptr<Ui::Checkbox>(
			inNames,
			u"Заполнять описание профиля (Bio)"_q,
			state->autoSetBio),
		style::margins(0, 8, 0, 0));
	bioCheck->checkedChanges(
	) | rpl::on_next([=](bool checked) {
		state->autoSetBio = checked;
		SaveSettings(*state);
	}, bioCheck->lifetime());

	const auto bioInput = inNames->add(
		object_ptr<Ui::InputField>(
			inNames,
			st::defaultInputField,
			rpl::single(u"Текст Bio (или пусто для случайного)"_q),
			state->customBio),
		style::margins(0, 6, 0, 0));
	bioInput->changes(
	) | rpl::on_next([=] {
		state->customBio = bioInput->getLastText().trimmed();
		SaveSettings(*state);
	}, bioInput->lifetime());

	// =====================================================================
	//  3. ПРОКСИ И СЕТЬ
	// =====================================================================
	addSectionTo(layout,
		u"Прокси и сеть"_q,
		u"Подключение аккаунтов через списки прокси"_q,
		&st::menuIconNetwork);

	const auto cardProxy = layout->add(
		object_ptr<Ggtg::CardBox>(layout, style::margins(16, 14, 16, 14)),
		st::boxRowPadding);
	const auto inProxy = cardProxy->inner();

	const auto countProxies = [=](const QString &name) {
		if (name.isEmpty()) return 0;
		for (const auto &list : MTP::GlobalProxyPool().lists()) {
			if (list.name == name) {
				return int(list.entries.size());
			}
		}
		return 0;
	};

	const auto getProxyTitle = [=](const QString &name) {
		if (name.isEmpty()) return u"Без прокси (напрямую)"_q;
		const auto count = countProxies(name);
		return name + u" ("_q + QString::number(count) + u" шт.)"_q;
	};

	const auto proxyBtn = inProxy->add(
		object_ptr<PickerRow>(
			inProxy,
			u"Список прокси"_q,
			getProxyTitle(state->proxyPoolName)),
		style::margins(0, 0, 0, style::ConvertScale(6)));

	proxyBtn->setClickedCallback([=] {
		show->show(Box([=](not_null<Ui::GenericBox*> pickBox) {
			pickBox->setTitle(rpl::single(u"Выберите список прокси"_q));
			pickBox->setWidth(st::boxWideWidth);

			pickBox->addRow(object_ptr<Ggtg::ActionRow>(
				pickBox,
				rpl::single(u"Без прокси (прямое подключение)"_q),
				u"Не рекомендуется для массовой регистрации"_q,
				&st::menuIconNetwork)
			)->setClickedCallback([=] {
				state->proxyPoolName.clear();
				proxyBtn->setText(rpl::single(getProxyTitle(QString())));
				SaveSettings(*state);
				pickBox->closeBox();
			});
			Ui::AddSkip(pickBox->verticalLayout(), st::defaultVerticalListSkip);

			const auto &lists = MTP::GlobalProxyPool().lists();
			for (const auto &list : lists) {
				const auto name = list.name;
				const auto count = int(list.entries.size());
				pickBox->addRow(object_ptr<Ggtg::ActionRow>(
					pickBox,
					rpl::single(name),
					u"Свободных прокси: "_q + QString::number(count),
					&st::menuIconNetwork)
				)->setClickedCallback([=] {
					state->proxyPoolName = name;
					proxyBtn->setText(rpl::single(getProxyTitle(name)));
					SaveSettings(*state);
					pickBox->closeBox();
				});
				Ui::AddSkip(pickBox->verticalLayout(), st::defaultVerticalListSkip);
			}
			pickBox->addButton(tr::lng_close(), [=] { pickBox->closeBox(); });
		}));
	});

	const auto manageProxyBtn = inProxy->add(
		object_ptr<Ui::RoundButton>(
			inProxy,
			rpl::single(u"Управление списками прокси"_q),
			st::defaultBoxButton),
		style::margins(0, 0, 0, style::ConvertScale(8)));
	manageProxyBtn->setTextTransform(Ui::RoundButtonTextTransform::NoTransform);

	manageProxyBtn->setClickedCallback([=] {
		show->show(MakeProxyPoolManagerBox());
	});

	const auto precheckCheck = inProxy->add(
		object_ptr<Ui::Checkbox>(
			inProxy,
			u"Проверять прокси перед заказом номера"_q,
			state->precheckProxy),
		style::margins(0, style::ConvertScale(4), 0, style::ConvertScale(2)));
	inProxy->add(
		object_ptr<Ui::FlatLabel>(
			inProxy,
			rpl::single(u"Перед списанием денег за SMS тестирует доступность сокета прокси. Если прокси недоступен, номер не заказывается и баланс не расходуется."_q),
			st::boxDividerLabel),
		style::margins(0, 0, 0, style::ConvertScale(6)));

	precheckCheck->checkedChanges() | rpl::on_next([=](bool checked) {
		state->precheckProxy = checked;
		SaveSettings(*state);
	}, precheckCheck->lifetime());

	const auto folderField = inProxy->add(
		object_ptr<Ui::InputField>(
			inProxy,
			FieldStyle(),
			rpl::single(u"Папка в GGTG (например: Регистрация)"_q),
			state->folderTag.isEmpty() ? u"Регистрация"_q : state->folderTag),
		style::margins(0, style::ConvertScale(4), 0, style::ConvertScale(6)));

	folderField->changes() | rpl::on_next([=] {
		state->folderTag = folderField->getLastText().trimmed();
		SaveSettings(*state);
	}, folderField->lifetime());

	const auto openFolderBtn = inProxy->add(
		object_ptr<Ui::RoundButton>(
			inProxy,
			rpl::single(u"Открыть папку аккаунтов на диске (Documents/GGTG)"_q),
			st::defaultBoxButton),
		style::margins(0, 0, 0, style::ConvertScale(4)));
	openFolderBtn->setTextTransform(Ui::RoundButtonTextTransform::NoTransform);

	openFolderBtn->setClickedCallback([=] {
		const auto tag = folderField->getLastText().trimmed();
		const auto docs = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + u"/GGTG"_q;
		const auto folder = tag.isEmpty() ? docs : (docs + '/' + tag);
		QDir().mkpath(folder);
		QDesktopServices::openUrl(QUrl::fromLocalFile(folder));
	});

	// =====================================================================
	//  4. ОБЛАЧНЫЙ ПАРОЛЬ (2FA)
	// =====================================================================
	addSectionTo(layout,
		u"Облачный пароль (2FA)"_q,
		u"Двухфакторная защита для новых аккаунтов"_q,
		&st::menuIconLock);

	const auto card2fa = layout->add(
		object_ptr<Ggtg::CardBox>(layout, style::margins(16, 14, 16, 14)),
		st::boxRowPadding);
	const auto in2fa = card2fa->inner();

	const auto single2faCheck = in2fa->add(
		object_ptr<Ui::Checkbox>(
			in2fa,
			u"Один пароль на все аккаунты"_q,
			state->single2faForAll),
		style::margins(0, style::ConvertScale(4), 0, style::ConvertScale(2)));
	in2fa->add(
		object_ptr<Ui::FlatLabel>(
			in2fa,
			rpl::single(u"Если включено — на всех создаваемых аккаунтах будет установлен указанный пароль. Если выключено — для каждого аккаунта генерируется свой уникальный надёжный пароль (18 символов), автоматически сохраняемый в 2FA Vault."_q),
			st::boxDividerLabel),
		style::margins(0, 0, 0, style::ConvertScale(6)));

	const auto single2faWrap = in2fa->add(
		object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			in2fa,
			object_ptr<Ui::VerticalLayout>(in2fa)),
		style::margins());
	const auto single2faLayout = single2faWrap->entity();

	const auto twoFactorField = single2faLayout->add(
		object_ptr<Ui::InputField>(
			single2faLayout,
			FieldStyle(),
			rpl::single(u"Пароль 2FA"_q),
			state->twoFactorPassword),
		style::margins(0, style::ConvertScale(4), 0, style::ConvertScale(6)));

	twoFactorField->changes() | rpl::on_next([=] {
		state->twoFactorPassword = twoFactorField->getLastText().trimmed();
		SaveSettings(*state);
	}, twoFactorField->lifetime());

	const auto pwdBtnsRow = single2faLayout->add(object_ptr<Ui::RpWidget>(single2faLayout));
	pwdBtnsRow->resize(pwdBtnsRow->width(), style::ConvertScale(38));

	const auto genPwdBtn = Ui::CreateChild<Ui::RoundButton>(
		pwdBtnsRow,
		rpl::single(u"Сгенерировать"_q),
		st::defaultBoxButton);
	genPwdBtn->setTextTransform(Ui::RoundButtonTextTransform::NoTransform);

	const auto copyPwdBtn = Ui::CreateChild<Ui::RoundButton>(
		pwdBtnsRow,
		rpl::single(u"Скопировать"_q),
		st::defaultBoxButton);
	copyPwdBtn->setTextTransform(Ui::RoundButtonTextTransform::NoTransform);

	pwdBtnsRow->widthValue() | rpl::on_next([=](int w) {
		genPwdBtn->moveToLeft(0, style::ConvertScale(2));
		copyPwdBtn->moveToLeft(genPwdBtn->width() + style::ConvertScale(10), style::ConvertScale(2));
	}, pwdBtnsRow->lifetime());

	genPwdBtn->setClickedCallback([=] {
		const auto pwd = GenerateStrongPassword(18);
		twoFactorField->setText(pwd);
		state->twoFactorPassword = pwd;
		SaveSettings(*state);
		RecordTwoFactorPassword(
			pwd,
			QString(),
			state->folderTag,
			u"Сгенерирован в форме регистрации"_q);
		TextUtilities::SetClipboardText(TextForMimeData::Simple(pwd));
		show->showToast(u"Пароль 2FA сгенерирован и скопирован в буфер"_q);
	});

	copyPwdBtn->setClickedCallback([=] {
		const auto current = twoFactorField->getLastText().trimmed();
		if (current.isEmpty()) {
			show->showToast(u"Поле пароля пустое"_q);
			return;
		}
		RecordTwoFactorPassword(
			current,
			QString(),
			state->folderTag,
			u"Скопирован из формы регистрации"_q);
		TextUtilities::SetClipboardText(TextForMimeData::Simple(current));
		show->showToast(u"Пароль 2FA скопирован в буфер обмена"_q);
	});

	const auto random2faWrap = in2fa->add(
		object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			in2fa,
			object_ptr<Ui::VerticalLayout>(in2fa)),
		style::margins());
	random2faWrap->entity()->add(
		object_ptr<Ui::FlatLabel>(
			random2faWrap->entity(),
			rpl::single(u"🔐 Для каждого создаваемого аккаунта автоматически генерируется уникальный надежный 18-значный пароль 2FA и сохраняется в базу и единый 2FA Vault."_q),
			st::boxLabel),
		style::margins(0, style::ConvertScale(4), 0, style::ConvertScale(4)));

	single2faWrap->toggle(state->single2faForAll, anim::type::instant);
	random2faWrap->toggle(!state->single2faForAll, anim::type::instant);

	single2faCheck->checkedChanges(
	) | rpl::on_next([=](bool checked) {
		state->single2faForAll = checked;
		single2faWrap->toggle(checked, anim::type::normal);
		random2faWrap->toggle(!checked, anim::type::normal);
		SaveSettings(*state);
	}, single2faCheck->lifetime());

	// =====================================================================
	//  5. ПРОЦЕСС РЕГИСТРАЦИИ
	// =====================================================================
	addSectionTo(layout,
		u"Процесс регистрации"_q,
		u"Управление процессом авторегистрации"_q,
		&st::menuIconManage);

	const auto cardProcess = layout->add(
		object_ptr<Ggtg::CardBox>(layout, style::margins(16, 14, 16, 14)),
		st::boxRowPadding);
	const auto inProcess = cardProcess->inner();

	const auto statusTextLabel = inProcess->add(
		object_ptr<Ui::FlatLabel>(
			inProcess,
			rpl::single(u"Готов к регистрации."_q),
			st::boxLabel),
		style::margins(0, style::ConvertScale(4), 0, style::ConvertScale(8)));

	const auto actionBtnRow = inProcess->add(object_ptr<Ui::RpWidget>(inProcess));
	actionBtnRow->resize(actionBtnRow->width(), style::ConvertScale(46));

	const auto startBtn = Ui::CreateChild<Ui::RoundButton>(
		actionBtnRow,
		rpl::single(u"Запустить регистрацию"_q),
		st::defaultActiveButton);

	const auto pauseBtn = Ui::CreateChild<Ui::RoundButton>(
		actionBtnRow,
		rpl::single(u"Пауза"_q),
		st::defaultBoxButton);
	pauseBtn->hide();

	const auto cancelBtn = Ui::CreateChild<Ui::RoundButton>(
		actionBtnRow,
		rpl::single(u"Остановить"_q),
		st::defaultActiveButton);
	cancelBtn->hide();

	actionBtnRow->widthValue() | rpl::on_next([=](int w) {
		const auto btnW = std::min(w, style::ConvertScale(340));
		startBtn->resizeToWidth(btnW);
		startBtn->moveToLeft((w - btnW) / 2, style::ConvertScale(4));

		const auto halfW = (btnW - style::ConvertScale(8)) / 2;
		pauseBtn->resizeToWidth(halfW);
		pauseBtn->moveToLeft((w - btnW) / 2, style::ConvertScale(4));
		cancelBtn->resizeToWidth(halfW);
		cancelBtn->moveToLeft((w - btnW) / 2 + halfW + style::ConvertScale(8), style::ConvertScale(4));
	}, actionBtnRow->lifetime());

	const auto weakStatusTextLabel = QPointer<Ui::FlatLabel>(statusTextLabel);
	const auto weakStartBtn = QPointer<Ui::RoundButton>(startBtn);
	const auto weakPauseBtn = QPointer<Ui::RoundButton>(pauseBtn);
	const auto weakCancelBtn = QPointer<Ui::RoundButton>(cancelBtn);

	regController->setStatusCallback([weakStatusTextLabel](QString msg, bool isErr, bool isOk) {
		if (!weakStatusTextLabel) return;
		weakStatusTextLabel->setText(msg);
		weakStatusTextLabel->setTextColorOverride(isErr
			? std::make_optional(st::boxTextFgError->c)
			: isOk
			? std::make_optional(st::boxTextFgGood->c)
			: std::nullopt);
	});

	regController->setRunningCallback([weakStartBtn, weakPauseBtn, weakCancelBtn](bool running) {
		if (weakStartBtn) weakStartBtn->setVisible(!running);
		if (weakPauseBtn) {
			weakPauseBtn->setVisible(running);
			if (!running) {
				weakPauseBtn->setText(rpl::single(u"Пауза"_q));
			}
		}
		if (weakCancelBtn) weakCancelBtn->setVisible(running);
	});

	regController->setPauseCallback([weakPauseBtn](bool paused) {
		if (weakPauseBtn) {
			weakPauseBtn->setText(rpl::single(paused ? u"Продолжить"_q : u"Пауза"_q));
		}
	});

	pauseBtn->setClickedCallback([=] {
		if (regController->isPaused()) {
			regController->resumeRegistration();
		} else {
			regController->pauseRegistration();
		}
	});

	cancelBtn->setClickedCallback([=] {
		regController->cancelRegistration(true);
	});

	if (regController->isRunning()) {
		startBtn->hide();
		pauseBtn->show();
		cancelBtn->show();
		pauseBtn->setText(rpl::single(regController->isPaused() ? u"Продолжить"_q : u"Пауза"_q));
		regController->updateOverallStatus();
	}

	// =====================================================================
	//  РАСШИРЕННЫЕ НАСТРОЙКИ (свёрнуты по умолчанию)
	// =====================================================================
	Ui::AddSkip(layout, style::ConvertScale(8));

	const auto advToggleCard = layout->add(
		object_ptr<Ggtg::CardBox>(layout, style::margins(16, 10, 16, 10)),
		st::boxRowPadding);
	const auto advToggle = advToggleCard->inner()->add(
		object_ptr<Ggtg::ActionRow>(
			advToggleCard->inner(),
			rpl::single(u"Расширенные настройки"_q),
			u"Email, токены, прокси, 2FA, потоки, задержки"_q,
			&st::menuIconSettings));

	const auto advancedWrap = layout->add(
		object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			layout,
			object_ptr<Ui::VerticalLayout>(layout)));
	advancedWrap->toggle(false, anim::type::instant);

	advToggle->setClickedCallback([=] {
		advancedWrap->toggle(
			!advancedWrap->toggled(),
			anim::type::normal);
	});

	const auto advLayout = advancedWrap->entity();

	// ----- Расш.: Email -----
	addSectionTo(advLayout,
		u"Покупка Email"_q,
		u"Автоматическая покупка временной почты при запросе Telegram"_q,
		&st::menuIconPhone);

	const auto cardEmail = advLayout->add(
		object_ptr<Ggtg::CardBox>(advLayout, style::margins(16, 14, 16, 14)),
		st::boxRowPadding);
	const auto inEmail = cardEmail->inner();

	const auto autoEmailCheck = inEmail->add(
		object_ptr<Ui::Checkbox>(
			inEmail,
			u"Покупать временную почту при запросе Telegram (Hero-SMS / SMSBower)"_q,
			state->autoBuyEmail),
		style::margins(0, style::ConvertScale(4), 0, style::ConvertScale(4)));

	const auto emailWrap = inEmail->add(
		object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			inEmail,
			object_ptr<Ui::VerticalLayout>(inEmail)),
		style::margins());
	const auto emailLayout = emailWrap->entity();

	emailLayout->add(
		object_ptr<Ui::FlatLabel>(
			emailLayout,
			rpl::single(u"Если Telegram потребует Email (SetUpEmailRequired), модуль сам закажет почту, получит код и завершит авторизацию."_q),
			st::boxDividerLabel),
		style::margins(0, style::ConvertScale(2), 0, style::ConvertScale(6)));

	const auto emailApiKeyField = emailLayout->add(
		object_ptr<Ui::InputField>(
			emailLayout,
			FieldStyle(),
			rpl::single(u"Отдельный API-ключ почты (необязательно, если совпадает с SMS)"_q),
			state->emailApiKey),
		style::margins(0, style::ConvertScale(6), 0, style::ConvertScale(4)));

	emailApiKeyField->changes(
	) | rpl::on_next([=] {
		state->emailApiKey = emailApiKeyField->getLastText().trimmed();
		SaveSettings(*state);
	}, emailApiKeyField->lifetime());

	emailWrap->toggle(state->autoBuyEmail, anim::type::instant);
	autoEmailCheck->checkedChanges(
	) | rpl::on_next([=](bool checked) {
		state->autoBuyEmail = checked;
		emailWrap->toggle(checked, anim::type::normal);
		SaveSettings(*state);
	}, autoEmailCheck->lifetime());

	// ----- Расш.: Эмуляция Android и токены -----
	addSectionTo(advLayout,
		u"Эмуляция Android и токены"_q,
		u"Параметры устройств Android и токены Play Integrity / SafetyNet"_q,
		&st::menuIconDevices);

	const auto cardDev = advLayout->add(
		object_ptr<Ggtg::CardBox>(advLayout, style::margins(16, 14, 16, 14)),
		st::boxRowPadding);
	const auto inDev = cardDev->inner();

	const auto dbInfoCount = CountBaseDbProfiles();
	inDev->add(
		object_ptr<Ui::FlatLabel>(
			inDev,
			rpl::single(dbInfoCount > 0
				? (u"Параметры Android: автоматическая генерация по базе Base.db (%1 моделей)"_q.arg(dbInfoCount))
				: u"Параметры Android: встроенный генератор AndroidProfiles (550+ моделей)"_q),
			st::boxLabel),
		style::margins(0, 0, 0, style::ConvertScale(8)));

	const auto tokenProviderTitle = [](int s) {
		if (s == 1) return u"TokenFast (tokenfast.shop/api)"_q;
		if (s == 2) return u"AntiSafety (antisafety.net)"_q;
		return u"Без сервиса токенов (только Base.db)"_q;
	};
	const auto advTokenServiceBtn = inDev->add(
		object_ptr<PickerRow>(
			inDev,
			u"Сервис токенов"_q,
			tokenProviderTitle(state->tokenService)));

	advTokenServiceBtn->setClickedCallback([=] {
		show->show(Box([=](not_null<Ui::GenericBox*> pickBox) {
			pickBox->setTitle(rpl::single(u"Выберите сервис токенов Play Integrity / SafetyNet"_q));
			pickBox->setWidth(st::boxWideWidth);

			const auto options = std::vector<std::pair<int, QString>>{
				{ 1, u"TokenFast (tokenfast.shop/api)"_q },
				{ 2, u"AntiSafety (antisafety.net)"_q },
				{ 0, u"Без сервиса токенов (только параметры устройства)"_q },
			};
			for (const auto &opt : options) {
				pickBox->addRow(object_ptr<Ggtg::ActionRow>(
					pickBox,
					rpl::single(opt.second),
					opt.first == 1 ? u"api.tokenfast.shop"_q : (opt.first == 2 ? u"antisafety.net"_q : QString()),
					&st::menuIconDiscussion)
				)->setClickedCallback([=] {
					state->tokenService = opt.first;
					advTokenServiceBtn->setText(rpl::single(tokenProviderTitle(opt.first)));
					tokenServiceBtn->setText(rpl::single(TokenProviderName(opt.first)));
					SaveSettings(*state);
					pickBox->closeBox();
				});
				Ui::AddSkip(pickBox->verticalLayout(), st::defaultVerticalListSkip);
			}
			pickBox->addButton(tr::lng_close(), [=] { pickBox->closeBox(); });
		}));
	});

	const auto advTokenApiKeyField = inDev->add(
		object_ptr<Ui::InputField>(
			inDev,
			FieldStyle(),
			rpl::single(u"API-ключ сервиса токенов (TokenFast / AntiSafety)"_q),
			state->tokenApiKey),
		style::margins(0, style::ConvertScale(6), 0, style::ConvertScale(6)));

	advTokenApiKeyField->changes(
	) | rpl::on_next([=] {
		const auto key = advTokenApiKeyField->getLastText().trimmed();
		state->tokenApiKey = key;
		if (tokenApiKeyField->getLastText().trimmed() != key) {
			tokenApiKeyField->setText(key);
		}
		SaveSettings(*state);
	}, advTokenApiKeyField->lifetime());

	const auto advCheckTokenRow = inDev->add(object_ptr<Ui::RpWidget>(inDev));
	advCheckTokenRow->resize(advCheckTokenRow->width(), style::ConvertScale(38));

	const auto advCheckTokenBtn = Ui::CreateChild<Ui::RoundButton>(
		advCheckTokenRow,
		rpl::single(u"Проверить баланс"_q),
		st::defaultActiveButton);

	const auto advTokenStatusLabel = Ui::CreateChild<Ui::FlatLabel>(
		advCheckTokenRow,
		rpl::single(QString()),
		st::boxLabel);

	advCheckTokenRow->widthValue() | rpl::on_next([=](int w) {
		advCheckTokenBtn->moveToLeft(0, style::ConvertScale(2));
		advTokenStatusLabel->moveToLeft(advCheckTokenBtn->width() + style::ConvertScale(14), style::ConvertScale(8));
	}, advCheckTokenRow->lifetime());

	const auto weakAdvTokenStatusLabel = QPointer<Ui::FlatLabel>(advTokenStatusLabel);
	advCheckTokenBtn->setClickedCallback([=] {
		const auto key = advTokenApiKeyField->getLastText().trimmed();
		state->tokenApiKey = key;
		SaveSettings(*state);
		if (weakAdvTokenStatusLabel) weakAdvTokenStatusLabel->setText(u"Проверка..."_q);
		CheckMobileTokenBalance(
			MobileTokenProvider(state->tokenService),
			key,
			[weakAdvTokenStatusLabel](QString bal, QString err) {
				if (!weakAdvTokenStatusLabel) return;
				if (!err.isEmpty()) {
					weakAdvTokenStatusLabel->setText(u"Ошибка: "_q + err);
				} else {
					weakAdvTokenStatusLabel->setText(u"Баланс: "_q + bal);
				}
			});
	});

	// --- Сервис решения капчи (reCAPTCHA Enterprise) ---
	Ui::AddSkip(inDev, style::ConvertScale(8));
	const auto advCaptchaServiceBtn = inDev->add(
		object_ptr<PickerRow>(
			inDev,
			u"Сервис капчи (reCAPTCHA)"_q,
			CaptchaProviderName(state->captchaService)));

	advCaptchaServiceBtn->setClickedCallback([=] {
		show->show(Box([=](not_null<Ui::GenericBox*> pickBox) {
			pickBox->setTitle(rpl::single(u"Выберите сервис решения капчи (reCAPTCHA Enterprise)"_q));
			pickBox->setWidth(st::boxWideWidth);

			const auto options = std::vector<std::pair<int, QString>>{
				{ 0, u"Авто (из сервиса токенов TokenFast / AntiSafety)"_q },
				{ 1, u"TokenFast (api.tokenfast.shop)"_q },
				{ 2, u"AntiSafety (antisafety.net)"_q },
				{ 3, u"NextCaptcha (nextcaptcha.com)"_q },
			};
			for (const auto &opt : options) {
				pickBox->addRow(object_ptr<Ggtg::ActionRow>(
					pickBox,
					rpl::single(opt.second),
					opt.first == 3 ? u"dashboard.nextcaptcha.com"_q : (opt.first == 1 ? u"api.tokenfast.shop"_q : QString()),
					&st::menuIconManage)
				)->setClickedCallback([=] {
					state->captchaService = opt.first;
					advCaptchaServiceBtn->setText(rpl::single(CaptchaProviderName(opt.first)));
					SaveSettings(*state);
					pickBox->closeBox();
				});
				Ui::AddSkip(pickBox->verticalLayout(), st::defaultVerticalListSkip);
			}
			pickBox->addButton(tr::lng_close(), [=] { pickBox->closeBox(); });
		}));
	});

	const auto advCaptchaApiKeyField = inDev->add(
		object_ptr<Ui::InputField>(
			inDev,
			FieldStyle(),
			rpl::single(u"API-ключ капчи (если отличается от ключа токенов)"_q),
			state->captchaApiKey),
		style::margins(0, style::ConvertScale(6), 0, style::ConvertScale(6)));

	advCaptchaApiKeyField->changes(
	) | rpl::on_next([=] {
		state->captchaApiKey = advCaptchaApiKeyField->getLastText().trimmed();
		SaveSettings(*state);
	}, advCaptchaApiKeyField->lifetime());

	const auto advCheckCaptchaRow = inDev->add(object_ptr<Ui::RpWidget>(inDev));
	advCheckCaptchaRow->resize(advCheckCaptchaRow->width(), style::ConvertScale(38));

	const auto advCheckCaptchaBtn = Ui::CreateChild<Ui::RoundButton>(
		advCheckCaptchaRow,
		rpl::single(u"Проверить баланс капчи"_q),
		st::defaultActiveButton);

	const auto advCaptchaStatusLabel = Ui::CreateChild<Ui::FlatLabel>(
		advCheckCaptchaRow,
		rpl::single(QString()),
		st::boxLabel);

	advCheckCaptchaRow->widthValue() | rpl::on_next([=](int w) {
		advCheckCaptchaBtn->moveToLeft(0, style::ConvertScale(2));
		advCaptchaStatusLabel->moveToLeft(advCheckCaptchaBtn->width() + style::ConvertScale(14), style::ConvertScale(8));
	}, advCheckCaptchaRow->lifetime());

	const auto weakAdvCaptchaStatusLabel = QPointer<Ui::FlatLabel>(advCaptchaStatusLabel);
	advCheckCaptchaBtn->setClickedCallback([=] {
		auto key = advCaptchaApiKeyField->getLastText().trimmed();
		if (key.isEmpty()) {
			key = state->tokenApiKey.trimmed();
		}
		SaveSettings(*state);
		if (weakAdvCaptchaStatusLabel) weakAdvCaptchaStatusLabel->setText(u"Проверка..."_q);
		int provider = state->captchaService;
		if (provider == 0) {
			provider = state->tokenService;
		}
		CheckCaptchaBalance(
			provider,
			key,
			[weakAdvCaptchaStatusLabel](QString bal, QString err) {
				if (!weakAdvCaptchaStatusLabel) return;
				if (!err.isEmpty()) {
					weakAdvCaptchaStatusLabel->setText(u"Ошибка: "_q + err);
				} else {
					weakAdvCaptchaStatusLabel->setText(u"Баланс: "_q + bal);
				}
			});
	});

	const auto replaceTokenCheck = inDev->add(
		object_ptr<Ui::Checkbox>(
			inDev,
			u"Заменять токен при флуде номера, неиспользованные токены хранить в резерве"_q,
			state->replaceTokenOnFlood),
		style::margins(0, style::ConvertScale(8), 0, style::ConvertScale(2)));
	replaceTokenCheck->checkedChanges(
	) | rpl::on_next([=](bool checked) {
		state->replaceTokenOnFlood = checked;
		SaveSettings(*state);
	}, replaceTokenCheck->lifetime());

	Ui::AddSkip(inDev, style::ConvertScale(8));
	const auto autoExportCheck = inDev->add(
		object_ptr<Ui::Checkbox>(
			inDev,
			u"Автоматически формировать пакет .ggtg для приложения Android"_q,
			state->autoExportGgtg),
		style::margins(0, style::ConvertScale(4), 0, style::ConvertScale(2)));

	inDev->add(
		object_ptr<Ui::FlatLabel>(
			inDev,
			rpl::single(u"В файл .ggtg включаются: авторизационные ключи Android, прокси, пароль 2FA и параметры устройства. Файлы сохраняются в общую папку аккаунтов на диске."_q),
			st::boxDividerLabel),
		style::margins(0, 0, 0, style::ConvertScale(6)));

	autoExportCheck->checkedChanges(
	) | rpl::on_next([=](bool checked) {
		state->autoExportGgtg = checked;
		SaveSettings(*state);
	}, autoExportCheck->lifetime());

	// ----- Расш.: Настройки SMS-заказа -----
	addSectionTo(advLayout,
		u"Настройки SMS-заказа"_q,
		u"Оператор, макс. цена"_q,
		&st::menuIconPhone);

	const auto cardSms = advLayout->add(
		object_ptr<Ggtg::CardBox>(advLayout, style::margins(16, 14, 16, 14)),
		st::boxRowPadding);
	const auto inSms = cardSms->inner();

	const auto advOperatorField = inSms->add(
		object_ptr<Ui::InputField>(
			inSms,
			FieldStyle(),
			rpl::single(u"Оператор (пусто или any = любой)"_q),
			state->smsOperator),
		style::margins(0, 0, 0, style::ConvertScale(6)));

	advOperatorField->changes() | rpl::on_next([=] {
		const auto val = advOperatorField->getLastText().trimmed();
		state->smsOperator = val;
		updateOperatorSubtitle();
		SaveSettings(*state);
	}, advOperatorField->lifetime());

	const auto advMaxPriceField = inSms->add(
		object_ptr<Ui::InputField>(
			inSms,
			FieldStyle(),
			rpl::single(u"Макс. цена за номер (пусто = без лимита)"_q),
			state->maxPrice),
		style::margins(0, 0, 0, style::ConvertScale(6)));

	advMaxPriceField->changes() | rpl::on_next([=] {
		const auto val = advMaxPriceField->getLastText().trimmed();
		state->maxPrice = val;
		if (maxPriceField->getLastText().trimmed() != val) {
			maxPriceField->setText(val);
		}
		SaveSettings(*state);
	}, advMaxPriceField->lifetime());

	inSms->add(
		object_ptr<Ui::FlatLabel>(
			inSms,
			rpl::single(u"Оператор и максимальная цена передаются SMS-сервису при заказе номера. Не все сервисы поддерживают эти параметры."_q),
			st::boxDividerLabel),
		style::margins(0, 0, 0, style::ConvertScale(6)));

	const auto resetAfterRegCheck = inSms->add(
		object_ptr<Ui::Checkbox>(
			inSms,
			u"Сбрасывать другие сессии после регистрации"_q,
			state->resetSessionsAfterReg),
		style::margins(0, style::ConvertScale(4), 0, style::ConvertScale(2)));
	inSms->add(
		object_ptr<Ui::FlatLabel>(
			inSms,
			rpl::single(u"Вызывает ResetAuthorizations после успешной регистрации — завершает все другие сессии на номере."_q),
			st::boxDividerLabel),
		style::margins(0, 0, 0, style::ConvertScale(6)));
	resetAfterRegCheck->checkedChanges(
	) | rpl::on_next([=](bool checked) {
		state->resetSessionsAfterReg = checked;
		SaveSettings(*state);
	}, resetAfterRegCheck->lifetime());

	const auto reset2faCheck = inSms->add(
		object_ptr<Ui::Checkbox>(
			inSms,
			u"Запрашивать сброс 2FA на занятых номерах"_q,
			state->reset2faOnOccupied),
		style::margins(0, style::ConvertScale(4), 0, style::ConvertScale(2)));
	inSms->add(
		object_ptr<Ui::FlatLabel>(
			inSms,
			rpl::single(u"Если номер уже зарегистрирован с паролем 2FA, отправляет запрос на сброс пароля (ResetPasswordRequest) перед отменой номера."_q),
			st::boxDividerLabel),
		style::margins(0, 0, 0, style::ConvertScale(6)));
	reset2faCheck->checkedChanges(
	) | rpl::on_next([=](bool checked) {
		state->reset2faOnOccupied = checked;
		SaveSettings(*state);
	}, reset2faCheck->lifetime());



	// ----- Расш.: Параметры очереди -----
	addSectionTo(advLayout,
		u"Параметры очереди"_q,
		u"Потоки, задержки и фильтрация номеров"_q,
		&st::menuIconStats);

	const auto cardQueue = advLayout->add(
		object_ptr<Ggtg::CardBox>(advLayout, style::margins(16, 14, 16, 14)),
		st::boxRowPadding);
	const auto inQueue = cardQueue->inner();

	const auto threadsField = inQueue->add(
		object_ptr<Ui::InputField>(
			inQueue,
			FieldStyle(),
			rpl::single(u"Параллельные потоки (1–10)"_q),
			QString::number(state->threadsCount)),
		style::margins(0, 0, 0, style::ConvertScale(8)));

	// Пауза между шагами
	inQueue->add(
		object_ptr<Ui::FlatLabel>(
			inQueue,
			rpl::single(u"Пауза между шагами регистрации (сек):"_q),
			st::boxLabel),
		style::margins(0, 0, 0, style::ConvertScale(4)));

	const auto stepDelayRow = inQueue->add(object_ptr<Ui::RpWidget>(inQueue));
	stepDelayRow->resize(stepDelayRow->width(), style::ConvertScale(42));

	const auto stepMinLabel = Ui::CreateChild<Ui::FlatLabel>(
		stepDelayRow,
		rpl::single(u"от"_q),
		st::boxLabel);
	const auto stepMinField = Ui::CreateChild<Ui::InputField>(
		stepDelayRow,
		CompactFieldStyle(),
		rpl::single(QString()),
		QString::number(state->stepDelayMin));
	stepMinField->resize(style::ConvertScale(76), stepMinField->height());

	const auto stepMaxLabel = Ui::CreateChild<Ui::FlatLabel>(
		stepDelayRow,
		rpl::single(u"до"_q),
		st::boxLabel);
	const auto stepMaxField = Ui::CreateChild<Ui::InputField>(
		stepDelayRow,
		CompactFieldStyle(),
		rpl::single(QString()),
		QString::number(state->stepDelayMax));
	stepMaxField->resize(style::ConvertScale(76), stepMaxField->height());

	const auto stepSecLabel = Ui::CreateChild<Ui::FlatLabel>(
		stepDelayRow,
		rpl::single(u"секунд"_q),
		st::boxLabel);

	stepDelayRow->widthValue() | rpl::on_next([=](int w) {
		stepMinLabel->moveToLeft(0, style::ConvertScale(11));
		stepMinField->moveToLeft(stepMinLabel->width() + style::ConvertScale(8), style::ConvertScale(3));
		stepMaxLabel->moveToLeft(stepMinField->x() + stepMinField->width() + style::ConvertScale(12), style::ConvertScale(11));
		stepMaxField->moveToLeft(stepMaxLabel->x() + stepMaxLabel->width() + style::ConvertScale(8), style::ConvertScale(3));
		stepSecLabel->moveToLeft(stepMaxField->x() + stepMaxField->width() + style::ConvertScale(8), style::ConvertScale(11));
	}, stepDelayRow->lifetime());

	Ui::AddSkip(inQueue, style::ConvertScale(4));

	// Пауза между аккаунтами
	inQueue->add(
		object_ptr<Ui::FlatLabel>(
			inQueue,
			rpl::single(u"Пауза между аккаунтами в потоке (сек):"_q),
			st::boxLabel),
		style::margins(0, 0, 0, style::ConvertScale(4)));

	const auto accDelayRow = inQueue->add(object_ptr<Ui::RpWidget>(inQueue));
	accDelayRow->resize(accDelayRow->width(), style::ConvertScale(42));

	const auto accMinLabel = Ui::CreateChild<Ui::FlatLabel>(
		accDelayRow,
		rpl::single(u"от"_q),
		st::boxLabel);
	const auto accMinField = Ui::CreateChild<Ui::InputField>(
		accDelayRow,
		CompactFieldStyle(),
		rpl::single(QString()),
		QString::number(state->accountDelayMin));
	accMinField->resize(style::ConvertScale(76), accMinField->height());

	const auto accMaxLabel = Ui::CreateChild<Ui::FlatLabel>(
		accDelayRow,
		rpl::single(u"до"_q),
		st::boxLabel);
	const auto accMaxField = Ui::CreateChild<Ui::InputField>(
		accDelayRow,
		CompactFieldStyle(),
		rpl::single(QString()),
		QString::number(state->accountDelayMax));
	accMaxField->resize(style::ConvertScale(76), accMaxField->height());

	const auto accSecLabel = Ui::CreateChild<Ui::FlatLabel>(
		accDelayRow,
		rpl::single(u"секунд"_q),
		st::boxLabel);

	accDelayRow->widthValue() | rpl::on_next([=](int w) {
		accMinLabel->moveToLeft(0, style::ConvertScale(11));
		accMinField->moveToLeft(accMinLabel->width() + style::ConvertScale(8), style::ConvertScale(3));
		accMaxLabel->moveToLeft(accMinField->x() + accMinField->width() + style::ConvertScale(12), style::ConvertScale(11));
		accMaxField->moveToLeft(accMaxLabel->x() + accMaxLabel->width() + style::ConvertScale(8), style::ConvertScale(3));
		accSecLabel->moveToLeft(accMaxField->x() + accMaxField->width() + style::ConvertScale(8), style::ConvertScale(11));
	}, accDelayRow->lifetime());

	Ui::AddSkip(inQueue, style::ConvertScale(4));

	// Ожидание SMS
	inQueue->add(
		object_ptr<Ui::FlatLabel>(
			inQueue,
			rpl::single(u"Сколько ждать SMS-код (сек):"_q),
			st::boxLabel),
		style::margins(0, 0, 0, style::ConvertScale(4)));

	const auto smsWaitRow = inQueue->add(object_ptr<Ui::RpWidget>(inQueue));
	smsWaitRow->resize(smsWaitRow->width(), style::ConvertScale(42));

	const auto smsWaitField = Ui::CreateChild<Ui::InputField>(
		smsWaitRow,
		CompactFieldStyle(),
		rpl::single(QString()),
		QString::number(state->smsWaitSeconds));
	smsWaitField->resize(style::ConvertScale(76), smsWaitField->height());

	const auto smsWaitSecLabel = Ui::CreateChild<Ui::FlatLabel>(
		smsWaitRow,
		rpl::single(u"секунд (от 120 до 1800, рекомендуется 300)"_q),
		st::boxLabel);

	smsWaitRow->widthValue() | rpl::on_next([=](int w) {
		smsWaitField->moveToLeft(0, style::ConvertScale(3));
		smsWaitSecLabel->moveToLeft(smsWaitField->width() + style::ConvertScale(10), style::ConvertScale(11));
	}, smsWaitRow->lifetime());

	inQueue->add(
		object_ptr<Ui::FlatLabel>(
			inQueue,
			rpl::single(u"Номер обычно живёт у сервиса около 20 минут, а слишком короткое ожидание отменяет номера, на которые код ещё придёт (и баланс может потеряться). Если SMS не придёт за это время, программа отменит номер и вернёт средства в сервис (статус 8)."_q),
			st::boxDividerLabel),
		style::margins(0, style::ConvertScale(4), 0, style::ConvertScale(10)));

	const auto queueSkipAppCheck = inQueue->add(
		object_ptr<Ui::Checkbox>(
			inQueue,
			u"Отклонять номер, если код отправлен в приложение (номер занят)"_q,
			state->skipAppCode),
		style::margins(0, style::ConvertScale(4), 0, style::ConvertScale(2)));
	inQueue->add(
		object_ptr<Ui::FlatLabel>(
			inQueue,
			rpl::single(u"Если номер уже зарегистрирован в Telegram, код отправляется в приложение, а не в SMS. Опция автоматически отменяет номер (статус 8) и возвращает средства в SMS-сервис."_q),
			st::boxDividerLabel),
		style::margins(0, 0, 0, style::ConvertScale(6)));
	*queueSkipAppHolder = queueSkipAppCheck;
	queueSkipAppCheck->checkedChanges(
	) | rpl::on_next([=](bool checked) {
		state->skipAppCode = checked;
		skipAppCheck->setChecked(checked);
		SaveSettings(*state);
	}, queueSkipAppCheck->lifetime());

	const auto filterCheck = inQueue->add(
		object_ptr<Ui::Checkbox>(
			inQueue,
			u"Фильтровать номера перед регистрацией"_q,
			state->filterNumbers),
		style::margins(0, style::ConvertScale(4), 0, style::ConvertScale(2)));
	inQueue->add(
		object_ptr<Ui::FlatLabel>(
			inQueue,
			rpl::single(u"Программа помнит номера, которые уже не подошли (заняты, заблокированы, неверные), и сразу отменяет их при повторной выдаче, не запрашивая код."_q),
			st::boxDividerLabel),
		style::margins(0, 0, 0, style::ConvertScale(6)));
	filterCheck->checkedChanges(
	) | rpl::on_next([=](bool checked) {
		state->filterNumbers = checked;
		SaveSettings(*state);
	}, filterCheck->lifetime());

	// =====================================================================
	//  ЖУРНАЛ ДЕЙСТВИЙ (всегда виден)
	// =====================================================================
	addSectionTo(layout,
		u"Журнал действий"_q,
		u"Лог работы фоновых потоков регистрации в реальном времени"_q,
		&st::menuIconRestore);

	const auto cardLog = layout->add(
		object_ptr<Ggtg::CardBox>(layout, style::margins(14, 12, 14, 12)),
		st::boxRowPadding);
	const auto inLog = cardLog->inner();

	const auto logField = inLog->add(
		object_ptr<Ui::InputField>(
			inLog,
			LogFieldStyle(),
			Ui::InputField::Mode::MultiLine,
			rpl::single(u"Лог регистрации..."_q),
			QString()),
		style::margins(0, 0, 0, 0));
	logField->rawTextEdit()->setReadOnly(true);

	if (!regController->logHistory().isEmpty()) {
		logField->setText(regController->logHistory().join('\n'));
		auto cursor = logField->rawTextEdit()->textCursor();
		cursor.movePosition(QTextCursor::End);
		logField->rawTextEdit()->setTextCursor(cursor);
	}

	const auto weakLogField = QPointer<Ui::InputField>(logField);
	regController->setLogCallback([weakLogField](QString msg) {
		if (!weakLogField) return;
		const auto prev = weakLogField->getLastText();
		const auto updated = prev.isEmpty() ? msg : (prev + '\n' + msg);
		const auto lines = updated.split('\n');
		const auto tail = lines.mid(std::max(0, int(lines.size()) - 100)).join('\n');
		weakLogField->setText(tail);
		if (const auto edit = weakLogField->rawTextEdit()) {
			auto cursor = edit->textCursor();
			cursor.movePosition(QTextCursor::End);
			edit->setTextCursor(cursor);
		}
	});

	// =====================================================================
	//  ОБРАБОТЧИК ЗАПУСКА
	// =====================================================================
	startBtn->setClickedCallback([=] {
		state->apiKey = apiKeyField->getLastText().trimmed();
		state->apiKeys.insert(state->serviceId, state->apiKey);
		state->customUrl = customWrap->entity()->getLastText().trimmed();
		state->folderTag = folderField->getLastText().trimmed();
		if (state->folderTag.isEmpty()) state->folderTag = u"Регистрация"_q;
		state->single2faForAll = single2faCheck->checked();
		state->twoFactorPassword = twoFactorField->getLastText().trimmed();
		state->singleNameForAll = singleNameCheck->checked();
		state->autoRandomNames = !singleNameCheck->checked();
		state->customFirstName = firstNameField->getLastText().trimmed();
		state->customLastName = lastNameField->getLastText().trimmed();
		state->emailApiKey = emailApiKeyField->getLastText().trimmed();
		state->tokenApiKey = tokenApiKeyField->getLastText().trimmed();
		state->smsOperator = state->smsOperator.trimmed();
		state->maxPrice = maxPriceField->getLastText().trimmed();
		state->firstNameFile = state->firstNameFile.trimmed();
		state->lastNameFile = state->lastNameFile.trimmed();

		state->targetSuccessCount = std::max(1, targetSuccessField->getLastText().trimmed().toInt());
		if (state->targetSuccessCount <= 0) state->targetSuccessCount = 10;
		state->threadsCount = std::max(1, std::min(10, threadsField->getLastText().trimmed().toInt()));
		if (state->threadsCount <= 0) state->threadsCount = 2;

		state->stepDelayMin = std::max(0, stepMinField->getLastText().trimmed().toInt());
		state->stepDelayMax = std::max(state->stepDelayMin, stepMaxField->getLastText().trimmed().toInt());
		state->accountDelayMin = std::max(0, accMinField->getLastText().trimmed().toInt());
		state->accountDelayMax = std::max(state->accountDelayMin, accMaxField->getLastText().trimmed().toInt());
		state->smsWaitSeconds = std::clamp(smsWaitField->getLastText().trimmed().toInt(), 120, 1800);

		SaveSettings(*state);

		if (state->apiKey.isEmpty() && state->mode == 0) {
			show->showToast(u"Введите API-ключ SMS-сервиса"_q);
			return;
		}

		if (state->countryId < 0 && state->mode == 0) {
			show->showToast(u"Выберите страну номера перед запуском"_q);
			return;
		}

		CurrentRegistrationController() = regController;
		regController->startAutoRegistration(*state);
	});

	box->addButton(tr::lng_close(), [=] {
		SaveSettings(*state);
		box->closeBox();
	});
}

inline object_ptr<Ui::GenericBox> MakeRegistrationBox(not_null<Window::SessionController*> controller) {
	return Box([=](not_null<Ui::GenericBox*> box) {
		FillRegistrationBox(box, controller);
	});
}

} // namespace Registration

inline object_ptr<Ui::GenericBox> MakeRegistrationBox(not_null<Window::SessionController*> controller) {
	return Registration::MakeRegistrationBox(controller);
}

} // namespace Settings

