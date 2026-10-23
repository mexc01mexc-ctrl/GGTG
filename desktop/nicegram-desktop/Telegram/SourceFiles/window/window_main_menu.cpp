/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "window/window_main_menu.h"

#include "window/ggtg_widgets.h"
#include "window/ggtg_home_widgets.h"
#include "apiwrap.h"
#include "base/event_filter.h"
#include "base/flat_set.h"
#include "base/qt_signal_producer.h"
#include "base/timer.h"
#include "base/call_delayed.h"
#include "mainwindow.h"
#include "settings/process_box.h"
#include "boxes/about_box.h"
#include "core/update_channel.h"
#include "boxes/peer_list_controllers.h"
#include "boxes/premium_preview_box.h"
#include "calls/group/calls_group_common.h"
#include "calls/calls_box_controller.h"
#include "calls/calls_instance.h"
#include "core/application.h"
#include "core/click_handler_types.h"
#include "data/data_changes.h"
#include "data/data_document_media.h"
#include "data/data_folder.h"
#include "data/data_group_call.h"
#include "data/data_session.h"
#include "data/data_stories.h"
#include "data/data_user.h"
#include "info/info_memento.h"
#include "info/profile/info_profile_badge.h"
#include "settings/settings_common.h"
#include "info/profile/info_profile_emoji_status_panel.h"
#include "info/profile/info_profile_icon.h"
#include "info/stories/info_stories_widget.h"
#include "lang/lang_keys.h"
#include "main/main_account.h"
#include "main/main_app_config.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "main/main_session_settings.h"
#include "settings.h"
#include "mtproto/mtproto_config.h"
#include "settings/sections/settings_advanced.h"
#include "settings/sections/settings_calls.h"
#include "settings/sections/settings_information.h"
#include "settings/sections/settings_processes.h"
#include "core/ggtg_log.h"
#include "window/ggtg_squidward.h"
#include "core/process_manager.h"
#include "storage/localstorage.h"
#include "storage/storage_account.h"
#include "support/support_templates.h"
#include "tde2e/tde2e_api.h"
#include "tde2e/tde2e_integration.h"
#include "ui/boxes/confirm_box.h"
#include "ui/chat/chat_theme.h"
#include "ui/controls/swipe_handler.h"
#include "ui/controls/userpic_button.h"
#include "ui/effects/snowflakes.h"
#include "ui/effects/toggle_arrow.h"
#include "ui/new_badges.h"
#include "ui/painter.h"
#include "ui/text/text_options.h"
#include "ui/text/text_utilities.h"
#include "ui/ui_utility.h"
#include "ui/unread_badge_paint.h"
#include "ui/vertical_list.h"
#include "ui/widgets/menu/menu_add_action_callback_factory.h"
#include "ui/widgets/popup_menu.h"
#include "ui/widgets/scroll_area.h"
#include "ui/widgets/shadow.h"
#include "ui/wrap/slide_wrap.h"
#include "wallet/wallet_panel.h"
#include "window/themes/window_theme.h"
#include "window/window_controller.h"
#include "window/window_main_menu_helpers.h"
#include "boxes/connection_box.h"
#include "settings/bulk_accounts_actions.h"
#include "settings/ggtg_export.h"
#include "settings/views_campaign.h"
#include "data/scraped_members_db.h"
#include "data/account_spam_status.h"
#include "settings/sms_campaign.h"
#include "settings/ggtg_ai_cards.h"
#include "mtproto/proxy_pool.h"
#include "ui/layers/generic_box.h"
#include "ui/layers/layer_widget.h"
#include "settings/ggtg_ai_box.h"
#include "settings/ggtg_sync_box.h"
#include "settings/ggtg_add_accounts.h"
#include "settings/ggtg_registration.h"
#include "settings/ggtg_chat_lists_ui.h"
#include "settings/ggtg_warmup.h"
#include "ui/widgets/labels.h"
#include "api/api_bot.h"
#include "api/api_common.h"
#include "base/weak_ptr.h"
#include "base/weak_qptr.h"
#include "data/data_peer_id.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_item_components.h"
#include "history/history_item_reply_markup.h"
#include "mtproto/sender.h"
#include "ui/abstract_button.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/separate_panel.h"
#include "ui/wrap/padding_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "styles/style_layers.h"
#include "window/window_peer_menu.h"
#include "window/window_session_controller.h"

#include "styles/style_chat.h" // popupMenuExpandedSeparator
#include "styles/style_dialogs.h" // dialogsMenuToggle, for the sidebar collapse button
#include "styles/style_menu_icons.h"
#include "styles/style_boxes.h"
#include "styles/style_settings.h"
#include "styles/style_widgets.h"
#include "styles/style_wallet.h"
#include "styles/style_window.h"
#include "styles/style_window_main_menu.h"

#include <QtGui/QWindow>
#include <tuple>
#include <array>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSaveFile>
#include <QtGui/QScreen>

#include <QtGui/QGuiApplication>
#include <QtWidgets/QApplication>
#include <QtGui/QClipboard>
#include <QtCore/QRegularExpression>
#include <QtCore/QPointer>

#include "ui/text/text_entity.h"

namespace Window {
namespace {

// A sidebar label ends with this mark while its section is busy: the row
// draws Telegram's own "sending" clock icon instead of an emoji.
constexpr auto kBusyMark = QChar(0xE000);

constexpr auto kPlayStatusLimit = 2;

// MainWindow::showMainMenu() makes a brand new MainMenu every time the menu
// is opened (it isn't a persistent widget just shown/hidden), so its own
// Ui::ScrollArea always starts back at the top - this is what actually
// survives that recreation, saved on every scroll and restored once the
// freshly-built menu's content has its final height. Not written to disk:
// it only needs to survive one open->close->open cycle in the same run,
// not an app restart, so a plain global is enough - and in the ordinary
// case of one window there's only ever one of these menus visible anyway.
int GlobalMainMenuScrollTop = 0;

[[nodiscard]] bool CanCheckSpecialEvent() {
	static const auto result = [] {
		const auto now = QDate::currentDate();
		return (now.month() == 12) || (now.month() == 1 && now.day() == 1);
	}();
	return result;
}

[[nodiscard]] bool CheckSpecialEvent() {
	const auto now = QDate::currentDate();
	return (now.month() == 12 && now.day() >= 24)
		|| (now.month() == 1 && now.day() == 1);
}

[[nodiscard]] rpl::producer<TextWithEntities> SetStatusLabel(
		not_null<Main::Session*> session) {
	const auto self = session->user();
	return session->changes().peerFlagsValue(
		self,
		Data::PeerUpdate::Flag::EmojiStatus
	) | rpl::map([=] {
		return !!self->emojiStatusId();
	}) | rpl::distinct_until_changed() | rpl::map([](bool has) {
		const auto makeLink = [](const QString &text) {
			return tr::link(text);
		};
		return (has
			? tr::lng_menu_change_status
			: tr::lng_menu_set_status)(makeLink);
	}) | rpl::flatten_latest();
}

} // namespace

class MainMenu::ToggleAccountsButton final : public Ui::AbstractButton {
public:
	ToggleAccountsButton(QWidget *parent, not_null<Main::Account*> current);

	[[nodiscard]] int rightSkip() const {
		return _rightSkip.current();
	}
	[[nodiscard]] rpl::producer<int> rightSkipValue() const {
		return _rightSkip.value();
	}

private:
	void paintEvent(QPaintEvent *e) override;
	void paintUnreadBadge(Painter &p);

	void validateUnreadBadge();
	[[nodiscard]] QString computeUnreadBadge() const;

	const not_null<Main::Account*> _current;
	rpl::variable<int> _rightSkip = 0;
	Ui::Animations::Simple _toggledAnimation;
	bool _toggled = false;

	QString _unreadBadge;
	bool _unreadBadgeStale = false;

};

class MainMenu::ResetScaleButton final : public Ui::AbstractButton {
public:
	ResetScaleButton(QWidget *parent);

protected:
	void paintEvent(QPaintEvent *e) override;

	static constexpr auto kText = "100%";

};

MainMenu::ToggleAccountsButton::ToggleAccountsButton(
	QWidget *parent,
	not_null<Main::Account*> current)
: AbstractButton(parent)
, _current(current) {
	rpl::single(rpl::empty) | rpl::then(
		Core::App().unreadBadgeChanges()
	) | rpl::on_next([=] {
		_unreadBadgeStale = true;
		if (!_toggled) {
			validateUnreadBadge();
			update();
		}
	}, lifetime());

	auto &settings = Core::App().settings();
	if (Core::App().domain().accounts().size() < 2
		&& settings.mainMenuAccountsShown()) {
		settings.setMainMenuAccountsShown(false);
	}
	settings.mainMenuAccountsShownValue(
	) | rpl::filter([=](bool value) {
		return (_toggled != value);
	}) | rpl::on_next([=](bool value) {
		_toggled = value;
		_toggledAnimation.start(
			[=] { update(); },
			_toggled ? 0. : 1.,
			_toggled ? 1. : 0.,
			st::slideWrapDuration);
		validateUnreadBadge();
	}, lifetime());
	_toggledAnimation.stop();
}

void MainMenu::ToggleAccountsButton::paintEvent(QPaintEvent *e) {
	auto p = Painter(this);

	const auto path = Ui::ToggleUpDownArrowPath(
		0. + width() - st::mainMenuTogglePosition.x(),
		0. + height() - st::mainMenuTogglePosition.y(),
		st::mainMenuToggleSize,
		st::mainMenuToggleFourStrokes,
		_toggledAnimation.value(_toggled ? 1. : 0.));

	auto hq = PainterHighQualityEnabler(p);
	p.fillPath(path, st::windowSubTextFg);

	paintUnreadBadge(p);
}

void MainMenu::ToggleAccountsButton::paintUnreadBadge(Painter &p) {
	const auto progress = 1. - _toggledAnimation.value(_toggled ? 1. : 0.);
	if (!progress) {
		return;
	}
	validateUnreadBadge();
	if (_unreadBadge.isEmpty()) {
		return;
	}

	auto st = Settings::Badge::Style();
	const auto right = width()
		- st::mainMenuTogglePosition.x()
		- st::mainMenuToggleSize * 3;
	const auto top = height()
		- st::mainMenuTogglePosition.y()
		- st::mainMenuBadgeSize / 2;
	p.setOpacity(progress);
	Ui::PaintUnreadBadge(p, _unreadBadge, right, top, st);
}

void MainMenu::ToggleAccountsButton::validateUnreadBadge() {
	const auto base = st::mainMenuTogglePosition.x()
		+ 2 * st::mainMenuToggleSize;
	if (_toggled) {
		_rightSkip = base;
		return;
	} else if (!_unreadBadgeStale) {
		return;
	}
	_unreadBadge = computeUnreadBadge();

	auto skip = base;
	if (!_unreadBadge.isEmpty()) {
		const auto st = Settings::Badge::Style();
		skip += 2 * st::mainMenuToggleSize
			+ Ui::CountUnreadBadgeSize(_unreadBadge, st).width();
	}
	_rightSkip = skip;
}

QString MainMenu::ToggleAccountsButton::computeUnreadBadge() const {
	const auto state = OtherAccountsUnreadStateCurrent(_current);
	return state.allMuted
		? QString()
		: (state.count > 0)
		? Lang::FormatCountToShort(state.count).string
		: QString();
}

MainMenu::ResetScaleButton::ResetScaleButton(QWidget *parent)
: AbstractButton(parent) {
	const auto margin = st::mainMenuCloudButton.height
		- st::mainMenuCloudSize;
	const auto textWidth = st::mainMenuResetScaleFont->width(kText);
	const auto innerWidth = st::mainMenuResetScaleLeft
		+ textWidth
		+ st::mainMenuResetScaleRight;
	const auto width = margin + innerWidth;
	resize(width, st::mainMenuCloudButton.height);
}

void MainMenu::ResetScaleButton::paintEvent(QPaintEvent *e) {
	Painter p(this);

	const auto innerHeight = st::mainMenuCloudSize;
	const auto radius = innerHeight / 2;
	const auto margin = st::mainMenuCloudButton.height
		- st::mainMenuCloudSize;
	const auto textWidth = st::mainMenuResetScaleFont->width(kText);
	const auto innerWidth = st::mainMenuResetScaleLeft
		+ textWidth
		+ st::mainMenuResetScaleRight;
	const auto left = margin / 2;
	const auto top = margin / 2;
	p.setPen(Qt::NoPen);
	p.setBrush(st::mainMenuCloudBg);
	p.drawRoundedRect(left, top, innerWidth, innerHeight, radius, radius);

	st::settingsIconInterfaceScale.paint(
		p,
		left + st::mainMenuResetScaleIconLeft,
		top + ((innerHeight - st::settingsIconInterfaceScale.height()) / 2),
		width(),
		st::mainMenuCloudFg->c);

	p.setFont(st::mainMenuResetScaleFont);
	p.setPen(st::mainMenuCloudFg);
	p.drawText(
		left + st::mainMenuResetScaleLeft,
		top + st::mainMenuResetScaleTop + st::mainMenuResetScaleFont->ascent,
		kText);
}

MainMenu::MainMenu(
	QWidget *parent,
	not_null<SessionController*> controller)
: LayerWidget(parent)
, _controller(controller)
, _userpicButton(
	this,
	_controller->session().user(),
	st::mainMenuUserpic)
, _toggleAccounts(this, &controller->session().account())
, _setEmojiStatus(this, SetStatusLabel(&controller->session()))
, _emojiStatusPanel(std::make_unique<Info::Profile::EmojiStatusPanel>())
, _badge(std::make_unique<Info::Profile::Badge>(
	this,
	st::settingsInfoPeerBadge,
	&controller->session(),
	Info::Profile::BadgeContentForPeer(controller->session().user()),
	_emojiStatusPanel.get(),
	[=] { return controller->isGifPausedAtLeastFor(GifPauseReason::Layer); },
	kPlayStatusLimit,
	Info::Profile::BadgeType::Premium))
, _scroll(this, st::defaultSolidScroll)
, _inner(_scroll->setOwnedWidget(
	object_ptr<Ui::VerticalLayout>(_scroll.data())))
, _topShadowSkip(_inner->add(
	object_ptr<Ui::FixedHeightWidget>(_inner.get(), st::lineWidth)))
, _accounts(_inner->add(object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
	_inner.get(),
	object_ptr<Ui::VerticalLayout>(_inner.get()))))
, _shadow(_inner->add(object_ptr<Ui::SlideWrap<Ui::PlainShadow>>(
	_inner.get(),
	object_ptr<Ui::PlainShadow>(_inner.get()))))
, _menu(_inner->add(
	object_ptr<Ui::VerticalLayout>(_inner.get()),
	{ 0, st::mainMenuSkip, 0, 0 }))
, _footer(_inner->add(object_ptr<Ui::RpWidget>(_inner.get())))
, _telegram(
	Ui::CreateChild<Ui::FlatLabel>(_footer.get(), st::mainMenuTelegramLabel))
, _version(AddVersionLabel(_footer)) {
	setAttribute(Qt::WA_OpaquePaintEvent);

	setupUserpicButton();
	setupAccountsToggle();
	setupSetEmojiStatus();
	setupAccounts();
	setupArchive();
	setupMenu();

	// See GlobalMainMenuScrollTop's own comment: restores wherever this
	// menu was scrolled to (which folder's accounts were in view, etc.)
	// the last time it was open, since this whole widget - unlike the
	// account list inside it - is torn down and rebuilt from scratch on
	// every open. Read the saved value before the save-on-scroll
	// subscription below starts (it fires once immediately with the
	// current, still-zero, position), then wait for the freshly-built
	// content to actually have a height before scrolling to it - doing it
	// any earlier scrolls a scroll area that doesn't have anything to
	// scroll yet.
	// The content keeps growing for a moment after this (the account list
	// is filled and slides open), so one early scrollToY() got cut short
	// at whatever was there then - and that shorter position was saved
	// over the real one. Keep scrolling back to the saved place as the
	// content and the viewport change, saving nothing meanwhile, until it
	// is reached, the user scrolls by hand, or a moment has passed.
	struct Restore {
		int target = 0;
		bool pending = false;
	};
	const auto restore = lifetime().make_state<Restore>();
	restore->target = GlobalMainMenuScrollTop;
	restore->pending = (restore->target > 0);
	const auto apply = [=] {
		if (!restore->pending) {
			return;
		}
		_scroll->scrollToY(restore->target);
		if (_scroll->scrollTop() == restore->target) {
			restore->pending = false;
		}
	};
	_scroll->scrollTopValue(
	) | rpl::on_next([=](int top) {
		if (!restore->pending) {
			GlobalMainMenuScrollTop = top;
		}
	}, lifetime());
	// Scrolling by hand ends the restore: the user's position wins.
	base::install_event_filter(_scroll->viewport(), [=](not_null<QEvent*> e) {
		const auto type = e->type();
		if (restore->pending
			&& (type == QEvent::Wheel
				|| type == QEvent::MouseButtonPress
				|| type == QEvent::KeyPress
				|| type == QEvent::TouchBegin)) {
			restore->pending = false;
		}
		return base::EventFilterResult::Continue;
	}, lifetime());
	if (restore->pending) {
		rpl::merge(
			_inner->heightValue() | rpl::to_empty,
			_scroll->heightValue() | rpl::to_empty,
			_showFinished.value() | rpl::to_empty
		) | rpl::on_next(apply, lifetime());
		base::call_delayed(1500, this, [=] {
			apply();
			if (restore->pending) {
				restore->pending = false;
				GlobalMainMenuScrollTop = _scroll->scrollTop();
			}
		});
	}

	const auto shadow = Ui::CreateChild<Ui::PlainShadow>(this);
	widthValue(
	) | rpl::on_next([=](int width) {
		const auto line = st::lineWidth;
		shadow->setGeometry(0, st::mainMenuCoverHeight - line, width, line);
	}, shadow->lifetime());

	_nightThemeSwitch.setCallback([this] {
		Expects(_nightThemeToggle != nullptr);

		const auto nightMode = Window::Theme::IsNightMode();
		if (_nightThemeToggle->toggled() != nightMode) {
			Window::Theme::ToggleNightMode();
			Window::Theme::KeepApplied();
		}
	});

	_footer->heightValue(
	) | rpl::on_next([=] {
		_telegram->moveToLeft(st::mainMenuFooterLeft, _footer->height() - st::mainMenuTelegramBottom - _telegram->height());
		_version->moveToLeft(st::mainMenuFooterLeft, _footer->height() - st::mainMenuVersionBottom - _version->height());
	}, _footer->lifetime());

	rpl::combine(
		heightValue(),
		_inner->heightValue()
	) | rpl::on_next([=] {
		updateInnerControlsGeometry();
	}, _inner->lifetime());

	parentResized();

	_telegram->setMarkedText(tr::link(
		u"Telegram Desktop"_q,
		u"https://desktop.telegram.org"_q));
	_telegram->setLinksTrusted();
	// The canary version is too long for the "Version {version}" form.
	_version->setMarkedText(
		tr::link(
			Core::BuildIsCanary
				? currentVersionShortText()
				: tr::lng_settings_current_version(
					tr::now,
					lt_version,
					currentVersionShortText()),
			1) // Link 1.
		.append(QChar(' '))
		.append(QChar(8211))
		.append(QChar(' '))
		.append(tr::link(tr::lng_menu_about(tr::now), 2))); // Link 2.
	_version->setLink(
		1,
		std::make_shared<UrlClickHandler>(Core::App().changelogLink()));
	_version->setLink(
		2,
		std::make_shared<LambdaClickHandler>([=] {
			controller->show(Box(AboutBox));
		}));

	rpl::combine(
		_toggleAccounts->rightSkipValue(),
		rpl::single(rpl::empty) | rpl::then(_badge->updated())
	) | rpl::on_next([=] {
		moveBadge();
	}, lifetime());
	_badge->setPremiumClickCallback([=] {
		chooseEmojiStatus();
	});

	_controller->session().downloaderTaskFinished(
	) | rpl::on_next([=] {
		update();
	}, lifetime());

	initResetScaleButton();

	if (CanCheckSpecialEvent() && CheckSpecialEvent()) {
		const auto snowLifetime = lifetime().make_state<rpl::lifetime>();
		const auto rebuild = [=] {
			const auto snowRaw = Ui::CreateChild<Ui::RpWidget>(this);
			const auto snow = snowLifetime->make_state<Ui::Snowflakes>(
				[=](const QRect &r) { snowRaw->update(r); });
			snow->setBrush(QColor(230, 230, 230));
			_showFinished.value(
			) | rpl::on_next([=](bool shown) {
				snow->setPaused(!shown);
			}, snowRaw->lifetime());
			snowRaw->paintRequest(
			) | rpl::on_next([=](const QRect &r) {
				auto p = Painter(snowRaw);
				p.fillRect(r, st::mainMenuBg);
				drawName(p);
				snow->paint(p, snowRaw->rect());
			}, snowRaw->lifetime());
			widthValue(
			) | rpl::on_next([=](int width) {
				snowRaw->setGeometry(0, 0, width, st::mainMenuCoverHeight);
			}, snowRaw->lifetime());
			snowRaw->show();
			snowRaw->lower();
			snowRaw->setAttribute(Qt::WA_TransparentForMouseEvents);
			snowLifetime->add([=] { base::unique_qptr{ snowRaw }; });
		};
		Window::Theme::IsNightModeValue(
		) | rpl::on_next([=](bool isNightMode) {
			snowLifetime->destroy();
			if (isNightMode) {
				rebuild();
			}
		}, lifetime());
	}

	setupSwipe();
}

MainMenu::~MainMenu() = default;

void MainMenu::moveBadge() {
	if (!_badge->widget()) {
		return;
	}
	const auto available = width()
		- st::mainMenuCoverNameLeft
		- _toggleAccounts->rightSkip()
		- _badge->widget()->width();
	const auto left = st::mainMenuCoverNameLeft
		+ std::min(_name.maxWidth() + st::semiboldFont->spacew, available);
	_badge->move(
		left,
		st::mainMenuCoverNameTop,
		st::mainMenuCoverNameTop + st::semiboldFont->height);
}

void MainMenu::setupArchive() {
	using namespace Settings;

	const auto controller = _controller;
	const auto folder = [=] {
		return controller->session().data().folderLoaded(Data::Folder::kId);
	};
	const auto showArchive = [=](Qt::KeyboardModifiers modifiers) {
		if (const auto f = folder()) {
			if (modifiers & Qt::ControlModifier) {
				controller->showInNewWindow(Window::SeparateId(
					Window::SeparateType::Archive,
					&controller->session()));
			} else {
				controller->openFolder(f);
			}
			controller->window().hideSettingsAndLayer();
			controller->removeLayerBlackout();
		}
	};
	const auto checkArchive = [=] {
		const auto f = folder();
		return f
			&& (!f->chatsList()->empty() || f->storiesCount() > 0)
			&& controller->session().settings().archiveInMainMenu();
	};

	const auto wrap = _menu->add(
		object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			_menu,
			object_ptr<Ui::VerticalLayout>(_menu)));
	const auto inner = wrap->entity();
	wrap->toggle(checkArchive(), anim::type::instant);

	const auto button = AddButtonWithIcon(
		inner,
		tr::lng_archived_name(),
		st::mainMenuButton,
		{ &st::menuIconArchiveOpen });
	inner->add(
		object_ptr<Ui::PlainShadow>(inner),
		{ 0, st::mainMenuSkip, 0, st::mainMenuSkip });
	button->setAcceptBoth(true);
	button->clicks(
	) | rpl::on_next([=](Qt::MouseButton which) {
		if (which == Qt::LeftButton) {
			showArchive(button->clickModifiers());
			return;
		} else if (which != Qt::RightButton) {
			return;
		}
		_contextMenu = base::make_unique_q<Ui::PopupMenu>(
			this,
			st::popupMenuExpandedSeparator);
		Window::FillDialogsEntryMenu(
			_controller,
			Dialogs::EntryState{
				.key = folder(),
				.section = Dialogs::EntryState::Section::ContextMenu,
			},
			Ui::Menu::CreateAddActionCallback(_contextMenu));
		_contextMenu->popup(QCursor::pos());
	}, button->lifetime());

	const auto now = folder();
	auto folderValue = now
		? (rpl::single(now) | rpl::type_erased)
		: controller->session().data().chatsListChanges(
		) | rpl::filter([](Data::Folder *folder) {
			return folder && (folder->id() == Data::Folder::kId);
		}) | rpl::take(1);

	using namespace Settings;
	Badge::AddUnread(button, rpl::single(rpl::empty) | rpl::then(std::move(
		folderValue
	) | rpl::map([=](not_null<Data::Folder*> folder) {
		return folder->owner().chatsList(folder)->unreadStateChanges();
	}) | rpl::flatten_latest() | rpl::to_empty) | rpl::map([=] {
		const auto loaded = folder();
		const auto state = loaded
			? loaded->chatListBadgesState()
			: Dialogs::BadgesState();
		return Badge::UnreadBadge{ state.unreadCounter, true };
	}));

	rpl::merge(
		controller->session().data().chatsListChanges(
		) | rpl::filter([](Data::Folder *folder) {
			return folder && (folder->id() == Data::Folder::kId);
		}) | rpl::to_empty,
		controller->session().data().stories().sourcesChanged(
			Data::StorySourcesList::Hidden
		)
	) | rpl::on_next([=] {
		const auto isArchiveVisible = checkArchive();
		wrap->toggle(isArchiveVisible, anim::type::normal);
		if (!isArchiveVisible) {
			_contextMenu = nullptr;
		}
		update();
	}, lifetime());
}

void MainMenu::setupUserpicButton() {
	_userpicButton->setClickedCallback([=] { toggleAccounts(); });
	_userpicButton->show();
}

void MainMenu::toggleAccounts() {
	auto &settings = Core::App().settings();
	const auto shown = !settings.mainMenuAccountsShown();
	settings.setMainMenuAccountsShown(shown);
	Core::App().saveSettingsDelayed();
}

void MainMenu::setupAccounts() {
	const auto inner = _accounts->entity();

	inner->add(object_ptr<Ui::FixedHeightWidget>(inner, st::mainMenuSkip));
	auto events = Settings::SetupAccounts(inner, _controller);
	inner->add(object_ptr<Ui::FixedHeightWidget>(inner, st::mainMenuSkip));

	std::move(
		events.closeRequests
	) | rpl::on_next([=] {
		closeLayer();
	}, inner->lifetime());

	_accounts->toggleOn(Core::App().settings().mainMenuAccountsShownValue());
	_accounts->finishAnimating();

	_shadow->setDuration(0)->toggleOn(_accounts->shownValue());
}

void MainMenu::setupAccountsToggle() {
	_toggleAccounts->show();
	_toggleAccounts->setAcceptBoth();
	_toggleAccounts->addClickHandler([=](Qt::MouseButton button) {
		if (button == Qt::LeftButton) {
			toggleAccounts();
		}
	});
}

void MainMenu::setupSetEmojiStatus() {
	_setEmojiStatus->overrideLinkClickHandler([=] {
		chooseEmojiStatus();
	});
}

void MainMenu::parentResized() {
	resize(st::mainMenuWidth, parentWidget()->height());
}

void MainMenu::showFinished() {
	_showFinished = true;

	_controller->checkHighlightControl(
		u"main-menu/emoji-status"_q,
		_setEmojiStatus,
		Settings::SubsectionTitleHighlight());
	_controller->checkHighlightControl(
		u"main-menu/night-mode"_q,
		_nightThemeToggle);
}

namespace {

// ---- The "GGTG" window: one resizable floating Ui::SeparatePanel holding
// every custom tool (Массовые действия, Сквидвард, База, Отправка SMS, Прокси,
// Процессы, Менеджер аккаунтов) behind a left sidebar - see GgtgShellHost.
// It is separate from Telegram's own account/chat window system (see
// window/window_separate_id.h and Core::App().ensureSeparateWindowFor) on
// purpose: that system is deeply wired into window management, the Dock,
// dragging chats between windows, etc., while Ui::SeparatePanel is the same
// lightweight floating-window building block already used for Payments,
// Passport and the location picker.
//
// A known limitation: only each section's own *top-level* box is shown
// inside the window. "База", "Сквидвард" and "Менеджер аккаунтов" also keep
// their nested dialogs inside it (they go through their own box's Ui::Show),
// but the others (Массовые действия, Отправка SMS, Прокси) still open
// follow-up dialogs through the main window's `controller->show(...)` -
// GgtgShellHost brings the main window forward while one is up (and this
// window back once it's dismissed) so those never end up hidden behind it.
// Nested dialogs are ordinary fixed-width Telegram dialogs, they don't
// stretch with the window.
//
// A GenericBox normally lives inside a LayerStackWidget/BoxLayerWidget,
// which centers it at a width the box declares once up front (see
// Ui::GenericBox::width()) and never reconsiders that width later -
// exactly right for a modal dialog, and exactly why a boxed section
// doesn't reflow: dragging this panel's edges resizes the *window* just
// fine, the box inside it never finds out. FloatingBoxHost stands in
// for that delegate: instead of centering the box, it keeps the box's
// own Ui::VerticalLayout resized to this host's current width on every
// resize (the same Ui::VerticalLayout::resizeToWidth call any ordinary
// resizable page in this app relies on) and always gives the box's own
// scroll area the rest of the available height, so a short box just
// leaves blank space below instead of floating in the middle of a big
// window. Only the box's own rows go through this - a box it opens on
// top of itself (getDelegate()->showBox(...)) still goes through the
// panel's normal showBox/LayerStackWidget, same as any nested box per
// the limitation above.
class FloatingBoxHost final
	: public Ui::RpWidget
	, public Ui::BoxContentDelegate {
public:
	FloatingBoxHost(
		not_null<QWidget*> parent,
		not_null<Ui::SeparatePanel*> panel)
	: RpWidget(parent.get())
	, _panel(panel) {
	}

	void setContent(object_ptr<Ui::BoxContent> content) {
		_content = std::move(content);
		_content->setParent(this);
		_content->setDelegate(this);
		relayout();
		_content->show();
	}

	[[nodiscard]] Ui::BoxContent *content() const {
		return _content.data();
	}

protected:
	void resizeEvent(QResizeEvent *e) override {
		relayout();
	}

private:
	void relayout() {
		if (!_content || width() <= 0) {
			return;
		}
		// Safe: every section this hosts is built via Box(...), i.e. is
		// always actually a Ui::GenericBox under the object_ptr<BoxContent>.
		static_cast<Ui::GenericBox*>(_content.data())->verticalLayout(
		)->resizeToWidth(width());
		updateContentGeometry();
	}

	void updateContentGeometry() {
		if (!_content) {
			return;
		}
		_content->setGeometry(
			0,
			0,
			width(),
			std::max(height() - buttonsHeight(), 0));
		updateButtonsPositions();
	}

	[[nodiscard]] int buttonsHeight() {
		if (_buttons.empty() && !_leftButton) {
			return 0;
		}
		const auto &st = style();
		return st.buttonPadding.top()
			+ st.buttonHeight
			+ st.buttonPadding.bottom();
	}

	void setLayerType(bool layerType) override {
	}
	void setMinimalTopSkip(int skip) override {
	}
	void setStyle(const style::Box &st) override {
		_st = &st;
		updateContentGeometry();
	}
	const style::Box &style() override {
		return _st ? *_st : st::defaultBox;
	}
	void setTitle(
			rpl::producer<TextWithEntities> title,
			Ui::Text::MarkedContext context) override {
		// The panel's own title bar already shows this section's name.
	}
	void setAdditionalTitle(rpl::producer<QString> additional) override {
	}
	void setCloseByOutsideClick(bool close) override {
	}
	rpl::producer<int> layerHeightMaxValue() override {
		return rpl::single(1'000'000);
	}
	rpl::producer<int> contentHeightMaxValue() override {
		return rpl::single(1'000'000);
	}
	void setCustomCornersFilling(RectParts corners) override {
	}
	void clearButtons() override {
		_buttons.clear();
		_leftButton = { nullptr };
		_topButtons.clear();
		updateContentGeometry();
	}
	void addButton(object_ptr<Ui::AbstractButton> button) override {
		// A section's own "Закрыть" would just close the whole GGTG window,
		// which already has its own close button - not shown here.
		const auto name = button->accessibilityName();
		if (name == tr::lng_close(tr::now) || name == u"Закрыть"_q) {
			// destroy(), not just dropping it: an object_ptr never deletes a
			// widget that already has a parent, and this one was created
			// as a child of the box - it would stay behind, drawn at 0,0.
			button.destroy();
			return;
		}
		button->setParent(this);
		button->show();
		_buttons.push_back(std::move(button));
		updateContentGeometry();
	}
	void addLeftButton(object_ptr<Ui::AbstractButton> button) override {
		button->setParent(this);
		button->show();
		_leftButton = std::move(button);
		updateContentGeometry();
	}
	void addTopButton(object_ptr<Ui::AbstractButton> button) override {
		// A box's own close cross: the GGTG window has its own close.
		if (button->objectName() == QString::fromLatin1(Ggtg::kCloseCrossName)) {
			button.destroy();
			return;
		}
		button->setParent(this);
		button->show();
		_topButtons.push_back(std::move(button));
		updateButtonsPositions();
	}
	void showLoading(bool show) override {
	}
	void updateButtonsPositions() override {
		const auto &st = style();
		const auto top = height() - st.buttonPadding.bottom() - st.buttonHeight;
		if (_leftButton) {
			_leftButton->moveToLeft(st.buttonPadding.right(), top);
		}
		auto right = st.buttonPadding.right();
		for (const auto &button : _buttons) {
			button->moveToRight(right, top);
			right += button->width() + st.buttonPadding.left();
		}
		auto topRight = 0;
		for (const auto &button : _topButtons) {
			button->moveToRight(topRight, 0);
			topRight += button->width();
		}
	}
	void showBox(
			object_ptr<Ui::BoxContent> box,
			Ui::LayerOptions options,
			anim::type animated) override {
		_panel->showBox(std::move(box), options, animated);
	}
	void setDimensions(
			int newWidth,
			int maxHeight,
			bool forceCenterPosition) override {
		updateContentGeometry();
	}
	void setNoContentMargin(bool noContentMargin) override {
	}
	bool isBoxShown() const override {
		return true;
	}
	void closeBox() override {
		// A section is the window's own content, not a dialog on top of
		// it: sections (and the SMS "Отправить" / proxy-list clicks that
		// used to call box->closeBox() before opening a follow-up) must not
		// take the whole GGTG window down with them. The window closes via
		// its own close button only.
	}
	void hideLayer() override {
		_panel->hideLayer(anim::type::normal);
	}
	void triggerButton(int index) override {
		if (index >= 0 && index < int(_buttons.size())) {
			_buttons[index]->clicked({}, Qt::LeftButton);
		}
	}
	Ui::ShowFactory showFactory() override {
		const auto panel = _panel;
		return [=] { return panel->uiShow(); };
	}
	QPointer<QWidget> outerContainer() override {
		return QPointer<QWidget>(_panel.get());
	}

	const not_null<Ui::SeparatePanel*> _panel;
	object_ptr<Ui::BoxContent> _content = { nullptr };
	const style::Box *_st = nullptr;
	std::vector<object_ptr<Ui::AbstractButton>> _buttons;
	object_ptr<Ui::AbstractButton> _leftButton = { nullptr };
	std::vector<object_ptr<Ui::AbstractButton>> _topButtons;

};

[[nodiscard]] QString SidebarCollapsedPath() {
	return cWorkingDir() + u"tdata/ggtg_sidebar_folded"_q;
}

void SaveSidebarCollapsed(bool collapsed) {
	if (collapsed) {
		(void)QFile(SidebarCollapsedPath()).open(QIODevice::WriteOnly);
	} else {
		QFile::remove(SidebarCollapsedPath());
	}
}

// One row of the GGTG window's left sidebar: icon + label, with a
// persistent "this is the open section" highlight - Ui::SettingsButton only
// has transient hover/press states, nothing that stays lit. Colors are the
// chat list's own "currently open chat" pair, the closest existing meaning
// of "the selected row of a list" in the app.
class GgtgSidebarRow final : public Ui::AbstractButton {
public:
	GgtgSidebarRow(
		not_null<QWidget*> parent,
		const style::icon *icon,
		rpl::producer<QString> label)
	: AbstractButton(parent.get())
	, _icon(icon) {
		resize(width(), st::ggtgSidebarRowHeight);
		std::move(label) | rpl::on_next([=](QString text) {
			_busy = text.contains(kBusyMark);
			text.remove(kBusyMark);
			_text = std::move(text);
			// The tooltip is the only place the label survives once the
			// sidebar is collapsed to icons - see setCollapsed below.
			setToolTip(_text);
			update();
		}, lifetime());
	}

	[[nodiscard]] const QString &text() const {
		return _text;
	}
	[[nodiscard]] const style::icon *icon() const {
		return _icon;
	}

	// A colour for the icon (nothing - its usual one): the "ИИ" robot
	// turns green while the engine is running.
	void setTint(std::optional<QColor> tint) {
		if (_tint != tint) {
			_tint = tint;
			update();
		}
	}

	void setActive(bool active) {
		if (_active != active) {
			_active = active;
			update();
		}
	}

	// Collapsed: icon centered, no label drawn (it's still available as
	// this row's tooltip on hover). Toggled from GgtgShellHost's own
	// collapse button, together for every row at once.
	void setCollapsed(bool collapsed) {
		if (_collapsed != collapsed) {
			_collapsed = collapsed;
			update();
		}
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		auto p = QPainter(this);
		// A rounded, inset "pill" for the open/hovered row instead of an
		// edge-to-edge bar - reads as a modern sidebar item, and keeps the
		// highlight from touching the divider line.
		if (_active || isOver()) {
			const auto inset = style::ConvertScale(8);
			const auto pill = rect().marginsRemoved(
				QMargins(inset, style::ConvertScale(2), inset, style::ConvertScale(2)));
			auto hq = PainterHighQualityEnabler(p);
			p.setPen(Qt::NoPen);
			p.setBrush(_active ? st::dialogsBgActive : st::windowBgOver);
			p.drawRoundedRect(pill, style::ConvertScale(10), style::ConvertScale(10));
		}
		const auto iconTop = (height() - _icon->height()) / 2;
		const auto iconLeft = _collapsed
			? (width() - _icon->width()) / 2
			: st::ggtgSidebarIconLeft;
		if (_tint) {
			_icon->paint(p, iconLeft, iconTop, width(), *_tint);
		} else if (_active) {
			_icon->paint(p, iconLeft, iconTop, width(), st::windowFgActive->c);
		} else {
			_icon->paint(p, iconLeft, iconTop, width());
		}
		if (_collapsed) {
			return;
		}
		p.setPen(_active ? st::windowFgActive : st::windowBoldFg);
		p.setFont(st::semiboldFont);
		const auto &clock = _active
			? st::historySendingInvertedIcon
			: st::historySendingIcon;
		const auto clockSkip = _busy ? (clock.width() + style::ConvertScale(4)) : 0;
		const auto available = width()
			- st::ggtgSidebarTextLeft
			- st::ggtgSidebarTextRight
			- clockSkip;
		if (_busy) {
			clock.paint(
				p,
				width() - st::ggtgSidebarTextRight - clock.width()
					+ style::ConvertScale(4),
				(height() - clock.height()) / 2,
				width());
		}
		// Up to two lines (a progress/indicator suffix can make the label
		// long); only a third line would be cut.
		auto lines = Ggtg::WrapText(st::semiboldFont, _text, available);
		if (lines.size() > 2) {
			const auto rest = lines.mid(1).join(' ');
			lines = QStringList{
				lines.front(),
				st::semiboldFont->elided(rest, available),
			};
		}
		auto top = (height() - st::semiboldFont->height * int(lines.size()))
			/ 2;
		for (const auto &line : lines) {
			p.drawText(
				st::ggtgSidebarTextLeft,
				top + st::semiboldFont->ascent,
				line);
			top += st::semiboldFont->height;
		}
	}

private:
	const style::icon *_icon;
	std::optional<QColor> _tint;
	QString _text;
	bool _busy = false;
	bool _active = false;
	bool _collapsed = false;

};

// A page of a section that has several (a strip of tabs under the heading).
struct GgtgTab {
	QString title;
	QString subtitle;
	QString description;
	Fn<object_ptr<Ui::BoxContent>(not_null<Ui::SeparatePanel*>)> makeBox;
};

struct GgtgSection {
	QString key;
	// The sidebar heading this row sits under (rows of one group go together).
	QString group;
	const style::icon *icon = nullptr;
	rpl::producer<QString> label;
	// The heading and the muted explanation shown above the section's
	// content (same as each page of the reference web panel: a title, then a
	// short description of what it does).
	QString title;
	// One line under the heading; the longer explanation sits behind "i".
	QString subtitle;
	QString description;
	// With several pages: a strip of tabs under the heading, each page its
	// own box (then `makeBox` stays empty).
	std::vector<GgtgTab> tabs;
	// Builds the section's box for the content pane. Left empty for a row
	// that does something instead of showing something (Процессы).
	Fn<object_ptr<Ui::BoxContent>(not_null<Ui::SeparatePanel*>)> makeBox;
	// Run instead of showing `makeBox` - always when there is no `makeBox`,
	// and when `preferAction` says so (a running campaign/bulk action: its
	// live progress box is the thing to reopen, not a fresh setup form).
	Fn<void()> action;
	Fn<bool()> preferAction;
};

// The three round window buttons of macOS in the top left corner: close,
// minimize, zoom. Gray while the window is not the active one; the glyphs
// show up when the cursor is over them.
class MacWindowControls final : public Ui::RpWidget {
public:
	MacWindowControls(
		not_null<QWidget*> parent,
		Fn<void()> close,
		Fn<void()> minimize,
		Fn<void()> zoom)
	: RpWidget(parent.get())
	, _actions{ std::move(close), std::move(minimize), std::move(zoom) } {
		setMouseTracking(true);
		events(
		) | rpl::on_next([=](not_null<QEvent*> e) {
			switch (e->type()) {
			case QEvent::WindowActivate:
			case QEvent::WindowDeactivate:
				update();
				break;
			case QEvent::Leave:
				_hover = false;
				update();
				break;
			default:
				break;
			}
		}, lifetime());
	}

	[[nodiscard]] static int Height() {
		return style::ConvertScale(30);
	}
	[[nodiscard]] static int Width() {
		return Margin() * 2 + Diameter() * 3 + Gap() * 2;
	}

protected:
	void mouseMoveEvent(QMouseEvent *e) override {
		const auto over = (indexAt(e->pos()) >= 0);
		if (_hover != over) {
			_hover = over;
			update();
		}
	}
	void mousePressEvent(QMouseEvent *e) override {
		_pressed = (e->button() == Qt::LeftButton)
			? indexAt(e->pos())
			: -1;
		if (_pressed < 0) {
			// Not on a button: the window is dragged by this strip.
			e->ignore();
		}
	}
	void mouseReleaseEvent(QMouseEvent *e) override {
		const auto index = indexAt(e->pos());
		const auto fire = (_pressed >= 0 && _pressed == index);
		_pressed = -1;
		if (fire && _actions[index]) {
			_actions[index]();
		}
	}
	void paintEvent(QPaintEvent *e) override {
		auto p = QPainter(this);
		auto hq = PainterHighQualityEnabler(p);
		const auto active = window()->isActiveWindow();
		const auto colors = std::array<QColor, 3>{
			QColor(255, 95, 87),
			QColor(254, 188, 46),
			QColor(40, 200, 64),
		};
		for (auto i = 0; i != 3; ++i) {
			const auto rect = QRectF(circle(i));
			p.setPen(Qt::NoPen);
			auto fill = active ? colors[i] : QColor(120, 120, 120, 150);
			if (_pressed == i) {
				fill = fill.darker(120);
			}
			p.setBrush(fill);
			p.drawEllipse(rect);
			if (!_hover || !active) {
				continue;
			}
			// The glyphs are drawn, not typed: they stay sharp at any scale.
			auto pen = QPen(QColor(0, 0, 0, 150));
			pen.setWidthF(style::ConvertScaleExact(1.2));
			pen.setCapStyle(Qt::RoundCap);
			p.setPen(pen);
			const auto c = rect.center();
			const auto r = rect.width() / 4.2;
			if (i == 0) {
				p.drawLine(QPointF(c.x() - r, c.y() - r), QPointF(c.x() + r, c.y() + r));
				p.drawLine(QPointF(c.x() - r, c.y() + r), QPointF(c.x() + r, c.y() - r));
			} else if (i == 1) {
				p.drawLine(QPointF(c.x() - r, c.y()), QPointF(c.x() + r, c.y()));
			} else {
				p.drawLine(QPointF(c.x() - r, c.y()), QPointF(c.x() + r, c.y()));
				p.drawLine(QPointF(c.x(), c.y() - r), QPointF(c.x(), c.y() + r));
			}
		}
	}

private:
	[[nodiscard]] static int Margin() { return style::ConvertScale(13); }
	[[nodiscard]] static int Diameter() { return style::ConvertScale(12); }
	[[nodiscard]] static int Gap() { return style::ConvertScale(8); }

	[[nodiscard]] QRect circle(int index) const {
		return QRect(
			Margin() + index * (Diameter() + Gap()),
			(height() - Diameter()) / 2,
			Diameter(),
			Diameter());
	}
	[[nodiscard]] int indexAt(QPoint point) const {
		// A little slack around each circle, like the system's own.
		const auto slack = style::ConvertScale(3);
		for (auto i = 0; i != 3; ++i) {
			if (circle(i).adjusted(-slack, -slack, slack, slack).contains(point)) {
				return i;
			}
		}
		return -1;
	}

	std::array<Fn<void()>, 3> _actions;
	bool _hover = false;
	int _pressed = -1;

};

// The whole content of the GGTG window: a fixed-width sidebar of section
// rows on the left, and on the right one FloatingBoxHost showing whichever
// section is open. Switching sections destroys the previous section's box
// outright and builds the new one fresh (nothing kept alive invisibly in a
// hidden pane) - the same "always start fresh" every section had when it
// was opened as its own box.
// The corners of the window, rounded like the system's own: what the
// panel's smaller round corner and the squares of the content leave outside
// the arc is cut out (the window is translucent), on top of everything.
class MacCornerCut final : public Ui::RpWidget {
public:
	explicit MacCornerCut(not_null<QWidget*> parent)
	: RpWidget(parent.get()) {
		setAttribute(Qt::WA_TransparentForMouseEvents);
	}

	[[nodiscard]] static int Radius() {
		return style::ConvertScale(11);
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		auto p = QPainter(this);
		auto hq = PainterHighQualityEnabler(p);
		p.setCompositionMode(QPainter::CompositionMode_Clear);
		const auto r = Radius();
		const auto w = width();
		const auto h = height();
		const auto cut = [&](QRectF square, QRectF circle) {
			auto path = QPainterPath();
			path.addRect(square);
			auto round = QPainterPath();
			round.addEllipse(circle);
			p.fillPath(path.subtracted(round), Qt::transparent);
		};
		const auto d = 2. * r;
		cut(QRectF(0, 0, r, r), QRectF(0, 0, d, d));
		cut(QRectF(w - r, 0, r, r), QRectF(w - d, 0, d, d));
		cut(QRectF(0, h - r, r, r), QRectF(0, h - d, d, d));
		cut(QRectF(w - r, h - r, r, r), QRectF(w - d, h - d, d, d));
	}

};

[[nodiscard]] base::flat_map<QString, int> &LastGgtgTabs() {
	static auto result = base::flat_map<QString, int>();
	return result;
}

class GroupHeaderButton final : public Ui::AbstractButton {
public:
	GroupHeaderButton(
		not_null<QWidget*> parent,
		const QString &name,
		Fn<void()> onClick)
	: AbstractButton(parent.get())
	, _name(name) {
		setPointerCursor(true);
		setClickedCallback(std::move(onClick));
	}

	void setCollapsed(bool collapsed) {
		if (_collapsed != collapsed) {
			_collapsed = collapsed;
			update();
		}
	}

	[[nodiscard]] bool collapsed() const {
		return _collapsed;
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		auto p = QPainter(this);
		if (isOver()) {
			auto tint = st::windowBgOver->c;
			tint.setAlphaF(0.4);
			p.fillRect(rect(), tint);
		}
		p.setFont(st::semiboldFont);
		p.setPen(isOver() ? st::windowActiveTextFg : st::windowSubTextFg);
		const auto icon = _collapsed ? u"▸ "_q : u"▾ "_q;
		const auto text = icon + _name.toUpper();
		p.drawText(
			QRect(
				st::ggtgSidebarToggleLeft,
				0,
				width() - st::ggtgSidebarToggleLeft,
				height()),
			Qt::AlignLeft | Qt::AlignVCenter,
			text);
	}

private:
	QString _name;
	bool _collapsed = false;
};

class GgtgShellHost final : public Ui::RpWidget {
public:
	GgtgShellHost(
			not_null<QWidget*> parent,
			not_null<Ui::SeparatePanel*> panel,
			not_null<Window::SessionController*> controller,
			std::vector<GgtgSection> sections)
	: RpWidget(parent.get())
	, _panel(panel) {
#ifdef Q_OS_MAC
		// macOS look: the window buttons are the round ones in the top left
		// corner, and everything else starts below their strip.
		_topInset = MacWindowControls::Height();
		_macControls = Ui::CreateChild<MacWindowControls>(
			this,
			[=] { _panel->hideGetDuration(); },
			[=] { _panel->showMinimized(); },
			[=] { toggleZoom(); });
		_macControls->resize(MacWindowControls::Width(), _topInset);
		_macControls->move(0, 0);
		_macControls->show();
		_macCut = Ui::CreateChild<MacCornerCut>(this);
		_macCut->show();
#endif // Q_OS_MAC
		_sidebarScroll = Ui::CreateChild<Ui::ScrollArea>(this);
		_sidebarContent = _sidebarScroll->setOwnedWidget(object_ptr<Ui::RpWidget>(_sidebarScroll));
		_entries.reserve(sections.size());
		for (auto &section : sections) {
			const auto row = Ui::CreateChild<GgtgSidebarRow>(
				_sidebarContent,
				section.icon,
				std::move(section.label));
			row->show();
			const auto key = section.key;
			row->setClickedCallback([=] { showSection(key); });
			if (key == u"ai"_q) {
				// Green while a key can take a job (amber while every key
				// rests after a limit): no need to open the section.
				// The night training runs by itself (when switched on).
				Settings::AiInbox::StartBackground();
				const auto timer = Ui::CreateChild<QTimer>(row);
				const auto refresh = [=] {
					using Level = Core::GgtgAi::EngineLevel;
					switch (Core::GgtgAi::EngineLevelNow()) {
					case Level::Ready:
						row->setTint(QColor(0x3f, 0xc2, 0x5c));
						break;
					case Level::Starting:
						row->setTint(QColor(0xe8, 0xa8, 0x2c));
						break;
					default:
						row->setTint(std::nullopt);
						break;
					}
				};
				QObject::connect(timer, &QTimer::timeout, row, refresh);
				timer->start(1000);
				refresh();
			}
			_entries.push_back({
				.key = key,
				.group = section.group,
				.tabs = std::move(section.tabs),
				.title = section.title,
				.subtitle = section.subtitle,
				.description = section.description,
				.row = row,
				.makeBox = std::move(section.makeBox),
				.action = std::move(section.action),
				.preferAction = std::move(section.preferAction),
			});
		}
		for (const auto &entry : _entries) {
			if (entry.group.isEmpty()
				|| ranges::contains(_groupLabels, entry.group, &Group::name)) {
				continue;
			}
			const auto groupName = entry.group;
			const auto header = Ui::CreateChild<GroupHeaderButton>(
				_sidebarContent,
				groupName,
				[=] {
					if (_collapsedGroups.contains(groupName)) {
						_collapsedGroups.remove(groupName);
					} else {
						_collapsedGroups.emplace(groupName);
					}
					layoutAll();
				});
			header->show();
			_groupLabels.push_back({ groupName, header });
		}
		_collapsed = QFile::exists(SidebarCollapsedPath());
		_title = Ui::CreateChild<Ui::FlatLabel>(
			this,
			QString(),
			st::boxTitle);
		// The heading is part of the strip the window is dragged by: a label
		// takes the press for itself, so the title would be a dead spot.
		_title->setAttribute(Qt::WA_TransparentForMouseEvents);
		_subtitle = Ui::CreateChild<Ui::FlatLabel>(
			this,
			QString(),
			st::boxDividerLabel);
		_subtitle->setAttribute(Qt::WA_TransparentForMouseEvents);
		// The section's explanation lives behind an "i" next to its
		// title instead of taking a paragraph of the header.
		_info = Ui::CreateChild<Ui::IconButton>(this, st::ggtgInfoButton);
		_info->setToolTip(u"Подсказка"_q);
		_info->setClickedCallback([=] {
			const auto i = ranges::find(_entries, _activeKey, &Entry::key);
			if (i == _entries.end() || i->description.isEmpty()) {
				return;
			}
			_panel->uiShow()->showBox(Ui::MakeInformBox(Ui::ConfirmBoxArgs{
				.text = i->description,
				.title = rpl::single(i->title),
			}));
		});
		_title->show();
		_menu = Ui::CreateChild<Ui::IconButton>(this, st::dialogsMenuToggle);
		_menu->setToolTip(u"Разделы"_q);
		_menu->setClickedCallback([=] { showSectionsMenu(); });
		_menu->hide();

		// Collapses the sidebar down to just the section icons, for a
		// narrow window (or anyone who prefers it that way) - same idea as
		// the mobile/web client's own collapsible chat list sidebar.
		_toggle = Ui::CreateChild<Ui::IconButton>(this, st::dialogsMenuToggle);
		_toggle->setToolTip(u"Свернуть/развернуть меню"_q);
		_toggle->setClickedCallback([=] {
			// Too narrow to unfold the sidebar: the button opens the list
			// of sections instead, so it can always be opened.
			if (mode() != Mode::Wide) {
				showSectionsMenu(_toggle);
				return;
			}
			_collapsed = !_collapsed;
			SaveSidebarCollapsed(_collapsed);
			for (const auto &entry : _entries) {
				entry.row->setCollapsed(_collapsed);
			}
			layoutAll();
		});
		_toggle->show();
		// The window has no title bar of its own (see OpenGgtgShell): the
		// name sits next to the fold button and folds away with the menu.
		_brand = Ui::CreateChild<Ui::FlatLabel>(
			this,
			u"GGTG"_q,
			st::boxTitle);
		_brand->setAttribute(Qt::WA_TransparentForMouseEvents);
	}

	[[nodiscard]] QString activeKey() const {
		return _activeKey;
	}

	// Where the open section is scrolled to: kept over the rebuild that a
	// switch of the account does, so the page does not jump to the top.
	[[nodiscard]] int scrollTop() const {
		const auto box = _content ? _content->content() : nullptr;
		return box ? box->scrollTop() : 0;
	}
	void restoreScroll(int top) {
		if (top <= 0) {
			return;
		}
		// The page fills itself in a moment: try again as it grows.
		for (const auto delay : { 0, 120, 400 }) {
			base::call_delayed(delay, this, [=] {
				if (const auto box = _content ? _content->content() : nullptr) {
					box->scrollToY(top);
				}
			});
		}
	}

	// Opens `key` if it is a section with its own page, else the first one.
	void showSectionOrFirst(const QString &key) {
		const auto i = ranges::find(_entries, key, &Entry::key);
		if (i != _entries.end()
			&& (i->makeBox || !i->tabs.empty())
			&& !(i->preferAction && i->preferAction())) {
			showSection(key);
		} else {
			showFirstSection();
		}
	}

	// Opens a section from inside another one (a home-screen row).
	void goTo(const QString &key) {
		showSection(key);
	}

	void showFirstSection() {
		for (const auto &entry : _entries) {
			if (entry.makeBox || !entry.tabs.empty()) {
				showSection(entry.key);
				return;
			}
		}
	}

protected:
	void resizeEvent(QResizeEvent *e) override {
		layoutAll();
	}

	void toggleZoom() {
		if (_panel->isMaximized()) {
			_panel->showNormal();
		} else {
			_panel->showMaximized();
		}
	}

	// A double click on the title strip zooms the window, like on a Mac.
	void mouseDoubleClickEvent(QMouseEvent *e) override {
		if (_topInset > 0
			&& e->button() == Qt::LeftButton
			&& e->pos().y() < _topInset) {
			toggleZoom();
		}
	}

	// With no title bar the window is moved by its header strip: the
	// section heading row and the sidebar's top (fold button, name).
	void mousePressEvent(QMouseEvent *e) override {
		if (e->button() != Qt::LeftButton) {
			return;
		}
		const auto sidebarTop = _topInset
			+ st::ggtgSidebarTopSkip * 2
			+ _toggle->height();
		// One band across the whole top: the strip with the buttons, the
		// heading row and the empty space under them, over the sidebar too
		// (not only the part of it next to the fold button).
		const auto inHeader = (e->pos().y() < std::max(
			_headerHeight,
			sidebarTop));
		if (!inHeader) {
			return;
		}
		if (const auto handle = window()->windowHandle()) {
			handle->startSystemMove();
		}
	}

	void paintEvent(QPaintEvent *e) override {
		auto p = QPainter(this);
		const auto sidebar = sidebarWidth();
		if (_topInset > 0) {
			// The title strip: a band the window is taken by, with the
			// name next to the round buttons, like in the original.
			auto hq = PainterHighQualityEnabler(p);
			auto strip = QColor(st::windowBgOver->c);
			strip.setAlphaF(0.55);
			auto path = QPainterPath();
			const auto r = MacCornerCut::Radius();
			path.addRoundedRect(
				QRectF(0, 0, width(), _topInset + 2 * r),
				r,
				r);
			auto clip = QPainterPath();
			clip.addRect(QRectF(0, 0, width(), _topInset));
			p.fillPath(path.intersected(clip), strip);
			p.fillRect(
				0,
				_topInset - st::lineWidth,
				width(),
				st::lineWidth,
				st::shadowFg);
			p.setPen(st::windowSubTextFg);
			p.setFont(st::semiboldFont);
			const auto left = MacWindowControls::Width()
				+ style::ConvertScale(4);
			p.drawText(
				QRect(left, 0, std::max(width() - left, 0), _topInset),
				Qt::AlignVCenter | Qt::AlignLeft,
				u"GGTG"_q);
		}
		if (sidebar > 0) p.fillRect(
			sidebar - st::lineWidth,
			_topInset,
			st::lineWidth,
			std::max(height() - _topInset, 0),
			st::shadowFg);
		if (_content) {
			p.fillRect(
				sidebar,
				_headerHeight - st::lineWidth,
				std::max(width() - sidebar, 0),
				st::lineWidth,
				st::shadowFg);
		}
	}

private:
	struct Group {
		QString name;
		GroupHeaderButton *header = nullptr;
	};
	struct Entry {
		QString key;
		QString group;
		std::vector<GgtgTab> tabs;
		QString title;
		QString subtitle;
		QString description;
		GgtgSidebarRow *row = nullptr;
		Fn<object_ptr<Ui::BoxContent>(not_null<Ui::SeparatePanel*>)> makeBox;
		Fn<void()> action;
		Fn<bool()> preferAction;
	};

	// How much room the window has: Wide keeps the full sidebar (or the
	// icons-only one when the user folded it), Compact always folds it to
	// icons, Narrow drops it altogether for a menu button in the header -
	// the same idea as a site's mobile layout.
	enum class Mode { Wide, Compact, Narrow };
	[[nodiscard]] Mode mode() const {
		if (width() < st::ggtgNarrowWidth) {
			return Mode::Narrow;
		} else if (width() < st::ggtgCompactWidth) {
			return Mode::Compact;
		}
		return Mode::Wide;
	}
	[[nodiscard]] bool foldedToIcons() const {
		return (mode() == Mode::Compact)
			|| (mode() == Mode::Wide && _collapsed);
	}
	[[nodiscard]] int sidebarWidth() const {
		if (mode() == Mode::Narrow) {
			return 0;
		}
		return foldedToIcons()
			? st::ggtgSidebarCollapsedWidth
			: st::ggtgSidebarWidth;
	}

	void showSectionsMenu(Ui::IconButton *anchor = nullptr) {
		if (!anchor) {
			anchor = _menu;
		}
		_sectionsMenu = base::make_unique_q<Ui::PopupMenu>(
			this,
			st::popupMenuWithIcons);
		for (const auto &entry : _entries) {
			if (!entry.row) {
				continue;
			}
			const auto key = entry.key;
			_sectionsMenu->addAction(
				entry.row->text(),
				[=] { showSection(key); },
				entry.row->icon(),
				entry.row->icon());
		}
		_sectionsMenu->popup(
			anchor->mapToGlobal(QPoint(0, anchor->height())));
	}

	[[nodiscard]] QRect contentRect() const {
		const auto sidebar = sidebarWidth();
		return QRect(
			sidebar,
			_headerHeight,
			std::max(width() - sidebar, 0),
			std::max(height() - _headerHeight, 0));
	}

	// The section heading + description above the content pane; returns
	// the height it takes (0 while nothing is shown).
	[[nodiscard]] int layoutHeader() {
		if (!_content) {
			_title->hide();
			_subtitle->hide();
			_info->hide();
			// The sections button stays: with no sidebar it is the only
			// way to open one.
			const auto narrowNow = (mode() == Mode::Narrow);
			_menu->setVisible(narrowNow);
			if (narrowNow) {
				_menu->moveToLeft(
					st::ggtgHeaderPadding / 4,
					_topInset + st::ggtgSidebarTopSkip);
			}
			return 0;
		}
		const auto narrow = (mode() == Mode::Narrow);
		const auto pad = narrow
			? st::ggtgHeaderPadding / 2
			: st::ggtgHeaderPadding;
		auto left = sidebarWidth() + pad;
		// One line with the sidebar's fold button and the GGTG name, and
		// with the window's close button on the right.
		const auto top = _topInset + st::ggtgSidebarTopSkip;
		const auto rowHeight = std::max({
			_title->height(),
			_info->height(),
			_toggle->height(),
			narrow ? _menu->height() : 0 });
		_menu->setVisible(narrow);
		if (narrow) {
			_menu->moveToLeft(
				left - pad / 2,
				top + (rowHeight - _menu->height()) / 2);
			left += _menu->width();
		}
		const auto hasInfo = _hasDescription;
		const auto room = std::max(
			width() - left - st::separatePanelClose.width
				- (hasInfo ? _info->width() : 0),
			0);
		_title->show();
		_title->resizeToWidth(std::min(_title->naturalWidth(), room));
		_title->moveToLeft(left, top + (rowHeight - _title->height()) / 2);
		_info->setVisible(hasInfo);
		if (hasInfo) {
			_info->moveToLeft(
				left + _title->width() + st::ggtgHeaderGap,
				top + (rowHeight - _info->height()) / 2);
		}
		auto bottom = top + rowHeight;
		if (_subtitleText.isEmpty()) {
			_subtitle->hide();
		} else {
			_subtitle->show();
			_subtitle->resizeToWidth(std::max(width() - left - pad, 0));
			_subtitle->moveToLeft(left, bottom);
			bottom += _subtitle->height();
		}
		if (_tabsWidget) {
			const auto skip = st::defaultVerticalListSkip;
			_tabsWidget->resize(
				std::clamp(width() - left - pad, 0, style::ConvertScale(460)),
				_tabsWidget->height());
			_tabsWidget->moveToLeft(left, bottom + skip);
			_tabsWidget->show();
			bottom += _tabsWidget->height() + skip;
		}
		// Only a small skip under the heading: the window's top strip (the
		// buttons) must not be counted a second time here, it made every
		// section start with a wide empty band.
		return bottom + st::ggtgSidebarTopSkip;
	}

	void layoutAll() {
		const auto sidebar = sidebarWidth();
		const auto folded = foldedToIcons();
		const auto showSidebar = (mode() != Mode::Narrow);
		// Folding by hand only makes sense when there is room to unfold.
		_toggle->setVisible(mode() != Mode::Narrow);
		_toggle->moveToLeft(
			st::ggtgSidebarToggleLeft,
			_topInset + st::ggtgSidebarTopSkip);
		_brand->setVisible(mode() == Mode::Wide && !folded && !_topInset);
		_brand->moveToLeft(
			st::ggtgSidebarToggleLeft + _toggle->width() + st::ggtgHeaderGap,
			_topInset
				+ st::ggtgSidebarTopSkip
				+ (_toggle->height() - _brand->height()) / 2);
		auto sidebarTop = _topInset
			+ st::ggtgSidebarTopSkip
			+ _toggle->height()
			+ st::ggtgSidebarTopSkip;
		if (mode() == Mode::Narrow) {
			sidebarTop = _topInset + st::ggtgSidebarTopSkip;
		}

		_sidebarScroll->setVisible(showSidebar);
		if (showSidebar) {
			const auto scrollH = std::max(height() - sidebarTop, 0);
			const auto scrollW = sidebar - st::lineWidth;
			_sidebarScroll->setGeometry(0, sidebarTop, scrollW, scrollH);

			const auto rowHeight = st::ggtgSidebarRowHeight;
			const auto headerHeight = style::ConvertScale(28);
			auto y = 0;
			auto previousGroup = QString();

			for (const auto &entry : _entries) {
				const auto hasGroup = !entry.group.isEmpty();
				const auto groupCollapsed = hasGroup && _collapsedGroups.contains(entry.group);

				if (hasGroup && entry.group != previousGroup) {
					const auto gi = ranges::find(
						_groupLabels,
						entry.group,
						&Group::name);
					if (gi != _groupLabels.end()) {
						if (!folded) {
							gi->header->setCollapsed(groupCollapsed);
							gi->header->setGeometry(0, y, scrollW, headerHeight);
							gi->header->show();
							y += headerHeight;
						} else {
							gi->header->hide();
						}
					}
					previousGroup = entry.group;
				}

				if (groupCollapsed && !folded) {
					entry.row->hide();
				} else {
					entry.row->setCollapsed(folded);
					entry.row->setGeometry(0, y, scrollW, rowHeight);
					entry.row->show();
					y += rowHeight;
				}
			}
			_sidebarContent->resize(scrollW, y);
		}
		_headerHeight = layoutHeader();
		if (_content) {
			_content->setGeometry(contentRect());
		}
		if (_macControls) {
			_macControls->raise();
		}
		if (_macCut) {
			_macCut->setGeometry(0, 0, width(), height());
			_macCut->raise();
		}
		update();
	}

	void showSection(const QString &key) {
		const auto i = ranges::find(_entries, key, &Entry::key);
		if (i == _entries.end()) {
			return;
		}
		if ((!i->makeBox && i->tabs.empty())
			|| (i->preferAction && i->preferAction())) {
			if (i->action) {
				i->action();
			}
			return;
		}
		if (_content && _activeKey == key) {
			return;
		}
		_activeKey = key;
		for (const auto &entry : _entries) {
			entry.row->setActive(entry.key == key);
		}
		_title->setText(i->title);
		_tabsWidget = nullptr;
		if (i->tabs.empty()) {
			applyPage(i->subtitle, i->description, i->makeBox);
			return;
		}
		auto names = QStringList();
		for (const auto &tab : i->tabs) {
			names.push_back(tab.title);
		}
		const auto tab = std::clamp(LastGgtgTabs()[key], 0, int(names.size()) - 1);
		_tabsWidget = base::make_unique_q<Settings::AiTabs>(this, names, tab);
		_tabsWidget->activeChanges(
		) | rpl::on_next([=](int index) {
			LastGgtgTabs()[key] = index;
			openTab(key, index);
		}, _tabsWidget->lifetime());
		openTab(key, tab);
	}

	void openTab(const QString &key, int index) {
		const auto i = ranges::find(_entries, key, &Entry::key);
		if (i == _entries.end() || index >= int(i->tabs.size())) {
			return;
		}
		const auto tab = i->tabs[index];
		applyPage(tab.subtitle, tab.description, tab.makeBox);
	}

	// Swaps the content pane for a freshly built page.
	void applyPage(
			const QString &subtitle,
			const QString &description,
			const Fn<object_ptr<Ui::BoxContent>(
				not_null<Ui::SeparatePanel*>)> &makeBox) {
		_content = nullptr;
		_content = base::make_unique_q<FloatingBoxHost>(this, _panel);
		_subtitle->setText(subtitle);
		_subtitleText = subtitle;
		_description = description;
		_hasDescription = !description.isEmpty();
		layoutAll();
		// setVisible, not show(): FloatingBoxHost inherits both QWidget's
		// show() and BoxContentDelegate's show<>() template, so a bare
		// show() is ambiguous.
		_content->setVisible(true);
		_content->setContent(makeBox(_panel));
	}

	const not_null<Ui::SeparatePanel*> _panel;
	Ui::ScrollArea *_sidebarScroll = nullptr;
	QPointer<Ui::RpWidget> _sidebarContent;
	base::flat_set<QString> _collapsedGroups;
	std::vector<Entry> _entries;
	base::unique_qptr<FloatingBoxHost> _content;
	Ui::FlatLabel *_title = nullptr;
	Ui::FlatLabel *_subtitle = nullptr;
	base::unique_qptr<Settings::AiTabs> _tabsWidget;
	std::vector<Group> _groupLabels;
	QString _subtitleText;
	QString _description;
	Ui::IconButton *_info = nullptr;
	Ui::IconButton *_menu = nullptr;
	base::unique_qptr<Ui::PopupMenu> _sectionsMenu;
	Ui::IconButton *_toggle = nullptr;
	Ui::FlatLabel *_brand = nullptr;
	bool _hasDescription = false;
	bool _collapsed = false;
	int _headerHeight = 0;
	int _topInset = 0; // the strip of the window buttons (macOS)
	Ui::RpWidget *_macControls = nullptr;
	Ui::RpWidget *_macCut = nullptr;
	QString _activeKey;

};

[[nodiscard]] QPointer<Ui::SeparatePanel> &GgtgPanel() {
	static auto result = QPointer<Ui::SeparatePanel>();
	return result;
}

[[nodiscard]] QPointer<GgtgShellHost> &GgtgShell() {
	static auto result = QPointer<GgtgShellHost>();
	return result;
}

void FillGgtgShell(
	not_null<Ui::SeparatePanel*> panel,
	not_null<Window::SessionController*> controller,
	const QString &section,
	bool activate = true);

// The GGTG window's last outer geometry (size and position), kept in a small
// file next to the bases so it comes back the way it was left.
[[nodiscard]] QString GgtgWindowGeometryPath() {
	return cWorkingDir() + u"tdata/ggtg_window.json"_q;
}

void SaveGgtgWindowGeometry(const QRect &rect) {
	if (rect.isEmpty()) {
		return;
	}
	auto file = QSaveFile(GgtgWindowGeometryPath());
	if (file.open(QIODevice::WriteOnly)) {
		file.write(QJsonDocument(QJsonObject{
			{ "x", rect.x() },
			{ "y", rect.y() },
			{ "w", rect.width() },
			{ "h", rect.height() },
		}).toJson(QJsonDocument::Compact));
		file.commit();
	}
}

// The saved geometry, or an empty rect when there is none. A window that
// would now be off every screen (a monitor was unplugged) keeps its size
// but is centered again instead of being restored somewhere unreachable.
[[nodiscard]] QRect LoadGgtgWindowGeometry(QSize minimum) {
	auto file = QFile(GgtgWindowGeometryPath());
	if (!file.open(QIODevice::ReadOnly)) {
		return QRect();
	}
	const auto object = QJsonDocument::fromJson(file.readAll()).object();
	auto rect = QRect(
		object["x"].toInt(),
		object["y"].toInt(),
		object["w"].toInt(),
		object["h"].toInt());
	if (rect.width() < minimum.width() || rect.height() < minimum.height()) {
		return QRect();
	}
	for (const auto screen : QGuiApplication::screens()) {
		const auto visible = screen->availableGeometry().intersected(rect);
		if (visible.width() >= 100 && visible.height() >= 50) {
			return rect;
		}
	}
	return QRect(QPoint(), rect.size());
}

// Opens the one GGTG window (or brings it forward if it's already open).
void OpenGgtgShell(not_null<Window::SessionController*> controller) {
	if (const auto strong = GgtgPanel().data()) {
		// Folded into the Dock with the yellow button: bring it back
		// (showAndActivate alone leaves a minimized window minimized).
		if (strong->isMinimized()) {
			strong->showNormal();
		} else if (strong->isHidden()) {
			strong->show();
		}
		strong->showAndActivate();
		return;
	}
	// Not while "Отправка SMS" runs: it reads (and prunes) those bases.
	if (Core::BlockedReason(Core::GuardedAction::EditBases).isEmpty()) {
		Data::RemoveDuplicatesAcrossBases();
	}
	const auto panel = new Ui::SeparatePanel();
	GgtgPanel() = QPointer<Ui::SeparatePanel>(panel);
	// SeparatePanel defaults to always-on-top (it's normally used for
	// modal-ish flows like Payments/Passport that sit over the main
	// window) - this is meant to behave like an ordinary window the user
	// can send behind other apps, so that's turned back off here, the same
	// way Payments::Panel does for its own non-modal case.
	panel->setWindowFlag(Qt::WindowStaysOnTopHint, false);
	// No title bar: the content starts at the very top (the name moves
	// into the sidebar, the close button stays at the top right, and the
	// window is dragged by its header - see GgtgShellHost).
	panel->setTitleHeight(0);
#ifdef Q_OS_MAC
	// The window buttons are drawn by the GGTG shell itself, top left.
	panel->setCloseAllowed(false);
#endif // Q_OS_MAC
	panel->setWindowTitle(u"GGTG"_q);
	panel->setInnerSize(
		QSize(st::ggtgWindowWidth, st::ggtgWindowHeight),
		true);
	// setInnerSize pins the minimum to the initial size (so it could only
	// ever grow); let it shrink down to something still usable instead.
	const auto padding = panel->computePadding();
	panel->setMinimumSize(QSize(
		st::ggtgWindowMinWidth + padding.left() + padding.right(),
		st::ggtgWindowMinHeight + padding.top() + padding.bottom()));
	// Back to the size and place it had when it was last closed.
	if (const auto saved = LoadGgtgWindowGeometry(panel->minimumSize());
		!saved.isNull()) {
		if (saved.topLeft().isNull()) {
			// Off-screen position dropped: just the size, centered as before.
			auto rect = QRect(QPoint(), saved.size());
			rect.moveCenter(panel->geometry().center());
			panel->setGeometry(rect);
		} else {
			panel->setGeometry(saved);
		}
	}
	// closeRequests() only fires on a click, it doesn't hide anything by
	// itself - hideGetDuration() runs the real hide animation, which fires
	// closeEvents() below once it's actually done.
	panel->closeRequests(
	) | rpl::on_next([=] {
		panel->hideGetDuration();
	}, panel->lifetime());
	panel->closeEvents(
	) | rpl::on_next([=] {
		SaveGgtgWindowGeometry(panel->geometry());
		// Not a direct delete: this fires from inside the panel's own
		// close handling, so destroying it right here would be tearing the
		// object down mid-call into itself.
		Ui::PostponeCall(panel, [=] {
			SetMainWindowBoxRedirect(nullptr);
			delete panel;
		});
	}, panel->lifetime());
	// Every section (Массовые действия, Отправка SMS, Прокси...) opens its
	// follow-up dialogs through the main window (`controller->show(...)`,
	// `Ui::show(...)`), which would leave them hidden behind this window.
	// While the main window isn't the active one, send them here instead -
	// see SetMainWindowBoxRedirect.
	// A native dialog (file picker...) closing hands activation back to the
	// main window while the user is still in this one, so "main is active"
	// alone can't be trusted: remember which window the user last actually
	// clicked or typed in.
	const auto userInMain = panel->lifetime().make_state<bool>(false);
	// A dialog (a running process's window, "Отправка SMS"...) covers the
	// header the window is dragged by, so its dimmed background - the
	// layer stack itself or the backdrop widget it keeps - drags the window
	// instead; the dialog's own area stays clickable as usual.
	const auto onLayerBackground = [](QWidget *widget) {
		if (!widget) {
			return false;
		}
		return dynamic_cast<Ui::LayerStackWidget*>(widget)
			|| (dynamic_cast<Ui::LayerStackWidget*>(widget->parentWidget())
				&& !dynamic_cast<Ui::LayerWidget*>(widget));
	};
	// A section's own empty space - the gaps between its rows and its hint
	// texts - moves the window too: a long section (like "ИИ", mostly texts
	// and gaps) would otherwise offer almost nothing to grab but the thin
	// header. Rows, buttons, fields and selectable texts keep their clicks.
	const auto onPassiveSpace = [](QWidget *widget) {
		if (!widget) {
			return false;
		}
		if (dynamic_cast<Ui::VerticalLayout*>(widget)
			|| dynamic_cast<Ui::FixedHeightWidget*>(widget)
			|| dynamic_cast<Ui::DividerLabel*>(widget)) {
			return true;
		}
		return dynamic_cast<Ui::FlatLabel*>(widget)
			&& dynamic_cast<Ui::DividerLabel*>(widget->parentWidget());
	};
	base::install_event_filter(qApp, [=](not_null<QEvent*> e) {
		const auto type = e->type();
		if (type == QEvent::MouseButtonPress
			&& static_cast<QMouseEvent*>(e.get())->button()
				== Qt::LeftButton) {
			const auto under = QApplication::widgetAt(QCursor::pos());
			if (under
				&& under->window() == panel
				&& (onLayerBackground(under) || onPassiveSpace(under))) {
				if (const auto handle = panel->windowHandle()) {
					handle->startSystemMove();
				}
			}
		}
		if (type == QEvent::MouseButtonPress || type == QEvent::KeyPress) {
			// A click goes by the window under the mouse: on the press that
			// brings a window forward the focus is still on the other one,
			// so the first click on «Массовое действие...» right after
			// working in the main window used to send its box there.
			const auto under = (type == QEvent::MouseButtonPress)
				? QApplication::widgetAt(QCursor::pos())
				: nullptr;
			const auto focus = QGuiApplication::focusWindow();
			const auto found = under
				? under
				: focus
				? QWidget::find(focus->winId())
				: nullptr;
			const auto top = found ? found->window() : nullptr;
			if (top == panel->window()) {
				*userInMain = false;
			} else if (dynamic_cast<::MainWindow*>(top)) {
				*userInMain = true;
			}
		}
		return base::EventFilterResult::Continue;
	}, panel->lifetime());
	SetMainWindowBoxRedirect([
		weak = QPointer<Ui::SeparatePanel>(panel),
		userInMain
	](bool mainWindowActive) -> std::shared_ptr<Ui::Show> {
		const auto strong = weak.data();
		if (!strong || !strong->isVisible()) {
			return nullptr;
		}
		return (!mainWindowActive || !*userInMain)
			? strong->uiShow()
			: nullptr;
	});

	FillGgtgShell(panel, controller, QString());

	// Switching accounts (the manager's «Открыть», the main menu's own
	// account list) replaces the main window's SessionController, which
	// every section here was built around: rebuild them for the new one,
	// staying on the same section, instead of closing the window. With no
	// account left there's nothing to show.
	controller->window().sessionControllerChanges(
	) | rpl::on_next([=](Window::SessionController *now) {
		const auto shell = GgtgShell().data();
		const auto section = shell ? shell->activeKey() : QString();
		const auto scroll = shell ? shell->scrollTop() : 0;
		// The windows of running processes don't depend on the old
		// account's controller: they are put back below instead of being
		// lost along with the rest.
		const auto processBoxes = Ggtg::ProcessBoxesIn(panel);
		// A dialog left open over the old sections belongs to the old
		// account - except the open list of a base, which the user goes on
		// with (it does not depend on the account).
		if (!Ggtg::HasKeptBoxIn(panel)) {
			panel->hideLayer(anim::type::instant);
		}
		if (now) {
			// Not raised: the account was most likely switched from the
			// main window, and the GGTG window must not jump over the chat
			// the user is working in.
			FillGgtgShell(panel, now, section, false);
			if (const auto rebuilt = GgtgShell().data()) {
				rebuilt->restoreScroll(scroll);
			}
			if (!processBoxes.empty()) {
				Ui::PostponeCall(panel, [=] {
					for (const auto &reopen : processBoxes) {
						reopen(panel->uiShow());
					}
				});
			}
		} else {
			panel->showInner(base::make_unique_q<Ui::RpWidget>(panel));
			panel->hideGetDuration();
		}
	}, panel->lifetime());
	panel->showAndActivate();
}

// Builds the window's sections around `controller` and shows `section`
// (or the first one).
// "Главная": what is going on right now, a one-line summary of each part of
// the program, and (until everything is set up) the four first steps.
[[nodiscard]] object_ptr<Ui::BoxContent> MakeGgtgHomeBox(
		not_null<Window::SessionController*> controller) {
	return Box([=](not_null<Ui::GenericBox*> box) {
		box->setWidth(st::accountsBoxWidth);
		box->setTitle(rpl::single(u"Главная"_q));
		const auto go = [](const QString &key) {
			if (const auto shell = GgtgShell().data()) {
				shell->goTo(key);
			}
		};
		// The page, top to bottom: a greeting with the one thing to do,
		// the numbers, what is running, the answers of people, the first
		// steps (until everything is set up).
		const auto top = box->addRow(
			object_ptr<Ui::VerticalLayout>(box),
			style::margins());
		Ui::AddSkip(box->verticalLayout());
		const auto bottom = box->addRow(
			object_ptr<Ui::VerticalLayout>(box),
			style::margins());
		struct Cache {
			QString lastRun;
			crl::time at = 0;
		};
		const auto cache = box->lifetime().make_state<Cache>();
		const auto rebuild = [=] {
			const Ggtg::ScrollKeeper keepScroll(top);
			top->clear();
			bottom->clear();
			const auto row = [&](
					const QString &title,
					const QString &subtitle,
					const QString &chip,
					bool good,
					const QString &key) {
				bottom->add(
					object_ptr<Ggtg::InfoRow>(
						bottom,
						rpl::single(Ggtg::InfoRowData{
							.title = title,
							.subtitle = subtitle,
							.chip = chip,
							.chipGood = good,
						})),
					st::boxRowPadding
				)->setClickedCallback([=] { go(key); });
			};

			auto accounts = 0;
			auto blocked = 0;
			const auto now = QDateTime::currentSecsSinceEpoch();
			for (const auto &account : Core::App().domain().orderedAccounts()) {
				if (!Main::AccountIsLive(account) || account->trashed()) {
					continue;
				}
				++accounts;
				const auto status = Data::FindAccountSpamStatus(
					account->session().user()->id.value);
				if (status
					&& (status->state == Data::AccountSpamState::Permanent
						|| (status->state == Data::AccountSpamState::Temporary
							&& status->until > now))) {
					++blocked;
				}
			}
			auto userBases = 0;
			auto remaining = 0;
			for (const auto &meta : Data::LoadScrapedGroupsIndex()) {
				if (meta.kind == Data::BaseKind::Users) {
					++userBases;
					remaining += Data::ComputeBaseStats(meta.id).remaining;
				}
			}
			// Reading the attempts and the loaded chats is the slow part;
			// the line about the last run is good for 15 seconds.
			if (cache->at == 0
				|| crl::now() - cache->at > 15000) {
				cache->lastRun = Settings::SmsLastRunSummary();
				cache->at = crl::now();
			}
			const auto lastRun = cache->lastRun;
			const auto proxyLists = int(MTP::GlobalProxyPool().lists().size());
			using Level = Core::GgtgAi::EngineLevel;
			const auto level = Core::GgtgAi::EngineLevelNow();
			auto running = std::vector<Core::ProcessSnapshot>();
			for (const auto &process : Core::App().processes().list()) {
				if (process.status == Core::ProcessStatus::Running) {
					running.push_back(process);
				}
			}

			const auto hour = QTime::currentTime().hour();
			const auto greeting = (hour >= 5 && hour < 12)
				? u"Доброе утро"_q
				: (hour >= 12 && hour < 18)
				? u"Добрый день"_q
				: (hour >= 18 && hour < 23)
				? u"Добрый вечер"_q
				: u"Доброй ночи"_q;
			auto text = QString();
			auto button = QString();
			auto target = Fn<void()>();
			if (!accounts) {
				text = u"Начнём с малого: добавьте первый аккаунт, и "
					"остальное оживёт."_q;
				button = u"Добавить аккаунт"_q;
				target = [=] { go(u"accounts"_q); };
			} else if (level != Level::Ready && level != Level::Starting) {
				text = u"Подключите бесплатный ключ ИИ: без него ответы "
					"людям не готовятся."_q;
				button = u"Открыть ИИ"_q;
				target = [=] { go(u"ai"_q); };
			} else if (!running.empty()) {
				text = u"Сейчас идёт: "_q + running.front().title
					+ (running.size() > 1
						? (u" и ещё "_q + QString::number(running.size() - 1))
						: QString())
					+ u"."_q;
				button = u"Посмотреть ход"_q;
				target = [=] { go(u"processes"_q); };
			} else if (remaining > 0) {
				text = u"Всё спокойно. В базах ждут "_q
					+ QString::number(remaining)
					+ u" человек - можно запускать рассылку."_q;
				button = u"Запустить рассылку"_q;
				target = [=] { go(u"sms"_q); };
			} else {
				text = u"Всё спокойно. Найдите аудиторию в Сквидварде и "
					"сохраните её в базу."_q;
				button = u"Открыть Сквидвард"_q;
				target = [=] { go(u"squidward"_q); };
			}
			Ui::AddSkip(top);
			top->add(
				object_ptr<Ggtg::HeroCard>(
					top,
					greeting,
					text,
					button,
					target),
				st::boxRowPadding);

			// The numbers: each tile goes to its section.
			const auto accent = QColor(st::windowActiveTextFg->c);
			const auto good = QColor(st::boxTextFgGood->c);
			const auto warn = QColor(255, 159, 10);
			Ui::AddSkip(top);
			const auto grid = top->add(
				object_ptr<Ggtg::TileGrid>(top),
				st::boxRowPadding);
			grid->add(
				u"Аккаунты"_q,
				QString::number(accounts),
				blocked
					? (u"спам-блок: "_q + QString::number(blocked))
					: (accounts ? u"подключены"_q : u"пока нет"_q),
				blocked ? warn : accent,
				[=] { go(u"accounts"_q); });
			grid->add(
				u"Уже написали"_q,
				QString::number(
					Data::CountSmsStatus(Data::SmsSendStatus::Sent)),
				u"осталось написать: "_q + QString::number(remaining)
					+ u" · списков: "_q + QString::number(userBases),
				accent,
				[=] { go(u"sms"_q); });
			auto runValue = u"Пока не было"_q;
			auto runSub = QString();
			if (!lastRun.isEmpty()) {
				static const auto lineRe = QRegularExpression(
					u"^(\\d\\d\\.\\d\\d \\d\\d:\\d\\d):\\s*(.*)$"_q);
				const auto match = lineRe.match(lastRun);
				if (match.hasMatch()) {
					runValue = match.captured(1);
					runSub = match.captured(2);
				} else {
					runValue = lastRun.left(24);
				}
			}
			grid->add(
				u"Последняя рассылка"_q,
				runValue,
				runSub,
				accent,
				[=] { go(u"sms"_q); });
			const auto aiReady = (level == Level::Ready);
			grid->add(
				u"ИИ"_q,
				aiReady
					? u"Работает"_q
					: (level == Level::Starting)
					? u"Отдыхает"_q
					: u"Нужен ключ"_q,
				(level == Level::Starting)
					? u"ключи ждут после лимита"_q
					: (aiReady ? u"всё в порядке"_q : u"бесплатный, за минуту"_q),
				aiReady ? good : warn,
				[=] { go(u"ai"_q); });

			// What is running right now.
			Ui::AddSkip(top);
			Ui::AddSubsectionTitle(top, rpl::single(u"Сейчас"_q));
			if (running.empty()) {
				top->add(object_ptr<Ggtg::EmptyState>(
					top,
					u"Сейчас ничего не запущено."_q));
			}
			for (const auto &process : running) {
				const auto card = top->add(
					object_ptr<Ggtg::CardBox>(top),
					st::boxRowPadding);
				card->inner()->add(object_ptr<Ui::FlatLabel>(
					card,
					process.title,
					st::boxLabel));
				card->inner()->add(object_ptr<Ui::FlatLabel>(
					card,
					(process.progressTotal > 0)
						? u"Обработано: %1 из %2"_q
							.arg(process.progressCurrent)
							.arg(process.progressTotal)
						: u"Выполняется..."_q,
					st::boxDividerLabel));
				if (process.progressTotal > 0) {
					card->inner()->add(
						object_ptr<Ggtg::ThinProgress>(
							card,
							double(process.progressCurrent)
								/ double(process.progressTotal)),
						style::margins(0, style::ConvertScale(6), 0, 0));
				}
				card->setClickedCallback([=] { go(u"processes"_q); });
				Ui::AddSkip(top, style::ConvertScale(6));
			}

			const auto steps = std::array<std::tuple<bool, QString, QString, QString>, 4>{{
				{ accounts > 0, u"1. Добавьте аккаунты"_q,
					u"Войдите в аккаунты через меню Telegram"_q,
					u"add_accounts"_q },
				{ proxyLists > 0, u"2. Добавьте прокси (по желанию)"_q,
					u"Списки запасных прокси для аккаунтов"_q,
					u"proxies"_q },
				{ userBases > 0, u"3. Найдите аудиторию"_q,
					u"Пробейте группу в Сквидварде и сохраните людей в базу"_q,
					u"squidward"_q },
				{ !lastRun.isEmpty(), u"4. Запустите рассылку"_q,
					u"Выберите аккаунты, базу и текст"_q,
					u"sms"_q },
			}};
			// The proxy step is optional: it alone never keeps the hints on.
			const auto missing = int(!std::get<0>(steps[0]))
				+ int(!std::get<0>(steps[2]))
				+ int(!std::get<0>(steps[3]));
			if (missing > 0) {
				Ui::AddSubsectionTitle(
					bottom,
					rpl::single(u"Быстрый старт"_q));
				for (const auto &[done, title, hint, key] : steps) {
					row(
						title,
						hint,
						done ? u"готово"_q : QString(),
						done,
						key);
				}
			}
		};
		rebuild();
		// A running mailing reports every line it writes: the screen
		// follows at most every two seconds.
		const auto refresh = box->lifetime().make_state<base::Timer>([=] {
			rebuild();
		});
		rpl::merge(
			Core::App().processes().changes(),
			Data::BasesChanged()
		) | rpl::on_next([=] {
			if (!refresh->isActive()) {
				refresh->callOnce(2000);
			}
		}, box->lifetime());
	});
}

void FillGgtgShell(
		not_null<Ui::SeparatePanel*> panel,
		not_null<Window::SessionController*> controller,
		const QString &section,
		bool activate) {
	// Running / finished markers, alive as long as these sections are.
	struct Indicators {
		Settings::SmsCampaignIndicator sms
			= Settings::SmsCampaignIndicator::None;
		Settings::ViewsIndicator views = Settings::ViewsIndicator::None;
		rpl::lifetime lifetime;
	};
	const auto indicators = std::make_shared<Indicators>();
	const auto raw = indicators.get();
	Settings::SmsCampaignIndicatorValue(
	) | rpl::on_next([=](Settings::SmsCampaignIndicator state) {
		raw->sms = state;
	}, raw->lifetime);
	Settings::ViewsIndicatorValue(
	) | rpl::on_next([=](Settings::ViewsIndicator state) {
		raw->views = state;
	}, raw->lifetime);
	// "База"'s own box family takes a Ui::Show instead of a
	// SessionController (see MakeScrapedGroupsDbBox's comment), so its own
	// nested navigation (editing a base, its clickable-usernames view,
	// etc.) stays fully inside this window.
	const auto makeBaseBox = [=](std::shared_ptr<Ui::Show> show) {
		auto actions = Settings::BasesActions();
		// No separate "Анализ базы": recipients are checked while the
		// campaign sends (not found / long ago / bots are skipped there).
		actions.sendToBase = [=](uint64 baseId) {
			// On top of the base, with "Назад" back to it.
			Settings::OpenSmsCampaign(controller, baseId, show);
		};
		actions.scrapeGroup = [=](const QString &username) {
			Data::ScrapeGroupByUsername(controller, username);
		};
		actions.removeChannels = [=](
				std::shared_ptr<Ui::Show> where,
				uint64 baseId,
				Fn<void()> changed) {
			Data::RemoveChannelsFromGroupsBase(
				controller,
				where,
				baseId,
				std::move(changed));
		};
		actions.scrapeGroupViaSquidward = [=](
				std::shared_ptr<Ui::Show> where,
				const QString &username) {
			where->showBox(
				MakeSquidwardBox(controller, username),
				Ui::LayerOption::KeepOther);
		};
		actions.searchGroups = [=] {
			Data::SearchGroups(controller);
		};
		return Settings::MakeScrapedGroupsDbBox(show, std::move(actions));
	};

	auto sections = std::vector<GgtgSection>();
	sections.push_back({
		.key = u"home"_q,
		.icon = &st::menuIconInfo,
		.label = rpl::single(u"Главная"_q),
		.title = u"Главная"_q,
		.subtitle = u"Что идёт сейчас и с чего начать."_q,
		.description = QString(),
		.makeBox = [=](not_null<Ui::SeparatePanel*>) {
			return MakeGgtgHomeBox(controller);
		},
	});
	sections.push_back({
		.key = u"accounts"_q,
		.group = u"Аккаунты"_q,
		.icon = &st::menuIconUserShow,
		// A running / finished bulk action shows next to the name, as the
		// separate "Массовые действия" row used to.
		.label = Settings::BulkActionIndicatorValue(
		) | rpl::map([](Settings::BulkActionIndicator state) {
			const auto text = u"Список"_q;
			switch (state) {
			case Settings::BulkActionIndicator::Running:
				return text + QString(kBusyMark);
			case Settings::BulkActionIndicator::Done:
				return text + u" ✓"_q;
			default:
				return text;
			}
		}),
		.title = u"Список аккаунтов"_q,
		.subtitle = u"Все аккаунты по папкам: статус, прокси, массовые действия."_q,
		.description = u"Все аккаунты по папкам. «Открыть» - переключиться на аккаунт (окно GGTG остаётся открытым), корзина в конце строки - убрать аккаунт в «Корзину» (оттуда его можно восстановить или удалить навсегда). Клик по имени/@username - изменить профиль, по прокси - изменить его, по папке - сменить папку, по статусу - проверить через @SpamBot, (i) - информация об аккаунте и его сессиях. ПКМ по строке - контекстное меню. Отметьте один, несколько или все аккаунты галочками и нажмите «Массовое действие...» - фото, имя, юзернейм, описание, пароль 2FA, сессии, перенос в папку, проверка SpamBot."_q,
		.makeBox = [=](not_null<Ui::SeparatePanel*>) {
			return Settings::MakeAccountManagerBox(controller);
		},
	});
	sections.push_back({
		.key = u"add_accounts"_q,
		.group = u"Аккаунты"_q,
		.icon = &st::menuIconAddAccount,
		.label = rpl::single(u"Добавить"_q),
		.title = u"Добавить аккаунты"_q,
		.subtitle = u"Новые аккаунты: импорт tdata, вход по номеру, приём с Android."_q,
		.description = u"Сначала добавьте списки прокси: каждому новому аккаунту выдаётся свой прокси из списка, без него аккаунт не подключается. Потом импортируйте папку tdata (одну или папку с несколькими: пароль 2FA берётся из файла рядом, папка аккаунта - из имени папки), войдите по номеру телефона или примите аккаунты с GGTG Android."_q,
		.makeBox = [=](not_null<Ui::SeparatePanel*>) {
			return Settings::MakeAddAccountsBox();
		},
	});
	sections.push_back({
		.key = u"registration"_q,
		.group = u"Аккаунты"_q,
		.icon = &st::menuIconPhone,
		.label = rpl::single(u"Регистрация"_q),
		.title = u"Авторегистрация"_q,
		.subtitle = u"Авторегистрация аккаунтов через SMS-сервисы с прокси."_q,
		.description = u"Автоматическая регистрация новых аккаунтов Telegram через сервисы SMS-активаций (SMS-Activate, 5SIM, Vak-SMS, SMSHub и др.). Задайте API-ключ, выберите сервис, страну и список прокси. Программа сама запросит номер, примет SMS-код, зарегистрирует аккаунт, установит пароль 2FA и сохранит новую сессию."_q,
		.makeBox = [=](not_null<Ui::SeparatePanel*>) {
			return Settings::MakeRegistrationBox(controller);
		},
	});
	sections.push_back({
		.key = u"proxies"_q,
		.group = u"Аккаунты"_q,
		.icon = &st::menuIconNetwork,
		.label = rpl::single(u"Прокси"_q),
		.title = u"Списки прокси"_q,
		.subtitle = u"Списки запасных прокси, которые раздаются аккаунтам."_q,
		.description = u"Добавляйте прокси пачками «хост:порт:логин:пароль», по одному на строку, группируйте их в списки (например, по регионам). Когда аккаунту выдаётся прокси из списка, он убирается из списка, чтобы не выдать один и тот же дважды."_q,
		.makeBox = [=](not_null<Ui::SeparatePanel*>) {
			return MakeProxyPoolManagerBox();
		},
	});
	sections.push_back({
		.key = u"export"_q,
		.group = u"Аккаунты"_q,
		.icon = &st::menuIconExport,
		.label = rpl::single(u"Экспорт"_q),
		.title = u"Экспорт сессий"_q,
		.subtitle = u"Выдать сессии: TData, Сессия + JSON или для GGTG Android."_q,
		.description = u"Экспорт в TData, Сессия + JSON (Telethon/Pyrogram) или .ggtg: основную сессию (с переходом на резервную), резервную (становится дублированной) или новую дублированную. 2FA, параметры устройства и прокси сохраняются. Все выданные сессии хранятся в Session Vault программы."_q,
		.makeBox = [=](not_null<Ui::SeparatePanel*>) {
			return Settings::MakeGgtgExportBox(controller);
		},
	});
	sections.push_back({
		.key = u"squidward"_q,
		.group = u"Аудитория"_q,
		.icon = &st::menuIconSquidward,
		.label = rpl::single(u"Сквидвард"_q),
		.title = u"Сквидвард"_q,
		.subtitle = u"Пробейте человека или группу и сохраните найденное в базу."_q,
		.description = u"Пробивает человека или группу через бота @skvidvarde_bot и показывает аккуратное досье. По кнопке собирает группы человека или участников группы и сохраняет их в «Базу»."_q,
		.makeBox = [=](not_null<Ui::SeparatePanel*>) {
			return MakeSquidwardBox(controller);
		},
	});
	sections.push_back({
		.key = u"bases"_q,
		.group = u"Аудитория"_q,
		.icon = &st::menuIconStorage,
		.label = rpl::single(u"База"_q),
		.title = u"База"_q,
		.subtitle = u"Списки групп и людей: просмотр, импорт, поиск, рассылка."_q,
		.description = u"Сохранённые списки групп и пользователей: просмотр, копирование, импорт, поиск групп по слову и отправка SMS по выбранной базе."_q,
		.makeBox = [=](not_null<Ui::SeparatePanel*> shownIn) {
			return makeBaseBox(shownIn->uiShow());
		},
	});
	sections.push_back({
		.key = u"lists"_q,
		.group = u"Аудитория"_q,
		.icon = &st::menuIconFave,
		.label = rpl::single(u"Списки чатов"_q),
		.title = u"Списки чатов"_q,
		.subtitle = u"Свои списки переписок: заинтересованные, подумать, клиенты."_q,
		.description = u"Свои списки переписок. Чтобы добавить чат, нажмите на него правой кнопкой в списке чатов (или «⋮» в самом чате) → «В список GGTG» (можно сразу создать новый список). Список открывается кликом по нему; в нём клик по чату открывает его в нужном аккаунте, а сам список остаётся открытым, и можно идти дальше по нему. «Написать всем» отправляет сообщение (с фото и подписью) всем чатам списка с паузой."_q,
		.makeBox = [=](not_null<Ui::SeparatePanel*>) {
			return Settings::MakeChatListsBox();
		},
	});
	sections.push_back({
		.key = u"sms"_q,
		.group = u"Продвижение"_q,
		.icon = &st::menuIconDiscussion,
		.label = rpl::combine(
			rpl::single(u"Рассылка"_q),
			Settings::SmsCampaignIndicatorValue(),
			Settings::SmsCampaignProgressValue()
		) | rpl::map([](
				const QString &text,
				Settings::SmsCampaignIndicator state,
				const QString &progress) {
			switch (state) {
			case Settings::SmsCampaignIndicator::Running:
				return text + u" "_q + progress + QString(kBusyMark);
			case Settings::SmsCampaignIndicator::Done:
				return text + u" ✓"_q;
			default:
				return text;
			}
		}),
		.title = u"Рассылка"_q,
		.subtitle = u"Личные сообщения по базам от нескольких аккаунтов."_q,
		.description = u"Отправляет сообщение выбранным получателям от одного или нескольких аккаунтов. У каждого получателя один из исходов: отправлено, отказ Telegram (аккаунт свободен, Telegram не пустил к этому человеку), закрыт для ЛС, не найден, спам-блок аккаунта или сбой. Такие получатели остаются в списке, но автоматически больше не пробуются, пока вы не сбросите статус. Итоги и ответы - во вкладках «Результаты» и «Ответы»."_q,
		.tabs = {
			{
				.title = u"Новая"_q,
				.subtitle = u"Личные сообщения по базам от нескольких аккаунтов."_q,
				.description = u"Отправляет сообщение выбранным получателям от одного или нескольких аккаунтов. У каждого получателя один из исходов: отправлено, отказ Telegram (аккаунт свободен, Telegram не пустил к этому человеку), закрыт для ЛС, не найден, спам-блок аккаунта или сбой. Такие получатели остаются в списке, но автоматически больше не пробуются, пока вы не сбросите статус. Итоги и ответы - во вкладках «Результаты» и «Ответы»."_q,
				.makeBox = [=](not_null<Ui::SeparatePanel*>) {
					return Settings::MakeSmsCampaignSetupBoxFresh(controller);
				},
			},
			{
				.title = u"Результаты"_q,
				.subtitle = u"Сколько отправлено, кто ответил и какие тексты работают лучше."_q,
				.description = QString(),
				.makeBox = [=](not_null<Ui::SeparatePanel*>) {
					return Settings::MakeSmsResultsBox(controller);
				},
			},
			{
				.title = u"Ответы"_q,
				.subtitle = u"Кто ответил на рассылку и что написал."_q,
				.description = QString(),
				.makeBox = [=](not_null<Ui::SeparatePanel*>) {
					return Settings::MakeSmsRepliesBox(controller);
				},
			},
		},
		.action = [=] { Settings::OpenSmsCampaign(controller); },
		.preferAction = [=] {
			return indicators->sms != Settings::SmsCampaignIndicator::None;
		},
	});
	sections.push_back({
		.key = u"views"_q,
		.group = u"Продвижение"_q,
		.icon = &st::menuIconShowInChat,
		.label = rpl::combine(
			rpl::single(u"Накрутка"_q),
			Settings::ViewsIndicatorValue()
		) | rpl::map([](
				const QString &text,
				Settings::ViewsIndicator state) {
			switch (state) {
			case Settings::ViewsIndicator::Running:
				return text + QString(kBusyMark);
			case Settings::ViewsIndicator::Done:
				return text + u" ✓"_q;
			default:
				return text;
			}
		}),
		.title = u"Накрутка"_q,
		.subtitle = u"Просмотры постов и вступление в каналы с нескольких аккаунтов."_q,
		.description = u"Просмотры постов (в том числе диапазон 123-145) и вступление в каналы и группы: выберите аккаунты и ссылки, аккаунты сделают это дружно, по числу потоков. Перед каждым действием - лёгкий прогрев: аккаунт несколько секунд «читает» канал."_q,
		.makeBox = [=](not_null<Ui::SeparatePanel*>) {
			return Settings::MakeViewsSetupBoxFresh(controller);
		},
		.action = [=] { Settings::OpenViews(controller); },
		.preferAction = [=] {
			return indicators->views != Settings::ViewsIndicator::None;
		},
	});
	sections.push_back({
		.key = u"warmup"_q,
		.group = u"Продвижение"_q,
		.icon = &st::menuIconRestartBot,
		.label = Settings::WarmupIndicatorLabel(),
		.title = u"Прогрев"_q,
		.subtitle = u"Повышение траста и прогрев аккаунтов перед рассылкой."_q,
		.description = u"Имитация реального человека: умные пресеты, безопасный каталог каналов, просмотры, реакции, опросы и взаимные P2P диалоги между аккаунтами с набором текста."_q,
		.makeBox = [=](not_null<Ui::SeparatePanel*>) {
			return Settings::MakeWarmupBox(controller);
		},
	});
	sections.push_back({
		.key = u"processes"_q,
		.group = u"Контроль"_q,
		.icon = &st::menuIconHourglass,
		// Every long-running operation (exports, SMS campaigns, bulk
		// actions, proxy replace, ...) lands in this list - see
		// core/process_manager.h. The count of currently running ones
		// rides along in the label.
		.label = Core::App().processes().runningCountValue(
		) | rpl::map([](int running) {
			return running > 0
				? u"Процессы (%1)"_q.arg(running)
				: u"Процессы"_q;
		}),
		.title = u"Процессы"_q,
		.subtitle = u"Все идущие и завершённые массовые операции."_q,
		.description = u"Все массовые операции - идущие и завершённые. Клик по карточке раскрывает её лог; «Открыть» возвращает к живому окну операции (рассылка SMS, массовое действие)."_q,
		.makeBox = [=](not_null<Ui::SeparatePanel*>) {
			return Settings::MakeProcessesBox();
		},
	});
	sections.push_back({
		.key = u"ai"_q,
		.group = u"Контроль"_q,
		.icon = &st::menuIconBot,
		.label = rpl::single(u"ИИ"_q),
		.title = u"ИИ"_q,
		.subtitle = u"Ваш помощник: говорите с ним обычными словами."_q,
		.description = u"Чат с ИИ: подключение, разговор, обучение и настройки в одном месте. Бесплатные облачные модели без карты (Gemini, Groq, OpenRouter, NVIDIA, LLM7)."_q,
		// The whole section is the chat with the AI: talking, connecting a
		// key, teaching and the settings are all there.
		.makeBox = [=](not_null<Ui::SeparatePanel*>) {
			return Box([=](not_null<Ui::GenericBox*> box) {
				Settings::FillAiAgent(box, false);
			});
		},
	});

	sections.push_back({
		.key = u"sync"_q,
		.group = u"Контроль"_q,
		.icon = &st::menuIconRestore,
		.label = rpl::single(u"Синхронизация"_q),
		.title = u"Синхронизация"_q,
		.subtitle = u"Общие данные с GGTG на Android."_q,
		.description = u"Базы, «уже написали», шаблоны и память ИИ сами переходят между Mac и телефоном. Телефон подключается по QR-коду."_q,
		.makeBox = [=](not_null<Ui::SeparatePanel*>) {
			return Settings::MakeGgtgSyncBox(controller);
		},
	});

	const auto shell = new GgtgShellHost(
		panel,
		panel,
		controller,
		std::move(sections));
	panel->showInner(base::unique_qptr<Ui::RpWidget>(shell), activate);
	GgtgShell() = shell;
	if (section.isEmpty()) {
		shell->showFirstSection();
	} else {
		shell->showSectionOrFirst(section);
	}
}

} // namespace

void OpenGgtgSection(
		not_null<SessionController*> controller,
		const QString &key) {
	OpenGgtgShell(controller);
	if (const auto shell = GgtgShell().data()) {
		shell->goTo(key);
	}
}

void DestroyGgtgShell() {
	// The panel has no parent and is deleted only when the user closes it.
	// Left open at quit it outlived Core::Application with its animations
	// still registered, tripping Ui::Animations::Manager's destructor.
	if (const auto panel = GgtgPanel().data()) {
		SetMainWindowBoxRedirect(nullptr);
		delete panel;
	}
}

void MainMenu::setupMenu() {
	using namespace Settings;

	const auto controller = _controller;
	// One entry point for every custom tool - Массовые действия,
	// Сквидвард, База, Отправка SMS, Прокси, Процессы and the account
	// manager all live inside this one window (see GgtgShellHost). Sits
	// right under the cover (userpic + name), above the accounts list.
	if (!_controller->session().supportMode()) {
		_ggtgTop = _inner->insert(
			1,
			object_ptr<Ui::VerticalLayout>(_inner.get()),
			{ 0, st::mainMenuSkip, 0, 0 });
		AddButtonWithIcon(
			_ggtgTop,
			rpl::single(u"GGTG"_q),
			st::mainMenuButton,
			{ &st::menuIconExperimental }
		)->setClickedCallback([=] {
			OpenGgtgShell(controller);
		});
		_ggtgTop->add(
			object_ptr<Ui::PlainShadow>(_ggtgTop),
			{ 0, st::mainMenuSkip, 0, 0 });
		_ggtgTop->heightValue(
		) | rpl::on_next([=] {
			updateInnerControlsGeometry();
		}, _ggtgTop->lifetime());
	}

	const auto addAction = [&](
			rpl::producer<QString> text,
			IconDescriptor &&descriptor) {
		return AddButtonWithIcon(
			_menu,
			std::move(text),
			st::mainMenuButton,
			std::move(descriptor));
	};
	if (!_controller->session().supportMode()) {
		_menu->add(
			CreateButtonWithIcon(
				_menu,
				tr::lng_menu_my_profile(),
				st::mainMenuButton,
				{ &st::menuIconProfile })
		)->setClickedCallback([=] {
			controller->showSection(
				Info::Stories::MakeMyProfile(controller->session().user()));
		});

		_menu->add(
			object_ptr<Ui::PlainShadow>(_menu),
			{ 0, st::mainMenuSkip, 0, st::mainMenuSkip });

		SetupMenuBots(_menu, controller);

		addAction(
			tr::lng_menu_contacts(),
			{ &st::menuIconUserShow }
		)->setClickedCallback([=] {
			controller->show(PrepareContactsBox(controller));
		});

		const auto moreButton = addAction(
			rpl::single(u"More"_q),
			{ &st::menuIconExpand });

		const auto moreWrap = _menu->add(
			object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
				_menu,
				object_ptr<Ui::VerticalLayout>(_menu)));
		const auto moreInner = moreWrap->entity();
		moreWrap->toggle(false, anim::type::instant);
		moreButton->setClickedCallback([=] {
			moreWrap->toggle(!moreWrap->toggled(), anim::type::normal);
		});

		AddMyChannelsBox(AddButtonWithIcon(
			moreInner,
			tr::lng_create_group_title(),
			st::mainMenuButton,
			{ &st::menuIconGroups }
		), controller, true)->addClickHandler([=](Qt::MouseButton which) {
			if (which == Qt::LeftButton) {
				controller->showNewGroup();
			}
		});

		AddMyChannelsBox(AddButtonWithIcon(
			moreInner,
			tr::lng_create_channel_title(),
			st::mainMenuButton,
			{ &st::menuIconChannel }
		), controller, false)->addClickHandler([=](Qt::MouseButton which) {
			if (which == Qt::LeftButton) {
				controller->showNewChannel();
			}
		});

		AddButtonWithIcon(
			moreInner,
			tr::lng_menu_calls(),
			st::mainMenuButton,
			{ &st::menuIconPhone }
		)->setClickedCallback([=] {
			::Calls::ShowCallsBox(controller);
		});

		AddButtonWithIcon(
			moreInner,
			tr::lng_saved_messages(),
			st::mainMenuButton,
			{ &st::menuIconSavedMessages }
		)->setClickedCallback([=] {
			controller->showPeerHistory(controller->session().user());
		});
		const auto session = &controller->session();
		const auto wallet = _menu->add(
			object_ptr<Ui::SlideWrap<Ui::SettingsButton>>(
				_menu,
				CreateButtonWithIcon(
					_menu,
					tr::lng_wallet_menu(),
					st::mainMenuButton,
					{ &st::walletMenuIcon })));
		Ui::NewBadge::AddAfterButtonText(
			wallet->entity(),
			tr::lng_wallet_menu(),
			st::mainMenuButton);
		wallet->toggleOn(session->appConfig().value(
		) | rpl::map([=] {
			return session->appConfig().walletAvailable();
		}) | rpl::distinct_until_changed());
		wallet->finishAnimating();
		wallet->entity()->setClickedCallback([=] {
			controller->window().hideSettingsAndLayer();
			Wallet::ShowWallet(session);
		});
	} else {
		addAction(
			tr::lng_profile_add_contact(),
			{ &st::menuIconProfile }
		)->setClickedCallback([=] {
			controller->showAddContact();
		});
		addAction(
			rpl::single(u"Fix chats order"_q),
			{ &st::menuIconPin }
		)->toggleOn(rpl::single(
			_controller->session().settings().supportFixChatsOrder()
		))->toggledChanges(
		) | rpl::on_next([=](bool fix) {
			_controller->session().settings().setSupportFixChatsOrder(fix);
			_controller->session().saveSettings();
		}, _menu->lifetime());
		addAction(
			rpl::single(u"Reload templates"_q),
			{ &st::menuIconRestore }
		)->setClickedCallback([=] {
			_controller->session().supportTemplates().reload();
		});
	}
	addAction(
		tr::lng_menu_settings(),
		{ &st::menuIconSettings }
	)->setClickedCallback([=] {
		controller->showSettings();
	});

	_nightThemeToggle = addAction(
		tr::lng_menu_night_mode(),
		{ &st::menuIconNightMode }
	)->toggleOn(_nightThemeSwitches.events_starting_with(
		Window::Theme::IsNightMode()
	));
	_nightThemeToggle->toggledChanges(
	) | rpl::filter([=](bool night) {
		return (night != Window::Theme::IsNightMode());
	}) | rpl::on_next([=](bool night) {
		if (Window::Theme::Background()->editingTheme()) {
			_nightThemeSwitches.fire(!night);
			controller->show(Ui::MakeInformBox(
				tr::lng_theme_editor_cant_change_theme()));
			return;
		}
		const auto weak = base::make_weak(this);
		const auto toggle = [=] {
			if (!weak) {
				Window::Theme::ToggleNightMode();
				Window::Theme::KeepApplied();
			} else {
				_nightThemeSwitch.callOnce(st::mainMenu.itemToggle.duration);
			}
		};
		Window::Theme::ToggleNightModeWithConfirmation(
			&_controller->window(),
			toggle);
	}, _nightThemeToggle->lifetime());

	Core::App().settings().systemDarkModeValue(
	) | rpl::on_next([=](std::optional<bool> darkMode) {
		const auto darkModeEnabled
			= Core::App().settings().systemDarkModeEnabled();
		if (darkModeEnabled && darkMode.has_value()) {
			_nightThemeSwitches.fire_copy(*darkMode);
		}
	}, _nightThemeToggle->lifetime());

}

void MainMenu::resizeEvent(QResizeEvent *e) {
	_inner->resizeToWidth(width());
	updateControlsGeometry();
}

void MainMenu::updateControlsGeometry() {
	_userpicButton->moveToLeft(
		st::mainMenuUserpicLeft,
		st::mainMenuUserpicTop);
	if (_resetScaleButton) {
		_resetScaleButton->moveToRight(0, 0);
	}
	_setEmojiStatus->moveToLeft(
		st::mainMenuCoverStatusLeft,
		st::mainMenuCoverStatusTop,
		width());
	_toggleAccounts->setGeometry(
		0,
		st::mainMenuCoverNameTop,
		width(),
		st::mainMenuCoverHeight - st::mainMenuCoverNameTop);
	// Allow cover shadow over the scrolled content.
	const auto top = st::mainMenuCoverHeight - st::lineWidth;
	_scroll->setGeometry(0, top, width(), height() - top);
	updateInnerControlsGeometry();
}

void MainMenu::updateInnerControlsGeometry() {
	const auto contentHeight = (_ggtgTop ? _ggtgTop->height() : 0)
		+ _accounts->height()
		+ _shadow->height()
		+ st::mainMenuSkip
		+ _menu->height();
	const auto available = height() - st::mainMenuCoverHeight - contentHeight;
	const auto footerHeight = std::max(
		available,
		st::mainMenuFooterHeightMin);
	if (_footer->height() != footerHeight) {
		_footer->resize(_footer->width(), footerHeight);
	}
}

void MainMenu::chooseEmojiStatus() {
	if (_controller->showFrozenError()) {
		return;
	} else if (const auto widget = _badge->widget()) {
		setupEmojiStatusDismiss();
		_emojiStatusPanel->show(_controller, widget, _badge->sizeTag());
	} else {
		ShowPremiumPreviewBox(_controller, PremiumFeature::EmojiStatus);
	}
}

void MainMenu::setupEmojiStatusDismiss() {
	if (_emojiStatusDismissSetup) {
		return;
	}
	_emojiStatusDismissSetup = true;

	base::install_event_filter(this, parentWidget(), [=](
			not_null<QEvent*> e) {
		if (e->type() != QEvent::MouseButtonPress
			|| !_emojiStatusPanel->shown()) {
			return base::EventFilterResult::Continue;
		}
		_emojiStatusPanel->hideAnimated();
		return base::EventFilterResult::Cancel;
	});
}

bool MainMenu::eventHook(QEvent *event) {
	const auto type = event->type();
	if (type == QEvent::TouchBegin
		|| type == QEvent::TouchUpdate
		|| type == QEvent::TouchEnd
		|| type == QEvent::TouchCancel) {
		QGuiApplication::sendEvent(_inner, event);
	}
	return RpWidget::eventHook(event);
}

void MainMenu::hideEvent(QHideEvent *e) {
	_emojiStatusPanel->hideFast();
}

void MainMenu::paintEvent(QPaintEvent *e) {
	auto p = Painter(this);
	const auto clip = e->rect();
	const auto cover = QRect(0, 0, width(), st::mainMenuCoverHeight);

	p.fillRect(clip, st::mainMenuBg);
	if (cover.intersects(clip)) {
		drawName(p);
	}
}

void MainMenu::drawName(Painter &p) {
	const auto widthText = width()
		- st::mainMenuCoverNameLeft
		- _toggleAccounts->rightSkip();

	const auto user = _controller->session().user();
	if (_nameVersion < user->nameVersion()) {
		_nameVersion = user->nameVersion();
		_name.setText(
			st::semiboldTextStyle,
			user->name(),
			Ui::NameTextOptions());
		moveBadge();
	}
	p.setFont(st::semiboldFont);
	p.setPen(st::windowBoldFg);
	_name.drawLeftElided(
		p,
		st::mainMenuCoverNameLeft,
		st::mainMenuCoverNameTop,
		(widthText
			- (_badge->widget()
				? (st::semiboldFont->spacew + _badge->widget()->width())
				: 0)),
		width());
}

void MainMenu::initResetScaleButton() {
	_controller->widget()->screenValue(
	) | rpl::map([](not_null<QScreen*> screen) {
		return rpl::single(
			screen->availableGeometry()
		) | rpl::then(
			base::qt_signal_producer(
				screen.get(),
				&QScreen::availableGeometryChanged
			)
		);
	}) | rpl::flatten_latest(
	) | rpl::map([](QRect available) {
		return (available.width() >= st::windowMinWidth)
			&& (available.height() >= st::windowMinHeight);
	}) | rpl::distinct_until_changed(
	) | rpl::on_next([=](bool good) {
		if (good) {
			_resetScaleButton.destroy();
		} else {
			_resetScaleButton.create(this);
			_resetScaleButton->addClickHandler([] {
				cSetConfigScale(style::kScaleDefault);
				Local::writeSettings();
				Core::Restart();
			});
			_resetScaleButton->show();
			updateControlsGeometry();
		}
	}, lifetime());
}

OthersUnreadState OtherAccountsUnreadStateCurrent(
		not_null<Main::Account*> current) {
	const auto &domain = Core::App().domain();
	auto counter = 0;
	auto allMuted = true;
	for (const auto &[index, account] : domain.accounts()) {
		if (account.get() == current || account->trashed()) {
			continue;
		} else if (const auto session = account->maybeSession()) {
			counter += session->data().unreadWithMentionsBadge();
			if (!session->data().unreadWithMentionsBadgeMuted()) {
				allMuted = false;
			}
		}
	}
	return {
		.count = counter,
		.allMuted = allMuted,
	};
}

rpl::producer<OthersUnreadState> OtherAccountsUnreadState(
		not_null<Main::Account*> current) {
	return rpl::single(rpl::empty) | rpl::then(
		Core::App().unreadBadgeChanges()
	) | rpl::map([=] {
		return OtherAccountsUnreadStateCurrent(current);
	});
}

base::EventFilterResult MainMenu::redirectToInnerChecked(not_null<QEvent*> e) {
	if (_insideEventRedirect) {
		return base::EventFilterResult::Continue;
	}
	const auto weak = base::make_weak(this);
	_insideEventRedirect = true;
	QGuiApplication::sendEvent(_inner, e);
	if (weak) {
		_insideEventRedirect = false;
	}
	return base::EventFilterResult::Cancel;
}

void MainMenu::setupSwipe() {
	const auto outer = _controller->widget()->body();
	base::install_event_filter(this, outer, [=](not_null<QEvent*> e) {
		const auto type = e->type();
		if (type == QEvent::TouchBegin
			|| type == QEvent::TouchUpdate
			|| type == QEvent::TouchEnd
			|| type == QEvent::TouchCancel) {
			return redirectToInnerChecked(e);
		} else if (type == QEvent::Wheel) {
			const auto w = static_cast<QWheelEvent*>(e.get());
			const auto d = Ui::ScrollDeltaF(w);
			if (std::abs(d.x()) > std::abs(d.y())) {
				return redirectToInnerChecked(e);
			}
		}
		return base::EventFilterResult::Continue;
	});
	const auto handles = outer->testAttribute(Qt::WA_AcceptTouchEvents);
	if (!handles) {
		outer->setAttribute(Qt::WA_AcceptTouchEvents);
		lifetime().add([=] {
			outer->setAttribute(Qt::WA_AcceptTouchEvents, false);
		});
	}

	auto update = [=](Ui::Controls::SwipeContextData data) {
		if (data.translation < 0) {
			if (!_swipeBackData.callback) {
				_swipeBackData = Ui::Controls::SetupSwipeBack(
					this,
					[=]() -> std::pair<QColor, QColor> {
						return {
							st::historyForwardChooseBg->c,
							st::historyForwardChooseFg->c,
						};
					});
			}
			_swipeBackData.callback(data);
			return;
		} else if (_swipeBackData.lifetime) {
			_swipeBackData = {};
		}
	};

	auto init = [=](Ui::Controls::SwipeHandlerInitData data) {
		if (data.direction != Qt::LeftToRight) {
			return Ui::Controls::SwipeHandlerFinishData();
		}
		if (_emojiStatusPanel && _emojiStatusPanel->hasFocus()) {
			return Ui::Controls::SwipeHandlerFinishData();
		}
		return Ui::Controls::DefaultSwipeBackHandlerFinishData([=] {
			closeLayer();
		});
	};

	Ui::Controls::SetupSwipeHandler({
		.widget = _inner,
		.scroll = _scroll.data(),
		.update = std::move(update),
		.init = std::move(init),
	});
}

} // namespace Window
