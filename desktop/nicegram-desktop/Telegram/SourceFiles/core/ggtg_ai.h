/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and exceptions, see https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "core/ggtg_paths.h"
#include "base/basic_types.h"

#include <crl/crl.h>
#include <rpl/variable.h>

#include <deque>
#include <functional>
#include <map>

#include <QtCore/QCryptographicHash>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QDirIterator>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QProcess>
#include <QtCore/QRegularExpression>
#include <QtCore/QSaveFile>
#include <QtCore/QSet>
#include <QtCore/QStandardPaths>
#include <QtGui/QTextDocument>
#include <QtGui/QImage>
#include <QtCore/QBuffer>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>
#include <QtWidgets/QApplication>

namespace Core::GgtgAi {

// GGTG's AI helpers talk to free cloud models through the OpenAI-compatible
// "/chat/completions" API. Google Gemini, Groq, OpenRouter, NVIDIA and LLM7
// all give a key without a card; the user pastes any number of
// keys in the "ИИ" section and the provider is told by the key itself.
//
// Each job goes to the models that suit it (see Task): replies to people and
// the first message to the cleverest model available, sorting thousands of
// messages to the fast ones with big limits, pictures to the ones that see.
// When a model hits its limit or fails it rests, and the next one takes the
// job. The memory, the knowledge and the style live on this computer
// (tdata/ggtg_db/), so a change of provider loses nothing.
//
// Bulk jobs carry other people's messages by the thousand: they go out with
// names, phone numbers, @usernames, mails and links replaced by marks, which
// are put back into the answer (see details::Mask).
//
// The settings live in tdata/ggtg_db/ai_settings.json and are edited in the
// "ИИ" section of the GGTG window.
enum class Role {
	General, // writing, rewriting, analysis, summaries
	Translator, // translation only
};

// What a job needs from a model: decides which models take it.
enum class Task {
	Smart, // replies to people, the first message, teaching the AI, the agent
	Bulk, // sorting replies, summaries, translation, logs: fast, big limits
	Vision, // pictures and screenshots
};

namespace details {

[[nodiscard]] inline QNetworkAccessManager &Manager() {
	static auto result = new QNetworkAccessManager(qApp);
	return *result;
}

} // namespace details

// ---- providers: who gives free keys, and how a key tells its provider ----

struct CloudProvider {
	QString id;
	QString name;
	QString baseUrl;
	QString keyUrl;
	QString note;
	QStringList prefixes;
};

[[nodiscard]] inline const std::vector<CloudProvider> &CloudProviders() {
	// Only services that give a key without a card (checked in October
	// 2026). Gone from here: GitHub Models (closed on July 30, 2026),
	// SambaNova and Cerebras (both want a card now).
	static const auto result = std::vector<CloudProvider>{
		{
			u"gemini"_q,
			u"Google Gemini"_q,
			u"https://generativelanguage.googleapis.com/v1beta/openai"_q,
			u"https://aistudio.google.com/apikey"_q,
			u"самый умный, видит картинки. Нужен только аккаунт Google"_q,
			{ u"AIza"_q },
		},
		{
			u"groq"_q,
			u"Groq"_q,
			u"https://api.groq.com/openai/v1"_q,
			u"https://console.groq.com/keys"_q,
			u"очень быстрый, большие лимиты. Вход через Google или почту"_q,
			{ u"gsk_"_q },
		},
		{
			u"openrouter"_q,
			u"OpenRouter"_q,
			u"https://openrouter.ai/api/v1"_q,
			u"https://openrouter.ai/keys"_q,
			u"десятки бесплатных моделей, около 50 запросов в день"_q,
			{ u"sk-or-"_q },
		},
		{
			u"nvidia"_q,
			u"NVIDIA"_q,
			u"https://integrate.api.nvidia.com/v1"_q,
			u"https://build.nvidia.com/settings/api-keys"_q,
			u"большие открытые модели (DeepSeek, Qwen). Просит подтвердить "
				"телефон, это не всегда проходит"_q,
			{ u"nvapi-"_q },
		},
		{
			u"llm7"_q,
			u"LLM7"_q,
			u"https://api.llm7.io/v1"_q,
			u"https://token.llm7.io/"_q,
			u"работает совсем без регистрации. Запасной вариант, сервис "
				"сторонний"_q,
			{ u"unused"_q },
		},
	};
	return result;
}

// The provider a key belongs to, by how it starts; empty when unknown.
[[nodiscard]] inline QString DetectProvider(const QString &key) {
	const auto trimmed = key.trimmed();
	for (const auto &provider : CloudProviders()) {
		for (const auto &prefix : provider.prefixes) {
			if (trimmed.startsWith(prefix)) {
				return provider.id;
			}
		}
	}
	return QString();
}

// Null for an unknown id.
[[nodiscard]] inline const CloudProvider *CloudProviderById(
		const QString &id) {
	for (const auto &provider : CloudProviders()) {
		if (provider.id == id) {
			return &provider;
		}
	}
	return nullptr;
}

[[nodiscard]] inline QString CloudProviderName(const QString &id) {
	const auto provider = CloudProviderById(id);
	return provider ? provider->name : u"неизвестный сервис"_q;
}

// "AIza…x9Qw": enough to tell keys apart, not enough to use one.
[[nodiscard]] inline QString ShortKey(const QString &key) {
	const auto trimmed = key.trimmed();
	return (trimmed.size() <= 12)
		? trimmed
		: (trimmed.left(6) + QChar(0x2026) + trimmed.right(4));
}

// ---- settings ----

struct CloudKey {
	QString key;
	QString provider; // see CloudProviders(); told by the key itself
};

struct Settings {
	bool enabled = true;
	// Which helpers are on (each one also needs `enabled`).
	bool templateAssistant = true; // "ИИ-помощник" under the message text
	bool summaries = true; // "Сводка ИИ" on a process
	// "Перевести (ИИ)" in a message's right-click menu, and what to
	// translate into (a code from Languages()).
	bool messageTranslate = true;
	QString translateTo = u"ru"_q;
	// The "ИИ" button in a chat's input field (reads the conversation and
	// suggests replies) and the user's own instructions for it: who they are,
	// what the talk is about, how to answer.
	bool chatAssistant = true;
	// How many days a conversation's memory is kept without being used
	// (0 - forever).
	int memoryDays = 30;
	QString chatPrompt;
	// "Отправка SMS": every recipient gets the template in their own
	// language (translated once per language, see TranslateTemplateCached).
	bool smsTranslate = false;
	QString smsTranslateLang = u"auto"_q;
	// The agent thinks before it answers: slower, but fewer slips.
	bool agentThink = false;
	// "Лента ответов": drafts for the people who answered a mailing are
	// prepared in the background every few minutes (nothing is sent without
	// the user's word). On by default: it needs a key anyway.
	bool inboxAuto = true;
	// A short brief in the Saved Messages of the active account every
	// morning: who waits, hot leads, what was learned.
	bool briefDaily = true;
	// "Обучение на чатах" runs by itself at night (1:00-7:00) for the
	// chosen accounts.
	bool trainNightly = true;
	// Chats stay marked "unread" after what the program does in them (a skipped
	// answer, a reply sent from the feed), so the person is not lost.
	bool keepUnread = true;
	// The free cloud keys, in the order they were added. Any number, from
	// any providers; they stand in for each other.
	std::vector<CloudKey> keys;
};

[[nodiscard]] inline QString SettingsPath() {
	return Core::GgtgPaths::DbDir() + u"ai_settings.json"_q;
}

[[nodiscard]] inline Settings Load() {
	auto result = Settings();
	auto file = QFile(SettingsPath());
	if (!file.open(QIODevice::ReadOnly)) {
		return result;
	}
	const auto object = QJsonDocument::fromJson(file.readAll()).object();
	result.enabled = object.value("enabled").toBool(result.enabled);
	result.templateAssistant = object.value("templateAssistant").toBool(
		result.templateAssistant);
	result.summaries = object.value("summaries").toBool(result.summaries);
	result.messageTranslate = object.value("messageTranslate").toBool(
		result.messageTranslate);
	result.translateTo = object.value("translateTo").toString(
		result.translateTo);
	result.chatAssistant = object.value("chatAssistant").toBool(
		result.chatAssistant);
	result.memoryDays = std::clamp(
		object.value("memoryDays").toInt(result.memoryDays),
		0,
		3650);
	result.chatPrompt = object.value("chatPrompt").toString();
	result.smsTranslate = object.value("smsTranslate").toBool(
		result.smsTranslate);
	result.smsTranslateLang = object.value("smsTranslateLang").toString(
		result.smsTranslateLang.isEmpty() ? u"auto"_q : result.smsTranslateLang);
	result.agentThink = object.value("agentThink").toBool(
		result.agentThink);
	result.inboxAuto = object.value("inboxAuto").toBool(result.inboxAuto);
	result.briefDaily = object.value("briefDaily").toBool(result.briefDaily);
	result.trainNightly = object.value("trainNightly").toBool(
		result.trainNightly);
	result.keepUnread = object.value("keepUnread").toBool(
		result.keepUnread);
	const auto addKey = [&](const QString &raw, const QString &provider) {
		const auto key = raw.trimmed();
		if (key.isEmpty()) {
			return;
		}
		for (const auto &existing : result.keys) {
			if (existing.key == key) {
				return;
			}
		}
		const auto detected = DetectProvider(key);
		const auto id = detected.isEmpty() ? provider : detected;
		// Keys of the services that closed or want a card now are dropped:
		// they can not answer any more.
		if (id == u"github"_q
			|| id == u"sambanova"_q
			|| id == u"cerebras"_q) {
			return;
		}
		result.keys.push_back({ key, id });
	};
	if (object.contains("keys")) {
		for (const auto &value : object.value("keys").toArray()) {
			const auto entry = value.toObject();
			addKey(
				entry.value("key").toString(),
				entry.value("provider").toString());
		}
	} else {
		// Earlier versions: up to three "cloud slots", or a single key.
		for (const auto &value : object.value("cloudSlots").toArray()) {
			const auto slot = value.toObject();
			addKey(
				slot.value("key").toString(),
				slot.value("provider").toString());
		}
		addKey(
			object.value("cloudKey").toString(),
			object.value("cloudProvider").toString());
	}
	return result;
}

// The settings as last loaded or saved: for the places that ask on every
// repaint (the "ИИ" button of the input field) and must not read a file.
[[nodiscard]] inline Settings &CachedSettings() {
	static auto result = Load();
	return result;
}

inline void Save(const Settings &settings) {
	CachedSettings() = settings;
	auto file = QSaveFile(SettingsPath());
	if (!file.open(QIODevice::WriteOnly)) {
		return;
	}
	file.write(QJsonDocument(QJsonObject{
		{ "enabled", settings.enabled },
		{ "templateAssistant", settings.templateAssistant },
		{ "summaries", settings.summaries },
		{ "messageTranslate", settings.messageTranslate },
		{ "translateTo", settings.translateTo },
		{ "chatAssistant", settings.chatAssistant },
		{ "memoryDays", settings.memoryDays },
		{ "chatPrompt", settings.chatPrompt },
		{ "smsTranslate", settings.smsTranslate },
		{ "smsTranslateLang", settings.smsTranslateLang.isEmpty() ? u"auto"_q : settings.smsTranslateLang },
		{ "agentThink", settings.agentThink },
		{ "inboxAuto", settings.inboxAuto },
		{ "briefDaily", settings.briefDaily },
		{ "trainNightly", settings.trainNightly },
		{ "keepUnread", settings.keepUnread },
		{ "keys", [&] {
			auto array = QJsonArray();
			for (const auto &key : settings.keys) {
				array.push_back(QJsonObject{
					{ "key", key.key },
					{ "provider", key.provider },
				});
			}
			return array;
		}() },
	}).toJson(QJsonDocument::Indented));
	file.commit();
}

// A key GGTG knows how to use.
[[nodiscard]] inline bool CloudKeyUsable(const CloudKey &key) {
	return !key.key.trimmed().isEmpty()
		&& CloudProviderById(key.provider) != nullptr;
}

// The AI is on and has at least one key it can use.
[[nodiscard]] inline bool CloudReady(const Settings &settings) {
	if (!settings.enabled) {
		return false;
	}
	for (const auto &key : settings.keys) {
		if (CloudKeyUsable(key)) {
			return true;
		}
	}
	return false;
}

// ---- which model for which job ----

// One row of the table below: a model of a provider that suits a job, how
// well (bigger is better), the pattern its name matches in the provider's
// own list (the newest version is taken) and the name to use while that list
// is not known yet. OpenRouter is not here: its free models come from its
// list (see OpenRouterFree) and change every few weeks.
struct ModelChoice {
	QString provider;
	Task task = Task::Smart;
	int score = 0;
	QString pattern;
	QString fallback;
};

[[nodiscard]] inline const std::vector<ModelChoice> &ModelChoices() {
	static const auto result = std::vector<ModelChoice>{
		// Smart: the cleverest that answers.
		{ u"gemini"_q, Task::Smart, 100,
			u"^gemini-[\\d.]+-pro$"_q, u"gemini-2.5-pro"_q },
		{ u"gemini"_q, Task::Smart, 92,
			u"^gemini-flash-latest$"_q, u"gemini-flash-latest"_q },
		{ u"gemini"_q, Task::Smart, 90,
			u"^gemini-[\\d.]+-flash$"_q, u"gemini-2.5-flash"_q },
		{ u"nvidia"_q, Task::Smart, 86,
			u"^nvidia/llama-3\\.1-nemotron-70b-instruct"_q, u"nvidia/llama-3.1-nemotron-70b-instruct"_q },
		{ u"groq"_q, Task::Smart, 84,
			u"^moonshotai/kimi-k2"_q, u"moonshotai/kimi-k2-instruct"_q },
		{ u"groq"_q, Task::Smart, 83,
			u"^openai/gpt-oss-120b$"_q, u"openai/gpt-oss-120b"_q },
		{ u"nvidia"_q, Task::Smart, 82,
			u"^mistralai/mistral-large-2-instruct"_q, u"mistralai/mistral-large-2-instruct"_q },
		{ u"groq"_q, Task::Smart, 80,
			u"^llama-3\\.3-70b-versatile$"_q, u"llama-3.3-70b-versatile"_q },
		{ u"nvidia"_q, Task::Smart, 78,
			u"^nvidia/llama-3\\.1-nemotron-51b-instruct"_q,
			u"nvidia/llama-3.1-nemotron-51b-instruct"_q },
		{ u"llm7"_q, Task::Smart, 72,
			u"^DeepSeek-V4-Flash"_q, u"DeepSeek-V4-Flash-0731"_q },
		{ u"llm7"_q, Task::Smart, 70,
			u"^GLM-5\\.3-Flash$"_q, u"GLM-5.3-Flash"_q },

		// Bulk: fast, with the biggest free limits.
		{ u"gemini"_q, Task::Bulk, 95,
			u"^gemini-flash-lite-latest$"_q, u"gemini-flash-lite-latest"_q },
		{ u"gemini"_q, Task::Bulk, 94,
			u"^gemini-[\\d.]+-flash-lite$"_q, u"gemini-2.5-flash-lite"_q },
		{ u"groq"_q, Task::Bulk, 93,
			u"^openai/gpt-oss-20b$"_q, u"openai/gpt-oss-20b"_q },
		{ u"groq"_q, Task::Bulk, 92,
			u"^llama-3\\.1-8b-instant$"_q, u"llama-3.1-8b-instant"_q },
		{ u"groq"_q, Task::Bulk, 88,
			u"^llama-3\\.3-70b-versatile$"_q, u"llama-3.3-70b-versatile"_q },
		{ u"gemini"_q, Task::Bulk, 86,
			u"^gemini-[\\d.]+-flash$"_q, u"gemini-2.5-flash"_q },
		{ u"nvidia"_q, Task::Bulk, 85,
			u"^nvidia/llama-3\\.1-nemotron-70b-instruct"_q,
			u"nvidia/llama-3.1-nemotron-70b-instruct"_q },
		{ u"nvidia"_q, Task::Bulk, 82,
			u"^mistralai/mistral-7b-instruct-v0\\.3"_q,
			u"mistralai/mistral-7b-instruct-v0.3"_q },
		{ u"nvidia"_q, Task::Bulk, 80,
			u"^openai/gpt-oss-20b"_q,
			u"openai/gpt-oss-20b"_q },
		{ u"nvidia"_q, Task::Bulk, 78,
			u"^nvidia/llama-3\\.1-nemotron-51b-instruct"_q,
			u"nvidia/llama-3.1-nemotron-51b-instruct"_q },
		{ u"llm7"_q, Task::Bulk, 66,
			u"^gpt-oss:20b$"_q, u"gpt-oss:20b"_q },

		// Vision: models that read pictures.
		{ u"gemini"_q, Task::Vision, 95,
			u"^gemini-flash-latest$"_q, u"gemini-flash-latest"_q },
		{ u"gemini"_q, Task::Vision, 94,
			u"^gemini-[\\d.]+-flash$"_q, u"gemini-2.5-flash"_q },
		{ u"gemini"_q, Task::Vision, 88,
			u"^gemini-[\\d.]+-flash-lite$"_q, u"gemini-2.5-flash-lite"_q },
		{ u"groq"_q, Task::Vision, 84,
			u"^meta-llama/llama-4-maverick"_q,
			u"meta-llama/llama-4-maverick-17b-128e-instruct"_q },
		{ u"groq"_q, Task::Vision, 83,
			u"^meta-llama/llama-4-scout"_q,
			u"meta-llama/llama-4-scout-17b-16e-instruct"_q },
		{ u"nvidia"_q, Task::Vision, 72,
			u"^meta/llama-3\\.2-11b-vision-instruct"_q,
			u"meta/llama-3.2-11b-vision-instruct"_q },
		{ u"nvidia"_q, Task::Vision, 70,
			u"^meta/llama-3\\.2-90b-vision-instruct"_q,
			u"meta/llama-3.2-90b-vision-instruct"_q },
	};
	return result;
}

// Where OpenRouter's free models stand among the others, per job.
[[nodiscard]] inline int OpenRouterBase(Task task) {
	switch (task) {
	case Task::Smart: return 85;
	case Task::Bulk: return 70;
	case Task::Vision: return 75;
	}
	return 70;
}

// ---- OpenRouter's free models: the list is downloaded once a day ----

struct FreeModel {
	QString id;
	bool vision = false;
	bool tools = false;
	int context = 0;
};

namespace details {

struct FreeList {
	std::vector<FreeModel> models;
	qint64 at = 0; // when downloaded, ms since the epoch
	bool loaded = false;
	bool fetching = false;
	std::vector<Fn<void(bool)>> waiting;
};

[[nodiscard]] inline QString FreeListPath() {
	return Core::GgtgPaths::DbDir() + u"ai_openrouter_free.json"_q;
}

// How clever a free model is likely to be, by its name: the family and the
// size (in billions of parameters) it states.
[[nodiscard]] inline int OpenRouterRank(const QString &id) {
	static const auto size = QRegularExpression(
		u"(\\d+(?:\\.\\d+)?)b(?![a-z])"_q,
		QRegularExpression::CaseInsensitiveOption);
	auto biggest = 0.;
	auto it = size.globalMatch(id);
	while (it.hasNext()) {
		biggest = std::max(biggest, it.next().captured(1).toDouble());
	}
	auto result = int(std::min(biggest, 700.) / 10.);
	static const auto families = std::vector<std::pair<QString, int>>{
		{ u"deepseek"_q, 40 },
		{ u"kimi"_q, 38 },
		{ u"qwen3"_q, 30 },
		{ u"gpt-oss"_q, 30 },
		{ u"glm"_q, 28 },
		{ u"llama-4"_q, 25 },
		{ u"llama-3.3"_q, 22 },
		{ u"nemotron"_q, 22 },
		{ u"qwen"_q, 20 },
		{ u"gemma"_q, 15 },
	};
	for (const auto &[name, bonus] : families) {
		if (id.contains(name, Qt::CaseInsensitive)) {
			result += bonus;
			break;
		}
	}
	return result;
}

[[nodiscard]] inline FreeList &OpenRouterFreeState() {
	static auto result = FreeList();
	if (!std::exchange(result.loaded, true)) {
		auto file = QFile(FreeListPath());
		if (file.open(QIODevice::ReadOnly)) {
			const auto object = QJsonDocument::fromJson(
				file.readAll()).object();
			result.at = qint64(object.value("at").toDouble());
			for (const auto &value : object.value("models").toArray()) {
				const auto entry = value.toObject();
				result.models.push_back({
					.id = entry.value("id").toString(),
					.vision = entry.value("vision").toBool(),
					.tools = entry.value("tools").toBool(),
					.context = entry.value("context").toInt(),
				});
			}
		}
	}
	return result;
}

} // namespace details

// The free models OpenRouter has, the likely cleverest first.
[[nodiscard]] inline const std::vector<FreeModel> &OpenRouterFree() {
	return details::OpenRouterFreeState().models;
}

// Downloads the list again when it is older than a day (or `force`).
// `done(ok)` may be null.
inline void RefreshOpenRouterFree(bool force, Fn<void(bool ok)> done) {
	auto &state = details::OpenRouterFreeState();
	const auto now = QDateTime::currentMSecsSinceEpoch();
	if (!force
		&& !state.models.empty()
		&& now - state.at < 24 * 60 * 60 * 1000) {
		if (done) {
			done(true);
		}
		return;
	}
	if (done) {
		state.waiting.push_back(std::move(done));
	}
	if (std::exchange(state.fetching, true)) {
		return;
	}
	auto request = QNetworkRequest(
		QUrl(u"https://openrouter.ai/api/v1/models"_q));
	request.setTransferTimeout(30'000);
	const auto reply = details::Manager().get(request);
	QObject::connect(reply, &QNetworkReply::finished, reply, [=] {
		const auto data = reply->readAll();
		const auto failed = (reply->error() != QNetworkReply::NoError);
		reply->deleteLater();
		auto &state = details::OpenRouterFreeState();
		state.fetching = false;
		auto models = std::vector<FreeModel>();
		if (!failed) {
			static const auto bad = QRegularExpression(
				u"embed|guard|tts|whisper|audio|image-gen|moderation|"
				"rerank|^openrouter/"_q,
				QRegularExpression::CaseInsensitiveOption);
			const auto rows = QJsonDocument::fromJson(data).object()
				.value("data").toArray();
			for (const auto &value : rows) {
				const auto row = value.toObject();
				const auto id = row.value("id").toString();
				const auto pricing = row.value("pricing").toObject();
				const auto free = id.endsWith(u":free"_q)
					|| (pricing.value("prompt").toString() == u"0"_q
						&& pricing.value("completion").toString()
							== u"0"_q);
				const auto architecture = row.value("architecture")
					.toObject();
				const auto outputs = architecture.value(
					"output_modalities").toArray();
				if (id.isEmpty()
					|| !free
					|| bad.match(id).hasMatch()
					|| (!outputs.isEmpty()
						&& !outputs.contains(QJsonValue(u"text"_q)))) {
					continue;
				}
				models.push_back({
					.id = id,
					.vision = architecture.value("input_modalities")
						.toArray().contains(QJsonValue(u"image"_q)),
					.tools = row.value("supported_parameters").toArray()
						.contains(QJsonValue(u"tools"_q)),
					.context = row.value("context_length").toInt(),
				});
			}
			std::stable_sort(models.begin(), models.end(), [](
					const FreeModel &a,
					const FreeModel &b) {
				return details::OpenRouterRank(a.id)
					> details::OpenRouterRank(b.id);
			});
		}
		const auto ok = !models.empty();
		if (ok) {
			state.models = std::move(models);
			state.at = QDateTime::currentMSecsSinceEpoch();
			auto array = QJsonArray();
			for (const auto &model : state.models) {
				array.push_back(QJsonObject{
					{ "id", model.id },
					{ "vision", model.vision },
					{ "tools", model.tools },
					{ "context", model.context },
				});
			}
			auto file = QSaveFile(details::FreeListPath());
			if (file.open(QIODevice::WriteOnly)) {
				file.write(QJsonDocument(QJsonObject{
					{ "at", double(state.at) },
					{ "models", array },
				}).toJson(QJsonDocument::Compact));
				file.commit();
			}
		}
		auto callbacks = std::move(state.waiting);
		state.waiting.clear();
		for (const auto &callback : callbacks) {
			callback(ok || !state.models.empty());
		}
	});
}

namespace details {

// ---- each key's own list of models (asked once per run) ----

struct KeyModels {
	QStringList ids;
	bool known = false;
	bool fetching = false;
	qint64 tried = 0; // the last try, ms since the epoch
	std::vector<Fn<void(int)>> waiting;
};

[[nodiscard]] inline std::map<QString, KeyModels> &ModelLists() {
	static auto result = std::map<QString, KeyModels>();
	return result;
}

} // namespace details

// Asks the key's provider which models it has (OpenRouter: its free list).
// `done(status)`: the HTTP status, 200 when the list came, 0 without an
// answer; may be null.
inline void FetchModels(const CloudKey &key, Fn<void(int status)> done) {
	auto &entry = details::ModelLists()[key.key];
	if (done) {
		entry.waiting.push_back(std::move(done));
	}
	if (std::exchange(entry.fetching, true)) {
		return;
	}
	entry.tried = QDateTime::currentMSecsSinceEpoch();
	const auto finish = [=](QStringList ids, bool known, int status) {
		auto &entry = details::ModelLists()[key.key];
		entry.fetching = false;
		if (known) {
			entry.ids = std::move(ids);
			entry.known = true;
		}
		auto callbacks = std::move(entry.waiting);
		entry.waiting.clear();
		for (const auto &callback : callbacks) {
			callback(status);
		}
	};
	if (key.provider == u"openrouter"_q) {
		RefreshOpenRouterFree(false, [=](bool ok) {
			auto ids = QStringList();
			for (const auto &model : OpenRouterFree()) {
				ids.push_back(model.id);
			}
			finish(ids, ok, ok ? 200 : 0);
		});
		return;
	}
	const auto provider = CloudProviderById(key.provider);
	if (!provider) {
		finish(QStringList(), false, 0);
		return;
	}
	auto request = QNetworkRequest(QUrl((key.provider == u"github"_q)
		? u"https://models.github.ai/catalog/models"_q
		: (provider->baseUrl + u"/models"_q)));
	request.setRawHeader(
		"Authorization",
		("Bearer " + key.key.trimmed()).toUtf8());
	request.setTransferTimeout(30'000);
	const auto reply = details::Manager().get(request);
	QObject::connect(reply, &QNetworkReply::finished, reply, [=] {
		const auto data = reply->readAll();
		const auto failed = (reply->error() != QNetworkReply::NoError);
		const auto status = reply->attribute(
			QNetworkRequest::HttpStatusCodeAttribute).toInt();
		reply->deleteLater();
		if (failed) {
			finish(QStringList(), false, status);
			return;
		}
		const auto document = QJsonDocument::fromJson(data);
		const auto array = document.isArray()
			? document.array()
			: document.object().contains("data")
			? document.object().value("data").toArray()
			: document.object().value("models").toArray();
		auto ids = QStringList();
		for (const auto &item : array) {
			auto id = item.toObject().value("id").toString();
			if (id.isEmpty()) {
				id = item.toObject().value("name").toString();
			}
			if (id.startsWith(u"models/"_q)) {
				id = id.mid(7);
			}
			if (!id.isEmpty()) {
				ids.push_back(id);
			}
		}
		finish(ids, !ids.isEmpty(), status ? status : 200);
	});
}

namespace details {

// ---- resting: a model (or a whole key) that failed waits a while ----

// Until when (ms since the epoch); the key "<key>\n<model>", or "<key>\n"
// for the whole key. Not saved: a new run starts fresh.
[[nodiscard]] inline std::map<QString, qint64> &Rests() {
	static auto result = std::map<QString, qint64>();
	return result;
}

[[nodiscard]] inline qint64 RestUntil(
		const QString &key,
		const QString &model) {
	const auto i = Rests().find(key + '\n' + model);
	return (i == Rests().end()) ? 0 : i->second;
}

[[nodiscard]] inline bool Resting(const QString &key, const QString &model) {
	const auto now = QDateTime::currentMSecsSinceEpoch();
	return RestUntil(key, QString()) > now
		|| (!model.isEmpty() && RestUntil(key, model) > now);
}

inline void Rest(const QString &key, const QString &model, int seconds) {
	Rests()[key + '\n' + model] = QDateTime::currentMSecsSinceEpoch()
		+ qint64(seconds) * 1000;
}

// Everything of the key rests no more (a manual check found it working).
inline void Wake(const QString &key) {
	auto &rests = Rests();
	for (auto i = rests.begin(); i != rests.end();) {
		if (i->first.startsWith(key + '\n')) {
			i = rests.erase(i);
		} else {
			++i;
		}
	}
}

// One model of one key for a job.
struct Candidate {
	CloudKey key;
	QString model;
	int score = 0;
	int order = 0; // the key's place in the list
};

// The newest of `ids` that matches `pattern`.
[[nodiscard]] inline QString Newest(
		const QStringList &ids,
		const QString &pattern) {
	const auto re = QRegularExpression(pattern);
	auto found = QStringList();
	for (const auto &id : ids) {
		if (re.match(id).hasMatch()) {
			found.push_back(id);
		}
	}
	std::sort(found.begin(), found.end(), std::greater<>());
	return found.isEmpty() ? QString() : found.front();
}

// Every model of every key that can take the job now, the best first.
[[nodiscard]] inline std::vector<Candidate> Candidates(
		const Settings &settings,
		Task task,
		bool tools) {
	auto result = std::vector<Candidate>();
	const auto add = [&](Candidate candidate) {
		if (Resting(candidate.key.key, candidate.model)) {
			return;
		}
		for (auto &existing : result) {
			if (existing.key.key == candidate.key.key
				&& existing.model == candidate.model) {
				existing.score = std::max(existing.score, candidate.score);
				return;
			}
		}
		result.push_back(std::move(candidate));
	};
	for (auto order = 0; order != int(settings.keys.size()); ++order) {
		const auto &key = settings.keys[order];
		if (!CloudKeyUsable(key) || Resting(key.key, QString())) {
			continue;
		}
		if (key.provider == u"openrouter"_q) {
			auto place = 0;
			for (const auto &model : OpenRouterFree()) {
				if ((task == Task::Vision && !model.vision)
					|| (tools && !model.tools)) {
					continue;
				}
				add({ key, model.id, OpenRouterBase(task) * 100 - place++,
					order });
			}
			continue;
		}
		const auto i = ModelLists().find(key.key);
		const auto known = (i != ModelLists().end()) && i->second.known;
		for (const auto &choice : ModelChoices()) {
			if (choice.provider != key.provider || choice.task != task) {
				continue;
			}
			auto model = known
				? Newest(i->second.ids, choice.pattern)
				: choice.fallback;
			if (model.isEmpty() && !choice.fallback.isEmpty()) {
				model = choice.fallback;
			}
			if (!model.isEmpty()) {
				add({ key, model, choice.score * 100, order });
			}
		}
		if (result.empty() && known && !i->second.ids.isEmpty()) {
			for (const auto &id : i->second.ids) {
				if (id.contains(u"nemotron"_q)
					|| id.contains(u"mistral-7b"_q)
					|| id.contains(u"gpt-oss"_q)) {
					add({ key, id, 5000, order });
					break;
				}
			}
		}
	}
	std::stable_sort(result.begin(), result.end(), [](
			const Candidate &a,
			const Candidate &b) {
		return (a.score != b.score) ? (a.score > b.score) : (a.order < b.order);
	});
	return result;
}

// Starts what is worth knowing before the next request: the lists of models
// of the keys that have none yet, the day's list of OpenRouter's free ones.
inline void Warm(const Settings &settings) {
	const auto now = QDateTime::currentMSecsSinceEpoch();
	for (const auto &key : settings.keys) {
		if (!CloudKeyUsable(key)) {
			continue;
		}
		const auto &entry = ModelLists()[key.key];
		if (!entry.known
			&& !entry.fetching
			&& now - entry.tried > 10 * 60 * 1000) {
			FetchModels(key, nullptr);
		} else if (key.provider == u"openrouter"_q) {
			RefreshOpenRouterFree(false, nullptr);
		}
	}
}

// Gemini says "API key not valid" with a plain 400.
[[nodiscard]] inline bool BadKeyText(const QString &lower) {
	return lower.contains(u"api key not valid"_q)
		|| lower.contains(u"api_key_invalid"_q)
		|| lower.contains(u"invalid api key"_q);
}

// A request to `candidate` failed with `status` (0: no answer): the model,
// or the whole key, rests for a while that suits the failure.
inline void Failed(
		const Candidate &candidate,
		int status,
		const QByteArray &body,
		const QByteArray &retryAfter) {
	const auto &key = candidate.key.key;
	const auto text = QString::fromUtf8(body).toLower();
	if (status == 401 || BadKeyText(text)) {
		Rest(key, QString(), 10 * 60 * 60); // A wrong key.
	} else if (status == 403 && !text.contains(u"model"_q)) {
		Rest(key, QString(), 30 * 60); // Blocked: the address, the rights.
	} else if (status == 403 || status == 404) {
		// This key has no such model: ask for its list again.
		Rest(key, candidate.model, 24 * 60 * 60);
		ModelLists()[key].known = false;
		ModelLists()[key].tried = 0;
	} else if (status == 429) {
		// "limit: 0": this model is not free on this key at all.
		const auto daily = text.contains(u"per day"_q)
			|| text.contains(u"perday"_q)
			|| text.contains(u"daily"_q)
			|| text.contains(u"per-day"_q)
			|| text.contains(u"limit: 0"_q);
		if (daily && candidate.key.provider == u"openrouter"_q) {
			// The day's free requests of the whole account are spent.
			Rest(key, QString(), 6 * 60 * 60);
		} else if (daily) {
			Rest(key, candidate.model, 6 * 60 * 60);
		} else {
			const auto seconds = retryAfter.trimmed().toInt();
			Rest(
				key,
				candidate.model,
				seconds > 0 ? std::clamp(seconds, 20, 3600) : 5 * 60);
		}
	} else if (status == 400 || status == 413 || status == 422) {
		// The model does not take this request (too long, no pictures...).
		Rest(key, candidate.model, 10 * 60);
	} else {
		Rest(key, candidate.model, 60);
	}
}

// What went wrong, in plain words.
[[nodiscard]] inline QString FailureText(
		int status,
		const QByteArray &body,
		const QString &fallback) {
	const auto lower = QString::fromUtf8(body).toLower();
	const auto object = QJsonDocument::fromJson(body).object();
	auto message = object.value("error").isObject()
		? object.value("error").toObject().value("message").toString()
		: object.value("message").toString();
	if (message.isEmpty() && !body.trimmed().startsWith('<')) {
		message = QString::fromUtf8(body).simplified();
	}
	message = message.simplified().left(200);
	const auto region = lower.contains(u"location"_q)
		|| lower.contains(u"region"_q)
		|| lower.contains(u"country"_q)
		|| lower.contains(u"not supported"_q)
		|| lower.contains(u"cloudflare"_q)
		|| lower.contains(u"<html"_q);
	const auto tail = message.isEmpty()
		? QString()
		: (u" Ответ сервиса: "_q + message);
	if (status == 401 || BadKeyText(lower)) {
		return u"ключ не подошёл (401): скопируйте его заново целиком, "
			"без пробелов, или создайте новый."_q + tail;
	} else if (status == 403) {
		return u"доступ запрещён (403): ключ без нужных прав или сервис не "
			"пускает с вашего адреса (часто это ограничение страны: "
			"включите VPN)."_q + tail;
	} else if (status == 400 && region) {
		return u"сервис не работает из вашей страны или с вашего адреса "
			"(включите VPN)."_q + tail;
	} else if (status == 404) {
		return u"такой модели нет у сервиса"_q;
	} else if (status == 429) {
		return u"бесплатный лимит исчерпан"_q;
	} else if (status == 0) {
		return u"нет соединения с интернетом или сервис недоступен"_q;
	}
	return message.isEmpty()
		? (fallback + u" (HTTP "_q + QString::number(status) + ')')
		: message;
}

// "через 3 мин" until the first resting key or model of the usable keys
// is back; empty when nothing rests.
[[nodiscard]] inline QString SoonestBack(const Settings &settings) {
	const auto now = QDateTime::currentMSecsSinceEpoch();
	auto soonest = qint64(0);
	for (const auto &[name, until] : Rests()) {
		if (until <= now) {
			continue;
		}
		for (const auto &key : settings.keys) {
			if (name.startsWith(key.key + '\n')
				&& (!soonest || until < soonest)) {
				soonest = until;
			}
		}
	}
	if (!soonest) {
		return QString();
	}
	const auto minutes = (soonest - now + 59'999) / 60'000;
	return (minutes >= 60)
		? u"через %1 ч"_q.arg((minutes + 59) / 60)
		: u"через %1 мин"_q.arg(minutes);
}

// ---- several requests at a time, the rest wait their turn ----

inline constexpr auto kParallel = 3;

struct Slot {
	bool busy = false; // a request is on its way
	int active = 0;
	std::deque<Fn<void()>> queue;
};

[[nodiscard]] inline Slot &SlotState() {
	static auto result = Slot();
	return result;
}

inline void Release() {
	auto &slot = SlotState();
	slot.active = std::max(slot.active - 1, 0);
	if (!slot.queue.empty() && slot.active < kParallel) {
		const auto next = std::move(slot.queue.front());
		slot.queue.pop_front();
		++slot.active;
		slot.busy = true;
		next();
		return;
	}
	slot.busy = (slot.active > 0);
}

inline void Enter(Fn<void()> start) {
	auto &slot = SlotState();
	if (slot.active >= kParallel) {
		slot.queue.push_back(std::move(start));
		return;
	}
	++slot.active;
	slot.busy = true;
	start();
}

} // namespace details

// "Остановить": the requests that wait for their turn are dropped, so
// nothing more goes to the services; those already sent end by themselves
// within seconds. Returns how many were dropped.
inline int StopQueued() {
	auto &slot = details::SlotState();
	const auto dropped = int(slot.queue.size());
	slot.queue.clear();
	return dropped;
}

namespace details {

// ---- privacy: other people's data leaves as marks ----

// The names and @usernames GGTG has seen while collecting bases
// (names.json): words of those names and the usernames, lower case. Read
// again when the file changes, at most once a minute.
struct People {
	QSet<QString> nameWords;
	QSet<QString> usernames;
	QDateTime modified;
	qint64 checked = 0;
};

[[nodiscard]] inline const People &KnownPeople() {
	static auto result = People();
	const auto now = QDateTime::currentMSecsSinceEpoch();
	if (now - result.checked < 60'000) {
		return result;
	}
	result.checked = now;
	const auto path = Core::GgtgPaths::DbDir() + u"names.json"_q;
	const auto modified = QFileInfo(path).lastModified();
	if (modified == result.modified) {
		return result;
	}
	result.modified = modified;
	result.nameWords.clear();
	result.usernames.clear();
	auto file = QFile(path);
	if (!file.open(QIODevice::ReadOnly)) {
		return result;
	}
	static const auto word = QRegularExpression(u"\\p{L}{3,}"_q);
	const auto object = QJsonDocument::fromJson(file.readAll()).object();
	for (auto i = object.begin(); i != object.end(); ++i) {
		if (i.key().size() >= 5) {
			result.usernames.insert(i.key().toLower());
		}
		auto it = word.globalMatch(i.value().toString());
		while (it.hasNext()) {
			result.nameWords.insert(it.next().captured(0).toLower());
		}
	}
	return result;
}

struct Masked {
	QString text;
	std::vector<std::pair<QString, QString>> marks; // mark, original
};

// Replaces mails, links, @usernames, phone numbers and known names and
// usernames with marks like <N1>, <P2>; Unmask puts them back.
[[nodiscard]] inline Masked Mask(const QString &text) {
	auto result = Masked{ .text = text };
	auto counters = std::map<QChar, int>();
	auto same = std::map<QString, QString>();
	const auto mark = [&](const QString &value, QChar kind) {
		if (const auto i = same.find(value); i != same.end()) {
			return i->second;
		}
		const auto name = u"<%1%2>"_q.arg(kind).arg(++counters[kind]);
		same.emplace(value, name);
		result.marks.push_back({ name, value });
		return name;
	};
	const auto replaceAll = [&](
			const QRegularExpression &re,
			QChar kind,
			Fn<bool(const QString&)> accept) {
		auto updated = QString();
		auto last = 0;
		auto it = re.globalMatch(result.text);
		while (it.hasNext()) {
			const auto match = it.next();
			const auto value = match.captured(0);
			if (accept && !accept(value)) {
				continue;
			}
			updated += result.text.mid(last, match.capturedStart() - last);
			updated += mark(value, kind);
			last = match.capturedEnd();
		}
		if (last) {
			result.text = updated + result.text.mid(last);
		}
	};
	static const auto mail = QRegularExpression(
		u"[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\\.[A-Za-z]{2,}"_q);
	static const auto link = QRegularExpression(
		u"(?:https?://|www\\.|t\\.me/|telegram\\.me/)\\S+"_q,
		QRegularExpression::CaseInsensitiveOption);
	static const auto username = QRegularExpression(
		u"@[A-Za-z][A-Za-z0-9_]{3,31}"_q);
	static const auto phone = QRegularExpression(
		u"(?<![\\w+])\\+?\\d[\\d\\s().-]{5,}\\d(?!\\w)"_q);
	replaceAll(mail, 'E', nullptr);
	replaceAll(link, 'L', nullptr);
	replaceAll(username, 'U', nullptr);
	replaceAll(phone, 'P', [](const QString &value) {
		// "+7 916 ..." or a long run of digits; counts and dates stay.
		auto digits = 0;
		for (const auto ch : value) {
			digits += ch.isDigit() ? 1 : 0;
		}
		return value.startsWith('+') ? (digits >= 7) : (digits >= 10);
	});
	const auto &people = KnownPeople();
	if (!people.nameWords.isEmpty() || !people.usernames.isEmpty()) {
		static const auto word = QRegularExpression(
			u"(?<![\\w<])[\\p{L}][\\p{L}\\p{M}0-9_'’-]{2,}"_q);
		replaceAll(word, 'N', [&](const QString &value) {
			const auto lower = value.toLower();
			return (value[0].isUpper() && people.nameWords.contains(lower))
				|| people.usernames.contains(lower);
		});
	}
	return result;
}

// The marks left in a text (when it must not carry a real name or number
// into what is kept): "<N1>" becomes "[имя]" and so on.
[[nodiscard]] inline QString PlainMarks(QString text) {
	static const auto re = QRegularExpression(u"<([NPUEL])\\d+>"_q);
	auto result = QString();
	auto last = 0;
	auto it = re.globalMatch(text);
	while (it.hasNext()) {
		const auto match = it.next();
		const auto kind = match.captured(1);
		result += text.mid(last, match.capturedStart() - last);
		result += (kind == u"N"_q)
			? u"[имя]"_q
			: (kind == u"P"_q)
			? u"[номер]"_q
			: (kind == u"U"_q)
			? u"[@юзернейм]"_q
			: (kind == u"E"_q)
			? u"[почта]"_q
			: u"[ссылка]"_q;
		last = match.capturedEnd();
	}
	return result + text.mid(last);
}

[[nodiscard]] inline QString Unmask(QString text, const Masked &masked) {
	// The longest marks first: <N12> must not be taken for <N1>.
	auto marks = masked.marks;
	std::sort(marks.begin(), marks.end(), [](
			const auto &a,
			const auto &b) {
		return a.first.size() > b.first.size();
	});
	for (const auto &[name, original] : marks) {
		text.replace(name, original);
	}
	return text;
}

// One line per question in ai_log.jsonl (the role, whether it worked, how
// long it took, which model): the base for judging a model or a prompt by
// numbers, not by feeling. The file is dropped when it grows past two
// megabytes.
inline void LogCall(Role role, bool ok, qint64 millis, const QString &note) {
	const auto path = Core::GgtgPaths::DbDir() + u"ai_log.jsonl"_q;
	auto file = QFile(path);
	if (file.size() > 2 * 1024 * 1024) {
		file.remove();
	}
	if (!file.open(QIODevice::Append)) {
		return;
	}
	file.write(QJsonDocument(QJsonObject{
		{ "at", double(QDateTime::currentSecsSinceEpoch()) },
		{ "role", role == Role::Translator ? u"translator"_q : u"general"_q },
		{ "ok", ok },
		{ "ms", double(millis) },
		{ "note", note.left(160) },
	}).toJson(QJsonDocument::Compact) + '\n');
}

// How many questions went to the models today (read from ai_log.jsonl, at
// most once in 10 seconds): what the free limits were spent on.
struct CallsToday {
	int ok = 0;
	int failed = 0;
};
[[nodiscard]] inline CallsToday CountCallsToday() {
	static auto cached = CallsToday();
	static auto checked = qint64(0);
	const auto now = QDateTime::currentMSecsSinceEpoch();
	if (now - checked < 10'000) {
		return cached;
	}
	checked = now;
	cached = CallsToday();
	auto file = QFile(Core::GgtgPaths::DbDir() + u"ai_log.jsonl"_q);
	if (!file.open(QIODevice::ReadOnly)) {
		return cached;
	}
	const auto from = QDateTime(QDate::currentDate(), QTime(0, 0)).toSecsSinceEpoch();
	for (const auto &line : file.readAll().split('\n')) {
		if (line.isEmpty()) {
			continue;
		}
		const auto object = QJsonDocument::fromJson(line).object();
		if (qint64(object.value("at").toDouble()) < from) {
			continue;
		}
		if (object.value("ok").toBool()) {
			++cached.ok;
		} else {
			++cached.failed;
		}
	}
	return cached;
}

// "Thinking" models put their reasoning in <think>...</think> before the
// answer; only the answer is wanted.
[[nodiscard]] inline QString StripThinking(QString text) {
	static const auto re = QRegularExpression(
		u"<think>.*?</think>"_q,
		QRegularExpression::DotMatchesEverythingOption);
	text.remove(re);
	return text.trimmed();
}

// Qwen writes a tool call as <tool_call>{...}</tool_call> in its text when
// the server did not take it apart itself.
[[nodiscard]] inline QJsonArray ToolCallsFromText(QString &content) {
	static const auto re = QRegularExpression(
		u"<tool_call>\\s*(\\{.*?\\})\\s*</tool_call>"_q,
		QRegularExpression::DotMatchesEverythingOption);
	auto result = QJsonArray();
	auto i = re.globalMatch(content);
	while (i.hasNext()) {
		const auto match = i.next();
		const auto call = QJsonDocument::fromJson(
			match.captured(1).toUtf8()).object();
		const auto name = call.value("name").toString();
		if (name.isEmpty()) {
			continue;
		}
		result.push_back(QJsonObject{
			{ "id", u"call_"_q + QString::number(result.size() + 1) },
			{ "type", "function" },
			{ "function", QJsonObject{
				{ "name", name },
				{ "arguments", QString::fromUtf8(QJsonDocument(
					call.value("arguments").toObject()).toJson(
						QJsonDocument::Compact)) },
			} },
		});
	}
	if (!result.isEmpty()) {
		content.remove(re);
	}
	return result;
}

// What a model needs on top of the common request: room for the reasoning of
// the models that think before they answer, and as little of it as they
// allow (it is slow and not wanted here).
inline void Tune(QJsonObject &body, const Candidate &candidate) {
	const auto &model = candidate.model;
	const auto &provider = candidate.key.provider;
	auto tokens = std::max(body.value("max_tokens").toInt(800), 256);
	if (provider == u"gemini"_q) {
		if (model.startsWith(u"gemini-2.5-flash"_q)) {
			body.insert("reasoning_effort", "none");
		} else {
			body.insert("reasoning_effort", "low");
			tokens += 2048;
		}
	} else if (model.contains(u"gpt-oss"_q)
		&& (provider == u"groq"_q || provider == u"openrouter"_q)) {
		body.insert("reasoning_effort", "low");
		tokens += 1024;
	} else {
		static const auto thinker = QRegularExpression(
			u"deepseek-r1|-r1\\b|qwq|thinking|reason"_q,
			QRegularExpression::CaseInsensitiveOption);
		if (thinker.match(model).hasMatch()) {
			tokens += 1536;
		}
	}
	body.insert("max_tokens", tokens);
}

// Sends `body` (everything but "model") to the models that suit `task`, the
// best first, until one gives a usable answer: a text, or with `tools` also
// tool calls. `done(message, note, error)`: the model's message, which model
// it was ("gemini gemini-2.5-flash"), and a readable error when none
// answered.
inline void Send(
		Settings settings,
		Task task,
		bool tools,
		QJsonObject body,
		int timeout,
		Fn<void(QJsonObject message, QString note, QString error)> done,
		int attempt = 0,
		QString last = QString()) {
	constexpr auto kMaxAttempts = 8;
	if (!settings.enabled) {
		done({}, {}, u"ИИ выключен - включите его в разделе «ИИ»"_q);
		return;
	} else if (!CloudReady(settings)) {
		done({}, {}, u"нет ни одного ключа: добавьте бесплатный ключ в "
			"разделе «ИИ» → «Ключи»"_q);
		return;
	}
	Warm(settings);
	const auto candidates = Candidates(settings, task, tools);
	if (candidates.empty() || attempt >= kMaxAttempts) {
		const auto back = SoonestBack(settings);
		if (attempt > 0) {
			done({}, {}, (u"не ответила ни одна модель (последняя: "_q + last + ')')
				+ (back.isEmpty() ? QString() : (u"; освободится "_q + back)));
		} else {
			const auto anyResting = std::any_of(
				settings.keys.begin(),
				settings.keys.end(),
				[](const CloudKey &k) { return Resting(k.key, QString()); });
			if (anyResting) {
				done({}, {}, u"ключ временно на паузе после лимита запросов"_q
					+ (back.isEmpty() ? QString() : (u"; освободится "_q + back))
					+ u". Проверьте лимиты на сайте сервиса"_q);
			} else {
				done({}, {}, u"у провайдера не найдено доступных моделей. Проверьте правильность ключа или статус на сайте сервиса"_q);
			}
		}
		return;
	}
	const auto candidate = candidates.front();
	const auto provider = CloudProviderById(candidate.key.provider);
	auto tuned = body;
	tuned.insert("model", candidate.model);
	Tune(tuned, candidate);
	auto request = QNetworkRequest(
		QUrl(provider->baseUrl + u"/chat/completions"_q));
	request.setHeader(
		QNetworkRequest::ContentTypeHeader,
		u"application/json"_q);
	request.setRawHeader(
		"Authorization",
		("Bearer " + candidate.key.key.trimmed()).toUtf8());
	if (candidate.key.provider == u"openrouter"_q) {
		request.setRawHeader("X-Title", "GGTG");
	}
	request.setTransferTimeout(timeout);
	const auto reply = Manager().post(
		request,
		QJsonDocument(tuned).toJson(QJsonDocument::Compact));
	QObject::connect(reply, &QNetworkReply::finished, reply, [=] {
		const auto data = reply->readAll();
		const auto failed = (reply->error() != QNetworkReply::NoError);
		const auto text = reply->errorString();
		const auto status = reply->attribute(
			QNetworkRequest::HttpStatusCodeAttribute).toInt();
		const auto retryAfter = reply->rawHeader("Retry-After");
		reply->deleteLater();
		const auto note = candidate.key.provider + ' ' + candidate.model;
		const auto retry = [=](int code, const QString &why) {
			Failed(candidate, code, data, retryAfter);
			LogCall(
				Role::General,
				false,
				0,
				u"cloud failed: "_q + note + u": "_q + why);
			Send(settings, task, tools, body, timeout, done, attempt + 1,
				note + u": "_q + why);
		};
		if (failed) {
			retry(status, FailureText(status, data, text));
			return;
		}
		const auto choices = QJsonDocument::fromJson(data).object()
			.value("choices").toArray();
		const auto message = choices.isEmpty()
			? QJsonObject()
			: choices.at(0).toObject().value("message").toObject();
		auto content = StripThinking(message.value("content").toString());
		const auto calls = tools
			? (message.value("tool_calls").toArray().isEmpty()
				? ToolCallsFromText(content)
				: message.value("tool_calls").toArray())
			: QJsonArray();
		if (content.isEmpty() && calls.isEmpty()) {
			retry(0, u"пустой ответ"_q);
			return;
		}
		done(message, note, QString());
	});
}

} // namespace details

// One question to a model. `done(result, error)` runs on the main thread
// later - the caller guards it against what it touches. `error` is empty on
// success; it is already a readable Russian sentence otherwise. The job
// decides the models: with `images` the ones that see, with `hard` the
// cleverest (replies to people, teaching the AI), otherwise the fast ones -
// and then names, numbers and usernames in `user` leave as marks, which come
// back as the real ones in the answer - unless `keepMarks`: then the answer
// keeps them (see details::PlainMarks) and nothing real is carried into what
// the caller stores. `role` is kept for the log. `system` may be empty.
inline void Ask(
		const QString &system,
		const QString &user,
		Fn<void(QString result, QString error)> done,
		double temperature = 0.6,
		int maxTokens = 800,
		Role role = Role::General,
		QStringList images = QStringList(),
		bool hard = false,
		bool keepMarks = false) {
	const auto task = !images.isEmpty()
		? Task::Vision
		: hard
		? Task::Smart
		: Task::Bulk;
	details::Enter([=] {
		const auto started = QDateTime::currentMSecsSinceEpoch();
		const auto masked = (task == Task::Bulk)
			? details::Mask(user)
			: details::Masked{ .text = user };
		auto messages = QJsonArray();
		if (!system.isEmpty()) {
			messages.push_back(QJsonObject{
				{ "role", "system" },
				{ "content", system },
			});
		}
		if (images.isEmpty()) {
			messages.push_back(QJsonObject{
				{ "role", "user" },
				{ "content", masked.text },
			});
		} else {
			// Pictures go in as data addresses next to the text.
			auto parts = QJsonArray();
			for (const auto &image : images) {
				parts.push_back(QJsonObject{
					{ "type", "image_url" },
					{ "image_url", QJsonObject{ { "url", image } } },
				});
			}
			parts.push_back(QJsonObject{
				{ "type", "text" },
				{ "text", masked.text },
			});
			messages.push_back(QJsonObject{
				{ "role", "user" },
				{ "content", parts },
			});
		}
		details::Send(
			Load(),
			task,
			false,
			QJsonObject{
				{ "messages", messages },
				{ "temperature", temperature },
				{ "max_tokens", maxTokens },
				{ "stream", false },
			},
			120'000,
			[=](QJsonObject message, QString note, QString error) {
				details::Release();
				details::LogCall(
					role,
					error.isEmpty(),
					QDateTime::currentMSecsSinceEpoch() - started,
					u"cloud "_q + (error.isEmpty() ? note : error));
				auto result = details::StripThinking(
					message.value("content").toString());
				result = keepMarks
					? details::PlainMarks(result)
					: details::Unmask(result, masked);
				// A long dash (em dash) gives away a machine-written text:
				// never let it through, whatever the model did. The ordinary
				// short dash is fine.
				result.replace(QChar(0x2014), QChar(0x2013));
				done(result, error);
			});
	});
}

// The "ИИ-агент" (see settings/ggtg_ai_agent.h) needs a clever model that
// can call tools: any key gives one. Empty - the agent can run.
[[nodiscard]] inline QString AgentUnavailableReason() {
	const auto settings = Load();
	if (!settings.enabled) {
		return u"ИИ выключен - включите его в разделе «ИИ»"_q;
	} else if (!CloudReady(settings)) {
		return u"Для ИИ-агента нужен бесплатный ключ: раздел «ИИ» → "
			"«Ключи»"_q;
	}
	return QString();
}

// One step of a conversation with tools: the whole `messages` list goes in,
// the model's message comes back - a text answer in "content", or the tools
// it wants called in "tool_calls" (OpenAI format). Without `tools` the model
// has to answer in words. Always a job for the cleverest models; `think`
// gives them more room to reason.
inline void AskWithTools(
		QJsonArray messages,
		QJsonArray tools,
		Fn<void(QJsonObject message, QString error)> done,
		double temperature = 0.4,
		int maxTokens = 1400,
		bool think = false,
		bool allowCloud = true) {
	(void)allowCloud; // Everything is in the cloud now.
	details::Enter([=] {
		auto body = QJsonObject{
			{ "messages", messages },
			{ "temperature", temperature },
			{ "max_tokens", think ? std::max(maxTokens, 4000) : maxTokens },
			{ "stream", false },
		};
		if (!tools.isEmpty()) {
			body.insert("tools", tools);
		}
		details::Send(
			Load(),
			Task::Smart,
			!tools.isEmpty(),
			body,
			think ? 600'000 : 300'000,
			[=](QJsonObject message, QString note, QString error) {
				details::Release();
				details::LogCall(
					Role::General,
					error.isEmpty(),
					0,
					u"agent "_q + (error.isEmpty() ? note : error));
				if (!error.isEmpty()) {
					done(QJsonObject(), error);
					return;
				}
				auto content = details::StripThinking(
					message.value("content").toString());
				auto calls = message.value("tool_calls").toArray();
				if (calls.isEmpty()) {
					calls = details::ToolCallsFromText(content);
					content = content.trimmed();
				}
				content.replace(QChar(0x2014), QChar(0x2013));
				auto result = QJsonObject{
					{ "role", "assistant" },
					{ "content", content },
				};
				if (!calls.isEmpty()) {
					result.insert("tool_calls", calls);
				}
				done(result, QString());
			});
	});
}

// "Проверить" next to a key: is it the provider's, does it answer, and what
// went wrong said in plain words. A key that works is back in the queue at
// once, even if it was resting.
inline void TestKey(
		const QString &key,
		Fn<void(bool ok, QString message)> done,
		const QString &hint = QString()) {
	auto entry = CloudKey{ key.trimmed(), DetectProvider(key) };
	if (entry.provider.isEmpty()) {
		if (!hint.isEmpty()) {
			entry.provider = hint;
		} else {
			// A key put into a service's list by hand keeps that service.
			for (const auto &saved : Load().keys) {
				if (saved.key == entry.key) {
					entry.provider = saved.provider;
				}
			}
		}
	}
	if (entry.key.isEmpty()) {
		done(false, u"Сначала вставьте ключ."_q);
		return;
	} else if (entry.provider.isEmpty()) {
		done(false, u"Не узнал ключ: ключи Gemini начинаются с AIza, Groq - "
			"с gsk_, OpenRouter - с sk-or-, NVIDIA - с nvapi-. "
			"Скопируйте ключ целиком."_q);
		return;
	}
	details::Wake(entry.key);
	const auto name = CloudProviderName(entry.provider);
	FetchModels(entry, [=](int status) {
		if (status == 401) {
			details::Rest(entry.key, QString(), 10 * 60 * 60);
			done(false, name + u": ключ не подошёл (401). Скопируйте его "
				"заново целиком, без пробелов, или создайте новый."_q);
			return;
		} else if (status == 403) {
			done(false, name + u": доступ отклонён (403 Forbidden). Проверьте подтверждение телефона и права на сайте сервиса."_q);
			return;
		} else if (status == 429) {
			done(false, name + u": превышен лимит запросов сервиса (429). Подождите пару минут или проверьте баланс кредитов."_q);
			return;
		}
		auto settings = Settings();
		settings.keys = { entry };
		details::Send(
			settings,
			Task::Bulk,
			false,
			QJsonObject{
				{ "messages", QJsonArray{ QJsonObject{
					{ "role", "user" },
					{ "content", "Ответь одним словом: готово" },
				} } },
				{ "max_tokens", 20 },
				{ "temperature", 0.0 },
				{ "stream", false },
			},
			30'000,
			[=](QJsonObject, QString note, QString error) {
				if (!error.isEmpty()) {
					done(false, name + u": "_q + error);
					return;
				}
				const auto i = details::ModelLists().find(entry.key);
				const auto count = (i == details::ModelLists().end())
					? 0
					: int(i->second.ids.size());
				done(true, name + u" работает, модель "_q
					+ note.section(' ', 1)
					+ (count
						? (u"; доступно моделей: "_q
							+ QString::number(count))
						: QString())
					+ '.');
			});
	});
}

// The things a draft must not invent: links, @names and numbers (prices,
// dates, phone numbers) that are in neither the conversation nor what the
// user told the assistant. Returns them, so the user is warned before
// sending; nothing is removed from the draft.
[[nodiscard]] inline QStringList UngroundedFacts(
		const QString &draft,
		const QString &known) {
	static const auto fact = QRegularExpression(
		u"https?://\\S+|www\\.\\S+|@[A-Za-z0-9_]{4,}|"
		"\\d[\\d\\s.,]*\\d|\\d{2,}"_q);
	const auto haystack = known.toLower();
	auto compact = haystack;
	compact.remove(QRegularExpression(u"[\\s.,]"_q));
	auto result = QStringList();
	auto it = fact.globalMatch(draft);
	while (it.hasNext()) {
		auto value = it.next().captured(0).trimmed();
		while (value.endsWith('.') || value.endsWith(',')) {
			value.chop(1);
		}
		if (value.size() < 2) {
			continue;
		}
		auto plain = value.toLower();
		plain.remove(QRegularExpression(u"[\\s.,]"_q));
		if (haystack.contains(value.toLower()) || compact.contains(plain)) {
			continue;
		}
		if (!result.contains(value)) {
			result.push_back(value);
		}
	}
	return result;
}

// ---- translation: placeholders kept intact ----

struct Language {
	QString code;
	QString english;
	QString russian;
	QString chinese;
};

[[nodiscard]] inline const std::vector<Language> &Languages() {
	static const auto result = std::vector<Language>{
		{ u"en"_q, u"English"_q, u"английский"_q, u"英语"_q },
		{ u"ru"_q, u"Russian"_q, u"русский"_q, u"俄语"_q },
		{ u"ko"_q, u"Korean"_q, u"корейский"_q, u"韩语"_q },
		{ u"zh"_q, u"Chinese"_q, u"китайский"_q, u"中文"_q },
		{ u"ja"_q, u"Japanese"_q, u"японский"_q, u"日语"_q },
		{ u"es"_q, u"Spanish"_q, u"испанский"_q, u"西班牙语"_q },
		{ u"de"_q, u"German"_q, u"немецкий"_q, u"德语"_q },
		{ u"fr"_q, u"French"_q, u"французский"_q, u"法语"_q },
		{ u"pt"_q, u"Portuguese"_q, u"португальский"_q, u"葡萄牙语"_q },
		{ u"tr"_q, u"Turkish"_q, u"турецкий"_q, u"土耳其语"_q },
		{ u"ar"_q, u"Arabic"_q, u"арабский"_q, u"阿拉伯语"_q },
		{ u"uk"_q, u"Ukrainian"_q, u"украинский"_q, u"乌克兰语"_q },
		{ u"it"_q, u"Italian"_q, u"итальянский"_q, u"意大利语"_q },
		{ u"hi"_q, u"Hindi"_q, u"хинди"_q, u"印地语"_q },
		{ u"id"_q, u"Indonesian"_q, u"индонезийский"_q, u"印度尼西亚语"_q },
		{ u"vi"_q, u"Vietnamese"_q, u"вьетнамский"_q, u"越南语"_q },
		{ u"th"_q, u"Thai"_q, u"тайский"_q, u"泰语"_q },
		{ u"pl"_q, u"Polish"_q, u"польский"_q, u"波兰语"_q },
	};
	return result;
}

[[nodiscard]] inline const Language &LanguageByCode(const QString &code) {
	for (const auto &language : Languages()) {
		if (language.code == code) {
			return language;
		}
	}
	return Languages().front();
}

// The language of a text by its letters (the script is enough to tell a
// recipient's name or a template apart: Hangul, Cyrillic, Han...).
[[nodiscard]] inline QString DetectLanguage(const QString &text) {
	auto hangul = 0, cyrillic = 0, han = 0, kana = 0, arabic = 0, thai = 0;
	auto devanagari = 0, latin = 0, ukrainian = 0;
	for (const auto ch : text) {
		const auto code = ch.unicode();
		if ((code >= 0xAC00 && code <= 0xD7AF) || (code >= 0x1100 && code <= 0x11FF)) {
			++hangul;
		} else if (code >= 0x0400 && code <= 0x04FF) {
			++cyrillic;
			if (ch == QChar(0x0456) || ch == QChar(0x0457)
				|| ch == QChar(0x0454) || ch == QChar(0x0491)) {
				++ukrainian;
			}
		} else if (code >= 0x4E00 && code <= 0x9FFF) {
			++han;
		} else if (code >= 0x3040 && code <= 0x30FF) {
			++kana;
		} else if (code >= 0x0600 && code <= 0x06FF) {
			++arabic;
		} else if (code >= 0x0E00 && code <= 0x0E7F) {
			++thai;
		} else if (code >= 0x0900 && code <= 0x097F) {
			++devanagari;
		} else if (ch.isLetter() && code < 0x250) {
			++latin;
		}
	}
	const auto most = std::max({
		hangul, cyrillic, han, kana, arabic, thai, devanagari, latin });
	if (most == 0) {
		return QString();
	} else if (kana > 0 && kana * 4 >= han) {
		return u"ja"_q;
	} else if (most == hangul) {
		return u"ko"_q;
	} else if (most == cyrillic) {
		return (ukrainian > 0) ? u"uk"_q : u"ru"_q;
	} else if (most == han) {
		return u"zh"_q;
	} else if (most == arabic) {
		return u"ar"_q;
	} else if (most == thai) {
		return u"th"_q;
	} else if (most == devanagari) {
		return u"hi"_q;
	}
	return u"en"_q;
}

// Which Latin-script language a text is: the common words of English,
// Spanish, Turkish, French, German, Portuguese, Italian and Indonesian are
// counted; a text with no clear winner (a short "Super", "Ok") is English -
// by far the likeliest. Never empty.
[[nodiscard]] inline QString DetectLatinLanguage(const QString &text) {
	struct Words {
		QString code;
		QStringList list;
	};
	static const auto kWords = std::vector<Words>{
		{ u"en"_q, { u"the"_q, u"you"_q, u"and"_q, u"is"_q, u"are"_q,
			u"what"_q, u"how"_q, u"hello"_q, u"hi"_q, u"hey"_q, u"thanks"_q,
			u"please"_q, u"ok"_q, u"okay"_q, u"yes"_q, u"not"_q, u"this"_q,
			u"that"_q, u"for"_q, u"with"_q, u"can"_q, u"your"_q, u"have"_q,
			u"do"_q, u"i"_q, u"my"_q, u"me"_q, u"good"_q } },
		{ u"es"_q, { u"el"_q, u"la"_q, u"de"_q, u"que"_q, u"y"_q, u"en"_q,
			u"hola"_q, u"gracias"_q, u"por"_q, u"para"_q, u"con"_q,
			u"cómo"_q, u"como"_q, u"está"_q, u"estás"_q, u"muy"_q,
			u"bien"_q, u"sí"_q, u"no"_q, u"un"_q, u"una"_q, u"es"_q } },
		{ u"tr"_q, { u"merhaba"_q, u"selam"_q, u"evet"_q, u"hayır"_q,
			u"teşekkür"_q, u"tamam"_q, u"nasılsın"_q, u"bir"_q, u"bu"_q,
			u"ve"_q, u"ben"_q, u"sen"_q, u"için"_q, u"çok"_q, u"iyi"_q,
			u"güzel"_q, u"mi"_q, u"mı"_q } },
		{ u"fr"_q, { u"le"_q, u"les"_q, u"des"_q, u"est"_q, u"et"_q,
			u"bonjour"_q, u"salut"_q, u"merci"_q, u"oui"_q, u"non"_q,
			u"je"_q, u"tu"_q, u"vous"_q, u"pour"_q, u"avec"_q, u"très"_q,
			u"comment"_q } },
		{ u"de"_q, { u"der"_q, u"die"_q, u"das"_q, u"und"_q, u"ist"_q,
			u"nicht"_q, u"ich"_q, u"du"_q, u"hallo"_q, u"danke"_q,
			u"bitte"_q, u"ja"_q, u"nein"_q, u"wie"_q, u"mit"_q } },
		{ u"pt"_q, { u"olá"_q, u"obrigado"_q, u"obrigada"_q, u"você"_q,
			u"não"_q, u"sim"_q, u"uma"_q, u"para"_q, u"com"_q, u"muito"_q,
			u"está"_q, u"tudo"_q, u"bem"_q } },
		{ u"it"_q, { u"ciao"_q, u"grazie"_q, u"sono"_q, u"come"_q,
			u"stai"_q, u"che"_q, u"non"_q, u"per"_q, u"molto"_q, u"bene"_q,
			u"sì"_q, u"il"_q, u"della"_q } },
		{ u"id"_q, { u"halo"_q, u"terima"_q, u"kasih"_q, u"apa"_q,
			u"kabar"_q, u"saya"_q, u"kamu"_q, u"ya"_q, u"tidak"_q,
			u"yang"_q, u"dan"_q, u"untuk"_q, u"dengan"_q } },
	};
	static const auto split = QRegularExpression(u"[^\\p{L}]+"_q);
	const auto words = text.toLower().split(split, Qt::SkipEmptyParts);
	auto best = u"en"_q;
	auto bestScore = 0;
	for (const auto &entry : kWords) {
		auto score = 0;
		for (const auto &word : words) {
			score += entry.list.contains(word) ? 1 : 0;
		}
		// English wins ties: it is the usual one.
		if (score > bestScore) {
			best = entry.code;
			bestScore = score;
		}
	}
	return best;
}

// A name fit for "Привет, <name>!": letters only (emoji, symbols and digits
// of a profile name are dropped), and empty when what is left is clearly not
// a name - one letter, "qqqq", a keyboard run like "asdf", a word without a
// vowel, "user", "admin"...
[[nodiscard]] inline QString CleanPersonName(const QString &raw) {
	auto kept = QString();
	auto lower = QString();
	auto alphabetic = true; // Latin / Cyrillic only: these can be judged.
	auto lastWasLetter = false;
	for (const auto cp : raw.toUcs4()) {
		if (QChar::isLetter(cp)) {
			kept += QString::fromUcs4(&cp, 1);
			lower += QString::fromUcs4(&cp, 1).toLower();
			lastWasLetter = true;
			if (!((cp >= 0x41 && cp <= 0x24F)
				|| (cp >= 0x400 && cp <= 0x4FF))) {
				alphabetic = false;
			}
		} else if (QChar::isMark(cp) && lastWasLetter) {
			kept += QString::fromUcs4(&cp, 1);
		} else if ((cp == '\'' || cp == 0x2019 || cp == '-' || cp == ' ')
				&& lastWasLetter) {
			kept += QChar(cp == ' ' ? ' ' : QChar(cp));
			lastWasLetter = false;
		} else {
			lastWasLetter = false;
		}
	}
	kept = kept.simplified();
	while (kept.endsWith('-') || kept.endsWith('\'')
		|| kept.endsWith(QChar(0x2019))) {
		kept.chop(1);
	}
	if (lower.isEmpty()) {
		return QString();
	}
	if (alphabetic) {
		if (lower.size() < 2) {
			return QString();
		}
		auto same = true;
		for (const auto ch : lower) {
			same = same && (ch == lower[0]);
		}
		if (same) {
			return QString(); // "qqqq", "ааа"
		}
		static const auto vowels = QString(
			u"aeiouyаеёиоуыэюяіїєáéíóúàèìòùäöüâêîôûãõ"_q);
		auto hasVowel = false;
		for (const auto ch : lower) {
			hasVowel = hasVowel || vowels.contains(ch);
		}
		if (lower.size() >= 4 && !hasVowel) {
			return QString(); // "qwrt", "хзбж"
		}
		if (lower.size() >= 3) {
			static const auto rows = QStringList{
				u"qwertyuiop"_q, u"asdfghjkl"_q, u"zxcvbnm"_q,
				u"йцукенгшщзхъ"_q, u"фывапролджэ"_q, u"ячсмитьбю"_q,
			};
			for (const auto &row : rows) {
				auto reversed = row;
				std::reverse(reversed.begin(), reversed.end());
				if (row.contains(lower) || reversed.contains(lower)) {
					return QString(); // "asdf", "йцук"
				}
			}
		}
		static const auto notNames = QStringList{
			u"user"_q, u"admin"_q, u"test"_q, u"unknown"_q, u"noname"_q,
			u"none"_q, u"null"_q, u"telegram"_q, u"deleted"_q, u"anon"_q,
			u"anonymous"_q, u"name"_q, u"nickname"_q, u"nick"_q, u"bot"_q,
			u"account"_q, u"no name"_q, u"имя"_q, u"аноним"_q,
			u"аккаунт"_q, u"пользователь"_q, u"user name"_q,
		};
		if (notNames.contains(kept.toLower())) {
			return QString();
		}
	}
	return kept;
}

// What to call a person whose name is not usable, in the language of the
// text being sent ("друг"): empty when the language is not sure (a Latin
// text that does not look English) - the name is then simply left out.
[[nodiscard]] inline QString FriendWord(const QString &text) {
	const auto code = DetectLanguage(text);
	if (code == u"ru"_q) {
		return u"друг"_q;
	} else if (code == u"uk"_q) {
		return u"друже"_q;
	} else if (code == u"zh"_q) {
		return u"朋友"_q;
	} else if (code == u"ja"_q) {
		return u"友達"_q;
	} else if (code == u"ko"_q) {
		return u"친구"_q;
	} else if (code == u"ar"_q) {
		return u"صديقي"_q;
	} else if (code == u"th"_q) {
		return u"เพื่อน"_q;
	} else if (code == u"hi"_q) {
		return u"दोस्त"_q;
	} else if (code == u"en"_q) {
		static const auto english = QRegularExpression(
			u"\\b(the|you|your|and|hello|hi|hey|please|thanks|for|with|"
			"this|that|have|are|can)\\b"_q,
			QRegularExpression::CaseInsensitiveOption);
		return english.match(text).hasMatch() ? u"friend"_q : QString();
	}
	return QString();
}

namespace details {

// Everything in a template that is not words: the [..] tokens in order and
// the count of { } |. A translation is accepted only when these are intact.
[[nodiscard]] inline QString TemplateSkeleton(const QString &text) {
	static const auto token = QRegularExpression(u"\\[[^\\]]*\\]"_q);
	// The fallback word of [NAME|друг] is words - it is translated with the
	// rest, so only the token's name counts here.
	static const auto fallback = QRegularExpression(
		u"^\\[(NAME|SURNAME|MYNAME|MYSURNAME|GROUP)\\|.*\\]$"_q);
	auto result = QString();
	auto it = token.globalMatch(text);
	while (it.hasNext()) {
		const auto found = it.next().captured(0);
		const auto match = fallback.match(found);
		result += (match.hasMatch()
			? (u"["_q + match.captured(1) + u"|]"_q)
			: found) + '\n';
	}
	result += u"{%1}%2|%3"_q
		.arg(text.count('{'))
		.arg(text.count('}'))
		.arg(text.count('|'));
	return result;
}

// The words after "|" in [NAME|друг] must come out translated: a template in
// another language with the original word left in it is not a translation.
[[nodiscard]] inline bool KeepsFallbackWords(
		const QString &original,
		const QString &translated) {
	static const auto fallback = QRegularExpression(
		u"\\[(?:NAME|SURNAME|MYNAME|MYSURNAME|GROUP)\\|([^\\]]+)\\]"_q);
	static const auto letters = QRegularExpression(u"\\p{L}"_q);
	auto it = fallback.globalMatch(original);
	while (it.hasNext()) {
		const auto word = it.next().captured(1);
		if (word.contains(letters)
			&& translated.contains(u"|"_q + word + ']')) {
			return true;
		}
	}
	return false;
}

} // namespace details

// Translates plain text (a fast model: translation is a bulk job).
inline void Translate(
		const QString &text,
		const QString &sourceCode,
		const QString &targetCode,
		Fn<void(QString result, QString error)> done,
		int maxTokens = 600) {
	(void)sourceCode; // The model finds the source language itself.
	const auto &target = LanguageByCode(targetCode);
	Ask(
		u"Ты профессиональный переводчик. Переводи точно, сохраняя тон. "
		"Верни только перевод, без пояснений и кавычек."_q,
		u"Переведи на %1 язык:\n\n%2"_q.arg(target.russian, text),
		std::move(done),
		0.2,
		maxTokens,
		Role::Translator);
}

// The safe way to translate a template: only the words, one run at a time
// ([NAME], [SPLIT]..., and the {a|b|c} brackets are never shown to the model;
// every variant inside {...} is a run of its own).
inline void TranslatePieces(
		const QString &templateText,
		const QString &targetCode,
		Fn<void(QString result, QString error)> done) {
	struct Piece {
		QString text;
		bool translate = false;
	};
	auto pieces = std::make_shared<std::vector<Piece>>();
	auto run = QString();
	const auto flush = [&] {
		if (!run.isEmpty()) {
			static const auto letters = QRegularExpression(u"\\p{L}"_q);
			pieces->push_back({ run, run.contains(letters) });
			run.clear();
		}
	};
	for (auto i = 0; i != templateText.size(); ++i) {
		const auto ch = templateText[i];
		if (ch == '[') {
			const auto end = templateText.indexOf(']', i);
			if (end > i) {
				flush();
				const auto token = templateText.mid(i, end - i + 1);
				static const auto fallback = QRegularExpression(
					u"^(\\[(?:NAME|SURNAME|MYNAME|MYSURNAME|GROUP)\\|)(.*)(\\])$"_q);
				const auto match = fallback.match(token);
				if (match.hasMatch()) {
					// [NAME|друг]: the name part stays, the fallback word
					// is translated.
					static const auto letters = QRegularExpression(
						u"\\p{L}"_q);
					pieces->push_back({ match.captured(1), false });
					pieces->push_back({
						match.captured(2),
						match.captured(2).contains(letters),
					});
					pieces->push_back({ match.captured(3), false });
				} else {
					pieces->push_back({ token, false });
				}
				i = end;
				continue;
			}
		}
		if (ch == '{' || ch == '}' || ch == '|') {
			flush();
			pieces->push_back({ QString(ch), false });
			continue;
		}
		run += ch;
	}
	flush();
	const auto sourceCode = [&] {
		const auto detected = DetectLanguage(templateText);
		return detected.isEmpty() ? u"ru"_q : detected;
	}();
	const auto index = std::make_shared<int>(0);
	const auto step = std::make_shared<Fn<void()>>();
	*step = [=] {
		while (*index < int(pieces->size()) && !(*pieces)[*index].translate) {
			++*index;
		}
		if (*index >= int(pieces->size())) {
			auto result = QString();
			for (const auto &piece : *pieces) {
				result += piece.text;
			}
			done(result, QString());
			return;
		}
		const auto current = *index;
		const auto original = (*pieces)[current].text;
		// The spaces around a run belong to the layout, not to the words.
		const auto words = original.trimmed();
		const auto start = original.indexOf(words);
		const auto lead = original.left(start);
		const auto tail = original.mid(start + words.size());
		Translate(words, sourceCode, targetCode, [=](
				QString translated,
				QString error) {
			if (!error.isEmpty()) {
				done(QString(), error);
				return;
			}
			(*pieces)[current].text = lead + translated.trimmed() + tail;
			(*pieces)[current].translate = false;
			(*step)();
		});
	};
	(*step)();
}

// Translates a message template into `targetCode`; [NAME], [SPLIT]... and
// the {a|b|c} brackets come out exactly as they went in. The model gets the
// whole template at once with those as hard rules (the context makes for a
// better translation); the answer is used only if its skeleton is intact,
// otherwise the words are translated run by run.
inline void TranslateTemplate(
		const QString &templateText,
		const QString &targetCode,
		Fn<void(QString result, QString error)> done) {
	Ask(
		u"Ты переводишь шаблон личного сообщения в Telegram. Строгие "
		"правила: 1) метки в квадратных скобках ([NAME], [SURNAME], "
		"[MYNAME], [MYSURNAME], [GROUP], [SPLIT]) оставь символ в символ; в "
		"[NAME|слово] переведи только слово после «|»; 2) группы вариантов "
		"{а|б|в} сохрани: те же фигурные скобки и черты, столько же "
		"вариантов, переведи только слова внутри; 3) тон живой, как в "
		"личной переписке. Верни только перевод, без пояснений."_q,
		u"Переведи на %1 язык:\n\n%2"_q.arg(
			LanguageByCode(targetCode).russian,
			templateText),
		[=](QString result, QString error) {
			if (error.isEmpty()
				&& details::TemplateSkeleton(result)
					== details::TemplateSkeleton(templateText)
				&& !details::KeepsFallbackWords(templateText, result)) {
				done(result, QString());
				return;
			}
			TranslatePieces(templateText, targetCode, done);
		},
		0.2,
		900,
		Role::Translator);
}

// The template in `targetCode`, translated once per (template, language) for
// the whole run: the first recipient of a language waits for the model, the
// others (even from other lanes at the same moment) take the same result.
inline void TranslateTemplateCached(
		const QString &templateText,
		const QString &targetCode,
		Fn<void(QString result, QString error)> done) {
	static auto cache = std::map<QString, QString>();
	static auto pending = std::map<
		QString,
		std::vector<Fn<void(QString, QString)>>>();
	const auto key = targetCode + '\n' + templateText;
	if (const auto i = cache.find(key); i != cache.end()) {
		done(i->second, QString());
		return;
	}
	auto &waiting = pending[key];
	waiting.push_back(std::move(done));
	if (waiting.size() > 1) {
		return;
	}
	TranslateTemplate(templateText, targetCode, [=](
			QString result,
			QString error) {
		if (error.isEmpty()) {
			cache[key] = result;
		}
		auto callbacks = std::move(pending[key]);
		pending.erase(key);
		for (const auto &callback : callbacks) {
			callback(result, error);
		}
	});
}

// The language of a short text by asking the model - for the cases where
// the letters do not tell (Latin: English? Spanish? Turkish?). `done(code)`
// is empty when it could not tell.
inline void GuessLanguage(
		const QString &text,
		Fn<void(QString code)> done) {
	Ask(
		u"Определи язык текста. Ответь ТОЛЬКО двухбуквенным кодом ISO 639-1 "
		"(например en, es, tr, de, fr, pt, it, id, vi, pl). Если текст не "
		"позволяет определить язык, ответь: ??"_q,
		text.left(400),
		[=](QString result, QString error) {
			const auto code = result.trimmed().toLower().left(2);
			for (const auto &language : Languages()) {
				if (error.isEmpty() && language.code == code) {
					done(code);
					return;
				}
			}
			done(QString());
		},
		0.,
		8);
}

// ---- the state, for the sidebar's robot and the "ИИ" section ----

// Off: no keys (or the AI is switched off); Starting: every key rests after
// a limit or an error; Ready: a key can take a job now.
enum class EngineLevel { Off, Starting, Ready };

[[nodiscard]] inline EngineLevel EngineLevelNow() {
	const auto &settings = CachedSettings();
	if (!CloudReady(settings)) {
		return EngineLevel::Off;
	}
	for (const auto &key : settings.keys) {
		if (CloudKeyUsable(key) && !details::Resting(key.key, QString())) {
			return EngineLevel::Ready;
		}
	}
	return EngineLevel::Starting;
}

// One line for the "ИИ" section: how many keys of which providers, how many
// rest, how many questions are on their way.
[[nodiscard]] inline QString EngineStatusText() {
	const auto &settings = CachedSettings();
	auto counts = std::map<QString, int>();
	auto usable = 0;
	auto resting = 0;
	for (const auto &key : settings.keys) {
		if (!CloudKeyUsable(key)) {
			continue;
		}
		++usable;
		++counts[CloudProviderName(key.provider)];
		if (details::Resting(key.key, QString())) {
			++resting;
		}
	}
	if (!usable) {
		return u"Нет ни одного ключа: добавьте бесплатный ключ в шаге "
			"«1. Ключи»"_q;
	}
	auto parts = QStringList();
	for (const auto &[name, count] : counts) {
		parts.push_back(name + (count > 1
			? (u" ×"_q + QString::number(count))
			: QString()));
	}
	auto result = u"Ключей: %1 (%2)"_q.arg(usable).arg(parts.join(u", "_q));
	if (resting) {
		const auto back = details::SoonestBack(settings);
		result += u". Отдыхают: %1"_q.arg(resting)
			+ (back.isEmpty() ? QString() : (u", первый вернётся "_q + back));
	}
	const auto &slot = details::SlotState();
	if (slot.active) {
		result += u". Сейчас запросов: %1"_q.arg(slot.active)
			+ (slot.queue.empty()
				? QString()
				: (u", в очереди: "_q + QString::number(slot.queue.size())));
	}
	return result;
}

// "Проверить все ключи": every key in turn, a line each. `progress` may be
// null.
inline void TestConnection(
		Fn<void(bool ok, QString message)> done,
		Fn<void(QString progress)> progress = nullptr) {
	const auto keys = Load().keys;
	if (keys.empty()) {
		done(false, u"Нет ни одного ключа: добавьте бесплатный ключ в "
			"шаге «1. Ключи»."_q);
		return;
	}
	struct Run {
		int index = 0;
		QStringList lines;
		bool ok = false;
	};
	const auto run = std::make_shared<Run>();
	const auto step = std::make_shared<Fn<void()>>();
	*step = [=] {
		if (run->index == int(keys.size())) {
			done(run->ok, run->lines.join('\n'));
			return;
		}
		const auto key = keys[run->index++];
		if (progress) {
			progress(u"Проверяю ключ %1 из %2 (%3)..."_q
				.arg(run->index)
				.arg(keys.size())
				.arg(ShortKey(key.key)));
		}
		TestKey(key.key, [=](bool ok, QString message) {
			run->ok = run->ok || ok;
			run->lines.push_back((ok ? u"✓ "_q : u"✗ "_q)
				+ ShortKey(key.key) + u": "_q + message);
			(*step)();
		});
	};
	(*step)();
}

// What to fill in for the chat assistant: who the user is, where the talk
// should lead, how to speak and what may be said - so that the replies are
// written as the user themselves and steer to their goal.
[[nodiscard]] inline QString ChatPromptTemplate() {
	return u"Кто я: \n"
		"Чем занимаюсь: \n"
		"Цель диалога (к чему вести собеседника): \n"
		"Как общаться (стиль, обращение на «ты» или «вы»): \n"
		"Что можно говорить (цены, условия, ссылки): \n"
		"Чего нельзя: "_q;
}


// Where the assistant's materials (and a picture being read) are kept.
[[nodiscard]] inline QString AiDir() {
	return Core::GgtgPaths::DbDir() + u"ai/"_q;
}

// What the earlier, local version left on the disk: the models, the
// engine and its archive (up to 12 GB). Nothing uses them any more. The old
// "materials" folder is left alone until the knowledge base has taken its
// texts in (see Knowledge::List).
[[nodiscard]] inline QStringList LegacyLocalPaths() {
	auto result = QStringList();
	for (const auto &name : { u"models"_q, u"runtime"_q }) {
		const auto path = AiDir() + name;
		if (QFileInfo(path).isDir()) {
			result.push_back(path);
		}
	}
	auto it = QDirIterator(
		AiDir(),
		QStringList{
			u"*.tar.gz"_q,
			u"*.zip"_q,
			u"*.gguf"_q,
			u"*.part"_q,
			u"*.partial"_q,
			u"*.download"_q },
		QDir::Files);
	while (it.hasNext()) {
		result.push_back(it.next());
	}
	return result;
}

[[nodiscard]] inline qint64 LegacyLocalBytes() {
	auto result = qint64(0);
	for (const auto &path : LegacyLocalPaths()) {
		if (QFileInfo(path).isDir()) {
			auto it = QDirIterator(
				path,
				QDir::Files | QDir::Hidden,
				QDirIterator::Subdirectories);
			while (it.hasNext()) {
				it.next();
				result += it.fileInfo().size();
			}
		} else {
			result += QFileInfo(path).size();
		}
	}
	return result;
}

// Deletes them; returns how many bytes were freed.
inline qint64 CleanLegacyLocal() {
	const auto size = LegacyLocalBytes();
	for (const auto &path : LegacyLocalPaths()) {
		if (QFileInfo(path).isDir()) {
			QDir(path).removeRecursively();
		} else {
			QFile::remove(path);
		}
	}
	// An empty folder of the old materials, and the folder itself.
	QDir().rmdir(AiDir() + u"materials"_q);
	QDir().rmdir(AiDir());
	return size;
}

[[nodiscard]] inline QString SizeText(qint64 bytes) {
	return (bytes >= (1LL << 30))
		? QString::number(double(bytes) / (1LL << 30), 'f', 1) + u" ГБ"_q
		: QString::number(std::max(bytes / (1 << 20), qint64(1))) + u" МБ"_q;
}

// The black console window of a helper program is not shown on Windows.
inline void HideConsoleWindow(QProcess *process) {
#ifdef Q_OS_WIN
	process->setCreateProcessArgumentsModifier([](
			QProcess::CreateProcessArguments *args) {
		args->flags |= 0x08000000; // CREATE_NO_WINDOW
	});
#else // Q_OS_WIN
	(void)process;
#endif // Q_OS_WIN
}

// The archiver that reads Office files (they are zip archives): it ships
// with macOS and with Windows 10 and newer, and reads .zip and .tar.gz alike.
[[nodiscard]] inline QString TarProgram() {
#ifdef Q_OS_WIN
	const auto found = QStandardPaths::findExecutable(u"tar"_q);
	return found.isEmpty()
		? u"C:/Windows/System32/tar.exe"_q
		: found;
#else // Q_OS_WIN
	return u"/usr/bin/tar"_q;
#endif // Q_OS_WIN
}


// ---- what the assistant has learned about the user ----

// The model's weights never change, but the assistant gets better the way a
// person does: what it learns about the user and their business lives in the
// profile (core/ggtg_ai_profile.h), and a few messages the user really sent
// after the assistant's draft (their own style) are kept here. Both go into
// the prompts of the chat assistant.

[[nodiscard]] inline QString ExamplesPath() {
	return Core::GgtgPaths::DbDir() + u"ai_examples.json"_q;
}

[[nodiscard]] inline QStringList LoadExamples() {
	auto file = QFile(ExamplesPath());
	if (!file.open(QIODevice::ReadOnly)) {
		return {};
	}
	auto result = QStringList();
	for (const auto &value : QJsonDocument::fromJson(file.readAll()).array()) {
		result.push_back(value.toString());
	}
	return result;
}

// A message the user really sent after the assistant's draft: how they write.
inline void AddStyleExample(const QString &text) {
	const auto clean = text.trimmed();
	if (clean.size() < 8 || clean.size() > 400) {
		return;
	}
	auto list = LoadExamples();
	list.removeAll(clean);
	list.push_back(clean);
	while (list.size() > 30) {
		list.pop_front();
	}
	auto array = QJsonArray();
	for (const auto &item : list) {
		array.push_back(item);
	}
	auto file = QSaveFile(ExamplesPath());
	if (file.open(QIODevice::WriteOnly)) {
		file.write(QJsonDocument(array).toJson(QJsonDocument::Compact));
		file.commit();
	}
}

[[nodiscard]] inline QString ExamplesBlock(int count = 4) {
	const auto list = LoadExamples();
	auto result = QString();
	for (auto i = std::max(0, int(list.size()) - count);
			i != int(list.size());
			++i) {
		result += u"• "_q + list[i] + '\n';
	}
	return result;
}

// What the assistant remembers of the conversations lives in one file, one
// entry per chat: {summary, lastId, usedAt}. An entry nobody has used for the
// time set in "ИИ" is deleted by itself, so nothing piles up for good.
[[nodiscard]] inline QString ChatMemoryPath() {
	return Core::GgtgPaths::DbDir() + u"ai_chat_memory.json"_q;
}

[[nodiscard]] inline QJsonObject ReadChatMemory() {
	auto file = QFile(ChatMemoryPath());
	if (!file.open(QIODevice::ReadOnly)) {
		return QJsonObject();
	}
	return QJsonDocument::fromJson(file.readAll()).object();
}

inline void WriteChatMemory(const QJsonObject &memory) {
	auto file = QSaveFile(ChatMemoryPath());
	if (file.open(QIODevice::WriteOnly)) {
		file.write(QJsonDocument(memory).toJson(QJsonDocument::Compact));
		file.commit();
	}
}

// Marks one chat's memory as used just now (it stays another full period).
inline void TouchChatMemory(const QString &key) {
	auto memory = ReadChatMemory();
	auto entry = memory.value(key).toObject();
	if (entry.isEmpty()) {
		return;
	}
	entry.insert("usedAt", double(QDateTime::currentSecsSinceEpoch()));
	memory.insert(key, entry);
	WriteChatMemory(memory);
}

// Deletes the entries unused for longer than the limit; an entry from before
// the limit existed gets today's date (a full period of grace). Returns how
// many conversations are remembered after that. `force` ignores the hourly
// throttle.
inline int PurgeChatMemory(bool force = false) {
	static auto last = qint64(0);
	const auto now = QDateTime::currentSecsSinceEpoch();
	auto memory = ReadChatMemory();
	if (!force && now - last < 3600) {
		return int(memory.size());
	}
	last = now;
	const auto days = Load().memoryDays;
	auto changed = false;
	for (const auto &key : memory.keys()) {
		auto entry = memory.value(key).toObject();
		const auto usedAt = qint64(entry.value("usedAt").toDouble());
		if (usedAt == 0) {
			entry.insert("usedAt", double(now));
			memory.insert(key, entry);
			changed = true;
		} else if (days > 0 && now - usedAt > qint64(days) * 86400) {
			memory.remove(key);
			changed = true;
		}
	}
	if (changed) {
		WriteChatMemory(memory);
	}
	return int(memory.size());
}

// Forgets what the assistant remembers of every conversation.
inline void ForgetAllChats() {
	QFile::remove(ChatMemoryPath());
}

namespace details {

// The text of a document by what macOS itself can read: plain text as it is,
// Word / Rich Text / HTML through textutil, a presentation through its slides'
// XML. Empty when the format is not one of these.
[[nodiscard]] inline QString ExtractDocumentText(const QString &path) {
	const auto suffix = QFileInfo(path).suffix().toLower();
	const auto run = [](const QString &program, const QStringList &args) {
		auto process = QProcess();
		HideConsoleWindow(&process);
		process.start(program, args);
		if (!process.waitForFinished(60000)) {
			process.kill();
			return QByteArray();
		}
		return process.readAllStandardOutput();
	};
	if (suffix == u"txt"_q || suffix == u"md"_q || suffix == u"csv"_q
		|| suffix == u"json"_q || suffix == u"log"_q || suffix == u"xml"_q
		|| suffix == u"tsv"_q) {
		auto file = QFile(path);
		return file.open(QIODevice::ReadOnly)
			? QString::fromUtf8(file.readAll())
			: QString();
	}
#ifdef Q_OS_MAC
	if (suffix == u"docx"_q || suffix == u"doc"_q
		|| suffix == u"rtf"_q || suffix == u"odt"_q || suffix == u"html"_q
		|| suffix == u"htm"_q) {
		return QString::fromUtf8(run(u"/usr/bin/textutil"_q, {
			u"-convert"_q, u"txt"_q, u"-stdout"_q, path }));
	}
#else // Q_OS_MAC
	if (suffix == u"html"_q || suffix == u"htm"_q) {
		auto file = QFile(path);
		if (!file.open(QIODevice::ReadOnly)) {
			return QString();
		}
		auto document = QTextDocument();
		document.setHtml(QString::fromUtf8(file.readAll()));
		return document.toPlainText();
	} else if (suffix == u"docx"_q) {
		// A Word file is a zip with the text in word/document.xml.
		auto xml = QString::fromUtf8(run(TarProgram(), {
			u"-xOf"_q, path, u"word/document.xml"_q }));
		xml.replace(u"</w:p>"_q, u"\n"_q);
		static const auto wordRe = QRegularExpression(
			u"<w:t(?:\\s[^>]*)?>([^<]*)</w:t>|(\\n)"_q);
		auto result = QString();
		auto it = wordRe.globalMatch(xml);
		while (it.hasNext()) {
			const auto match = it.next();
			result += match.captured(2).isEmpty()
				? match.captured(1)
				: u"\n"_q;
		}
		result.replace(u"&amp;"_q, u"&"_q);
		result.replace(u"&lt;"_q, u"<"_q);
		result.replace(u"&gt;"_q, u">"_q);
		result.replace(u"&quot;"_q, u"\""_q);
		return result;
	}
#endif // Q_OS_MAC
	if (suffix == u"pptx"_q) {
		// A presentation is a zip too: read the slides' XML with the archiver
		// (macOS and Windows both have it).
		const auto listing = QString::fromUtf8(
			run(TarProgram(), { u"-tf"_q, path }));
		auto slides = QStringList();
		for (auto name : listing.split('\n')) {
			name = name.trimmed();
			if (name.startsWith(u"ppt/slides/slide"_q)
				&& name.endsWith(u".xml"_q)) {
				slides.push_back(name);
			}
		}
		std::sort(slides.begin(), slides.end(), [](
				const QString &a,
				const QString &b) {
			// slide2 before slide10
			if (a.size() != b.size()) {
				return a.size() < b.size();
			}
			return a < b;
		});
		static const auto textRe = QRegularExpression(
			u"<a:t>([^<]*)</a:t>"_q);
		auto result = QString();
		auto number = 0;
		for (const auto &name : slides) {
			const auto xml = QString::fromUtf8(run(
				TarProgram(),
				{ u"-xOf"_q, path, name }));
			auto line = QString();
			auto it = textRe.globalMatch(xml);
			while (it.hasNext()) {
				line += it.next().captured(1) + ' ';
			}
			line = line.simplified();
			if (!line.isEmpty()) {
				result += u"Слайд %1: "_q.arg(++number) + line + '\n';
			}
		}
		result.replace(u"&amp;"_q, u"&"_q);
		result.replace(u"&lt;"_q, u"<"_q);
		result.replace(u"&gt;"_q, u">"_q);
		result.replace(u"&quot;"_q, u"\""_q);
		return result;
	}
	return QString();
}

// A photo shrunk to something the model can take (macOS sips), as a data
// address; empty when it could not be read.
[[nodiscard]] inline QString PictureDataUrl(const QString &path) {
#ifndef Q_OS_MAC
	// Elsewhere the picture is shrunk by Qt itself.
	auto image = QImage(path);
	if (image.isNull()) {
		return QString();
	}
	if (std::max(image.width(), image.height()) > 1280) {
		image = image.scaled(
			1280,
			1280,
			Qt::KeepAspectRatio,
			Qt::SmoothTransformation);
	}
	auto bytes = QByteArray();
	auto buffer = QBuffer(&bytes);
	buffer.open(QIODevice::WriteOnly);
	image.convertToFormat(QImage::Format_RGB888).save(&buffer, "JPEG", 85);
	return bytes.isEmpty()
		? QString()
		: (u"data:image/jpeg;base64,"_q
			+ QString::fromLatin1(bytes.toBase64()));
#endif // !Q_OS_MAC
	const auto temp = AiDir() + u"picture.tmp.jpg"_q;
	QDir().mkpath(AiDir());
	QFile::remove(temp);
	auto process = QProcess();
	process.start(u"/usr/bin/sips"_q, QStringList{
		u"-s"_q, u"format"_q, u"jpeg"_q,
		u"-Z"_q, u"1280"_q,
		path,
		u"--out"_q, temp });
	if (!process.waitForFinished(60000)) {
		process.kill();
		return QString();
	}
	auto file = QFile(temp);
	if (!file.open(QIODevice::ReadOnly)) {
		return QString();
	}
	const auto data = file.readAll();
	file.close();
	QFile::remove(temp);
	return data.isEmpty()
		? QString()
		: (u"data:image/jpeg;base64,"_q + QString::fromLatin1(data.toBase64()));
}

} // namespace details

} // namespace Core::GgtgAi
