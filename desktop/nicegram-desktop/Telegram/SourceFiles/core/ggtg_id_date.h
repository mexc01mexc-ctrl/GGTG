#pragma once

#include <QtCore/QString>
#include <QtCore/QDateTime>
#include <vector>

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

inline QString EstimateIdDate(uint64 id) {
	if (id == 0) {
		return QString();
	}
	const auto &anchors = IdDateAnchors();
	int64 t = 0;
	if (id <= anchors.front().id) {
		t = anchors.front().timestamp;
	} else if (id >= anchors.back().id) {
		t = anchors.back().timestamp;
	} else {
		for (size_t i = 0; i + 1 < anchors.size(); ++i) {
			const auto &p1 = anchors[i];
			const auto &p2 = anchors[i + 1];
			if (id >= p1.id && id <= p2.id) {
				const auto ratio = double(id - p1.id) / double(p2.id - p1.id);
				t = p1.timestamp + int64(ratio * double(p2.timestamp - p1.timestamp));
				break;
			}
		}
	}
	const auto dt = QDateTime::fromSecsSinceEpoch(t);
	static const QString months[] = {
		u"январь"_q, u"февраль"_q, u"март"_q, u"апрель"_q,
		u"май"_q, u"июнь"_q, u"июль"_q, u"август"_q,
		u"сентябрь"_q, u"октябрь"_q, u"ноябрь"_q, u"декабрь"_q
	};
	const auto m = dt.date().month();
	if (m >= 1 && m <= 12) {
		return u"~%1 %2"_q.arg(months[m - 1]).arg(dt.date().year());
	}
	return QString();
}

} // namespace Ggtg

