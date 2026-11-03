/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and exceptions, see https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "core/ggtg_ai.h"
#include "core/ggtg_ai_feedback.h"
#include "core/ggtg_ai_profile.h"

#include <random>

namespace Core::GgtgAi::Report {

// Two things that tell whether the AI is doing well, from the user's own
// data and not from feeling:
//
//  * Daily(): what the answers to the mailings say and how the model itself
//    behaved (ai_log.jsonl), with plain advice computed from the numbers;
//  * Evaluate(): the sorting of answers checked against what is already
//    sorted - how much the word rules take and whether the model agrees
//    with itself on a sample.

namespace details {

struct CallStats {
	int total = 0;
	int failed = 0;
	int switched = 0; // a model failed and the next one took the job
	qint64 ms = 0;
	int ok = 0;
	std::map<QString, int> byProvider; // answers per provider
};

[[nodiscard]] inline CallStats ReadCalls(qint64 since) {
	auto result = CallStats();
	auto file = QFile(Core::GgtgPaths::DbDir() + u"ai_log.jsonl"_q);
	if (!file.open(QIODevice::ReadOnly)) {
		return result;
	}
	while (!file.atEnd()) {
		const auto object = QJsonDocument::fromJson(file.readLine()).object();
		if (qint64(object.value("at").toDouble()) < since) {
			continue;
		}
		const auto note = object.value("note").toString();
		const auto ok = object.value("ok").toBool();
		const auto ms = qint64(object.value("ms").toDouble());
		if (note.startsWith(u"cloud failed"_q)) {
			// One model failed and the next one took the job.
			++result.switched;
			continue;
		}
		++result.total;
		result.failed += ok ? 0 : 1;
		if (ok && ms > 0) {
			++result.ok;
			result.ms += ms;
		}
		if (ok && note.startsWith(u"cloud "_q)) {
			++result.byProvider[note.section(' ', 1, 1)];
		}
	}
	return result;
}

[[nodiscard]] inline QString Percent(int part, int whole) {
	return whole
		? (QString::number(part * 100 / whole) + u"%"_q)
		: u"-"_q;
}

} // namespace details

[[nodiscard]] inline QString Daily() {
	namespace F = Feedback;
	const auto now = QDateTime::currentSecsSinceEpoch();
	const auto replies = F::LoadReplies();
	auto all = std::map<QString, int>();
	auto week = std::map<QString, int>();
	auto unsorted = 0;
	auto weekTotal = 0;
	for (const auto &[key, reply] : replies) {
		if (reply.text.isEmpty()) {
			continue;
		}
		if (reply.kind.isEmpty()) {
			++unsorted;
			continue;
		}
		++all[reply.kind];
		if (reply.at >= now - 7 * 86400) {
			++week[reply.kind];
			++weekTotal;
		}
	}
	auto total = 0;
	for (const auto &[kind, count] : all) {
		total += count;
	}
	const auto line = [&](const QString &kind) {
		return F::details::KindWord(kind) + u": "_q
			+ QString::number(week[kind]) + u" за неделю, "_q
			+ QString::number(all[kind]) + u" всего"_q;
	};
	auto result = u"Ответы на рассылки\n"_q;
	if (!total && !unsorted) {
		result += u"  Пока нет ни одного ответа.\n"_q;
	} else {
		for (const auto &kind : { u"interest"_q, u"question"_q,
			u"refusal"_q, u"negative"_q, u"other"_q }) {
			result += u"  "_q + line(kind) + '\n';
		}
		if (unsorted) {
			result += u"  Не разобрано: "_q + QString::number(unsorted)
				+ u"\n"_q;
		}
		result += u"  Помечено успешными: "_q
			+ QString::number(F::LoadSuccesses().size()) + '\n';
	}

	const auto calls = details::ReadCalls(now - 7 * 86400);
	result += u"\nМодель за 7 дней\n"_q;
	if (!calls.total) {
		result += u"  Вопросов к ИИ не было.\n"_q;
	} else {
		result += u"  Вопросов: "_q + QString::number(calls.total)
			+ u", без ответа: "_q + QString::number(calls.failed) + '\n';
		if (calls.ok) {
			result += u"  Ответ приходит в среднем за "_q
				+ QString::number(calls.ms / calls.ok / 1000.)
				+ u" с\n"_q;
		}
		if (!calls.byProvider.empty()) {
			auto parts = QStringList();
			for (const auto &[provider, count] : calls.byProvider) {
				parts.push_back(Core::GgtgAi::CloudProviderName(provider)
					+ ' ' + QString::number(count));
			}
			result += u"  Кто отвечал: "_q + parts.join(u", "_q) + '\n';
		}
		if (calls.switched) {
			result += u"  Модель упиралась в лимит или ошибку, и работу "
				"брала следующая: "_q + QString::number(calls.switched)
				+ u" раз\n"_q;
		}
	}
	result += u"\nПрофиль: пунктов "_q
		+ QString::number(Profile::Read().count()) + u".\n"_q;

	auto advice = QStringList();
	if (unsorted > 0) {
		advice.push_back(u"Есть неразобранные ответы: откройте «Ответы» и "
			"нажмите «Разобрать»."_q);
	}
	if (weekTotal >= 20 && week[u"negative"_q] * 100 / weekTotal >= 15) {
		advice.push_back(u"Много негатива за неделю ("_q
			+ details::Percent(week[u"negative"_q], weekTotal)
			+ u"): смягчите первое сообщение, уберите давление и ссылки."_q);
	}
	if (weekTotal >= 30 && week[u"interest"_q] * 100 / weekTotal < 5) {
		advice.push_back(u"Интереса почти нет: попробуйте другой шаблон "
			"или другую аудиторию, сравните через «Статистику»."_q);
	}
	if (calls.total >= 5 && calls.switched * 100 / calls.total >= 50) {
		advice.push_back(u"Модели часто упираются в лимит: добавьте ещё "
			"ключ (другого сервиса или аккаунта) в шаге «1. Ключи»."_q);
	}
	if (calls.total >= 10 && calls.failed * 100 / calls.total >= 20) {
		advice.push_back(u"Много вопросов без ответа: проверьте ключи "
			"кнопкой «Проверить все ключи» в шаге «1. Ключи»."_q);
	}
	if (!advice.isEmpty()) {
		result += u"\nЧто сделать\n"_q;
		for (const auto &item : advice) {
			result += u"  - "_q + item + '\n';
		}
	}
	return result.trimmed();
}

// Checks the sorting: `done(report)` runs on the main thread. Costs one
// question to the model.
inline void Evaluate(Fn<void(QString report)> done) {
	namespace F = Feedback;
	struct Item {
		QString text;
		QString kind;
	};
	auto labeled = std::vector<Item>();
	for (const auto &[key, reply] : F::LoadReplies()) {
		if (!reply.kind.isEmpty() && !reply.text.isEmpty()) {
			labeled.push_back({ reply.text.left(200), reply.kind });
		}
	}
	if (labeled.size() < 10) {
		done(u"Мало данных: нужно хотя бы 10 разобранных ответов, сейчас "_q
			+ QString::number(labeled.size()) + u". Проверка станет "
				"полезной, когда на рассылки ответят."_q);
		return;
	}
	auto covered = 0;
	auto agreed = 0;
	for (const auto &item : labeled) {
		const auto kind = F::details::QuickKind(item.text);
		if (!kind.isEmpty()) {
			++covered;
			agreed += (kind == item.kind) ? 1 : 0;
		}
	}
	const auto rules = u"Правила по словам\n  Берут на себя "_q
		+ details::Percent(covered, int(labeled.size()))
		+ u" ответов (без модели, мгновенно), совпадают с разметкой в "_q
		+ details::Percent(agreed, covered) + u" случаев.\n"_q;

	auto sample = labeled;
	std::shuffle(
		sample.begin(),
		sample.end(),
		std::mt19937(std::random_device()()));
	sample.resize(std::min<size_t>(sample.size(), 20));
	auto material = QString();
	for (auto i = 0; i != int(sample.size()); ++i) {
		material += QString::number(i + 1) + u": "_q + sample[i].text + '\n';
	}
	Core::GgtgAi::Ask(
		F::details::ClassifySystem(false),
		material,
		[=](QString result, QString error) {
			if (!error.isEmpty()) {
				done(rules + u"\nМодель: не вышло проверить ("_q + error
					+ u")."_q);
				return;
			}
			static const auto line = QRegularExpression(
				u"^\\s*(?:№|#)?\\s*(\\d+)\\s*[:.\\-)]\\s*(.+?)\\s*$"_q);
			auto answered = 0;
			auto same = 0;
			for (const auto &row : result.split('\n')) {
				const auto match = line.match(row);
				if (!match.hasMatch()) {
					continue;
				}
				const auto index = match.captured(1).toInt() - 1;
				const auto kind = F::details::KindByWord(match.captured(2));
				if (index < 0 || index >= int(sample.size())
					|| kind.isEmpty()) {
					continue;
				}
				++answered;
				same += (kind == sample[index].kind) ? 1 : 0;
			}
			done(rules + u"\nМодель (выборка "_q
				+ QString::number(sample.size())
				+ u" ответов)\n  Ответила по "_q + QString::number(answered)
				+ u", совпала с прежней разметкой в "_q
				+ details::Percent(same, answered)
				+ u" случаев.\n\nРазметка сделана самой программой, поэтому "
					"это проверка устойчивости, а не истины: если цифра "
					"ниже 80%, правьте шаблон или меняйте модель, а "
					"сомнительные ответы разбирайте вручную."_q);
		},
		0.1,
		300);
}

} // namespace Core::GgtgAi::Report
