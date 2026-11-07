/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and exceptions, see https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

// ggtg: the state of the accounts table (Аккаунты > Список), apart from how
// it is drawn: which columns exist, which are shown, their order, widths,
// the pinned first column and the row density. QtCore only, so ggtg-tests/
// can check it without the app. The same split as TanStack Table (visibility,
// order, sizing, pinning are independent fields of one state). Saved as JSON
// in tdata/ggtg_db/accounts_table.json, the format is in docs/ggtg-formats.md.

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QString>

#include <algorithm>
#include <array>
#include <vector>

#ifdef GGTG_MODEL_STANDALONE
[[nodiscard]] inline QString operator""_q(const char16_t *data, size_t size) {
	return QString::fromRawData(
		reinterpret_cast<const QChar*>(data),
		qsizetype(size));
}
#else // GGTG_MODEL_STANDALONE
#include "base/basic_types.h"
#endif // GGTG_MODEL_STANDALONE

namespace Settings::TableLayout {

enum class Col {
	Number,
	Account,
	Folder,
	Proxy,
	Status,
	Sms24h,
	Registered,
	Age,
	Device,
	TwoFa,
	SpamRating,
	Sessions,
	LastCheck,
	Premium,
	Open,
	Trash,
};
inline constexpr auto kColCount = 16;

enum class Density {
	Comfortable,
	Normal,
	Compact,
};

struct ColInfo {
	Col id;
	const char16_t *key; // saved in JSON, never renamed
	const char16_t *title;
	int defaultWidth; // at scale 100%
	int minWidth;
	bool defaultVisible;
	bool alwaysOn; // «Открыть», «Корзина», «Аккаунт»: cannot be hidden
};

[[nodiscard]] inline const std::array<ColInfo, kColCount> &Infos() {
	static const auto result = std::array<ColInfo, kColCount>{ {
		{ Col::Number, u"number", u"№", 34, 30, true, false },
		{ Col::Account, u"account", u"Аккаунт", 170, 105, true, true },
		{ Col::Folder, u"folder", u"Папка", 100, 65, true, false },
		{ Col::Proxy, u"proxy", u"Прокси", 140, 90, true, false },
		{ Col::Status, u"status", u"Статус", 110, 75, true, false },
		{ Col::Sms24h, u"sms24h", u"За 24 ч", 56, 46, true, false },
		{ Col::Registered, u"registered", u"Регистрация", 96, 70, false, false },
		{ Col::Age, u"age", u"Возраст", 70, 50, false, false },
		{ Col::Device, u"device", u"Устройство", 120, 80, false, false },
		{ Col::TwoFa, u"twofa", u"2FA", 50, 40, false, false },
		{ Col::SpamRating, u"spam", u"SpamBot", 90, 60, false, false },
		{ Col::Sessions, u"sessions", u"Сессии", 64, 46, false, false },
		{ Col::LastCheck, u"lastcheck", u"Проверка", 96, 70, false, false },
		{ Col::Premium, u"premium", u"Премиум", 64, 50, false, false },
		{ Col::Open, u"open", u"Открыть", 76, 76, true, true },
		{ Col::Trash, u"trash", u"Корзина", 72, 72, true, true },
	} };
	return result;
}

[[nodiscard]] inline const ColInfo &Info(Col id) {
	return Infos()[static_cast<int>(id)];
}

[[nodiscard]] inline const char16_t *DensityKey(Density d) {
	switch (d) {
	case Density::Comfortable: return u"comfortable";
	case Density::Normal: return u"normal";
	case Density::Compact: return u"compact";
	}
	return u"normal";
}

[[nodiscard]] inline QString DensityTitle(Density d) {
	switch (d) {
	case Density::Comfortable: return u"Удобная"_q;
	case Density::Normal: return u"Обычная"_q;
	case Density::Compact: return u"Компактная"_q;
	}
	return QString();
}

// Lines of text in the account cell: name, @username, phone, ID.
[[nodiscard]] inline int DensityLines(Density d) {
	switch (d) {
	case Density::Comfortable: return 4;
	case Density::Normal: return 3;
	case Density::Compact: return 1;
	}
	return 4;
}

struct State {
	// Every column once, in the order they are shown. Hidden ones keep
	// their place, so showing a column again puts it back where it was.
	std::vector<Col> order;
	std::array<bool, kColCount> visible{};
	// 0 = the default width, otherwise the user's width at scale 100%.
	std::array<int, kColCount> width{};
	bool pinFirst = true; // the account column stays while scrolling sideways
	Density density = Density::Comfortable;
};

// Open and Trash are always the last two, Account never leaves the front
// part: only the middle can be shuffled.
inline void Normalize(State &state) {
	auto seen = std::array<bool, kColCount>{};
	auto fixed = std::vector<Col>();
	for (const auto id : state.order) {
		const auto i = static_cast<int>(id);
		if (i < 0 || i >= kColCount || seen[i]) {
			continue;
		}
		seen[i] = true;
		fixed.push_back(id);
	}
	for (const auto &info : Infos()) {
		if (!seen[static_cast<int>(info.id)]) {
			fixed.push_back(info.id);
		}
	}
	const auto isTail = [](Col id) {
		return (id == Col::Open) || (id == Col::Trash);
	};
	fixed.erase(
		std::remove_if(fixed.begin(), fixed.end(), isTail),
		fixed.end());
	fixed.push_back(Col::Open);
	fixed.push_back(Col::Trash);
	state.order = std::move(fixed);
	for (const auto &info : Infos()) {
		const auto i = static_cast<int>(info.id);
		if (info.alwaysOn) {
			state.visible[i] = true;
		}
		if (state.width[i] != 0) {
			state.width[i] = std::max(state.width[i], info.minWidth);
		}
	}
}

[[nodiscard]] inline State Defaults() {
	auto result = State();
	for (const auto &info : Infos()) {
		result.order.push_back(info.id);
		result.visible[static_cast<int>(info.id)] = info.defaultVisible;
	}
	Normalize(result);
	return result;
}

[[nodiscard]] inline bool IsDefault(const State &state) {
	const auto defaults = Defaults();
	return state.order == defaults.order
		&& state.visible == defaults.visible
		&& state.width == defaults.width
		&& state.pinFirst == defaults.pinFirst
		&& state.density == defaults.density;
}

[[nodiscard]] inline int ShownCount(const State &state) {
	auto result = 0;
	for (const auto &info : Infos()) {
		if (state.visible[static_cast<int>(info.id)]) {
			++result;
		}
	}
	return result;
}

// The columns the user can turn on and off: all but the always shown ones.
[[nodiscard]] inline int ToggleableCount() {
	auto result = 0;
	for (const auto &info : Infos()) {
		if (!info.alwaysOn) {
			++result;
		}
	}
	return result;
}

[[nodiscard]] inline int ToggleableShown(const State &state) {
	auto result = 0;
	for (const auto &info : Infos()) {
		if (!info.alwaysOn && state.visible[static_cast<int>(info.id)]) {
			++result;
		}
	}
	return result;
}

// «9 из 14» as in the reference's «Columns 9/10».
[[nodiscard]] inline QString CountText(const State &state) {
	return u"%1 из %2"_q
		.arg(ToggleableShown(state))
		.arg(ToggleableCount());
}

inline void SetVisible(State &state, Col id, bool shown) {
	if (Info(id).alwaysOn) {
		return;
	}
	state.visible[static_cast<int>(id)] = shown;
}

// Moves `id` so that it sits right before `before` (or at the end of the
// movable part when `before` is the tail).
inline void MoveBefore(State &state, Col id, Col before) {
	const auto isTail = [](Col c) {
		return (c == Col::Open) || (c == Col::Trash);
	};
	if (isTail(id) || id == before) {
		return;
	}
	auto &order = state.order;
	order.erase(std::remove(order.begin(), order.end(), id), order.end());
	auto at = std::find(order.begin(), order.end(), before);
	order.insert(at, id);
	Normalize(state);
}

// The width in pixels at scale 100% (user's, or the default).
[[nodiscard]] inline int WidthOf(const State &state, Col id) {
	const auto w = state.width[static_cast<int>(id)];
	return w > 0 ? w : Info(id).defaultWidth;
}

inline void SetWidth(State &state, Col id, int width) {
	const auto &info = Info(id);
	if (info.id == Col::Open || info.id == Col::Trash) {
		return;
	}
	state.width[static_cast<int>(id)] = std::max(width, info.minWidth);
}

// Visible columns in display order.
[[nodiscard]] inline std::vector<Col> Shown(const State &state) {
	auto result = std::vector<Col>();
	for (const auto id : state.order) {
		if (state.visible[static_cast<int>(id)]) {
			result.push_back(id);
		}
	}
	return result;
}

[[nodiscard]] inline QByteArray ToJson(const State &state) {
	auto order = QJsonArray();
	auto visible = QJsonObject();
	auto width = QJsonObject();
	for (const auto id : state.order) {
		const auto key = QString(Info(id).key);
		order.push_back(key);
		visible.insert(key, state.visible[static_cast<int>(id)]);
		if (state.width[static_cast<int>(id)] > 0) {
			width.insert(key, state.width[static_cast<int>(id)]);
		}
	}
	auto root = QJsonObject();
	root.insert(u"version"_q, 1);
	root.insert(u"order"_q, order);
	root.insert(u"visible"_q, visible);
	root.insert(u"width"_q, width);
	root.insert(u"pinFirst"_q, state.pinFirst);
	root.insert(u"density"_q, QString(DensityKey(state.density)));
	return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

// Bad or missing data gives the defaults; unknown keys are ignored, new
// columns appear at their default place (see Normalize).
[[nodiscard]] inline State FromJson(const QByteArray &data) {
	auto result = Defaults();
	const auto doc = QJsonDocument::fromJson(data);
	if (!doc.isObject()) {
		return result;
	}
	const auto root = doc.object();
	const auto keyToId = [](const QString &key, Col &out) {
		for (const auto &info : Infos()) {
			if (key == QString(info.key)) {
				out = info.id;
				return true;
			}
		}
		return false;
	};
	if (root.contains(u"order"_q)) {
		result.order.clear();
		for (const auto &value : root.value(u"order"_q).toArray()) {
			auto id = Col::Number;
			if (keyToId(value.toString(), id)) {
				result.order.push_back(id);
			}
		}
	}
	const auto visible = root.value(u"visible"_q).toObject();
	for (auto it = visible.begin(); it != visible.end(); ++it) {
		auto id = Col::Number;
		if (keyToId(it.key(), id)) {
			result.visible[static_cast<int>(id)] = it.value().toBool();
		}
	}
	const auto width = root.value(u"width"_q).toObject();
	for (auto it = width.begin(); it != width.end(); ++it) {
		auto id = Col::Number;
		if (keyToId(it.key(), id)) {
			result.width[static_cast<int>(id)] = it.value().toInt();
		}
	}
	result.pinFirst = root.value(u"pinFirst"_q).toBool(true);
	const auto density = root.value(u"density"_q).toString();
	for (const auto d : { Density::Comfortable, Density::Normal, Density::Compact }) {
		if (density == QString(DensityKey(d))) {
			result.density = d;
		}
	}
	Normalize(result);
	return result;
}

} // namespace Settings::TableLayout
