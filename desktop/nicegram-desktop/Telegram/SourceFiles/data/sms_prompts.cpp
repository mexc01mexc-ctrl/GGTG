/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "data/sms_prompts.h"
#include "core/ggtg_paths.h"

#include "base/random.h"
#include "settings.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSaveFile>

namespace Data {
namespace {

// Same directory as scraped_members_db.cpp's DbDir(), duplicated locally
// rather than exported from there - both files independently keep their
// own small ReadJson/WriteJson pair, matching this codebase's existing
// one-file-per-concern style for the "tdata/ggtg_db/" store.
[[nodiscard]] QString DbDir() {
	return Core::GgtgPaths::DbDir();
}

[[nodiscard]] QString FormPath() {
	return DbDir() + u"sms_form.json"_q;
}

[[nodiscard]] QString JobPath() {
	return DbDir() + u"sms_job.json"_q;
}

[[nodiscard]] QString PromptsPath() {
	return DbDir() + u"sms_prompts.json"_q;
}

[[nodiscard]] QJsonDocument ReadJson(const QString &path) {
	auto file = QFile(path);
	if (!file.open(QIODevice::ReadOnly)) {
		return QJsonDocument();
	}
	return QJsonDocument::fromJson(file.readAll());
}

void WriteJson(const QString &path, const QJsonDocument &document) {
	QDir().mkpath(DbDir());
	auto file = QSaveFile(path);
	if (file.open(QIODevice::WriteOnly)) {
		file.write(document.toJson(QJsonDocument::Compact));
		file.commit();
	}
}

} // namespace

std::vector<SmsPrompt> LoadSmsPrompts() {
	auto result = std::vector<SmsPrompt>();
	const auto document = ReadJson(PromptsPath());
	for (const auto &value : document.array()) {
		const auto object = value.toObject();
		const auto name = object["name"].toString();
		if (name.isEmpty()) {
			continue;
		}
		result.push_back({
			.id = object["id"].toString().toULongLong(),
			.name = name,
			.text = object["text"].toString(),
		});
	}
	return result;
}

void SaveAllSmsPrompts(const std::vector<SmsPrompt> &prompts) {
	auto array = QJsonArray();
	for (const auto &prompt : prompts) {
		array.append(QJsonObject{
			{ "id", QString::number(prompt.id) },
			{ "name", prompt.name },
			{ "text", prompt.text },
		});
	}
	WriteJson(PromptsPath(), QJsonDocument(array));
}

void SaveSmsPrompt(uint64 id, const QString &name, const QString &text) {
	auto prompts = LoadSmsPrompts();
	auto found = false;
	for (auto &prompt : prompts) {
		if (prompt.id == id) {
			prompt.name = name;
			prompt.text = text;
			found = true;
			break;
		}
	}
	if (!found) {
		prompts.push_back({ .id = id, .name = name, .text = text });
	}
	SaveAllSmsPrompts(prompts);
}

void DeleteSmsPrompt(uint64 id) {
	auto prompts = LoadSmsPrompts();
	const auto before = prompts.size();
	prompts.erase(
		std::remove_if(
			prompts.begin(),
			prompts.end(),
			[&](const SmsPrompt &prompt) { return prompt.id == id; }),
		prompts.end());
	if (prompts.size() == before) {
		return;
	}
	SaveAllSmsPrompts(prompts);
}

namespace {

[[nodiscard]] QJsonArray IdsToJson(const std::vector<uint64> &ids) {
	auto result = QJsonArray();
	for (const auto id : ids) {
		result.append(QString::number(id));
	}
	return result;
}

[[nodiscard]] std::vector<uint64> IdsFromJson(const QJsonValue &value) {
	auto result = std::vector<uint64>();
	for (const auto &item : value.toArray()) {
		result.push_back(item.toString().toULongLong());
	}
	return result;
}

} // namespace

SmsFormSettings LoadSmsForm() {
	const auto document = ReadJson(FormPath());
	if (!document.isObject()) {
		return {};
	}
	const auto object = document.object();
	auto result = SmsFormSettings();
	result.saved = true;
	result.accountUserIds = IdsFromJson(object["accounts"]);
	result.selectedBaseIds = IdsFromJson(object["selectedBases"]);
	result.knownBaseIds = IdsFromJson(object["knownBases"]);
	result.limitPerAccount = object["limitPerAccount"].toString();
	result.limitThreads = object["limitThreads"].toString();
	result.limitSkipSpamBlock = object["limitSkipSpamBlock"].toString();
	result.pauseMin = object["pauseMin"].toString();
	result.pauseMax = object["pauseMax"].toString();
	result.pauseAccountMin = object["pauseAccountMin"].toString();
	result.pauseAccountMax = object["pauseAccountMax"].toString();
	result.limitRefusals = object["limitRefusals"].toString();
	result.limitMode = object["limitMode"].toString();
	result.rotationPromptIds = IdsFromJson(object["rotationPrompts"]);
	result.protectionMode = object.contains("protectionMode")
		? object["protectionMode"].toInt(1)
		: 1;
	result.nightSleep = object.contains("nightSleep")
		? object["nightSleep"].toBool(true)
		: true;
	result.simulateTypos = object.contains("simulateTypos")
		? object["simulateTypos"].toBool(true)
		: true;
	result.filterByGgr = object["filterByGgr"].toBool(false);
	result.skipSuspectedFakes = object.contains("skipSuspectedFakes")
		? object["skipSuspectedFakes"].toBool(true)
		: true;
	result.warmup = object["warmup"].toBool();
	result.greetingSticker = object["greetingSticker"].toBool();
	result.text = object["text"].toString();
	return result;
}

void SaveSmsForm(const SmsFormSettings &form) {
	WriteJson(FormPath(), QJsonDocument(QJsonObject{
		{ "accounts", IdsToJson(form.accountUserIds) },
		{ "selectedBases", IdsToJson(form.selectedBaseIds) },
		{ "knownBases", IdsToJson(form.knownBaseIds) },
		{ "limitPerAccount", form.limitPerAccount },
		{ "limitThreads", form.limitThreads },
		{ "limitSkipSpamBlock", form.limitSkipSpamBlock },
		{ "pauseMin", form.pauseMin },
		{ "pauseMax", form.pauseMax },
		{ "pauseAccountMin", form.pauseAccountMin },
		{ "pauseAccountMax", form.pauseAccountMax },
		{ "limitRefusals", form.limitRefusals },
		{ "limitMode", form.limitMode },
		{ "rotationPrompts", IdsToJson(form.rotationPromptIds) },
		{ "protectionMode", form.protectionMode },
		{ "nightSleep", form.nightSleep },
		{ "simulateTypos", form.simulateTypos },
		{ "filterByGgr", form.filterByGgr },
		{ "skipSuspectedFakes", form.skipSuspectedFakes },
		{ "warmup", form.warmup },
		{ "greetingSticker", form.greetingSticker },
		{ "text", form.text },
	}));
}

std::optional<SmsJob> LoadSmsJob() {
	const auto document = ReadJson(JobPath());
	if (!document.isObject()) {
		return std::nullopt;
	}
	const auto object = document.object();
	auto result = SmsJob();
	for (const auto &value : object["usernames"].toArray()) {
		result.usernames.push_back(value.toString());
	}
	if (result.usernames.isEmpty()) {
		return std::nullopt;
	}
	const auto bases = object["baseOf"].toObject();
	for (auto it = bases.begin(); it != bases.end(); ++it) {
		result.baseOf.emplace_back(
			it.key(),
			it.value().toString().toULongLong());
	}
	for (const auto &value : object["templates"].toArray()) {
		result.templates.push_back(value.toString());
	}
	result.accountUserIds = IdsFromJson(object["accounts"]);
	result.perAccountQuota = object["quota"].toInt(1);
	result.threadCount = object["threads"].toInt(1);
	result.spamTolerance = object["spamTolerance"].toInt(0);
	result.refusalLimit = object["refusalLimit"].toInt(3);
	result.protectionMode = object.contains("protectionMode")
		? object["protectionMode"].toInt(1)
		: 1;
	result.nightSleep = object.contains("nightSleep")
		? object["nightSleep"].toBool(true)
		: true;
	result.simulateTypos = object.contains("simulateTypos")
		? object["simulateTypos"].toBool(true)
		: true;
	result.filterByGgr = object["filterByGgr"].toBool(false);
	result.skipSuspectedFakes = object.contains("skipSuspectedFakes")
		? object["skipSuspectedFakes"].toBool(true)
		: true;
	result.warmup = object["warmup"].toBool();
	result.greetingSticker = object["greetingSticker"].toBool();
	result.pauseMinSeconds = object["pauseMin"].toInt(40);
	result.pauseMaxSeconds = object["pauseMax"].toInt(60);
	result.pauseAccountMinSeconds = object["pauseAccountMin"].toInt(60);
	result.pauseAccountMaxSeconds = object["pauseAccountMax"].toInt(90);
	return result;
}

void SaveSmsJob(const SmsJob &job) {
	auto usernames = QJsonArray();
	for (const auto &username : job.usernames) {
		usernames.append(username);
	}
	auto bases = QJsonObject();
	for (const auto &[username, baseId] : job.baseOf) {
		bases[username] = QString::number(baseId);
	}
	auto templates = QJsonArray();
	for (const auto &text : job.templates) {
		templates.append(text);
	}
	WriteJson(JobPath(), QJsonDocument(QJsonObject{
		{ "usernames", usernames },
		{ "baseOf", bases },
		{ "templates", templates },
		{ "accounts", IdsToJson(job.accountUserIds) },
		{ "quota", job.perAccountQuota },
		{ "threads", job.threadCount },
		{ "spamTolerance", job.spamTolerance },
		{ "refusalLimit", job.refusalLimit },
		{ "protectionMode", job.protectionMode },
		{ "nightSleep", job.nightSleep },
		{ "simulateTypos", job.simulateTypos },
		{ "filterByGgr", job.filterByGgr },
		{ "skipSuspectedFakes", job.skipSuspectedFakes },
		{ "warmup", job.warmup },
		{ "greetingSticker", job.greetingSticker },
		{ "pauseMin", job.pauseMinSeconds },
		{ "pauseMax", job.pauseMaxSeconds },
		{ "pauseAccountMin", job.pauseAccountMinSeconds },
		{ "pauseAccountMax", job.pauseAccountMaxSeconds },
	}));
}

void ClearSmsJob() {
	QFile::remove(JobPath());
}

uint64 GenerateSmsPromptId() {
	return base::RandomValue<uint64>();
}

} // namespace Data
