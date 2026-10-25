/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "settings/ggtg_export.h"
#include "core/ggtg_log.h"
#include "core/ggtg_geo.h"

#include "base/random.h"
#include "base/weak_ptr.h"
#include "base/zlib_help.h"
#include "core/application.h"
#include "core/process_manager.h"
#include "data/data_user.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "main/session_duplicate.h"
#include "mtproto/mtproto_proxy_data.h"
#include "settings/bulk_accounts_actions.h"
#include "settings/ggtg_android_profiles.h"
#include "settings/process_box.h"
#include "ui/layers/generic_box.h"
#include "ui/ui_utility.h"
#include "ui/vertical_list.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "window/ggtg_click.h"
#include "window/ggtg_widgets.h"
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_widgets.h"
#include "styles/style_window_main_menu.h"

#include "core/ggtg_sync_transport.h"
#include "core/ggtg_sessions.h"
#include "storage/storage_tdata_import.h"
#include "settings/ggtg_api_override_box.h"
#include "base/call_delayed.h"

#include <QtCore/QDataStream>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QStandardPaths>
#include <QtCore/QTimer>
#include <QtWidgets/QFileDialog>

namespace Settings {

namespace {

constexpr auto kPackFormat = 1;
constexpr auto kWideIdsTag = ~quint64(0);
constexpr auto kAuthKeySize = 256;
// Random pause between two accounts' duplications, so a batch doesn't hit
// auth.exportLoginToken for many users from one IP back to back.
constexpr auto kMinPauseMs = 3000;
constexpr auto kMaxPauseMs = 8000;

struct ExportOptions {
	bool proxy = true;
	bool twoFactor = true;
	bool folders = true;
	// Not a file for the user: put into the main account's "Saved messages"
	// for GGTG Android (see core/ggtg_sync_transport.h).
	bool viaSync = false;
};

} // namespace

// Reads back Main::SerializeMtpAuthorization()'s wide-ids format: tag,
// userId, mainDcId, then the key list (dcId + raw 256-byte key each). The
// keys-to-destroy list after it is irrelevant for an export.
[[nodiscard]] std::optional<ParsedAuthorization> ParseAuthorization(
		const QByteArray &serialized) {
	QDataStream stream(serialized);
	stream.setVersion(QDataStream::Qt_5_1);
	auto tag = quint64();
	auto result = ParsedAuthorization();
	auto count = qint32();
	stream >> tag >> result.userId >> result.mainDcId >> count;
	if (stream.status() != QDataStream::Ok
		|| tag != kWideIdsTag
		|| !result.userId
		|| count <= 0
		|| count > 16) {
		return std::nullopt;
	}
	for (auto i = 0; i != count; ++i) {
		auto dcId = qint32();
		stream >> dcId;
		auto key = QByteArray(kAuthKeySize, Qt::Uninitialized);
		if (stream.readRawData(key.data(), kAuthKeySize) != kAuthKeySize
			|| stream.status() != QDataStream::Ok) {
			return std::nullopt;
		}
		result.keys.emplace_back(dcId, key);
	}
	return result;
}

[[nodiscard]] QJsonObject ProxyToJson(const MTP::ProxyData &proxy) {
	using Type = MTP::ProxyData::Type;
	auto result = QJsonObject();
	switch (proxy.type) {
	case Type::Socks5: result["type"] = u"socks5"_q; break;
	case Type::Http: result["type"] = u"http"_q; break;
	case Type::Mtproto: result["type"] = u"mtproto"_q; break;
	default: return QJsonObject();
	}
	result["host"] = proxy.host;
	result["port"] = int(proxy.port);
	if (proxy.type == Type::Mtproto) {
		// ProxyData keeps an MTProto proxy's secret in `password`.
		result["secret"] = proxy.password;
	} else {
		result["user"] = proxy.user;
		result["password"] = proxy.password;
	}
	return result;
}

namespace {

[[nodiscard]] QString ExportAccountLabel(not_null<Main::Account*> account) {
	const auto user = account->session().user();
	auto name = user->firstName;
	if (!user->lastName.isEmpty()) {
		name += ' ' + user->lastName;
	}
	auto result = name.trimmed();
	const auto phone = user->phone();
	if (!phone.isEmpty()) {
		result += (result.isEmpty() ? QString() : u" · "_q) + '+' + phone;
	}
	if (!user->username().isEmpty()) {
		result += u" · @"_q + user->username();
	}
	return result.isEmpty()
		? u"ID %1"_q.arg(user->id.value)
		: result;
}

// Everything about one account the .ggtg manifest carries except its keys,
// captured before the duplication starts (the account may go away while
// the batch runs).
[[nodiscard]] QJsonObject AccountMeta(
		not_null<Main::Account*> account,
		const ExportOptions &options) {
	const auto user = account->session().user();
	auto result = QJsonObject();
	result["phone"] = user->phone();
	result["firstName"] = user->firstName;
	result["lastName"] = user->lastName;
	result["username"] = user->username();
	result["pinned"] = account->pinned();
	if (options.folders) {
		result["folder"] = account->folderTag();
	}
	if (options.twoFactor && !account->twoFactorPassword().isEmpty()) {
		result["twoFactor"] = account->twoFactorPassword();
	}
	// The target app gets its own random device profile in the exported manifest.
	// If the account has an iPhone profile, preserve the iOS device identity.
	const auto effective = EffectiveDeviceOverride(account);
	const auto phone = user->phone();
	auto targetDev = IsIosSession(effective)
		? RandomIosDeviceOverride(phone)
		: RandomAndroidDeviceOverride(phone);
	if (!phone.isEmpty()) {
		const auto geo = Core::GgtgGeo::MatchForPhone(phone);
		targetDev.cloudLangCode = geo.cloudLang;
		targetDev.systemLangCode = geo.systemLang;
		targetDev.tzOffset = geo.tzOffset;
	} else {
		if (const auto known = Ggtg::AndroidProfiles::LanguageForSystemCode(
				effective.systemLangCode)) {
			targetDev.cloudLangCode = known->cloud;
			targetDev.systemLangCode = known->system;
		}
		if (effective.tzOffset) {
			targetDev.tzOffset = effective.tzOffset;
		}
	}
	result["device"] = DeviceOverrideToJson(targetDev);
	if (options.proxy) {
		auto proxy = ProxyToJson(account->proxyOverride());
		if (proxy.isEmpty()) {
			const auto &selected = Core::App().settings().proxy().selected();
			if (selected && Core::App().settings().proxy().isEnabled()) {
				proxy = ProxyToJson(*selected);
			}
		}
		if (!proxy.isEmpty()) {
			result["proxy"] = proxy;
		}
	}
	return result;
}

} // namespace

[[nodiscard]] bool WriteGgtgPack(
		const QString &path,
		const QJsonArray &accounts) {
	auto manifest = QJsonObject();
	manifest["format"] = kPackFormat;
	manifest["kind"] = u"ggtg-android-export"_q;
	manifest["createdAt"] = QDateTime::currentSecsSinceEpoch();
	manifest["accounts"] = accounts;
	const auto json = QJsonDocument(manifest).toJson(
		QJsonDocument::Indented);

	zlib::FileToWrite zip;
	zip_fileinfo zfi = { { 0, 0, 0, 0, 0, 0 }, 0, 0, 0 };
	zip.openNewFile(
		"manifest.json",
		&zfi,
		nullptr,
		0,
		nullptr,
		0,
		nullptr,
		Z_DEFLATED,
		Z_DEFAULT_COMPRESSION);
	zip.writeInFile(json.constData(), json.size());
	zip.closeFile();
	zip.close();
	if (zip.error() != ZIP_OK) {
		return false;
	}
	auto file = QFile(path);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		return false;
	}
	const auto bytes = zip.result();
	return file.write(bytes) == bytes.size();
}

[[nodiscard]] bool WritePack(
		const QString &path,
		const QJsonArray &accounts) {
	return WriteGgtgPack(path, accounts);
}

[[nodiscard]] QJsonArray ReadGgtgPack(const QString &path) {
	auto file = QFile(path);
	if (!file.open(QIODevice::ReadOnly)) {
		return QJsonArray();
	}
	const auto bytes = file.readAll();
	file.close();
	auto zip = zlib::FileToRead(bytes);
	const auto content = zip.readFileContent("manifest.json", 1, 10 * 1024 * 1024);
	if (content.isEmpty()) {
		return QJsonArray();
	}
	const auto doc = QJsonDocument::fromJson(content);
	if (!doc.isObject()) {
		return QJsonArray();
	}
	return doc.object().value(u"accounts"_q).toArray();
}

[[nodiscard]] bool ExportAccountToGgtg(
		const QString &path,
		const QByteArray &serializedAuth,
		const QString &phone,
		const QString &firstName,
		const QString &lastName,
		const QString &username,
		const QString &twoFactor,
		const MTP::ProxyData &proxy,
		const QJsonObject &device,
		const QString &folder,
		bool append,
		QString *outError) {
	const auto parsed = ParseAuthorization(serializedAuth);
	if (!parsed) {
		if (outError) *outError = u"Не удалось разобрать авторизационные ключи сессии."_q;
		return false;
	}
	auto entry = QJsonObject();
	entry["userId"] = QString::number(parsed->userId);
	entry["mainDcId"] = parsed->mainDcId;
	auto keys = QJsonArray();
	for (const auto &[dcId, key] : parsed->keys) {
		auto k = QJsonObject();
		k["dcId"] = dcId;
		k["key"] = QString::fromLatin1(key.toHex());
		keys.push_back(k);
	}
	entry["keys"] = keys;
	entry["phone"] = phone;
	entry["firstName"] = firstName;
	entry["lastName"] = lastName;
	entry["username"] = username;
	entry["pinned"] = false;
	if (!folder.isEmpty()) {
		entry["folder"] = folder;
	}
	if (!twoFactor.isEmpty()) {
		entry["twoFactor"] = twoFactor;
	}
	const auto proxyJson = ProxyToJson(proxy);
	if (!proxyJson.isEmpty()) {
		entry["proxy"] = proxyJson;
	}
	if (!device.isEmpty()) {
		entry["device"] = device;
	}
	// Registered natively as mobile - phone installs keys directly without QR handoff!
	entry["handoff"] = false;

	auto accounts = (append && QFile::exists(path)) ? ReadGgtgPack(path) : QJsonArray();
	accounts.push_back(entry);
	if (!WriteGgtgPack(path, accounts)) {
		if (outError) *outError = u"Не удалось сохранить архив .ggtg"_q;
		return false;
	}
	return true;
}

namespace {

// Asked when Main::DuplicateSession() can't finish on the stored 2FA
// password (missing or wrong). Closing the box any other way is a skip.
void AskTwoFactor(
		std::shared_ptr<Ui::Show> show,
		const QString &label,
		Fn<void(QString password)> submit,
		Fn<void()> cancel) {
	show->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"Нужен пароль 2FA"_q));
		box->addRow(object_ptr<Ui::FlatLabel>(
			box.get(),
			u"Для дублирования сессии нужен пароль 2FA, сохранённого нет "
				"(или он не подошёл):\n\n"_q + label,
			st::boxLabel));
		const auto field = box->addRow(object_ptr<Ui::InputField>(
			box.get(),
			st::defaultInputField,
			rpl::single(u"Пароль 2FA"_q)));
		box->setFocusCallback([=] { field->setFocusFast(); });
		const auto handled = box->lifetime().make_state<bool>(false);
		const auto trySubmit = [=] {
			const auto text = field->getLastText();
			if (text.isEmpty()) {
				return;
			}
			*handled = true;
			box->closeBox();
			submit(text);
		};
		field->submits(
		) | rpl::on_next(trySubmit, field->lifetime());
		box->addButton(rpl::single(u"Продолжить"_q), trySubmit);
		box->addButton(rpl::single(u"Пропустить"_q), [=] {
			*handled = true;
			box->closeBox();
			cancel();
		});
		box->boxClosing(
		) | rpl::on_next([=] {
			if (!*handled) {
				*handled = true;
				cancel();
			}
		}, box->lifetime());
	}));
}

struct ExportRun {
	std::shared_ptr<Ui::Show> show;
	std::shared_ptr<Core::ProcessHandle> process;
	std::vector<base::weak_ptr<Main::Account>> accounts;
	std::vector<QString> labels;
	std::vector<QJsonObject> metas;
	ExportOptions options;
	QString path;
	QJsonArray exported;
	QStringList errors;
};

void ExportNext(std::shared_ptr<ExportRun> run, size_t index);

void FinishExport(const std::shared_ptr<ExportRun> &run) {
	const auto &process = run->process;
	if (run->errors.join('\n').contains(u"API_ID_PUBLISHED_FLOOD"_q)
		|| run->errors.join('\n').contains(u"API_ID_INVALID"_q)) {
		// Telegram refuses the shared api_id: offer to use one of the
		// user's own (see settings/ggtg_api_override_box.h).
		const auto show = run->show;
		base::call_delayed(300, [=] { ShowApiOverrideBox(show); });
	}
	if (run->exported.isEmpty()) {
		process->fail(u"Ни один аккаунт не экспортирован:\n"_q
			+ run->errors.join('\n'));
		return;
	}
	if (!WritePack(run->path, run->exported)) {
		process->fail(u"Не удалось записать файл "_q + run->path);
		return;
	}
	if (run->options.viaSync) {
		auto bytes = QByteArray();
		auto file = QFile(run->path);
		if (file.open(QIODevice::ReadOnly)) {
			bytes = file.readAll();
			file.close();
		}
		QFile::remove(run->path);
		const auto count = run->exported.size();
		const auto total = run->accounts.size();
		const auto errors = run->errors;
		Core::GgtgSync::SendAccounts(u"pack"_q, bytes, [=](
				bool ok,
				QString text) {
			auto line = ok
				? u"Готово: %1 из %2 аккаунтов. %3. На телефоне: "
					"«Синхронизация» → «Принять аккаунты»."_q
					.arg(count).arg(total).arg(text)
				: u"Не отправилось: "_q + text;
			if (!errors.isEmpty()) {
				line += u"\nОшибки:\n"_q + errors.join('\n');
			}
			if (ok) {
				process->succeed(line);
			} else {
				process->fail(line);
			}
		});
		return;
	}
	auto line = u"Готово: %1 из %2 аккаунтов в %3"_q
		.arg(run->exported.size())
		.arg(run->accounts.size())
		.arg(QDir::toNativeSeparators(run->path));
	if (!run->errors.isEmpty()) {
		line += u"\nОшибки:\n"_q + run->errors.join('\n');
	}
	process->succeed(line);
}

void ExportNext(std::shared_ptr<ExportRun> run, size_t index) {
	if (index >= run->accounts.size()) {
		FinishExport(run);
		return;
	}
	const auto &process = run->process;
	const auto label = u"[%1/%2] %3"_q
		.arg(index + 1)
		.arg(run->accounts.size())
		.arg(run->labels[index]);
	process->progress(int(index) + 1, int(run->accounts.size()));
	const auto next = [=] {
		if (index + 1 >= run->accounts.size()) {
			ExportNext(run, index + 1);
			return;
		}
		const auto pause = kMinPauseMs
			+ base::RandomIndex(kMaxPauseMs - kMinPauseMs);
		QTimer::singleShot(pause, [=] { ExportNext(run, index + 1); });
	};
	const auto fail = [=](const QString &error) {
		process->log(label + u": ошибка - "_q + error);
		run->errors.push_back(run->labels[index] + u": "_q + error);
		next();
	};
	const auto account = run->accounts[index].get();
	if (!account || !account->sessionExists()) {
		fail(u"аккаунт больше не залогинен"_q);
		return;
	}

	// If this account already has an active, valid stored Android session in Session Vault,
	// reuse it directly so we don't spam duplicate sessions or trigger 2FA again.
	namespace Sessions = Core::GgtgSessions;
	const auto userId = QString::number(account->session().userId().bare);
	const auto vault = Sessions::StoredForUser(Sessions::LoadVault(), userId);
	const auto existingAndroid = [&]() -> const Sessions::StoredSession* {
		for (const auto &item : vault) {
			if (item.state != u"revoked"_q
				&& Sessions::HasKey(item)
				&& (item.role == u"exported"_q
					|| item.role == u"android"_q
					|| item.note.contains(u"Android"_q, Qt::CaseInsensitive))) {
				const auto vaultFolder = Sessions::VaultFolder(item) + u"/tdata"_q;
				if (QDir(vaultFolder).exists()) {
					return &item;
				}
			}
		}
		return nullptr;
	}();

	if (existingAndroid) {
		const auto vaultPath = Sessions::VaultFolder(*existingAndroid) + u"/tdata"_q;
		const auto auths = Storage::ReadForeignTdataAuthorizations(vaultPath);
		if (!auths.empty()) {
			const auto parsed = ParseAuthorization(auths.front().data);
			if (parsed) {
				auto entry = run->metas[index];
				if (run->options.twoFactor) {
					if (!existingAndroid->twoFactor.isEmpty()) {
						entry["twoFactor"] = existingAndroid->twoFactor;
					} else if (!account->twoFactorPassword().isEmpty()) {
						entry["twoFactor"] = account->twoFactorPassword();
					}
				}
				if (!existingAndroid->device.isEmpty()) {
					entry["device"] = existingAndroid->device;
				}
				if (!existingAndroid->proxy.isEmpty()) {
					entry["proxy"] = existingAndroid->proxy;
				} else if (entry.value(u"proxy"_q).toObject().isEmpty()) {
					auto proxy = ProxyToJson(account->proxyOverride());
					if (proxy.isEmpty()) {
						const auto &selected = Core::App().settings().proxy().selected();
						if (selected && Core::App().settings().proxy().isEnabled()) {
							proxy = ProxyToJson(*selected);
						}
					}
					if (!proxy.isEmpty()) {
						entry["proxy"] = proxy;
					}
				}
				entry["userId"] = QString::number(parsed->userId);
				entry["mainDcId"] = parsed->mainDcId;
				auto keys = QJsonArray();
				for (const auto &[dcId, key] : parsed->keys) {
					auto item = QJsonObject();
					item["dcId"] = dcId;
					item["key"] = QString::fromLatin1(key.toHex());
					keys.push_back(item);
				}
				entry["keys"] = keys;
				entry["handoff"] = false;
				run->exported.push_back(entry);

				const auto devName = existingAndroid->device.value(u"deviceModel"_q).toString();
				process->log(label + u": использована готовая сессия ("_q
					+ (devName.isEmpty() ? u"Android"_q : devName) + u")"_q);
				next();
				return;
			}
		}
	}

	process->log(label + u": создаём новую сессию..."_q);
	const auto weakAccount = run->accounts[index];
	const auto targetDevice = Settings::DeviceOverrideFromJson(run->metas[index].value(u"device"_q).toObject());
	Main::DuplicateSessionTracked(account, [=](
			Fn<void(QString password)> submit,
			Fn<void()> cancel) {
		process->log(label + u": нужен пароль 2FA, ждём ввода..."_q);
		if (weakAccount) {
			AskTwoFactor(run->show, run->labels[index], submit, cancel);
		} else {
			cancel();
		}
	}, [=](QString error, QByteArray serializedAuth, uint64 hash) {
		if (!error.isEmpty()) {
			fail(error);
			return;
		}
		const auto parsed = ParseAuthorization(serializedAuth);
		if (!parsed) {
			fail(u"не удалось разобрать новую сессию"_q);
			return;
		}
		auto entry = run->metas[index];
		// The 2FA password may have been typed by hand during the
		// duplication (and stored by it) after the meta was snapshotted -
		// take the current stored one so it lands in the file.
		if (run->options.twoFactor && weakAccount) {
			const auto password = weakAccount.get()->twoFactorPassword();
			if (!password.isEmpty()) {
				entry["twoFactor"] = password;
			}
		}
		entry["userId"] = QString::number(parsed->userId);
		entry["mainDcId"] = parsed->mainDcId;
		auto keys = QJsonArray();
		for (const auto &[dcId, key] : parsed->keys) {
			auto item = QJsonObject();
			item["dcId"] = dcId;
			item["key"] = QString::fromLatin1(key.toHex());
			keys.push_back(item);
		}
		entry["keys"] = keys;
		// The session is already born as an authentic target device (Android/iPhone)
		// with its own independent authorization keys - no second handoff needed.
		entry["handoff"] = false;
		run->exported.push_back(entry);

		// Also store the newly generated exported session into Session Vault so it is kept, signed, and switchable
		if (const auto alive = weakAccount.get()) {
			auto storeErr = QString();
			Settings::StoreSessionKey(
				alive,
				u"exported"_q,
				u"экспорт для "_q + (Settings::IsIosSession(targetDevice) ? u"iOS"_q : u"Android"_q),
				serializedAuth,
				hash,
				&storeErr,
				targetDevice);
		}

		process->log(label + u": готово"_q);
		// What Telegram actually recorded for the new session (it is written
		// once, when the session is made): the device, the app and its
		// version, so a wrong identity is seen at once, not on the phone.
		if (const auto alive = weakAccount.get()) {
			Main::FetchSessionDescription(alive, hash, [=](QString text) {
				if (!text.isEmpty()) {
					process->log(label + u": в Telegram сессия записана как: "_q
						+ text);
					Core::GgtgLog::Info(
						u"Экспорт"_q,
						label + u": сессия записана как: "_q + text);
				}
			});
		}
		next();
	}, targetDevice, !Settings::IsIosSession(targetDevice));
}

void StartExport(
		std::shared_ptr<Ui::Show> show,
		std::vector<not_null<Main::Account*>> accounts,
		ExportOptions options) {
	auto path = QString();
	if (options.viaSync) {
		QDir().mkpath(QDir::tempPath() + u"/ggtg-sync"_q);
		path = QDir::tempPath() + u"/ggtg-sync/accounts_out.ggtg"_q;
	} else {
		const auto documents = QStandardPaths::writableLocation(
			QStandardPaths::DesktopLocation);
		const auto suggested = documents + u"/ggtg_android_%1_%2.ggtg"_q
			.arg(QDateTime::currentDateTime().toString(u"yyyyMMdd_HHmm"_q))
			.arg(accounts.size());
		path = QFileDialog::getSaveFileName(
			nullptr,
			u"Сохранить экспорт для Android"_q,
			suggested,
			u"GGTG Android (*.ggtg)"_q);
		if (path.isEmpty()) {
			return;
		}
		if (!path.endsWith(u".ggtg"_q, Qt::CaseInsensitive)) {
			path += u".ggtg"_q;
		}
	}
	const auto run = std::make_shared<ExportRun>();
	run->show = show;
	run->options = options;
	run->path = path;
	for (const auto &account : accounts) {
		run->accounts.push_back(base::make_weak(account.get()));
		run->labels.push_back(ExportAccountLabel(account));
		run->metas.push_back(AccountMeta(account, options));
	}
	const auto title = accounts.size() == 1
		? u"Экспорт для Android"_q
		: u"Экспорт для Android (%1)"_q.arg(accounts.size());
	run->process = Core::App().processes().start(
		title,
		nullptr,
		Core::ProcessKind::Export);
	ShowProcessBox(show, run->process->id());
	ExportNext(run, 0);
}

// ---- account picker: folders that fold/unfold, like in "Отправка SMS" ----

struct PickState : AccountPickerState {
};

} // namespace

// ---- the "what to export / what the app keeps" choices ----

Fn<std::optional<TdataExportOptions>()> AddTdataExportChoices(
		not_null<Ui::GenericBox*> box,
		bool withAndroid) {
	const auto content = box->verticalLayout();
	constexpr auto kFormatTdata = 0;
	constexpr auto kFormatSessionJson = 1;
	constexpr auto kFormatAndroid = 2;

	const auto radio = [&](
			not_null<Ui::VerticalLayout*> to,
			const std::shared_ptr<Ui::RadiobuttonGroup> &group,
			int value,
			const QString &text) {
		const auto button = to->add(
			object_ptr<Ui::Radiobutton>(to, group, value, text),
			st::ggtgChoiceRowPadding);
		button->setAllowTextLines(0); // long options wrap, never "..."
	};
	const auto section = [&] {
		const auto wrap = content->add(
			object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
				content,
				object_ptr<Ui::VerticalLayout>(content)));
		return wrap;
	};

	Ui::AddSubsectionTitle(content, rpl::single(u"Формат экспорта"_q));
	const auto formatGroup = std::make_shared<Ui::RadiobuttonGroup>(kFormatTdata);
	radio(content, formatGroup, kFormatTdata, u"TData (папка Telegram Desktop)"_q);
	radio(content, formatGroup, kFormatSessionJson, u"Сессия + JSON (Telethon / Pyrogram / софт)"_q);
	if (withAndroid) {
		radio(content, formatGroup, kFormatAndroid, u"Для GGTG Android (один файл .ggtg)"_q);
	}

	const auto sessionWrap = section();
	const auto sessionInner = sessionWrap->entity();
	Ui::AddSkip(sessionInner);
	Ui::AddSubsectionTitle(sessionInner, rpl::single(u"Какую сессию отдать"_q));
	const auto source = std::make_shared<Ui::RadiobuttonGroup>(0);
	radio(sessionInner, source, 0, u"Основную (программа перейдёт на резервную)"_q);
	radio(sessionInner, source, 1, u"Резервную (станет дублированной)"_q);
	radio(sessionInner, source, 2, u"Новую дублированную (программа остаётся на своей)"_q);

	const auto backupWrap = section();
	const auto backupInner = backupWrap->entity();
	Ui::AddSkip(backupInner);
	Ui::AddSubsectionTitle(
		backupInner,
		rpl::single(u"Какую резервную отдать"_q));
	const auto pick = std::make_shared<Ui::RadiobuttonGroup>(
		int(TdataBackupPick::Oldest));
	radio(backupInner, pick, int(TdataBackupPick::Oldest), u"Самую старую"_q);
	radio(backupInner, pick, int(TdataBackupPick::Newest), u"Самую новую"_q);
	const auto newWhenNone = backupInner->add(
		object_ptr<Ui::Checkbox>(
			backupInner,
			u"Если резервной нет - отдать новую дублированную"_q,
			false),
		st::ggtgChoiceRowPadding);
	newWhenNone->setAllowTextLines(0);

	const auto sidecarWrap = section();
	const auto sidecarInner = sidecarWrap->entity();
	Ui::AddSkip(sidecarInner);
	const auto sidecars = sidecarInner->add(
		object_ptr<Ui::Checkbox>(
			sidecarInner,
			u"Положить рядом 2fa.txt и ggtg.json (папка, прокси, пароль 2FA)"_q,
			true),
		st::ggtgChoiceRowPadding);
	sidecars->setAllowTextLines(0);
	Ui::AddSkip(content);

	const auto refresh = [=](anim::type animated) {
		const auto isAndroid = (formatGroup->current() == kFormatAndroid);
		sessionWrap->toggle(!isAndroid, animated);
		backupWrap->toggle(!isAndroid && (source->current() == 1), animated);
		sidecarWrap->toggle(!isAndroid, animated);
	};
	refresh(anim::type::instant);
	formatGroup->setChangedCallback([=](int) {
		refresh(anim::type::normal);
	});
	source->setChangedCallback([=](int) {
		refresh(anim::type::normal);
	});

	return [=]() -> std::optional<TdataExportOptions> {
		const auto fmt = formatGroup->current();
		if (fmt == kFormatAndroid) {
			return std::nullopt;
		}
		const auto value = source->current();
		return TdataExportOptions{
			.format = (fmt == kFormatSessionJson)
				? ExportFormat::SessionJson
				: ExportFormat::Tdata,
			.source = (value == 0)
				? TdataSource::Working
				: (value == 1)
				? TdataSource::Backup
				: TdataSource::New,
			.pick = TdataBackupPick(pick->current()),
			.newWhenNoBackup = newWhenNone->checked(),
			.sidecars = sidecars->checked(),
		};
	};
}

object_ptr<Ui::BoxContent> MakeGgtgExportBox(
		not_null<Window::SessionController*> controller,
		bool viaSync) {
	return Box([=](not_null<Ui::GenericBox*> box) {
		box->setWidth(st::boxWideWidth);
		const auto content = box->verticalLayout();

		const auto state = box->lifetime().make_state<PickState>();

		const auto choices = viaSync
			? Fn<std::optional<TdataExportOptions>()>(
				[] { return std::optional<TdataExportOptions>(); })
			: AddTdataExportChoices(box, true);
		if (viaSync) {
			box->setTitle(rpl::single(u"Аккаунты на Android"_q));
			box->addRow(
				object_ptr<Ui::FlatLabel>(
					box,
					u"Каждому выбранному аккаунту выдаётся новая "
					"отдельная сессия, и она уходит в «Избранное» "
					"главного аккаунта (зашифрованная вашей фразой). На "
					"телефоне останется нажать «Принять аккаунты»."_q,
					st::boxLabel),
				st::boxRowPadding);
		}

		Ui::AddSkip(content);
		const auto apiOverrideLink = box->addRow(
			object_ptr<Ui::LinkButton>(box, u"Свой api_id (my.telegram.org)"_q),
			st::boxRowPadding);
		apiOverrideLink->setClickedCallback([=] {
			ShowApiOverrideBox(box->uiShow());
		});
		Ui::AddSkip(content);
		Ui::AddSubsectionTitle(content, rpl::single(u"Аккаунты"_q));
		AddAccountPicker(
			box,
			state,
			u"Нет залогиненных аккаунтов."_q);

		box->addButton(
			state->selectedCount.value() | rpl::map([=](int count) {
				return viaSync
					? (count
						? u"Отправить (%1)"_q.arg(count)
						: u"Отправить"_q)
					: (count
						? u"Экспортировать (%1)"_q.arg(count)
						: u"Экспортировать"_q);
			}),
			[=] {
				// Only accounts that are still there (one may have been
				// removed while this box was open).
				auto chosen = std::vector<not_null<Main::Account*>>();
				for (const auto &account
						: Core::App().domain().orderedAccounts()) {
					if (account->sessionExists()
						&& state->selected.contains(account)) {
						chosen.push_back(account);
					}
				}
				if (chosen.empty()) {
					Ggtg::Notice(box->uiShow(), 
						u"Выберите хотя бы один аккаунт"_q);
					return;
				}
				if (Core::App().processes().running(
						Core::ProcessKind::Export)) {
					Ggtg::Notice(box->uiShow(), 
						u"Экспорт уже идёт - дождитесь его в «Процессах»"_q);
					return;
				}
				if (const auto options = choices()) {
					ExportAccountsAsTdata(
						controller,
						std::move(chosen),
						*options);
					return;
				}
				auto exportOptions = ExportOptions();
				exportOptions.viaSync = viaSync;
				StartExport(
					box->uiShow(),
					std::move(chosen),
					exportOptions);
				if (viaSync) {
					box->closeBox();
				}
			}, st::defaultActiveButton);
	});
}

} // namespace Settings
