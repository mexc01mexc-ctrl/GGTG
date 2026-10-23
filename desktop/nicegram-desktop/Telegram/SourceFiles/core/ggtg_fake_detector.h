#pragma once

#include "core/ggtg_id_date.h"
#include "core/ggtg_paths.h"
#include "core/ggtg_ai.h"
#include "data/scraped_members_db.h"
#include "boxes/abstract_box.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/vertical_list.h"
#include "lang/lang_keys.h"
#include "data/data_user.h"
#include "data/data_photo.h"
#include "data/data_session.h"
#include "data/data_peer_values.h"
#include "base/unixtime.h"
#include "styles/style_layers.h"
#include "styles/style_boxes.h"

#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonArray>
#include <QtCore/QFile>
#include <QtCore/QDir>
#include <QtCore/QRegularExpression>
#include <vector>
#include <algorithm>

namespace Ggtg::FakeDetector {

enum class Verdict {
	Alive,
	Suspicious,
	Fake
};

struct AnalysisResult {
	int fakeProbability = 0; // 0..100
	Verdict verdict = Verdict::Alive;
	QString verdictText;
	QStringList reasons;
	bool isMarkedFake = false;
	bool isMarkedAlive = false;
};

inline QString PatternsFilePath() {
	return Core::GgtgPaths::DbDir() + u"fake_patterns.json"_q;
}

struct StoredPatterns {
	std::vector<uint64> markedFakeIds;
	std::vector<uint64> markedAliveIds;
	QStringList markedFakeUsernames;
	QStringList markedAliveUsernames;
	QStringList customKeywords;
};

inline StoredPatterns LoadStoredPatterns() {
	auto result = StoredPatterns();
	auto file = QFile(PatternsFilePath());
	if (!file.open(QIODevice::ReadOnly)) {
		return result;
	}
	const auto doc = QJsonDocument::fromJson(file.readAll());
	if (!doc.isObject()) {
		return result;
	}
	const auto root = doc.object();
	for (const auto &v : root.value(u"marked_fake_ids"_q).toArray()) {
		result.markedFakeIds.push_back(v.toString().toULongLong());
	}
	for (const auto &v : root.value(u"marked_alive_ids"_q).toArray()) {
		result.markedAliveIds.push_back(v.toString().toULongLong());
	}
	for (const auto &v : root.value(u"marked_fake_usernames"_q).toArray()) {
		result.markedFakeUsernames.push_back(v.toString().toLower().trimmed());
	}
	for (const auto &v : root.value(u"marked_alive_usernames"_q).toArray()) {
		result.markedAliveUsernames.push_back(v.toString().toLower().trimmed());
	}
	for (const auto &v : root.value(u"custom_keywords"_q).toArray()) {
		result.customKeywords.push_back(v.toString().toLower().trimmed());
	}
	return result;
}

inline void SaveStoredPatterns(const StoredPatterns &data) {
	auto root = QJsonObject();
	auto fakeIds = QJsonArray();
	for (const auto id : data.markedFakeIds) {
		fakeIds.push_back(QString::number(id));
	}
	root[u"marked_fake_ids"_q] = fakeIds;

	auto aliveIds = QJsonArray();
	for (const auto id : data.markedAliveIds) {
		aliveIds.push_back(QString::number(id));
	}
	root[u"marked_alive_ids"_q] = aliveIds;

	auto fakeUsers = QJsonArray();
	for (const auto &u : data.markedFakeUsernames) {
		fakeUsers.push_back(u);
	}
	root[u"marked_fake_usernames"_q] = fakeUsers;

	auto aliveUsers = QJsonArray();
	for (const auto &u : data.markedAliveUsernames) {
		aliveUsers.push_back(u);
	}
	root[u"marked_alive_usernames"_q] = aliveUsers;

	auto keywords = QJsonArray();
	for (const auto &k : data.customKeywords) {
		keywords.push_back(k);
	}
	root[u"custom_keywords"_q] = keywords;

	QDir().mkpath(Core::GgtgPaths::DbDir());
	auto file = QFile(PatternsFilePath());
	if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
	}
}

inline void MarkAsFake(uint64 userId, const QString &username) {
	auto data = LoadStoredPatterns();
	if (userId > 0 && !ranges::contains(data.markedFakeIds, userId)) {
		data.markedFakeIds.push_back(userId);
	}
	if (userId > 0) {
		data.markedAliveIds.erase(
			std::remove(data.markedAliveIds.begin(), data.markedAliveIds.end(), userId),
			data.markedAliveIds.end());
	}
	const auto cleanUser = username.trimmed().remove('@').toLower();
	if (!cleanUser.isEmpty()) {
		if (!data.markedFakeUsernames.contains(cleanUser)) {
			data.markedFakeUsernames.push_back(cleanUser);
		}
		data.markedAliveUsernames.removeAll(cleanUser);
	}
	SaveStoredPatterns(data);
}

inline void MarkAsAlive(uint64 userId, const QString &username) {
	auto data = LoadStoredPatterns();
	if (userId > 0 && !ranges::contains(data.markedAliveIds, userId)) {
		data.markedAliveIds.push_back(userId);
	}
	if (userId > 0) {
		data.markedFakeIds.erase(
			std::remove(data.markedFakeIds.begin(), data.markedFakeIds.end(), userId),
			data.markedFakeIds.end());
	}
	const auto cleanUser = username.trimmed().remove('@').toLower();
	if (!cleanUser.isEmpty()) {
		if (!data.markedAliveUsernames.contains(cleanUser)) {
			data.markedAliveUsernames.push_back(cleanUser);
		}
		data.markedFakeUsernames.removeAll(cleanUser);
	}
	SaveStoredPatterns(data);
}

inline void ClearMark(uint64 userId, const QString &username) {
	auto data = LoadStoredPatterns();
	if (userId > 0) {
		data.markedFakeIds.erase(
			std::remove(data.markedFakeIds.begin(), data.markedFakeIds.end(), userId),
			data.markedFakeIds.end());
		data.markedAliveIds.erase(
			std::remove(data.markedAliveIds.begin(), data.markedAliveIds.end(), userId),
			data.markedAliveIds.end());
	}
	const auto cleanUser = username.trimmed().remove('@').toLower();
	if (!cleanUser.isEmpty()) {
		data.markedFakeUsernames.removeAll(cleanUser);
		data.markedAliveUsernames.removeAll(cleanUser);
	}
	SaveStoredPatterns(data);
}

inline AnalysisResult AnalyzeUsername(const QString &rawUsername) {
	const auto username = rawUsername.trimmed().remove('@').toLower();
	auto result = AnalysisResult();
	if (username.isEmpty()) {
		result.fakeProbability = 30;
		result.verdict = Verdict::Alive;
		result.verdictText = u"Без юзернейма"_q;
		return result;
	}

	const auto patterns = LoadStoredPatterns();
	if (patterns.markedFakeUsernames.contains(username)) {
		result.fakeProbability = 100;
		result.verdict = Verdict::Fake;
		result.verdictText = u"Помечен как фейк (100%)"_q;
		result.isMarkedFake = true;
		result.reasons.push_back(u"В вашем чёрном списке фейков"_q);
		return result;
	}
	if (patterns.markedAliveUsernames.contains(username)) {
		result.fakeProbability = 0;
		result.verdict = Verdict::Alive;
		result.verdictText = u"Помечен как живой (0%)"_q;
		result.isMarkedAlive = true;
		result.reasons.push_back(u"В вашем белом списке проверенных"_q);
		return result;
	}

	auto score = 15;

	if (username.endsWith(u"bot"_q) || username.endsWith(u"_bot"_q) || username.endsWith(u"robot"_q)) {
		score += 65;
		result.reasons.push_back(u"Имя бота"_q);
	}

	static const auto digitEndRegex = QRegularExpression(u"\\d{4,}$"_q);
	if (digitEndRegex.match(username).hasMatch()) {
		score += 35;
		result.reasons.push_back(u"Случайный цифровой хвост"_q);
	} else {
		static const auto digitEnd3Regex = QRegularExpression(u"\\d{3,}$"_q);
		if (digitEnd3Regex.match(username).hasMatch()) {
			score += 15;
			result.reasons.push_back(u"Цифры в конце ника"_q);
		}
	}

	static const QString spamKeywords[] = {
		u"crypto"_q, u"ton"_q, u"airdrop"_q, u"invest"_q, u"profit"_q,
		u"заработок"_q, u"сигнал"_q, u"trade"_q, u"trading"_q, u"bonus"_q,
		u"claim"_q, u"casino"_q, u"support"_q, u"official"_q, u"manager"_q
	};
	for (const auto &kw : spamKeywords) {
		if (username.contains(kw)) {
			score += 30;
			result.reasons.push_back(u"Спам-слово: "_q + kw);
			break;
		}
	}

	for (const auto &kw : patterns.customKeywords) {
		if (!kw.isEmpty() && username.contains(kw)) {
			score += 25;
			result.reasons.push_back(u"Ключевое слово из разметки: "_q + kw);
			break;
		}
	}

	// High consonant density / gibberish check
	static const auto consonantsRegex = QRegularExpression(u"^[bcdfghjklmnpqrstvwxyz_0-9]+$"_q);
	if (username.size() >= 6 && consonantsRegex.match(username).hasMatch()) {
		score += 25;
		result.reasons.push_back(u"Нечитаемый никнейм"_q);
	}

	score = std::clamp(score, 0, 100);
	result.fakeProbability = score;
	if (score >= 65) {
		result.verdict = Verdict::Fake;
		result.verdictText = u"Фейк (%1%)"_q.arg(score);
	} else if (score >= 35) {
		result.verdict = Verdict::Suspicious;
		result.verdictText = u"Подозрительный (%1%)"_q.arg(score);
	} else {
		result.verdict = Verdict::Alive;
		result.verdictText = u"Живой (%1%)"_q.arg(100 - score);
	}
	return result;
}

inline AnalysisResult AnalyzeUser(
		uint64 userId,
		const QString &username,
		const QString &firstName,
		const QString &lastName,
		const QString &bio,
		bool hasPhoto,
		bool isPremium,
		bool isBot,
		bool isVerified,
		bool isScam,
		bool isFake,
		const QString &onlineText,
		const std::vector<TimeId> &photoDates = {},
		TimeId now = 0) {
	auto result = AnalysisResult();

	const auto patterns = LoadStoredPatterns();
	const auto cleanUser = username.trimmed().remove('@').toLower();

	if ((userId > 0 && ranges::contains(patterns.markedFakeIds, userId))
		|| (!cleanUser.isEmpty() && patterns.markedFakeUsernames.contains(cleanUser))) {
		result.fakeProbability = 100;
		result.verdict = Verdict::Fake;
		result.verdictText = u"Помечен вами как фейк (100%)"_q;
		result.isMarkedFake = true;
		result.reasons.push_back(u"Помечен вами вручную как фейк"_q);
		return result;
	}

	if ((userId > 0 && ranges::contains(patterns.markedAliveIds, userId))
		|| (!cleanUser.isEmpty() && patterns.markedAliveUsernames.contains(cleanUser))) {
		result.fakeProbability = 0;
		result.verdict = Verdict::Alive;
		result.verdictText = u"Помечен вами как живой (0%)"_q;
		result.isMarkedAlive = true;
		result.reasons.push_back(u"Помечен вами вручную как живой"_q);
		return result;
	}

	auto score = 20;

	// Telegram system flags
	if (isScam || isFake) {
		score += 70;
		result.reasons.push_back(u"Метка Scam/Fake от Telegram"_q);
	}
	if (isBot) {
		score += 70;
		result.reasons.push_back(u"Официальный Telegram Bot"_q);
	}
	if (isVerified) {
		score -= 50;
		result.reasons.push_back(u"Верифицированный профиль"_q);
	}
	if (isPremium) {
		score -= 35;
		result.reasons.push_back(u"Telegram Premium"_q);
	}

	// Avatar presence and photo timestamps
	const auto currentTime = (now > 0) ? now : base::unixtime::now();
	if (!hasPhoto) {
		score += 30;
		result.reasons.push_back(u"Нет фото профиля"_q);
	} else {
		score -= 15;
		if (!photoDates.empty()) {
			const auto latestDate = *std::max_element(photoDates.begin(), photoDates.end());
			if (latestDate > 0 && currentTime >= latestDate) {
				const auto photoAge = currentTime - latestDate;
				if (photoAge < 48 * 3600 && (userId > 7000000000ULL || cleanUser.isEmpty())) {
					score += 20;
					result.reasons.push_back(u"Свежее фото профиля (< 48ч) на новом аккаунте"_q);
				} else if (photoAge > 365 * 86400) {
					score -= 25;
					result.reasons.push_back(u"Старое фото профиля (> 1 года, признак живого)"_q);
				}
			}
			// Multiple photos uploaded within small time delta (bot farm batch upload)
			if (photoDates.size() >= 2) {
				auto sorted = photoDates;
				std::sort(sorted.begin(), sorted.end());
				bool quickBatch = false;
				for (size_t i = 1; i < sorted.size(); ++i) {
					if (sorted[i] > 0 && sorted[i - 1] > 0) {
						const auto delta = sorted[i] - sorted[i - 1];
						if (delta >= 0 && delta < 7200) { // < 2 hours
							quickBatch = true;
							break;
						}
					}
				}
				if (quickBatch) {
					score += 35;
					result.reasons.push_back(u"Фото залиты почти одновременно (< 2ч, признак авторега)"_q);
				} else if (sorted.back() > 0 && sorted.front() > 0 && (sorted.back() - sorted.front() > 30 * 86400)) {
					score -= 20;
					result.reasons.push_back(u"Фото профиля менялись со временем (естественный аккаунт)"_q);
				}
			}
		}
	}

	// Registration Date / User ID age
	if (userId > 7900000000ULL) {
		score += 25;
		result.reasons.push_back(u"Очень свежий аккаунт (2025–2026)"_q);
	} else if (userId > 7200000000ULL) {
		score += 15;
		result.reasons.push_back(u"Свежий аккаунт (2024–2025)"_q);
	} else if (userId > 0 && userId < 2500000000ULL) {
		score -= 20;
		result.reasons.push_back(u"Старый аккаунт"_q);
	}

	// Username patterns
	if (cleanUser.isEmpty()) {
		score += 10;
		if (userId > 7500000000ULL) {
			score += 15;
			result.reasons.push_back(u"Новый ID без юзернейма (массовый авторег)"_q);
		} else {
			result.reasons.push_back(u"Без @username"_q);
		}
	} else {
		const auto userAnalysis = AnalyzeUsername(cleanUser);
		if (userAnalysis.fakeProbability >= 65) {
			score += 30;
			for (const auto &r : userAnalysis.reasons) {
				if (!result.reasons.contains(r)) {
					result.reasons.push_back(r);
				}
			}
		} else if (userAnalysis.fakeProbability <= 15) {
			score -= 15;
		}
	}

	// Name patterns
	const auto fullName = (firstName + ' ' + lastName).trimmed().toLower();
	const auto cleanFirst = firstName.trimmed().toLower();
	const auto cleanLast = lastName.trimmed().toLower();

	// Check repeated first and last name ("Ivan Ivan", "Alex Alex")
	if (!cleanFirst.isEmpty() && !cleanLast.isEmpty() && cleanFirst == cleanLast && cleanFirst.size() >= 3) {
		score += 25;
		result.reasons.push_back(u"Одинаковые имя и фамилия («%1 %2»)"_q.arg(firstName).arg(lastName));
	}

	// Check decorative unicode math/script characters (surrogate 0xD835 -> U+1D400..U+1D7FF)
	bool hasDecorativeUnicode = false;
	for (int i = 0; i < firstName.size(); ++i) {
		if (firstName[i].unicode() == 0xD835) {
			hasDecorativeUnicode = true;
			break;
		}
	}
	if (!hasDecorativeUnicode) {
		for (int i = 0; i < lastName.size(); ++i) {
			if (lastName[i].unicode() == 0xD835) {
				hasDecorativeUnicode = true;
				break;
			}
		}
	}
	if (hasDecorativeUnicode) {
		score += 30;
		result.reasons.push_back(u"Декоративный Unicode-шрифт в имени (почерк ботов)"_q);
	}

	// Check emoji-only / symbol-only name (no letters or digits)
	if (!cleanFirst.isEmpty()) {
		bool hasLettersOrDigits = false;
		for (const auto &ch : cleanFirst) {
			if (ch.isLetterOrNumber()) {
				hasLettersOrDigits = true;
				break;
			}
		}
		if (!hasLettersOrDigits) {
			score += 25;
			result.reasons.push_back(u"Имя состоит только из символов/эмодзи"_q);
		}
	}

	static const QString nameKeywords[] = {
		u"crypto"_q, u"ton"_q, u"invest"_q, u"заработок"_q, u"сигналы"_q,
		u"manager"_q, u"admin"_q, u"support"_q, u"official"_q, u"arbitrage"_q
	};
	for (const auto &kw : nameKeywords) {
		if (fullName.contains(kw)) {
			score += 25;
			result.reasons.push_back(u"Спам-слово в имени: "_q + kw);
			break;
		}
	}
	if (firstName.trimmed().size() <= 1 && lastName.trimmed().isEmpty()) {
		score += 10;
		result.reasons.push_back(u"Односимвольное имя"_q);
	}

	// Bio patterns
	const auto lowerBio = bio.toLower();
	if (!lowerBio.isEmpty()) {
		if (lowerBio.contains(u"t.me/"_q) || lowerBio.contains(u"http"_q) || lowerBio.contains(u"bit.ly"_q)) {
			score += 25;
			result.reasons.push_back(u"Рекламная ссылка в Bio"_q);
		}
		static const QString bioKeywords[] = {
			u"канал"_q, u"личка"_q, u"пиши сюда"_q, u"крипт"_q, u"profit"_q,
			u"usdt"_q, u"whatsapp"_q, u"бонус"_q, u"вывод"_q, u"инвестиции"_q
		};
		for (const auto &kw : bioKeywords) {
			if (lowerBio.contains(kw)) {
				score += 20;
				result.reasons.push_back(u"Спам-фраза в Bio: "_q + kw);
				break;
			}
		}
	}

	// Custom keywords from AI learning
	for (const auto &kw : patterns.customKeywords) {
		if (!kw.isEmpty() && (cleanUser.contains(kw) || fullName.contains(kw) || lowerBio.contains(kw))) {
			score += 25;
			result.reasons.push_back(u"Ключевое слово из ИИ-обучения: "_q + kw);
			break;
		}
	}

	// Online status
	const auto lowerOnline = onlineText.toLower();
	if (lowerOnline.contains(u"давно"_q) || lowerOnline.contains(u"long ago"_q)) {
		score += 20;
		result.reasons.push_back(u"Давно не был в сети"_q);
	} else if (lowerOnline.contains(u"недавно"_q) || lowerOnline.contains(u"в сети"_q) || lowerOnline.contains(u"online"_q)) {
		score -= 15;
	}

	score = std::clamp(score, 0, 100);
	result.fakeProbability = score;

	if (score >= 65) {
		result.verdict = Verdict::Fake;
		result.verdictText = u"Фейк (%1%)"_q.arg(score);
	} else if (score >= 35) {
		result.verdict = Verdict::Suspicious;
		result.verdictText = u"Подозрительный (%1%)"_q.arg(score);
	} else {
		result.verdict = Verdict::Alive;
		result.verdictText = u"Живой (%1%)"_q.arg(100 - score);
	}
	return result;
}

inline AnalysisResult AnalyzeUserData(
		not_null<UserData*> user,
		TimeId now = 0,
		const std::vector<TimeId> &photoDates = {}) {
	const auto rawId = user->id.value;
	const auto username = user->username();
	const auto firstName = user->firstName;
	const auto lastName = user->lastName;
	const auto bio = user->about();
	const auto hasPhoto = user->hasUserpic();
	const auto isPremium = user->isPremium();
	const auto isBot = user->isBot();
	const auto isVerified = user->isVerified();
	const auto isScam = user->isScam();
	const auto isFake = user->isFake();
	const auto onlineText = Data::OnlineText(user, now ? now : base::unixtime::now());

	auto dates = photoDates;
	if (dates.empty() && user->hasUserpic()) {
		if (const auto photoId = user->userpicPhotoId()) {
			if (photoId != PeerData::kUnknownPhotoId) {
				if (const auto photo = user->owner().photo(photoId)) {
					if (photo->date() > 0) {
						dates.push_back(photo->date());
					}
				}
			}
		}
	}

	return AnalyzeUser(
		rawId,
		username,
		firstName,
		lastName,
		bio,
		hasPhoto,
		isPremium,
		isBot,
		isVerified,
		isScam,
		isFake,
		onlineText,
		dates,
		now);
}

inline void TrainDetectorFromMarked(
		std::shared_ptr<Ui::Show> show,
		Fn<void(int newRulesCount)> onDone = nullptr) {
	const auto patterns = LoadStoredPatterns();
	auto fakesList = patterns.markedFakeUsernames;
	// Also collect usernames marked as SuspectedFake in the database
	const auto records = Data::LoadAllSmsSendRecords();
	for (const auto &rec : records) {
		if (rec.status == Data::SmsSendStatus::SuspectedFake && !fakesList.contains(rec.username)) {
			fakesList.push_back(rec.username);
		}
	}

	if (fakesList.isEmpty()) {
		show->showToast(u"Нет помеченных фейков для обучения ИИ"_q);
		return;
	}

	show->showToast(u"ИИ анализирует базу фейков..."_q);

	// Statistical token extraction
	static const auto wordRe = QRegularExpression(u"[a-zA-Zа-яА-ЯёЁ]{3,}"_q);
	auto tokenCounts = std::map<QString, int>();
	for (const auto &u : fakesList) {
		auto it = wordRe.globalMatch(u);
		while (it.hasNext()) {
			const auto tok = it.next().captured().toLower();
			if (tok.size() >= 3) {
				tokenCounts[tok]++;
			}
		}
	}

	auto statisticalCandidates = QStringList();
	for (const auto &[tok, count] : tokenCounts) {
		if (count >= 2 && !patterns.markedAliveUsernames.contains(tok) && !patterns.customKeywords.contains(tok)) {
			statisticalCandidates.push_back(tok);
		}
	}

	const auto finishTraining = [=](QStringList foundKeywords, QString explanation) {
		auto updated = LoadStoredPatterns();
		auto added = QStringList();
		for (const auto &kw : foundKeywords) {
			const auto clean = kw.trimmed().toLower();
			if (!clean.isEmpty() && !updated.customKeywords.contains(clean)) {
				updated.customKeywords.push_back(clean);
				added.push_back(clean);
			}
		}
		for (const auto &kw : statisticalCandidates) {
			if (!updated.customKeywords.contains(kw)) {
				updated.customKeywords.push_back(kw);
				added.push_back(kw);
			}
		}
		SaveStoredPatterns(updated);

		// Save knowledge json for AI memory
		auto kObj = QJsonObject();
		kObj[u"updated_at"_q] = QString::number(base::unixtime::now());
		kObj[u"fakes_count"_q] = int(fakesList.size());
		auto kwArr = QJsonArray();
		for (const auto &kw : updated.customKeywords) kwArr.push_back(kw);
		kObj[u"keywords"_q] = kwArr;
		kObj[u"summary"_q] = explanation;

		QDir().mkpath(Core::GgtgPaths::DbDir() + u"ai_knowledge/"_q);
		auto kFile = QFile(Core::GgtgPaths::DbDir() + u"ai_knowledge/fake_patterns.json"_q);
		if (kFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
			kFile.write(QJsonDocument(kObj).toJson(QJsonDocument::Indented));
		}

		show->showBox(Box([=](not_null<Ui::GenericBox*> resBox) {
			resBox->setTitle(rpl::single(u"Обучение детектора завершено"_q));
			auto text = u"Проанализировано фейков: %1\nЖивых аккаунтов: %2\n\n"_q
				.arg(fakesList.size())
				.arg(patterns.markedAliveUsernames.size());
			if (!added.isEmpty()) {
				text += u"Выявлено новых спам-токенов (%1):\n• %2\n\n"_q
					.arg(added.size())
					.arg(added.join(u"\n• "_q));
			} else {
				text += u"Новых повторяющихся паттернов не обнаружено (все известные правила уже учтены).\n\n"_q;
			}
			if (!explanation.isEmpty()) {
				text += u"ИИ-заключение:\n"_q + explanation + u"\n\n";
			}
			text += u"Все новые критерии сохранены и применяются при фильтрации и в рассылках."_q;

			resBox->addRow(
				object_ptr<Ui::FlatLabel>(resBox, text, st::boxLabel),
				st::boxRowPadding);

			resBox->addButton(rpl::single(u"Отлично"_q), [=] {
				resBox->closeBox();
			});
		}));

		if (onDone) onDone(int(added.size()));
	};

	const auto ai = Core::GgtgAi::CachedSettings();
	if (ai.enabled) {
		const auto sampleFakes = fakesList.mid(0, 40).join(u", "_q);
		const auto prompt = u"Проанализируй список спам-ботов и фейковых Telegram-юзернеймов, помеченных пользователем:\n"
			+ sampleFakes + u"\n\nВыяви общие спам-шаблоны и назови 3-7 характерных ключевых слов или подозрительных корней слов. "
			"Ответь в формате JSON:\n{\n  \"keywords\": [\"слово1\", \"слово2\"],\n  \"summary\": \"краткое описание шаблонов ботов\"\n}"_q;

		Core::GgtgAi::Ask(
			u"Ты эксперт по машинному обучению и выявлению ботов в Telegram. Анализируй шаблоны имен и возвращай строго JSON."_q,
			prompt,
			[=](QString reply, QString error) {
				if (!error.isEmpty() || reply.isEmpty()) {
					finishTraining({}, u"Анализ проведён по статистической частотности слов."_q);
					return;
				}
				auto extractedKws = QStringList();
				auto explanation = QString();
				auto jsonStr = reply.trimmed();
				const auto startIdx = jsonStr.indexOf('{');
				const auto endIdx = jsonStr.lastIndexOf('}');
				if (startIdx >= 0 && endIdx > startIdx) {
					jsonStr = jsonStr.mid(startIdx, endIdx - startIdx + 1);
					const auto doc = QJsonDocument::fromJson(jsonStr.toUtf8());
					if (doc.isObject()) {
						const auto o = doc.object();
						for (const auto &v : o.value(u"keywords"_q).toArray()) {
							extractedKws.push_back(v.toString());
						}
						explanation = o.value(u"summary"_q).toString();
					}
				}
				finishTraining(extractedKws, explanation);
			});
	} else {
		finishTraining({}, u"Анализ проведён локально по статистической частотности токенов."_q);
	}
}

// Dialog with detailed report and feedback buttons
inline void ShowFakeDetailsBox(
		std::shared_ptr<Ui::Show> show,
		not_null<UserData*> user,
		Fn<void()> onUpdated = nullptr) {
	show->showBox(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"Анализ профиля: @"_q + user->username()));

		const auto result = AnalyzeUserData(user);

		auto headerColor = (result.verdict == Verdict::Fake)
			? u"red"_q
			: (result.verdict == Verdict::Suspicious)
			? u"orange"_q
			: u"green"_q;

		box->addRow(
			object_ptr<Ui::FlatLabel>(
				box,
				u"Вердикт: %1"_q.arg(result.verdictText),
				st::boxTitle),
			st::boxRowPadding);

		if (!result.reasons.isEmpty()) {
			box->addRow(
				object_ptr<Ui::FlatLabel>(
					box,
					u"Факторы оценки:\n• "_q + result.reasons.join(u"\n• "_q),
					st::boxLabel),
				st::boxRowPadding);
		} else {
			box->addRow(
				object_ptr<Ui::FlatLabel>(
					box,
					u"Подозрительных признаков не обнаружено."_q,
					st::boxLabel),
				st::boxRowPadding);
		}

		Ui::AddSkip(box->verticalLayout(), st::defaultVerticalListSkip);

		// AI Analysis Section
		const auto aiButton = box->addRow(
			object_ptr<Ui::RoundButton>(
				box,
				rpl::single(u"Запросить разбор у ИИ-нейросети"_q),
				st::defaultBoxButton),
			st::boxRowPadding);

		const auto aiAnswerLabel = box->addRow(
			object_ptr<Ui::FlatLabel>(
				box,
				QString(),
				st::boxLabel),
			st::boxRowPadding);
		aiAnswerLabel->hide();

		aiButton->setClickedCallback([=] {
			aiButton->setText(rpl::single(u"Анализирую профиль..."_q));
			aiButton->setEnabled(false);

			const auto prompt = u"Оцени Telegram-профиль пользователя:\n"
				"ID: %1\n"
				"Username: @%2\n"
				"Имя: %3 %4\n"
				"Описание: %5\n"
				"Премиум: %6\n"
				"Дай краткий вердикт: живой человек, бот или спам-аккаунт, и назови главную причину (до 2 предложений)."_q
				.arg(user->id.value)
				.arg(user->username())
				.arg(user->firstName)
				.arg(user->lastName)
				.arg(user->about())
				.arg(user->isPremium() ? u"Да"_q : u"Нет"_q);

			Core::GgtgAi::Ask(
				u"Ты эксперт по выявлению спам-ботов и фейковых аккаунтов в Telegram. Отвечай кратко и аргументированно."_q,
				prompt,
				[=](QString text, QString error) {
					aiButton->setText(rpl::single(u"Повторить ИИ-разбор"_q));
					aiButton->setEnabled(true);
					if (!error.isEmpty()) {
						aiAnswerLabel->setText(u"Ошибка ИИ: "_q + error);
					} else {
						aiAnswerLabel->setText(u"ИИ-вердикт:\n"_q + text.trimmed());
					}
					aiAnswerLabel->show();
				});
		});

		Ui::AddDivider(box->verticalLayout());

		// Feedback Buttons: "Пометить как фейк" / "Пометить как живой"
		box->addRow(
			object_ptr<Ui::FlatLabel>(
				box,
				u"Обучение детектора (ваша обратная связь):"_q,
				st::boxDividerLabel),
			st::boxRowPadding);

		const auto markRow = box->addRow(
			object_ptr<Ui::VerticalLayout>(box),
			st::boxRowPadding);

		const auto fakeBtn = markRow->add(
			object_ptr<Ui::SettingsButton>(
				markRow,
				rpl::single(result.isMarkedFake ? u"✓ Помечен вами как фейк"_q : u"Пометить: «Это фейк / бот»"_q)));

		fakeBtn->setClickedCallback([=] {
			MarkAsFake(user->id.value, user->username());
			show->showToast(u"Профиль добавлен в список фейков. Веса обновлены."_q);
			box->closeBox();
			if (onUpdated) onUpdated();
		});

		const auto aliveBtn = markRow->add(
			object_ptr<Ui::SettingsButton>(
				markRow,
				rpl::single(result.isMarkedAlive ? u"✓ Помечен вами как живой"_q : u"Пометить: «Живой человек»"_q)));

		aliveBtn->setClickedCallback([=] {
			MarkAsAlive(user->id.value, user->username());
			show->showToast(u"Профиль добавлен в белый список."_q);
			box->closeBox();
			if (onUpdated) onUpdated();
		});

		if (result.isMarkedFake || result.isMarkedAlive) {
			const auto resetBtn = markRow->add(
				object_ptr<Ui::SettingsButton>(
					markRow,
					rpl::single(u"Сбросить мою метку"_q)));
			resetBtn->setClickedCallback([=] {
				ClearMark(user->id.value, user->username());
				show->showToast(u"Метка сброшена."_q);
				box->closeBox();
				if (onUpdated) onUpdated();
			});
		}

		const auto trainBtn = markRow->add(
			object_ptr<Ui::SettingsButton>(
				markRow,
				rpl::single(u"Обучить ИИ на помеченных"_q)));
		trainBtn->setClickedCallback([=] {
			TrainDetectorFromMarked(show, [=](int count) {
				if (onUpdated) onUpdated();
			});
		});

		box->addButton(rpl::single(u"Закрыть"_q), [=] {
			box->closeBox();
		});
	}));
}

// Database bulk cleanup dialog
struct CleanResult {
	QStringList alive;
	QStringList fakes;
};

inline CleanResult FilterUsernames(const QStringList &usernames, int thresholdPercent = 60) {
	auto result = CleanResult();
	for (const auto &u : usernames) {
		const auto res = AnalyzeUsername(u);
		if (res.fakeProbability >= thresholdPercent) {
			result.fakes.push_back(u);
		} else {
			result.alive.push_back(u);
		}
	}
	return result;
}

inline void ShowCleanBaseBox(
		std::shared_ptr<Ui::Show> show,
		uint64 baseId,
		const QString &title,
		Fn<void()> onDone = nullptr) {
	const auto usernames = Data::LoadScrapedGroupUsernames(baseId);
	if (usernames.isEmpty()) {
		show->showToast(u"В этой базе нет пользователей"_q);
		return;
	}

	show->showBox(Box([=](not_null<Ui::GenericBox*> box) {
		box->setWidth(style::ConvertScale(520));
		box->setTitle(rpl::single(u"Очистка от фейков: "_q + title));

		const auto group = std::make_shared<Ui::RadiobuttonGroup>(60);

		const auto filterAt = [=](int threshold) {
			return FilterUsernames(usernames, threshold);
		};

		const auto initialResult = filterAt(60);

		const auto statsLabel = box->addRow(
			object_ptr<Ui::FlatLabel>(
				box,
				u"Всего в базе: %1\nПодозрительных / фейков: %2\nОстанется живых: %3"_q
					.arg(usernames.size())
					.arg(initialResult.fakes.size())
					.arg(initialResult.alive.size()),
				st::boxLabel),
			st::boxRowPadding);

		Ui::AddSkip(box->verticalLayout(), st::defaultVerticalListSkip);

		box->addRow(
			object_ptr<Ui::FlatLabel>(
				box,
				u"Уровень строгости фильтрации:"_q,
				st::boxDividerLabel),
			st::boxRowPadding);

		const auto updateStats = [=](int threshold) {
			const auto res = filterAt(threshold);
			statsLabel->setText(
				u"Всего в базе: %1\nПодозрительных / фейков: %2\nОстанется живых: %3"_q
					.arg(usernames.size())
					.arg(res.fakes.size())
					.arg(res.alive.size()));
		};

		box->addRow(
			object_ptr<Ui::Radiobutton>(
				box,
				group,
				80,
				u"Мягкий (только очевидные боты и скам >80%)"_q),
			st::boxRowPadding);

		box->addRow(
			object_ptr<Ui::Radiobutton>(
				box,
				group,
				60,
				u"Стандартный (рекомендуется, порог >60%)"_q),
			st::boxRowPadding);

		box->addRow(
			object_ptr<Ui::Radiobutton>(
				box,
				group,
				40,
				u"Строгий (удалить всех подозрительных >40%)"_q),
			st::boxRowPadding);

		group->setChangedCallback([=](int value) {
			updateStats(value);
		});

		box->addButton(rpl::single(u"Отделить в список фейков"_q), [=] {
			const auto threshold = group->current();
			const auto res = filterAt(threshold);
			if (res.fakes.isEmpty()) {
				show->showToast(u"Фейков при данном пороге не найдено"_q);
				box->closeBox();
				return;
			}
			const auto fakeBaseTitle = title + u" - ФЕЙКИ"_q;
			const auto fakeBaseId = Data::GenerateManualBaseId();
			Data::SaveScrapedGroupUsernames(fakeBaseId, fakeBaseTitle, res.fakes);
			Data::SaveScrapedGroupUsernames(baseId, title, res.alive);
			show->showToast(u"Создана база «%1» (%2 чел.). В текущей осталось: %3"_q
				.arg(fakeBaseTitle)
				.arg(res.fakes.size())
				.arg(res.alive.size()));
			box->closeBox();
			if (onDone) onDone();
		});

		box->addButton(rpl::single(u"Удалить навсегда"_q), [=] {
			const auto threshold = group->current();
			const auto res = filterAt(threshold);
			if (res.fakes.isEmpty()) {
				show->showToast(u"Фейков при данном пороге не найдено"_q);
				box->closeBox();
				return;
			}
			Data::SaveScrapedGroupUsernames(baseId, title, res.alive);
			show->showToast(u"Удалено фейков: %1. Осталось в базе: %2"_q
				.arg(res.fakes.size())
				.arg(res.alive.size()));
			box->closeBox();
			if (onDone) onDone();
		});

		box->addButton(rpl::single(u"Обучить ИИ"_q), [=] {
			TrainDetectorFromMarked(show, [=](int count) {
				updateStats(group->current());
			});
		});

		box->addButton(rpl::single(u"Отмена"_q), [=] {
			box->closeBox();
		});
	}));
}

} // namespace Ggtg::FakeDetector
