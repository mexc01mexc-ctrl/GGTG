// ggtg: country, timezone and language resolution by phone prefix and proxy
#pragma once

#include <QtCore/QString>
#include <QtCore/QRegularExpression>

namespace Core::GgtgGeo {

struct GeoMatch {
	QString country;    // "US", "RU", "DE", etc.
	int tzOffset = 0;   // seconds from UTC (e.g. -18000 for US EST, 10800 for MSK)
	QString systemLang; // "en-US", "ru-RU", "de-DE", etc.
	QString cloudLang;  // "en", "ru", "de", etc.
};

inline GeoMatch MatchForPhone(const QString &rawPhone) {
	auto digits = rawPhone;
	digits.remove(QRegularExpression(u"[^0-9]"_q));

	// Ordered by prefix length descending
	static const struct {
		const char *prefix;
		const char *country;
		int tz;
		const char *sys;
		const char *cloud;
	} kPhoneTable[] = {
		// 4-digit prefixes
		{ "1242", "BS", -18000, "en-US", "en" },
		{ "1246", "BB", -14400, "en-US", "en" },
		{ "1264", "AI", -14400, "en-US", "en" },
		{ "1268", "AG", -14400, "en-US", "en" },
		{ "1345", "KY", -18000, "en-US", "en" },
		{ "1441", "BM", -14400, "en-US", "en" },
		{ "1473", "GD", -14400, "en-US", "en" },
		{ "1664", "MS", -14400, "en-US", "en" },
		{ "1721", "SX", -14400, "en-US", "en" },
		{ "1758", "LC", -14400, "en-US", "en" },
		{ "1767", "DM", -14400, "en-US", "en" },
		{ "1784", "VC", -14400, "en-US", "en" },
		{ "1809", "DO", -14400, "es-ES", "es" },
		{ "1829", "DO", -14400, "es-ES", "es" },
		{ "1849", "DO", -14400, "es-ES", "es" },
		{ "1868", "TT", -14400, "en-US", "en" },
		{ "1869", "KN", -14400, "en-US", "en" },
		{ "1876", "JM", -18000, "en-US", "en" },

		// 3-digit prefixes
		{ "998", "UZ", 18000, "ru-RU", "ru" },
		{ "996", "KG", 21600, "ru-RU", "ru" },
		{ "995", "GE", 14400, "ru-RU", "ru" },
		{ "994", "AZ", 14400, "ru-RU", "ru" },
		{ "993", "TM", 18000, "ru-RU", "ru" },
		{ "992", "TJ", 18000, "ru-RU", "ru" },
		{ "977", "NP", 20700, "en-US", "en" },
		{ "976", "MN", 28800, "ru-RU", "ru" },
		{ "975", "BT", 21600, "en-US", "en" },
		{ "974", "QA", 10800, "ar-SA", "ar" },
		{ "973", "BH", 10800, "ar-SA", "ar" },
		{ "972", "IL", 7200, "he-IL", "he" },
		{ "971", "AE", 14400, "ar-SA", "ar" },
		{ "968", "OM", 14400, "ar-SA", "ar" },
		{ "967", "YE", 10800, "ar-SA", "ar" },
		{ "966", "SA", 10800, "ar-SA", "ar" },
		{ "965", "KW", 10800, "ar-SA", "ar" },
		{ "964", "IQ", 10800, "ar-SA", "ar" },
		{ "963", "SY", 10800, "ar-SA", "ar" },
		{ "962", "JO", 10800, "ar-SA", "ar" },
		{ "961", "LB", 7200, "ar-SA", "ar" },
		{ "886", "TW", 28800, "zh-TW", "zh-hant" },
		{ "880", "BD", 21600, "en-US", "en" },
		{ "856", "LA", 25200, "en-US", "en" },
		{ "855", "KH", 25200, "en-US", "en" },
		{ "853", "MO", 28800, "zh-HK", "zh-hant" },
		{ "852", "HK", 28800, "zh-HK", "zh-hant" },
		{ "598", "UY", -10800, "es-ES", "es" },
		{ "595", "PY", -14400, "es-ES", "es" },
		{ "593", "EC", -18000, "es-ES", "es" },
		{ "591", "BO", -14400, "es-ES", "es" },
		{ "507", "PA", -18000, "es-ES", "es" },
		{ "506", "CR", -21600, "es-ES", "es" },
		{ "505", "NI", -21600, "es-ES", "es" },
		{ "504", "HN", -21600, "es-ES", "es" },
		{ "503", "SV", -21600, "es-ES", "es" },
		{ "502", "GT", -21600, "es-ES", "es" },
		{ "423", "LI", 3600, "de-DE", "de" },
		{ "421", "SK", 3600, "sk-SK", "sk" },
		{ "420", "CZ", 3600, "cs-CZ", "cs" },
		{ "389", "MK", 3600, "mk-MK", "mk" },
		{ "387", "BA", 3600, "bs-BA", "bs" },
		{ "386", "SI", 3600, "sl-SI", "sl" },
		{ "385", "HR", 3600, "hr-HR", "hr" },
		{ "382", "ME", 3600, "sr-ME", "sr" },
		{ "381", "RS", 3600, "sr-RS", "sr" },
		{ "380", "UA", 7200, "uk-UA", "uk" },
		{ "378", "SM", 3600, "it-IT", "it" },
		{ "377", "MC", 3600, "fr-FR", "fr" },
		{ "376", "AD", 3600, "ca-ES", "es" },
		{ "375", "BY", 10800, "ru-RU", "ru" },
		{ "374", "AM", 14400, "ru-RU", "ru" },
		{ "373", "MD", 7200, "ru-RU", "ru" },
		{ "372", "EE", 7200, "et-EE", "et" },
		{ "371", "LV", 7200, "lv-LV", "lv" },
		{ "370", "LT", 7200, "lt-LT", "lt" },
		{ "359", "BG", 7200, "bg-BG", "bg" },
		{ "358", "FI", 7200, "fi-FI", "fi" },
		{ "357", "CY", 7200, "el-CY", "el" },
		{ "356", "MT", 3600, "en-GB", "en" },
		{ "355", "AL", 3600, "sq-AL", "sq" },
		{ "354", "IS", 0, "is-IS", "is" },
		{ "353", "IE", 0, "en-GB", "en" },
		{ "352", "LU", 3600, "fr-FR", "fr" },
		{ "351", "PT", 0, "pt-PT", "pt" },
		{ "256", "UG", 10800, "en-GB", "en" },
		{ "255", "TZ", 10800, "en-GB", "en" },
		{ "254", "KE", 10800, "en-GB", "en" },
		{ "234", "NG", 3600, "en-US", "en" },
		{ "233", "GH", 0, "en-GB", "en" },
		{ "218", "LY", 7200, "ar-SA", "ar" },
		{ "216", "TN", 3600, "ar-SA", "ar" },
		{ "213", "DZ", 3600, "ar-SA", "ar" },
		{ "212", "MA", 3600, "ar-SA", "ar" },

		// 2-digit prefixes
		{ "98",  "IR", 12600, "fa-IR", "fa" },
		{ "95",  "MM", 23400, "en-US", "en" },
		{ "94",  "LK", 19800, "en-GB", "en" },
		{ "93",  "AF", 16200, "fa-AF", "fa" },
		{ "92",  "PK", 18000, "en-US", "en" },
		{ "91",  "IN", 19800, "en-IN", "en" },
		{ "90",  "TR", 10800, "tr-TR", "tr" },
		{ "86",  "CN", 28800, "zh-CN", "zh-hans" },
		{ "84",  "VN", 25200, "vi-VN", "vi" },
		{ "82",  "KR", 32400, "ko-KR", "ko" },
		{ "81",  "JP", 32400, "ja-JP", "ja" },
		{ "77",  "KZ", 18000, "ru-RU", "ru" },
		{ "76",  "KZ", 18000, "ru-RU", "ru" },
		{ "66",  "TH", 25200, "th-TH", "th" },
		{ "65",  "SG", 28800, "en-GB", "en" },
		{ "64",  "NZ", 43200, "en-GB", "en" },
		{ "63",  "PH", 28800, "en-US", "en" },
		{ "62",  "ID", 25200, "id-ID", "id" },
		{ "61",  "AU", 36000, "en-GB", "en" },
		{ "60",  "MY", 28800, "en-GB", "en" },
		{ "58",  "VE", -14400, "es-ES", "es" },
		{ "57",  "CO", -18000, "es-ES", "es" },
		{ "56",  "CL", -10800, "es-ES", "es" },
		{ "55",  "BR", -10800, "pt-BR", "pt-br" },
		{ "54",  "AR", -10800, "es-ES", "es" },
		{ "53",  "CU", -18000, "es-ES", "es" },
		{ "52",  "MX", -21600, "es-ES", "es" },
		{ "51",  "PE", -18000, "es-ES", "es" },
		{ "49",  "DE", 3600, "de-DE", "de" },
		{ "48",  "PL", 3600, "pl-PL", "pl" },
		{ "47",  "NO", 3600, "nb-NO", "nb" },
		{ "46",  "SE", 3600, "sv-SE", "sv" },
		{ "45",  "DK", 3600, "da-DK", "da" },
		{ "44",  "GB", 0, "en-GB", "en" },
		{ "43",  "AT", 3600, "de-DE", "de" },
		{ "41",  "CH", 3600, "de-DE", "de" },
		{ "40",  "RO", 7200, "ro-RO", "ro" },
		{ "39",  "IT", 3600, "it-IT", "it" },
		{ "36",  "HU", 3600, "hu-HU", "hu" },
		{ "34",  "ES", 3600, "es-ES", "es" },
		{ "33",  "FR", 3600, "fr-FR", "fr" },
		{ "32",  "BE", 3600, "fr-FR", "fr" },
		{ "31",  "NL", 3600, "nl-NL", "nl" },
		{ "30",  "GR", 7200, "el-GR", "el" },
		{ "27",  "ZA", 7200, "en-GB", "en" },
		{ "20",  "EG", 7200, "ar-SA", "ar" },

		// 1-digit prefixes
		{ "7",   "RU", 10800, "ru-RU", "ru" },
		{ "1",   "US", -18000, "en-US", "en" },
	};

	for (const auto &row : kPhoneTable) {
		if (digits.startsWith(QLatin1String(row.prefix))) {
			return {
				.country = QString::fromLatin1(row.country),
				.tzOffset = row.tz,
				.systemLang = QString::fromLatin1(row.sys),
				.cloudLang = QString::fromLatin1(row.cloud),
			};
		}
	}
	return { "US", -18000, "en-US", "en" };
}

inline GeoMatch MatchForCountry(const QString &country) {
	const auto c = country.toUpper().trimmed();
	static const struct {
		const char *country;
		int tz;
		const char *sys;
		const char *cloud;
	} kCountryTable[] = {
		{ "RU", 10800, "ru-RU", "ru" },
		{ "BY", 10800, "ru-RU", "ru" },
		{ "KZ", 18000, "ru-RU", "ru" },
		{ "KG", 21600, "ru-RU", "ru" },
		{ "UA", 7200, "uk-UA", "uk" },
		{ "UZ", 18000, "ru-RU", "ru" },
		{ "TJ", 18000, "ru-RU", "ru" },
		{ "AZ", 14400, "ru-RU", "ru" },
		{ "AM", 14400, "ru-RU", "ru" },
		{ "GE", 14400, "ru-RU", "ru" },
		{ "MD", 7200, "ru-RU", "ru" },
		{ "TM", 18000, "ru-RU", "ru" },
		{ "US", -18000, "en-US", "en" },
		{ "CA", -18000, "en-US", "en" },
		{ "GB", 0, "en-GB", "en" },
		{ "IE", 0, "en-GB", "en" },
		{ "DE", 3600, "de-DE", "de" },
		{ "AT", 3600, "de-DE", "de" },
		{ "CH", 3600, "de-DE", "de" },
		{ "FR", 3600, "fr-FR", "fr" },
		{ "BE", 3600, "fr-FR", "fr" },
		{ "ES", 3600, "es-ES", "es" },
		{ "IT", 3600, "it-IT", "it" },
		{ "PT", 0, "pt-PT", "pt" },
		{ "BR", -10800, "pt-BR", "pt-br" },
		{ "PL", 3600, "pl-PL", "pl" },
		{ "NL", 3600, "nl-NL", "nl" },
		{ "CZ", 3600, "cs-CZ", "cs" },
		{ "SK", 3600, "sk-SK", "sk" },
		{ "HU", 3600, "hu-HU", "hu" },
		{ "RO", 7200, "ro-RO", "ro" },
		{ "BG", 7200, "bg-BG", "bg" },
		{ "GR", 7200, "el-GR", "el" },
		{ "SE", 3600, "sv-SE", "sv" },
		{ "NO", 3600, "nb-NO", "nb" },
		{ "DK", 3600, "da-DK", "da" },
		{ "FI", 7200, "fi-FI", "fi" },
		{ "TR", 10800, "tr-TR", "tr" },
		{ "ID", 25200, "id-ID", "id" },
		{ "VN", 25200, "vi-VN", "vi" },
		{ "IN", 19800, "en-IN", "en" },
		{ "NG", 3600, "en-US", "en" },
		{ "PH", 28800, "en-US", "en" },
		{ "SA", 10800, "ar-SA", "ar" },
		{ "AE", 14400, "ar-SA", "ar" },
		{ "EG", 7200, "ar-SA", "ar" },
		{ "IQ", 10800, "ar-SA", "ar" },
		{ "KR", 32400, "ko-KR", "ko" },
		{ "JP", 32400, "ja-JP", "ja" },
		{ "CN", 28800, "zh-CN", "zh-hans" },
		{ "TW", 28800, "zh-TW", "zh-hant" },
		{ "HK", 28800, "zh-HK", "zh-hant" },
		{ "TH", 25200, "th-TH", "th" },
		{ "MY", 28800, "en-GB", "en" },
		{ "SG", 28800, "en-GB", "en" },
		{ "AU", 36000, "en-GB", "en" },
		{ "NZ", 43200, "en-GB", "en" },
		{ "MX", -21600, "es-ES", "es" },
		{ "AR", -10800, "es-ES", "es" },
		{ "CO", -18000, "es-ES", "es" },
		{ "CL", -10800, "es-ES", "es" },
		{ "PE", -18000, "es-ES", "es" },
		{ "IL", 7200, "he-IL", "he" },
	};
	for (const auto &row : kCountryTable) {
		if (c == QLatin1String(row.country)) {
			return {
				.country = QString::fromLatin1(row.country),
				.tzOffset = row.tz,
				.systemLang = QString::fromLatin1(row.sys),
				.cloudLang = QString::fromLatin1(row.cloud),
			};
		}
	}
	return { c.isEmpty() ? "US" : c, -18000, "en-US", "en" };
}

} // namespace Core::GgtgGeo

