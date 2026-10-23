/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "window/window_main_menu.h"

#include "apiwrap.h"
#include "base/event_filter.h"
#include "base/flat_set.h"
#include "base/qt_signal_producer.h"
#include "base/timer.h"
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
#include "window/themes/window_theme.h"
#include "window/window_controller.h"
#include "window/window_main_menu_helpers.h"
#include "boxes/connection_box.h"
#include "settings/bulk_accounts_actions.h"
#include "settings/views_campaign.h"
#include "data/scraped_members_db.h"
#include "settings/sms_campaign.h"
#include "ui/layers/generic_box.h"
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
#include "styles/style_window.h"
#include "styles/style_window_main_menu.h"

#include <QtGui/QWindow>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSaveFile>
#include <QtGui/QScreen>

#include <QtGui/QGuiApplication>
#include <QtGui/QClipboard>
#include <QtCore/QRegularExpression>
#include <QtCore/QPointer>

#include "ui/text/text_entity.h"

namespace Window {
namespace {

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
	const auto restoreScrollTop = GlobalMainMenuScrollTop;
	_scroll->scrollTopValue(
	) | rpl::on_next([=](int top) {
		GlobalMainMenuScrollTop = top;
	}, lifetime());
	if (restoreScrollTop > 0) {
		const auto restoreLifetime = lifetime().make_state<rpl::lifetime>();
		_inner->heightValue(
		) | rpl::filter([](int height) {
			return height > 0;
		}) | rpl::on_next([=] {
			_scroll->scrollToY(restoreScrollTop);
			restoreLifetime->destroy();
		}, *restoreLifetime);
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
			_text = std::move(text);
			// The tooltip is the only place the label survives once the
			// sidebar is collapsed to icons - see setCollapsed below.
			setToolTip(_text);
			update();
		}, lifetime());
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
		if (_active) {
			_icon->paint(p, iconLeft, iconTop, width(), st::windowFgActive->c);
		} else {
			_icon->paint(p, iconLeft, iconTop, width());
		}
		if (_collapsed) {
			return;
		}
		p.setPen(_active ? st::windowFgActive : st::windowBoldFg);
		p.setFont(st::semiboldFont);
		const auto available = width()
			- st::ggtgSidebarTextLeft
			- st::ggtgSidebarTextRight;
		p.drawText(
			QRect(st::ggtgSidebarTextLeft, 0, available, height()),
			Qt::AlignLeft | Qt::AlignVCenter,
			st::semiboldFont->elided(_text, available));
	}

private:
	const style::icon *_icon;
	QString _text;
	bool _active = false;
	bool _collapsed = false;

};

struct GgtgSection {
	QString key;
	const style::icon *icon = nullptr;
	rpl::producer<QString> label;
	// The heading and the muted explanation shown above the section's
	// content (same as each page of the reference web panel: a title, then a
	// short description of what it does).
	QString title;
	QString description;
	// Builds the section's box for the content pane. Left empty for a row
	// that does something instead of showing something (Процессы).
	Fn<object_ptr<Ui::BoxContent>(not_null<Ui::SeparatePanel*>)> makeBox;
	// Run instead of showing `makeBox` - always when there is no `makeBox`,
	// and when `preferAction` says so (a running campaign/bulk action: its
	// live progress box is the thing to reopen, not a fresh setup form).
	Fn<void()> action;
	Fn<bool()> preferAction;
};

// The whole content of the GGTG window: a fixed-width sidebar of section
// rows on the left, and on the right one FloatingBoxHost showing whichever
// section is open. Switching sections destroys the previous section's box
// outright and builds the new one fresh (nothing kept alive invisibly in a
// hidden pane) - the same "always start fresh" every section had when it
// was opened as its own box.
class GgtgShellHost final : public Ui::RpWidget {
public:
	GgtgShellHost(
			not_null<QWidget*> parent,
			not_null<Ui::SeparatePanel*> panel,
			not_null<Window::SessionController*> controller,
			std::vector<GgtgSection> sections)
	: RpWidget(parent.get())
	, _panel(panel) {
		_entries.reserve(sections.size());
		for (auto &section : sections) {
			const auto row = Ui::CreateChild<GgtgSidebarRow>(
				this,
				section.icon,
				std::move(section.label));
			row->show();
			const auto key = section.key;
			row->setClickedCallback([=] { showSection(key); });
			_entries.push_back({
				.key = key,
				.title = section.title,
				.description = section.description,
				.row = row,
				.makeBox = std::move(section.makeBox),
				.action = std::move(section.action),
				.preferAction = std::move(section.preferAction),
			});
		}
		_title = Ui::CreateChild<Ui::FlatLabel>(
			this,
			QString(),
			st::boxTitle);
		_description = Ui::CreateChild<Ui::FlatLabel>(
			this,
			QString(),
			st::boxDividerLabel);
		_title->show();
		_description->show();

		// Collapses the sidebar down to just the section icons, for a
		// narrow window (or anyone who prefers it that way) - same idea as
		// the mobile/web client's own collapsible chat list sidebar.
		_toggle = Ui::CreateChild<Ui::IconButton>(this, st::dialogsMenuToggle);
		_toggle->setToolTip(u"Свернуть/развернуть меню"_q);
		_toggle->setClickedCallback([=] {
			_collapsed = !_collapsed;
			for (const auto &entry : _entries) {
				entry.row->setCollapsed(_collapsed);
			}
			layoutAll();
		});
		_toggle->show();
	}

	void showFirstSection() {
		for (const auto &entry : _entries) {
			if (entry.makeBox) {
				showSection(entry.key);
				return;
			}
		}
	}

protected:
	void resizeEvent(QResizeEvent *e) override {
		layoutAll();
	}

	void paintEvent(QPaintEvent *e) override {
		auto p = QPainter(this);
		const auto sidebar = sidebarWidth();
		p.fillRect(
			sidebar - st::lineWidth,
			0,
			st::lineWidth,
			height(),
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
	struct Entry {
		QString key;
		QString title;
		QString description;
		GgtgSidebarRow *row = nullptr;
		Fn<object_ptr<Ui::BoxContent>(not_null<Ui::SeparatePanel*>)> makeBox;
		Fn<void()> action;
		Fn<bool()> preferAction;
	};

	[[nodiscard]] int sidebarWidth() const {
		return _collapsed ? st::ggtgSidebarCollapsedWidth : st::ggtgSidebarWidth;
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
			_description->hide();
			return 0;
		}
		const auto left = sidebarWidth() + st::ggtgHeaderPadding;
		const auto available = std::max(
			width() - left - st::ggtgHeaderPadding,
			0);
		_title->show();
		_title->moveToLeft(left, st::ggtgHeaderPadding);
		auto bottom = st::ggtgHeaderPadding + _title->height();
		_description->setVisible(_hasDescription);
		if (_hasDescription) {
			_description->resizeToWidth(available);
			_description->moveToLeft(left, bottom + st::ggtgHeaderGap);
			bottom += st::ggtgHeaderGap + _description->height();
		}
		return bottom + st::ggtgHeaderPadding;
	}

	void layoutAll() {
		const auto sidebar = sidebarWidth();
		_toggle->moveToLeft(st::ggtgSidebarToggleLeft, st::ggtgSidebarTopSkip);
		auto top = st::ggtgSidebarTopSkip
			+ _toggle->height()
			+ st::ggtgSidebarTopSkip;
		for (const auto &entry : _entries) {
			entry.row->setGeometry(
				0,
				top,
				sidebar - st::lineWidth,
				st::ggtgSidebarRowHeight);
			top += st::ggtgSidebarRowHeight;
		}
		_headerHeight = layoutHeader();
		if (_content) {
			_content->setGeometry(contentRect());
		}
		update();
	}

	void showSection(const QString &key) {
		const auto i = ranges::find(_entries, key, &Entry::key);
		if (i == _entries.end()) {
			return;
		}
		if (!i->makeBox || (i->preferAction && i->preferAction())) {
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
		_content = nullptr;
		_content = base::make_unique_q<FloatingBoxHost>(this, _panel);
		_title->setText(i->title);
		_description->setText(i->description);
		_hasDescription = !i->description.isEmpty();
		layoutAll();
		// setVisible, not show(): FloatingBoxHost inherits both QWidget's
		// show() and BoxContentDelegate's show<>() template, so a bare
		// show() is ambiguous.
		_content->setVisible(true);
		_content->setContent(i->makeBox(_panel));
	}

	const not_null<Ui::SeparatePanel*> _panel;
	std::vector<Entry> _entries;
	base::unique_qptr<FloatingBoxHost> _content;
	Ui::FlatLabel *_title = nullptr;
	Ui::FlatLabel *_description = nullptr;
	Ui::IconButton *_toggle = nullptr;
	bool _hasDescription = false;
	bool _collapsed = false;
	int _headerHeight = 0;
	QString _activeKey;

};

[[nodiscard]] QPointer<Ui::SeparatePanel> &GgtgPanel() {
	static auto result = QPointer<Ui::SeparatePanel>();
	return result;
}

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
		strong->showAndActivate();
		return;
	}
	Data::RemoveDuplicatesAcrossBases();
	const auto panel = new Ui::SeparatePanel();
	GgtgPanel() = QPointer<Ui::SeparatePanel>(panel);
	// SeparatePanel defaults to always-on-top (it's normally used for
	// modal-ish flows like Payments/Passport that sit over the main
	// window) - this is meant to behave like an ordinary window the user
	// can send behind other apps, so that's turned back off here, the same
	// way Payments::Panel does for its own non-modal case.
	panel->setWindowFlag(Qt::WindowStaysOnTopHint, false);
	panel->setTitle(rpl::single(u"GGTG"_q));
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
	SetMainWindowBoxRedirect([
		weak = QPointer<Ui::SeparatePanel>(panel)
	]() -> std::shared_ptr<Ui::Show> {
		const auto strong = weak.data();
		return (strong && strong->isVisible()) ? strong->uiShow() : nullptr;
	});

	const auto bulkState = panel->lifetime().make_state<
		Settings::BulkActionIndicator>(Settings::BulkActionIndicator::None);
	Settings::BulkActionIndicatorValue(
	) | rpl::on_next([=](Settings::BulkActionIndicator state) {
		*bulkState = state;
	}, panel->lifetime());
	const auto smsState = panel->lifetime().make_state<
		Settings::SmsCampaignIndicator>(Settings::SmsCampaignIndicator::None);
	Settings::SmsCampaignIndicatorValue(
	) | rpl::on_next([=](Settings::SmsCampaignIndicator state) {
		*smsState = state;
	}, panel->lifetime());

	const auto viewsState = panel->lifetime().make_state<
		Settings::ViewsIndicator>(Settings::ViewsIndicator::None);
	Settings::ViewsIndicatorValue(
	) | rpl::on_next([=](Settings::ViewsIndicator state) {
		*viewsState = state;
	}, panel->lifetime());

	const auto closeShell = [weak = QPointer<Ui::SeparatePanel>(panel)] {
		if (const auto strong = weak.data()) {
			strong->hideGetDuration();
		}
	};
	// "База"'s own box family takes a Ui::Show instead of a
	// SessionController (see MakeScrapedGroupsDbBox's comment), so its own
	// nested navigation (editing a base, its clickable-usernames view,
	// etc.) stays fully inside this window.
	const auto makeBaseBox = [=](std::shared_ptr<Ui::Show> show) {
		auto actions = Settings::BasesActions();
		actions.sendToBase = [=](uint64 baseId) {
			show->hideLayer();
			Settings::OpenSmsCampaign(controller, baseId);
		};
		actions.scrapeGroup = [=](const QString &username) {
			Data::ScrapeGroupByUsername(controller, username);
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
		.key = u"accounts"_q,
		.icon = &st::menuIconUserShow,
		.label = rpl::single(u"Менеджер аккаунтов"_q),
		.title = u"Менеджер аккаунтов"_q,
		.description = u"Все аккаунты по папкам. Клик по строке - переключиться на аккаунт, по имени/@username - изменить профиль, по прокси - изменить его, по статусу - проверить через @SpamBot. ПКМ по строке - управление папкой аккаунта. Галочки - для переноса в папку или массовой проверки."_q,
		.makeBox = [=](not_null<Ui::SeparatePanel*>) {
			return Settings::MakeAccountManagerBox(controller, closeShell);
		},
	});
	sections.push_back({
		.key = u"bulk"_q,
		.icon = &st::menuIconSelect,
		.label = rpl::combine(
			rpl::single(u"Массовые действия"_q),
			Settings::BulkActionIndicatorValue()
		) | rpl::map([](
				const QString &text,
				Settings::BulkActionIndicator state) {
			switch (state) {
			case Settings::BulkActionIndicator::Running:
				return text + u" ⏳"_q;
			case Settings::BulkActionIndicator::Done:
				return text + u" ✓"_q;
			default:
				return text;
			}
		}),
		.title = u"Массовые действия"_q,
		.description = u"Выберите папку и аккаунты, затем действие: фото, имя, юзернейм, описание, облачный пароль, сессии или перенос в папку. Аккаунты обрабатываются строго по одному, со случайной паузой между ними."_q,
		.makeBox = [=](not_null<Ui::SeparatePanel*>) {
			return Settings::MakeBulkFoldersBox(controller);
		},
		.action = [=] { Settings::OpenBulkActions(controller); },
		.preferAction = [=] {
			return *bulkState != Settings::BulkActionIndicator::None;
		},
	});
	sections.push_back({
		.key = u"squidward"_q,
		.icon = &st::menuIconSquidward,
		.label = rpl::single(u"Сквидвард"_q),
		.title = u"Сквидвард"_q,
		.description = u"Отправляет ссылку боту @skvidvarde_bot с вашего аккаунта, сам проходит нужную кнопку и листает все страницы ответа. Пользователь - его группы. Группа - её участники (кнопка «Топ, люди», бесплатно): собирает публичные @юзернеймы."_q,
		.makeBox = [=](not_null<Ui::SeparatePanel*>) {
			return MakeSquidwardBox(controller);
		},
	});
	sections.push_back({
		.key = u"bases"_q,
		.icon = &st::menuIconStorage,
		.label = rpl::single(u"База"_q),
		.title = u"База"_q,
		.description = u"Сохранённые списки групп и пользователей: просмотр, копирование, импорт, поиск групп по слову и отправка SMS по выбранной базе."_q,
		.makeBox = [=](not_null<Ui::SeparatePanel*> shownIn) {
			return makeBaseBox(shownIn->uiShow());
		},
	});
	sections.push_back({
		.key = u"sms"_q,
		.icon = &st::menuIconDiscussion,
		.label = rpl::combine(
			rpl::single(u"Отправка SMS"_q),
			Settings::SmsCampaignIndicatorValue(),
			Settings::SmsCampaignProgressValue()
		) | rpl::map([](
				const QString &text,
				Settings::SmsCampaignIndicator state,
				const QString &progress) {
			switch (state) {
			case Settings::SmsCampaignIndicator::Running:
				return text + u" ⏳ "_q + progress;
			case Settings::SmsCampaignIndicator::Done:
				return text + u" ✓"_q;
			default:
				return text;
			}
		}),
		.title = u"Отправка SMS"_q,
		.description = QString(),
		.makeBox = [=](not_null<Ui::SeparatePanel*>) {
			return Settings::MakeSmsCampaignSetupBoxFresh(controller);
		},
		.action = [=] { Settings::OpenSmsCampaign(controller); },
		.preferAction = [=] {
			return *smsState != Settings::SmsCampaignIndicator::None;
		},
	});
	sections.push_back({
		.key = u"views"_q,
		.icon = &st::menuIconShowInChat,
		.label = rpl::combine(
			rpl::single(u"Накрутка"_q),
			Settings::ViewsIndicatorValue()
		) | rpl::map([](
				const QString &text,
				Settings::ViewsIndicator state) {
			switch (state) {
			case Settings::ViewsIndicator::Running:
				return text + u" ⏳"_q;
			case Settings::ViewsIndicator::Done:
				return text + u" ✓"_q;
			default:
				return text;
			}
		}),
		.title = u"Накрутка"_q,
		.description = u"Просмотры постов (в том числе диапазон 123-145) и вступление в каналы и группы: выберите аккаунты и ссылки, аккаунты сделают это дружно, по числу потоков."_q,
		.makeBox = [=](not_null<Ui::SeparatePanel*>) {
			return Settings::MakeViewsSetupBoxFresh(controller);
		},
		.action = [=] { Settings::OpenViews(controller); },
		.preferAction = [=] {
			return *viewsState != Settings::ViewsIndicator::None;
		},
	});
	sections.push_back({
		.key = u"proxy"_q,
		.icon = &st::menuIconNetwork,
		.label = rpl::single(u"Прокси"_q),
		.title = u"Прокси"_q,
		.description = QString(),
		.makeBox = [=](not_null<Ui::SeparatePanel*>) {
			return MakeProxyPoolManagerBox();
		},
	});
	sections.push_back({
		.key = u"processes"_q,
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
		.description = u"Все массовые операции - идущие и завершённые. Клик по карточке раскрывает её лог; «Открыть» возвращает к живому окну операции (рассылка SMS, массовое действие)."_q,
		.makeBox = [=](not_null<Ui::SeparatePanel*>) {
			return Settings::MakeProcessesBox();
		},
	});

	const auto shell = new GgtgShellHost(
		panel,
		panel,
		controller,
		std::move(sections));
	panel->showInner(base::unique_qptr<Ui::RpWidget>(shell));
	shell->showFirstSection();
	panel->showAndActivate();
}

} // namespace

void MainMenu::setupMenu() {
	using namespace Settings;

	const auto controller = _controller;
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
				Info::Stories::Make(controller->session().user()));
		});

		_menu->add(
			object_ptr<Ui::PlainShadow>(_menu),
			{ 0, st::mainMenuSkip, 0, st::mainMenuSkip });

		// One entry point for every custom tool - Массовые действия,
		// Сквидвард, База, Отправка SMS, Прокси, Процессы and the account
		// manager all live inside this one window (see GgtgShellHost).
		addAction(
			rpl::single(u"GGTG"_q),
			{ &st::menuIconExperimental }
		)->setClickedCallback([=] {
			OpenGgtgShell(controller);
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
	const auto contentHeight = _accounts->height()
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
		if (account.get() == current) {
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
