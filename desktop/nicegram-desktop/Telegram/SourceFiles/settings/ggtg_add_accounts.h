/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "boxes/connection_box.h"
#include "core/application.h"
#include "core/ggtg_sync_transport.h"
#include "lang/lang_keys.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "data/data_user.h"
#include "mtproto/facade.h"
#include "mtproto/mtproto_proxy_data.h"
#include "mtproto/proxy_pool.h"
#include "core/ggtg_sessions.h"
#include "storage/storage_tdata_export.h"
#include "settings/bulk_accounts_actions.h"
#include "settings/ggtg_sync_box.h"
#include "settings/ggtg_session_json.h"
#include "ui/boxes/confirm_box.h"
#include "ui/layers/generic_box.h"
#include "ui/layers/show.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"
#include "window/ggtg_widgets.h"
#include "window/window_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QPointer>
#include <QtCore/QRegularExpression>
#include <QtWidgets/QFileDialog>

namespace Settings {

// The ways to add accounts: the "Добавить" tab of «Аккаунты», the side
// menu's "Add account" button and the GGTG button of the sign-in screen
// (when there are no accounts yet, so nothing here may need a session).
// A proxy is mandatory everywhere: a new account never makes its first
// connection from this machine's own address.
namespace AddAccounts {

// Sellers' 2FA notes come wrapped in punctuation: "(TelegramExpert.pro:
// ckxk7)". Peels a leading/trailing run of it off.
[[nodiscard]] inline QString StripWrapping(QString value) {
	static const auto edges = u"()[]{}<>\"'«»"_q;
	while (!value.isEmpty() && edges.contains(value.front())) {
		value.remove(0, 1);
	}
	while (!value.isEmpty() && edges.contains(value.back())) {
		value.chop(1);
	}
	return value.trimmed();
}

// The password out of such a note: a "<label>: <password>" or "<label> это
// <password>" line, else the first non-empty line as it is.
[[nodiscard]] inline QString ExtractTwoFactor(const QString &raw) {
	static const auto reIs = QRegularExpression(
		u"это\\s*[:=]?\\s*(\\S+)\\s*$"_q,
		QRegularExpression::CaseInsensitiveOption);
	auto firstLine = QString();
	for (const auto &part : raw.split('\n', Qt::SkipEmptyParts)) {
		const auto line = StripWrapping(part.trimmed());
		if (line.isEmpty()) {
			continue;
		}
		if (firstLine.isEmpty()) {
			firstLine = line;
		}
		if (const auto match = reIs.match(line); match.hasMatch()) {
			return StripWrapping(match.captured(1));
		}
		const auto colon = line.lastIndexOf(':');
		if (colon >= 0) {
			const auto after = StripWrapping(line.mid(colon + 1));
			if (!after.isEmpty()) {
				return after;
			}
		}
	}
	return firstLine;
}

// Purchased accounts come as a folder with the tdata directory and a text
// file with the 2FA password beside it ("2fa.txt", "twoFA.txt"): looked for
// in the tdata folder's parent first, then in the folder itself.
[[nodiscard]] inline QString FindSiblingTwoFactor(const QString &tdataFolder) {
	const auto dirs = std::vector<QDir>{
		QFileInfo(tdataFolder).dir(),
		QDir(tdataFolder),
	};
	for (const auto &dir : dirs) {
		for (const auto &info : dir.entryInfoList(QDir::Files)) {
			const auto name = info.fileName();
			if (!name.contains(u"2fa"_q, Qt::CaseInsensitive)
				&& !name.contains(u"twofa"_q, Qt::CaseInsensitive)) {
				continue;
			}
			auto file = QFile(info.absoluteFilePath());
			if (file.open(QIODevice::ReadOnly)) {
				const auto text = ExtractTwoFactor(
					QString::fromUtf8(file.readAll()));
				if (!text.isEmpty()) {
					return text;
				}
			}
		}
		auto ggtgFile = QFile(dir.filePath(u"ggtg.json"_q));
		if (ggtgFile.open(QIODevice::ReadOnly)) {
			const auto doc = QJsonDocument::fromJson(ggtgFile.readAll());
			if (doc.isObject()) {
				const auto obj = doc.object();
				for (const auto &key : { u"twoFactor"_q, u"twoFA"_q, u"password"_q, u"two_factor"_q }) {
					if (obj.contains(key) && !obj.value(key).toString().isEmpty()) {
						return obj.value(key).toString();
					}
				}
			}
		}
	}
	return QString();
}

[[nodiscard]] inline QString DigitsOnly(const QString &text) {
	auto result = QString();
	for (const auto &ch : text) {
		if (ch.isDigit()) {
			result += ch;
		}
	}
	return result;
}

inline void Inform(std::shared_ptr<Ui::Show> show, const QString &text) {
	if (show->valid()) {
		show->showBox(Ui::MakeInformBox(text));
	}
}

[[nodiscard]] inline bool LimitReached(std::shared_ptr<Ui::Show> show) {
	auto &domain = Core::App().domain();
	if (domain.accounts().size() < domain.maxAccounts()) {
		return false;
	}
	Inform(show, u"Достигнут лимит аккаунтов в программе."_q);
	return true;
}

// The folder, proxy and 2FA of a freshly imported account from its
// "ggtg.json" (written by this app's export), else the folder is taken from
// where it sits: "<picked>/<folder>/<phone>/tdata", or "<picked>/<phone>/
// tdata" for the picked folder's own name.
inline void ApplyImportedMeta(
		not_null<::Main::Account*> account,
		const QString &picked,
		const QString &source) {
	const auto twoFactor = FindSiblingTwoFactor(source);
	if (!twoFactor.isEmpty()) {
		account->setTwoFactorPassword(twoFactor);
	}
	auto info = QJsonObject();
	for (const auto &dir : { QFileInfo(source).dir(), QDir(source) }) {
		auto file = QFile(dir.filePath(u"ggtg.json"_q));
		if (file.open(QIODevice::ReadOnly)) {
			info = QJsonDocument::fromJson(file.readAll()).object();
			break;
		}
	}
	const auto cleanName = [](QString name) {
		name.remove(QRegularExpression(u"(:\\s*\\d+|\\s*\\(\\d+\\))$"_q));
		name = name.trimmed();
		return (name == u"Без папки"_q) ? QString() : name;
	};
	auto tag = QString();
	if (info.contains(u"folder"_q)) {
		tag = info.value(u"folder"_q).toString();
	} else {
		auto parts = QDir(picked).relativeFilePath(source)
			.split('/', Qt::SkipEmptyParts);
		if (!parts.isEmpty()
			&& parts.back().compare(u"tdata"_q, Qt::CaseInsensitive) == 0) {
			parts.pop_back();
		}
		if (parts.size() >= 2) {
			tag = cleanName(parts.front());
		} else if (parts.size() == 1
			&& QFileInfo(picked).fileName().compare(
				u"tdata"_q,
				Qt::CaseInsensitive) != 0) {
			tag = cleanName(QFileInfo(picked).fileName());
		}
	}
	if (!tag.isEmpty()) {
		account->setFolderTag(tag);
	}
	if (info.contains(u"twoFactor"_q) && twoFactor.isEmpty()) {
		account->setTwoFactorPassword(info.value(u"twoFactor"_q).toString());
	}
	const auto proxy = info.value(u"proxy"_q).toObject();
	if (!proxy.isEmpty()) {
		auto data = MTP::ProxyData();
		const auto type = proxy.value(u"type"_q).toString();
		data.type = (type == u"socks5"_q)
			? MTP::ProxyData::Type::Socks5
			: (type == u"http"_q)
			? MTP::ProxyData::Type::Http
			: (type == u"mtproto"_q)
			? MTP::ProxyData::Type::Mtproto
			: MTP::ProxyData::Type::None;
		data.host = proxy.value(u"host"_q).toString();
		data.port = proxy.value(u"port"_q).toInt();
		data.user = proxy.value(u"user"_q).toString();
		data.password = proxy.value(u"password"_q).toString();
		if (data.type != MTP::ProxyData::Type::None
			&& !data.host.isEmpty()
			&& data.port != 0) {
			account->setProxyOverride(data);
			// Language / time zone follow the proxy, unless the export
			// carried this account's own device data.
			if (!info.contains(u"device"_q)) {
				AutoDeviceGeo(account);
			}
		}
	}

	// Persist the imported session in Session Vault
	if (account->sessionExists() && account->session().user()) {
		namespace Sessions = Core::GgtgSessions;
		const auto userId = QString::number(account->session().userId().bare);
		const auto existing = Sessions::StoredForUser(Sessions::LoadVault(), userId);
		if (existing.empty()) {
			auto entry = Sessions::StoredSession();
			entry.userId = userId;
			entry.role = u"purchased"_q;
			entry.source = u"import_tdata"_q;
			entry.note = u"исходная сессия (купленная tdata)"_q;
			entry.phone = account->session().user()->phone();
			entry.name = account->session().user()->name();
			entry.username = account->session().user()->username();
			entry.folder = account->folderTag();
			entry.twoFactor = account->twoFactorPassword();
			entry.proxy = info.value(u"proxy"_q).toObject();
			if (entry.proxy.isEmpty() && account->proxyOverride().type != MTP::ProxyData::Type::None) {
				const auto &p = account->proxyOverride();
				entry.proxy = QJsonObject{
					{ "type", int(p.type) },
					{ "host", p.host },
					{ "port", int(p.port) },
					{ "user", p.user },
					{ "password", p.password },
				};
			}
			if (info.contains(u"device"_q)) {
				entry.device = info.value(u"device"_q).toObject();
			}
			entry = Sessions::AddStored(entry);
			auto err = QString();
			Storage::ExportRawAuthorizationAsTdata(
				account->serializeMtpAuthorization(),
				Sessions::VaultFolder(entry),
				&err);
			Sessions::WriteSessionVaultSidecar(entry);
		}
	}
}

} // namespace AddAccounts

// Sign-in by phone number (or QR) through a proxy. On the sign-in screen
// (the active account has no session yet) the proxy goes to that screen's
// own account and the login goes on right there; otherwise a new account
// opens its own sign-in screen.
inline void AddAccountByPhone(
		std::shared_ptr<Ui::Show> show,
		bool newWindow = false) {
	using Environment = MTP::Environment;
	show->showBox(MakeNewProxyBox([=](const MTP::ProxyData &proxy) {
		auto &domain = Core::App().domain();
		if (domain.started() && !domain.active().sessionExists()) {
			if (proxy.type != MTP::ProxyData::Type::None) {
				MTP::GlobalProxyPool().removeMatching(proxy);
			}
			domain.active().setProxyOverride(proxy);
			AddAccounts::Inform(
				show,
				u"Прокси задан. Войдите по номеру телефона или QR-коду "
					"на этом экране."_q);
			return;
		}
		domain.removeRedundantAccounts();
		auto found = false;
		for (const auto &[index, account] : domain.accounts()) {
			if (!account->sessionExists()
				&& account->mtp().environment() == Environment::Production) {
				found = true;
			}
		}
		if (!found && AddAccounts::LimitReached(show)) {
			return;
		}
		// Taken off its pool list only now: a failed attempt shouldn't
		// burn through the list for nothing.
		if (proxy.type != MTP::ProxyData::Type::None) {
			MTP::GlobalProxyPool().removeMatching(proxy);
		}
		if (newWindow) {
			domain.addActivated(Environment::Production, proxy, true);
		} else if (const auto window = Core::App().activePrimaryWindow()) {
			window->preventOrInvoke([=] {
				Core::App().domain().addActivated(
					Environment::Production,
					proxy);
			});
		} else {
			domain.addActivated(Environment::Production, proxy);
		}
	}));
}

// A tdata folder (one account or a folder of many): every account gets its
// own proxy from the chosen list, and its 2FA / folder / proxy from the
// files beside it.
inline void ImportTdataFolder(std::shared_ptr<Ui::Show> show) {
	show->showBox(MakeMultiProxyBox([=](Fn<MTP::ProxyData()> nextProxy) {
		const auto folder = QFileDialog::getExistingDirectory(
			nullptr,
			u"Выберите папку tdata (или папку с несколькими)"_q);
		if (folder.isEmpty() || AddAccounts::LimitReached(show)) {
			return;
		}
		auto &domain = Core::App().domain();
		// The 2FA is taken per account from the folder it was read from:
		// the phone numbers aren't known yet at this point.
		const auto imported = domain.addFromTdata(
			folder,
			MTP::Environment::Production,
			nextProxy,
			[=](not_null<::Main::Account*> account, const QString &source) {
				AddAccounts::ApplyImportedMeta(account, folder, source);
			});
		if (imported.empty()) {
			AddAccounts::Inform(
				show,
				u"Не удалось импортировать эту папку tdata: она либо "
					"нечитаема, либо все аккаунты из неё уже добавлены."_q);
			return;
		}
		const auto hadSession = domain.started()
			&& domain.active().sessionExists();
		if (!hadSession) {
			// From the sign-in screen: straight into the first one.
			domain.activate(imported.front());
		}
		if (imported.size() > 1 || hadSession) {
			AddAccounts::Inform(
				show,
				u"Добавлено аккаунтов: %1. Те, кому не хватило прокси из "
					"списка, покажут «отключено» - задайте прокси правым "
					"кликом по аккаунту."_q.arg(imported.size()));
		}
	}));
}

// For accounts already in the app: picks the parent folder holding
// "<phone>/tdata" (+ the 2FA note) subfolders and updates just the saved
// 2FA password of each account matched by phone number.
inline void UpdateTwoFactorFromFolder(std::shared_ptr<Ui::Show> show) {
	const auto root = QFileDialog::getExistingDirectory(
		nullptr,
		u"Выберите папку с подпапками по номеру телефона для каждого "
			"аккаунта"_q);
	if (root.isEmpty()) {
		return;
	}
	auto accountsByPhone = base::flat_map<QString, not_null<::Main::Account*>>();
	for (const auto &account : Core::App().domain().orderedAccounts()) {
		if (!account->sessionExists()) {
			continue;
		}
		const auto digits = AddAccounts::DigitsOnly(
			account->session().user()->phone());
		if (!digits.isEmpty()) {
			accountsByPhone.emplace(digits, account);
		}
	}
	auto checked = 0;
	auto updated = 0;
	for (const auto &info
			: QDir(root).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
		const auto digits = AddAccounts::DigitsOnly(info.fileName());
		const auto match = digits.isEmpty()
			? accountsByPhone.end()
			: accountsByPhone.find(digits);
		if (match == accountsByPhone.end()) {
			continue;
		}
		++checked;
		const auto twoFactor = AddAccounts::FindSiblingTwoFactor(
			info.absoluteFilePath());
		if (!twoFactor.isEmpty()) {
			match->second->setTwoFactorPassword(twoFactor);
			++updated;
		}
	}
	AddAccounts::Inform(
		show,
		u"Папок с подходящим по телефону аккаунтом: %1. Обновлено "
			"паролей 2FA: %2."_q.arg(checked).arg(updated));
}

// "Добавить": the proxy lists first (every new account takes its own proxy
// from them), then the ways to add accounts. Needs no session, so the
// sign-in screen shows it too (`standalone` gives it a title and "Закрыть").
inline void FillAddAccountsBox(
		not_null<Ui::GenericBox*> box,
		bool standalone) {
	if (standalone) {
		box->setTitle(rpl::single(u"GGTG"_q));
	}
	box->setWidth(st::boxWideWidth);
	const auto layout = box->verticalLayout();
	const auto show = box->uiShow();
	const auto action = [&](
			const QString &text,
			const style::icon &icon,
			Fn<void()> callback) {
		box->addRow(object_ptr<Ggtg::ActionRow>(
			box,
			rpl::single(text),
			QString(),
			&icon)
		)->setClickedCallback(std::move(callback));
		Ui::AddSkip(layout, st::defaultVerticalListSkip);
	};
	const auto hint = [&](const QString &text) {
		box->addRow(
			object_ptr<Ui::FlatLabel>(box, text, st::boxDividerLabel),
			st::boxRowPadding);
		Ui::AddSkip(layout, st::defaultVerticalListSkip);
	};

	Ui::AddSubsectionTitle(layout, rpl::single(u"1. Прокси"_q));
	const auto lists = MTP::GlobalProxyPool().lists();
	auto total = 0;
	for (const auto &list : lists) {
		total += int(list.entries.size());
	}
	hint(lists.empty()
		? u"Списков прокси ещё нет. Добавьте их до аккаунтов: каждому "
			"новому аккаунту выдаётся свой прокси из списка."_q
		: (u"Списков: "_q + QString::number(lists.size())
			+ u", свободных прокси: "_q + QString::number(total) + u"."_q));
	action(u"Списки прокси…"_q, st::menuIconNetwork, [=] {
		show->showBox(MakeProxyPoolManagerBox());
	});

	Ui::AddSubsectionTitle(layout, rpl::single(u"2. Аккаунты"_q));
	action(u"Импорт папки tdata…"_q, st::menuIconShowInFolder, [=] {
		ImportTdataFolder(show);
	});
	hint(u"Одна папка tdata или папка с несколькими. Пароль 2FA берётся "
		"из файла рядом (2fa.txt, twoFA.txt), папка аккаунта - из имени "
		"папки."_q);
	action(u"Импорт Сессия + JSON…"_q, st::menuIconDownload, [=] {
		ImportSessionJsonFolder(show);
	});
	hint(u"Папка с файлами .session и .json (Telethon, Pyrogram, софт) или архив. "
		"Пароль 2FA, параметры устройства и прокси применяются автоматически."_q);
	action(u"Войти по номеру телефона…"_q, st::menuIconPhone, [=] {
		AddAccountByPhone(show);
	});
	if (Core::GgtgSync::MainSession()) {
		const auto weak = QPointer<Ui::GenericBox>(box.get());
		action(u"Принять аккаунты с Android…"_q, st::menuIconDevices, [=] {
			Core::GgtgSync::FindIncoming([=](
					std::vector<Core::GgtgSync::Remote> list,
					QString error) {
				if (!weak) {
					return;
				} else if (!error.isEmpty()) {
					AddAccounts::Inform(show, error);
				} else if (list.empty()) {
					AddAccounts::Inform(show, u"С Android пока ничего не пришло"_q);
				} else {
					AcceptAccountsFromAndroid(weak.data(), list.front());
				}
			});
		});
	}
	auto any = false;
	for (const auto &[index, account] : Core::App().domain().accounts()) {
		any = any || account->sessionExists();
	}
	if (any) {
		action(u"Обновить 2FA из папки…"_q, st::menuIconLock, [=] {
			UpdateTwoFactorFromFolder(show);
		});
	}
	if (standalone) {
		box->addButton(tr::lng_close(), [=] { box->closeBox(); });
	}
}

[[nodiscard]] inline object_ptr<Ui::BoxContent> MakeAddAccountsBox(
		bool standalone = false) {
	return Box([=](not_null<Ui::GenericBox*> box) {
		FillAddAccountsBox(box, standalone);
	});
}

} // namespace Settings
