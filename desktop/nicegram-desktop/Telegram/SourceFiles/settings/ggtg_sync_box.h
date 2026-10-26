/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and exceptions, see https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "boxes/connection_box.h"
#include "core/application.h"
#include "lang/lang_keys.h"
#include "core/ggtg_sync.h"
#include "core/ggtg_sync_transport.h"
#include "core/ggtg_sessions.h"
#include "storage/storage_tdata_export.h"
#include "data/data_user.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "mtproto/mtproto_proxy_data.h"
#include "settings/ggtg_api_override_box.h"
#include "settings/ggtg_export.h"
#include "ui/layers/generic_box.h"
#include "ui/vertical_list.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/painter.h"
#include "ui/rp_widget.h"
#include "qr/qr_generate.h"
#include "base/timer.h"
#include "base/unixtime.h"
#include "window/ggtg_widgets.h"
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_widgets.h"

#include <QtCore/QDataStream>
#include <QtCore/QDateTime>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QPointer>

namespace Settings {

// "Синхронизация": one main Telegram account carries GGTG's data between
// this Mac and GGTG Android (see core/ggtg_sync.h). The phone joins by
// scanning a QR code with the main account and the key; from then on it all
// goes by itself, and every mailing first takes in the phone's records.
// Accounts that GGTG Android put into the main account's "Saved messages":
// new sessions made for this Mac (see Android's GgtgSyncAccountsActivity).
// They are added like a tdata import: a proxy is asked for first.
inline void AcceptAccountsFromAndroid(
		not_null<Ui::GenericBox*> box,
		const Core::GgtgSync::Remote &remote) {
	const auto show = box->uiShow();
	Core::GgtgSync::FetchIncoming(remote, [=](
			QByteArray payload,
			QString error) {
		if (!error.isEmpty()) {
			Ggtg::Notice(show, error);
			return;
		}
		const auto manifest = QJsonDocument::fromJson(payload).object();
		auto list = std::vector<Storage::ForeignTdataAuthorization>();
		auto labels = std::make_shared<std::map<QString, QJsonObject>>();
		for (const auto &value : manifest["accounts"].toArray()) {
			const auto account = value.toObject();
			const auto userId = account["userId"].toString().toULongLong();
			const auto keys = account["keys"].toArray();
			if (!userId || keys.isEmpty()) {
				continue;
			}
			auto data = QByteArray();
			{
				QDataStream stream(&data, QIODevice::WriteOnly);
				stream.setVersion(QDataStream::Qt_5_1);
				stream
					<< quint64(~quint64(0))
					<< quint64(userId)
					<< qint32(account["mainDcId"].toInt());
				stream << qint32(keys.size());
				for (const auto &item : keys) {
					const auto key = QByteArray::fromHex(
						item.toObject()["key"].toString().toLatin1());
					stream << qint32(item.toObject()["dcId"].toInt());
					stream.writeRawData(key.constData(), key.size());
				}
				stream << qint32(0);
			}
			const auto tag = u"sync:"_q + QString::number(userId);
			(*labels)[tag] = account;
			list.push_back({ .data = data, .folder = tag });

			// Also persist this session in the Session Vault as an Android session
			namespace Sessions = Core::GgtgSessions;
			const auto uIdStr = QString::number(userId);
			auto alreadySaved = false;
			for (const auto &s : Sessions::StoredForUser(Sessions::LoadVault(), uIdStr)) {
				if (s.role == u"android"_q || s.source == u"android_sync"_q) {
					alreadySaved = true;
					break;
				}
			}
			if (!alreadySaved) {
				auto entry = Sessions::StoredSession();
				entry.userId = uIdStr;
				entry.role = u"android"_q;
				entry.source = u"android_sync"_q;
				entry.note = u"сессия с Android (синхронизация)"_q;
				entry.phone = account["phone"].toString();
				entry.name = account["name"].toString();
				entry.username = account["username"].toString();
				entry.folder = account["folder"].toString();
				entry.twoFactor = account["twoFactor"].toString();
				if (account.contains("proxy")) {
					entry.proxy = account["proxy"].toObject();
				}
				if (account.contains("device")) {
					entry.device = account["device"].toObject();
				}
				entry = Sessions::AddStored(entry);
				auto storeErr = QString();
				Storage::ExportRawAuthorizationAsTdata(
					data,
					Sessions::VaultFolder(entry),
					&storeErr);
				Sessions::WriteSessionVaultSidecar(entry);
			}
		}
		if (list.empty()) {
			Ggtg::Notice(show, u"В файле нет аккаунтов"_q);
			return;
		}

		const auto getAccountProxy = [=](const QString &tag) -> MTP::ProxyData {
			const auto it = labels->find(tag);
			if (it != labels->end() && it->second.contains("proxy")) {
				const auto p = it->second["proxy"].toObject();
				const auto type = p["type"].toString();
				auto result = MTP::ProxyData();
				result.host = p["host"].toString();
				result.port = p["port"].toInt();
				result.user = p["user"].toString();
				result.password = p["password"].toString();
				if (type == u"mtproto"_q) {
					result.type = MTP::ProxyData::Type::Mtproto;
					result.password = p["secret"].toString();
				} else if (type == u"socks5"_q) {
					result.type = MTP::ProxyData::Type::Socks5;
				} else if (type == u"http"_q) {
					result.type = MTP::ProxyData::Type::Http;
				}
				if (!result.host.isEmpty() && result.port > 0) {
					return result;
				}
			}
			return MTP::ProxyData();
		};

		auto needProxyCount = 0;
		for (const auto &auth : list) {
			if (getAccountProxy(auth.folder).type == MTP::ProxyData::Type::None) {
				++needProxyCount;
			}
		}

		const auto executeImport = [=](Fn<MTP::ProxyData()> fallbackProxy) {
			auto &domain = Core::App().domain();
			if (domain.accounts().size() >= domain.maxAccounts()) {
				Ggtg::Notice(show, u"Достигнут лимит аккаунтов"_q);
				return;
			}
			auto nextIdx = std::make_shared<size_t>(0);
			const auto nextProxy = [=]() -> MTP::ProxyData {
				const auto idx = (*nextIdx)++;
				if (idx < list.size()) {
					const auto own = getAccountProxy(list[idx].folder);
					if (own.type != MTP::ProxyData::Type::None) {
						return own;
					}
				}
				return fallbackProxy ? fallbackProxy() : MTP::ProxyData();
			};
			const auto imported = domain.addFromAuthorizations(
				list,
				MTP::Environment::Production,
				nextProxy,
				[=](not_null<::Main::Account*> account, const QString &tag) {
					const auto i = labels->find(tag);
					if (i == labels->end()) {
						return;
					}
					const auto &meta = i->second;
					if (meta.contains("folder")) {
						account->setFolderTag(meta["folder"].toString());
					}
					account->setPinned(meta["pinned"].toBool());
					const auto password = meta["twoFactor"].toString();
					if (!password.isEmpty()) {
						account->setTwoFactorPassword(password);
					}
					if (meta.contains("device")) {
						account->setDeviceOverride(
							Settings::DeviceOverrideFromJson(meta["device"].toObject()));
					}
				});
			if (imported.empty()) {
				Ggtg::Notice(
					show,
					u"Ничего не добавлено: эти аккаунты уже есть в программе"_q);
				return;
			}
			domain.activate(imported.front());
			Ggtg::Notice(
				show,
				u"Добавлено аккаунтов: "_q + QString::number(imported.size()));
		};

		if (needProxyCount == 0) {
			executeImport(nullptr);
		} else {
			show->show(MakeMultiProxyBox([=](Fn<MTP::ProxyData()> poolProxy) {
				executeImport(poolProxy);
			}));
		}
	});
}

// The QR code GGTG Android scans to join (see Core::GgtgSync::PairingText).
[[nodiscard]] inline QImage RenderGgtgSyncQr(const QString &text) {
	const auto data = Qr::Encode(text, Qr::Redundancy::Quartile);
	const auto ratio = style::DevicePixelRatio();
	const auto pixel = std::max(st::boxWideWidth / 2 / std::max(data.size, 1), 1);
	auto image = Qr::Generate(data, pixel * ratio, Qt::black, Qt::white);
	image.setDevicePixelRatio(ratio);
	return image;
}

inline void ShowGgtgSyncQrBox(
		std::shared_ptr<Ui::Show> show,
		const Core::GgtgSync::Settings &settings,
		const QString &accountLabel) {
	const auto image = std::make_shared<QImage>(
		RenderGgtgSyncQr(Core::GgtgSync::PairingText(settings)));
	show->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"Подключить телефон"_q));
		box->setWidth(st::boxWideWidth);
		const auto size = image->width() / style::DevicePixelRatio();
		const auto skip = st::boxRowPadding.left();
		const auto qr = box->addRow(
			object_ptr<Ui::RpWidget>(box),
			style::margins(0, skip, 0, skip));
		qr->resize(st::boxWideWidth, size + 2 * skip);
		qr->paintRequest() | rpl::on_next([=] {
			auto p = QPainter(qr);
			const auto x = (qr->width() - size) / 2;
			auto hq = PainterHighQualityEnabler(p);
			p.setPen(Qt::NoPen);
			p.setBrush(Qt::white);
			p.drawRoundedRect(
				QRect(x - skip, 0, size + 2 * skip, size + 2 * skip),
				skip,
				skip);
			p.drawImage(x, skip, *image);
		}, qr->lifetime());
		box->addRow(
			object_ptr<Ui::FlatLabel>(
				box,
				u"На телефоне откройте GGTG → «Главная» → «Синхронизация» "
				"→ «Сканировать QR-код с Desktop» и наведите камеру.\n\n"
				"Аккаунт "_q + accountLabel + u" должен быть вошедшим и "
				"на телефоне. Дальше всё синхронизируется само."_q,
				st::boxLabel),
			st::boxRowPadding);
		box->addRow(
			object_ptr<Ui::FlatLabel>(
				box,
				u"В коде ключ шифрования ("_q
					+ Core::GgtgSync::KeyId(settings.passphrase)
					+ u"): не показывайте его посторонним."_q,
				st::boxDividerLabel),
			st::boxRowPadding);
		box->addButton(rpl::single(u"Готово"_q), [=] { box->closeBox(); });
	}));
}

class SyncStatusCard final : public Ui::RpWidget {
public:
	SyncStatusCard(not_null<QWidget*> parent) : RpWidget(parent.get()) {
		resize(width(), style::ConvertScale(80));
	}

	void setStatus(
			const QString &title,
			const QString &subtitle,
			QColor accent) {
		_title = title;
		_subtitle = subtitle;
		_accent = accent;
		update();
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		auto p = Painter(this);
		auto hq = PainterHighQualityEnabler(p);
		const auto r = style::ConvertScale(12);
		auto bg = _accent;
		bg.setAlphaF(0.10);
		p.setPen(QPen(_accent, style::ConvertScale(1.5)));
		p.setBrush(bg);
		p.drawRoundedRect(rect().adjusted(1, 1, -1, -1), r, r);

		const auto pad = style::ConvertScale(16);
		const auto dotSize = style::ConvertScale(10);
		p.setPen(Qt::NoPen);
		p.setBrush(_accent);
		p.drawEllipse(pad, pad + style::ConvertScale(3), dotSize, dotSize);

		const auto textX = pad + dotSize + style::ConvertScale(10);
		p.setFont(st::semiboldFont);
		p.setPen(st::windowBoldFg);
		p.drawText(
			QRect(textX, pad, width() - textX - pad, st::semiboldFont->height),
			Qt::AlignLeft | Qt::AlignVCenter,
			_title);

		p.setFont(st::normalFont);
		p.setPen(st::windowSubTextFg);
		p.drawText(
			QRect(textX, pad + st::semiboldFont->height + style::ConvertScale(4), width() - textX - pad, st::normalFont->height * 2),
			Qt::AlignLeft | Qt::AlignTop,
			_subtitle);
	}

private:
	QString _title;
	QString _subtitle;
	QColor _accent = QColor(10, 132, 255);
};

inline void FillGgtgSyncBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller) {
	box->setWidth(st::boxWideWidth * 3 / 2);
	const auto layout = box->verticalLayout();

	const auto settings = std::make_shared<Core::GgtgSync::Settings>(
		Core::GgtgSync::LoadSettings());

	// ---- Hero Status Card ----
	const auto statusCard = box->addRow(
		object_ptr<SyncStatusCard>(box),
		st::boxRowPadding);

	struct Failure {
		QString text;
		crl::time at = 0;
	};
	const auto failure = box->lifetime().make_state<Failure>();

	const auto syncNowWrap = box->addRow(
		object_ptr<Ui::VerticalLayout>(box),
		st::boxRowPadding);
	const auto syncNowBtn = syncNowWrap->add(
		object_ptr<Ui::RoundButton>(
			syncNowWrap,
			rpl::single(u"Синхронизировать сейчас"_q),
			st::defaultBoxButton));

	const auto refresh = [=] {
		const auto now = Core::GgtgSync::LoadSettings();
		auto title = QString();
		auto subtitle = QString();
		auto accent = QColor(10, 132, 255);

		if (!failure->text.isEmpty() && crl::now() - failure->at < 15000) {
			title = u"Ошибка синхронизации"_q;
			subtitle = failure->text;
			accent = QColor(255, 69, 58);
		} else if (!now.enabled || now.passphrase.isEmpty()) {
			title = u"Синхронизация выключена"_q;
			subtitle = u"Включите автоматическую синхронизацию ниже для обмена базами и аккаунтами"_q;
			accent = QColor(142, 142, 147);
		} else if (!Core::GgtgSync::MainSession()) {
			title = u"Требуется главный аккаунт"_q;
			subtitle = u"Главный аккаунт не выбран или не подключён в этой программе"_q;
			accent = QColor(255, 159, 10);
		} else if (Core::GgtgSync::Running()) {
			title = u"Выполняется синхронизация…"_q;
			subtitle = u"Передаются базы диалогов, шаблоны, аккаунты и память ИИ"_q;
			accent = QColor(10, 132, 255);
		} else if (now.lastSyncAt) {
			title = u"Синхронизировано"_q;
			const auto keyId = Core::GgtgSync::KeyId(now.passphrase);
			subtitle = QDateTime::fromSecsSinceEpoch(now.lastSyncAt).toString(u"dd.MM.yyyy HH:mm"_q)
				+ u" · "_q + now.lastResult
				+ (keyId.isEmpty() ? QString() : (u" · Ключ: "_q + keyId));
			accent = QColor(52, 199, 89);
		} else {
			title = u"Ожидание синхронизации"_q;
			subtitle = u"Синхронизация запустится в фоновом режиме через несколько секунд"_q;
			accent = QColor(10, 132, 255);
		}
		statusCard->setStatus(title, subtitle, accent);
	};
	refresh();
	const auto ticker = box->lifetime().make_state<base::Timer>(refresh);
	ticker->callEach(1000);

	const auto save = [=] {
		Core::GgtgSync::SaveSettings(*settings);
		Core::GgtgSync::Restart();
		refresh();
	};

	syncNowBtn->setClickedCallback([=] {
		if (!Core::GgtgSync::Running()) {
			Core::GgtgSync::RunSync([=](Core::GgtgSync::Report report) {
				if (!report.ok) {
					failure->text = u"Не вышло: "_q + report.text;
					failure->at = crl::now();
				} else {
					failure->text = QString();
				}
				refresh();
			});
			refresh();
		}
	});

	Ui::AddSkip(layout, st::defaultVerticalListSkip);

	// ---- Transport account section ----
	Ui::AddSubsectionTitle(layout, rpl::single(u"Главный аккаунт (транспорт данных)"_q));
	const auto entries = std::make_shared<
		std::vector<std::pair<QString, QString>>>();
	for (const auto &[index, account] : Core::App().domain().accounts()) {
		if (!Main::AccountIsLive(account.get())
			|| !account->sessionExists()) {
			continue;
		}
		const auto user = account->session().user();
		const auto username = user->username();
		entries->emplace_back(
			QString::number(user->id.value),
			user->name()
				+ (username.isEmpty() ? QString() : (u"  @"_q + username)));
	}
	const auto labelOf = [=](const QString &id) {
		for (const auto &[entryId, label] : *entries) {
			if (entryId == id) {
				return label;
			}
		}
		return u"не выбран"_q;
	};

	const auto accountCard = box->addRow(
		object_ptr<Ggtg::ActionRow>(
			box,
			rpl::single(labelOf(settings->mainUserId)),
			u"A"_q),
		st::boxRowPadding);

	accountCard->setClickedCallback([=] {
		box->uiShow()->show(Box([=](not_null<Ui::GenericBox*> pick) {
			pick->setTitle(rpl::single(u"Выбрать главный аккаунт"_q));
			pick->setWidth(st::boxWideWidth);
			const auto search = pick->addRow(
				object_ptr<Ui::InputField>(
					pick,
					st::defaultInputField,
					rpl::single(u"Поиск по имени или @username"_q)),
				st::boxRowPadding);
			const auto list = pick->addRow(
				object_ptr<Ui::VerticalLayout>(pick),
				style::margins());
			const auto fill = [=] {
				list->clear();
				const auto query = search->getLastText().trimmed().toLower();
				auto shown = 0;
				for (const auto &[id, label] : *entries) {
					if (!query.isEmpty()
						&& !label.toLower().contains(query)) {
						continue;
					}
					if (++shown > 40) {
						list->add(
							object_ptr<Ui::FlatLabel>(
								list,
								u"Показаны первые 40 — уточните поиск."_q,
								st::boxDividerLabel),
							st::boxRowPadding);
						break;
					}
					const auto button = list->add(
						object_ptr<Ui::LinkButton>(
							list,
							((id == settings->mainUserId)
								? u"● "_q
								: u"○ "_q) + label),
						style::margins(
							st::boxRowPadding.left(),
							st::defaultVerticalListSkip,
							st::boxRowPadding.right(),
							0));
					button->setClickedCallback([=] {
						settings->mainUserId = id;
						save();
						pick->closeBox();
					});
				}
			};
			search->changes() | rpl::on_next(fill, search->lifetime());
			fill();
			pick->addButton(tr::lng_close(), [=] { pick->closeBox(); });
		}));
	});

	box->addRow(
		object_ptr<Ui::FlatLabel>(
			box,
			entries->empty()
				? u"Нет вошедших аккаунтов. Добавьте хотя бы один аккаунт в программу."_q
				: u"Через «Избранное» этого аккаунта безопасно передаются зашифрованные базы, память ИИ и списки рассылки между Desktop и телефоном. Рекомендуется использовать личный аккаунт."_q,
			st::boxDividerLabel),
		st::boxRowPadding);
	Ui::AddSkip(layout, st::defaultVerticalListSkip);

	// ---- Pairing section ----
	Ui::AddSubsectionTitle(layout, rpl::single(u"Подключение телефона (Android)"_q));
	const auto automatic = box->addRow(
		object_ptr<Ui::Checkbox>(
			box,
			u"Автоматическая синхронизация (каждые 5 минут и при изменениях)"_q,
			settings->enabled && !settings->passphrase.isEmpty()),
		st::boxRowPadding);
	automatic->checkedChanges(
	) | rpl::on_next([=](bool checked) {
		settings->enabled = checked;
		if (checked && settings->passphrase.isEmpty()) {
			settings->passphrase = Core::GgtgSync::NewPassphrase();
		}
		save();
	}, automatic->lifetime());

	const auto pairWrap = box->addRow(
		object_ptr<Ui::VerticalLayout>(box),
		st::boxRowPadding);
	const auto pairBtn = pairWrap->add(
		object_ptr<Ui::RoundButton>(
			pairWrap,
			rpl::single(u"Подключить телефон по QR-коду…"_q),
			st::defaultBoxButton));
	pairBtn->setClickedCallback([=] {
		if (settings->mainUserId.isEmpty()
			|| !Core::GgtgSync::MainSession()) {
			Ggtg::Notice(
				box->uiShow(),
				u"Сначала выберите главный аккаунт"_q);
			return;
		}
		if (settings->passphrase.isEmpty()) {
			settings->passphrase = Core::GgtgSync::NewPassphrase();
		}
		if (!settings->enabled) {
			settings->enabled = true;
			automatic->setChecked(true);
		}
		save();
		ShowGgtgSyncQrBox(
			box->uiShow(),
			*settings,
			labelOf(settings->mainUserId));
	});

	// ---- Manual key drawer ----
	const auto manual = box->addRow(
		object_ptr<Ui::LinkButton>(
			box,
			u"Ключ шифрования вручную (без QR-кода)  ▾"_q),
		st::boxRowPadding);
	const auto phraseWrap = box->addRow(
		object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			box,
			object_ptr<Ui::VerticalLayout>(box)),
		style::margins());
	const auto phraseLayout = phraseWrap->entity();
	const auto phrase = phraseLayout->add(
		object_ptr<Ui::InputField>(
			phraseLayout,
			st::defaultInputField,
			rpl::single(u"Ключ шифрования (одинаковый на Desktop и телефоне)"_q),
			settings->passphrase),
		st::boxRowPadding);
	phraseLayout->add(
		object_ptr<Ui::FlatLabel>(
			phraseLayout,
			u"Ключ создаётся автоматически при подключении по QR-коду. Если изменить его вручную, телефон нужно подключить заново."_q,
			st::boxDividerLabel),
		st::boxRowPadding);
	phraseWrap->hide(anim::type::instant);
	manual->setClickedCallback([=] {
		phraseWrap->toggle(phraseWrap->isHidden(), anim::type::normal);
	});
	phrase->changes(
	) | rpl::on_next([=] {
		settings->passphrase = phrase->getLastText().trimmed();
		save();
	}, phrase->lifetime());
	Ui::AddSkip(layout, st::defaultVerticalListSkip);

	// ---- Accounts exchange ----
	Ui::AddSubsectionTitle(layout, rpl::single(u"Обмен аккаунтами между устройствами"_q));
	box->addRow(
		object_ptr<Ui::FlatLabel>(
			box,
			u"Каждому аккаунту при переносе создаётся новая изолированная сессия, поэтому сессии не рвутся: аккаунт одновременно работает на Desktop и Android."_q,
			st::boxDividerLabel),
		st::boxRowPadding);

	const auto send = box->addRow(
		object_ptr<Ggtg::ActionRow>(
			box,
			rpl::single(u"Отправить аккаунты на Android…"_q),
			u"↑"_q),
		st::boxRowPadding);
	send->setClickedCallback([=] {
		box->uiShow()->show(MakeGgtgExportBox(controller, true));
	});

	const auto receive = box->addRow(
		object_ptr<Ggtg::ActionRow>(
			box,
			rpl::single(u"Принять аккаунты с Android…"_q),
			u"↓"_q),
		st::boxRowPadding);
	receive->setClickedCallback([=] {
		Core::GgtgSync::FindIncoming([=](
				std::vector<Core::GgtgSync::Remote> list,
				QString error) {
			if (!error.isEmpty()) {
				Ggtg::Notice(box->uiShow(), error);
				return;
			}
			if (list.empty()) {
				Ggtg::Notice(
					box->uiShow(),
					u"С Android пока ничего не пришло"_q);
				return;
			}
			AcceptAccountsFromAndroid(box, list.front());
		});
	});

	const auto own = box->addRow(
		object_ptr<Ui::LinkButton>(
			box,
			u"Свой api_id и api_hash (если Telegram отказывает в новой сессии)…"_q),
		st::boxRowPadding);
	own->setClickedCallback([=] { ShowApiOverrideBox(box->uiShow()); });

	box->addButton(tr::lng_close(), [=] { box->closeBox(); });
}

[[nodiscard]] inline object_ptr<Ui::BoxContent> MakeGgtgSyncBox(
		not_null<Window::SessionController*> controller) {
	return Box([=](not_null<Ui::GenericBox*> box) {
		FillGgtgSyncBox(box, controller);
	});
}

} // namespace Settings
