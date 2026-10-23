#pragma once

// ggtg: authentic Telegram iOS (iPhone) client profile generator.
// Matches official Telegram iOS sessions with Apple device models, iOS versions,
// and Telegram iOS build numbers.
// api_id = 10840, lang_pack = "ios".

#include <QtCore/QString>
#include "core/ggtg_geo.h"
#include <base/random.h>

namespace Ggtg::IosProfiles {

constexpr auto kAppId = 10840;
constexpr auto kAppHash = "b7137f4139934259b6cb5039360c75c8";
constexpr auto kLangPack = "ios";

inline constexpr const char *kIosDevices[] = {
	"iPhone 18 Pro Max",
	"iPhone 18 Pro",
	"iPhone 18 Plus",
	"iPhone 18",
	"iPhone 17 Pro Max",
	"iPhone 17 Pro",
	"iPhone 17 Air",
	"iPhone 17",
	"iPhone 16 Pro Max",
	"iPhone 16 Pro",
	"iPhone 16 Plus",
	"iPhone 16",
	"iPhone 15 Pro Max",
	"iPhone 15 Pro",
	"iPhone 15 Plus",
	"iPhone 15",
	"iPhone 14 Pro Max",
	"iPhone 14 Pro",
	"iPhone 14",
	"iPhone 13 Pro Max",
	"iPhone 13 Pro",
	"iPhone 13",
};

inline constexpr const char *kIosSystems[] = {
	"iOS 27.0.1",
	"iOS 27.0",
	"iOS 18.2",
	"iOS 18.1.1",
	"iOS 18.1",
	"iOS 18.0.1",
	"iOS 18.0",
	"iOS 17.6.1",
	"iOS 17.5.1",
};

inline constexpr const char *kIosVersions[] = {
	"11.5.0",
	"11.4.1",
	"11.3.0",
	"11.2.2",
	"11.2.1",
	"11.1.0",
	"11.0.0",
	"10.9.1",
};

struct Profile {
	QString deviceModel;
	QString systemVersion;
	QString appVersion;
	QString cloudLang;
	QString systemLang;
};

[[nodiscard]] inline QString RandomDevice() {
	constexpr auto count = int(std::size(kIosDevices));
	return QString::fromUtf8(kIosDevices[base::RandomIndex(count)]);
}

[[nodiscard]] inline QString RandomSystem() {
	constexpr auto count = int(std::size(kIosSystems));
	return QString::fromUtf8(kIosSystems[base::RandomIndex(count)]);
}

[[nodiscard]] inline QString RandomVersion() {
	constexpr auto count = int(std::size(kIosVersions));
	return QString::fromUtf8(kIosVersions[base::RandomIndex(count)]);
}

[[nodiscard]] inline Profile RandomProfile(const QString &phone = QString()) {
	auto cloudLang = u"en"_q;
	auto systemLang = u"en-US"_q;
	if (!phone.isEmpty()) {
		const auto geo = Core::GgtgGeo::MatchForPhone(phone);
		cloudLang = geo.cloudLang;
		systemLang = geo.systemLang;
	}
	return {
		.deviceModel = RandomDevice(),
		.systemVersion = RandomSystem(),
		.appVersion = RandomVersion(),
		.cloudLang = cloudLang,
		.systemLang = systemLang,
	};
}

} // namespace Ggtg::IosProfiles

