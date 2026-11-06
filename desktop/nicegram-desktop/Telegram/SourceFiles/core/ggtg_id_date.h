#pragma once

#include <QtCore/QRegularExpression>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileDevice>
#include <QtCore/QSaveFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QString>
#include <QtCore/QDateTime>
#include <algorithm>
#include <cmath>
#include <vector>

#include "core/ggtg_paths.h"

namespace Ggtg {

struct IdDateAnchor {
	uint64 id = 0;
	int64 timestamp = 0; // unix seconds
};

inline const std::vector<IdDateAnchor> &IdDateAnchors() {
	static const auto anchors = std::vector<IdDateAnchor>{
		{ 1ULL, 1376438400LL },          // 2013-08-14 (Telegram launch)
		{ 2768409ULL, 1383264000LL },    // 2013-11-01
		{ 7679610ULL, 1388448000LL },    // 2013-12-31
		{ 15835244ULL, 1392940000LL },   // 2014-02-21
		{ 44634663ULL, 1399334000LL },   // 2014-05-06
		{ 63263518ULL, 1414454000LL },   // 2014-10-28
		{ 101260938ULL, 1425600000LL },  // 2015-03-06
		{ 157242073ULL, 1446768000LL },  // 2015-11-06
		{ 222021233ULL, 1465344000LL },  // 2016-06-08
		{ 297621225ULL, 1481846000LL },  // 2016-12-16
		{ 400169472ULL, 1501459000LL },  // 2017-07-31
		{ 500000000ULL, 1515000000LL },  // 2018-01-03
		{ 700000000ULL, 1545000000LL },  // 2018-12-17
		{ 805158066ULL, 1563208000LL },  // 2019-07-15
		{ 1000000000ULL, 1577836800LL }, // 2020-01-01
		{ 1300000000ULL, 1600000000LL }, // 2020-09-13
		{ 1600000000ULL, 1615000000LL }, // 2021-03-06
		{ 1974255900ULL, 1634000000LL }, // 2021-10-12
		{ 2100000000ULL, 1640995200LL }, // 2022-01-01
		{ 5000000000ULL, 1643673600LL }, // 2022-02-01 (64-bit jump)
		{ 5795034000ULL, 1662076800LL }, // 2022-09-02
		{ 6227468000ULL, 1679270400LL }, // 2023-03-20
		{ 6600000000ULL, 1695000000LL }, // 2023-09-18
		{ 7000000000ULL, 1712000000LL }, // 2024-04-01
		{ 7583599300ULL, 1739664000LL }, // 2025-02-16
		{ 7947063900ULL, 1754092800LL }, // 2025-08-02
		{ 8235679900ULL, 1758758400LL }, // 2025-09-25
		{ 8600000000ULL, 1775000000LL }, // 2026-04-01
	};
	return anchors;
}

// ---- the estimate of when an account was registered, by its id ----
//
// Ids are given out one after another, so the id says roughly when the account appeared. The anchors
// above are known (id, date) pairs; between them the date follows a smooth monotone curve (a plain
// straight line gives a month or more of error where the rate of new accounts changed). Past the last
// anchor the curve goes on at the recent rate, up to now. Exact points make it better: every account
// registered by GGTG adds its own (id, time) to tdata/ggtg_db/id_calibration.json. A person's own
// facts bound the answer: the account cannot be younger than its first profile photo.

struct IdEstimate {
	int64 time = 0; // unix seconds, 0 when the id is unknown
	int uncertaintyDays = 0; // how far the real date can be, about
	bool extrapolated = false; // the id is newer than every known point
};

namespace details {

struct IdPoint {
	double id = 0.;
	double time = 0.;
	bool exact = false;
};

[[nodiscard]] inline QString IdCalibrationPath() {
	const auto dir = Core::GgtgPaths::DbDir();
	return dir.isEmpty() ? QString() : (dir + u"id_calibration.json"_q);
}

[[nodiscard]] inline std::vector<std::pair<uint64, int64>> &IdCalibrationList() {
	static auto result = [] {
		auto list = std::vector<std::pair<uint64, int64>>();
		const auto path = IdCalibrationPath();
		if (!path.isEmpty()) {
			auto file = QFile(path);
			if (file.open(QIODevice::ReadOnly)) {
				for (const auto &value : QJsonDocument::fromJson(file.readAll()).array()) {
					const auto object = value.toObject();
					const auto id = uint64(object.value("id").toDouble());
					const auto time = int64(object.value("t").toDouble());
					if (id > 0 && time > 0) {
						list.emplace_back(id, time);
					}
				}
			}
		}
		return list;
	}();
	return result;
}

// The marked dates are only as exact as the month Telegram shows (the middle of the month is taken), and ids grow
// with time, so two marks can disagree (a bigger id with an earlier month). Instead of throwing away the later one,
// neighbours that contradict each other are AVERAGED into one point (isotonic regression, "pool adjacent
// violators"): the curve stays monotone and every mark counts.
[[nodiscard]] inline std::vector<std::pair<double, double>> PooledCalibration() {
	auto points = std::vector<std::pair<double, double>>();
	for (const auto &[id, time] : IdCalibrationList()) {
		points.emplace_back(double(id), double(time));
	}
	std::sort(points.begin(), points.end());
	struct Block {
		double sumId = 0.;
		double sumTime = 0.;
		int count = 0;
		[[nodiscard]] double time() const { return sumTime / count; }
	};
	auto blocks = std::vector<Block>();
	for (const auto &[id, time] : points) {
		blocks.push_back({ id, time, 1 });
		while (blocks.size() >= 2
			&& blocks[blocks.size() - 2].time() > blocks.back().time()) {
			auto last = blocks.back();
			blocks.pop_back();
			blocks.back().sumId += last.sumId;
			blocks.back().sumTime += last.sumTime;
			blocks.back().count += last.count;
		}
	}
	auto result = std::vector<std::pair<double, double>>();
	for (const auto &block : blocks) {
		result.emplace_back(block.sumId / block.count, block.sumTime / block.count);
	}
	return result;
}

// Anchors and exact points, sorted by id and kept monotone: an exact point wins over an anchor
// that contradicts it.
[[nodiscard]] inline std::vector<IdPoint> IdPoints() {
	auto all = std::vector<IdPoint>();
	for (const auto &anchor : IdDateAnchors()) {
		all.push_back({ double(anchor.id), double(anchor.timestamp), false });
	}
	for (const auto &[id, time] : PooledCalibration()) {
		all.push_back({ id, time, true });
	}
	std::sort(all.begin(), all.end(), [](const IdPoint &a, const IdPoint &b) {
		return (a.id != b.id) ? (a.id < b.id) : (a.exact > b.exact);
	});
	auto result = std::vector<IdPoint>();
	for (const auto &point : all) {
		if (!result.empty() && point.id == result.back().id) {
			continue;
		}
		if (!result.empty() && point.time < result.back().time) {
			if (!point.exact) {
				continue;
			}
			while (!result.empty() && !result.back().exact && result.back().time > point.time) {
				result.pop_back();
			}
			if (!result.empty() && result.back().time > point.time) {
				continue;
			}
		}
		result.push_back(point);
	}
	return result;
}

// Fritsch-Carlson slopes of a monotone cubic through the points.
[[nodiscard]] inline std::vector<double> IdSlopes(const std::vector<IdPoint> &p) {
	const auto n = p.size();
	auto delta = std::vector<double>(n - 1);
	for (auto i = size_t(0); i + 1 < n; ++i) {
		delta[i] = (p[i + 1].time - p[i].time) / (p[i + 1].id - p[i].id);
	}
	auto m = std::vector<double>(n);
	m[0] = delta[0];
	m[n - 1] = delta[n - 2];
	for (auto i = size_t(1); i + 1 < n; ++i) {
		if (delta[i - 1] * delta[i] <= 0.) {
			m[i] = 0.;
		} else {
			const auto h0 = p[i].id - p[i - 1].id;
			const auto h1 = p[i + 1].id - p[i].id;
			const auto w1 = 2. * h1 + h0;
			const auto w2 = h1 + 2. * h0;
			m[i] = (w1 + w2) / (w1 / delta[i - 1] + w2 / delta[i]);
		}
	}
	return m;
}

} // namespace details

inline void AddIdCalibration(uint64 id, int64 time) {
	if (id == 0 || time <= 0) {
		return;
	}
	auto &list = details::IdCalibrationList();
	for (const auto &[known, when] : list) {
		if (known == id) {
			return;
		}
	}
	list.emplace_back(id, time);
	const auto path = details::IdCalibrationPath();
	if (path.isEmpty()) {
		return;
	}
	QDir().mkpath(QFileInfo(path).absolutePath());
	auto file = QSaveFile(path);
	if (!file.open(QIODevice::WriteOnly)) {
		return;
	}
	auto array = QJsonArray();
	for (const auto &[known, when] : list) {
		array.push_back(QJsonObject{ { "id", double(known) }, { "t", double(when) } });
	}
	file.write(QJsonDocument(array).toJson(QJsonDocument::Compact));
	file.commit();
}

// Month and year as a person types them or Telegram writes them: "03.2025", "3 2025", "март 2025", "Mar 2025".
// Returns unix seconds of the middle of that month, 0 if unreadable.
[[nodiscard]] inline int64 ParseRegistrationMonth(const QString &text) {
	auto month = 0;
	auto year = 0;
	static const auto names = std::vector<std::pair<QString, int>>{
		{ u"янв"_q, 1 }, { u"фев"_q, 2 }, { u"мар"_q, 3 }, { u"апр"_q, 4 },
		{ u"мая"_q, 5 }, { u"май"_q, 5 }, { u"июн"_q, 6 }, { u"июл"_q, 7 },
		{ u"авг"_q, 8 }, { u"сен"_q, 9 }, { u"окт"_q, 10 }, { u"ноя"_q, 11 },
		{ u"дек"_q, 12 },
		{ u"jan"_q, 1 }, { u"feb"_q, 2 }, { u"mar"_q, 3 }, { u"apr"_q, 4 },
		{ u"may"_q, 5 }, { u"jun"_q, 6 }, { u"jul"_q, 7 }, { u"aug"_q, 8 },
		{ u"sep"_q, 9 }, { u"oct"_q, 10 }, { u"nov"_q, 11 }, { u"dec"_q, 12 },
	};
	for (const auto &part : text.toLower().split(
			QRegularExpression(u"[^0-9a-zа-яё]+"_q),
			Qt::SkipEmptyParts)) {
		auto ok = false;
		const auto value = part.toInt(&ok);
		if (ok) {
			if (part.size() == 4) {
				year = value;
			} else if (value >= 1 && value <= 12) {
				month = value;
			}
			continue;
		}
		for (const auto &[name, number] : names) {
			if (part.startsWith(name)) {
				month = number;
				break;
			}
		}
	}
	if (year < 2013 || year > 2100 || month < 1) {
		return 0;
	}
	return QDateTime(QDate(year, month, 15), QTime(12, 0), Qt::UTC).toSecsSinceEpoch();
}

// `upperBound`: the account is not younger than this (unix seconds, 0 - unknown), e.g. the date of its
// first profile photo.
[[nodiscard]] inline IdEstimate EstimateIdInfo(uint64 id, int64 upperBound = 0) {
	if (id == 0) {
		return {};
	}
	const auto points = details::IdPoints();
	if (points.size() < 2) {
		return {};
	}
	const auto now = QDateTime::currentSecsSinceEpoch();
	const auto x = double(id);
	auto result = IdEstimate();
	if (x <= points.front().id) {
		result.time = int64(points.front().time);
		result.uncertaintyDays = 30;
	} else if (x >= points.back().id) {
		// After the last known point: at the recent rate (the points of the last half year or more).
		const auto &last = points.back();
		auto from = points.size() - 2;
		while (from > 0 && last.time - points[from].time < 180. * 86400.) {
			--from;
		}
		const auto slope = (last.time - points[from].time) / (last.id - points[from].id);
		const auto time = last.time + (x - last.id) * slope;
		result.time = int64(std::min(time, double(now)));
		result.extrapolated = true;
		const auto days = (result.time - int64(last.time)) / 86400;
		result.uncertaintyDays = int(30 + days * 0.15);
	} else {
		const auto m = details::IdSlopes(points);
		auto i = size_t(0);
		while (i + 2 < points.size() && x > points[i + 1].id) {
			++i;
		}
		const auto &a = points[i];
		const auto &b = points[i + 1];
		const auto h = b.id - a.id;
		const auto t = (x - a.id) / h;
		const auto t2 = t * t;
		const auto t3 = t2 * t;
		const auto time = (2 * t3 - 3 * t2 + 1) * a.time
			+ (t3 - 2 * t2 + t) * h * m[i]
			+ (-2 * t3 + 3 * t2) * b.time
			+ (t3 - t2) * h * m[i + 1];
		result.time = int64(time);
		const auto gapDays = int((b.time - a.time) / 86400.);
		// Between two exact points the answer is tight, between old anchors it is a few weeks.
		result.uncertaintyDays = (a.exact && b.exact)
			? std::max(3, gapDays / 20)
			: std::clamp(gapDays / 4, 14, 120);
	}
	if (upperBound > 0 && result.time > upperBound) {
		result.time = upperBound;
		result.uncertaintyDays = std::min(result.uncertaintyDays, 30);
	}
	return result;
}

// ggtg: the same estimate as unix seconds (0 when the id is unknown); the age of an account is counted from it.
[[nodiscard]] inline int64 EstimateIdTime(uint64 id, int64 upperBound = 0) {
	return EstimateIdInfo(id, upperBound).time;
}

// "около 2 лет 3 мес." for an account created at `created`.
inline QString FormatAccountAge(int64 created, int64 now) {
	if (created <= 0 || now <= created) {
		return QString();
	}
	const auto months = int((now - created) / (30LL * 24 * 3600));
	const auto years = months / 12;
	const auto rest = months % 12;
	const auto yearWord = [](int n) {
		const auto m100 = n % 100;
		const auto m10 = n % 10;
		return (m100 >= 11 && m100 <= 14) ? u"лет"_q
			: (m10 == 1) ? u"год"_q
			: (m10 >= 2 && m10 <= 4) ? u"года"_q
			: u"лет"_q;
	};
	if (years <= 0) {
		return rest <= 1 ? u"меньше месяца"_q : u"около %1 мес."_q.arg(rest);
	}
	return u"около %1 %2"_q.arg(years).arg(yearWord(years))
		+ (rest > 0 ? u" %1 мес."_q.arg(rest) : QString());
}

// "~сентябрь 2026 (±1 мес.)": the month of the estimate and how far it can be off.
inline QString EstimateIdDate(uint64 id, int64 upperBound = 0) {
	const auto info = EstimateIdInfo(id, upperBound);
	if (info.time <= 0) {
		return QString();
	}
	const auto dt = QDateTime::fromSecsSinceEpoch(info.time);
	static const QString months[] = {
		u"январь"_q, u"февраль"_q, u"март"_q, u"апрель"_q,
		u"май"_q, u"июнь"_q, u"июль"_q, u"август"_q,
		u"сентябрь"_q, u"октябрь"_q, u"ноябрь"_q, u"декабрь"_q
	};
	const auto m = dt.date().month();
	if (m < 1 || m > 12) {
		return QString();
	}
	auto result = u"~%1 %2"_q.arg(months[m - 1]).arg(dt.date().year());
	if (info.uncertaintyDays >= 20) {
		const auto months = std::max(1, int(std::lround(info.uncertaintyDays / 30.)));
		result += u" (±%1 мес.)"_q.arg(months);
	}
	return result;
}

} // namespace Ggtg

