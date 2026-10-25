/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "settings/sections/settings_information.h"
#include "window/ggtg_widgets.h"
#include "settings.h"
#include "settings/sms_campaign.h"

#include <QtCore/QDateTime>
#include <QtWidgets/QFileDialog>

#include "settings/sections/settings_main.h"
#include "settings/settings_builder.h"
#include "settings/settings_common_session.h"
#include "settings/business/settings_chatbots.h"
#include "ui/wrap/vertical_layout.h"
#include "ui/wrap/vertical_layout_reorder.h"
#include "ui/wrap/padding_wrap.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/popup_menu.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/box_content_divider.h"
#include "ui/widgets/menu/menu_add_action_callback_factory.h"
#include "ui/boxes/confirm_box.h"
#include "ui/controls/button_context_menu.h"
#include "ui/controls/userpic_button.h"
#include "ui/new_badges.h"
#include "ui/text/text_utilities.h"
#include "ui/delayed_activation.h"
#include "ui/painter.h"
#include "ui/vertical_list.h"
#include "ui/unread_badge_paint.h"
#include "ui/empty_userpic.h"
#include "ui/ui_utility.h"
#include "core/application.h"
#include "core/file_utilities.h"
#include "core/click_handler_types.h"
#include "core/core_settings.h"
#include "core/process_manager.h"
#include "settings/process_box.h"
#include "settings/ggtg_export.h"
#include "settings/ggtg_session_json.h"
#include "core/ggtg_geo.h"
#include "settings/ggtg_add_accounts.h"
#include "chat_helpers/emoji_suggestions_widget.h"
#include "boxes/add_contact_box.h"
#include "boxes/connection_box.h"
#include "mtproto/proxy_pool.h"
#include "boxes/premium_limits_box.h"
#include "boxes/username_box.h"
#include "boxes/peers/edit_peer_color_box.h"
#include "data/account_spam_status.h"
#include "data/business/data_business_chatbots.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "data/data_peer_values.h"
#include "data/data_changes.h"
#include "data/data_channel.h"
#include "data/data_premium_limits.h"
#include "info/profile/info_profile_values.h"
#include "info/profile/info_profile_badge.h"
#include "info/profile/info_profile_phone_menu.h"
#include "lang/lang_keys.h"
#include "menu/menu_mark_as_read.h"
#include "main/main_account.h"
#include "main/main_session.h"
#include "main/main_domain.h"
#include "main/session_duplicate.h"
#include "base/timer.h"
#include "settings/bulk_accounts_actions.h"
#include "storage/storage_tdata_export.h"
#include "mtproto/mtproto_dc_options.h"
#include "mtproto/facade.h"
#include "window/window_session_controller.h"
#include "window/window_controller.h"
#include "window/window_peer_menu.h"
#include "api/api_cloud_password.h"
#include "apiwrap.h"
#include "api/api_peer_photo.h"
#include "api/api_updates.h"
#include "api/api_user_names.h"
#include "api/api_user_privacy.h"
#include "base/call_delayed.h"
#include "core/ggtg_sessions.h"
#include "base/flat_set.h"
#include "base/weak_ptr.h"
#include "base/options.h"
#include "base/unixtime.h"
#include "base/random.h"
#include "styles/style_chat.h" // popupMenuExpandedSeparator
#include "styles/style_dialogs.h" // dialogsPinnedIcon
#include "styles/style_dialogs_layout.h" // dialogsMuteIcon
#include "styles/style_layers.h"
#include "styles/style_settings.h"
#include "styles/style_menu_icons.h"
#include "styles/style_window.h"
#include "styles/style_window_main_menu.h" // mainMenuWidth

#include <QtGui/QGuiApplication>
#include <QtCore/QBuffer>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QRegularExpression>
#include <QtCore/QSaveFile>

namespace Settings {
namespace {

using namespace Builder;

struct InformationHighlightTargets {
	QPointer<Ui::RpWidget> photo;
	QPointer<Ui::RpWidget> uploadPhoto;
	QPointer<Ui::RpWidget> bio;
	QPointer<Ui::RpWidget> colorButton;
	QPointer<Ui::RpWidget> channelButton;
	QPointer<Ui::RpWidget> addAccount;
	QPointer<Ui::RpWidget> name;
	QPointer<Ui::RpWidget> phone;
	QPointer<Ui::RpWidget> username;
	QPointer<Ui::RpWidget> birthday;
	QPointer<Ui::RpWidget> chatAutomation;
};

constexpr auto kSaveBioTimeout = 1000;
constexpr auto kPlayStatusLimit = 2;

// Persisted across AccountsList instances (recreated every time the side
// menu/settings page opens) and across app restarts, so collapsed/expanded
// folders and their order don't reset every time the menu is reopened.
struct FoldersUiState {
	std::vector<QString> order;
	base::flat_set<QString> collapsed;
	base::flat_set<QString> pinned;
};

[[nodiscard]] QString FoldersUiStatePath() {
	return cWorkingDir() + u"tdata/folders_ui.json"_q;
}

[[nodiscard]] FoldersUiState &GlobalFoldersUiState() {
	static auto state = [] {
		auto result = FoldersUiState();
		auto file = QFile(FoldersUiStatePath());
		if (file.open(QIODevice::ReadOnly)) {
			const auto json = QJsonDocument::fromJson(
				file.readAll()).object();
			for (const auto &value : json["order"].toArray()) {
				result.order.push_back(value.toString());
			}
			for (const auto &value : json["collapsed"].toArray()) {
				result.collapsed.insert(value.toString());
			}
			for (const auto &value : json["pinned"].toArray()) {
				result.pinned.insert(value.toString());
			}
		}
		return result;
	}();
	return state;
}

void SaveFoldersUiState() {
	const auto &state = GlobalFoldersUiState();
	auto order = QJsonArray();
	for (const auto &name : state.order) {
		order.push_back(name);
	}
	auto collapsed = QJsonArray();
	for (const auto &name : state.collapsed) {
		collapsed.push_back(name);
	}
	auto pinned = QJsonArray();
	for (const auto &name : state.pinned) {
		pinned.push_back(name);
	}
	const auto path = FoldersUiStatePath();
	QDir().mkpath(QFileInfo(path).absolutePath());
	auto file = QFile(path);
	if (file.open(QIODevice::WriteOnly)) {
		file.write(QJsonDocument(QJsonObject{
			{ "order", order },
			{ "collapsed", collapsed },
			{ "pinned", pinned },
		}).toJson(QJsonDocument::Compact));
	}
}

class ComposedBadge final : public Ui::RpWidget {
public:
	ComposedBadge(
		not_null<Ui::RpWidget*> parent,
		not_null<Ui::SettingsButton*> button,
		not_null<::Main::Session*> session,
		rpl::producer<QString> &&text,
		bool hasUnread,
		Fn<bool()> animationPaused);

private:
	rpl::variable<QString> _text;
	rpl::event_stream<int> _unreadWidth;
	rpl::event_stream<int> _premiumWidth;

	QPointer<Ui::RpWidget> _unread;
	Info::Profile::Badge _badge;

};

ComposedBadge::ComposedBadge(
	not_null<Ui::RpWidget*> parent,
	not_null<Ui::SettingsButton*> button,
	not_null<::Main::Session*> session,
	rpl::producer<QString> &&text,
	bool hasUnread,
	Fn<bool()> animationPaused)
: Ui::RpWidget(parent)
, _text(std::move(text))
, _badge(
		this,
		st::settingsInfoPeerBadge,
		session,
		Info::Profile::BadgeContentForPeer(session->user()),
		nullptr,
		std::move(animationPaused),
		kPlayStatusLimit,
		Info::Profile::BadgeType::Premium) {
	if (hasUnread) {
		_unread = Badge::CreateUnread(this, rpl::single(
			rpl::empty
		) | rpl::then(
			session->data().unreadBadgeChanges()
		) | rpl::map([=] {
			const auto &owner = session->data();
			return Badge::UnreadBadge{
				owner.unreadWithMentionsBadge(),
				owner.unreadWithMentionsBadgeMuted(),
			};
		}));
		rpl::combine(
			_unread->shownValue(),
			_unread->widthValue()
		) | rpl::map([=](bool shown, int width) {
			return shown ? width : 0;
		}) | rpl::start_to_stream(_unreadWidth, _unread->lifetime());
	}

	_badge.updated(
	) | rpl::on_next([=] {
		if (const auto button = _badge.widget()) {
			button->widthValue(
			) | rpl::start_to_stream(_premiumWidth, button->lifetime());
		} else {
			_premiumWidth.fire(0);
		}
	}, lifetime());

	auto textWidth = _text.value() | rpl::map([=] {
		return button->fullTextWidth();
	});
	rpl::combine(
		_unreadWidth.events_starting_with(_unread ? _unread->width() : 0),
		_premiumWidth.events_starting_with(_badge.widget()
			? _badge.widget()->width()
			: 0),
		std::move(textWidth),
		button->sizeValue()
	) | rpl::on_next([=](
			int unreadWidth,
			int premiumWidth,
			int textWidth,
			const QSize &buttonSize) {
		const auto &st = button->st();
		const auto skip = st.style.font->spacew;
		const auto textRightPosition = st.padding.left()
			+ textWidth
			+ skip;
		const auto minWidth = unreadWidth + premiumWidth + skip;
		const auto maxTextWidth = buttonSize.width()
			- minWidth
			- st.padding.right();

		const auto finalTextRight = std::min(textRightPosition, maxTextWidth);

		resize(
			buttonSize.width() - st.padding.right() - finalTextRight,
			buttonSize.height());

		_badge.move(
			0,
			st.padding.top(),
			buttonSize.height() - st.padding.top());
		if (_unread) {
			_unread->moveToRight(
				0,
				(buttonSize.height() - _unread->height()) / 2);
		}
	}, lifetime());
}

class AccountsList final {
public:
	AccountsList(
		not_null<Ui::VerticalLayout*> container,
		not_null<Window::SessionController*> controller);

	[[nodiscard]] rpl::producer<> closeRequests() const;
	[[nodiscard]] Ui::RpWidget *addAccountButton() const;

private:
	void setup();

	[[nodiscard]] not_null<Ui::SlideWrap<Ui::SettingsButton>*> setupAdd();
	void rebuild();
	// rebuild() destroys the whole account/folder button tree, including
	// whichever widget's click handler triggered the change (a folder
	// header's own collapse toggle, a popup menu item owned by an account
	// row about to be torn down, ...). Calling it synchronously from such
	// a handler is a use-after-free; defer it a tick instead.
	void scheduleRebuild();
	// Returns the header row itself, so callers can use it as a drag
	// handle for reordering the whole folder block it sits in.
	not_null<Ui::SettingsButton*> addFolderHeader(
		not_null<Ui::VerticalLayout*> container,
		const QString &folder,
		const std::vector<not_null<::Main::Account*>> &accounts,
		const std::vector<QString> &folderOrder);
	void moveAccountToFolder(
		not_null<::Main::Account*> account,
		const QString &folder);
	void promptNewFolder(not_null<::Main::Account*> account);
	void promptRenameFolder(const QString &folder);
	void removeFolder(const QString &folder);
	void exportFolder(
		const QString &folder,
		const std::vector<not_null<::Main::Account*>> &accounts);
	void replaceFolderProxy(
		const QString &folder,
		const std::vector<not_null<::Main::Account*>> &accounts);
	void togglePinned(not_null<::Main::Account*> account);
	void toggleMuted(not_null<::Main::Account*> account);
	void toggleAlwaysOffline(not_null<::Main::Account*> account);
	void toggleFolderPinned(const QString &folder);

	const not_null<Window::SessionController*> _controller;
	const not_null<Ui::VerticalLayout*> _outer;
	// Established at construction time, while _outer is guaranteed alive,
	// and only ever copied afterwards - re-wrapping _outer.get() at a later,
	// deferred-call time is not safe, since by then _outer may already be
	// dangling (e.g. the panel it lives in closed while a box was open).
	const QPointer<Ui::VerticalLayout> _outerWeak;
	int _outerIndex = 0;
	// The container rebuild() (re)inserts everything into - each call must
	// destroy the previous one first, or the old folder headers/rows stay
	// behind as orphaned, still-visible duplicates (Qt destroying _wrap
	// also destroys the account buttons living in it as its children,
	// which auto-nulls their QPointer-backed handles in _watched below).
	QPointer<Ui::VerticalLayout> _wrap;

	Ui::SlideWrap<Ui::SettingsButton> *_addAccount = nullptr;
	base::flat_map<
		not_null<::Main::Account*>,
		base::unique_qptr<Ui::SettingsButton>> _watched;
	// Collapsed/expanded state lives in GlobalFoldersUiState() instead of
	// here, so it survives this object being destroyed and recreated every
	// time the side menu (or settings page) closes and reopens.

	base::unique_qptr<Ui::PopupMenu> _contextMenu;
	base::unique_qptr<Ui::PopupMenu> _folderContextMenu;
	// One reorder per draggable list: each expanded folder's own account
	// list, the ungrouped accounts list, and (if any folders exist) the
	// top-level list of folder blocks itself. All owned via make_state() on
	// their respective container, which dies with _wrap on the next
	// rebuild() - this vector is only ever rebuilt from scratch, never
	// mutated in place.
	std::vector<Ui::VerticalLayoutReorder*> _allReorders;
	int _reordering = 0;

	rpl::event_stream<> _closeRequests;

	base::binary_guard _accountSwitchGuard;
	base::binary_guard _rebuildGuard;
	bool _firstBuild = true;

};

[[nodiscard]] rpl::producer<TextWithEntities> StatusValue(
		not_null<UserData*> user) {
	return [=](auto consumer) {
		auto lifetime = rpl::lifetime();
		const auto timer = lifetime.make_state<base::Timer>();
		const auto push = [=] {
			const auto now = base::unixtime::now();
			consumer.put_next(Data::OnlineTextActive(user, now)
				? tr::link(Data::OnlineText(user, now))
				: tr::marked(Data::OnlineText(user, now)));
			timer->callOnce(Data::OnlineChangeTimeout(user, now));
		};
		timer->setCallback(push);
		user->session().changes().peerFlagsValue(
			user,
			Data::PeerUpdate::Flag::OnlineStatus
		) | rpl::on_next(push, lifetime);
		return lifetime;
	};
}

void SetupPhoto(
		not_null<Ui::VerticalLayout*> container,
		not_null<Window::SessionController*> controller,
		not_null<UserData*> self,
		InformationHighlightTargets *targets) {
	const auto wrap = container->add(object_ptr<Ui::FixedHeightWidget>(
		container,
		st::settingsInfoPhotoHeight));
	const auto photo = Ui::CreateChild<Ui::UserpicButton>(
		wrap,
		controller,
		self,
		Ui::UserpicButton::Role::OpenPhoto,
		Ui::UserpicButton::Source::PeerPhoto,
		st::settingsInfoPhoto);
	const auto upload = CreateUploadSubButton(wrap, controller);
	if (targets) {
		targets->photo = photo;
		targets->uploadPhoto = upload;
	}

	upload->setVideoAllowed(true);
	upload->chosenImages(
	) | rpl::on_next([=](Ui::UserpicButton::ChosenImage &&chosen) {
		auto &image = chosen.image;
		UpdatePhotoLocally(self, image);
		photo->showCustom(base::duplicate(image));
		const auto isMarkup = (chosen.markup.documentId != 0);
		self->session().api().peerPhoto().upload(
			self,
			{
				.image = std::move(image),
				.markupDocumentId = chosen.markup.documentId,
				.markupColors = chosen.markup.colors,
				.video = std::move(chosen.video),
			});
		if (!isMarkup) {
			photo->showUploadProgress();
		}
	}, upload->lifetime());

	const auto name = Ui::CreateChild<Ui::FlatLabel>(
		wrap,
		Info::Profile::NameValue(self),
		st::settingsCoverName);
	const auto status = Ui::CreateChild<Ui::FlatLabel>(
		wrap,
		StatusValue(self),
		st::settingsCoverStatus);
	status->setAttribute(Qt::WA_TransparentForMouseEvents);
	rpl::combine(
		wrap->widthValue(),
		photo->widthValue(),
		Info::Profile::NameValue(self),
		status->widthValue()
	) | rpl::on_next([=](
			int max,
			int photoWidth,
			const QString&,
			int statusWidth) {
		photo->moveToLeft(
			(max - photoWidth) / 2,
			st::settingsInfoPhotoTop);
		upload->moveToLeft(
			((max - photoWidth) / 2
				+ photoWidth
				- upload->width()
				+ st::settingsInfoUploadLeft),
			photo->y() + photo->height() - upload->height());
		const auto skip = st::settingsButton.iconLeft;
		name->resizeToNaturalWidth(max - 2 * skip);
		name->moveToLeft(
			(max - name->width()) / 2,
			(photo->y() + photo->height() + st::settingsInfoPhotoSkip));
		status->moveToLeft(
			(max - statusWidth) / 2,
			(name->y() + name->height() + st::settingsInfoNameSkip));
	}, photo->lifetime());
}

void ShowMenu(
		QWidget *parent,
		const QString &copyButton,
		const QString &text,
		const style::icon *copyIcon = nullptr,
		Fn<void(not_null<Ui::PopupMenu*>)> extend = nullptr) {
	const auto menu = Ui::CreateChild<Ui::PopupMenu>(
		parent,
		extend ? st::popupMenuWithIcons : st::defaultPopupMenu);

	menu->addAction(copyButton, [=] {
		QGuiApplication::clipboard()->setText(text);
	}, copyIcon);
	if (extend) {
		extend(menu);
	}
	menu->popup(QCursor::pos());
}

// Small "type a name" prompt, used for naming/renaming account folders.
void PromptFolderName(
		not_null<Window::SessionController*> controller,
		const QString &title,
		const QString &initial,
		Fn<void(QString)> done) {
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(title));
		const auto field = box->addRow(object_ptr<Ui::InputField>(
			box.get(),
			st::defaultInputField,
			rpl::single(QString()),
			initial));
		box->setFocusCallback([=] {
			field->setFocusFast();
		});
		const auto save = [=] {
			const auto text = field->getLastText().trimmed();
			if (text.isEmpty()) {
				return;
			}
			done(text);
			box->closeBox();
		};
		field->submits(
		) | rpl::on_next(save, field->lifetime());
		box->addButton(tr::lng_settings_save(), save);
		box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
	}));
}

// A folder name safe on Windows too: ':' (formerly used in "<name>: <N>")
// and the other reserved characters make mkpath() fail there, which left
// the export folder empty.
[[nodiscard]] QString SafeFolderPart(QString name) {
	for (const auto ch : u"\\/:*?\"<>|"_q) {
		name.replace(ch, '_');
	}
	name = name.trimmed();
	while (name.endsWith('.')) {
		name.chop(1);
	}
	return name;
}

// Same plain "<folder>" level the per-folder export writes, no count in the
// name (it only added spaces and brackets to the path).
[[nodiscard]] QString GroupFolderName(const QString &tag) {
	const auto safe = SafeFolderPart(tag);
	return safe.isEmpty() ? u"Без папки"_q : safe;
}

// Shows the account's stored 2FA password (if any) for viewing/copying,
// and lets it be entered or changed by hand too - not just picked up
// automatically from a sibling file at import time.
// Shows the stored password and lets another one be typed: it is checked
// against the real account and saved only if the server accepts it.
// Changing 2FA through Privacy & Security keeps the stored copy in sync too
// (see Api::CloudPassword).
void ShowTwoFactorBox(
		not_null<Window::SessionController*> controller,
		not_null<::Main::Account*> account) {
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"Пароль 2FA"_q));
		const auto saved = account->twoFactorPassword();
		if (saved.isEmpty()) {
			box->addRow(object_ptr<Ui::FlatLabel>(
				box.get(),
				u"Для этого аккаунта пароль 2FA не сохранён."_q,
				st::boxLabel));
		} else {
			const auto label = box->addRow(object_ptr<Ui::FlatLabel>(
				box.get(),
				rpl::single(TextWithEntities()),
				st::boxLabel));
			label->setMarkedText(Ui::Text::Link(saved));
			label->setClickHandlerFilter([=](const auto &...) {
				TextUtilities::SetClipboardText(
					TextForMimeData::Simple(saved));
				box->uiShow()->showToast(u"Скопировано в буфер обмена"_q);
				return false;
			});
		}
		// Not known (or stale): let a password be typed and checked against
		// the real account; only one the server accepts is saved.
		if (account->sessionExists()) {
			const auto field = box->addRow(object_ptr<Ui::InputField>(
				box.get(),
				st::defaultInputField,
				rpl::single(saved.isEmpty()
					? u"Введите пароль 2FA"_q
					: u"Другой пароль 2FA"_q)));
			const auto status = box->addRow(object_ptr<Ui::FlatLabel>(
				box.get(),
				QString(),
				st::boxLabel));
			const auto busy = box->lifetime().make_state<bool>(false);
			const auto check = [=] {
				const auto typed = field->getLastText();
				if (typed.isEmpty() || *busy || !account->sessionExists()) {
					return;
				}
				*busy = true;
				status->setText(u"Проверяем..."_q);
				const auto lifetime
					= box->lifetime().make_state<rpl::lifetime>();
				*lifetime = account->session().api().cloudPassword().check(
					typed
				) | rpl::on_error_done([=](const QString &type) {
					*busy = false;
					status->setText(
						(type == u"PASSWORD_HASH_INVALID"_q)
							? u"Пароль не подошёл."_q
							: (u"Ошибка: "_q + type));
				}, [=] {
					*busy = false;
					if (account->twoFactorPassword() == typed) {
						status->setText(
							u"Пароль подошёл и сохранён."_q);
						box->uiShow()->showToast(
							u"Пароль 2FA сохранён"_q);
						box->closeBox();
					} else {
						status->setText(
							u"У этого аккаунта нет пароля 2FA."_q);
					}
				});
			};
			field->submits() | rpl::on_next(check, field->lifetime());
			box->addButton(rpl::single(u"Проверить и сохранить"_q), check);
			box->setFocusCallback([=] {
				field->setFocusFast();
			});
		}
		Ggtg::AddCloseCross(box);
	}));
}

// Writes the account's stored 2FA password (if any) as a sidecar file next
// to its exported tdata, mirroring the sibling-file layout the import side
// already reads (see FindSiblingTwoFactor in ggtg_add_accounts.h) - so exporting
// and re-importing an account round-trips the password too, not just the
// session itself.
// Returns an empty string on success, otherwise what could not be written.
[[nodiscard]] QString WriteTwoFactorSidecar(
		not_null<::Main::Account*> account,
		const QString &accountFolder,
		std::optional<MTP::DeviceOverride> customDevice = std::nullopt) {
	if (!QDir().mkpath(accountFolder)) {
		return u"не удалось создать папку "_q
			+ QDir::toNativeSeparators(accountFolder);
	}
	const auto writeFile = [&](const QString &name, const QByteArray &data) {
		auto file = QSaveFile(accountFolder + '/' + name);
		if (!file.open(QIODevice::WriteOnly)) {
			return false;
		}
		file.write(data);
		return file.commit();
	};
	const auto twoFactor = account->twoFactorPassword();
	auto failed = QStringList();
	if (!twoFactor.isEmpty()
		&& !writeFile(u"2fa.txt"_q, twoFactor.toUtf8())) {
		failed.push_back(u"2fa.txt"_q);
	}
	// Everything else that travels with an account - its folder, proxy and
	// 2FA - in one "ggtg.json", which the import reads back so the account
	// lands in the same folder, with the same proxy and password.
	auto info = QJsonObject();
	info["folder"] = account->folderTag();
	if (!twoFactor.isEmpty()) {
		info["twoFactor"] = twoFactor;
	}
	// Device model / OS / language / time zone, so the account keeps the
	// identity Telegram already knows it by after a re-import.
	const auto dev = customDevice
		? *customDevice
		: EffectiveDeviceOverride(account);
	info["device"] = DeviceOverrideToJson(dev);
	const auto proxy = account->proxyOverride();
	using Type = MTP::ProxyData::Type;
	if (proxy.type == Type::Socks5
		|| proxy.type == Type::Http
		|| proxy.type == Type::Mtproto) {
		auto json = QJsonObject();
		json["type"] = (proxy.type == Type::Socks5)
			? u"socks5"_q
			: (proxy.type == Type::Http)
			? u"http"_q
			: u"mtproto"_q;
		json["host"] = proxy.host;
		json["port"] = int(proxy.port);
		json["user"] = proxy.user;
		json["password"] = proxy.password;
		info["proxy"] = json;
	}
	if (!writeFile(
			u"ggtg.json"_q,
			QJsonDocument(info).toJson(QJsonDocument::Indented))) {
		failed.push_back(u"ggtg.json"_q);
	}
	return failed.isEmpty()
		? QString()
		: (u"не записаны файлы: "_q + failed.join(u", "_q) + u" ("_q
			+ QDir::toNativeSeparators(accountFolder) + ')');
}

// Same local-only teardown "Log out" ends up doing (destroySession() +
// local().reset(), which the account's own sessionChanges() reactively
// turns into closing any separate window for it and dropping its now-
// empty slot - see Domain::watchSession/removeRedundantAccounts) but
// skipping logOut()'s server round-trip first (the MTP auth.logOut call,
// which revokes that session everywhere it's used). "Delete" is for
// removing an account from *this app* without burning the session itself -
// e.g. it's about to live somewhere else instead, or was only ever parked
// here - so forcedLogOut() (no server call) is used in place of the normal
// logOut().
void DeleteAccountsLocally(
		const std::vector<not_null<::Main::Account*>> &accounts) {
	for (const auto &account : accounts) {
		if (account->sessionExists()) {
			account->forcedLogOut();
		}
	}
}

// Main::DuplicateSession()'s requestPassword callback, for the case its
// own automatic attempt (account->twoFactorPassword(), if this app has it
// stored) is missing or wrong: identifies which account needs it (phone,
// name, username - nothing else here distinguishes one account row from
// another) and lets whoever is sitting at this app try a password by hand,
// or give up on just this one account and let the batch move on.
void PromptSessionDuplicatePassword(
		not_null<Window::SessionController*> window,
		not_null<::Main::Account*> account,
		Fn<void(QString password)> submit,
		Fn<void()> cancel) {
	const auto user = account->session().user();
	auto identity = u"Телефон: "_q + user->phone();
	if (!user->firstName.isEmpty() || !user->lastName.isEmpty()) {
		identity += u"\nИмя: "_q + user->firstName
			+ (user->lastName.isEmpty() ? QString() : (u" "_q + user->lastName));
	}
	if (!user->username().isEmpty()) {
		identity += u"\n@"_q + user->username();
	}
	window->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(u"Нужен пароль 2FA"_q);
		box->addRow(object_ptr<Ui::FlatLabel>(
			box.get(),
			u"Для дублирования сессии этого аккаунта нужен пароль 2FA, "
				"сохранённого локально нет (или он не подошёл):\n\n"_q
				+ identity,
			st::boxLabel));
		// Ui::PasswordInput isn't an Ui::RpWidget (it derives from
		// RpWidgetBase<QLineEdit> directly, one level below - GenericBox::
		// addRow() specifically requires the former), so a masked field
		// would need a whole custom BoxContent subclass instead of this
		// generic box (see boxes/passcode_box.cpp's own _oldPasscode et al.
		// for that heavier pattern) - not worth it for this one-off prompt,
		// and this app already shows 2FA passwords in plain text elsewhere
		// (ShowTwoFactorBox's click-to-copy), so an unmasked field here
		// isn't a new exposure.
		const auto field = box->addRow(object_ptr<Ui::InputField>(
			box.get(),
			st::defaultInputField,
			rpl::single(u"Пароль 2FA"_q)));
		box->setFocusCallback([=] {
			field->setFocusFast();
		});
		auto cancelled = box->lifetime().make_state<bool>(false);
		const auto trySubmit = [=] {
			const auto text = field->getLastText();
			if (text.isEmpty()) {
				return;
			}
			*cancelled = true; // Box is closing on its own terms, not a skip.
			box->closeBox();
			submit(text);
		};
		field->submits(
		) | rpl::on_next(trySubmit, field->lifetime());
		box->addButton(rpl::single(u"Продолжить"_q), trySubmit);
		box->addButton(rpl::single(u"Пропустить"_q), [=] {
			*cancelled = true;
			box->closeBox();
			cancel();
		});
		// Closing the box any other way (Esc, clicking outside) is also a
		// skip - nothing here should leave the batch silently stuck waiting
		// forever for a box that's already gone.
		box->boxClosing(
		) | rpl::on_next([=] {
			if (!*cancelled) {
				*cancelled = true;
				cancel();
			}
		}, box->lifetime());
	}));
}

// ---- Exporting accounts as tdata (see settings/ggtg_export.h) ----
//
// One account at a time, a short random pause between them. Handing out
// the main session: the spare the app moves onto is checked first, then the
// export is written, then the app switches; a failed switch while the app
// still runs the original takes the written export back. Without a spare
// the app stays on the main session and the export is a copy (warned
// about). Every session handed out is kept in the session vault.

[[nodiscard]] Window::SessionController *ActiveSessionWindow() {
	const auto window = Core::App().activePrimaryWindow();
	return window ? window->sessionController() : nullptr;
}

// "79991234567" for the account's own folder in the export.
[[nodiscard]] QString PhoneFolderName(
		not_null<::Main::Account*> account,
		int *unnamed) {
	auto phone = account->session().user()->phone();
	phone.remove('+');
	return phone.isEmpty() ? u"account_%1"_q.arg(++*unnamed) : phone;
}

// The account's stored spares that can still be used, oldest first.
[[nodiscard]] std::vector<Core::GgtgSessions::StoredSession> UsableBackups(
		not_null<::Main::Account*> account) {
	namespace Sessions = Core::GgtgSessions;
	const auto userId = QString::number(account->session().userId().bare);
	auto result = std::vector<Sessions::StoredSession>();
	auto fallback = std::vector<Sessions::StoredSession>();
	for (const auto &entry
			: Sessions::StoredForUser(Sessions::LoadVault(), userId)) {
		if (Sessions::HasKey(entry)
			&& QDir(Sessions::VaultFolder(entry) + u"/tdata"_q).exists()) {
			if (entry.state != u"revoked"_q) {
				result.push_back(entry);
			} else {
				fallback.push_back(entry);
			}
		}
	}
	if (result.empty()) {
		result = std::move(fallback);
	}
	std::sort(result.begin(), result.end(), [](
			const Sessions::StoredSession &a,
			const Sessions::StoredSession &b) {
		return a.createdAt < b.createdAt;
	});
	return result;
}

[[nodiscard]] std::optional<Core::GgtgSessions::StoredSession> StoredById(
		const QString &id) {
	for (const auto &entry : Core::GgtgSessions::LoadVault()) {
		if (entry.id == id) {
			return entry;
		}
	}
	return std::nullopt;
}

// A session handed out with an export: saved with its key in the vault
// so it is signed, kept, and can be switched back onto. Returns its id.
QString NoteHandedOut(
		not_null<::Main::Account*> account,
		const QString &what,
		const QString &folder) {
	namespace Sessions = Core::GgtgSessions;
	const auto user = account->session().user();
	auto entry = Sessions::StoredSession();
	entry.userId = QString::number(account->session().userId().bare);
	entry.phone = user->phone();
	entry.name = user->name();
	entry.username = user->username();
	entry.folder = account->folderTag();
	entry.twoFactor = account->twoFactorPassword();
	const auto p = account->proxyOverride();
	if (p.type != MTP::ProxyData::Type::None && !p.host.isEmpty()) {
		entry.proxy = QJsonObject{
			{ "type", int(p.type) },
			{ "host", p.host },
			{ "port", int(p.port) },
			{ "user", p.user },
			{ "password", p.password },
		};
	}
	entry.role = u"exported"_q;
	entry.source = u"export"_q;
	entry.note = what + u" передана в экспорт: "_q
		+ QDir::toNativeSeparators(folder);
	entry = Sessions::AddStored(entry);
	auto err = QString();
	Storage::ExportRawAuthorizationAsTdata(
		account->serializeMtpAuthorization(),
		Sessions::VaultFolder(entry),
		&err);
	Sessions::WriteSessionVaultSidecar(entry);
	return entry.id;
}

bool CopyFolder(const QString &from, const QString &to) {
	if (!QDir().mkpath(to)) {
		return false;
	}
	const auto filters = QDir::Files
		| QDir::Dirs
		| QDir::NoDotAndDotDot
		| QDir::Hidden;
	for (const auto &info : QDir(from).entryInfoList(filters)) {
		const auto target = to + '/' + info.fileName();
		if (info.isDir()) {
			if (!CopyFolder(info.filePath(), target)) {
				return false;
			}
		} else {
			QFile::remove(target);
			if (!QFile::copy(info.filePath(), target)) {
				return false;
			}
		}
	}
	return true;
}

// Takes a written export back (the app kept running on that session).
void RemoveExportedFiles(const QString &folder) {
	QDir(folder + u"/tdata"_q).removeRecursively();
	QFile::remove(folder + u"/2fa.txt"_q);
	QFile::remove(folder + u"/ggtg.json"_q);
	QFile::remove(folder + u"/session.json"_q);
	for (const auto &info : QDir(folder).entryInfoList({ u"*.session"_q, u"*.json"_q })) {
		QFile::remove(info.filePath());
	}
	QDir().rmdir(folder); // only if nothing else is there
}

struct TdataExportRun {
	std::shared_ptr<Core::ProcessHandle> process;
	TdataExportOptions options;
	std::vector<base::weak_ptr<::Main::Account>> accounts;
	std::vector<QString> folders;
	std::vector<QString> labels;
	int exported = 0;
	QStringList errors;
};

void TdataExportNext(std::shared_ptr<TdataExportRun> run, size_t index);

// Everything one account's export does, start to finish.
class TdataExportStep final {
public:
	TdataExportStep(std::shared_ptr<TdataExportRun> run, size_t index)
	: _run(std::move(run))
	, _index(index)
	, _weak(_run->accounts[index])
	, _folder(_run->folders[index])
	, _label(u"[%1/%2] %3"_q
		.arg(index + 1)
		.arg(_run->accounts.size())
		.arg(_run->labels[index])) {
	}

	void start() const {
		const auto account = alive();
		if (!account) {
			fail(u"нет активной сессии"_q);
			return;
		}
		switch (_run->options.source) {
		case TdataSource::New: exportNew(); break;
		case TdataSource::Backup: exportBackup(account); break;
		case TdataSource::Working: handOutWorking(account); break;
		}
	}

private:
	[[nodiscard]] ::Main::Account *alive() const {
		const auto account = _weak.get();
		return (account && ::Main::AccountIsLive(account)) ? account : nullptr;
	}

	void log(const QString &text) const {
		_run->process->log(_label + u": "_q + text);
	}

	void fail(const QString &error) const {
		log(u"ошибка - "_q + error);
		_run->errors.push_back(_run->labels[_index] + u": "_q + error);
		next();
	}

	void succeed(const QString &text, const QString &warning = {}) const {
		log(text);
		++_run->exported;
		if (!warning.isEmpty()) {
			log(warning);
			_run->errors.push_back(_run->labels[_index] + u": "_q + warning);
		}
		next();
	}

	void next() const {
		const auto run = _run;
		const auto index = _index + 1;
		if (index >= run->accounts.size()) {
			TdataExportNext(run, index);
			return;
		}
		// A batch shouldn't make many logins from one address back to back.
		const auto pause = 3000 + base::RandomIndex(5000);
		base::call_delayed(pause, &Core::App(), [=] {
			TdataExportNext(run, index);
		});
	}

	[[nodiscard]] Fn<void(Fn<void(QString)>, Fn<void()>)> askPassword() const {
		const auto weak = _weak;
		const auto process = _run->process;
		const auto label = _label;
		// Whatever window is active now: the one the export started from
		// may be gone (its account was switched or removed meanwhile).
		return [=](Fn<void(QString password)> submit, Fn<void()> cancel) {
			process->log(label + u": нужен пароль 2FA, ждём ввода..."_q);
			const auto account = weak.get();
			const auto window = ActiveSessionWindow();
			if (account && window) {
				PromptSessionDuplicatePassword(window, account, submit, cancel);
			} else {
				cancel();
			}
		};
	}

	// <folder>/tdata from `serialized`, plus the sidecars. Empty on success,
	// `*warning` for sidecars that could not be written.
	[[nodiscard]] QString write(
			not_null<::Main::Account*> account,
			const QByteArray &serialized,
			QString *warning,
			std::optional<MTP::DeviceOverride> customDevice = std::nullopt) const {
		if (_run->options.format == Settings::ExportFormat::SessionJson) {
			auto error = QString();
			if (!Settings::ExportAccountAsSessionJson(account, _folder, serialized, customDevice, &error)) {
				return u"не удалось записать сессию: "_q + error;
			}
			return QString();
		}
		auto error = QString();
		if (!Storage::ExportRawAuthorizationAsTdata(
				serialized,
				_folder,
				&error)) {
			return u"не удалось записать экспорт: "_q + error;
		}
		if (_run->options.sidecars) {
			*warning = WriteTwoFactorSidecar(account, _folder, customDevice);
		}
		return QString();
	}

	void exportNew() const {
		const auto self = *this;
		log(u"создаём новую сессию для экспорта..."_q);
		const auto account = _weak.get();
		if (!account) {
			fail(u"нет активной сессии"_q);
			return;
		}
		const auto phone = (account->sessionExists() && account->session().user())
			? account->session().user()->phone()
			: QString();
		auto device = Settings::RandomDeviceOverrideForAccount(account);
		if (!phone.isEmpty()) {
			const auto geo = Core::GgtgGeo::MatchForPhone(phone);
			device.systemLangCode = geo.systemLang;
			device.cloudLangCode = geo.cloudLang;
			device.tzOffset = geo.tzOffset;
		}
		::Main::DuplicateSessionTracked(account, askPassword(), [=](
				QString error,
				QByteArray serialized,
				uint64 hash) {
			if (!error.isEmpty()) {
				self.fail(error);
				return;
			}
			const auto account = self._weak.get();
			if (!account) {
				self.fail(u"аккаунт пропал из программы"_q);
				return;
			}
			auto warning = QString();
			if (const auto failed = self.write(account, serialized, &warning, device);
					!failed.isEmpty()) {
				self.fail(failed);
				return;
			}
			// Kept here as a "duplicate": labelled, never ended by "end
			// other sessions", and the account can be switched onto it.
			auto storeError = QString();
			if (StoreSessionKey(
					account,
					u"duplicate"_q,
					u"выдана в экспорт: "_q
						+ QDir::toNativeSeparators(self._folder),
					serialized,
					hash,
					&storeError,
					device).isEmpty()) {
				warning += (warning.isEmpty() ? QString() : u"; "_q)
					+ u"копия не сохранена в программе: "_q + storeError;
			}
			self.succeed(u"новая дублированная сессия записана, программа "
				"на своей"_q, warning);
		}, device);
	}

	void exportBackup(not_null<::Main::Account*> account) const {
		namespace Sessions = Core::GgtgSessions;
		const auto backups = UsableBackups(account);
		if (backups.empty()) {
			if (_run->options.newWhenNoBackup) {
				log(u"резервной нет"_q);
				exportNew();
			} else {
				fail(u"нет резервной сессии - пропущен"_q);
			}
			return;
		}
		const auto &backup = (_run->options.pick == TdataBackupPick::Oldest)
			? backups.front()
			: backups.back();
		auto backupDevice = std::optional<MTP::DeviceOverride>();
		if (!backup.device.isEmpty()) {
			backupDevice = Settings::DeviceOverrideFromJson(backup.device);
		} else {
			auto file = QFile(Sessions::VaultFolder(backup) + u"/ggtg.json"_q);
			if (file.open(QIODevice::ReadOnly)) {
				const auto doc = QJsonDocument::fromJson(file.readAll());
				if (doc.object().contains("device")) {
					backupDevice = Settings::DeviceOverrideFromJson(doc.object().value("device").toObject());
				}
			}
		}
		if (!backupDevice || backupDevice->deviceModel.isEmpty()) {
			backupDevice = Settings::RandomDeviceOverrideForAccount(account);
		}
		const auto phone = (account->sessionExists() && account->session().user())
			? account->session().user()->phone()
			: QString();
		if (backupDevice && !phone.isEmpty()) {
			const auto geo = Core::GgtgGeo::MatchForPhone(phone);
			if (backupDevice->systemLangCode.isEmpty()) {
				backupDevice->systemLangCode = geo.systemLang;
			}
			if (backupDevice->cloudLangCode.isEmpty()) {
				backupDevice->cloudLangCode = geo.cloudLang;
			}
			if (!backupDevice->tzOffset) {
				backupDevice->tzOffset = geo.tzOffset;
			}
		}

		auto warning = QString();
		if (_run->options.format == Settings::ExportFormat::SessionJson) {
			const auto auths = Storage::ReadForeignTdataAuthorizations(Sessions::VaultFolder(backup) + u"/tdata"_q);
			if (auths.empty()) {
				RemoveExportedFiles(_folder);
				fail(u"не удалось прочитать ключ резервной сессии"_q);
				return;
			}
			if (const auto failed = write(account, auths.front().data, &warning, backupDevice); !failed.isEmpty()) {
				RemoveExportedFiles(_folder);
				fail(failed);
				return;
			}
		} else {
			if (!CopyFolder(
					Sessions::VaultFolder(backup) + u"/tdata"_q,
					_folder + u"/tdata"_q)) {
				RemoveExportedFiles(_folder);
				fail(u"не удалось скопировать резервную сессию в "_q
					+ QDir::toNativeSeparators(_folder));
				return;
			}
			if (_run->options.sidecars) {
				warning = WriteTwoFactorSidecar(account, _folder, backupDevice);
			}
		}
		// Not a spare any more (it is in use elsewhere now): a duplicate,
		// still kept and still recognized.
		Sessions::UpdateStoredRole(
			backup.id,
			u"duplicate"_q,
			u"выдана в экспорт: "_q + QDir::toNativeSeparators(_folder));
		succeed(u"резервная записана и стала дублированной, программа на "
			"своей"_q, warning);
	}

	void handOutWorking(not_null<::Main::Account*> account) const {
		const auto backups = UsableBackups(account);
		if (backups.empty()) {
			handOut(QString());
		} else {
			log(u"программа перейдёт на резервную сессию"_q);
			handOut(backups.front().id);
		}
	}

	// Writes the working session out, then moves the app onto the spare
	// `targetId` (or, with none, leaves the app on that same session).
	void handOut(const QString &targetId) const {
		namespace Sessions = Core::GgtgSessions;
		const auto account = alive();
		if (!account) {
			fail(u"аккаунт пропал из программы"_q);
			return;
		}
		const auto target = targetId.isEmpty()
			? std::nullopt
			: StoredById(targetId);
		if (!targetId.isEmpty() && !target) {
			fail(u"резервная сессия не найдена"_q);
			return;
		}
		auto warning = QString();
		if (const auto failed = write(
				account,
				account->serializeMtpAuthorization(),
				&warning);
				!failed.isEmpty()) {
			RemoveExportedFiles(_folder);
			fail(failed);
			return;
		}
		if (!target) {
			// No spare to move onto: a copy, and a loud warning.
			const auto copy = u"резервной нет - программа осталась на "
				"основной, в экспорте её копия. Не запускайте копию, пока "
				"аккаунт работает здесь: Telegram разорвёт сессию в обоих "
				"местах"_q;
			succeed(u"основная сессия записана"_q,
				warning.isEmpty() ? copy : (warning + u"; "_q + copy));
			return;
		}
		const auto noteId = NoteHandedOut(account, u"Основная сессия"_q, _folder);
		log(u"основная сессия записана, переключаем программу..."_q);
		const auto self = *this;
		// Tracked: the handed-out session's number lands in the note, so
		// it is labelled and never ended by "end other sessions".
		SwitchAccountToFolderTracked(
			account,
			Sessions::VaultFolder(*target),
			noteId,
			[=](QString error) {
				if (error.isEmpty()) {
					// Its key lives in the account now; keep it marked active in vault.
					Sessions::UpdateStoredRole(targetId, target->role, u"активная (текущая)"_q);
					self.succeed(u"готово, программа на другой сессии"_q,
						warning);
				} else if (self.alive()) {
					// Still on the original: take the export back.
					RemoveExportedFiles(self._folder);
					Sessions::RemoveStored(noteId);
					self.fail(u"не удалось переключить программу, экспорт "
						"этого аккаунта отменён: "_q + error);
				} else {
					self.fail(u"переключение прервалось, рабочая сессия "
						"осталась только в экспорте. Аккаунт можно вернуть "
						"из резервной в «Менеджере аккаунтов»: "_q + error);
				}
			});
	}

	const std::shared_ptr<TdataExportRun> _run;
	const size_t _index = 0;
	const base::weak_ptr<::Main::Account> _weak;
	const QString _folder;
	const QString _label;

};

void TdataExportNext(std::shared_ptr<TdataExportRun> run, size_t index) {
	if (index >= run->accounts.size()) {
		const auto line = run->errors.isEmpty()
			? u"Готово. Экспортировано аккаунтов: %1."_q.arg(run->exported)
			: u"Готово. Экспортировано %1, ошибки и предупреждения:\n"_q
				.arg(run->exported) + run->errors.join('\n');
		if (run->exported > 0 || run->errors.isEmpty()) {
			run->process->succeed(line);
		} else {
			run->process->fail(line);
		}
		return;
	}
	run->process->progress(int(index) + 1, int(run->accounts.size()));
	TdataExportStep(run, index).start();
}

// What the run will do, for the box before it starts.
[[nodiscard]] QString TdataExportSummary(
		const std::vector<not_null<::Main::Account*>> &accounts,
		const TdataExportOptions &options) {
	auto withBackup = 0;
	for (const auto &account : accounts) {
		if (!UsableBackups(account).empty()) {
			++withBackup;
		}
	}
	const auto total = int(accounts.size());
	const auto without = total - withBackup;
	auto lines = QStringList{ u"Аккаунтов: %1."_q.arg(total) };
	auto logins = 0;
	switch (options.source) {
	case TdataSource::Working:
		lines.push_back(u"В экспорт уйдут их основные сессии. Программа "
			"перейдёт на резервную: %1."_q.arg(withBackup));
		if (without > 0) {
			lines.push_back(u"\nВнимание: без резервной: %1. Программа останется на "
				"основной, а в экспорт уйдёт её копия - одна сессия в двух "
				"местах. Не запускайте такую копию, пока аккаунт работает "
				"здесь, иначе Telegram разорвёт сессию. Лучше сначала создать "
				"резервные («Массовое действие» → «Создать резервную "
				"сессию»)."_q.arg(without));
		}
		break;
	case TdataSource::New:
		lines.push_back(u"Для каждого будет создана новая сессия, она уйдёт "
			"в экспорт и сохранится в программе как дублированная. "
			"Программа остаётся на своих сессиях."_q);
		logins = total;
		break;
	case TdataSource::Backup:
		lines.push_back(u"Резервная сессия есть у %1. Отданная резервная "
			"станет дублированной (остаётся в программе)."_q.arg(withBackup));
		if (without > 0) {
			lines.push_back(options.newWhenNoBackup
				? u"Без резервной (%1): будет создана и отдана новая "
					"дублированная."_q.arg(without)
				: u"Без резервной будут пропущены: %1."_q.arg(without));
			logins = options.newWhenNoBackup ? without : 0;
		}
		lines.push_back(u"Программа остаётся на своих сессиях."_q);
		break;
	}
	if (logins > 0) {
		lines.push_back(u"\nНовых входов в аккаунты: %1. Они видны в "
			"«Активных сеансах», а у только что созданной сессии сутки "
			"действуют ограничения. Если у аккаунта включён пароль 2FA, а "
			"сохранённого нет, программа спросит его."_q.arg(logins));
	}
	if (options.source == TdataSource::Working && withBackup > 0) {
		lines.push_back(u"\nНе запускайте экспорт, пока он не закончится: "
			"сессия должна сначала уйти из программы."_q);
	}
	return lines.join('\n');
}

// The "Export" choice for an account or a folder from the sidebar: what
// goes out and what the app keeps. `label` names what's being exported ("аккаунт \"Имя\"", "5 аккаунт(ов) в
// папке \"Work\"").
void ShowExportOptionsBox(
		not_null<Window::SessionController*> window,
		const QString &label,
		std::vector<not_null<::Main::Account*>> accounts,
		const QString &title,
		TdataExportLayout layout) {
	window->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"Экспорт"_q));
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			u"Экспортировать "_q + label + u"."_q,
			st::boxLabel));
		const auto choices = AddTdataExportChoices(box, false);
		box->addButton(rpl::single(u"Далее"_q), [=] {
			const auto options = choices();
			box->closeBox();
			if (options) {
				StartTdataExport(window, accounts, *options, title, layout);
			}
		});
		box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
	}));
}

// Confirms, offers to export every account first (checked by default - the
// whole point of deleting instead of "Log out" is usually moving the
// account elsewhere, not throwing it away), then deletes. `label` names
// what's being deleted for the confirmation text (e.g. "аккаунт \"+1 234
// ...\"" or "5 аккаунтов в папке \"Work\"").
//
// `exportGroupName`, when non-empty (a folder delete - see
// AccountsList::addFolderHeader's caller below), is used the same way
// exportFolder()/exportAccounts() already group a folder's own export: one
// "<name>: <count>" subfolder holding every account's own <phone>
// subfolder, instead of dumping them directly into whatever destination
// was picked (which is what happens for a single account's own delete -
// nothing to group there).
void ConfirmDeleteAccounts(
		not_null<Window::SessionController*> controller,
		const QString &label,
		std::vector<not_null<::Main::Account*>> accounts,
		const QString &exportGroupName = QString()) {
	if (accounts.empty()) {
		return;
	}
	if (!Core::GuardAllows(
			controller->uiShow(),
			Core::GuardedAction::RemoveAccount)) {
		return;
	}
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"Удалить"_q));
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			u"Удалить "_q + label
				+ u"? Это уберёт "_q
				+ (accounts.size() == 1 ? u"его"_q : u"их"_q)
				+ u" из приложения, но не выйдет из аккаунта на сервере "
					"Telegram - в отличие от обычного выхода."_q,
			st::boxLabel));
		const auto exportFirst = box->addRow(object_ptr<Ui::Checkbox>(
			box,
			u"Экспортировать перед удалением"_q,
			true));
		box->addButton(rpl::single(u"Удалить"_q), [=] {
			// Read everything this needs from the box (which is about to
			// go away) and close it *before* doing any of the actual
			// work below - not after. DeleteAccountsLocally() can end up
			// closing this window out from under us if one of the
			// accounts it logs out happens to be the one this settings
			// panel belongs to (folder deletes are exactly when that's
			// likely - the whole folder, active account included, is
			// going at once): that tears this box down as a side effect,
			// so a closeBox() still queued for *after* the work is done
			// fires on an already-gone box and crashes (its delegate,
			// cleared on teardown, is asserted non-null - the exact
			// crash reported). Same "close(), then act" shape
			// Ui::ConfirmBoxArgs.confirmed callers already use elsewhere
			// in this file.
			const auto shouldExport = exportFirst->checked();
			box->closeBox();
			if (shouldExport) {
				const auto destination = QFileDialog::getExistingDirectory(
					nullptr,
					u"Выберите папку для экспорта перед удалением"_q);
				if (destination.isEmpty()) {
					// Cancelled picking a destination - don't go through
					// with a delete the checkbox promised would be backed
					// up first.
					return;
				}
				const auto groupFolder = exportGroupName.isEmpty()
					? destination
					: (destination
						+ '/'
						+ GroupFolderName(exportGroupName));
				auto unnamed = 0;
				for (const auto &account : accounts) {
					if (!account->sessionExists()) {
						continue;
					}
					auto phone = account->session().user()->phone();
					phone.remove('+');
					if (phone.isEmpty()) {
						phone = u"account_%1"_q.arg(++unnamed);
					}
					const auto accountFolder = groupFolder + '/' + phone;
					auto error = QString();
					if (!Storage::ExportAccountAsTdata(
							account,
							accountFolder,
							&error)) {
						// The backup the checkbox promised failed: keep
						// the accounts rather than delete them unsaved.
						controller->show(Ui::MakeInformBox(
							u"Не удалось экспортировать "_q + phone
								+ u": "_q + error
								+ u"\nУдаление отменено."_q));
						return;
					}
					const auto sidecarError = WriteTwoFactorSidecar(
						account,
						accountFolder);
					if (!sidecarError.isEmpty()) {
						controller->show(Ui::MakeInformBox(
							phone + u": "_q + sidecarError
								+ u"\nУдаление отменено."_q));
						return;
					}
				}
			}
			DeleteAccountsLocally(accounts);
		}, st::attentionBoxButton);
		box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
	}));
}

not_null<Ui::SettingsButton*> AddRow(
		not_null<Ui::VerticalLayout*> container,
		rpl::producer<QString> label,
		rpl::producer<TextWithEntities> value,
		const QString &copyButton,
		Fn<void()> edit,
		IconDescriptor &&descriptor,
		bool markedValue = false,
		Fn<void(not_null<Ui::PopupMenu*>)> menuExtender = nullptr,
		const style::icon *copyIcon = nullptr) {
	const auto wrap = markedValue
		? AddButtonWithIcon(
			container,
			rpl::duplicate(label),
			st::settingsButton,
			std::move(descriptor))
		: AddButtonWithLabel(
			container,
			rpl::duplicate(label),
			rpl::duplicate(value) | rpl::map([](const auto &t) {
				return t.text;
			}),
			st::settingsButton,
			std::move(descriptor));
	if (markedValue) {
		CreateRightLabel(
			wrap,
			rpl::duplicate(value),
			st::settingsButton,
			rpl::duplicate(label));
	}
	const auto forcopy = Ui::CreateChild<QString>(wrap.get());
	wrap->setAcceptBoth();
	wrap->clicks(
	) | rpl::filter([=] {
		return !wrap->isDisabled();
	}) | rpl::on_next([=](Qt::MouseButton button) {
		if (button == Qt::LeftButton) {
			edit();
		} else if (!forcopy->isEmpty()) {
			ShowMenu(wrap, copyButton, *forcopy, copyIcon, menuExtender);
		}
	}, wrap->lifetime());

	std::move(
		value
	) | rpl::filter([=](const TextWithEntities &text) {
		return markedValue || text.entities.isEmpty();
	}) | rpl::on_next([=](const TextWithEntities &text) {
		*forcopy = text.text;
	}, wrap->lifetime());
	return wrap;
}

void SetupBirthday(
		not_null<Ui::VerticalLayout*> container,
		not_null<Window::SessionController*> controller,
		not_null<UserData*> self,
		InformationHighlightTargets *targets) {
	const auto session = &self->session();

	Ui::AddSkip(container);

	auto value = rpl::combine(
		Info::Profile::BirthdayValue(self),
		tr::lng_settings_birthday_add()
	) | rpl::map([](Data::Birthday birthday, const QString &add) {
		const auto text = Data::BirthdayText(birthday);
		return TextWithEntities{ !text.isEmpty() ? text : add };
	});
	const auto edit = [=] {
		Core::App().openInternalUrl(
			u"internal:edit_birthday"_q,
			QVariant::fromValue(ClickHandlerContext{
				.sessionWindow = base::make_weak(controller),
			}));
	};
	const auto birthdayButton = AddRow(
		container,
		tr::lng_settings_birthday_label(),
		std::move(value),
		tr::lng_mediaview_copy(tr::now),
		edit,
		{ &st::menuIconGiftPremium });
	if (targets) {
		targets->birthday = birthdayButton;
	}

	const auto key = Api::UserPrivacy::Key::Birthday;
	session->api().userPrivacy().reload(key);
	auto isExactlyContacts = session->api().userPrivacy().value(
		key
	) | rpl::map([=](const Api::UserPrivacy::Rule &value) {
		return (value.option == Api::UserPrivacy::Option::Contacts)
			&& value.always.peers.empty()
			&& !value.always.premiums
			&& value.never.peers.empty();
	}) | rpl::distinct_until_changed();

	Ui::AddSkip(container);
	Ui::AddDividerText(container, rpl::conditional(
		std::move(isExactlyContacts),
		tr::lng_settings_birthday_contacts(
			lt_link,
			tr::lng_settings_birthday_contacts_link(
				tr::url(u"internal:edit_privacy_birthday"_q)),
			tr::marked),
		tr::lng_settings_birthday_about(
			lt_link,
			tr::lng_settings_birthday_about_link(
				tr::url(u"internal:edit_privacy_birthday"_q)),
			tr::marked)));
}

void SetupChatAutomation(
		not_null<Ui::VerticalLayout*> container,
		not_null<Window::SessionController*> controller,
		not_null<UserData*> self,
		InformationHighlightTargets *targets) {
	const auto session = &self->session();
	session->data().chatbots().preload();

	auto label = session->data().chatbots().value(
	) | rpl::map([](const Data::ChatbotsSettings &value) {
		if (!value.bot) {
			return tr::lng_settings_chat_automation_off(tr::now);
		}
		const auto username = value.bot->username();
		return username.isEmpty()
			? value.bot->name()
			: ('@' + username);
	});

	const auto &st = st::settingsButton;
	auto title = tr::lng_settings_chat_automation_label();
	const auto button = AddButtonWithLabel(
		container,
		rpl::duplicate(title),
		std::move(label),
		st,
		{ &st::settingsIconChatAutomation });

	button->setClickedCallback([=] {
		controller->showSettings(Settings::ChatbotsId());
	});

	Ui::NewBadge::AddAfterButtonText(button, std::move(title), st);

	if (targets) {
		targets->chatAutomation = button;
	}
}

void SetupPersonalChannel(
		not_null<Ui::VerticalLayout*> container,
		not_null<Window::SessionController*> controller,
		not_null<UserData*> self,
		InformationHighlightTargets *targets) {
	Ui::AddSkip(container);

	auto value = rpl::combine(
		Info::Profile::PersonalChannelValue(self),
		tr::lng_settings_channel_add()
	) | rpl::map([](ChannelData *channel, const QString &add) {
		return TextWithEntities{ channel ? channel->name() : add };
	});
	const auto edit = [=] {
		Core::App().openInternalUrl(
			u"internal:edit_personal_channel"_q,
			QVariant::fromValue(ClickHandlerContext{
				.sessionWindow = base::make_weak(controller),
			}));
	};
	const auto channelButton = AddRow(
		container,
		tr::lng_settings_channel_label(),
		std::move(value),
		tr::lng_mediaview_copy(tr::now),
		edit,
		{ &st::menuIconChannel });

	SetupChatAutomation(container, controller, self, targets);

	const auto colorButton = AddPeerColorButton(
		container,
		controller->uiShow(),
		self,
		st::settingsColorButton);
	if (targets) {
		targets->channelButton = channelButton;
		targets->colorButton = colorButton;
	}

	Ui::AddSkip(container);
	Ui::AddDivider(container);
}

void SetupRows(
		not_null<Ui::VerticalLayout*> container,
		not_null<Window::SessionController*> controller,
		not_null<UserData*> self,
		InformationHighlightTargets *targets) {
	const auto session = &self->session();

	Ui::AddSkip(container);

	const auto showEditName = [=] {
		if (controller->showFrozenError()) {
			return;
		}
		controller->show(Box<EditNameBox>(self));
	};
	const auto nameButton = AddRow(
		container,
		tr::lng_settings_name_label(),
		Info::Profile::NameValue(self) | rpl::map(tr::marked),
		tr::lng_profile_copy_fullname(tr::now),
		showEditName,
		{ &st::menuIconProfile });
	if (targets) {
		targets->name = nameButton;
	}

	const auto showChangePhone = [=] {
		controller->show(
			Ui::MakeInformBox(tr::lng_change_phone_error()));
		controller->window().activate();
	};
	const auto phoneButton = AddRow(
		container,
		tr::lng_settings_phone_label(),
		Info::Profile::PhoneWithSpoilerValue(
			self,
			Info::Profile::PhoneValue(self)),
		tr::lng_profile_copy_phone(tr::now),
		showChangePhone,
		{ &st::menuIconPhone },
		true,
		[=](not_null<Ui::PopupMenu*> menu) {
			Info::Profile::AddPhoneSpoilerMenu(menu, self);
		},
		&st::menuIconCopy);
	if (targets) {
		targets->phone = phoneButton;
	}

	auto username = Info::Profile::UsernameValue(self);
	auto empty = base::duplicate(
		username
	) | rpl::map([](const TextWithEntities &username) {
		return username.text.isEmpty();
	});
	auto label = rpl::combine(
		tr::lng_settings_username_label(),
		std::move(empty)
	) | rpl::map([](const QString &label, bool empty) {
		return empty ? "t.me/username" : label;
	});
	auto usernameValue = rpl::combine(
		std::move(username),
		tr::lng_settings_username_add()
	) | rpl::map([](const TextWithEntities &username, const QString &add) {
		if (!username.text.isEmpty()) {
			return username;
		}
		auto result = TextWithEntities{ add };
		result.entities.push_back({
			EntityType::CustomUrl,
			0,
			int(add.size()),
			"internal:edit_username" });
		return result;
	});
	session->api().usernames().requestToCache(session->user());
	const auto usernameButton = AddRow(
		container,
		std::move(label),
		std::move(usernameValue),
		tr::lng_context_copy_mention(tr::now),
		[=] {
			if (controller->showFrozenError()) {
				return;
			}
			const auto box = controller->show(
				Box(UsernamesBox, session->user()));
			box->boxClosing(
			) | rpl::on_next([=] {
				session->api().usernames().requestToCache(session->user());
			}, box->lifetime());
		},
		{ &st::menuIconUsername });
	if (targets) {
		targets->username = usernameButton;
	}

	Ui::AddSkip(container);
	Ui::AddDividerText(container, tr::lng_settings_username_about());
}

void SetupBio(
		not_null<Ui::VerticalLayout*> container,
		not_null<UserData*> self,
		InformationHighlightTargets *targets) {
	const auto limits = Data::PremiumLimits(&self->session());
	const auto defaultLimit = limits.aboutLengthDefault();
	const auto premiumLimit = limits.aboutLengthPremium();
	const auto bioStyle = [=] {
		auto result = st::settingsBio;
		result.textMargins.setRight(st::boxTextFont->spacew
			+ st::boxTextFont->width('-' + QString::number(premiumLimit)));
		return result;
	};
	const auto style = Ui::AttachAsChild(container, bioStyle());
	const auto current = Ui::AttachAsChild(container, self->about());
	const auto changed = Ui::CreateChild<rpl::event_stream<bool>>(
		container.get());
	const auto bio = container->add(
		object_ptr<Ui::InputField>(
			container,
			*style,
			Ui::InputField::Mode::MultiLine,
			tr::lng_bio_placeholder(),
			*current),
		st::settingsBioMargins);
	if (targets) {
		targets->bio = bio;
	}

	const auto countdown = Ui::CreateChild<Ui::FlatLabel>(
		container.get(),
		QString(),
		st::settingsBioCountdown);

	rpl::combine(
		bio->geometryValue(),
		countdown->widthValue()
	) | rpl::on_next([=](QRect geometry, int width) {
		countdown->move(
			geometry.x() + geometry.width() - width,
			geometry.y() + style->textMargins.top());
	}, countdown->lifetime());

	const auto assign = [=](QString text) {
		auto position = bio->textCursor().position();
		bio->setText(text.replace('\n', ' '));
		auto cursor = bio->textCursor();
		cursor.setPosition(position);
		bio->setTextCursor(cursor);
	};
	const auto updated = [=] {
		auto text = bio->getLastText();
		if (text.indexOf('\n') >= 0) {
			assign(text);
			text = bio->getLastText();
		}
		changed->fire(*current != text);
		const auto limit = self->isPremium() ? premiumLimit : defaultLimit;
		const auto countLeft = limit - Ui::ComputeFieldCharacterCount(bio);
		countdown->setText(QString::number(countLeft));
		countdown->setTextColorOverride(
			countLeft < 0 ? st::boxTextFgError->c : std::optional<QColor>());
	};
	const auto save = [=] {
		self->session().api().saveSelfBio(
			TextUtilities::PrepareForSending(bio->getLastText()));
	};

	Info::Profile::AboutValue(
		self
	) | rpl::on_next([=](const TextWithEntities &text) {
		const auto wasChanged = (*current != bio->getLastText());
		*current = text.text;
		if (wasChanged) {
			changed->fire(*current != bio->getLastText());
		} else {
			assign(text.text);
			*current = bio->getLastText();
		}
	}, bio->lifetime());

	const auto generation = Ui::CreateChild<int>(bio);
	changed->events(
	) | rpl::on_next([=](bool changed) {
		if (changed) {
			const auto saved = *generation = std::abs(*generation) + 1;
			base::call_delayed(kSaveBioTimeout, bio, [=] {
				if (*generation == saved) {
					save();
					*generation = 0;
				}
			});
		} else if (*generation > 0) {
			*generation = -*generation;
		}
	}, bio->lifetime());

	container->lifetime().add([=] {
		if (*generation > 0) {
			save();
		}
	});

	bio->setMaxLength(premiumLimit * 2);
	bio->setSubmitSettings(Ui::InputField::SubmitSettings::Both);
	auto cursor = bio->textCursor();
	cursor.setPosition(bio->getLastText().size());
	bio->setTextCursor(cursor);
	bio->submits() | rpl::on_next([=] { save(); }, bio->lifetime());
	bio->changes() | rpl::on_next(updated, bio->lifetime());
	bio->setInstantReplaces(Ui::InstantReplaces::Default());
	bio->setInstantReplacesEnabled(
		Core::App().settings().replaceEmojiValue(),
		Core::App().settings().systemTextReplaceValue());
	Ui::Emoji::SuggestionsController::Init(
		container->window(),
		bio,
		&self->session());
	updated();

	Ui::AddDividerText(container, tr::lng_settings_about_bio());
}

void SetupAccountsWrap(
		not_null<Ui::VerticalLayout*> container,
		not_null<Window::SessionController*> controller,
		InformationHighlightTargets *targets) {
	Ui::AddSkip(container);

	auto events = SetupAccounts(container, controller);
	if (targets) {
		targets->addAccount = events.addAccountButton;
	}
}

// Connected here means the account's MTP connection is actually up; frozen
// is Telegram's own account freeze (FROZEN_METHOD_INVALID on most requests),
// which the connection itself doesn't reveal.
[[nodiscard]] bool AccountFrozen(not_null<::Main::Account*> account) {
	const auto session = account->maybeSession();
	return session && bool(session->frozen());
}

[[nodiscard]] bool AccountConnected(not_null<::Main::Account*> account) {
	const auto dcState = account->mtp().dcstate();
	return (dcState != MTP::DisconnectedState)
		&& (dcState != MTP::ConnectingState)
		&& (dcState >= 0);
}

[[nodiscard]] object_ptr<Ui::SettingsButton> MakeAccountButton(
		QWidget *parent,
		not_null<Window::SessionController*> window,
		not_null<::Main::Account*> account,
		Fn<void(Qt::KeyboardModifiers)> callback,
		bool locked,
		const std::vector<QString> &folders,
		Fn<void(QString)> moveToFolder,
		Fn<void()> promptNewFolder,
		Fn<void()> togglePinned,
		Fn<void()> toggleMuted,
		Fn<void()> toggleAlwaysOffline) {
	const auto active = (account == &window->session().account());
	const auto session = &account->session();
	const auto user = session->user();

	auto text = rpl::single(
		user->name()
	) | rpl::then(session->changes().realtimeNameUpdates(
		user
	) | rpl::map([=] {
		return user->name();
	}));
	auto result = object_ptr<Ui::SettingsButton>(
		parent,
		rpl::duplicate(text),
		st::mainMenuAddAccountButton);
	const auto raw = result.data();

	struct State {
		State(QWidget *parent) : userpic(parent) {
			userpic.setAttribute(Qt::WA_TransparentForMouseEvents);
		}

		Ui::RpWidget userpic;
		Ui::PeerUserpicView view;
		base::unique_qptr<Ui::PopupMenu> menu;
		bool connected = false;
		bool frozen = false;
		bool connecting = false; // just started, not connected yet
		bool blinkOn = true;
		base::Timer connectionPoll;
		base::Timer blink;
		QPointer<Ui::RpWidget> connectionDot;
	};
	const auto state = raw->lifetime().make_state<State>(raw);

	{
		const auto container = Badge::AddRight(raw, st::mainMenuAccountLine);
		const auto composedBadge = Ui::CreateChild<ComposedBadge>(
			container.get(),
			raw,
			session,
			std::move(text),
			!active,
			[=] { return window->isGifPausedAtLeastFor(
				Window::GifPauseReason::Layer); });
		composedBadge->sizeValue(
		) | rpl::on_next([=](const QSize &s) {
			container->resize(s);
		}, container->lifetime());
	}

	// Pin/mute badges used to be painted right on the (tiny, 26px) userpic
	// (crowded), then folded into the ComposedBadge/ Badge::AddRight pairing
	// above (also wrong: that pairing sizes itself to reach almost all the
	// way across the row whenever the name fits - it's built to host content
	// flush right, not to just report a small badge's width - so adding our
	// width into its container inflated the button's reserved text padding
	// far beyond our actual footprint and crushed the account name to
	// nothing). A standalone overlay pinned to the row's own right edge,
	// untangled from that padding math entirely, avoids both problems.
	const auto badges = Ui::CreateChild<Ui::RpWidget>(raw);
	{
		const auto badgeSize = st::mainMenuAccountBadgeSize;
		const auto badgeSkip = st::mainMenuAccountBadgeSkip;
		const auto badgeColor = st::dialogsUnreadBgMuted->c;
		const auto muteImage = account->muted()
			? std::make_optional(
				st::dialogsMuteIcon.icon.instance(badgeColor))
			: std::nullopt;
		const auto pinImage = account->pinned()
			? std::make_optional(
				st::dialogsPinnedIcon.icon.instance(badgeColor))
			: std::nullopt;
		auto badgesWidth = 0;
		auto badgesCount = 0;
		if (muteImage) {
			badgesWidth += badgeSize;
			++badgesCount;
		}
		if (pinImage) {
			badgesWidth += badgeSize;
			++badgesCount;
		}
		if (badgesCount > 1) {
			badgesWidth += badgeSkip * (badgesCount - 1);
		}
		badges->resize(badgesWidth, badgesWidth ? badgeSize : 0);
		badges->paintRequest(
		) | rpl::on_next([=] {
			auto p = Painter(badges);
			auto left = 0;
			// Both drawn scaled to a common badgeSize square via drawImage
			// rather than icon.paint() at native size (dialogs_mute is 8x8,
			// dialogs_pinned 20x20 - painted at native size they'd be wildly
			// inconsistent). That alone isn't quite enough though: the mute
			// glyph fills its whole source canvas edge to edge, while the
			// pin glyph sits in a canvas with generous padding around it -
			// scaled to the same box, mute still reads as visibly bigger/
			// bolder. Shrinking mute's drawn rect (synthetic padding to
			// match) is what actually makes them look the same size.
			if (muteImage) {
				const auto inset = badgeSize / 3;
				const auto muteRect = QRect(
					left + inset / 2,
					inset / 2,
					badgeSize - inset,
					badgeSize - inset);
				p.drawImage(muteRect, *muteImage);
				left += badgeSize + badgeSkip;
			}
			if (pinImage) {
				p.drawImage(QRect(left, 0, badgeSize, badgeSize), *pinImage);
			}
		}, badges->lifetime());
	}
	raw->sizeValue(
	) | rpl::on_next([=](QSize outer) {
		badges->move(
			outer.width()
				- st::mainMenuAccountBadgeRightMargin
				- badges->width(),
			(outer.height() - badges->height()) / 2);
	}, badges->lifetime());

	// Telegram's own anti-spam limit on this account (see
	// data/account_spam_status.h), drawn right after the account's own
	// name (not over by the pin/mute badges, which are unrelated to it).
	// Only an actual problem (a temporary or permanent limit) is worth a
	// permanent fixture in this compact, always-visible list - unlike the
	// "Отправка SMS" accounts list (which is a deliberate, one-at-a-time
	// look at every account's status), showing "Свободен" here for every
	// clear account would just be noise next to every single name.
	const auto spamLabel = Ui::CreateChild<Ui::FlatLabel>(
		raw,
		QString(),
		st::mainMenuAccountSpamLabel);
	spamLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
	spamLabel->hide();
	const auto refreshSpamLabel = [=] {
		// The account manager's own verdict (a frozen account, a spam
		// limit that has not run out yet): one source for both lists.
		const auto trouble = Settings::AccountTrouble(account);
		if (!trouble) {
			spamLabel->hide();
			return;
		}
		// Same wording/colors as the "Отправка SMS" accounts list's badge -
		// see Settings::AccountSpamStatusText/Color's own comment for why
		// the color in particular isn't just a theme token.
		const auto full = trouble->text;
		// fullTextWidth() is the name's *natural* width, which can be more
		// than what actually got drawn if the button elided it to fit -
		// capping at the button's own available text width (the same
		// bound SettingsButton::paintText itself elides against) is what
		// keeps this from landing past the button's own right edge for a
		// long name.
		const auto padding = raw->st().padding;
		const auto available = std::max(
			raw->width() - padding.left() - padding.right(),
			0);
		const auto nameRight = padding.left()
			+ std::min(raw->fullTextWidth(), available);
		const auto rightLimit = raw->width()
			- st::mainMenuAccountBadgeRightMargin
			- badges->width();
		const auto left = nameRight + st::mainMenuAccountSpamLabelSkip;
		const auto room = rightLimit - left;
		if (room <= 0) {
			spamLabel->hide();
			return;
		}
		spamLabel->setText(
			st::mainMenuAccountSpamLabel.style.font->elided(full, room));
		spamLabel->setTextColorOverride(trouble->color);
		spamLabel->moveToLeft(left, (raw->height() - spamLabel->height()) / 2);
		spamLabel->show();
	};
	refreshSpamLabel();
	// A limit ends (and a freeze appears) with no event of its own.
	const auto spamTimer = raw->lifetime().make_state<base::Timer>(
		refreshSpamLabel);
	spamTimer->callEach(60 * 1000);
	Data::AccountSpamStatusChanges(
	) | rpl::filter([userId = user->id.value](uint64 id) {
		return id == userId;
	}) | rpl::on_next([=](uint64) {
		refreshSpamLabel();
	}, raw->lifetime());
	// The name itself (and the row's own width) can change independently
	// of a status update - re-run the same positioning/eliding logic
	// whenever either does, not just when the status itself changes.
	rpl::combine(
		rpl::duplicate(text),
		raw->sizeValue()
	) | rpl::on_next([=](const QString&, QSize) {
		refreshSpamLabel();
	}, raw->lifetime());

	// Small always-visible online/offline dot, polled rather than wired to
	// MTP::Instance::setStateChangedHandler - that's a single-subscriber
	// slot other code (the main connecting-widget) already occupies, and
	// clobbering it there would break that indicator instead of adding
	// to it. Polling dcstate() is read-only, so it can't step on anyone.
	// A just-started account blinks while it connects; it turns red only
	// if it still isn't connected kConnectingGrace after it started.
	constexpr auto kConnectingGrace = crl::time(90000);
	const auto refreshConnected = [=] {
		const auto connected = AccountConnected(account);
		const auto frozen = AccountFrozen(account);
		const auto connecting = !connected
			&& !frozen
			&& (crl::now() - account->startedAt() < kConnectingGrace);
		if (connected != state->connected
			|| frozen != state->frozen
			|| connecting != state->connecting) {
			state->connected = connected;
			state->frozen = frozen;
			state->connecting = connecting;
			if (connecting) {
				state->blink.callEach(500);
			} else {
				state->blink.cancel();
				state->blinkOn = true;
			}
			if (state->connectionDot) {
				state->connectionDot->update();
			}
		}
	};
	state->blink.setCallback([=] {
		state->blinkOn = !state->blinkOn;
		if (state->connectionDot) {
			state->connectionDot->update();
		}
	});
	state->connectionPoll.setCallback(refreshConnected);
	state->connectionPoll.callEach(2000 + base::RandomIndex(1001));
	refreshConnected();

	const auto userpicSkip = 2 * st::mainMenuAccountLine + st::lineWidth;
	const auto userpicSize = st::mainMenuAccountSize
		+ userpicSkip * 2;

	const auto connectionDot = Ui::CreateChild<Ui::RpWidget>(raw);
	state->connectionDot = connectionDot;
	const auto dotSize = st::mainMenuAccountStatusDot;
	connectionDot->resize(dotSize, dotSize);
	connectionDot->setAttribute(Qt::WA_TransparentForMouseEvents);
	connectionDot->paintRequest(
	) | rpl::on_next([=] {
		auto p = Painter(connectionDot);
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		p.setBrush(state->frozen
			? QColor(0, 122, 255)
			: state->connected
			? QColor(76, 217, 100)
			: state->connecting
			? QColor(255, 204, 0, state->blinkOn ? 255 : 60)
			: QColor(255, 59, 48));
		p.drawEllipse(connectionDot->rect());
	}, connectionDot->lifetime());

	raw->heightValue(
	) | rpl::on_next([=](int height) {
		const auto left = st::mainMenuAddAccountButton.iconLeft
			+ (st::settingsIconAdd.width() - userpicSize) / 2;
		const auto top = (height - userpicSize) / 2;
		state->userpic.setGeometry(left, top, userpicSize, userpicSize);
		const auto dotSkip = st::mainMenuAccountStatusSkip;
		connectionDot->move(
			std::max(left - dotSkip - dotSize, 0),
			(height - dotSize) / 2);
	}, state->userpic.lifetime());

	state->userpic.paintRequest(
	) | rpl::on_next([=] {
		auto p = Painter(&state->userpic);
		const auto size = st::mainMenuAccountSize;
		const auto line = st::mainMenuAccountLine;
		const auto skip = 2 * line + st::lineWidth;
		const auto full = size + skip * 2;
		user->paintUserpicLeft(p, state->view, skip, skip, full, size);
		if (active) {
			const auto shift = st::lineWidth + (line * 0.5);
			const auto diameter = full - 2 * shift;
			const auto rect = QRectF(shift, shift, diameter, diameter);
			auto hq = PainterHighQualityEnabler(p);
			auto pen = st::windowBgActive->p; // The same as '+' in add.
			pen.setWidthF(line);
			p.setPen(pen);
			p.setBrush(Qt::NoBrush);
			p.drawEllipse(rect);
		}
	}, state->userpic.lifetime());

	raw->setAcceptBoth(true);
	raw->clicks(
	) | rpl::on_next([=](Qt::MouseButton which) {
		if (which == Qt::LeftButton) {
			callback(raw->clickModifiers());
			return;
		} else if (which == Qt::MiddleButton) {
			callback(Qt::ControlModifier);
			return;
		} else if (which != Qt::RightButton) {
			return;
		}
		if (state->menu) {
			return;
		}
		const auto isActive = session == &window->session();
		state->menu = base::make_unique_q<Ui::PopupMenu>(
			raw,
			st::popupMenuExpandedSeparator);
		const auto addAction = Ui::Menu::CreateAddActionCallback(
			state->menu);
		if (!isActive) {
			addAction(tr::lng_context_new_window(tr::now), [=] {
				Ui::PreventDelayedActivation();
				callback(Qt::ControlModifier);
			}, &st::menuIconNewWindow);
			Window::AddSeparatorAndShiftUp(addAction);
		}

		addAction(tr::lng_profile_copy_phone(tr::now), [=] {
			Info::Profile::CopyPhoneToClipboard(
				Info::Profile::PhoneValue(session->user()));
		}, &st::menuIconCopy);

		if (!locked) {
			if (!isActive) {
				addAction(tr::lng_menu_activate(tr::now), [=] {
					callback({});
				}, &st::menuIconProfile);
			}
			MarkAsReadMenu::AddAllChatsAction(
				session,
				window->uiShow(),
				addAction);
		}

		state->menu->addSeparator();
		state->menu->addAction(
			account->pinned() ? u"Открепить"_q : u"Закрепить"_q,
			[=] { togglePinned(); });
		state->menu->addAction(
			account->muted() ? u"Включить звук"_q : u"Отключить звук"_q,
			[=] { toggleMuted(); });
		state->menu->addAction(u"Прокси"_q, [=] {
			window->show(MakeAccountProxyBox(account));
		});
		state->menu->addAction(u"Переподключить"_q, [=] {
			if (account->sessionExists()) {
				account->mtp().restart();
			}
		});
		{
			// All folder actions (move to an existing folder, move to a
			// brand new one, remove from the current one) tucked behind
			// one "Folder" submenu instead of three flat top-level items -
			// easier to scan, and a row per folder no longer eats space in
			// the account's own context menu directly.
			const auto currentFolder = account->folderTag();
			auto submenu = std::make_unique<Ui::PopupMenu>(state->menu.get());
			auto hasOtherFolders = false;
			for (const auto &folder : folders) {
				if (folder != currentFolder) {
					hasOtherFolders = true;
					submenu->addAction(folder, [=] { moveToFolder(folder); });
				}
			}
			if (hasOtherFolders) {
				submenu->addSeparator();
			}
			submenu->addAction(u"Переместить в новую папку..."_q, [=] {
				promptNewFolder();
			});
			if (!currentFolder.isEmpty()) {
				submenu->addAction(u"Убрать из папки"_q, [=] {
					moveToFolder(QString());
				});
			}
			state->menu->addAction(u"Папка"_q, std::move(submenu));
		}
		state->menu->addAction(u"Пароль 2FA"_q, [=] {
			ShowTwoFactorBox(window, account);
		});
		state->menu->addAction(u"Экспортировать этот аккаунт..."_q, [=] {
			ShowExportOptionsBox(
				window,
				u"аккаунт \""_q + user->name() + u"\""_q,
				{ account },
				u"Экспорт аккаунта"_q,
				[=](const QString &destination) {
					auto unnamed = 0;
					return std::vector<TdataExportItem>{ {
						account,
						destination + '/' + PhoneFolderName(account, &unnamed),
					} };
				});
		});

		if (!isActive) {
			auto logoutCallback = [=] {
				if (!Core::GuardAllows(
						window->uiShow(),
						Core::GuardedAction::RemoveAccount)) {
					return;
				}
				const auto callback = [=](Fn<void()> &&close) {
					close();
					Core::App().logoutWithChecks(&session->account());
				};
				window->show(
					Ui::MakeConfirmBox({
						.text = Window::LogoutConfirmationText(
							&session->account()),
						.confirmed = crl::guard(session, callback),
						.confirmText = tr::lng_settings_logout(),
						.confirmStyle = &st::attentionBoxButton,
					}),
					Ui::LayerOption::CloseOther);
			};
			addAction({
				.text = tr::lng_settings_logout(tr::now),
				.handler = std::move(logoutCallback),
				.icon = &st::menuIconLeaveAttention,
				.isAttention = true,
			});
			addAction({
				.text = u"Удалить аккаунт..."_q,
				.handler = [=] {
					ConfirmDeleteAccounts(
						window,
						u"аккаунт \""_q + user->name() + u"\""_q,
						{ account });
				},
				.icon = &st::menuIconDeleteAttention,
				.isAttention = true,
			});
		}
		state->menu->popup(QCursor::pos());
	}, raw->lifetime());

	return result;
}

AccountsList::AccountsList(
	not_null<Ui::VerticalLayout*> container,
	not_null<Window::SessionController*> controller)
: _controller(controller)
, _outer(container)
, _outerWeak(container.get())
, _outerIndex(container->count()) {
	setup();
}

rpl::producer<> AccountsList::closeRequests() const {
	return _closeRequests.events();
}

Ui::RpWidget *AccountsList::addAccountButton() const {
	return _addAccount ? _addAccount->entity() : nullptr;
}

void AccountsList::setup() {
	_addAccount = setupAdd();

	rpl::single(rpl::empty) | rpl::then(
		Core::App().domain().accountsChanges()
	) | rpl::on_next([=] {
		const auto &list = Core::App().domain().accounts();
		const auto exists = [&](not_null<::Main::Account*> account) {
			for (const auto &[index, existing] : list) {
				if (account == existing.get()) {
					return true;
				}
			}
			return false;
		};
		for (auto i = _watched.begin(); i != _watched.end();) {
			if (!exists(i->first)) {
				i = _watched.erase(i);
			} else {
				++i;
			}
		}
		for (const auto &[index, account] : list) {
			if (_watched.emplace(account.get()).second) {
				account->sessionChanges(
				) | rpl::on_next([=] {
					// Coalesced: accounts start in batches at launch, and a
					// full rebuild per account would stall the app again.
					scheduleRebuild();
				}, _outer->lifetime());
			}
		}
		if (std::exchange(_firstBuild, false)) {
			rebuild();
		} else {
			scheduleRebuild();
		}
	}, _outer->lifetime());

	Core::App().domain().pendingAccountsValue(
	) | rpl::skip(1) | rpl::on_next([=] {
		scheduleRebuild();
	}, _outer->lifetime());

	Core::App().domain().maxAccountsChanges(
	) | rpl::on_next([=] {
		for (auto i = _watched.begin(); i != _watched.end(); i++) {
			i->second = nullptr;
		}
		rebuild();
	}, _outer->lifetime());
}


not_null<Ui::SlideWrap<Ui::SettingsButton>*> AccountsList::setupAdd() {
	const auto result = _outer->add(
		object_ptr<Ui::SlideWrap<Ui::SettingsButton>>(
			_outer.get(),
			CreateButtonWithIcon(
				_outer.get(),
				tr::lng_menu_add_account(),
				st::mainMenuAddAccountButton,
				{
					&st::settingsIconAdd,
					IconType::Round,
					&st::windowBgActive
				})))->setDuration(0);
	const auto button = result->entity();

	// The ways to add accounts are shared with «Аккаунты» → «Добавить» and
	// the sign-in screen's GGTG button (settings/ggtg_add_accounts.h). The
	// proxy box stays open while the user picks, and this menu may close
	// meanwhile: the callbacks hold only the controller's own show.
	const auto show = _controller->uiShow();
	button->setAcceptBoth(true);
	button->clicks(
	) | rpl::on_next([=](Qt::MouseButton which) {
		if (which == Qt::LeftButton) {
			const auto modifiers = button->clickModifiers();
			const auto newWindow = (modifiers & Qt::ControlModifier);
			AddAccountByPhone(show, newWindow);
			return;
		} else if (which != Qt::RightButton) {
			return;
		}
		_contextMenu = base::make_unique_q<Ui::PopupMenu>(_outer);
		_contextMenu->addAction(
			u"Импорт папки tdata..."_q,
			[=] { ImportTdataFolder(show); });
		_contextMenu->addAction(
			u"Импорт Сессия + JSON..."_q,
			[=] { ImportSessionJsonFolder(show); });
		_contextMenu->addAction(
			u"Обновить 2FA из папки..."_q,
			[=] { UpdateTwoFactorFromFolder(show); });
		_contextMenu->popup(QCursor::pos());
	}, button->lifetime());

	return result;
}

void AccountsList::rebuild() {
	delete _wrap.data();
	_allReorders.clear();

	const auto wrap = _outer->insert(
		_outerIndex,
		object_ptr<Ui::VerticalLayout>(_outer.get()));
	_wrap = wrap;

	if (const auto pending = Core::App().domain().pendingAccounts()) {
		const auto total = int(Core::App().domain().accounts().size())
			+ pending;
		wrap->add(
			object_ptr<Ui::FlatLabel>(
				wrap,
				u"Подключаем: "_q
					+ QString::number(total - pending) + u" из "_q
					+ QString::number(total),
				st::boxDividerLabel),
			st::boxRowPadding);
	}

	const auto premiumLimit = _controller->session().domain().maxAccounts();
	const auto list = _controller->session().domain().orderedAccounts();

	// Group accounts by their folder tag (see Main::Account::folderTag).
	// Folders are purely a UI grouping derived from that per-account tag -
	// there's no separate "list of folders" stored anywhere, so an empty
	// folder simply stops existing once its last account leaves it.
	auto folderOrder = std::vector<QString>();
	auto grouped = base::flat_map<
		QString,
		std::vector<not_null<::Main::Account*>>>();
	auto ungrouped = std::vector<not_null<::Main::Account*>>();
	for (const auto &account : list) {
		if (!account->sessionExists() || list.size() == 1) {
			continue;
		}
		// An account in the «Корзина» is not listed here (it is reached
		// through GGTG's manager); the one this window is on always stays.
		if (account->trashed()
			&& account.get() != &_controller->session().account()) {
			continue;
		}
		const auto tag = account->folderTag();
		if (tag.isEmpty()) {
			ungrouped.push_back(account);
		} else {
			auto &bucket = grouped[tag];
			if (bucket.empty()) {
				folderOrder.push_back(tag);
			}
			bucket.push_back(account);
		}
	}
	{
		// Pinned folders sort first, same as pinned accounts. Custom,
		// user-dragged order (persisted separately from the account order
		// below, since folders aren't accounts) breaks ties within each
		// pinned/unpinned tier - any folder not yet in the saved order
		// (new folders) falls back to whatever order it was first
		// encountered in above.
		const auto &state = GlobalFoldersUiState();
		ranges::stable_sort(folderOrder, [&](
				const QString &a,
				const QString &b) {
			const auto aPinned = state.pinned.contains(a);
			const auto bPinned = state.pinned.contains(b);
			if (aPinned != bPinned) {
				return aPinned && !bPinned;
			}
			return ranges::find(state.order, a)
				< ranges::find(state.order, b);
		});
	}

	{
		// Inside a folder (and among accounts without one): pinned first,
		// then the ones with unread messages, then the rest - each tier in
		// its usual order. Counted when the list is built (every time the
		// menu opens), not live, so rows don't jump while being clicked.
		const auto hasUnread = [](not_null<::Main::Account*> account) {
			if (!account->sessionExists()) {
				return false;
			}
			const auto &owner = account->session().data();
			return (owner.unreadWithMentionsBadge() > 0)
				&& !owner.unreadWithMentionsBadgeMuted();
		};
		const auto tier = [&](not_null<::Main::Account*> account) {
			return account->pinned() ? 0 : hasUnread(account) ? 1 : 2;
		};
		const auto sortTier = [&](std::vector<not_null<::Main::Account*>> &v) {
			ranges::stable_sort(v, ranges::less(), tier);
		};
		for (auto &[tag, bucket] : grouped) {
			sortTier(bucket);
		}
		sortTier(ungrouped);
	}

	auto renderedCount = 0;
	const auto addAccountButton = [=, &renderedCount](
			not_null<Ui::VerticalLayout*> container,
			not_null<::Main::Account*> account,
			bool indented) {
		auto i = _watched.find(account);
		Assert(i != _watched.end());
		auto &button = i->second;
		const auto nextIsLocked = (renderedCount >= premiumLimit);
		++renderedCount;
		if (button) {
			return;
		}
		auto callback = [=](Qt::KeyboardModifiers modifiers) {
			if (_reordering) {
				return;
			}
			if (account == &_controller->session().account()) {
				_closeRequests.fire({});
				return;
			}
			const auto newWindow = (modifiers & Qt::ControlModifier);
			auto activate = [=, guard = _accountSwitchGuard.make_guard()]{
				if (guard) {
					for (const auto reorder : _allReorders) {
						reorder->finishReordering();
					}
					if (newWindow) {
						_closeRequests.fire({});
						Core::App().ensureSeparateWindowFor(account);
					}
					Core::App().domain().maybeActivate(account);
				}
			};
			if (const auto window = Core::App().separateWindowFor(
					account)) {
				_closeRequests.fire({});
				window->activate();
			} else {
				base::call_delayed(
					st::defaultRippleAnimation.hideDuration,
					account,
					std::move(activate));
			}
		};
		button.reset(container->add(
			MakeAccountButton(
				container,
				_controller,
				account,
				std::move(callback),
				nextIsLocked,
				folderOrder,
				[=](QString folder) { moveAccountToFolder(account, folder); },
				[=] { promptNewFolder(account); },
				[=] { togglePinned(account); },
				[=] { toggleMuted(account); },
				[=] { toggleAlwaysOffline(account); }),
			// Shift accounts inside a folder to the right, so it's visually
			// clear they belong to it (not just implied by where they sit
			// between headers).
			indented ? style::margins(28, 0, 0, 0) : style::margins()));
	};

	// Each folder's header + (if expanded) its own accounts live in their
	// own block, a direct child of wrap - so the whole block can be
	// dragged as one unit to reorder folders below, while a *separate*
	// reorder scoped to just its own account sub-list handles reordering
	// accounts within that one folder.
	auto folderBlocks = std::vector<not_null<Ui::VerticalLayout*>>();
	// Whole-block dragging can only start from a press on the folder's own
	// header row - the block itself is a plain, non-interactive container
	// and never actually receives mouse events (its header/account button
	// children do), so VerticalLayoutReorder needs to be told explicitly
	// which widget to listen on for each block index (setMouseEventProxy
	// below), or it silently never sees a single press.
	auto folderHeaderButtons = std::vector<not_null<Ui::RpWidget*>>();
	auto blockFolder = base::flat_map<not_null<Ui::VerticalLayout*>, QString>();
	auto accountContainers = std::vector<not_null<Ui::VerticalLayout*>>();
	auto perFolderReorders = std::vector<std::pair<
		not_null<Ui::VerticalLayoutReorder*>,
		not_null<Ui::VerticalLayout*>>>();
	for (const auto &folder : folderOrder) {
		const auto block = wrap->add(object_ptr<Ui::VerticalLayout>(wrap));
		folderBlocks.push_back(block);
		blockFolder.emplace(block, folder);
		folderHeaderButtons.push_back(
			addFolderHeader(block, folder, grouped[folder], folderOrder));
		if (!GlobalFoldersUiState().collapsed.contains(folder)) {
			const auto folderInner = block->add(
				object_ptr<Ui::VerticalLayout>(block));
			for (const auto &account : grouped[folder]) {
				addAccountButton(folderInner, account, true);
			}
			folderInner->resizeToWidth(_outer->width());
			accountContainers.push_back(folderInner);
			const auto folderReorder = folderInner->lifetime(
			).make_state<Ui::VerticalLayoutReorder>(folderInner);
			perFolderReorders.emplace_back(folderReorder, folderInner);
		} else {
			renderedCount += int(grouped[folder].size());
		}
	}

	const auto inner = wrap->add(object_ptr<Ui::VerticalLayout>(wrap));
	const auto innerReorder = inner->lifetime(
	).make_state<Ui::VerticalLayoutReorder>(inner);

	for (const auto &account : ungrouped) {
		addAccountButton(inner, account, false);
	}
	inner->resizeToWidth(_outer->width());
	accountContainers.push_back(inner);

	// Recomputes the *entire* flat account order (every folder's own list
	// plus the ungrouped list) from what's actually on screen right now -
	// shared by every per-list reorder below, so a drag in any one of
	// them always ends up saving a fully consistent order for all of them.
	const auto persistAccountOrder = [=] {
		std::vector<uint64> order;
		for (const auto &container : accountContainers) {
			for (auto i = 0; i < container->count(); i++) {
				for (const auto &[account, button] : _watched) {
					if (button.get() == container->widgetAt(i)) {
						order.push_back(account->session().uniqueId());
					}
				}
			}
		}
		// Keep the places of accounts not on screen (still waiting to
		// start at launch) - they'd otherwise fall to the end.
		for (const auto id : Core::App().settings().accountsOrder()) {
			if (!ranges::contains(order, id)) {
				order.push_back(id);
			}
		}
		Core::App().settings().setAccountsOrder(order);
		Core::App().saveSettings();
	};
	const auto wireAccountReorder = [=](
			not_null<Ui::VerticalLayoutReorder*> reorder,
			not_null<Ui::VerticalLayout*> container) {
		reorder->updates(
		) | rpl::on_next([=](Ui::VerticalLayoutReorder::Single data) {
			using State = Ui::VerticalLayoutReorder::State;
			if (data.state == State::Started) {
				++_reordering;
			} else {
				Ui::PostponeCall(container, [=] {
					--_reordering;
				});
				if (data.state == State::Applied) {
					persistAccountOrder();
				}
			}
		}, container->lifetime());
	};
	for (const auto &[reorder, container] : perFolderReorders) {
		wireAccountReorder(reorder, container);
		_allReorders.push_back(reorder);
	}
	wireAccountReorder(innerReorder, inner);
	_allReorders.push_back(innerReorder);

	const auto ungroupedCount = int(ungrouped.size());
	innerReorder->addPinnedInterval(
		premiumLimit,
		std::max(1, ungroupedCount - premiumLimit));

	// Whole-folder drag reordering: wrap's own children up to
	// folderBlocks.size() are the folder blocks, and "inner" (ungrouped
	// accounts) is always the one right after them - pin that one spot so
	// only folder blocks themselves can be dragged here.
	if (!folderBlocks.empty()) {
		const auto blocksReorder = wrap->lifetime(
		).make_state<Ui::VerticalLayoutReorder>(wrap);
		blocksReorder->addPinnedInterval(int(folderBlocks.size()), 1);
		blocksReorder->setMouseEventProxy([=](int i) -> not_null<Ui::RpWidget*> {
			return (i >= 0 && i < int(folderHeaderButtons.size()))
				? folderHeaderButtons[i]
				: wrap->widgetAt(i);
		});
		blocksReorder->updates(
		) | rpl::on_next([=](Ui::VerticalLayoutReorder::Single data) {
			using State = Ui::VerticalLayoutReorder::State;
			if (data.state == State::Started) {
				++_reordering;
			} else {
				Ui::PostponeCall(wrap, [=] {
					--_reordering;
				});
				if (data.state == State::Applied) {
					std::vector<QString> order;
					order.reserve(folderBlocks.size());
					for (auto i = 0; i < int(folderBlocks.size()); i++) {
						const auto widget = wrap->widgetAt(i);
						for (const auto &[block, name] : blockFolder) {
							if (block.get() == widget) {
								order.push_back(name);
							}
						}
					}
					GlobalFoldersUiState().order = order;
					SaveFoldersUiState();
				}
			}
		}, wrap->lifetime());
		_allReorders.push_back(blocksReorder);
	}

	wrap->resizeToWidth(_outer->width());

	const auto totalCount = int(list.size());
	_addAccount->toggle(
		(totalCount < ::Main::Domain::kPremiumMaxAccounts),
		anim::type::instant);

	for (const auto reorder : _allReorders) {
		reorder->start();
	}
}

void AccountsList::scheduleRebuild() {
	// Don't wrap _outer.get() here: by the time this runs, the panel that
	// _outer lives in may have already closed (a box opened on top of it),
	// making the raw pointer stale. _outerWeak was captured back when the
	// object was still guaranteed alive, so just copy that instead.
	const auto weak = _outerWeak;
	crl::on_main([=, guard = _rebuildGuard.make_guard()] {
		if (guard && weak) {
			rebuild();
		}
	});
}

not_null<Ui::SettingsButton*> AccountsList::addFolderHeader(
		not_null<Ui::VerticalLayout*> container,
		const QString &folder,
		const std::vector<not_null<::Main::Account*>> &accountsIn,
		const std::vector<QString> &folderOrderIn) {
	const auto accounts = accountsIn;
	const auto folderOrder = folderOrderIn;
	const auto collapsed = GlobalFoldersUiState().collapsed.contains(folder);
	const auto arrow = QString(QChar(collapsed ? 0x25B8 : 0x25BE));
	// "(online/total)": online means connected and not frozen. The counts
	// are painted by hand in a child widget right after the folder name
	// (plain text can't carry colored markers, and emoji are far too big
	// here): a small green dot + online count, a small blue dot + frozen
	// count (only when there are any), then "/total)". Polled like the
	// per-account dot, through weak pointers since an account can go away
	// between a poll and the deferred rebuild that drops this header.
	auto weakAccounts = std::vector<base::weak_ptr<::Main::Account>>();
	for (const auto &account : accounts) {
		weakAccounts.push_back(base::make_weak(account.get()));
	}
	const auto baseText = arrow + u" "_q + folder + u" "_q;
	const auto button = container->add(object_ptr<Ui::SettingsButton>(
		container,
		rpl::single(baseText),
		st::mainMenuAddAccountButton));
	{
		struct CountsState {
			int online = 0;
			int frozen = 0;
			base::Timer timer;
		};
		const auto &bst = st::mainMenuAddAccountButton;
		const auto font = bst.style.font;
		const auto total = int(weakAccounts.size());
		const auto counts = Ui::CreateChild<Ui::RpWidget>(button);
		counts->setAttribute(Qt::WA_TransparentForMouseEvents);
		const auto cs = counts->lifetime().make_state<CountsState>();
		const auto dot = std::max(font->height / 3, 4);
		const auto gap = font->spacew / 2;
		const auto layout = [=] {
			auto width = font->width(u"("_q);
			width += dot + gap + font->width(QString::number(cs->online));
			if (cs->frozen) {
				width += font->spacew + dot + gap
					+ font->width(QString::number(cs->frozen));
			}
			width += font->width(u"/%1)"_q.arg(total));
			counts->resize(width, font->height);
			counts->move(
				bst.padding.left() + font->width(baseText),
				bst.padding.top());
			counts->update();
		};
		const auto poll = [=] {
			auto online = 0;
			auto frozen = 0;
			for (const auto &weak : weakAccounts) {
				const auto account = weak.get();
				if (!account || !account->sessionExists()) {
					continue;
				}
				if (AccountFrozen(account)) {
					++frozen;
				} else if (AccountConnected(account)) {
					++online;
				}
			}
			if (online != cs->online || frozen != cs->frozen) {
				cs->online = online;
				cs->frozen = frozen;
				layout();
			}
		};
		counts->paintRequest(
		) | rpl::on_next([=] {
			auto p = Painter(counts);
			auto hq = PainterHighQualityEnabler(p);
			p.setFont(font);
			p.setPen(bst.textFg);
			auto x = 0;
			const auto ascent = font->ascent;
			const auto drawText = [&](const QString &text) {
				p.drawText(x, ascent, text);
				x += font->width(text);
			};
			const auto drawDot = [&](QColor color) {
				p.setPen(Qt::NoPen);
				p.setBrush(color);
				p.drawEllipse(
					x,
					(font->height - dot) / 2 + 1,
					dot,
					dot);
				x += dot + gap;
				p.setPen(bst.textFg);
			};
			drawText(u"("_q);
			drawDot(QColor(76, 217, 100));
			drawText(QString::number(cs->online));
			if (cs->frozen) {
				x += font->spacew;
				drawDot(QColor(0, 122, 255));
				drawText(QString::number(cs->frozen));
			}
			drawText(u"/%1)"_q.arg(total));
		}, counts->lifetime());
		cs->timer.setCallback(poll);
		cs->timer.callEach(5000 + base::RandomIndex(5001));
		layout();
		poll();
		counts->show();
	}
	button->setAcceptBoth(true);

	// A folder has no photo of its own - paint a colored circle with its
	// initial in the same spot an account row's userpic sits, instead of
	// leaving that space empty next to account rows that do have one.
	const auto icon = Ui::CreateChild<Ui::RpWidget>(button);
	icon->setAttribute(Qt::WA_TransparentForMouseEvents);
	auto nameHash = uint64(0);
	for (const auto &ch : folder) {
		nameHash = nameHash * 31 + ch.unicode();
	}
	const auto emptyUserpic = std::make_shared<Ui::EmptyUserpic>(
		Ui::EmptyUserpic::UserpicColor(Ui::EmptyUserpic::ColorIndex(nameHash)),
		folder);
	const auto userpicSkip = 2 * st::mainMenuAccountLine + st::lineWidth;
	const auto userpicSize = st::mainMenuAccountSize + userpicSkip * 2;
	// The whole point of this ring is to stay visible while the folder is
	// collapsed - that's exactly when the active account's own row (and
	// its own ring, see MakeAccountButton) is hidden, with nothing else on
	// screen to say which account is actually open.
	const auto containsActive = ranges::any_of(
		accounts,
		[&](not_null<::Main::Account*> acc) {
			return acc == &_controller->session().account();
		});
	// button->height() isn't reliably its final value synchronously right
	// after construction (same as the account row above) - track it
	// reactively instead of reading it once.
	button->heightValue(
	) | rpl::on_next([=](int height) {
		const auto left = st::mainMenuAddAccountButton.iconLeft
			+ (st::settingsIconAdd.width() - userpicSize) / 2;
		const auto top = (height - userpicSize) / 2;
		icon->setGeometry(left, top, userpicSize, userpicSize);
	}, icon->lifetime());
	icon->show();
	icon->raise();
	icon->paintRequest(
	) | rpl::on_next([=] {
		auto p = Painter(icon);
		const auto size = st::mainMenuAccountSize;
		const auto line = st::mainMenuAccountLine;
		const auto full = size + userpicSkip * 2;
		emptyUserpic->paintCircle(p, userpicSkip, userpicSkip, full, size);
		if (containsActive) {
			// Same ring as MakeAccountButton draws on the active account's
			// own userpic.
			const auto shift = st::lineWidth + (line * 0.5);
			const auto diameter = full - 2 * shift;
			const auto rect = QRectF(shift, shift, diameter, diameter);
			auto hq = PainterHighQualityEnabler(p);
			auto pen = st::windowBgActive->p;
			pen.setWidthF(line);
			p.setPen(pen);
			p.setBrush(Qt::NoBrush);
			p.drawEllipse(rect);
		}
	}, icon->lifetime());

	// Aggregate unread count across every account currently in this
	// folder - polled rather than merged from each session's own change
	// stream, since the number of accounts (and so the number of streams
	// to merge) varies per folder and rpl::merge only takes a fixed,
	// compile-time argument list.
	{
		const auto container = Badge::AddRight(
			button,
			st::mainMenuAccountLine);
		container->show();
		container->raise();

		const auto events = container->lifetime(
		).make_state<rpl::event_stream<Badge::UnreadBadge>>();
		const auto recompute = [=] {
			auto total = 0;
			auto anyUnmuted = false;
			for (const auto &account : accounts) {
				if (!account->sessionExists()) {
					continue;
				}
				auto &owner = account->session().data();
				total += owner.unreadWithMentionsBadge();
				if (!owner.unreadWithMentionsBadgeMuted()) {
					anyUnmuted = true;
				}
			}
			events->fire(Badge::UnreadBadge{ total, !anyUnmuted });
		};
		const auto poll = container->lifetime().make_state<base::Timer>();
		poll->setCallback(recompute);
		poll->callEach(1000);
		// Subscribe *before* the first recompute(), not after: events is a
		// plain rpl::event_stream, which only delivers events fired after a
		// subscriber attaches. Firing first meant that initial value had
		// nobody listening yet and was silently dropped - the badge stayed
		// blank (CreateUnread's state->count starts at 0, hidden) until the
		// timer's first real tick a full second later caught up, which is
		// exactly what read as the folder's unread count "jumping" right
		// after every rebuild (collapsing/expanding it, among other things,
		// rebuilds this header from scratch - see AccountsList::rebuild).
		const auto unread = Badge::CreateUnread(
			container.get(),
			events->events());
		recompute();
		unread->sizeValue(
		) | rpl::on_next([=](const QSize &s) {
			container->resize(s);
		}, container->lifetime());
	}

	// A standalone overlay pinned to the folder row's own right edge,
	// same as the account row's pin/mute badges - kept independent of the
	// Badge::AddRight/unread-count pairing above rather than folded into
	// it, so it can never end up fighting that pairing's own width/padding
	// math for space (see the account row's own version of this for the
	// full story on how that went wrong there).
	const auto isPinned = GlobalFoldersUiState().pinned.contains(folder);
	if (isPinned) {
		const auto pinBadge = Ui::CreateChild<Ui::RpWidget>(button);
		const auto badgeSize = st::mainMenuAccountBadgeSize;
		pinBadge->resize(badgeSize, badgeSize);
		const auto pinImage = st::dialogsPinnedIcon.icon.instance(
			st::dialogsUnreadBgMuted->c);
		pinBadge->paintRequest(
		) | rpl::on_next([=] {
			auto p = Painter(pinBadge);
			p.drawImage(QRect(0, 0, badgeSize, badgeSize), pinImage);
		}, pinBadge->lifetime());
		// A folder header is never indented (only the accounts nested
		// under it are), so its width is always exactly the panel's own -
		// use that fixed constant instead of the button's own sizeValue(),
		// which (unlike heightValue(), see the icon geometry above) wasn't
		// reliably reflecting the post-rebuild width when this folder
		// collapsed as a side effect of expanding a different one, leaving
		// the badge positioned from a stale width until the whole panel
		// was torn down and rebuilt from scratch.
		button->heightValue(
		) | rpl::on_next([=](int height) {
			pinBadge->move(
				st::mainMenuWidth
					- st::mainMenuAccountBadgeRightMargin
					- badgeSize,
				(height - badgeSize) / 2);
		}, pinBadge->lifetime());
	}

	button->clicks(
	) | rpl::on_next([=](Qt::MouseButton which) {
		if (which == Qt::LeftButton) {
			auto &state = GlobalFoldersUiState();
			if (state.collapsed.contains(folder)) {
				state.collapsed.remove(folder);
				for (const auto &other : folderOrder) {
					if (other != folder) {
						state.collapsed.insert(other);
					}
				}
			} else {
				state.collapsed.insert(folder);
			}
			SaveFoldersUiState();
			scheduleRebuild();
			return;
		} else if (which != Qt::RightButton) {
			return;
		}
		_folderContextMenu = base::make_unique_q<Ui::PopupMenu>(_outer, st::folderContextMenu);
		_folderContextMenu->addAction(
			GlobalFoldersUiState().pinned.contains(folder)
				? u"Открепить папку"_q
				: u"Закрепить папку"_q,
			[=] { toggleFolderPinned(folder); },
			&st::menuIconPin);
		_folderContextMenu->addAction(u"В онлайн"_q, [=] {
			for (const auto &acc : accounts) {
				acc->setAlwaysOffline(false);
				if (const auto session = acc->maybeSession()) {
					session->updates().updateOnline();
					acc->mtp().restart();
				}
			}
			scheduleRebuild();
		}, &st::menuIconShowInChat);
		_folderContextMenu->addAction(u"В спящий режим"_q, [=] {
			for (const auto &acc : accounts) {
				acc->setAlwaysOffline(true);
				if (const auto session = acc->maybeSession()) {
					session->updates().updateOnline();
				}
			}
			scheduleRebuild();
		}, &st::menuIconMute);
		_folderContextMenu->addAction(u"Переименовать..."_q, [=] {
			promptRenameFolder(folder);
		}, &st::menuIconEdit);
		_folderContextMenu->addAction(u"Убрать папку"_q, [=] {
			removeFolder(folder);
		}, &st::menuIconRemove);
		_folderContextMenu->addAction(u"Экспорт папки..."_q, [=] {
			exportFolder(folder, accounts);
		}, &st::menuIconExport);
		_folderContextMenu->addAction(u"Заменить прокси..."_q, [=] {
			replaceFolderProxy(folder, accounts);
		}, &st::menuIconSettings);
		_folderContextMenu->addAction(u"Удалить с аккаунтами..."_q, [=] {
			ConfirmDeleteAccounts(
				_controller,
				QString::number(accounts.size())
					+ u" аккаунт(ов) в папке \""_q + folder + u"\""_q,
				accounts,
				folder);
		}, &st::menuIconDeleteAttention);
		_folderContextMenu->popup(QCursor::pos());
	}, button->lifetime());

	return button;
}

void AccountsList::moveAccountToFolder(
		not_null<::Main::Account*> account,
		const QString &folder) {
	account->setFolderTag(folder);
	scheduleRebuild();
}

void AccountsList::togglePinned(not_null<::Main::Account*> account) {
	account->setPinned(!account->pinned());
	scheduleRebuild();
}

void AccountsList::toggleMuted(not_null<::Main::Account*> account) {
	account->setMuted(!account->muted());
	scheduleRebuild();
}

void AccountsList::toggleAlwaysOffline(not_null<::Main::Account*> account) {
	account->setAlwaysOffline(!account->alwaysOffline());
	if (const auto session = account->maybeSession()) {
		// Push the new status right away instead of waiting for the next
		// periodic ping (session->updates().updateOnline() re-reads the
		// flag we just flipped) or the next focus/blur event.
		session->updates().updateOnline();
	}
	scheduleRebuild();
}

void AccountsList::toggleFolderPinned(const QString &folder) {
	auto &state = GlobalFoldersUiState();
	if (state.pinned.contains(folder)) {
		state.pinned.remove(folder);
	} else {
		state.pinned.insert(folder);
	}
	SaveFoldersUiState();
	scheduleRebuild();
}

void AccountsList::promptNewFolder(not_null<::Main::Account*> account) {
	PromptFolderName(_controller, u"Новая папка"_q, QString(), [=](
			QString name) {
		moveAccountToFolder(account, name);
	});
}

void AccountsList::promptRenameFolder(const QString &folder) {
	PromptFolderName(_controller, u"Переименовать папку"_q, folder, [=](
			QString name) {
		if (name == folder) {
			return;
		}
		for (const auto &account
				: _controller->session().domain().orderedAccounts()) {
			if (account->folderTag() == folder) {
				account->setFolderTag(name);
			}
		}
		auto &state = GlobalFoldersUiState();
		if (state.collapsed.remove(folder)) {
			state.collapsed.insert(name);
		}
		if (state.pinned.remove(folder)) {
			state.pinned.insert(name);
		}
		ranges::replace(state.order, folder, name);
		SaveFoldersUiState();
		scheduleRebuild();
	});
}

void AccountsList::removeFolder(const QString &folder) {
	for (const auto &account
			: _controller->session().domain().orderedAccounts()) {
		if (account->folderTag() == folder) {
			account->setFolderTag(QString());
		}
	}
	auto &state = GlobalFoldersUiState();
	state.collapsed.remove(folder);
	state.pinned.remove(folder);
	state.order.erase(
		ranges::remove(state.order, folder),
		state.order.end());
	SaveFoldersUiState();
	scheduleRebuild();
}

void AccountsList::exportFolder(
		const QString &folder,
		const std::vector<not_null<::Main::Account*>> &accounts) {
	// Every account of the folder under one <destination>/<folder name>/
	// wrapper, each in its own <phone> folder.
	ShowExportOptionsBox(
		_controller,
		QString::number(accounts.size()) + u" аккаунт(ов) в папке \""_q
			+ folder + u"\""_q,
		accounts,
		u"Экспорт папки \"%1\""_q.arg(folder),
		[=](const QString &destination) {
			const auto root = destination + '/' + SafeFolderPart(folder);
			auto items = std::vector<TdataExportItem>();
			auto unnamed = 0;
			for (const auto &account : accounts) {
				if (account->sessionExists()) {
					items.push_back({
						account,
						root + '/' + PhoneFolderName(account, &unnamed),
					});
				}
			}
			return items;
		});
}

void AccountsList::replaceFolderProxy(
		const QString &folder,
		const std::vector<not_null<::Main::Account*>> &accounts) {
	if (!Core::GuardAllows(
			_controller->uiShow(),
			Core::GuardedAction::ReplaceProxy)) {
		return;
	}
	// Same "pick/paste a proxy list, one distinct entry per account" flow
	// already used for importing several accounts at once (see
	// ImportTdataFolder in ggtg_add_accounts.h) - here every account already exists, so
	// each one just gets its proxy override replaced live instead of
	// being created with one.
	//
	// setProxyOverride() below restarts that account's MTP connection - if
	// one of these accounts happens to be the one whose window this
	// settings panel is open in, that restart can tear this window (and
	// so this panel, and _controller along with it) down before the loop
	// even finishes. Guard the follow-up show() with a weak pointer
	// instead of assuming _controller is still around by then - a crash
	// on exactly that assumption is what this replaced (SessionController
	// derives from base::has_weak_ptr, so this works the same way the
	// logout confirmation just above guards its own account/session).
	const auto weakController = base::make_weak(_controller.get());
	_controller->show(MakeMultiProxyBox([=](Fn<MTP::ProxyData()> nextProxy) {
		// Simple loop, no per-step async boundary - one completed entry in
		// "Процессы" is enough to show it happened, proportionate to how
		// simple the operation itself is (unlike the export flow above,
		// which is genuinely multi-step and live).
		const auto process = Core::App().processes().start(
			u"Замена прокси в папке \"%1\""_q.arg(folder));
		auto changed = 0;
		for (const auto &account : accounts) {
			account->setProxyOverride(nextProxy());
			AutoDeviceGeo(account);
			++changed;
		}
		process->succeed(u"Прокси заменён у %1 аккаунт(ов)."_q.arg(changed));
		if (const auto controller = weakController.get()) {
			controller->show(Ui::MakeInformBox(
				u"Прокси заменён у %1 аккаунт(ов) в папке \"%2\". Аккаунты, "
					"на которых список закончился, останутся без рабочего "
					"прокси - задайте им прокси вручную (правый клик → "
					"Прокси)."_q
					.arg(changed)
					.arg(folder)));
		}
	}));
}

void BuildInformationSection(SectionBuilder &builder) {
	builder.add(nullptr, [] {
		return SearchEntry{
			.id = u"edit/bio"_q,
			.title = tr::lng_bio_placeholder(tr::now),
			.keywords = { u"bio"_q, u"about"_q, u"description"_q },
		};
	});
	builder.add(nullptr, [] {
		return SearchEntry{
			.id = u"edit/name"_q,
			.title = tr::lng_settings_name_label(tr::now),
			.keywords = { u"name"_q, u"first"_q, u"last"_q },
		};
	});
	builder.add(nullptr, [] {
		return SearchEntry{
			.id = u"edit/phone"_q,
			.title = tr::lng_settings_phone_label(tr::now),
			.keywords = { u"phone"_q, u"number"_q, u"mobile"_q },
		};
	});
	builder.add(nullptr, [] {
		return SearchEntry{
			.id = u"edit/username"_q,
			.title = tr::lng_settings_username_label(tr::now),
			.keywords = { u"username"_q, u"link"_q, u"t.me"_q },
		};
	});
	builder.add(nullptr, [] {
		return SearchEntry{
			.id = u"edit/your-color"_q,
			.title = tr::lng_settings_theme_name_color(tr::now),
			.keywords = { u"color"_q, u"theme"_q, u"name"_q },
		};
	});
	builder.add(nullptr, [] {
		return SearchEntry{
			.id = u"edit/channel"_q,
			.title = tr::lng_settings_channel_label(tr::now),
			.keywords = { u"channel"_q, u"personal"_q },
		};
	});
	builder.add(nullptr, [] {
		return SearchEntry{
			.id = u"edit/birthday"_q,
			.title = tr::lng_settings_birthday_label(tr::now),
			.keywords = { u"birthday"_q, u"date"_q, u"birth"_q },
		};
	});
	builder.add(nullptr, [] {
		return SearchEntry{
			.id = u"edit/chat-automation"_q,
			.title = tr::lng_settings_chat_automation_label(tr::now),
			.keywords = {
				u"chat"_q,
				u"automation"_q,
				u"bot"_q,
				u"chatbot"_q,
				u"chatbots"_q,
			},
		};
	});
	builder.add(nullptr, [] {
		return SearchEntry{
			.id = u"edit/add-account"_q,
			.title = tr::lng_menu_add_account(tr::now),
			.keywords = { u"account"_q, u"add"_q, u"switch"_q, u"multiple"_q },
		};
	});
}

class Information : public Section<Information> {
public:
	Information(
		QWidget *parent,
		not_null<Window::SessionController*> controller);

	[[nodiscard]] rpl::producer<QString> title() override;
	void showFinished() override;

private:
	void setupContent();

	QPointer<Ui::RpWidget> _photo;
	QPointer<Ui::RpWidget> _uploadPhoto;
	QPointer<Ui::RpWidget> _bio;
	QPointer<Ui::RpWidget> _colorButton;
	QPointer<Ui::RpWidget> _channelButton;
	QPointer<Ui::RpWidget> _addAccount;
	QPointer<Ui::RpWidget> _name;
	QPointer<Ui::RpWidget> _phone;
	QPointer<Ui::RpWidget> _username;
	QPointer<Ui::RpWidget> _birthday;
	QPointer<Ui::RpWidget> _chatAutomation;

};

Information::Information(
	QWidget *parent,
	not_null<Window::SessionController*> controller)
: Section(parent, controller) {
	setupContent();
}

rpl::producer<QString> Information::title() {
	return tr::lng_settings_section_info();
}

void Information::showFinished() {
	Section<Information>::showFinished();
}

void Information::setupContent() {
	const auto content = Ui::CreateChild<Ui::VerticalLayout>(this);

	const SectionBuildMethod buildMethod = [
		photo = &_photo,
		uploadPhoto = &_uploadPhoto,
		bio = &_bio,
		colorButton = &_colorButton,
		channelButton = &_channelButton,
		addAccount = &_addAccount,
		name = &_name,
		phone = &_phone,
		username = &_username,
		birthday = &_birthday,
		chatAutomation = &_chatAutomation
	](
			not_null<Ui::VerticalLayout*> container,
			not_null<Window::SessionController*> controller,
			Fn<void(Type)> showOther,
			rpl::producer<> showFinished) {
		auto &lifetime = container->lifetime();
		const auto highlights = lifetime.make_state<HighlightRegistry>();
		const auto isPaused = Window::PausedIn(
			controller,
			Window::GifPauseReason::Layer);

		auto builder = SectionBuilder(WidgetContext{
			.container = container,
			.controller = controller,
			.showOther = std::move(showOther),
			.isPaused = isPaused,
			.highlights = highlights,
		});

		const auto self = controller->session().user();
		auto targets = InformationHighlightTargets();

		SetupPhoto(container, controller, self, &targets);
		SetupBio(container, self, &targets);
		SetupRows(container, controller, self, &targets);
		SetupPersonalChannel(container, controller, self, &targets);
		SetupBirthday(container, controller, self, &targets);
		SetupAccountsWrap(container, controller, &targets);

		*photo = targets.photo;
		*uploadPhoto = targets.uploadPhoto;
		*bio = targets.bio;
		*colorButton = targets.colorButton;
		*channelButton = targets.channelButton;
		*addAccount = targets.addAccount;
		*name = targets.name;
		*phone = targets.phone;
		*username = targets.username;
		*birthday = targets.birthday;
		*chatAutomation = targets.chatAutomation;

		if (highlights) {
			if (*photo) {
				highlights->push_back({
					u"profile-photo"_q,
					{ photo->data(), { .shape = HighlightShape::Ellipse } },
				});
			}
			if (*uploadPhoto) {
				highlights->push_back({
					u"profile-photo/use-emoji"_q,
					{ uploadPhoto->data(), { .shape = HighlightShape::Ellipse } },
				});
			}
			if (*bio) {
				highlights->push_back({
					u"edit/bio"_q,
					{ bio->data(), { .margin = st::settingsBioHighlightMargin } },
				});
			}
			if (*colorButton) {
				highlights->push_back({
					u"edit/your-color"_q,
					{ colorButton->data(), { .rippleShape = true } },
				});
			}
			if (*channelButton) {
				highlights->push_back({
					u"edit/channel"_q,
					{ channelButton->data(), { .rippleShape = true } },
				});
			}
			if (*addAccount) {
				highlights->push_back({
					u"edit/add-account"_q,
					{ addAccount->data(), { .rippleShape = true } },
				});
			}
			if (*name) {
				highlights->push_back({
					u"edit/name"_q,
					{ name->data(), { .rippleShape = true } },
				});
			}
			if (*phone) {
				highlights->push_back({
					u"edit/phone"_q,
					{ phone->data(), { .rippleShape = true } },
				});
			}
			if (*username) {
				highlights->push_back({
					u"edit/username"_q,
					{ username->data(), { .rippleShape = true } },
				});
			}
			if (*birthday) {
				highlights->push_back({
					u"edit/birthday"_q,
					{ birthday->data(), { .rippleShape = true } },
				});
			}
			if (*chatAutomation) {
				highlights->push_back({
					u"edit/chat-automation"_q,
					{ chatAutomation->data(), { .rippleShape = true } },
				});
			}
		}

		std::move(showFinished) | rpl::on_next([=] {
			for (const auto &[id, entry] : *highlights) {
				if (entry.widget) {
					controller->checkHighlightControl(
						id,
						entry.widget,
						base::duplicate(entry.args));
				}
			}
		}, lifetime);
	};

	build(content, buildMethod);

	Ui::ResizeFitChild(this, content);
}

const auto kMeta = BuildHelper({
	.id = Information::Id(),
	.parentId = MainId(),
	.title = &tr::lng_settings_section_info,
	.icon = &st::menuIconProfile,
}, [](SectionBuilder &builder) {
	BuildInformationSection(builder);
});

} // namespace

Fn<void(Fn<void(QString password)> submit, Fn<void()> cancel)>
MakeSessionPasswordPrompt(not_null<::Main::Account*> account) {
	const auto weak = base::make_weak(account.get());
	return [=](Fn<void(QString password)> submit, Fn<void()> cancel) {
		const auto strong = weak.get();
		const auto window = ActiveSessionWindow();
		if (strong && window) {
			PromptSessionDuplicatePassword(window, strong, submit, cancel);
		} else {
			cancel();
		}
	};
}

Type InformationId() {
	return Information::Id();
}

AccountsEvents SetupAccounts(
		not_null<Ui::VerticalLayout*> container,
		not_null<Window::SessionController*> controller) {
	const auto list = container->lifetime().make_state<AccountsList>(
		container,
		controller);
	return {
		.closeRequests = list->closeRequests(),
		.addAccountButton = list->addAccountButton(),
	};
}

void UpdatePhotoLocally(not_null<UserData*> user, const QImage &image) {
	auto bytes = QByteArray();
	auto buffer = QBuffer(&bytes);
	image.save(&buffer, "JPG", 87);
	user->setUserpic(
		base::RandomValue<PhotoId>(),
		ImageLocation(
			{ .data = InMemoryLocation{ .bytes = bytes } },
			image.width(),
			image.height()),
		false);
}

// The GGTG "Экспорт" section's first two modes (the third, a .ggtg file for
// GGTG Android, lives in settings/ggtg_export.cpp): the accounts as plain
// tdata folders, one "<group>: <count>/<phone>" folder each, the way the
// old right-click "Экспорт аккаунтов..." wrote them - either their own
// live sessions (`duplicate` false) or brand-new independent ones.
void StartTdataExport(
		not_null<Window::SessionController*> window,
		std::vector<not_null<::Main::Account*>> accounts,
		TdataExportOptions options,
		const QString &title,
		TdataExportLayout layout) {
	accounts.erase(std::remove_if(accounts.begin(), accounts.end(), [](
			not_null<::Main::Account*> account) {
		return !::Main::AccountIsLive(account);
	}), accounts.end());
	if (accounts.empty()) {
		window->uiShow()->showToast(u"Нет подключённых аккаунтов для экспорта"_q);
		return;
	}
	const auto summary = TdataExportSummary(accounts, options);
	window->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(title));
		box->addRow(object_ptr<Ui::FlatLabel>(box, summary, st::boxLabel));
		box->addButton(rpl::single(u"Выбрать папку и начать"_q), [=] {
			box->closeBox();
			// The working session is swapped or removed: nothing that
			// holds the accounts (a campaign) may be running.
			if (options.source == TdataSource::Working
				&& !Core::GuardAllows(
					window->uiShow(),
					Core::GuardedAction::RemoveAccount)) {
				return;
			}
			const auto destination = QFileDialog::getExistingDirectory(
				nullptr,
				u"Выберите папку для экспорта"_q);
			if (destination.isEmpty()) {
				return;
			}
			const auto run = std::make_shared<TdataExportRun>();
			run->options = options;
			for (const auto &item : layout(destination)) {
				run->accounts.push_back(base::make_weak(item.account.get()));
				run->folders.push_back(item.accountFolder);
				run->labels.push_back(QFileInfo(item.accountFolder).fileName());
			}
			if (run->accounts.empty()) {
				return;
			}
			run->process = Core::App().processes().start(
				title,
				nullptr,
				Core::ProcessKind::Export);
			ShowProcessBox(window, run->process->id());
			TdataExportNext(run, 0);
		});
		box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
	}));
}

void ExportAccountsAsTdata(
		not_null<Window::SessionController*> window,
		std::vector<not_null<::Main::Account*>> accounts,
		TdataExportOptions options) {
	const auto count = int(accounts.size());
	StartTdataExport(
		window,
		accounts,
		options,
		(count == 1)
			? u"Экспорт аккаунта"_q
			: u"Экспорт аккаунтов (%1)"_q.arg(count),
		[=](const QString &destination) {
			// <destination>/<folder>/<phone>/tdata (+ 2fa.txt and ggtg.json
			// beside it); accounts without a folder go to "Без папки".
			auto grouped = base::flat_map<
				QString,
				std::vector<not_null<::Main::Account*>>>();
			for (const auto &account : accounts) {
				if (account->sessionExists()) {
					grouped[account->folderTag()].push_back(account);
				}
			}
			auto items = std::vector<TdataExportItem>();
			auto unnamed = 0;
			for (const auto &[tag, group] : grouped) {
				const auto root = destination + '/' + GroupFolderName(tag);
				for (const auto &account : group) {
					items.push_back({
						account,
						root + '/' + PhoneFolderName(account, &unnamed),
					});
				}
			}
			return items;
		});
}

namespace Badge {

Ui::UnreadBadgeStyle Style() {
	auto result = Ui::UnreadBadgeStyle();
	result.font = st::mainMenuBadgeFont;
	result.size = st::mainMenuBadgeSize;
	result.sizeId = Ui::UnreadBadgeSize::MainMenu;
	return result;
}

not_null<Ui::RpWidget*> AddRight(
		not_null<Ui::SettingsButton*> button,
		int rightPadding) {
	const auto widget = Ui::CreateChild<Ui::RpWidget>(button.get());

	rpl::combine(
		button->sizeValue(),
		widget->sizeValue(),
		widget->shownValue()
	) | rpl::on_next([=](QSize outer, QSize inner, bool shown) {
		auto padding = button->st().padding;
		if (shown) {
			widget->moveToRight(
				padding.right() + rightPadding,
				(outer.height() - inner.height()) / 2,
				outer.width());
			padding.setRight(padding.right() + inner.width() + rightPadding);
		}
		button->setPaddingOverride(padding);
		button->update();
	}, widget->lifetime());

	return widget;
}

not_null<Ui::RpWidget*> CreateUnread(
		not_null<Ui::RpWidget*> container,
		rpl::producer<UnreadBadge> value) {
	struct State {
		State(QWidget *parent) : widget(parent) {
			widget.setAttribute(Qt::WA_TransparentForMouseEvents);
		}

		Ui::RpWidget widget;
		Ui::UnreadBadgeStyle st = Style();
		int count = 0;
		QString string;
	};
	const auto state = container->lifetime().make_state<State>(container);

	std::move(
		value
	) | rpl::on_next([=](UnreadBadge badge) {
		state->st.muted = badge.muted;
		state->count = badge.count;
		if (!state->count) {
			state->widget.hide();
			return;
		}
		state->string = Lang::FormatCountToShort(state->count).string;
		state->widget.resize(Ui::CountUnreadBadgeSize(state->string, state->st));
		if (state->widget.isHidden()) {
			state->widget.show();
		}
	}, state->widget.lifetime());

	state->widget.paintRequest(
	) | rpl::on_next([=] {
		auto p = Painter(&state->widget);
		Ui::PaintUnreadBadge(
			p,
			state->string,
			state->widget.width(),
			0,
			state->st);
	}, state->widget.lifetime());

	return &state->widget;
}

void AddUnread(
		not_null<Ui::SettingsButton*> button,
		rpl::producer<UnreadBadge> value) {
	const auto container = AddRight(button);
	const auto badge = CreateUnread(container, std::move(value));
	badge->sizeValue(
	) | rpl::on_next([=](const QSize &s) {
		container->resize(s);
	}, container->lifetime());
}

} // namespace Badge

namespace Builder {

SectionBuildMethod InformationSection = kMeta.build;

} // namespace Builder
} // namespace Settings
