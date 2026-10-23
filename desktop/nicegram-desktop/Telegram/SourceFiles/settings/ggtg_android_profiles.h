// ggtg: random Android client profile (device, Android version, Telegram build, language) that
// Telegram shows consistently in the account's session list. Same steps as the reference
// generator: brand -> SDK of that brand -> model of that brand + SDK -> build -> language.
// The data is ggtg_android_profiles_data.h (tools/gen_android_profiles.py); Android mirrors this
// in GgtgAndroidProfiles.kt.
#pragma once

#include "settings/ggtg_android_profiles_data.h"
#include "core/ggtg_geo.h"
#include "base/random.h"

#include <QtCore/QString>
#include <QtCore/QStringList>

#include <algorithm>
#include <iterator>
#include <optional>
#include <vector>

namespace Ggtg::AndroidProfiles {

constexpr auto kAppId = 6;
constexpr auto kAppHash = "eb06d4abfb49dc3eeb1aeb98ae0f581e";
constexpr auto kBetaAppId = 4;
constexpr auto kBetaAppHash = "014b35b6184100b085b0d0572f9b5103";
constexpr auto kLangPack = "android";
constexpr auto kPerfCat = 2;

struct PhoneModel {
	const char *label;
	const char *model;
	const char *system;
};

inline constexpr PhoneModel kCuratedPhones[] = {
	{ "Samsung Galaxy S25 Ultra", "samsungSM-S938B", "SDK 35" },
	{ "Samsung Galaxy S24 Ultra", "samsungSM-S928B", "SDK 34" },
	{ "Samsung Galaxy S24", "samsungSM-S921B", "SDK 34" },
	{ "Samsung Galaxy S23", "samsungSM-S911B", "SDK 34" },
	{ "Samsung Galaxy A55 5G", "samsungSM-A556B", "SDK 34" },
	{ "Samsung Galaxy A54 5G", "samsungSM-A546B", "SDK 34" },
	{ "Samsung Galaxy A35 5G", "samsungSM-A356B", "SDK 34" },
	{ "Samsung Galaxy Z Flip5", "samsungSM-F731B", "SDK 34" },
	{ "Google Pixel 9 Pro", "GooglePixel 9 Pro", "SDK 35" },
	{ "Google Pixel 9", "GooglePixel 9", "SDK 35" },
	{ "Google Pixel 8 Pro", "GooglePixel 8 Pro", "SDK 35" },
	{ "Google Pixel 8", "GooglePixel 8", "SDK 35" },
	{ "Google Pixel 7", "GooglePixel 7", "SDK 35" },
	{ "Google Pixel 7a", "GooglePixel 7a", "SDK 35" },
	{ "Xiaomi 14", "Xiaomi23127PN0CG", "SDK 34" },
	{ "Xiaomi 13", "Xiaomi2211133G", "SDK 33" },
	{ "Redmi Note 13 Pro", "Xiaomi2312DRA50G", "SDK 34" },
	{ "POCO F6", "Xiaomi24069PC21G", "SDK 34" },
	{ "OnePlus 12", "OnePlusCPH2581", "SDK 34" },
	{ "OnePlus 11", "OnePlusCPH2449", "SDK 34" },
	{ "realme GT 6", "realmeRMX3851", "SDK 34" },
	{ "Nothing Phone (2)", "NothingA065", "SDK 34" },
	{ "Motorola Moto G84", "motorolaXT2347-2", "SDK 34" },
	{ "HONOR Magic6 Pro", "HONORBVL-N49", "SDK 34" },
};

inline constexpr const char *kCuratedSystems[] = {
	"SDK 35",
	"SDK 34",
	"SDK 33",
	"SDK 31",
	"SDK 30",
};

inline constexpr const char *kCuratedBuilds[] = {
	"12.10.6 (71129)",
	"12.10.4 (70999)",
	"12.9.3 (70459)",
	"12.8.1 (69929)",
	"12.7.0 (69359)",
};

[[nodiscard]] inline QString PhoneLabel(const QString &model) {
	for (const auto &p : kCuratedPhones) {
		if (QString::fromUtf8(p.model) == model) {
			return QString::fromUtf8(p.label);
		}
	}
	return model;
}

struct Profile {
	QString deviceModel; // Build.MANUFACTURER + Build.MODEL, e.g. "XiaomiRedmi 9C".
	QString systemVersion; // "SDK 30".
	QString appVersion; // "12.10.6 (71129)".
	QString cloudLang; // "ru", "id".
	QString systemLang; // "ru-ru", "in-id".
};

struct Language {
	QString cloud;
	QString system;
};

[[nodiscard]] inline std::vector<Language> Languages() {
	auto result = std::vector<Language>();
	for (const auto line : kLangs) {
		const auto parts = QString::fromLatin1(line).split(QChar('|'));
		result.push_back({ parts[0], parts[1] });
	}
	return result;
}

// A language from the catalog for an Android locale code ("ru-ru"), if the
// generator knows it.
[[nodiscard]] inline std::optional<Language> LanguageForSystemCode(
		const QString &systemCode) {
	const auto lower = systemCode.toLower();
	for (const auto &language : Languages()) {
		if (language.system == lower) {
			return language;
		}
	}
	return std::nullopt;
}

[[nodiscard]] inline QString RandomBuild() {
	constexpr auto count = int(std::size(kBuilds));
	return QString::fromLatin1(kBuilds[base::RandomIndex(count)]);
}

[[nodiscard]] inline Language RandomLanguage(const QString &phone = QString()) {
	if (!phone.isEmpty()) {
		const auto geo = Core::GgtgGeo::MatchForPhone(phone);
		return Language{ geo.cloudLang, geo.systemLang };
	}
	return Language{ u"en"_q, u"en-US"_q };
}

[[nodiscard]] inline Profile RandomProfile(const QString &phone = QString()) {
	struct Entry {
		QString brand;
		QString model;
		int sdk = 0;
	};
	auto entries = std::vector<Entry>();
	auto brands = QStringList();
	for (const auto line : kDevices) {
		const auto parts = QString::fromLatin1(line).split(QChar('|'));
		entries.push_back({ parts[0], parts[1], parts[2].toInt() });
		if (!brands.contains(parts[0])) {
			brands.push_back(parts[0]);
		}
	}
	const auto brand = brands[base::RandomIndex(int(brands.size()))];
	auto sdks = std::vector<int>();
	for (const auto &entry : entries) {
		if (entry.brand == brand
			&& std::find(sdks.begin(), sdks.end(), entry.sdk) == sdks.end()) {
			sdks.push_back(entry.sdk);
		}
	}
	const auto sdk = sdks[base::RandomIndex(int(sdks.size()))];
	auto models = std::vector<QString>();
	for (const auto &entry : entries) {
		if (entry.brand == brand && entry.sdk == sdk) {
			models.push_back(entry.model);
		}
	}
	const auto model = models[base::RandomIndex(int(models.size()))];
	const auto language = RandomLanguage(phone);
	return {
		.deviceModel = brand + u" "_q + model,
		.systemVersion = QString::fromLatin1("SDK ") + QString::number(sdk),
		.appVersion = RandomBuild(),
		.cloudLang = language.cloud,
		.systemLang = language.system,
	};
}

} // namespace Ggtg::AndroidProfiles

namespace Ggtg::DesktopProfiles {

constexpr auto kAppId = 2040;
constexpr auto kAppHash = "b18441a1ff607e10a989891a5462e627";
constexpr auto kLangPack = "tdesktop";
constexpr auto kPerfCat = 2;

inline constexpr const char *kDesktopVersions[] = {
	"7.3.1 x64",
	"7.3.0 x64",
	"7.2.9 x64",
	"7.2.8 x64",
	"7.1.5 x64",
	"7.1.0 x64",
	"7.0.8 x64",
	"6.9.4 x64",
	"6.9.0 x64",
	"6.8.2 x64",
};

inline constexpr const char *kDesktopSystems[] = {
	"Windows 11 x64",
	"Windows 10 x64",
};

struct WindowsModel {
	const char *label;
	const char *code;
};

inline constexpr WindowsModel kCuratedWindowsDevices[] = {
	{ "Стандартный ПК (PC 64bit)", "PC 64bit" },
	{ "DESKTOP-UECSK40", "DESKTOP-UECSK40" },
	{ "LAPTOP-V13BLVG", "LAPTOP-V13BLVG" },
	{ "LAPTOP-A23H4TH", "LAPTOP-A23H4TH" },
	{ "Lenovo Legion 5 (15ACH6)", "15ACH6" },
	{ "Lenovo Legion 7 (16ACH6H)", "16ACH6H" },
	{ "HP Pavilion Gaming (15-ec1073dx)", "15-ec1073dx" },
	{ "ASUS TUF Gaming A15 (FA506QM)", "FA506QM-HN124W" },
	{ "ASUS ROG Strix G17 (G713QM)", "G713QM-K4215TS" },
	{ "ASUS ROG Zephyrus G14 (GA401QC)", "GA401QC-HZ094TS" },
	{ "ASUS ZenBook Pro Duo (UX582LR)", "UX582LR-H701TS" },
};

[[nodiscard]] inline QString WindowsLabel(const QString &code) {
	for (const auto &w : kCuratedWindowsDevices) {
		if (QString::fromUtf8(w.code) == code) {
			return QString::fromUtf8(w.label);
		}
	}
	return code;
}

inline constexpr const char *kMacDevices[] = {
	"MacBook Pro",
	"MacBook Air",
	"iMac",
	"Mac mini",
	"Mac Studio",
	"MacBook Pro 16\"",
	"MacBook Pro 14\"",
	"MacBook Air 13\"",
	"MacBook Air 15\"",
	"Mac Pro",
};

inline constexpr const char *kMacSystems[] = {
	"macOS 15.1",
	"macOS 15.0.1",
	"macOS 15.0",
	"macOS 14.7.1",
	"macOS 14.7",
	"macOS 14.6.1",
	"macOS 14.5",
	"macOS 14.4.1",
	"macOS 13.6.9",
};

inline constexpr const char *kMacVersions[] = {
	"7.3.1",
	"7.3.0",
	"7.2.9",
	"7.2.8",
	"7.1.5",
	"7.1.0",
	"7.0.8",
	"6.9.4",
};

struct Device {
	QString model;
	QString system;
};

struct Profile {
	QString deviceModel;
	QString systemVersion;
	QString appVersion;
	QString cloudLang;
	QString systemLang;
};

[[nodiscard]] inline QString RandomDesktopVersion() {
	constexpr auto count = int(std::size(kDesktopVersions));
	return QString::fromLatin1(kDesktopVersions[base::RandomIndex(count)]);
}

[[nodiscard]] inline QString RandomDesktopSystem() {
	constexpr auto count = int(std::size(kDesktopSystems));
	return QString::fromLatin1(kDesktopSystems[base::RandomIndex(count)]);
}

[[nodiscard]] inline QString RandomMacVersion() {
	constexpr auto count = int(std::size(kMacVersions));
	return QString::fromLatin1(kMacVersions[base::RandomIndex(count)]);
}

[[nodiscard]] inline QString RandomMacSystem() {
	constexpr auto count = int(std::size(kMacSystems));
	return QString::fromLatin1(kMacSystems[base::RandomIndex(count)]);
}

[[nodiscard]] inline QString RandomMacDevice() {
	constexpr auto count = int(std::size(kMacDevices));
	return QString::fromUtf8(kMacDevices[base::RandomIndex(count)]);
}

// A random Windows laptop (real model code + "Windows 11 x64") of the Desktop profile.
[[nodiscard]] inline Device RandomWindowsDevice() {
	constexpr auto devices = int(std::size(kWindowsDevices));
	return {
		QString::fromLatin1(kWindowsDevices[base::RandomIndex(devices)]),
		RandomDesktopSystem(),
	};
}

[[nodiscard]] inline Profile RandomWindowsProfile(const QString &phone = QString()) {
	constexpr auto devices = int(std::size(kWindowsDevices));
	const auto model = (devices > 0)
		? QString::fromLatin1(kWindowsDevices[base::RandomIndex(devices)])
		: QString::fromUtf8("PC 64bit");
	const auto lang = AndroidProfiles::RandomLanguage(phone);
	return {
		.deviceModel = model,
		.systemVersion = RandomDesktopSystem(),
		.appVersion = RandomDesktopVersion(),
		.cloudLang = lang.cloud,
		.systemLang = lang.system,
	};
}

[[nodiscard]] inline Profile RandomMacProfile(const QString &phone = QString()) {
	const auto lang = AndroidProfiles::RandomLanguage(phone);
	return {
		.deviceModel = RandomMacDevice(),
		.systemVersion = RandomMacSystem(),
		.appVersion = RandomMacVersion(),
		.cloudLang = lang.cloud,
		.systemLang = lang.system,
	};
}

enum class PlatformKind {
	Any,
	Windows,
	Mac,
};

[[nodiscard]] inline Profile RandomProfile(PlatformKind kind, const QString &phone = QString()) {
	if (kind == PlatformKind::Mac) {
		return RandomMacProfile(phone);
	} else if (kind == PlatformKind::Windows) {
		return RandomWindowsProfile(phone);
	}
	// Roughly 60% Windows, 40% Mac
	return (base::RandomIndex(100) < 40)
		? RandomMacProfile(phone)
		: RandomWindowsProfile(phone);
}

[[nodiscard]] inline Profile RandomProfile(const QString &phone = QString()) {
	return RandomProfile(PlatformKind::Any, phone);
}

} // namespace Ggtg::DesktopProfiles

