/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "settings/bulk_accounts_actions.h"
#include "settings/ggtg_warmup.h"
#include "settings/ggtg_android_profiles.h"
#include "settings/ggtg_ios_profiles.h"
#include "core/ggtg_id_date.h"
#include "settings/sections/settings_active_sessions.h"
#include "settings/sms_campaign.h"
#include "settings/settings_common.h"

#include "api/api_authorizations.h"
#include "api/api_cloud_password.h"
#include "api/api_peer_photo.h"
#include "apiwrap.h"
#include "base/algorithm.h"
#include "base/call_delayed.h"
#include "base/platform/base_platform_info.h"
#include "base/random.h"
#include "base/weak_ptr.h"
#include "base/event_filter.h"
#include "ui/widgets/tooltip.h"
#include "mtproto/proxy_pool.h"
#include "boxes/connection_box.h" // MakeAccountProxyBox, for the "click the Прокси cell to edit it" flow
#include "core/application.h"
#include "core/file_utilities.h"
#include "core/process_manager.h"
#include "data/data_photo.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "data/data_peer_values.h"
#include "data/account_spam_status.h"
#include "core/ggtg_ai_feedback.h"
#include "core/ggtg_name_sort.h"
#include "core/ggtg_fake_detector.h"
#include "data/scraped_members_db.h"
#include "lang/lang_instance.h"
#include "lang/lang_keys.h"
#include "main/main_account.h"
#include "base/timer.h"
#include "base/unixtime.h"
#include "mtproto/facade.h"
#include "storage/storage_account.h"
#include "storage/storage_tdata_export.h"
#include "storage/storage_tdata_import.h"
#include "settings/ggtg_export.h"
#include "settings/ggtg_session_json.h"
#include "core/ggtg_sessions.h"
#include "settings/ggtg_registration.h"
#include "main/main_domain.h"
#include "main/session_duplicate.h"
#include "main/main_session.h"
#include "mtproto/sender.h"
#include "ui/boxes/confirm_box.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/empty_userpic.h"
#include "ui/text/text_utilities.h"
#include "ui/userpic_view.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/popup_menu.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/fields/password_input.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/scroll_area.h"

#include <array>

#include <QtCore/QLocale>
#include <QtWidgets/QScrollBar> // horizontal-scroll sync between the account manager's table header and its rows
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"
#include "window/window_session_controller_link_info.h"
#include "boxes/abstract_box.h" // Ui::show().
#include "styles/style_boxes.h"
#include "styles/style_settings.h"
#include "styles/palette.h"
#include "styles/style_layers.h"
#include "styles/style_window_main_menu.h"
#include "styles/style_menu_icons.h"
#include "styles/style_widgets.h"

#include <QtCore/QDateTime>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkProxy>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>
#include <map>
#include <QtCore/QTimer>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QRegularExpression>
#include "ui/widgets/separate_panel.h"
#include "core/ggtg_log.h"
#include "ui/ui_utility.h"
#include "window/ggtg_click.h"
#include "window/ggtg_widgets.h"
#include <QtCore/QRandomGenerator>
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>
#include <QtGui/QMouseEvent>

namespace Settings {
namespace {

bool CopyDirRecursive(const QString &from, const QString &to) {
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
			if (!CopyDirRecursive(info.filePath(), target)) {
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

[[nodiscard]] QJsonObject ProxyToObject(const MTP::ProxyData &proxy) {
	if (proxy.type == MTP::ProxyData::Type::None || proxy.host.isEmpty()) {
		return QJsonObject();
	}
	return QJsonObject{
		{ "type", int(proxy.type) },
		{ "host", proxy.host },
		{ "port", int(proxy.port) },
		{ "user", proxy.user },
		{ "password", proxy.password },
	};
}

[[nodiscard]] MTP::ProxyData ProxyFromObject(const QJsonObject &object) {
	auto result = MTP::ProxyData();
	if (object.isEmpty()) {
		return result;
	}
	result.type = MTP::ProxyData::Type(object["type"].toInt());
	result.host = object["host"].toString();
	result.port = uint32(object["port"].toInt());
	result.user = object["user"].toString();
	result.password = object["password"].toString();
	return result;
}

// The pause the user asked for: never hit the server for several accounts
// in the same instant, always one at a time with a human-ish gap between.
constexpr auto kBulkPauseMin = crl::time(20'000);
constexpr auto kBulkPauseMax = crl::time(30'000);

// Retyping the old 2FA password by hand is annoying enough without also
// having to restart the whole bulk run over one typo - give it a couple
// more tries before finally giving up on that one account.
constexpr auto kPasswordAttempts = 3;

// If a single account's request never calls back (dropped connection,
// server just sitting on it - seen in practice on a first-time username
// assignment), the whole queue must not wait on it forever: after this
// long, count that account as failed and move on to the next one. Wide
// margin (not just a "typical request" budget) because a photo upload can
// legitimately take well past 30s on a slow or proxied connection, and a
// step that eventually succeeds server-side but gets reported as a false
// "таймаут" here is worse than the queue occasionally waiting longer on a
// genuinely dead one.
constexpr auto kStepTimeout = crl::time(90'000);

// "Открыть списком" row: the same eye icon as next to the recipients lists
// in the SMS campaign, instead of an emoji.
// A row button with a small icon on its left, like the eye of "Открыть
// списком" - "Отправить SMS по этой базе" gets the same look.
[[nodiscard]] Ui::SettingsButton *AddIconRowButton(
		not_null<Ui::GenericBox*> box,
		const QString &text,
		const style::IconButton &iconSt) {
	const auto button = box->addRow(object_ptr<Ui::SettingsButton>(
		box,
		rpl::single(text),
		st::settingsButton));
	const auto icon = Ui::CreateChild<Ui::IconButton>(button, iconSt);
	icon->setAttribute(Qt::WA_TransparentForMouseEvents);
	button->heightValue(
	) | rpl::on_next([=](int height) {
		icon->move(14, (height - icon->height()) / 2);
	}, icon->lifetime());
	return button;
}

[[nodiscard]] Ui::SettingsButton *AddOpenListButton(
		not_null<Ui::GenericBox*> box) {
	const auto button = box->addRow(object_ptr<Ui::SettingsButton>(
		box,
		rpl::single(u"Открыть списком (кликабельно)"_q),
		st::settingsButton));
	const auto eye = Ui::CreateChild<Ui::IconButton>(
		button,
		st::ggtgEyeButton);
	eye->setAttribute(Qt::WA_TransparentForMouseEvents);
	button->heightValue(
	) | rpl::on_next([=](int height) {
		eye->move(14, (height - eye->height()) / 2);
	}, eye->lifetime());
	return button;
}

} // namespace

// Accounts/StepDone/Step/GuardedStep declared in bulk_accounts_actions.h -
// shared with settings/sms_campaign.cpp.

// Wraps done() so a request that never calls back doesn't leave the queue
// stuck on this account forever - see kStepTimeout above.
GuardedStep GuardedDone(StepDone done) {
	const auto answered = std::make_shared<bool>(false);
	const auto guard = [answered, done](bool success, QString message) {
		if (*answered) {
			return;
		}
		*answered = true;
		done(success, message);
	};
	base::call_delayed(kStepTimeout, &Core::App(), [guard] {
		guard(false, u"таймаут ожидания ответа сервера"_q);
	});
	return {
		.done = guard,
		.cancel = [answered] { *answered = true; },
	};
}

// The color an SMS status - live or recalled from data/scraped_members_db
// - is shown in: green for an actual send, amber for anything skipped
// (an error, or a username already messaged before), red for SPAM_ACC
// (the sending account hit Telegram's "mutual contacts only" limit).
[[nodiscard]] QColor SmsRowColor(bool spamAcc, bool ok) {
	return spamAcc
		? st::boxTextFgError->c
		: ok
		? st::boxTextFgGood->c
		: st::historyPeer8NameFg->c;
}

[[nodiscard]] QString AccountLabel(not_null<Main::Account*> account) {
	if (!Main::AccountIsLive(account)) {
		return u"account (no session)"_q;
	}
	const auto user = account->session().user();
	const auto name = user->name();
	const auto phone = user->phone();
	if (name.isEmpty()) {
		return phone.isEmpty() ? u"account"_q : phone;
	}
	return phone.isEmpty() ? name : (name + u" ("_q + phone + ')');
}

[[nodiscard]] QString AccountPickerText(not_null<Main::Account*> account) {
	if (!Main::AccountIsLive(account)) {
		return u"(нет сессии)"_q;
	}
	const auto user = account->session().user();
	auto text = user->name();
	if (text.isEmpty()) {
		text = u"Аккаунт"_q;
	}
	const auto username = user->username();
	const auto phone = user->phone();
	if (!username.isEmpty()) {
		text += u" · @"_q + username;
	}
	if (!phone.isEmpty()) {
		text += u" · "_q + phone;
	}
	return text;
}

// A row ordered left-to-right as the user asked: the selection checkbox
// first, then the account's real photo, then its name/@username/phone as
// a plain label filling the rest - clicking the label (not just the tiny
// checkbox square) also toggles selection, same as clicking used to work
// when the text was the checkbox's own label.
[[nodiscard]] not_null<Ui::Checkbox*> AddAccountPickerRow(
		not_null<Ui::VerticalLayout*> container,
		not_null<Main::Account*> account) {
	// st::defaultCheckbox's own checkPosition/diameter are already scaled
	// by the app's UI scale factor (style::ConvertScale, applied to every
	// px() value at load time) - kPickerUserpicSize and the plain-pixel
	// offsets below (8, 12, 4...) are raw literals and were NOT, so at any
	// scale other than 100% the checkbox and the userpic/text drifted out
	// of proportion with each other (checkbox scaled, everything else
	// didn't) even though the centering formula itself was correct at
	// 100% - that's the real reason this kept coming back "crooked" no
	// matter how the checkbox's own vertical centering was recomputed.
	// Scale every literal here the same way so the whole row scales as
	// one unit.
	const auto userpicSize = style::ConvertScale(kPickerUserpicSize);
	const auto rowHeight = userpicSize + style::ConvertScale(8);
	auto rowOwned = object_ptr<Ui::RpWidget>(container);
	const auto row = rowOwned.data();
	row->resize(row->width(), rowHeight);

	const auto checkbox = Ui::CreateChild<Ui::Checkbox>(
		row,
		QString(),
		false);
	// st::defaultCheckbox's `width: -44px` bakes in room for a text label
	// via Checkbox::updateNaturalWidth() even when the label is empty (as
	// it always is here), so the widget's own hit-tested/painted bounding
	// box was ~74px wide - almost 2.5x the glyph's real ~30px footprint,
	// reaching well past where the userpic starts and overlapping it (the
	// actual source of the reported crooked/broken-looking checkbox -
	// ripple/hover painted across that whole overlap, not just the glyph).
	// Force the widget down to the glyph's own tight bounds instead.
	const auto checkboxVisualWidth = st::defaultCheckbox.checkPosition.x()
		+ st::defaultCheck.diameter;
	// Height only needs to fit the glyph itself, not checkPosition.y()'s
	// padding too: Ui::Checkbox centers the glyph vertically inside
	// whatever height the widget is given (see the comment on the
	// vertical-centering formula below), so a taller widget here only
	// grows the *hit-tested* box - hoverable and clickable, but with
	// nothing actually drawn there - above and below the visible glyph,
	// without moving the glyph itself at all. Width still needs the
	// checkPosition.x() padding: that axis is left-aligned, not centered,
	// so the glyph starts *after* it rather than being centered within it.
	checkbox->resize(checkboxVisualWidth, st::defaultCheck.diameter);
	const auto checkLeft = 0;
	const auto userpicLeft = checkboxVisualWidth + style::ConvertScale(8);
	const auto textLeft = userpicLeft + userpicSize + style::ConvertScale(12);
	// Mathematically this glyph's own bounding box centers exactly on the
	// row's middle (Ui::Checkbox's default alignment is Qt::AlignVCenter),
	// same as the userpic's and the labels' - all three use the identical
	// (rowHeight - elementHeight) / 2 formula, which lands on rowHeight/2
	// regardless of the element's own size. Despite that, this still reads
	// as sitting above the userpic - a small checkbox square next to a
	// larger round photo doesn't look centered at equal geometric centers
	// the way two same-shaped things would (the same reason a checkmark
	// glyph is nudged a pixel or two off true-center in most icon sets).
	// Bias it down a couple of px to match how it actually reads.
	const auto checkboxOpticalNudge = style::ConvertScale(7);
	checkbox->moveToLeft(
		checkLeft,
		(row->height() - checkbox->height()) / 2 + checkboxOpticalNudge);

	auto userpicWidget = QPointer<Ui::RpWidget>();
	if (account->sessionExists()) {
		const auto view = row->lifetime().make_state<Ui::PeerUserpicView>();
		const auto userpic = Ui::CreateChild<Ui::RpWidget>(row);
		userpicWidget = userpic;
		userpic->setAttribute(Qt::WA_TransparentForMouseEvents);
		userpic->resize(userpicSize, userpicSize);
		userpic->moveToLeft(userpicLeft, (row->height() - userpicSize) / 2);
		const auto user = account->session().user();
		userpic->paintRequest(
		) | rpl::on_next([=] {
			auto p = Painter(userpic);
			user->paintUserpicLeft(
				p,
				*view,
				0,
				0,
				userpicSize,
				userpicSize);
		}, userpic->lifetime());
	}

	// Name on top, "@username · phone" under it: nothing is cut with "…" -
	// both wrap to the width of the row, which grows to fit them.
	const auto nameText = account->sessionExists()
		? account->session().user()->name()
		: u"(нет сессии)"_q;
	auto detailParts = QStringList();
	if (account->sessionExists()) {
		const auto user = account->session().user();
		if (!user->username().isEmpty()) {
			detailParts.push_back(u"@"_q + user->username());
		}
		if (!user->phone().isEmpty()) {
			detailParts.push_back(user->phone());
		}
	}
	const auto detailsText = detailParts.join(u"  ·  "_q);
	const auto nameLabel = Ui::CreateChild<Ui::FlatLabel>(
		row,
		nameText.isEmpty() ? u"Аккаунт"_q : nameText,
		st::boxLabel);
	nameLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
	const auto detailsLabel = Ui::CreateChild<Ui::FlatLabel>(
		row,
		detailsText,
		st::boxLabel);
	detailsLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
	detailsLabel->setTextColorOverride(st::windowSubTextFg->c);
	detailsLabel->setVisible(!detailsText.isEmpty());
	// Telegram's Premium star / emoji status right after the name.
	const auto premium = account->sessionExists()
		? Ui::CreateChild<Ggtg::PremiumBadge>(
			row,
			account->session().user())
		: nullptr;
	row->widthValue(
	) | rpl::on_next([=](int width) {
		const auto badgeWidth = premium ? premium->badgeWidth() : 0;
		const auto available = std::max(
			width - textLeft - style::ConvertScale(8) - badgeWidth,
			style::ConvertScale(60));
		nameLabel->resizeToWidth(available);
		detailsLabel->resizeToWidth(available);
		const auto textHeight = nameLabel->height()
			+ (detailsText.isEmpty() ? 0 : detailsLabel->height());
		const auto height = std::max(
			rowHeight,
			textHeight + 2 * style::ConvertScale(6));
		if (row->height() != height) {
			row->resize(width, height);
		}
		checkbox->moveToLeft(
			checkLeft,
			(height - checkbox->height()) / 2 + checkboxOpticalNudge);
		if (const auto userpic = userpicWidget.data()) {
			userpic->moveToLeft(userpicLeft, (height - userpicSize) / 2);
		}
		const auto top = (height - textHeight) / 2;
		nameLabel->moveToLeft(textLeft, top, width);
		if (badgeWidth > 0) {
			premium->moveToLeft(
				textLeft + std::min(
					nameLabel->naturalWidth(),
					nameLabel->width()),
				top,
				width);
		}
		if (!detailsText.isEmpty()) {
			detailsLabel->moveToLeft(
				textLeft,
				top + nameLabel->height(),
				width);
		}
	}, row->lifetime());

	// One click handler for the whole row (checkbox glyph included) -
	// see Ggtg::SetupRowClicks for why the checkbox itself must not also
	// react (double toggle) and why a quick second click must not be lost.
	Ggtg::SetupRowClicks(row, { checkbox }, [=](QPoint) {
		checkbox->setChecked(!checkbox->checked());
	});

	// box->addRow() insets every other row in these boxes by
	// st::boxRowPadding (24px each side) - added straight to the
	// container without it, this row sat flush against the box edge
	// while everything above/below it was properly inset, which read as
	// misaligned next to the rest of the box.
	container->add(std::move(rowOwned), st::accountsPickerRowPadding);
	return checkbox;
}

// The key of the «Корзина» group in the account lists (never shown as is: a
// private character keeps it from clashing with a folder the user named).
[[nodiscard]] QString TrashGroupTag() {
	return u"\u2063trash"_q;
}

not_null<Ui::RpWidget*> MakeFolderIcon(
		not_null<Ui::RpWidget*> parent,
		const QString &folderName,
		int size,
		bool trash = false) {
	const auto icon = Ui::CreateChild<Ui::RpWidget>(parent.get());
	icon->setAttribute(Qt::WA_TransparentForMouseEvents);
	icon->resize(size, size);
	if (trash) {
		// Telegram's own trash icon on a round grey chip, not a letter.
		icon->paintRequest(
		) | rpl::on_next([=] {
			auto p = QPainter(icon);
			auto hq = PainterHighQualityEnabler(p);
			p.setPen(Qt::NoPen);
			p.setBrush(st::windowSubTextFg);
			p.drawEllipse(0, 0, size, size);
			st::menuIconDelete.paintInCenter(
				p,
				QRect(0, 0, size, size),
				st::windowFgActive->c);
		}, icon->lifetime());
		icon->show();
		icon->raise();
		return icon;
	}
	auto nameHash = uint64(0);
	for (const auto &ch : folderName) {
		nameHash = nameHash * 31 + ch.unicode();
	}
	const auto emptyUserpic = std::make_shared<Ui::EmptyUserpic>(
		Ui::EmptyUserpic::UserpicColor(Ui::EmptyUserpic::ColorIndex(nameHash)),
		folderName);
	icon->paintRequest(
	) | rpl::on_next([=] {
		auto p = Painter(icon);
		emptyUserpic->paintCircle(p, 0, 0, size, size);
	}, icon->lifetime());
	icon->show();
	icon->raise();
	return icon;
}

// Ui::ScrollArea isn't an Ui::RpWidget (it wraps QScrollArea directly), so
// it can't go through VerticalLayout::add on its own - same reason
// AddPasswordRow above wraps Ui::PasswordInput in a plain object_ptr<
// Ui::RpWidget> first. The wrap's own height is pinned to
// st::previewListHeight so the box reserves exactly that much space
// regardless of the scrolled content's real height.
[[nodiscard]] not_null<Ui::ScrollArea*> AddScrollAreaRow(
		not_null<Ui::VerticalLayout*> container) {
	auto wrapOwned = object_ptr<Ui::RpWidget>(container);
	const auto wrap = wrapOwned.data();
	const auto scroll = Ui::CreateChild<Ui::ScrollArea>(
		wrap, st::previewListScroll);
	// The scroll area always fills the wrap; the wrap's own height starts at
	// the maximum and may be shrunk by the caller to its content.
	wrap->sizeValue(
	) | rpl::on_next([=](QSize size) {
		scroll->setGeometry(0, 0, size.width(), size.height());
	}, wrap->lifetime());
	wrap->resize(wrap->width(), st::previewListHeight);
	container->add(std::move(wrapOwned), st::boxRowPadding);
	return scroll;
}

not_null<Ui::FlatLabel*> AddScrollablePreview(
		not_null<Ui::VerticalLayout*> container,
		const QString &text) {
	const auto scroll = AddScrollAreaRow(container);
	const auto label = scroll->setOwnedWidget(
		object_ptr<Ui::FlatLabel>(scroll, text, st::boxLabel)).data();
	label->setSelectable(true);
	scroll->widthValue(
	) | rpl::on_next([=](int width) {
		label->resizeToWidth(width);
	}, label->lifetime());
	// A short text (one queued recipient) no longer leaves a big empty
	// block: the row is as tall as its text, up to the usual maximum, past
	// which it scrolls.
	const auto wrap = static_cast<Ui::RpWidget*>(scroll->parentWidget());
	label->heightValue(
	) | rpl::on_next([=](int height) {
		const auto wanted = std::min(
			std::max(height, st::normalFont->height),
			st::previewListHeight);
		if (wrap->height() != wanted) {
			wrap->resize(wrap->width(), wanted);
		}
	}, label->lifetime());
	return label;
}

not_null<Ui::VerticalLayout*> AddScrollableRowsArea(
		not_null<Ui::VerticalLayout*> container) {
	const auto scroll = AddScrollAreaRow(container);
	const auto inner = scroll->setOwnedWidget(
		object_ptr<Ui::VerticalLayout>(scroll)).data();
	scroll->widthValue(
	) | rpl::on_next([=](int width) {
		inner->resizeToWidth(width);
	}, inner->lifetime());
	return inner;
}

namespace {

[[nodiscard]] crl::time NextBulkPause() {
	return kBulkPauseMin + crl::time(base::RandomIndex(
		int(kBulkPauseMax - kBulkPauseMin) + 1));
}

[[nodiscard]] QChar FirstAsciiLetter(const QString &text, QChar fallback) {
	for (const auto &ch : text) {
		if (((ch >= 'a') && (ch <= 'z')) || ((ch >= 'A') && (ch <= 'Z'))) {
			return ch.toLower();
		}
	}
	return fallback;
}

// Ui::PasswordInput is a MaskedInputField, not an RpWidget, so it can't go
// through GenericBox::addRow(object_ptr<Widget>) directly - wrap it the
// same way ProxyBox::setupCredentials does (see connection_box.cpp).
[[nodiscard]] not_null<Ui::PasswordInput*> AddPasswordRow(
		not_null<Ui::GenericBox*> box,
		rpl::producer<QString> placeholder) {
	auto wrap = object_ptr<Ui::RpWidget>(box);
	const auto field = Ui::CreateChild<Ui::PasswordInput>(
		wrap.data(),
		st::defaultInputField,
		std::move(placeholder));
	field->move(0, 0);
	field->heightValue(
	) | rpl::on_next([raw = wrap.data()](int height) {
		raw->resize(raw->width(), height);
	}, field->lifetime());
	wrap->widthValue(
	) | rpl::on_next([=](int width) {
		field->resize(width, field->height());
	}, field->lifetime());
	box->addRow(std::move(wrap));
	return field;
}

// Everything a running (or just-finished) bulk action queue needs to keep
// going, and to redraw its live progress box from scratch - stored outside
// any box, same idea as settings/sms_campaign.cpp's CampaignRunState, so
// "Закрыть" (which only hides the current layer) never loses a run and
// OpenBulkActions can reopen an identical-looking box around it later.
struct BulkRunState {
	QString title = u"Массовое действие"_q;
	std::vector<QString> rowTexts;
	std::vector<std::optional<QColor>> rowColors;
	std::vector<QPointer<Ui::FlatLabel>> rows;

	QString summaryText;
	QPointer<Ui::FlatLabel> summary;
	QString statusText;
	QPointer<Ui::FlatLabel> status;

	rpl::variable<QString> buttonText
		= rpl::variable<QString>(u"Остановить очередь"_q);
	std::shared_ptr<bool> cancelled = std::make_shared<bool>(false);
	bool finished = false;

	int index = 0;
	int done = 0;
	int total = 0;

	// Mirrors this run into the shared "Процессы" list - see
	// core/process_manager.h and settings/sms_campaign.cpp's identical use
	// of the same pattern on CampaignRunState.
	std::shared_ptr<Core::ProcessHandle> process;
};

[[nodiscard]] rpl::variable<BulkActionIndicator> &BulkIndicatorState() {
	static auto result = rpl::variable<BulkActionIndicator>(
		BulkActionIndicator::None);
	return result;
}

[[nodiscard]] std::shared_ptr<BulkRunState> &CurrentBulkRun() {
	static auto result = std::shared_ptr<BulkRunState>();
	return result;
}

void FinishBulkRun(not_null<BulkRunState*> state) {
	if (state->finished) {
		return;
	}
	state->finished = true;
	state->buttonText = u"Готово"_q;
	BulkIndicatorState() = BulkActionIndicator::Done;
	if (state->process) {
		state->process->succeed(state->summaryText);
	}
}

// Builds (or rebuilds, on reopen) the live progress box around an existing
// BulkRunState - mirrors settings/sms_campaign.cpp's
// BuildCampaignProgressBox exactly, including the same "Готово"/"Закрыть"
// dismiss semantics.
void BuildBulkProgressBox(
		std::shared_ptr<Ui::Show> show,
		std::shared_ptr<BulkRunState> state) {
	show->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setWidth(st::accountsBoxWidth);
		box->setTitle(rpl::single(state->title));
		Ggtg::AddProcessHideButton(box, [=](std::shared_ptr<Ui::Show> to) {
			BuildBulkProgressBox(std::move(to), state);
		});
		Ui::AddSkip(box->verticalLayout());
		state->summary = box->addRow(object_ptr<Ui::FlatLabel>(
			box, state->summaryText, st::boxLabel));
		// Live "what's happening right now" line (pause countdown between
		// accounts, currently blank while a step is actually in flight) -
		// the per-row text below already says "выполняется"/"успешно"/
		// the error, but doesn't show the pause between them.
		state->status = box->addRow(
			object_ptr<Ui::FlatLabel>(box, state->statusText, st::boxLabel));
		Ui::AddSkip(box->verticalLayout());
		const auto rows = AddScrollableRowsArea(box->verticalLayout());
		for (auto i = 0; i != int(state->rowTexts.size()); ++i) {
			const auto label = rows->add(
				object_ptr<Ui::FlatLabel>(
					rows, state->rowTexts[i], st::boxLabel),
				st::boxRowPadding);
			if (state->rowColors[i]) {
				label->setTextColorOverride(state->rowColors[i]);
			}
			state->rows[i] = label;
		}

		const auto dismiss = [=] {
			if (CurrentBulkRun() == state) {
				CurrentBulkRun() = nullptr;
				BulkIndicatorState() = BulkActionIndicator::None;
			}
		};
		box->addButton(state->buttonText.value(), [=] {
			if (!state->finished) {
				*state->cancelled = true;
			}
			FinishBulkRun(state.get());
			dismiss();
			box->closeBox();
		});
	}));
}

struct BulkOptions {
	QString title = u"Массовое действие"_q;
	Fn<crl::time()> pause;
	QString failPrefix = u"ошибка: "_q;
};

// Sequential runner: one row per account, its status text updated in place
// as the run reaches it, with a Cancel button that stops the *queue* (an
// already-in-flight request still finishes on its own - cancelling can't
// un-send a request already on the wire). Once the queue runs out on its
// own (not cancelled), that button turns into "Готово" and just closes the
// box - there's nothing left to stop. The progress box itself can be
// hidden ("Закрыть") and reopened later via OpenBulkActions while this
// keeps running in the background.
void RunBulkAction(
		not_null<Window::SessionController*> controller,
		Accounts accounts,
		StepWithProgress step,
		BulkOptions options = {}) {
	Expects(!accounts.empty());

	if (!Core::GuardAllows(
			controller->uiShow(),
			Core::GuardedAction::StartBulk)) {
		return;
	}
	// Accounts from the «Корзина» sleep without a connection - wake the
	// ones this run uses.
	for (const auto &account : accounts) {
		account->wake();
	}
	const auto state = std::make_shared<BulkRunState>();
	state->title = options.title;
	state->total = int(accounts.size());
	state->rowTexts.resize(state->total);
	state->rowColors.resize(state->total);
	state->rows.resize(state->total);
	for (auto i = 0; i != state->total; ++i) {
		state->rowTexts[i] = AccountLabel(accounts[i]) + u" — в очереди"_q;
	}
	state->summaryText = u"Выполнено: 0 / "_q + QString::number(state->total);

	CurrentBulkRun() = state;
	BulkIndicatorState() = BulkActionIndicator::Running;

	// Weak, not the controller itself - reopen() can fire long after this
	// call returns, from whatever window the user is looking at "Процессы"
	// in by then.
	const auto weakController = base::make_weak(controller);
	state->process = Core::App().processes().start(
		options.title + u" (%1)"_q.arg(state->total),
		[=] {
			if (const auto alive = CurrentSessionController(weakController.get())) {
				OpenBulkActions(alive);
			}
		},
		Core::ProcessKind::Bulk);

	BuildBulkProgressBox(controller->uiShow(), state);

	const auto setRowText = [=](
			int i,
			const QString &text,
			std::optional<QColor> color) {
		state->rowTexts[i] = text;
		state->rowColors[i] = color;
		if (const auto label = state->rows[i].data()) {
			label->setText(text);
			label->setTextColorOverride(color);
		}
		if (state->process) {
			state->process->log(text);
		}
	};
	const auto setSummary = [=](const QString &text) {
		state->summaryText = text;
		if (const auto label = state->summary.data()) {
			label->setText(text);
		}
		if (state->process) {
			state->process->progress(state->done, state->total);
		}
	};
	const auto setStatus = [=](const QString &text) {
		state->statusText = text;
		if (const auto label = state->status.data()) {
			label->setText(text);
		}
	};

	const auto runNext = std::make_shared<Fn<void()>>();
	*runNext = [=] {
		if (*state->cancelled) {
			return;
		}
		if (state->index >= state->total) {
			FinishBulkRun(state.get());
			setStatus(QString());
			return;
		}
		const auto i = state->index;
		const auto account = accounts[i];
		// Taken now: a step may replace the account (switching its
		// session), and the object is gone by the time it answers.
		const auto label = AccountLabel(account);
		setRowText(
			i,
			label + u" — выполняется..."_q,
			std::nullopt);
		setStatus(label + u" — выполняется..."_q);

		const auto onProgress = [=](QString progress) {
			if (progress.isEmpty()) {
				progress = u"выполняется..."_q;
			}
			setRowText(
				i,
				label + u" — "_q + progress,
				std::nullopt);
			setStatus(label + u": "_q + progress);
		};

		step(account, onProgress, [=](bool success, QString message) {
			setStatus(QString());
			setRowText(
				i,
				label + u" — "_q + (success
					? (u"успешно"_q + (message.isEmpty()
						? QString()
						: (u" ("_q + message + ')')))
					: (options.failPrefix + (message.isEmpty()
						? u"неизвестная"_q
						: message))),
				// Color-coded like the SMS campaign's own progress rows,
				// so a long queue scans at a glance instead of forcing a
				// read of every line.
				SmsRowColor(!success, success));
			++state->done;
			setSummary(u"Выполнено: "_q
				+ QString::number(state->done) + u" / "_q
				+ QString::number(state->total));
			state->index = i + 1;
			if (*state->cancelled) {
				return;
			}
			if (state->index >= state->total) {
				FinishBulkRun(state.get());
				setStatus(QString());
				return;
			}
			const auto remaining = std::make_shared<int>(int(
				(options.pause ? options.pause() : NextBulkPause()) / 1000));
			const auto tick = std::make_shared<Fn<void()>>();
			*tick = [=] {
				if (*state->cancelled) {
					return;
				}
				if (*remaining <= 0) {
					setStatus(QString());
					(*runNext)();
					return;
				}
				setStatus(u"Пауза перед следующим аккаунтом: "_q
					+ QString::number(*remaining) + u" сек..."_q);
				--(*remaining);
				base::call_delayed(crl::time(1000), &Core::App(), *tick);
			};
			(*tick)();
		});
	};
	(*runNext)();
}

void RunBulkAction(
		not_null<Window::SessionController*> controller,
		Accounts accounts,
		Step step,
		BulkOptions options = {}) {
	RunBulkAction(
		controller,
		std::move(accounts),
		[step = std::move(step)](
				not_null<Main::Account*> account,
				StepProgress,
				StepDone done) {
			step(account, std::move(done));
		},
		std::move(options));
}

// ---- Step implementations (one account, MTP request(s), then done()) ----
// Asks @spambot about one account and waits for its verdict (25 s at most):
// the row of the progress window then says what it answered.
void StepCheckSpam(not_null<Main::Account*> account, StepDone done) {
	if (!Main::AccountIsLive(account)) {
		done(false, u"нет активной сессии"_q);
		return;
	}
	const auto userId = account->session().user()->id.value;
	const auto finished = std::make_shared<bool>(false);
	const auto life = std::make_shared<rpl::lifetime>();
	const auto finish = [=](bool free, QString text) {
		if (std::exchange(*finished, true)) {
			return;
		}
		life->destroy();
		done(free, text);
	};
	Data::AccountSpamStatusChanges(
	) | rpl::filter([=](uint64 id) {
		return id == userId;
	}) | rpl::on_next([=](uint64) {
		const auto status = Data::FindAccountSpamStatus(userId);
		const auto free = !status
			|| status->state == Data::AccountSpamState::Clear;
		finish(free, free
			? u"свободен"_q
			: (u"спам-блок: "_q
				+ Settings::AccountSpamStatusText(userId).toLower()));
	}, *life);
	base::call_delayed(crl::time(25'000), &Core::App(), [=] {
		finish(false, u"@spambot не ответил"_q);
	});
	CheckAccountSpamStatus(account, [=](QString reason) {
		if (!reason.isEmpty()) {
			finish(false, u"не удалось проверить: "_q + reason);
		}
	});
}

void StepAutoAppealSpam(not_null<Main::Account*> account, StepDone done) {
	if (!Main::AccountIsLive(account)) {
		done(false, u"нет активной сессии"_q);
		return;
	}
	const auto userId = account->session().user()->id.value;
	const auto status = Data::FindAccountSpamStatus(userId);
	if (status && status->state == Data::AccountSpamState::Clear) {
		done(true, u"свободен, апелляция не требуется"_q);
		return;
	}
	Settings::AutoAppealSpamBot(account, [=](bool success, QString message) {
		done(success, message.isEmpty()
			? (success ? u"апелляция отправлена"_q : u"ошибка"_q)
			: message);
	});
}


void StepUpdateProfile(
		not_null<Main::Account*> account,
		std::optional<QString> firstName,
		std::optional<QString> lastName,
		std::optional<QString> about,
		StepDone done) {
	if (!Main::AccountIsLive(account)) {
		done(false, u"нет активной сессии"_q);
		return;
	}
	done = GuardedDone(done).done;
	using Flag = MTPaccount_UpdateProfile::Flag;
	auto flags = MTPaccount_UpdateProfile::Flags(0);
	if (firstName) flags |= Flag::f_first_name;
	if (lastName) flags |= Flag::f_last_name;
	if (about) flags |= Flag::f_about;
	const auto api = static_cast<MTP::Sender*>(&account->session().api());
	const auto session = &account->session();
	api->request(MTPaccount_UpdateProfile(
		MTP_flags(flags),
		MTP_string(firstName.value_or(QString())),
		MTP_string(lastName.value_or(QString())),
		MTP_string(about.value_or(QString()))
	// Sent by the session's own ApiWrap, which outlives its requests and
	// cancels them when the session goes - see ResolveUsernameForAccount.
	)).done([=, api = api](const MTPUser &user) {
		session->data().processUser(user);
		done(true, QString());
	}).fail([=, api = api](const MTP::Error &error) {
		done(false, error.type());
	}).send();
}

void StepSetUsername(
		not_null<Main::Account*> account,
		bool remove,
		StepDone done) {
	if (!Main::AccountIsLive(account)) {
		done(false, u"нет активной сессии"_q);
		return;
	}
	done = GuardedDone(done).done;
	const auto session = &account->session();
	const auto user = session->user();
	auto username = QString();
	if (!remove) {
		const auto first = FirstAsciiLetter(user->firstName, 'u');
		const auto last = FirstAsciiLetter(user->lastName, 'x');
		username = QString(first) + QString(last)
			+ QString::number(session->userId().bare);
	}
	const auto api = static_cast<MTP::Sender*>(&session->api());
	api->request(MTPaccount_UpdateUsername(
		MTP_string(username)
	)).done([=, api = api](const MTPUser &result) {
		session->data().processUser(result);
		done(true, remove ? QString() : ('@' + username));
	}).fail([=, api = api](const MTP::Error &error) {
		done(false, error.type());
	}).send();
}

// Same request as StepSetUsername above, but with the caller's own typed
// username instead of the auto-generated "first letter + last letter + id"
// one - used by the single-account "click the account cell to edit it"
// flow (see ShowEditAccountBox), where the whole point is picking your own
// username rather than accepting whatever the bulk flow would generate.
void StepSetUsernameTo(
		not_null<Main::Account*> account,
		QString username,
		StepDone done) {
	if (!Main::AccountIsLive(account)) {
		done(false, u"нет активной сессии"_q);
		return;
	}
	done = GuardedDone(done).done;
	const auto session = &account->session();
	const auto api = static_cast<MTP::Sender*>(&session->api());
	api->request(MTPaccount_UpdateUsername(
		MTP_string(username)
	)).done([=, api = api](const MTPUser &result) {
		session->data().processUser(result);
		done(true, username.isEmpty() ? QString() : ('@' + username));
	}).fail([=, api = api](const MTP::Error &error) {
		done(false, error.type());
	}).send();
}

void StepTerminateOtherSessions(
		not_null<Main::Account*> account,
		StepDone done) {
	if (!Main::AccountIsLive(account)) {
		done(false, u"нет активной сессии"_q);
		return;
	}
	done = GuardedDone(done).done;
	TerminateOtherSessions(account, [=](QString error, QString report) {
		done(error.isEmpty(), error.isEmpty() ? report : error);
	});
}

// Switches the account onto its oldest usable spare; the session it leaves
// becomes a spare itself (see SwapToStoredSession).
void StepSwapToBackup(
		not_null<Main::Account*> account,
		StepDone done) {
	namespace Sessions = Core::GgtgSessions;
	if (!Main::AccountIsLive(account)) {
		done(false, u"нет активной сессии"_q);
		return;
	}
	const auto userId = QString::number(account->session().userId().bare);
	auto spares = std::vector<Sessions::StoredSession>();
	auto fallbackSpares = std::vector<Sessions::StoredSession>();
	for (const auto &entry
			: Sessions::StoredForUser(Sessions::LoadVault(), userId)) {
		if (entry.role == u"backup"_q
			&& QDir(Sessions::VaultFolder(entry) + u"/tdata"_q).exists()) {
			if (entry.state != u"revoked"_q) {
				spares.push_back(entry);
			} else {
				fallbackSpares.push_back(entry);
			}
		}
	}
	if (spares.empty()) {
		spares = std::move(fallbackSpares);
	}
	if (spares.empty()) {
		done(false, u"нет резервной сессии"_q);
		return;
	}
	const auto oldest = *std::min_element(spares.begin(), spares.end(), [](
			const Sessions::StoredSession &a,
			const Sessions::StoredSession &b) {
		return a.createdAt < b.createdAt;
	});
	done = GuardedDone(done).done;
	const auto created = QDateTime::fromSecsSinceEpoch(oldest.createdAt)
		.toString(u"dd.MM.yyyy"_q);
	SwapToStoredSession(account, oldest.id, [=](QString error) {
		done(
			error.isEmpty(),
			error.isEmpty()
				? (u"на резервной от "_q + created
					+ u", прежняя стала резервной"_q)
				: error);
	});
}

void StepAntiTheftProtection(
		not_null<Main::Account*> account,
		StepDone done) {
	namespace Sessions = Core::GgtgSessions;
	if (!Main::AccountIsLive(account)) {
		done(false, u"нет активной сессии"_q);
		return;
	}
	const auto userId = QString::number(account->session().userId().bare);
	const auto vault = Sessions::LoadVault();
	auto backups = std::vector<Sessions::StoredSession>();
	for (const auto &entry : Sessions::StoredForUser(vault, userId)) {
		if ((entry.role == u"backup"_q || entry.role == u"anti_theft_backup"_q)
			&& QDir(Sessions::VaultFolder(entry) + u"/tdata"_q).exists()
			&& entry.state != u"revoked"_q) {
			backups.push_back(entry);
		}
	}

	done = GuardedDone(done).done;

	if (backups.empty()) {
		CreateBackupSessionWith(
			account,
			u"резервная (анти-угон)"_q,
			MakeSessionPasswordPrompt(account),
			[=](QString err, QString id) {
				if (!err.isEmpty()) {
					done(false, u"ошибка создания сессии: "_q + err);
					return;
				}
				done(true, u"Резервная сессия создана. Запущен таймер 24ч отлёжки (Telegram блокирует сброс чужих сессий)."_q);
			});
		return;
	}

	std::sort(backups.begin(), backups.end(), [](const auto &a, const auto &b) {
		return a.createdAt < b.createdAt;
	});
	const auto oldest = backups.front();

	if (!Sessions::IsSessionTrusted(oldest)) {
		const auto countdown = Sessions::TrustCountdownString(oldest);
		done(false, u"Сессия на отлёжке (%1). Сброс сессий продавца пока заблокирован Telegram."_q.arg(countdown));
		return;
	}

	auto purchasedIds = std::vector<QString>();
	for (const auto &entry : Sessions::StoredForUser(vault, userId)) {
		if (entry.role == u"purchased"_q || entry.role == u"imported"_q) {
			purchasedIds.push_back(entry.id);
		}
	}

	SwapToStoredSession(account, oldest.id, [=](QString swapErr) {
		if (!swapErr.isEmpty()) {
			done(false, u"не удалось переключить сессию: "_q + swapErr);
			return;
		}

		auto nowAcc = (Main::Account*)nullptr;
		for (const auto &a : Core::App().domain().orderedAccounts()) {
			if (a->sessionExists() && QString::number(a->session().userId().bare) == userId) {
				nowAcc = a.get();
				break;
			}
		}
		if (!nowAcc) {
			done(true, u"Сессия переключена на резервную"_q);
			return;
		}

		TerminateOtherSessions(nowAcc, [=](QString termErr, QString report) {
			for (const auto &pid : purchasedIds) {
				Sessions::RemoveStored(pid);
			}

			CreateBackupSession(nowAcc, u"резервная копия"_q, [=](QString backupErr) {
				auto msg = u"Защита от угона завершена: сессия продавца разорвана и удалена"_q;
				if (!report.isEmpty()) {
					msg += u" ("_q + report + u")"_q;
				}
				if (backupErr.isEmpty()) {
					msg += u", создан свежий бэкап"_q;
				}
				done(true, msg);
			});
		});
	});
}

void StepDeleteCurrentPhoto(
		not_null<Main::Account*> account,
		PhotoId oldPhotoId,
		Fn<void()> done) {
	if (!oldPhotoId) {
		done();
		return;
	}
	const auto session = &account->session();
	const auto old = session->data().photo(oldPhotoId);
	if (!old || old->isNull()) {
		done();
		return;
	}
	const auto api = static_cast<MTP::Sender*>(&session->api());
	api->request(MTPphotos_DeletePhotos(
		MTP_vector<MTPInputPhoto>(1, old->mtpInput())
	)).done([=, api = api](const MTPVector<MTPlong> &) {
		done();
	}).fail([=, api = api](const MTP::Error &) {
		done();
	}).send();
}

void StepChangePhoto(
		not_null<Main::Account*> account,
		QString path,
		bool deleteOldFirst,
		StepDone done) {
	if (!Main::AccountIsLive(account)) {
		done(false, u"нет активной сессии"_q);
		return;
	}
	done = GuardedDone(done).done;
	auto image = QImage();
	if (!image.load(path)) {
		done(false, u"не удалось прочитать файл"_q);
		return;
	}
	const auto session = &account->session();
	const auto user = session->user();
	const auto oldPhotoId = deleteOldFirst ? user->userpicPhotoId() : PhotoId(0);
	const auto lifetime = std::make_shared<rpl::lifetime>();
	session->api().peerPhoto().subscribeToUpload(user, *lifetime, {
		.done = [=] {
			lifetime->destroy();
			StepDeleteCurrentPhoto(account, oldPhotoId, [=] {
				done(true, QString());
			});
		},
		.failed = [=] {
			lifetime->destroy();
			done(false, u"ошибка загрузки фото"_q);
		},
	});
	session->api().peerPhoto().upload(user, { .image = std::move(image) });
}

// Retries with a freshly-typed old password (or gives up on this one
// account) when the cached "2FA password" note is missing/stale - see
// AskUserQuestion answer: try the note, prompt if it's wrong, skip if the
// user doesn't know it either.
void ResolveOldPasswordAndSet(
		not_null<Window::SessionController*> controller,
		not_null<Main::Account*> account,
		QString oldPassword,
		QString newPassword,
		QString hint,
		int attemptsLeft,
		StepDone done) {
	if (!Main::AccountIsLive(account)) {
		done(false, u"нет активной сессии"_q);
		return;
	}
	// A fresh guard per attempt - if this specific request never calls
	// back, it fails after kStepTimeout same as any other step; but once
	// a *real* answer comes back (wrong-password included) and we move to
	// waiting on the user instead, guarded.cancel() defuses it so it can't
	// fire done(false, "timeout") out from under the open prompt box.
	const auto guarded = GuardedDone(done);
	const auto session = &account->session();
	const auto lifetime = std::make_shared<rpl::lifetime>();
	*lifetime = session->api().cloudPassword().set(
		oldPassword,
		newPassword,
		hint,
		false,
		QString()
	) | rpl::on_error_done([=](const QString &type) {
		lifetime->destroy();
		if (type == u"PASSWORD_HASH_INVALID"_q && attemptsLeft > 0) {
			guarded.cancel();
			controller->show(Box([=](not_null<Ui::GenericBox*> box) {
				box->setWidth(st::accountsBoxWidth);
				box->setTitle(rpl::single(AccountLabel(account)));
				box->addRow(object_ptr<Ui::FlatLabel>(
					box,
					u"Сохранённая заметка не подошла (или её нет). "
						"Введите текущий пароль 2FA для этого аккаунта:"_q,
					st::boxLabel));
				const auto field = AddPasswordRow(
					box,
					rpl::single(u"Текущий пароль"_q));
				box->addButton(rpl::single(u"Сохранить"_q), [=] {
					const auto typed = field->getLastText();
					box->closeBox();
					ResolveOldPasswordAndSet(
						controller,
						account,
						typed,
						newPassword,
						hint,
						attemptsLeft - 1,
						done);
				});
				box->addButton(rpl::single(u"Пропустить аккаунт"_q), [=] {
					box->closeBox();
					done(false, u"пропущено (не удалось ввести старый пароль)"_q);
				});
			}), Ui::LayerOption::KeepOther);
		} else {
			guarded.done(false, type);
		}
	}, [=] {
		lifetime->destroy();
		account->setTwoFactorPassword(newPassword);
		guarded.done(true, QString());
	});
}

// ---- Parameter boxes: choose what to do, then hand off to RunBulkAction ----

void ShowBulkPhotoBox(
		not_null<Window::SessionController*> controller,
		Accounts accounts) {
	enum class Mode {
		AddOnly,
		Replace,
	};
	struct State {
		QStringList paths;
		std::shared_ptr<Ui::RadioenumGroup<Mode>> mode
			= std::make_shared<Ui::RadioenumGroup<Mode>>(Mode::AddOnly);
		QPointer<Ui::FlatLabel> chosen;
	};
	const auto state = std::make_shared<State>();

	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setWidth(st::accountsBoxWidth);
		box->setTitle(rpl::single(u"Фото — массово"_q));
		box->addRow(object_ptr<Ui::Radioenum<Mode>>(
			box,
			state->mode,
			Mode::AddOnly,
			u"Просто добавить новое"_q), st::ggtgChoiceRowPadding);
		box->addRow(object_ptr<Ui::Radioenum<Mode>>(
			box,
			state->mode,
			Mode::Replace,
			u"Удалить старое и добавить новое"_q), st::ggtgChoiceRowPadding);
		Ui::AddSkip(box->verticalLayout());
		const auto choose = box->addRow(object_ptr<Ui::SettingsButton>(
			box,
			rpl::single(u"Выбрать файлы..."_q)));
		state->chosen = box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			u"Файлы не выбраны"_q,
			st::boxLabel));
		Ui::AddDividerText(
			box->verticalLayout(),
			rpl::single(
				u"Можно выбрать несколько фото сразу - они будут "
					"распределены по очереди между выбранными аккаунтами "
					"(например, 3 фото на 5 аккаунтов: 1-2-3-1-2), а не "
					"поставлены всем одно и то же."_q));
		choose->setClickedCallback([=] {
			FileDialog::GetOpenPaths(
				box.get(),
				u"Выберите фото"_q,
				FileDialog::ImagesFilter(),
				[=](FileDialog::OpenResult &&result) {
					if (result.paths.isEmpty()) {
						return;
					}
					state->paths = result.paths;
					if (const auto label = state->chosen.data()) {
						label->setText((state->paths.size() == 1)
							? state->paths.front()
							: u"Выбрано файлов: "_q
								+ QString::number(state->paths.size()));
					}
				});
		});
		box->addButton(rpl::single(u"Начать"_q), [=] {
			if (state->paths.isEmpty()) {
				Ggtg::Notice(box->uiShow(), u"Сначала выберите файл"_q);
				return;
			}
			const auto replace = (state->mode->current() == Mode::Replace);
			const auto paths = state->paths;
			box->closeBox();
			RunBulkAction(controller, accounts, [=](
					not_null<Main::Account*> account,
					StepDone done) {
				// Round-robin: which photo an account gets only depends on
				// its own position in the selected list, not on send
				// order/timing - so every account gets a predictable,
				// even share even if some steps fail and skip ahead.
				const auto index = ranges::find(accounts, account)
					- begin(accounts);
				const auto &path = paths[index % paths.size()];
				StepChangePhoto(account, path, replace, done);
			});
		});
		Ggtg::AddBackButton(box);
	}));
}

void ShowBulkNameBox(
		not_null<Window::SessionController*> controller,
		Accounts accounts) {
	enum class Mode {
		SameForAll,
		PerAccount,
	};
	struct State {
		std::shared_ptr<Ui::RadioenumGroup<Mode>> mode
			= std::make_shared<Ui::RadioenumGroup<Mode>>(Mode::SameForAll);
		QPointer<Ui::InputField> first;
		QPointer<Ui::InputField> last;
		QPointer<Ui::InputField> list;
	};
	const auto state = std::make_shared<State>();

	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setWidth(st::accountsBoxWidth);
		box->setTitle(rpl::single(u"Имя и фамилия — массово"_q));
		box->addRow(object_ptr<Ui::Radioenum<Mode>>(
			box,
			state->mode,
			Mode::SameForAll,
			u"Одно имя и фамилия для всех"_q), st::ggtgChoiceRowPadding);
		state->first = box->addRow(object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			rpl::single(u"Имя"_q)));
		state->last = box->addRow(object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			rpl::single(u"Фамилия"_q)));
		Ui::AddSkip(box->verticalLayout());
		box->addRow(object_ptr<Ui::Radioenum<Mode>>(
			box,
			state->mode,
			Mode::PerAccount,
			u"Список построчно (Имя Фамилия), по одной строке на аккаунт, "
				"в порядке отображения ниже"_q), st::ggtgChoiceRowPadding);
		state->list = box->addRow(object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			Ui::InputField::Mode::MultiLine,
			rpl::single(QString())));
		Ui::AddSkip(box->verticalLayout());
		Ui::AddSubsectionTitle(
			box->verticalLayout(),
			rpl::single(u"Аккаунты ("_q + QString::number(accounts.size()) + ')'));
		auto namesList = QStringList();
		for (const auto &account : accounts) {
			namesList.push_back(AccountLabel(account));
		}
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			namesList.join('\n'),
			st::boxLabel));
		box->addButton(rpl::single(u"Начать"_q), [=] {
			auto names = std::vector<std::pair<QString, QString>>();
			if (state->mode->current() == Mode::SameForAll) {
				const auto first = state->first->getLastText().trimmed();
				const auto last = state->last->getLastText().trimmed();
				if (first.isEmpty()) {
					Ggtg::Notice(box->uiShow(), u"Введите имя"_q);
					return;
				}
				names.assign(accounts.size(), { first, last });
			} else {
				const auto lines = state->list->getLastText().split('\n');
				if (int(lines.size()) != int(accounts.size())) {
					Ggtg::Notice(box->uiShow(), 
						u"Число строк должно совпадать с числом "
							"выбранных аккаунтов"_q);
					return;
				}
				for (const auto &line : lines) {
					const auto trimmed = line.trimmed();
					const auto space = trimmed.indexOf(' ');
					names.push_back((space < 0)
						? std::pair{ trimmed, QString() }
						: std::pair{
							trimmed.left(space),
							trimmed.mid(space + 1).trimmed(),
						});
				}
			}
			box->closeBox();
			const auto index = std::make_shared<int>(0);
			RunBulkAction(controller, accounts, [=](
					not_null<Main::Account*> account,
					StepDone done) {
				const auto &[first, last] = names[(*index)++];
				StepUpdateProfile(account, first, last, std::nullopt, done);
			});
		});
		Ggtg::AddBackButton(box);
	}));
}

void ShowBulkBioBox(
		not_null<Window::SessionController*> controller,
		Accounts accounts) {
	enum class Mode {
		Delete,
		SameForAll,
		PerAccount,
	};
	struct State {
		std::shared_ptr<Ui::RadioenumGroup<Mode>> mode
			= std::make_shared<Ui::RadioenumGroup<Mode>>(Mode::SameForAll);
		QPointer<Ui::InputField> same;
		QPointer<Ui::InputField> list;
	};
	const auto state = std::make_shared<State>();

	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setWidth(st::accountsBoxWidth);
		box->setTitle(rpl::single(u"Описание — массово"_q));
		box->addRow(object_ptr<Ui::Radioenum<Mode>>(
			box, state->mode, Mode::Delete, u"Удалить описание"_q), st::ggtgChoiceRowPadding);
		box->addRow(object_ptr<Ui::Radioenum<Mode>>(
			box, state->mode, Mode::SameForAll, u"Один текст для всех"_q), st::ggtgChoiceRowPadding);
		state->same = box->addRow(object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			Ui::InputField::Mode::MultiLine,
			rpl::single(u"Описание"_q)));
		Ui::AddSkip(box->verticalLayout());
		box->addRow(object_ptr<Ui::Radioenum<Mode>>(
			box,
			state->mode,
			Mode::PerAccount,
			u"Список построчно, по одной строке на аккаунт, в порядке "
				"отображения ниже"_q), st::ggtgChoiceRowPadding);
		state->list = box->addRow(object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			Ui::InputField::Mode::MultiLine,
			rpl::single(QString())));
		Ui::AddSkip(box->verticalLayout());
		Ui::AddSubsectionTitle(
			box->verticalLayout(),
			rpl::single(u"Аккаунты ("_q + QString::number(accounts.size()) + ')'));
		auto namesList = QStringList();
		for (const auto &account : accounts) {
			namesList.push_back(AccountLabel(account));
		}
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			namesList.join('\n'),
			st::boxLabel));
		box->addButton(rpl::single(u"Начать"_q), [=] {
			auto texts = std::vector<QString>();
			switch (state->mode->current()) {
			case Mode::Delete:
				texts.assign(accounts.size(), QString());
				break;
			case Mode::SameForAll:
				texts.assign(accounts.size(), state->same->getLastText());
				break;
			case Mode::PerAccount: {
				const auto lines = state->list->getLastText().split('\n');
				if (int(lines.size()) != int(accounts.size())) {
					Ggtg::Notice(box->uiShow(), 
						u"Число строк должно совпадать с числом "
							"выбранных аккаунтов"_q);
					return;
				}
				for (const auto &line : lines) {
					texts.push_back(line);
				}
				break;
			}
			}
			box->closeBox();
			const auto index = std::make_shared<int>(0);
			RunBulkAction(controller, accounts, [=](
					not_null<Main::Account*> account,
					StepDone done) {
				StepUpdateProfile(
					account,
					std::nullopt,
					std::nullopt,
					texts[(*index)++],
					done);
			});
		});
		Ggtg::AddBackButton(box);
	}));
}

void ShowBulkUsernameBox(
		not_null<Window::SessionController*> controller,
		Accounts accounts) {
	enum class Mode {
		Delete,
		Set,
	};
	const auto mode = std::make_shared<Ui::RadioenumGroup<Mode>>(Mode::Set);

	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setWidth(st::accountsBoxWidth);
		box->setTitle(rpl::single(u"@username — массово"_q));
		box->addRow(object_ptr<Ui::Radioenum<Mode>>(
			box,
			mode,
			Mode::Set,
			u"Установить/заменить (формат: первая буква имени + первая "
				"буква фамилии + id аккаунта - он у каждого свой)"_q), st::ggtgChoiceRowPadding);
		box->addRow(object_ptr<Ui::Radioenum<Mode>>(
			box, mode, Mode::Delete, u"Удалить username"_q), st::ggtgChoiceRowPadding);
		box->addButton(rpl::single(u"Начать"_q), [=] {
			const auto remove = (mode->current() == Mode::Delete);
			box->closeBox();
			RunBulkAction(controller, accounts, [=](
					not_null<Main::Account*> account,
					StepDone done) {
				StepSetUsername(account, remove, done);
			});
		});
		Ggtg::AddBackButton(box);
	}));
}

// Purely local (Main::Account::setFolderTag persists it, no server request
// at all) - no anti-flood reason to run this through RunBulkAction's
// sequential 20-30s-paced queue, so it just applies instantly to everyone
// selected.
void ShowBulkMoveToFolderBox(
		not_null<Window::SessionController*> controller,
		Accounts accounts) {
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setWidth(st::accountsBoxWidth);
		box->setTitle(rpl::single(u"Переместить в папку — массово"_q));
		Ui::AddDividerText(
			box->verticalLayout(),
			rpl::single(
				u"Впишите имя папки (новой или уже существующей) - "
					"выбранные аккаунты переместятся в неё. Пустое имя "
					"уберёт их из папок."_q));
		const auto field = box->addRow(object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			rpl::single(u"Имя папки"_q)));
		box->setFocusCallback([=] { field->setFocusFast(); });
		const auto save = [=] {
			const auto folder = field->getLastText().trimmed();
			box->closeBox();
			for (const auto &account : accounts) {
				account->setFolderTag(folder);
			}
			Ggtg::Notice(controller->uiShow(), 
				u"Перемещено аккаунтов: "_q
					+ QString::number(accounts.size()));
		};
		field->submits() | rpl::on_next(save, field->lifetime());
		box->addButton(rpl::single(u"Сохранить"_q), save);
		Ggtg::AddBackButton(box);
	}));
}

void ShowBulkTerminateSessionsBox(
		not_null<Window::SessionController*> controller,
		Accounts accounts) {
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setWidth(st::accountsBoxWidth);
		box->setTitle(rpl::single(u"Деактивировать другие сессии"_q));
		Ui::AddDividerText(
			box->verticalLayout(),
			rpl::single(
				u"Завершит все остальные сеансы входа (телефон, веб, "
					"другие клиенты и т.п.) у выбранных аккаунтов. Сам "
					"этот аккаунт здесь, в GGTG, останется подключён, и "
					"резервные сессии из хранилища программы тоже не "
					"завершаются."_q));
		box->addButton(rpl::single(u"Деактивировать"_q), [=] {
			box->closeBox();
			RunBulkAction(controller, accounts, [=](
					not_null<Main::Account*> account,
					StepDone done) {
				StepTerminateOtherSessions(account, done);
			});
		});
		Ggtg::AddBackButton(box);
	}));
}

void StepCreateBackup(
		not_null<Main::Account*> account,
		StepDone done) {
	if (!Main::AccountIsLive(account)) {
		done(false, u"нет активной сессии"_q);
		return;
	}
	done = GuardedDone(done).done;
	CreateBackupSession(account, QString(), [=](QString error) {
		done(
			error.isEmpty(),
			error.isEmpty() ? u"резервная сессия создана"_q : error);
	});
}

void ShowBulkBackupBox(
		not_null<Window::SessionController*> controller,
		Accounts accounts) {
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"Создать резервную сессию"_q));
		Ui::AddDividerText(
			box->verticalLayout(),
			rpl::single(
				u"Для каждого выбранного аккаунта будет создана отдельная "
					"резервная сессия (новый вход, подтверждённый с "
					"работающей) и сохранена в программе. Она не запускается, "
					"пока не понадобится: если рабочая сессия погибнет, "
					"аккаунт можно восстановить из резервной.\n\n"
					"Это новый вход в аккаунт: Telegram покажет его в "
					"«Активных сеансах», а у только что созданной сессии "
					"сутки действуют ограничения. Если у аккаунта включён "
					"пароль 2FA, он должен быть сохранён в программе. "
					"Аккаунты обрабатываются по одному, с паузами - не "
					"создавайте резервные для сотен аккаунтов за раз."_q));
		box->addButton(rpl::single(u"Создать"_q), [=] {
			box->closeBox();
			RunBulkAction(controller, accounts, [=](
					not_null<Main::Account*> account,
					StepDone done) {
				StepCreateBackup(account, done);
			});
		});
		Ggtg::AddBackButton(box);
	}));
}

void ShowBulkSwapToBackupBox(
		not_null<Window::SessionController*> controller,
		Accounts accounts) {
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"Перейти на резервную сессию"_q));
		Ui::AddDividerText(
			box->verticalLayout(),
			rpl::single(
				u"Каждый выбранный аккаунт переключится на свою самую "
					"старую резервную сессию. Сессия, на которой он работает "
					"сейчас, не завершается - она сохраняется как резервная, "
					"так что ничего не теряется.\n\n"
					"Аккаунты без резервной будут пропущены (создать её - "
					"«Создать резервную сессию»). Пока идёт переключение, "
					"рассылки и другие процессы с этими аккаунтами должны "
					"быть остановлены."_q));
		box->addButton(rpl::single(u"Перейти"_q), [=] {
			// The accounts are replaced: nothing that holds them (a
			// campaign) may be running.
			if (!Core::GuardAllows(
					controller->uiShow(),
					Core::GuardedAction::RemoveAccount)) {
				return;
			}
			box->closeBox();
			RunBulkAction(controller, accounts, [=](
					not_null<Main::Account*> account,
					StepDone done) {
				StepSwapToBackup(account, done);
			});
		});
		Ggtg::AddBackButton(box);
	}));
}

void ShowBulkAntiTheftBox(
		not_null<Window::SessionController*> controller,
		Accounts accounts) {
	namespace Sessions = Core::GgtgSessions;
	const auto vault = Sessions::LoadVault();

	int needBackup = 0;
	int waitingCooldown = 0;
	int readyToProtect = 0;

	for (const auto &account : accounts) {
		if (!Main::AccountIsLive(account)) {
			continue;
		}
		const auto userId = QString::number(account->session().userId().bare);
		auto backups = std::vector<Sessions::StoredSession>();
		for (const auto &entry : Sessions::StoredForUser(vault, userId)) {
			if ((entry.role == u"backup"_q || entry.role == u"anti_theft_backup"_q)
				&& QDir(Sessions::VaultFolder(entry) + u"/tdata"_q).exists()
				&& entry.state != u"revoked"_q) {
				backups.push_back(entry);
			}
		}
		if (backups.empty()) {
			++needBackup;
		} else {
			bool anyTrusted = false;
			for (const auto &b : backups) {
				if (Sessions::IsSessionTrusted(b)) {
					anyTrusted = true;
					break;
				}
			}
			if (anyTrusted) {
				++readyToProtect;
			} else {
				++waitingCooldown;
			}
		}
	}

	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"Защита от угона (Анти-угон)"_q));
		box->setWidth(style::ConvertScale(480));

		auto infoText = u"Защита купленных аккаунтов (TData / Сессия+JSON) от доступа продавца и третьих лиц.\n\n"
			u"Как работает механизм защиты:\n"
			u"1. Программа создаёт чистую резервную сессию со своим ключом авторизации.\n"
			u"2. По правилам безопасности Telegram новая сессия должна отлежаться 24 часа. Сброс чужих сессий заблокирован сервером Telegram на 24 часа (ошибка FRESH_RESET_AUTHORISATION_FORBIDDEN).\n"
			u"3. Спустя 24 часа программа переключает аккаунт на выдержанную сессию, завершает сессию продавца и все чужие сессии, удаляет старую сессию и создаёт свежий бэкап.\n\n"
			u"Статус выбранных аккаунтов (%1):\n"
			u"• Без резервной сессии: %2 (требуется создать сессию и запустить таймер 24ч)\n"
			u"• На 24-часовой отлёжке: %3 (ожидание доверия Telegram)\n"
			u"• Готовы к полной защите: %4 (выдержаны > 24ч, можно сбросить продавца)"_q
			.arg(accounts.size())
			.arg(needBackup)
			.arg(waitingCooldown)
			.arg(readyToProtect);

		Ui::AddDividerText(box->verticalLayout(), rpl::single(infoText));

		if (readyToProtect > 0) {
			box->addButton(rpl::single(u"Защитить готовые (%1)"_q.arg(readyToProtect)), [=] {
				box->closeBox();
				RunBulkAction(controller, accounts, [=](not_null<Main::Account*> account, StepDone done) {
					StepAntiTheftProtection(account, done);
				});
			});
		}
		if (needBackup > 0) {
			box->addButton(rpl::single(u"Создать сессии и запустить таймер (%1)"_q.arg(needBackup)), [=] {
				box->closeBox();
				RunBulkAction(controller, accounts, [=](not_null<Main::Account*> account, StepDone done) {
					StepAntiTheftProtection(account, done);
				});
			});
		}
		if (readyToProtect == 0 && needBackup == 0 && waitingCooldown > 0) {
			box->addButton(rpl::single(u"Проверить статус отлёжки"_q), [=] {
				box->closeBox();
				RunBulkAction(controller, accounts, [=](not_null<Main::Account*> account, StepDone done) {
					StepAntiTheftProtection(account, done);
				});
			});
		}
		Ggtg::AddBackButton(box);
	}));
}

void ShowAccountAntiTheftBox(
		std::shared_ptr<Ui::Show> show,
		not_null<Main::Account*> account) {
	namespace Sessions = Core::GgtgSessions;
	if (!Main::AccountIsLive(account)) {
		Ggtg::Notice(show, u"Аккаунт не в сети"_q);
		return;
	}
	const auto userId = QString::number(account->session().userId().bare);
	const auto vault = Sessions::LoadVault();
	auto backups = std::vector<Sessions::StoredSession>();
	for (const auto &entry : Sessions::StoredForUser(vault, userId)) {
		if ((entry.role == u"backup"_q || entry.role == u"anti_theft_backup"_q)
			&& QDir(Sessions::VaultFolder(entry) + u"/tdata"_q).exists()
			&& entry.state != u"revoked"_q) {
			backups.push_back(entry);
		}
	}
	std::sort(backups.begin(), backups.end(), [](const auto &a, const auto &b) {
		return a.createdAt < b.createdAt;
	});

	show->showBox(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"Защита от угона (Анти-угон)"_q));
		box->setWidth(style::ConvertScale(460));

		const auto phone = account->session().user()->phone();
		auto desc = u"Аккаунт: +%1\n\n"
			u"Защита купленных аккаунтов исключает риск возврата доступа продавцом или сторонними лицами.\n\n"
			u"Механизм защиты:\n"
			u"1. Создание чистой резервной сессии программы.\n"
			u"2. 24-часовая отлёжка (требование сервера Telegram для разрешения сброса чужих сессий).\n"
			u"3. Переключение на выдержанную сессию, завершение сессии продавца, удаление её из программы и создание свежего бэкапа.\n\n"_q
			.arg(phone);

		if (backups.empty()) {
			desc += u"Статус: Резервная сессия ещё не создана. Нажмите «Создать сессию», чтобы запустить 24-часовой таймер отлёжки."_q;
		} else {
			const auto &b = backups.front();
			const auto createdStr = QDateTime::fromSecsSinceEpoch(b.createdAt).toString(u"dd.MM.yyyy hh:mm"_q);
			const auto countdown = Sessions::TrustCountdownString(b);
			desc += u"Резервная сессия: создана %1\nСтатус отлёжки: %2\n\n"_q
				.arg(createdStr)
				.arg(countdown);

			if (Sessions::IsSessionTrusted(b)) {
				desc += u"Сессия набрала необходимый возраст (> 24 часов) и готова к полной защите аккаунта."_q;
			} else {
				desc += u"Telegram вернёт ошибку FRESH_RESET_AUTHORISATION_FORBIDDEN при попытке сброса других сессий до истечения 24 часов. Дождитесь окончания таймера."_q;
			}
		}

		Ui::AddDividerText(box->verticalLayout(), rpl::single(desc));

		if (backups.empty()) {
			box->addButton(rpl::single(u"Создать сессию и запустить таймер"_q), [=] {
				box->closeBox();
				Ggtg::Notice(show, u"Создаём резервную сессию..."_q);
				CreateBackupSessionWith(
					account,
					u"резервная (анти-угон)"_q,
					MakeSessionPasswordPrompt(account),
					[=](QString err, QString) {
						if (!err.isEmpty()) {
							Ggtg::Notice(show, u"Ошибка: "_q + err);
						} else {
							Ggtg::Notice(show, u"Резервная сессия создана. Таймер 24ч отлёжки запущен."_q);
						}
					});
			});
		} else if (Sessions::IsSessionTrusted(backups.front())) {
			box->addButton(rpl::single(u"Защитить аккаунт (Перейти и сбросить)"_q), [=] {
				box->closeBox();
				Ggtg::Notice(show, u"Выполняем защиту аккаунта..."_q);
				StepAntiTheftProtection(account, [=](bool ok, QString text) {
					Ggtg::Notice(show, ok ? text : (u"Ошибка: "_q + text));
				});
			});
		}

		box->addButton(tr::lng_close(), [=] { box->closeBox(); });
	}));
}

void ShowBulkPasswordBox(
		not_null<Window::SessionController*> controller,
		Accounts accounts) {
	enum class Mode {
		SetOrChange,
		Delete,
	};
	struct State {
		std::shared_ptr<Ui::RadioenumGroup<Mode>> mode
			= std::make_shared<Ui::RadioenumGroup<Mode>>(Mode::SetOrChange);
		QPointer<Ui::PasswordInput> password;
		QPointer<Ui::InputField> hint;
	};
	const auto state = std::make_shared<State>();

	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setWidth(st::accountsBoxWidth);
		box->setTitle(rpl::single(u"2FA пароль — массово"_q));
		Ui::AddDividerText(
			box->verticalLayout(),
			rpl::single(
				u"Для смены нужен текущий пароль аккаунта. Сначала "
					"пробуем сохранённую в контекстном меню аккаунта "
					"заметку \"2FA password\"; если она не подходит - "
					"спросим пароль вручную, а если вы его не знаете - "
					"этот аккаунт просто пропускаем."_q));
		Ui::AddSkip(box->verticalLayout());
		box->addRow(object_ptr<Ui::Radioenum<Mode>>(
			box,
			state->mode,
			Mode::SetOrChange,
			u"Установить/сменить пароль"_q), st::ggtgChoiceRowPadding);
		state->password = AddPasswordRow(box, rpl::single(u"Новый пароль"_q));
		state->hint = box->addRow(object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			rpl::single(u"Подсказка (необязательно)"_q)));
		box->addRow(object_ptr<Ui::Radioenum<Mode>>(
			box,
			state->mode,
			Mode::Delete,
			u"Удалить пароль (выключить 2FA)"_q), st::ggtgChoiceRowPadding);
		box->addButton(rpl::single(u"Начать"_q), [=] {
			const auto deleting = (state->mode->current() == Mode::Delete);
			const auto newPassword = deleting
				? QString()
				: state->password->getLastText();
			if (!deleting && newPassword.isEmpty()) {
				Ggtg::Notice(box->uiShow(), u"Введите новый пароль"_q);
				return;
			}
			const auto hint = state->hint->getLastText();
			box->closeBox();
			RunBulkAction(controller, accounts, [=](
					not_null<Main::Account*> account,
					StepDone done) {
				ResolveOldPasswordAndSet(
					controller,
					account,
					account->twoFactorPassword(),
					newPassword,
					hint,
					kPasswordAttempts,
					done);
			});
		});
		Ggtg::AddBackButton(box);
	}));
}

} // namespace

[[nodiscard]] QString FormatScrapedAt(qint64 unixSeconds) {
	return QDateTime::fromSecsSinceEpoch(unixSeconds).toString(
		u"dd.MM.yyyy hh:mm"_q);
}

[[nodiscard]] object_ptr<Ui::BoxContent> MakeScrapedGroupsDbBox(
	std::shared_ptr<Ui::Show> show,
	BasesActions actions);

// The list sort in "База" (0 newest first, 1 by title, 2 by size, 3 by how
// many are left to write) - kept here so it survives the box being rebuilt
// after every save.
[[nodiscard]] int &BaseSortMode() {
	static auto result = 0;
	return result;
}

// Past this many usernames the in-box text field is too slow to be usable
// (and nobody scrolls through that many lines anyway): such a base is managed
// with the buttons instead - rename, export, and the operations below.
constexpr auto kEditorLimit = 3000;

[[nodiscard]] QString BaseTitleById(uint64 id) {
	for (const auto &meta : Data::LoadScrapedGroupsIndex()) {
		if (meta.id == id) {
			return meta.title;
		}
	}
	return QString();
}

// Lets the user pick other users bases (never `excludeId`): one tap on a row
// for a single choice, or checkboxes plus "Готово" for several.
void ShowBasePicker(
		std::shared_ptr<Ui::Show> show,
		const QString &title,
		uint64 excludeId,
		bool multi,
		Fn<void(std::vector<uint64>)> chosen) {
	struct Item {
		uint64 id = 0;
		QPointer<Ui::Checkbox> checkbox;
	};
	const auto items = std::make_shared<std::vector<Item>>();
	show->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setWidth(st::accountsBoxWidth);
		box->setTitle(rpl::single(title));
		auto any = false;
		for (const auto &meta : Data::LoadScrapedGroupsIndex()) {
			if (meta.kind != Data::BaseKind::Users || meta.id == excludeId) {
				continue;
			}
			any = true;
			const auto label = meta.title + u" ("_q
				+ QString::number(meta.count) + ')';
			if (multi) {
				items->push_back({
					meta.id,
					box->addRow(object_ptr<Ui::Checkbox>(box, label, false), st::ggtgChoiceRowPadding),
				});
				if (const auto check = items->back().checkbox.data()) {
					check->setAllowTextLines(0); // wraps, never "..."
				}
			} else {
				const auto id = meta.id;
				box->addRow(object_ptr<Ggtg::TextRow>(
					box, rpl::single(label))
				)->setClickedCallback([=] {
					box->closeBox();
					chosen(std::vector<uint64>{ id });
				});
			}
		}
		if (!any) {
			box->addRow(object_ptr<Ui::FlatLabel>(
				box,
				u"Других списков пользователей пока нет."_q,
				st::boxLabel));
		}
		if (multi) {
			box->addButton(rpl::single(u"Готово"_q), [=] {
				auto ids = std::vector<uint64>();
				for (const auto &item : *items) {
					if (item.checkbox && item.checkbox->checked()) {
						ids.push_back(item.id);
					}
				}
				if (ids.empty()) {
					Ggtg::Notice(box->uiShow(), 
						u"Выберите хотя бы один список"_q);
					return;
				}
				box->closeBox();
				chosen(std::move(ids));
			});
		}
		box->addButton(rpl::single(u"Отмена"_q), [=] { box->closeBox(); });
	}));
}

void ShowNumberPrompt(
		std::shared_ptr<Ui::Show> show,
		const QString &title,
		const QString &label,
		const QString &initial,
		Fn<void(int)> chosen) {
	const auto field = std::make_shared<QPointer<Ui::InputField>>();
	show->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setWidth(st::accountsBoxWidth);
		box->setTitle(rpl::single(title));
		*field = box->addRow(object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			rpl::single(label),
			initial));
		box->setFocusCallback([=] {
			if (const auto input = field->data()) {
				input->setFocusFast();
			}
		});
		box->addButton(rpl::single(u"Готово"_q), [=] {
			auto ok = false;
			const auto value = field->data()
				? field->data()->getLastText().trimmed().toInt(&ok)
				: 0;
			if (!ok || value <= 0) {
				Ggtg::Notice(box->uiShow(), u"Введите число больше нуля"_q);
				return;
			}
			box->closeBox();
			chosen(value);
		});
		box->addButton(rpl::single(u"Отмена"_q), [=] { box->closeBox(); });
	}));
}

// Opens a username exactly the way clicking a real @mention in a chat
// does (see MentionClickHandler::onClick in core/click_handler_types.cpp,
// the same showPeerByLink/ResolveType::Mention call) - resolves it if not
// already known locally, then navigates there, so from here on it's a
// completely normal chat: send messages, see their profile, etc. These
// boxes only ever get a Ui::Show (see ShowEditBaseBox's own comment on
// why), not a SessionController, so the active window's own controller is
// found the same way that click handler falls back to one when it isn't
// handed one directly.
void OpenUsernamePeer(
		const QString &rawUsername,
		std::shared_ptr<Ui::Show> show = nullptr,
		bool openProfile = false) {
	auto username = rawUsername.trimmed();
	if (username.startsWith('@')) {
		username = username.mid(1);
	} else {
		static const auto linkExpression = QRegularExpression(
			u"^(?:https?://)?t(?:elegram)?\\.me/([A-Za-z0-9_]{4,32})/?$"_q);
		if (const auto match = linkExpression.match(username);
				match.hasMatch()) {
			username = match.captured(1);
		}
	}
	if (username.isEmpty()) {
		return;
	}

	const auto notFound = [=] {
		if (show) {
			show->showToast(u"@"_q + username + u" - не найден"_q);
		}
	};

	const auto openNow = [=] {
		const auto controller = Core::App().activeWindow()
			? Core::App().activeWindow()->sessionController()
			: nullptr;
		if (!controller) {
			return;
		}
		if (openProfile) {
			if (const auto peer = controller->session().data().peerByUsername(username)) {
				controller->showPeerInfo(peer);
			} else {
				controller->showPeerByLink(Window::PeerByLinkInfo{
					.usernameOrId = username,
					.resolveType = Window::ResolveType::Profile,
				});
			}
		} else {
			controller->showPeerByLink(Window::PeerByLinkInfo{
				.usernameOrId = username,
				.resolveType = Window::ResolveType::Default,
			});
		}
		const auto bringForward = [] {
			if (const auto window = Core::App().activeWindow()) {
				window->activate();
			}
		};
		bringForward();
		base::call_delayed(1200, &Core::App(), bringForward);
	};

	if (const auto record = Data::FindSmsSendRecord(username);
			show && record && record->status == Data::SmsSendStatus::NotFound) {
		notFound();
		return;
	}

	const auto initialController = Core::App().activeWindow()
		? Core::App().activeWindow()->sessionController()
		: nullptr;
	if (!initialController) {
		return;
	}
	const auto session = &initialController->session();
	if (!show || session->data().peerByUsername(username)) {
		openNow();
		return;
	}

	const auto weakController = base::make_weak(initialController);
	const auto api = static_cast<MTP::Sender*>(&session->api());
	api->request(MTPcontacts_ResolveUsername(
		MTP_flags(MTPcontacts_ResolveUsername::Flags(0)),
		MTP_string(username),
		MTP_string(QString())
	)).done([=](const MTPcontacts_ResolvedPeer &result) {
		const auto strongController = weakController.get();
		if (!strongController) {
			openNow();
			return;
		}
		result.match([&](const MTPDcontacts_resolvedPeer &data) {
			strongController->session().data().processUsers(data.vusers());
			strongController->session().data().processChats(data.vchats());
			if (peerFromMTP(data.vpeer())) {
				openNow();
			} else {
				notFound();
			}
		});
	}).fail([=](const MTP::Error &error) {
		const auto type = error.type();
		if (type.startsWith(u"USERNAME_"_q)
			|| type == u"PEER_ID_INVALID"_q) {
			notFound();
		} else {
			// Flood or a network hiccup: let the normal lookup try.
			openNow();
		}
	}).send();
}

[[nodiscard]] QString SentStatusText(Data::SmsSendStatus status) {
	switch (status) {
	case Data::SmsSendStatus::SpamAcc: return u"аккаунт в спам-блоке"_q;
	case Data::SmsSendStatus::NotFound: return u"не найден"_q;
	case Data::SmsSendStatus::Restricted: return u"получатель закрыл ЛС"_q;
	case Data::SmsSendStatus::PeerLimit: return u"Telegram не дал написать"_q;
	case Data::SmsSendStatus::AccountFail: return u"аккаунт не смог написать"_q;
	case Data::SmsSendStatus::SuspectedFake: return u"предположительный фейк"_q;
	case Data::SmsSendStatus::LongAgo: return u"был(а) в сети давно"_q;
	case Data::SmsSendStatus::Sent: break;
	}
	return u"OK"_q;
}

[[nodiscard]] QString FormatLastseenTime(qint64 lastseen) {
	if (lastseen <= 0) {
		return QString();
	}
	const auto now = base::unixtime::now();
	if (lastseen > now + 300) {
		return u"в сети"_q;
	}
	const auto diff = now - lastseen;
	if (diff <= 300 && diff >= -60) {
		return u"в сети"_q;
	} else if (diff < 3600) {
		const auto mins = std::max(1, int(diff / 60));
		return QString::number(mins) + u" мин. назад"_q;
	} else if (diff < 86400) {
		const auto hours = int(diff / 3600);
		return QString::number(hours) + u" ч. назад"_q;
	} else if (diff < 86400 * 2) {
		return u"вчера"_q;
	} else if (diff < 86400 * 7) {
		const auto days = int(diff / 86400);
		return QString::number(days) + u" дн. назад"_q;
	} else if (diff < 86400 * 30) {
		return u"был(а) в этом месяце"_q;
	}
	return u"был(а) давно"_q;
}

// One line of ShowClickableUsernamesBox: avatar on left (photo or initials circle),
// semibold name on top, (@username · online/last seen · marks) below,
// Telegram group members list style.
class UsernameRow final : public Ui::AbstractButton {
public:
	UsernameRow(
		not_null<QWidget*> parent,
		const QString &username,
		rpl::producer<QString> details,
		const QString &customTitle = QString())
	: AbstractButton(parent.get())
	, _username(username.trimmed().remove('@'))
	, _customTitle(customTitle.contains('\n') ? customTitle.section('\n', 0, 0) : customTitle)
	, _customSubtitle(customTitle.contains('\n') ? customTitle.section('\n', 1) : QString())
	, _meta(Data::LookupUserMeta(_username)) {
		if (const auto session = Core::App().activeAccount().maybeSession()) {
			if (const auto peer = session->data().peerByUsername(_username)) {
				setPeer(peer);
			}
		}
		if (_meta.lastseenText.isEmpty() && _meta.lastseen > 0) {
			_meta.lastseenText = FormatLastseenTime(_meta.lastseen);
		}
		std::move(details) | rpl::on_next([=](const QString &value) {
			_details = value.trimmed();
			update();
			_changed.fire({});
		}, lifetime());
		setCursor(style::cur_pointer);
	}

	void setPeer(not_null<PeerData*> peer) {
		_peer = peer;
		_peerSession = &peer->session();
		_peerLifetime.destroy();
		_peer->loadUserpic();
		_peer->session().downloaderTaskFinished(
		) | rpl::on_next([=] {
			update();
		}, _peerLifetime);

		if (const auto user = peer->asUser()) {
			const auto now = base::unixtime::now();
			const auto text = Data::OnlineText(user, now);
			if (!text.isEmpty()) {
				_meta.lastseenText = text;
				_meta.lastseen = Data::SortByOnlineValue(user, now);
			}
			if (_meta.name.isEmpty()) {
				_meta.name = peer->name();
			}
			if (!_meta.id) {
				_meta.id = peer->id.value;
			}
		} else {
			if (_meta.name.isEmpty()) {
				_meta.name = peer->name();
			}
			if (!_meta.id) {
				_meta.id = peer->id.value;
			}
		}
		update();
		_changed.fire({});
	}

	[[nodiscard]] bool needsResolution() const {
		const auto currentSession = Core::App().activeAccount().maybeSession();
		return !_peer || (_peerSession != currentSession);
	}

	[[nodiscard]] rpl::producer<> changed() const {
		return _changed.events();
	}

	[[nodiscard]] int heightFor(int width) const {
		return style::ConvertScale(64);
	}

	void setAvailableWidth(int available) {
		_availableWidth = available;
		update();
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		auto p = Painter(this);
		if (isOver()) {
			auto fill = st::windowBoldFg->c;
			fill.setAlphaF(0.06);
			p.fillRect(rect(), fill);
		}
		const auto photoSize = style::ConvertScale(42);
		const auto photoLeft = st::boxRowPadding.left();
		const auto photoTop = (height() - photoSize) / 2;

		const auto displayName = !_customTitle.isEmpty()
			? _customTitle
			: (!_meta.name.isEmpty()
				? _meta.name
				: ('@' + _username));

		const auto currentSession = Core::App().activeAccount().maybeSession();
		const auto validPeer = (_peer && currentSession && (_peerSession == currentSession))
			? _peer
			: nullptr;

		if (validPeer) {
			validPeer->paintUserpicLeft(
				p,
				_userpicView,
				photoLeft,
				photoTop,
				width(),
				photoSize);
		} else {
			const auto idForColor = _meta.id
				? _meta.id
				: uint64(qHash(_username));
			Ui::EmptyUserpic(
				Ui::EmptyUserpic::UserpicColor(
					Ui::EmptyUserpic::ColorIndex(idForColor)),
				displayName
			).paintCircle(p, photoLeft, photoTop, width(), photoSize);
		}

		const auto textLeft = photoLeft + photoSize + style::ConvertScale(12);
		const auto avail = std::max(
			(_availableWidth > 0 ? _availableWidth : width()) - textLeft - style::ConvertScale(8),
			style::ConvertScale(40));

		const auto line1Y = style::ConvertScale(6);
		const auto line2Y = line1Y + st::semiboldFont->height;
		const auto line3Y = line2Y + st::normalFont->height;

		// Line 1: Title / Name
		p.setFont(st::semiboldFont);
		p.setPen(st::windowBoldFg);
		p.drawTextLeft(
			textLeft,
			line1Y,
			avail,
			st::semiboldFont->elided(displayName, avail));

		// Line 2: Username (@handle, link or ID)
		p.setFont(st::normalFont);
		p.setPen(st::windowSubTextFg);
		auto handleText = (_username.startsWith(u"https://"_q) || _username.startsWith(u"t.me/"_q))
			? _username
			: (!_username.isEmpty() ? ('@' + _username) : QString());
		if (handleText.compare(displayName, Qt::CaseInsensitive) == 0 && _meta.id != 0) {
			handleText = u"ID: "_q + QString::number(_meta.id);
		}
		if (!handleText.isEmpty()) {
			p.drawTextLeft(
				textLeft,
				line2Y,
				avail,
				st::normalFont->elided(handleText, avail));
		}

		// Line 3: Online (for users) or Members count (for groups) + marks
		const auto isOnline = (validPeer && validPeer->asUser() && Data::OnlineTextActive(validPeer->asUser(), base::unixtime::now()))
			|| _meta.lastseenText.compare(u"в сети"_q, Qt::CaseInsensitive) == 0
			|| _meta.lastseenText.compare(u"online"_q, Qt::CaseInsensitive) == 0;

		auto statusText = !_customSubtitle.isEmpty()
			? _customSubtitle
			: (!_meta.lastseenText.isEmpty()
				? _meta.lastseenText
				: (_meta.lastseen > 0 ? FormatLastseenTime(_meta.lastseen) : QString()));

		if (statusText.isEmpty() && validPeer) {
			if (const auto channel = validPeer->asChannel()) {
				if (channel->membersCount() > 1) {
					statusText = QLocale().toString(channel->membersCount()) + u" уч."_q;
				}
			}
		}

		int curX = textLeft;
		const auto sep = u"  ·  "_q;
		const auto sepWidth = st::normalFont->width(sep);

		if (!statusText.isEmpty()) {
			p.setPen(isOnline ? st::windowActiveTextFg : st::windowSubTextFg);
			const auto maxW = std::max(0, avail - (curX - textLeft));
			const auto elided = st::normalFont->elided(statusText, maxW);
			p.drawTextLeft(curX, line3Y, maxW, elided);
			curX += st::normalFont->width(elided);
			if (!_details.isEmpty() && curX < textLeft + avail) {
				p.setPen(st::windowSubTextFg);
				p.drawTextLeft(curX, line3Y, std::max(0, avail - (curX - textLeft)), sep);
				curX += sepWidth;
			}
		}

		if (!_details.isEmpty() && curX < textLeft + avail) {
			p.setPen((_details == u"✓ OK"_q) ? st::boxTextFgGood : st::windowSubTextFg);
			const auto maxW = std::max(0, avail - (curX - textLeft));
			p.drawTextLeft(curX, line3Y, maxW, st::normalFont->elided(_details, maxW));
		}
	}

private:
	const QString _username;
	const QString _customTitle;
	const QString _customSubtitle;
	Data::UserMeta _meta;
	PeerData *_peer = nullptr;
	Main::Session *_peerSession = nullptr;
	rpl::lifetime _peerLifetime;
	Ui::PeerUserpicView _userpicView;
	QString _details;
	int _availableWidth = -1;
	rpl::event_stream<> _changed;
};

// The order of the rows in the clickable list.
enum class ListOrder {
	Base, // as the base keeps them
	Name, // by the person's name
	Username, // by @username
	Id, // by user ID
	Online, // by online / last seen date
};

// The usernames in the order asked for, the same order kept among equals.
// With `sentLast` the ones already written to go to the very end (in the
// same order), so they do not stand in the way.
[[nodiscard]] QStringList OrderedUsernames(
		const QStringList &usernames,
		ListOrder order,
		bool sentLast) {
	struct Row {
		QString username;
		int bucket = 0;
		QString key;
		uint64 id = 0;
		qint64 lastseen = 0;
		bool sent = false;
		bool named = false;
	};
	auto rows = std::vector<Row>();
	rows.reserve(usernames.size());
	for (const auto &username : usernames) {
		auto row = Row{ .username = username };
		const auto record = sentLast
			? Data::FindSmsSendRecord(username)
			: std::nullopt;
		row.sent = record && (record->status == Data::SmsSendStatus::Sent);
		const auto meta = Data::LookupUserMeta(username);
		row.id = meta.id;
		row.lastseen = meta.lastseen;
		const auto bare = username.trimmed().remove('@');
		const auto name = meta.name;
		row.named = !name.isEmpty();
		if (order == ListOrder::Name) {
			const auto key = Core::GgtgSort::SortKey(name.isEmpty() ? bare : name);
			row.bucket = key.first;
			row.key = key.second;
		}
		rows.push_back(std::move(row));
	}
	std::stable_sort(rows.begin(), rows.end(), [=](
			const Row &a,
			const Row &b) {
		if (a.sent != b.sent) {
			return b.sent;
		} else if (order == ListOrder::Base) {
			return false;
		} else if (order == ListOrder::Name) {
			if (a.named != b.named) {
				return a.named;
			} else if (a.bucket != b.bucket) {
				return a.bucket < b.bucket;
			}
			return a.key < b.key;
		} else if (order == ListOrder::Username) {
			return a.username.compare(b.username, Qt::CaseInsensitive) < 0;
		} else if (order == ListOrder::Id) {
			const auto aHas = (a.id != 0);
			const auto bHas = (b.id != 0);
			if (aHas != bHas) {
				return aHas;
			}
			return a.id < b.id;
		} else if (order == ListOrder::Online) {
			if (a.lastseen != b.lastseen) {
				return a.lastseen > b.lastseen;
			}
			return a.username.compare(b.username, Qt::CaseInsensitive) < 0;
		}
		return false;
	});
	auto result = QStringList();
	result.reserve(rows.size());
	for (const auto &row : rows) {
		result.push_back(row.username);
	}
	return result;
}

// A read-only, click-to-open view of a base's usernames - styled like
// group members with photo/initials avatar, bold name, online status and
// progressive chunked loading.
void ShowClickableUsernamesBox(
		std::shared_ptr<Ui::Show> show,
		const QString &title,
		const QStringList &usernames,
		Fn<void(const QString &username)> remove = nullptr,
		Fn<QString(const QString &username)> extraMark = nullptr,
		Fn<QString(const QString &username)> displayName = nullptr,
		Fn<void(const QString &username)> download = nullptr,
		Fn<void(const QString &username)> viaSquidward = nullptr,
		Fn<bool(const QString &username)> isClosed = nullptr,
		Fn<void(const QString &username, Fn<void()> done)> move = nullptr) {
	show->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setWidth(style::ConvertScale(580));
		box->setTitle(rpl::single(title));
		if (usernames.isEmpty()) {
			box->addRow(object_ptr<Ui::FlatLabel>(
				box, u"Список пуст."_q, st::boxLabel));
		} else {
			// The list stays open when the account is switched
			Ggtg::KeepOpenOnAccountSwitch(box);

			const auto order = std::make_shared<ListOrder>(ListOrder::Base);
			const auto rebuild = std::make_shared<Fn<void()>>();
			const auto controls = box->addRow(
				object_ptr<Ui::RpWidget>(box));

			const auto sortTitles = std::map<ListOrder, QString>{
				{ ListOrder::Base, u"Как в базе"_q },
				{ ListOrder::Name, u"По имени"_q },
				{ ListOrder::Username, u"По юзернейму"_q },
				{ ListOrder::Id, u"По ID"_q },
				{ ListOrder::Online, u"По дате онлайна"_q },
			};

			const auto isGroupList = (download != nullptr) || (viaSquidward != nullptr);
			const auto unitWord = [=](int count) {
				const auto mod10 = count % 10;
				const auto mod100 = count % 100;
				if (isGroupList) {
					if (mod100 >= 11 && mod100 <= 19) {
						return u" групп"_q;
					} else if (mod10 == 1) {
						return u" группа"_q;
					} else if (mod10 >= 2 && mod10 <= 4) {
						return u" группы"_q;
					} else {
						return u" групп"_q;
					}
				} else {
					if (mod100 >= 11 && mod100 <= 19) {
						return u" участников"_q;
					} else if (mod10 == 1) {
						return u" участник"_q;
					} else if (mod10 >= 2 && mod10 <= 4) {
						return u" участника"_q;
					} else {
						return u" участников"_q;
					}
				}
			};

			const auto sortBtn = Ui::CreateChild<Ui::LinkButton>(
				controls,
				QString());
			const auto countLabel = Ui::CreateChild<Ui::FlatLabel>(
				controls,
				QString::number(usernames.size()) + unitWord(usernames.size()),
				st::boxDividerLabel);

			const auto updateSortBtn = [=] {
				sortBtn->setText(u"≡  Сортировка: "_q + sortTitles.at(*order) + u" ▾"_q);
			};
			updateSortBtn();

			const auto relayoutControls = [=] {
				sortBtn->moveToLeft(0, 0);
				countLabel->moveToRight(0, (sortBtn->height() - countLabel->height()) / 2);
				controls->resize(controls->width(), sortBtn->height());
			};
			controls->widthValue() | rpl::on_next([=](int) {
				relayoutControls();
			}, controls->lifetime());

			const auto sortMenu = box->lifetime().make_state<base::unique_qptr<Ui::PopupMenu>>();
			sortBtn->setClickedCallback([=] {
				*sortMenu = base::make_unique_q<Ui::PopupMenu>(box, st::popupMenuWithIcons);
				const auto choices = std::vector<ListOrder>{
					ListOrder::Base,
					ListOrder::Name,
					ListOrder::Username,
					ListOrder::Id,
					ListOrder::Online,
				};
				for (const auto choice : choices) {
					const auto isCurrent = (*order == choice);
					const auto label = (isCurrent ? u"● "_q : u"   "_q) + sortTitles.at(choice);
					(*sortMenu)->addAction(label, [=] {
						if (*order != choice) {
							*order = choice;
							updateSortBtn();
							relayoutControls();
							(*rebuild)();
						}
					}, &st::menuIconReorder);
				}
				(*sortMenu)->popup(sortBtn->mapToGlobal(QPoint(0, sortBtn->height())));
			});

			const auto searchField = box->addRow(
				object_ptr<Ui::InputField>(
					box,
					st::defaultInputField,
					rpl::single(u"Поиск по имени, @username или ID..."_q)),
				st::boxRowPadding);
			const auto searchQuery = std::make_shared<QString>();
			searchField->changes() | rpl::on_next([=] {
				*searchQuery = searchField->getLastText();
				(*rebuild)();
			}, searchField->lifetime());

			if (remove || move) {
				box->addRow(
					object_ptr<Ui::FlatLabel>(
						box,
						u"Правая кнопка мыши по строке - ещё действия: "
							"перенести в другую базу, скопировать, вернуть "
							"в очередь."_q,
						st::boxDividerLabel),
					st::boxRowPadding);
			}
			const auto list = box->addRow(
				object_ptr<Ui::VerticalLayout>(box));

			const auto allUsernames = std::make_shared<QStringList>();
			const auto renderedCount = std::make_shared<int>(0);
			constexpr int kChunkSize = 50;

			const auto resolveQueue = std::make_shared<std::deque<std::pair<QString, QPointer<UsernameRow>>>>();
			const auto resolveActive = std::make_shared<bool>(false);
			const auto resolveTimer = std::make_shared<base::Timer>();
			const auto floodCooldownUntil = std::make_shared<crl::time>(0);

			const auto stepResolve = std::make_shared<Fn<void()>>();
			*stepResolve = [=] {
				if (resolveQueue->empty() || *resolveActive) {
					return;
				}
				const auto now = crl::now();
				if (*floodCooldownUntil > now) {
					resolveTimer->callOnce(int(*floodCooldownUntil - now));
					return;
				}
				const auto session = Core::App().activeAccount().maybeSession();
				if (!session) {
					return;
				}
				auto [username, row] = resolveQueue->front();
				resolveQueue->pop_front();
				if (!row) {
					if (!resolveQueue->empty()) {
						resolveTimer->callOnce(20);
					}
					return;
				}
				if (const auto peer = session->data().peerByUsername(username)) {
					Data::RememberPeerName(peer);
					row->setPeer(peer);
					if (!resolveQueue->empty()) {
						resolveTimer->callOnce(30);
					}
					return;
				}
				*resolveActive = true;
				const auto api = static_cast<MTP::Sender*>(&session->api());
				api->request(MTPcontacts_ResolveUsername(
					MTP_flags(MTPcontacts_ResolveUsername::Flags(0)),
					MTP_string(username),
					MTP_string(QString())
				)).done([=, api = api](const MTPcontacts_ResolvedPeer &result) {
					*resolveActive = false;
					const auto currentSession = Core::App().activeAccount().maybeSession();
					if (currentSession && (currentSession == session)) {
						result.match([&](const MTPDcontacts_resolvedPeer &data) {
							session->data().processUsers(data.vusers());
							session->data().processChats(data.vchats());
							const auto peerId = peerFromMTP(data.vpeer());
							if (peerId) {
								if (const auto peer = session->data().peer(peerId)) {
									Data::RememberPeerName(peer);
									if (row) {
										row->setPeer(peer);
									}
								}
							}
						});
					}
					if (!resolveQueue->empty()) {
						resolveTimer->callOnce(500);
					}
				}).fail([=, api = api](const MTP::Error &error) {
					*resolveActive = false;
					const auto type = error.type();
					if (type.startsWith(u"FLOOD_WAIT"_q)) {
						const auto seconds = type.mid(11).toInt();
						const auto waitMs = (seconds > 0 ? std::min(seconds, 60) : 30) * 1000;
						*floodCooldownUntil = crl::now() + waitMs;
						if (!resolveQueue->empty()) {
							resolveTimer->callOnce(waitMs);
						}
					} else {
						if (!resolveQueue->empty()) {
							resolveTimer->callOnce(350);
						}
					}
				}).send();
			};
			resolveTimer->setCallback([=] { (*stepResolve)(); });
			box->lifetime().add([=] {
				resolveQueue->clear();
				resolveTimer->cancel();
			});

			const auto addUser = [=](const QString &username) {
				const auto statusFor = [=] {
					if (extraMark) {
						return extraMark(username);
					} else if (const auto record = Data::FindSmsSendRecord(username)) {
						return (record->status == Data::SmsSendStatus::Sent)
							? u"✓ OK"_q
							: (u"— "_q + SentStatusText(record->status)
								+ (record->note.isEmpty()
									? QString()
									: (u" ("_q + record->note + ')')));
					}
					return QString();
				};
				auto wrapOwned = object_ptr<Ui::RpWidget>(box);
				const auto wrap = wrapOwned.data();
				const auto customName = displayName ? displayName(username) : QString();
				const auto button = Ui::CreateChild<UsernameRow>(
					wrap,
					username,
					rpl::single(statusFor()) | rpl::then(
						Data::BasesChanged() | rpl::map([=] { return statusFor(); })),
					customName);

				const auto cross = remove
					? Ui::CreateChild<Ggtg::RemoveButton>(wrap)
					: nullptr;
				const auto writeBtn = !isGroupList
					? Ui::CreateChild<Ggtg::GlyphButton>(
						wrap,
						QString(),
						u"Написать сообщение"_q,
						&st::menuIconChatBubble)
					: nullptr;
				const auto squid = viaSquidward
					? Ui::CreateChild<Ggtg::GlyphButton>(
						wrap,
						QString(),
						u"Собрать участников через Сквидвард"_q,
						&st::menuIconSquidward)
					: nullptr;
				const auto closed = isClosed && isClosed(username);
				const auto load = (download && !closed)
					? Ui::CreateChild<Ggtg::GlyphButton>(
						wrap,
						QString(),
						u"Скачать участников группы"_q,
						&st::menuIconDownload)
					: nullptr;

				const auto rowHeight = style::ConvertScale(64);
				wrap->resize(wrap->width(), rowHeight);

				const auto relayout = [=] {
					const auto width = wrap->width();
					auto right = width - style::ConvertScale(4);
					if (cross) {
						right -= cross->width();
						cross->move(
							right,
							(rowHeight - cross->height()) / 2);
					}
					if (writeBtn) {
						right -= writeBtn->width();
						writeBtn->move(
							right,
							(rowHeight - writeBtn->height()) / 2);
					}
					if (squid) {
						right -= squid->width();
						squid->move(
							right,
							(rowHeight - squid->height()) / 2);
					}
					if (load) {
						right -= load->width();
						load->move(
							right,
							(rowHeight - load->height()) / 2);
					}
					button->setGeometry(0, 0, std::max(right, 0), rowHeight);
					button->setAvailableWidth(right);
					wrap->resize(width, rowHeight);
				};
				wrap->widthValue() | rpl::on_next([=](int) {
					relayout();
				}, wrap->lifetime());
				button->changed() | rpl::on_next(relayout, wrap->lifetime());

				if (writeBtn) {
					writeBtn->setClickedCallback([=] {
						if (closed) {
							if (viaSquidward) {
								viaSquidward(username);
							}
							return;
						}
						OpenUsernamePeer(username, show, /*openProfile*/ false);
					});
				}

				if (load) {
					load->setClickedCallback([=] { download(username); });
				}
				if (squid) {
					squid->setClickedCallback([=] { viaSquidward(username); });
				}
				if (cross) {
					cross->setClickedCallback([=] {
						if (!Core::GuardAllows(
								box->uiShow(),
								Core::GuardedAction::EditBases)) {
							return;
						}
						remove(username);
						wrap->deleteLater();
					});
				}
				Ggtg::SetContextMenu(button, [=] {
					auto entries = std::vector<Ggtg::MenuEntry>();
					if (!closed) {
						if (!isGroupList) {
							entries.push_back({
								u"Просмотр профиля"_q,
								[=] { OpenUsernamePeer(username, show, /*openProfile*/ true); },
								&st::menuIconProfile,
							});
							entries.push_back({
								u"Написать сообщение"_q,
								[=] { OpenUsernamePeer(username, show, /*openProfile*/ false); },
								&st::menuIconChatBubble,
							});
						} else {
							entries.push_back({
								u"Открыть группу"_q,
								[=] { OpenUsernamePeer(username, show, /*openProfile*/ false); },
								&st::menuIconShowInChat,
							});
						}
					}
					entries.push_back({
						u"Копировать @username"_q,
						[=] {
							QGuiApplication::clipboard()->setText(
								u"@"_q + username.trimmed().remove('@'));
							show->showToast(u"Скопировано"_q);
						},
						&st::menuIconCopy,
					});
					const auto meta = Data::LookupUserMeta(username);
					if (!meta.name.isEmpty()) {
						entries.push_back({
							u"Копировать имя"_q,
							[=] {
								QGuiApplication::clipboard()->setText(meta.name);
								show->showToast(u"Скопировано"_q);
							},
							&st::menuIconCopy,
						});
					}
					if (meta.id != 0) {
						entries.push_back({
							u"Копировать ID"_q,
							[=] {
								QGuiApplication::clipboard()->setText(QString::number(meta.id));
								show->showToast(u"Скопировано"_q);
							},
							&st::menuIconCopy,
						});
					}
					if (move) {
						entries.push_back({
							u"Перенести в другую базу..."_q,
							[=] { move(username, [=] { wrap->deleteLater(); }); },
							&st::menuIconReplace,
						});
					}
					if (Data::FindSmsSendRecord(username)) {
						entries.push_back({
							u"Вернуть в очередь (сбросить статус)"_q,
							[=] {
								Data::DeleteSmsSendRecord(username);
								show->showToast(u"Статус сброшен"_q);
							},
							&st::menuIconRestore,
							true,
						});
					}
					return entries;
				});

				list->add(std::move(wrapOwned));
				button->setClickedCallback([=] {
					if (closed) {
						if (viaSquidward) {
							viaSquidward(username);
						}
						return;
					}
					if (isGroupList) {
						OpenUsernamePeer(username, show, /*openProfile*/ false);
					} else {
						OpenUsernamePeer(username, show, /*openProfile*/ true);
					}
				});

				if (button->needsResolution()) {
					resolveQueue->push_back({ username, QPointer<UsernameRow>(button) });
					if (!resolveTimer->isActive() && !*resolveActive) {
						resolveTimer->callOnce(100);
					}
				}
			};

			const auto appendChunk = std::make_shared<Fn<void()>>();
			*appendChunk = [=] {
				if (*renderedCount >= allUsernames->size()) {
					return;
				}
				const auto from = *renderedCount;
				const auto to = std::min(from + kChunkSize, int(allUsernames->size()));
				for (auto i = from; i < to; ++i) {
					addUser((*allUsernames)[i]);
				}
				*renderedCount = to;
			};

			*rebuild = [=] {
				resolveQueue->clear();
				resolveTimer->cancel();
				*resolveActive = false;
				const Ggtg::ScrollKeeper keepScroll(list);
				list->clear();
				const auto q = searchQuery->trimmed().toLower();
				const auto qNoAt = q.startsWith('@') ? q.mid(1) : q;
				auto filtered = QStringList();
				filtered.reserve(usernames.size());
				for (const auto &u : usernames) {
					if (q.isEmpty()) {
						filtered.push_back(u);
						continue;
					}
					const auto bare = u.trimmed().remove('@').toLower();
					if (bare.contains(qNoAt)) {
						filtered.push_back(u);
						continue;
					}
					const auto meta = Data::LookupUserMeta(u);
					if (!meta.name.isEmpty() && meta.name.toLower().contains(q)) {
						filtered.push_back(u);
						continue;
					}
					if (meta.id != 0 && QString::number(meta.id).contains(q)) {
						filtered.push_back(u);
						continue;
					}
				}
				*allUsernames = OrderedUsernames(
					filtered,
					*order,
					!extraMark);
				*renderedCount = 0;
				(*appendChunk)();
				if (q.isEmpty()) {
					countLabel->setText(QString::number(usernames.size()) + unitWord(usernames.size()));
				} else {
					countLabel->setText(QString::number(filtered.size()) + u" из "_q + QString::number(usernames.size()) + unitWord(usernames.size()));
				}
				relayoutControls();
			};
			(*rebuild)();

			box->scrolls() | rpl::on_next([=] {
				if (box->scrollTop() + box->scrollHeight() + 300 >= box->verticalLayout()->height()) {
					(*appendChunk)();
				}
			}, box->lifetime());
		}
		Ggtg::AddCloseCross(box);
	}), Ui::LayerOption::KeepOther);
}

// Shared by both "+ Новый список пользователей" and "Изменить" on a
// BaseKind::Users base: `id` is a fresh synthetic one for a new base, or
// an existing base's id to overwrite in place. Takes a Ui::Show rather
// than a SessionController so it can be opened either from the main
// window ("База" in the side menu) or from inside the "Отправка SMS" box
// ("Управлять списками получателей..." - see settings/sms_campaign.cpp)
// and stay inside whichever one it was opened from for all its own
// nested navigation too.
void ShowEditBaseBox(
		std::shared_ptr<Ui::Show> show,
		BasesActions actions,
		uint64 id,
		const QString &title,
		const QStringList &usernames,
		bool isNew) {
	struct State {
		QPointer<Ui::InputField> title;
		QPointer<Ui::InputField> list;
	};
	const auto state = std::make_shared<State>();

	show->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setWidth(st::accountsBoxWidth);
		box->setTitle(rpl::single(
			isNew ? u"Новый список пользователей"_q : u"Изменить список"_q));
		if (const auto locked = Core::BlockedReason(
				Core::GuardedAction::EditBases);
			!isNew && !locked.isEmpty()) {
			box->addRow(object_ptr<Ui::FlatLabel>(
				box,
				locked,
				st::boxLabel));
		}
		if (!isNew) {
			// Opens straight into editing now (see MakeScrapedGroupsDbBox's
			// row click) instead of via an intermediate "count + Изменить/
			// Открыть список/Удалить" box - these numbers (and the Удалить
			// button below) are what that box used to show, so nothing is
			// lost by skipping it.
			// Same numbers as "Отправка SMS" (Data::ComputeBaseStats).
			// Live: a running campaign (or a reset in "Отправленные")
			// changes these numbers while this box is open.
			const auto describe = [=] {
				// From the stored base, not the list this box was opened
				// with: users can be removed from it while this is open.
				const auto stats = Data::ComputeBaseStats(
					Data::LoadScrapedGroupUsernames(id));
				auto text = u"Юзернеймов: "_q + QString::number(stats.total)
					+ u". Осталось написать: "_q
					+ QString::number(stats.remaining)
					+ u"\nOK (уже написано): "_q + QString::number(stats.sent);
				if (stats.longAgo > 0) {
					text += u", были давно: "_q + QString::number(stats.longAgo);
				}
				if (stats.bots > 0) {
					text += u", боты: "_q + QString::number(stats.bots);
				}
				text += u"\nНе найдено: "_q + QString::number(stats.notFound)
					+ u", закрыт ЛС: "_q + QString::number(stats.restricted)
					+ u", лимит диалогов: "_q + QString::number(stats.peerLimit);
				return text;
			};
			const auto statsLabel = box->addRow(object_ptr<Ui::FlatLabel>(
				box,
				describe(),
				st::boxLabel));
			Data::BasesChanged(
			) | rpl::on_next([=] {
				statsLabel->setText(describe());
			}, box->lifetime());
			AddOpenListButton(box)->setClickedCallback([=] {
				// "✕" in that list removes a user from the stored base right
				// away (no confirmation, no need to have written to them) and
				// from this box's own editor text, so a later "Сохранить"
				// here can't bring them back.
				const auto removeUser = [=](const QString &username) {
					if (!Core::GuardAllows(show, Core::GuardedAction::EditBases)) {
						return;
					}
					const auto same = [&](const QString &other) {
						return other.trimmed().remove('@').toLower()
							== username.trimmed().remove('@').toLower();
					};
					auto list = Data::LoadScrapedGroupUsernames(id);
					list.erase(
						std::remove_if(list.begin(), list.end(), same),
						list.end());
					auto baseTitle = title;
					for (const auto &meta : Data::LoadScrapedGroupsIndex()) {
						if (meta.id == id) {
							baseTitle = meta.title;
							break;
						}
					}
					Data::SaveScrapedGroupUsernames(id, baseTitle, list);
					Core::GgtgLog::Info(
						u"Базы"_q,
						u"Из базы \""_q + baseTitle + u"\" удалён @"_q
							+ username.trimmed().remove('@'));
					if (const auto field = state->list.data()) {
						auto lines = field->getLastText().split('\n');
						lines.erase(
							std::remove_if(lines.begin(), lines.end(), same),
							lines.end());
						field->setText(lines.join('\n'));
					}
				};
				ShowClickableUsernamesBox(
					show,
					title,
					Data::LoadScrapedGroupUsernames(id),
					removeUser,
					nullptr,
					[=](const QString &username) {
						return Data::KnownPeerName(username);
					},
					nullptr,
					nullptr,
					nullptr,
					// Right click -> "Перенести в другую базу...": a person
					// lives in one base, so he leaves this one.
					[=](const QString &username, Fn<void()> done) {
						if (!Core::GuardAllows(
								show,
								Core::GuardedAction::EditBases)) {
							return;
						}
						ShowBasePicker(
							show,
							u"Перенести в базу"_q,
							id,
							false,
							[=](std::vector<uint64> ids) {
								if (ids.empty()) {
									return;
								}
								const auto target = ids.front();
								auto targetTitle = QString();
								for (const auto &meta
										: Data::LoadScrapedGroupsIndex()) {
									if (meta.id == target) {
										targetTitle = meta.title;
										break;
									}
								}
								auto list = Data::LoadScrapedGroupUsernames(
									target);
								list.push_back(username);
								removeUser(username);
								Data::SaveScrapedGroupUsernames(
									target,
									targetTitle,
									list);
								show->showToast(
									u"@"_q + username.trimmed().remove('@')
										+ u" перенесён в «"_q + targetTitle
										+ u"»"_q);
								if (done) {
									done();
								}
							});
					});
			});
			if (actions.sendToBase) {
				AddIconRowButton(
					box,
					u"Отправить SMS по этой базе"_q,
					st::ggtgSendButton
				)->setClickedCallback([=] {
					actions.sendToBase(id);
				});
			}
		}
		state->title = box->addRow(object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			rpl::single(u"Название"_q),
			title));
		const auto large = int(usernames.size()) > kEditorLimit;
		if (large) {
			box->addRow(object_ptr<Ui::FlatLabel>(
				box,
				u"Список слишком большой для редактирования здесь ("_q
					+ QString::number(usernames.size())
					+ u" юзернеймов) - пользуйтесь операциями ниже; "
						"сохранить можно только новое название."_q,
				st::boxLabel));
		} else {
			Ui::AddDividerText(
				box->verticalLayout(),
				rpl::single(
					u"Имена пользователей, по одному на строку (или через "
						"пробел/запятую), с @ или без, можно и ссылки "
						"t.me/...:"_q));
			state->list = box->addRow(object_ptr<Ui::InputField>(
				box,
				st::defaultInputField,
				Ui::InputField::Mode::MultiLine,
				rpl::single(QString()),
				usernames.join('\n')));
		}
		if (!isNew) {
			Ui::AddSkip(box->verticalLayout());
			Ui::AddSubsectionTitle(
				box->verticalLayout(), rpl::single(u"Операции"_q));
			Ui::AddDividerText(
				box->verticalLayout(),
				rpl::single(
					u"Работают с сохранённым списком (несохранённые правки "
						"выше не учитываются). Человек хранится только в "
						"одной базе, поэтому \"Объединить\" переносит "
						"выбранные базы в эту, а \"Разбить на части\" "
						"оставляет первую часть в этой базе."_q));
			// The picker may have taken this box's place meanwhile.
			const auto weak = QPointer<Ui::GenericBox>(box.get());
			const auto reopen = [=] {
				if (const auto strong = weak.data()) {
					strong->closeBox();
				}
			};
			const auto addOp = [=](const QString &text, Fn<void()> handler) {
				box->addRow(object_ptr<Ui::SettingsButton>(
					box, rpl::single(text))
				)->setClickedCallback(std::move(handler));
			};

			addOp(u"Экспорт в файл (txt / csv)..."_q, [=] {
				FileDialog::GetWritePath(
					QPointer<QWidget>(box.get()),
					u"Экспорт списка"_q,
					u"Текст (*.txt);;CSV (*.csv)"_q,
					title + u".txt"_q,
					[=](QString &&path) {
						auto file = QFile(path);
						if (!file.open(
								QIODevice::WriteOnly | QIODevice::Truncate)) {
							Ggtg::Notice(show, u"Не удалось записать файл"_q);
							return;
						}
						const auto csv = path.endsWith(
							u".csv"_q,
							Qt::CaseInsensitive);
						auto text = csv ? u"username\n"_q : QString();
						text += usernames.join('\n');
						text += '\n';
						file.write(text.toUtf8());
						Ggtg::Notice(show, u"Экспортировано: "_q
							+ QString::number(usernames.size()));
					});
			});
			addOp(u"Очистить от неактивных («был(а) давно»)..."_q, [=] {
				if (!Core::GuardAllows(show, Core::GuardedAction::EditBases)) {
					return;
				}
				const auto currentList = Data::LoadScrapedGroupUsernames(id);
				auto kept = QStringList();
				auto removed = 0;
				for (const auto &u : currentList) {
					const auto norm = u.trimmed().remove('@').toLower();
					const auto rec = Data::FindSmsSendRecord(norm);
					if (rec && rec->status == Data::SmsSendStatus::LongAgo) {
						++removed;
					} else {
						kept.push_back(u);
					}
				}
				if (!removed) {
					Ggtg::Notice(show, u"Неактивных («был(а) давно») в базе нет"_q);
					return;
				}
				show->show(Ui::MakeConfirmBox({
					.text = u"Удалить из списка неактивных («был(а) давно»): "_q
						+ QString::number(removed)
						+ u"? Список изменится насовсем."_q,
					.confirmed = [=](Fn<void()> &&close) {
						close();
						Data::SaveScrapedGroupUsernames(id, title, kept);
						if (const auto field = state->list.data()) {
							field->setText(kept.join('\n'));
						}
						Ggtg::Notice(show, u"Удалено неактивных: "_q + QString::number(removed));
						reopen();
					},
					.confirmText = u"Удалить"_q,
				}));
			});
			addOp(u"Удалить уже написанных (статус OK)..."_q, [=] {
				if (!Core::GuardAllows(show, Core::GuardedAction::EditBases)) {
					return;
				}
				const auto currentList = Data::LoadScrapedGroupUsernames(id);
				auto kept = QStringList();
				auto removed = 0;
				for (const auto &u : currentList) {
					const auto norm = u.trimmed().remove('@').toLower();
					const auto rec = Data::FindSmsSendRecord(norm);
					if (rec && rec->status == Data::SmsSendStatus::Sent) {
						++removed;
					} else {
						kept.push_back(u);
					}
				}
				if (!removed) {
					Ggtg::Notice(show, u"Написанных (OK) в базе нет"_q);
					return;
				}
				show->show(Ui::MakeConfirmBox({
					.text = u"Удалить из списка уже написанных (OK): "_q
						+ QString::number(removed)
						+ u"? Список изменится насовсем."_q,
					.confirmed = [=](Fn<void()> &&close) {
						close();
						Data::SaveScrapedGroupUsernames(id, title, kept);
						if (const auto field = state->list.data()) {
							field->setText(kept.join('\n'));
						}
						Ggtg::Notice(show, u"Удалено написанных: "_q + QString::number(removed));
						reopen();
					},
					.confirmText = u"Удалить"_q,
				}));
			});
			addOp(u"Очистить от фейков и ботов…"_q, [=] {
				Ggtg::FakeDetector::ShowCleanBaseBox(show, id, title, [=] {
					reopen();
				});
			});
			// A person lives in one base only (SaveScrapedGroupUsernames
			// drops names another base holds), so a "new merged base" came
			// out empty: the chosen bases are moved into this one instead,
			// and removed once their people are here.
			addOp(u"Объединить"_q, [=] {
				if (!Core::GuardAllows(show, Core::GuardedAction::EditBases)) {
					return;
				}
				ShowBasePicker(
					show,
					u"Объединить с..."_q,
					id,
					true,
					[=](std::vector<uint64> ids) {
						auto merged = usernames;
						auto titles = QStringList();
						for (const auto other : ids) {
							merged.append(
								Data::LoadScrapedGroupUsernames(other));
							titles.push_back(BaseTitleById(other));
						}
						show->show(Ui::MakeConfirmBox({
							.text = u"Перенести в \""_q + title + u"\" базы: "_q
								+ titles.join(u", "_q)
								+ u"? Они станут частью этой базы, а сами "
									"исчезнут из списка."_q,
							.confirmed = [=](Fn<void()> &&close) {
								close();
								for (const auto other : ids) {
									Data::DeleteScrapedGroup(other);
								}
								Data::SaveScrapedGroupUsernames(
									id,
									title,
									merged);
								const auto now = int(
									Data::LoadScrapedGroupUsernames(id).size());
								Ggtg::Notice(show, u"Базы объединены: "_q
									+ QString::number(now)
									+ u" пользователей"_q);
								reopen();
							},
							.confirmText = u"Объединить"_q,
						}));
					});
			});
			// People whose names are in Korean, Chinese, Hindi... go to bases
			// of their own: "Название (корейский)" and so on. The language
			// is the script of the name; a person whose name is not known
			// yet stays in this base.
			addOp(u"Разбить по языкам имён..."_q, [=] {
				if (!Core::GuardAllows(show, Core::GuardedAction::EditBases)) {
					return;
				}
				auto groups = std::map<QString, QStringList>();
				auto unknown = QStringList();
				for (const auto &username : usernames) {
					const auto name = Data::KnownPeerName(username);
					const auto code = name.isEmpty()
						? QString()
						: Core::GgtgAi::DetectLanguage(name);
					if (code.isEmpty()) {
						unknown.push_back(username);
					} else {
						groups[code].push_back(username);
					}
				}
				if (groups.empty()) {
					Ggtg::Notice(show, u"Имена людей из этой базы пока "
						"неизвестны: они запоминаются, когда база собирается "
						"из групп или когда человек попадался программе. "
						"Делить нечего."_q);
					return;
				} else if (groups.size() == 1 && unknown.isEmpty()) {
					Ggtg::Notice(show, u"Все имена на одном языке: делить "
						"нечего."_q);
					return;
				}
				const auto label = [](const QString &code) {
					return (code == u"en"_q)
						? u"латиница"_q
						: Core::GgtgAi::LanguageByCode(code).russian;
				};
				// One checkbox per language found (all ticked at first): only
				// the ticked ones get a base of their own, the people of the
				// others stay in this base together with the unknown ones.
				struct Pick {
					QString code;
					QPointer<Ui::Checkbox> checkbox;
				};
				const auto picks = std::make_shared<std::vector<Pick>>();
				show->show(Box([=](not_null<Ui::GenericBox*> box) {
					box->setWidth(st::accountsBoxWidth);
					box->setTitle(rpl::single(u"Разбить по языкам"_q));
					box->addRow(object_ptr<Ui::FlatLabel>(
						box,
						u"Отметьте языки, для которых создать отдельные "
							"базы. Остальные люди"_q
							+ (unknown.isEmpty()
								? QString()
								: u" и те, чьё имя неизвестно ("_q
									+ QString::number(unknown.size())
									+ ')')
							+ u" останутся в этой базе."_q,
						st::boxLabel));
					for (const auto &[code, list] : groups) {
						const auto text = label(code) + u": "_q
							+ QString::number(list.size());
						picks->push_back({
							code,
							box->addRow(
								object_ptr<Ui::Checkbox>(box, text, true),
								st::ggtgChoiceRowPadding),
						});
						if (const auto check = picks->back().checkbox.data()) {
							check->setAllowTextLines(0);
						}
					}
					box->addButton(rpl::single(u"Разбить"_q), [=] {
						auto chosen = std::vector<QString>();
						for (const auto &pick : *picks) {
							if (pick.checkbox && pick.checkbox->checked()) {
								chosen.push_back(pick.code);
							}
						}
						if (chosen.empty()) {
							Ggtg::Notice(
								box->uiShow(),
								u"Выберите хотя бы один язык"_q);
							return;
						}
						box->closeBox();
						// This base lets go of the people first, so that the
						// new ones can take them (a person lives in one base).
						auto rest = unknown;
						for (const auto &[code, list] : groups) {
							if (ranges::find(chosen, code) == end(chosen)) {
								rest.append(list);
							}
						}
						if (rest.isEmpty()) {
							Data::DeleteScrapedGroup(id);
						} else {
							Data::SaveScrapedGroupUsernames(id, title, rest);
						}
						for (const auto &code : chosen) {
							Data::SaveScrapedGroupUsernames(
								Data::GenerateManualBaseId(),
								title + u" ("_q + label(code) + ')',
								groups.at(code));
						}
						Ggtg::Notice(show, u"Создано баз по языкам: "_q
							+ QString::number(chosen.size()));
						reopen();
					});
					box->addButton(
						rpl::single(u"Отмена"_q),
						[=] { box->closeBox(); });
				}));
			});
			addOp(u"Разбить на части..."_q, [=] {
				if (!Core::GuardAllows(show, Core::GuardedAction::EditBases)) {
					return;
				}
				ShowNumberPrompt(
					show,
					u"Разбить на части"_q,
					u"На сколько частей"_q,
					u"2"_q,
					[=](int parts) {
						const auto total = int(usernames.size());
						if (parts < 2 || parts > total) {
							Ggtg::Notice(show, 
								u"Частей должно быть от 2 до "_q
									+ QString::number(total));
							return;
						}
						const auto size = total / parts;
						const auto extra = total % parts;
						auto chunks = std::vector<QStringList>();
						auto from = 0;
						for (auto i = 0; i != parts; ++i) {
							const auto length = size + (i < extra ? 1 : 0);
							chunks.push_back(usernames.mid(from, length));
							from += length;
						}
						const auto partTitle = [=](int i) {
							return title + u" (часть %1/%2)"_q
								.arg(i + 1)
								.arg(parts);
						};
						// This base keeps the first part first, so the
						// others' people are free to go into new bases.
						Data::SaveScrapedGroupUsernames(
							id,
							partTitle(0),
							chunks.front());
						for (auto i = 1; i != parts; ++i) {
							Data::SaveScrapedGroupUsernames(
								Data::GenerateManualBaseId(),
								partTitle(i),
								chunks[i]);
						}
						Ggtg::Notice(show, u"Создано частей: "_q
							+ QString::number(parts));
						reopen();
					});
			});
		}
		box->setFocusCallback([=] { state->title->setFocusFast(); });
		box->addButton(rpl::single(u"Сохранить"_q), [=] {
			if (!isNew && !Core::GuardAllows(show, Core::GuardedAction::EditBases)) {
				return;
			}
			const auto newTitle = state->title->getLastText().trimmed();
			if (newTitle.isEmpty()) {
				Ggtg::Notice(box->uiShow(), u"Введите название"_q);
				return;
			}
			// A base too big for the text field isn't editable here - its
			// list is kept exactly as it was, only the title can change.
			auto parsed = Data::ParsedUsernames();
			if (large) {
				parsed.valid = usernames;
			} else {
				parsed = Data::ParseUsernames(
					state->list->getLastText().split('\n'));
			}
			const auto commit = [=] {
				box->closeBox();
				Data::SaveScrapedGroupUsernames(id, newTitle, parsed.valid);
				if (!parsed.rejected.isEmpty() || parsed.duplicates > 0) {
					Ggtg::Notice(show, 
						u"Сохранено: "_q + QString::number(parsed.valid.size())
							+ u", отклонено: "_q
							+ QString::number(parsed.rejected.size())
							+ u", дубликатов: "_q
							+ QString::number(parsed.duplicates));
				}
				// The "База" box underneath refreshes itself on BasesChanged - no
				// need to open a second copy (which, inside the GGTG window,
				// stacked up instead of replacing it).
			};
			if (parsed.rejected.isEmpty()) {
				commit();
				return;
			}
			// Don't silently drop what looks like a typo - say what
			// wouldn't be recognized, and let the user go back and fix it.
			show->show(Ui::MakeConfirmBox({
				.text = u"Не распознано строк: "_q
					+ QString::number(parsed.rejected.size()) + u" ("_q
					+ parsed.rejected.mid(0, 5).join(u", "_q)
					+ (parsed.rejected.size() > 5 ? u"…"_q : QString())
					+ u"). Сохранить без них?"_q,
				.confirmed = [=](Fn<void()> &&close) {
					close();
					commit();
				},
				.confirmText = u"Сохранить без них"_q,
			}));
		});
		if (!isNew) {
			box->addButton(rpl::single(u"Удалить"_q), [=] {
				if (!Core::GuardAllows(show, Core::GuardedAction::EditBases)) {
					return;
				}
				// A base can hold tens of thousands of usernames and
				// there's no undo - always ask first.
				show->show(Ui::MakeConfirmBox({
					.text = u"Удалить список \""_q + title + u"\" ("_q
						+ QString::number(usernames.size())
						+ u")? Это нельзя отменить."_q,
					.confirmed = [=](Fn<void()> &&close) {
						close();
						Data::DeleteScrapedGroup(id);
						box->closeBox();
					},
					.confirmText = u"Удалить"_q,
				}));
			});
		}
		Ggtg::AddBackButton(box);
	}));
}

// Groups counterpart of ShowEditBaseBox - manual entry only takes an
// @username per line (a hand-typed group has no title/member count to
// offer), which is enough to identify it.
void ShowEditGroupsBaseBox(
		std::shared_ptr<Ui::Show> show,
		BasesActions actions,
		uint64 id,
		const QString &title,
		const std::vector<Data::ScrapedGroupEntry> &entries,
		bool isNew) {
	struct State {
		QPointer<Ui::InputField> title;
		QPointer<Ui::InputField> list;
	};
	const auto state = std::make_shared<State>();

	auto lines = QStringList();
	auto clickableUsernames = QStringList();
	for (const auto &entry : entries) {
		lines.push_back(!entry.username.isEmpty()
			? ('@' + entry.username)
			: (!entry.link.isEmpty() ? entry.link : entry.title));
		// A closed group is listed by its link (🔒, Сквидвард only).
		if (!entry.username.isEmpty()) {
			clickableUsernames.push_back(entry.username);
		} else if (!entry.link.isEmpty()) {
			clickableUsernames.push_back(entry.link);
		}
	}

	show->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setWidth(st::accountsBoxWidth);
		box->setTitle(rpl::single(
			isNew ? u"Новый список групп"_q : u"Изменить список"_q));
		if (const auto locked = Core::BlockedReason(
				Core::GuardedAction::EditBases);
			!isNew && !locked.isEmpty()) {
			box->addRow(object_ptr<Ui::FlatLabel>(
				box,
				locked,
				st::boxLabel));
		}
		if (!isNew) {
			// Same "open straight into editing" change as ShowEditBaseBox -
			// see its own comment.
			box->addRow(object_ptr<Ui::FlatLabel>(
				box,
				u"Групп: "_q + QString::number(entries.size()) + '.',
				st::boxLabel));
			if (!clickableUsernames.isEmpty()) {
				// Only entries with a public @username can be opened this
				// way - one with just an invite link has nowhere to open
				// to from here (see ScrapedGroupEntry's own comment on why
				// that link isn't kept at all), so those stay text-only in
				// this list.
				AddOpenListButton(box)->setClickedCallback([=] {
					const auto bare = [](const QString &name) {
						return name.trimmed().remove('@').toLower();
					};
					// "✓" next to a group when a recipients base named after
					// it exists (that's what saving "Топ, люди" creates).
					// A row is identified by the group's @username, or by its
					// link when it has none (a closed group).
					const auto idOf = [=](const Data::ScrapedGroupEntry &e) {
						return e.username.isEmpty() ? e.link : e.username;
					};
					const auto isClosed = [=](const QString &row) {
						return row.contains('/');
					};
					const auto groupTitle = [=](const QString &username) {
						for (const auto &entry
								: Data::LoadScrapedGroupEntries(id)) {
							if (bare(idOf(entry)) == bare(username)) {
								return entry.title;
							}
						}
						return QString();
					};
					const auto mark = [=](const QString &username) {
						if (isClosed(username)) {
							return QString();
						}
						const auto name = groupTitle(username);
						for (const auto &meta : Data::LoadScrapedGroupsIndex()) {
							if (meta.kind == Data::BaseKind::Users
								&& Data::BaseTitleMatchesGroup(
									meta.title,
									username,
									name)) {
								return u"  ✓ есть в получателях"_q;
							}
						}
						return QString();
					};
					// Read once: the label is rebuilt often, and re-reading the
					// base from disk for each of them would be slow on a long list.
					const auto members = std::make_shared<
						base::flat_map<QString, int>>();
					for (const auto &entry : Data::LoadScrapedGroupEntries(id)) {
						members->emplace(
							bare(idOf(entry)),
							entry.membersCount);
					}
					const auto display = [=](const QString &username) {
						auto name = groupTitle(username);
						if (!isClosed(username)
							&& (name.isEmpty()
								|| bare(name) == bare(username))) {
							name = Data::KnownPeerName(username);
						}
						if (isClosed(username)) {
							return (name.isEmpty()
								? u"Закрытая группа"_q
								: name)
								+ u"\nзакрытая  ·  только через Сквидвард"_q;
						}
						const auto it = members->find(bare(username));
						const auto countText = (it != members->end() && it->second > 1)
							? (QLocale().toString(it->second) + u" уч."_q)
							: QString();
						return name + '\n' + countText;
					};
					const auto removeGroup = [=](const QString &username) {
						if (!Core::GuardAllows(show, Core::GuardedAction::EditBases)) {
							return;
						}
						auto list = Data::LoadScrapedGroupEntries(id);
						list.erase(
							std::remove_if(
								list.begin(),
								list.end(),
								[&](const auto &e) {
									return bare(idOf(e))
										== bare(username);
								}),
							list.end());
						auto baseTitle = title;
						for (const auto &meta
								: Data::LoadScrapedGroupsIndex()) {
							if (meta.id == id) {
								baseTitle = meta.title;
								break;
							}
						}
						Data::SaveScrapedGroupEntries(id, baseTitle, list);
						Core::GgtgLog::Info(
							u"Базы"_q,
							u"Из базы групп \""_q + baseTitle
								+ u"\" удалена @"_q + bare(username));
						if (const auto field = state->list.data()) {
							auto lines = field->getLastText().split('\n');
							lines.erase(
								std::remove_if(
									lines.begin(),
									lines.end(),
									[&](const QString &l) {
										return bare(l) == bare(username);
									}),
								lines.end());
							field->setText(lines.join('\n'));
						}
					};
					auto current = QStringList();
					for (const auto &entry : Data::LoadScrapedGroupEntries(id)) {
						if (!idOf(entry).isEmpty()) {
							current.push_back(idOf(entry));
						}
					}
					ShowClickableUsernamesBox(
						show,
						title,
						current,
						removeGroup,
						mark,
						display,
						actions.scrapeGroup,
						actions.scrapeGroupViaSquidward
							? Fn<void(const QString &)>([=](
								const QString &username) {
								actions.scrapeGroupViaSquidward(
									show,
									username);
							})
							: nullptr,
						isClosed);
				});
			}
		}
		if (!isNew && actions.removeChannels) {
			auto unchecked = 0;
			for (const auto &entry : entries) {
				if (!entry.username.isEmpty() && !entry.checked) {
					++unchecked;
				}
			}
			if (unchecked) {
				box->addRow(object_ptr<Ui::SettingsButton>(
					box,
					rpl::single(u"Убрать каналы · не проверено "_q
						+ QString::number(unchecked))
				))->setClickedCallback([=] {
					if (!Core::GuardAllows(show, Core::GuardedAction::EditBases)) {
						return;
					}
					const auto weak = QPointer<Ui::GenericBox>(box.get());
					actions.removeChannels(show, id, [=] {
						// The list field would save the old rows back.
						if (const auto strong = weak.data()) {
							strong->closeBox();
						}
					});
				});
			} else {
				box->addRow(object_ptr<Ui::FlatLabel>(
					box,
					u"Все группы проверены, каналов нет."_q,
					st::boxLabel));
			}
		}
		state->title = box->addRow(object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			rpl::single(u"Название"_q),
			title));
		Ui::AddDividerText(
			box->verticalLayout(),
			rpl::single(
				u"Юзернеймы групп/каналов, по одному на строку (с @ или "
					"без). Название и число участников заполняются "
					"автоматически только при добавлении через поиск "
					"групп."_q));
		state->list = box->addRow(object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			Ui::InputField::Mode::MultiLine,
			rpl::single(QString()),
			lines.join('\n')));
		box->setFocusCallback([=] { state->title->setFocusFast(); });
		box->addButton(rpl::single(u"Сохранить"_q), [=] {
			if (!isNew && !Core::GuardAllows(show, Core::GuardedAction::EditBases)) {
				return;
			}
			const auto newTitle = state->title->getLastText().trimmed();
			if (newTitle.isEmpty()) {
				Ggtg::Notice(box->uiShow(), u"Введите название"_q);
				return;
			}
			// Lines that match an entry already in this base keep that
			// entry as it was - its title and member count came from a
			// group search, and rebuilding every entry from the bare
			// username here used to wipe them on any edit at all.
			auto byUsername = base::flat_map<QString, Data::ScrapedGroupEntry>();
			auto byTitle = base::flat_map<QString, Data::ScrapedGroupEntry>();
			for (const auto &entry : entries) {
				if (entry.username.isEmpty()) {
					byTitle.emplace(
						entry.link.isEmpty() ? entry.title : entry.link,
						entry);
				} else {
					byUsername.emplace(entry.username.toLower(), entry);
				}
			}
			auto newEntries = std::vector<Data::ScrapedGroupEntry>();
			auto rejected = QStringList();
			auto seen = base::flat_set<QString>();
			for (const auto &line
					: state->list->getLastText().split('\n')) {
				const auto trimmed = line.trimmed();
				if (trimmed.isEmpty()) {
					continue;
				}
				if (const auto it = byTitle.find(trimmed);
						it != byTitle.end()) {
					newEntries.push_back(it->second);
					continue;
				}
				const auto parsed = Data::ParseUsernames({ trimmed });
				if (!parsed.rejected.isEmpty() || parsed.valid.size() != 1) {
					if (parsed.valid.isEmpty() && parsed.rejected.isEmpty()) {
						continue;
					}
					rejected.push_back(trimmed);
					continue;
				}
				const auto username = parsed.valid.front();
				if (!seen.emplace(username.toLower()).second) {
					continue;
				}
				if (const auto it = byUsername.find(username.toLower());
						it != byUsername.end()) {
					newEntries.push_back(it->second);
				} else {
					newEntries.push_back({
						.username = username,
						.title = username,
					});
				}
			}
			const auto commit = [=] {
				box->closeBox();
				Data::SaveScrapedGroupEntries(id, newTitle, newEntries);
				// The "База" box underneath refreshes itself on BasesChanged - no
				// need to open a second copy (which, inside the GGTG window,
				// stacked up instead of replacing it).
			};
			if (rejected.isEmpty()) {
				commit();
				return;
			}
			show->show(Ui::MakeConfirmBox({
				.text = u"Не распознано строк: "_q
					+ QString::number(rejected.size()) + u" ("_q
					+ rejected.mid(0, 5).join(u", "_q)
					+ (rejected.size() > 5 ? u"…"_q : QString())
					+ u"). Сохранить без них?"_q,
				.confirmed = [=](Fn<void()> &&close) {
					close();
					commit();
				},
				.confirmText = u"Сохранить без них"_q,
			}));
		});
		if (!isNew) {
			box->addButton(rpl::single(u"Удалить"_q), [=] {
				if (!Core::GuardAllows(show, Core::GuardedAction::EditBases)) {
					return;
				}
				show->show(Ui::MakeConfirmBox({
					.text = u"Удалить список \""_q + title + u"\" ("_q
						+ QString::number(entries.size())
						+ u")? Это нельзя отменить."_q,
					.confirmed = [=](Fn<void()> &&close) {
						close();
						Data::DeleteScrapedGroup(id);
						box->closeBox();
					},
					.confirmText = u"Удалить"_q,
				}));
			});
		}
		Ggtg::AddBackButton(box);
	}));
}

// "Отправленные" - browses the global SmsSendRecord registry (see
// data/scraped_members_db.h) that RunSmsCampaign checks before writing to
// a username, regardless of which base(s) it came from - the local
// equivalent of tg-web-panel's own separate "sent users" viewer, rather
// than only the blanket SPAM_ACC-only reset next to it in "База". A row
// click opens that record's actions (open chat / copy / reset status, the
// last one with confirmation); "Очистить всё" drops the
// whole registry, Sent records included, and does ask for confirmation
// since that's every past recipient at once.

// Rows shown at a time in "Отправленные" - a registry can hold tens of
// thousands of records, and a button widget for each froze the box.
constexpr auto kSentPageSize = 100;

// Actions for one "Отправленные" record. Reset asks first: it removes the
// "already written to" mark, so a later campaign messages this username
// again.
void ShowSentRecordActionsBox(
		std::shared_ptr<Ui::Show> show,
		const QString &username,
		std::shared_ptr<Fn<void()>> refresh) {
	show->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setWidth(st::accountsBoxWidth);
		box->setTitle(rpl::single('@' + username));
		const auto record = Data::FindSmsSendRecord(username);
		if (record) {
			box->addRow(object_ptr<Ui::FlatLabel>(
				box,
				u"Статус: "_q + SentStatusText(record->status)
					+ (record->note.isEmpty()
						? QString()
						: (u" ("_q + record->note + ')'))
					+ u"\nАккаунт: "_q + record->account
					+ u"\nКогда: "_q + FormatScrapedAt(record->at),
				st::boxLabel));
		}
		box->addRow(object_ptr<Ui::SettingsButton>(
			box, rpl::single(u"Открыть чат"_q))
		)->setClickedCallback([=] {
			OpenUsernamePeer(username, box->uiShow());
		});
		box->addRow(object_ptr<Ui::SettingsButton>(
			box, rpl::single(u"Копировать @username"_q))
		)->setClickedCallback([=] {
			QGuiApplication::clipboard()->setText('@' + username);
			Ggtg::Notice(box->uiShow(), u"Скопировано"_q);
		});
		box->addRow(object_ptr<Ui::SettingsButton>(
			box, rpl::single(u"Сбросить статус"_q))
		)->setClickedCallback([=] {
			box->uiShow()->show(Ui::MakeConfirmBox({
				.text = u"Сбросить статус @"_q + username
					+ u"? Рассылка снова сможет ему написать."_q,
				.confirmed = [=](Fn<void()> &&close) {
					close();
					Data::DeleteSmsSendRecord(username);
					Ggtg::Notice(box->uiShow(), 
						u"Статус @"_q + username + u" сброшен"_q);
					(*refresh)();
					box->closeBox();
				},
				.confirmText = u"Сбросить"_q,
			}));
		});
		Ggtg::AddBackButton(box);
	}));
}

[[nodiscard]] object_ptr<Ui::BoxContent> MakeSmsSentUsersBox(
		std::shared_ptr<Ui::Show> show) {
	return Box([=](not_null<Ui::GenericBox*> box) {
		box->setWidth(st::accountsBoxWidth);
		box->setTitle(rpl::single(u"Отправленные"_q));

		struct State {
			QString query;
			int filter = 0; // 0 all, 1 OK, 2 SPAM_ACC, 3 not found, 4 restricted
			int shown = kSentPageSize;
			QPointer<Ui::VerticalLayout> rows;
			QPointer<Ui::FlatLabel> summary;
		};
		const auto state = std::make_shared<State>();
		const auto filterLabel = [](int filter) {
			switch (filter) {
			case 1: return u"Фильтр: OK"_q;
			case 2: return u"Фильтр: спам-блок аккаунта"_q;
			case 3: return u"Фильтр: не найден"_q;
			case 4: return u"Фильтр: нельзя писать"_q;
			case 5: return u"Фильтр: не пустило (аккаунт свободен)"_q;
			case 6: return u"Фильтр: аккаунт не смог написать"_q;
			case 7: return u"Фильтр: предположительный фейк"_q;
			case 8: return u"Фильтр: был(а) давно"_q;
			}
			return u"Фильтр: все статусы"_q;
		};
		const auto filterText = box->lifetime().make_state<
			rpl::variable<QString>>(filterLabel(0));

		// Deleting a record or paging changes what the list shows, and the
		// list rebuilds after the click handler returns - never inside it,
		// since rebuilding destroys the very button that was clicked.
		// Assigned below, once `fill` exists.
		const auto refresh = std::make_shared<Fn<void()>>();

		const auto fill = [=] {
			const auto rows = state->rows.data();
			if (!rows) {
				return;
			}
			const Ggtg::ScrollKeeper keepScroll(rows);
			rows->clear();
			const auto records = Data::LoadAllSmsSendRecords();
			auto query = state->query.trimmed().toLower();
			if (query.startsWith('@')) {
				query.remove(0, 1);
			}
			auto matched = std::vector<Data::SmsSendRecord>();
			for (const auto &record : records) {
				const auto wanted = (state->filter == 0)
					|| (state->filter == 1
						&& record.status == Data::SmsSendStatus::Sent)
					|| (state->filter == 2
						&& record.status == Data::SmsSendStatus::SpamAcc)
					|| (state->filter == 3
						&& record.status == Data::SmsSendStatus::NotFound)
					|| (state->filter == 4
						&& record.status == Data::SmsSendStatus::Restricted)
					|| (state->filter == 5
						&& record.status == Data::SmsSendStatus::PeerLimit)
					|| (state->filter == 6
						&& record.status == Data::SmsSendStatus::AccountFail)
					|| (state->filter == 7
						&& record.status == Data::SmsSendStatus::SuspectedFake)
					|| (state->filter == 8
						&& record.status == Data::SmsSendStatus::LongAgo);
				if (!wanted) {
					continue;
				}
				if (!query.isEmpty()
					&& !record.username.contains(query)
					&& !record.account.toLower().contains(query)
					&& !Data::SmsSourceOf(record.username).toLower().contains(
						query)) {
					continue;
				}
				matched.push_back(record);
			}
			const auto shown = std::min(state->shown, int(matched.size()));
			if (const auto label = state->summary.data()) {
				label->setText(u"Всего записей: "_q
					+ QString::number(records.size())
					+ u". Подходит: "_q + QString::number(matched.size())
					+ u", показано: "_q + QString::number(shown) + '.');
			}
			if (matched.empty()) {
				rows->add(object_ptr<Ggtg::EmptyState>(
					rows,
					records.empty()
						? u"Пока пусто. Здесь появятся получатели после "
							"первой рассылки."_q
						: u"Ничего не найдено."_q));
				return;
			}
			for (auto i = 0; i != shown; ++i) {
				const auto &record = matched[i];
				const auto username = record.username;
				// A tap only opens this record's actions: it never changes
				// anything by itself (deleting a record means the username
				// gets messaged again, so it's an explicit, confirmed step).
				const auto item = rows->add(object_ptr<Ggtg::TextRow>(
					rows,
					rpl::single('@' + record.username + u" — "_q
						+ SentStatusText(record.status)
						+ (record.note.isEmpty()
							? QString()
							: (u" - "_q + record.note))
						+ u" ("_q + record.account + u", "_q
						+ FormatScrapedAt(record.at) + u')'
						+ (Data::SmsSourceOf(record.username).isEmpty()
							? QString()
							: (u" · из «"_q
								+ Data::SmsSourceOf(record.username)
								+ u"»"_q)))));
				item->setClickedCallback([=] {
					ShowSentRecordActionsBox(show, username, refresh);
				});
				// The same actions straight from a right click.
				Ggtg::SetContextMenu(item, [=] {
					auto entries = std::vector<Ggtg::MenuEntry>();
					entries.push_back({
						u"Открыть чат"_q,
						[=] { OpenUsernamePeer(username, show); },
						&st::menuIconShowInChat,
					});
					entries.push_back({
						u"Копировать @username"_q,
						[=] {
							QGuiApplication::clipboard()->setText(
								'@' + username);
							show->showToast(u"Скопировано"_q);
						},
						&st::menuIconCopy,
					});
					if (const auto source = Data::SmsSourceOf(username);
							!source.isEmpty()) {
						entries.push_back({
							u"Копировать «откуда»"_q,
							[=] {
								QGuiApplication::clipboard()->setText(source);
								show->showToast(u"Скопировано"_q);
							},
							&st::menuIconCopy,
						});
					}
					entries.push_back({
						u"Сбросить статус"_q,
						[=] {
							show->show(Ui::MakeConfirmBox({
								.text = u"Сбросить статус @"_q + username
									+ u"? Рассылка снова сможет ему "
										"написать."_q,
								.confirmed = [=](Fn<void()> &&close) {
									close();
									Data::DeleteSmsSendRecord(username);
									show->showToast(
										u"Статус @"_q + username
											+ u" сброшен"_q);
									(*refresh)();
								},
								.confirmText = u"Сбросить"_q,
							}));
						},
						&st::menuIconRestore,
						true,
					});
					return entries;
				});
			}
			if (int(matched.size()) > shown) {
				rows->add(object_ptr<Ui::SettingsButton>(
					rows,
					rpl::single(u"Показать ещё ("_q
						+ QString::number(int(matched.size()) - shown)
						+ u" осталось)"_q))
				)->setClickedCallback([=] {
					state->shown += kSentPageSize;
					(*refresh)();
				});
			}
		};
		*refresh = [=] {
			Ui::PostponeCall(box, fill);
		};
		// A running campaign (or anything else) writing to the registry
		// updates this list live instead of on reopen.
		Data::BasesChanged(
		) | rpl::on_next([=] {
			(*refresh)();
		}, box->lifetime());

		state->summary = box->addRow(object_ptr<Ui::FlatLabel>(
			box, QString(), st::boxLabel));
		if (Data::CountSmsSendRecords() > 0) {
			box->addRow(object_ptr<Ui::SettingsButton>(
				box, rpl::single(u"Очистить всё"_q))
			)->setClickedCallback([=] {
				show->show(Ui::MakeConfirmBox({
					.text = u"Удалить все "_q
						+ QString::number(Data::CountSmsSendRecords())
						+ u" записей? Рассылка снова сможет писать всем "_q
							"этим получателям."_q,
					.confirmed = [=](Fn<void()> &&close) {
						close();
						Data::ClearAllSmsSendRecords();
						show->show(
							MakeSmsSentUsersBox(show),
							Ui::LayerOption::CloseOther);
					},
					.confirmText = u"Удалить"_q,
				}));
			});
		}
		const auto search = box->addRow(object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			rpl::single(u"Поиск по @username, аккаунту или источнику"_q)));
		search->changes(
		) | rpl::on_next([=] {
			state->query = search->getLastText();
			state->shown = kSentPageSize;
			(*refresh)();
		}, search->lifetime());
		box->addRow(object_ptr<Ui::SettingsButton>(
			box, filterText->value())
		)->setClickedCallback([=] {
			state->filter = (state->filter + 1) % 9;
			state->shown = kSentPageSize;
			*filterText = filterLabel(state->filter);
			(*refresh)();
		});
		Ui::AddSkip(box->verticalLayout());
		Ui::AddDivider(box->verticalLayout());
		Ui::AddSkip(box->verticalLayout());

		state->rows = AddScrollableRowsArea(box->verticalLayout());
		fill();

		Ggtg::AddCloseCross(box);
	});
}

object_ptr<Ui::BoxContent> MakeScrapedGroupsDbBox(
		std::shared_ptr<Ui::Show> show,
		BasesActions actions) {
	return Box([=](not_null<Ui::GenericBox*> box) {
		box->setWidth(st::accountsBoxWidth);
		box->setTitle(rpl::single(u"База"_q));

		// Search + sort for both lists below. The sort mode outlives this
		// box (every edit rebuilds it), so it isn't reset on each save.
		struct ListState {
			bool refreshQueued = false;
			QString query;
			QPointer<Ui::VerticalLayout> groups;
			QPointer<Ui::VerticalLayout> users;
		};
		const auto list = std::make_shared<ListState>();
		const auto sortLabel = [](int mode) {
			switch (mode) {
			case 1: return u"Сортировка: по названию"_q;
			case 2: return u"Сортировка: по размеру"_q;
			case 3: return u"Сортировка: по остатку"_q;
			}
			return u"Сортировка: новые сверху"_q;
		};
		const auto sortText = box->lifetime().make_state<
			rpl::variable<QString>>(sortLabel(BaseSortMode()));

		const auto newGroups = [=] {
			ShowEditGroupsBaseBox(
				show,
				actions,
				Data::GenerateManualBaseId(),
				QString(),
				{},
				true);
		};
		const auto newUsers = [=] {
			ShowEditBaseBox(
				show,
				actions,
				Data::GenerateManualBaseId(),
				QString(),
				QStringList(),
				true);
		};
		const auto importFile = [=] {
			FileDialog::GetOpenPath(
				QPointer<QWidget>(box.get()),
				u"Импорт списка"_q,
				u"Текст и CSV (*.txt *.csv);;Все файлы (*)"_q,
				[=](FileDialog::OpenResult &&result) {
					if (result.paths.isEmpty()) {
						return;
					}
					const auto path = result.paths.front();
					auto file = QFile(path);
					if (!file.open(QIODevice::ReadOnly)) {
						Ggtg::Notice(show, u"Не удалось прочитать файл"_q);
						return;
					}
					auto lines = QString::fromUtf8(file.readAll()).split('\n');
					// A "username" header line (as our own CSV export
					// writes) is a label, not a recipient.
					if (!lines.isEmpty()
						&& lines.front().trimmed().compare(
							u"username"_q,
							Qt::CaseInsensitive) == 0) {
						lines.removeFirst();
					}
					const auto parsed = Data::ParseUsernames(lines);
					if (parsed.valid.isEmpty()) {
						Ggtg::Notice(show, 
							u"В файле не найдено ни одного юзернейма"_q);
						return;
					}
					Data::SaveScrapedGroupUsernames(
						Data::GenerateManualBaseId(),
						QFileInfo(path).completeBaseName(),
						parsed.valid);
					Ggtg::Notice(show, 
						u"Импортировано: "_q + QString::number(parsed.valid.size())
							+ u", отклонено: "_q
							+ QString::number(parsed.rejected.size())
							+ u", дубликатов: "_q
							+ QString::number(parsed.duplicates));
				});
		};
		// Housekeeping that rarely matters lives behind "Ещё".
		const auto menu = box->lifetime().make_state<
			base::unique_qptr<Ui::PopupMenu>>();
		const auto showMore = [=] {
			*menu = base::make_unique_q<Ui::PopupMenu>(
				box,
				st::popupMenuWithIcons);
			(*menu)->addAction(
				u"Отправленные ("_q
					+ QString::number(Data::CountSmsSendRecords()) + ')',
				[=] { show->show(MakeSmsSentUsersBox(show)); },
				&st::menuIconShowInChat);
			// Each kind of mark comes back into the queue on its own.
			for (const auto status : Data::SmsMarkKinds()) {
				const auto count = Data::CountSmsStatus(status);
				if (!count) {
					continue;
				}
				(*menu)->addAction(
					u"Вернуть в очередь: "_q + Data::SmsMarkTitle(status)
						+ u" ("_q + QString::number(count) + ')',
					[=] {
						const auto removed = Data::ClearSmsStatus(status);
						Ggtg::Notice(box->uiShow(),
							u"Возвращено в очередь («"_q
								+ Data::SmsMarkTitle(status) + u"»): "_q
								+ QString::number(removed));
					},
					&st::menuIconRestore);
			}
			if (Data::CountSmsResettable() > 0) {
				(*menu)->addAction(
					u"Вернуть в очередь все пометки ("_q
						+ QString::number(Data::CountSmsResettable()) + ')',
					[=] {
						Data::ClearAllSmsResettable();
						Ggtg::Notice(box->uiShow(),
							u"Все пометки сняты, получатели снова в очереди"_q);
					},
					&st::menuIconRestore);
			}
			(*menu)->popup(QCursor::pos());
		};
		const auto search = box->addRow(object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			rpl::single(u"Поиск по названию"_q)));

		const auto fill = [=] {
			const auto groupsBox = list->groups.data();
			const auto usersBox = list->users.data();
			if (!groupsBox || !usersBox) {
				return;
			}
			const Ggtg::ScrollKeeper keepScroll(groupsBox);
			groupsBox->clear();
			usersBox->clear();

			struct Row {
				Data::ScrapedGroupMeta meta;
				int remaining = 0;
			};
			const auto mode = BaseSortMode();
			const auto query = list->query.trimmed();
			auto groups = std::vector<Row>();
			auto users = std::vector<Row>();
			for (const auto &meta : Data::LoadScrapedGroupsIndex()) {
				if (!query.isEmpty()
					&& !meta.title.contains(query, Qt::CaseInsensitive)) {
					continue;
				}
				auto row = Row{ meta, 0 };
				if (mode == 3 && meta.kind == Data::BaseKind::Users) {
					row.remaining = Data::ComputeBaseStats(meta.id).remaining;
				}
				(meta.kind == Data::BaseKind::Groups ? groups : users)
					.push_back(std::move(row));
			}
			const auto sortRows = [&](std::vector<Row> &rows) {
				std::stable_sort(rows.begin(), rows.end(), [&](
						const Row &a,
						const Row &b) {
					switch (mode) {
					case 1: return a.meta.title.compare(
						b.meta.title, Qt::CaseInsensitive) < 0;
					case 2: return a.meta.count > b.meta.count;
					case 3: return (a.remaining != b.remaining)
						? (a.remaining > b.remaining)
						: (a.meta.count > b.meta.count);
					}
					return a.meta.savedAt > b.meta.savedAt;
				});
			};
			sortRows(groups);
			sortRows(users);

			// Who has answered, from the saved answers ("sender:name").
			auto answered = base::flat_set<QString>();
			for (const auto &[key, reply] : Core::GgtgAi::Feedback::LoadReplies()) {
				answered.emplace(key.mid(key.indexOf(':') + 1));
			}
			// Per base: the registry already knows which base each
			// recipient came from, no need to reread every base.
			auto repliedByBase = base::flat_map<uint64, int>();
			if (!answered.empty()) {
				for (const auto &record : Data::LoadAllSmsSendRecords()) {
					if (record.baseId && answered.contains(record.username)) {
						++repliedByBase[record.baseId];
					}
				}
			}

			// Right click on a base: open it or delete it right away. The
			// recipients already written to keep their "Откуда контакт"
			// (the base's name is saved in each send record).
			const auto showRowMenu = [=](
					const Data::ScrapedGroupMeta &meta,
					bool users,
					Fn<void()> open) {
				*menu = base::make_unique_q<Ui::PopupMenu>(
					box,
					st::popupMenuWithIcons);
				(*menu)->addAction(
					u"Открыть"_q,
					std::move(open),
					&st::menuIconEdit);
				// Shortcuts to what is otherwise inside the base's window.
				if (users && actions.sendToBase) {
					(*menu)->addAction(
						u"Отправить SMS по этой базе"_q,
						[=] { actions.sendToBase(meta.id); },
						&st::menuIconSend);
				}
				if (users) {
					(*menu)->addAction(
						u"Очистить от фейков и ботов…"_q,
						[=] { Ggtg::FakeDetector::ShowCleanBaseBox(show, meta.id, meta.title); },
						&st::menuIconCancel);
				}
				(*menu)->addAction(
					u"Копировать название"_q,
					[=] {
						QGuiApplication::clipboard()->setText(meta.title);
						show->showToast(u"Скопировано"_q);
					},
					&st::menuIconCopy);
				(*menu)->addSeparator();
				(*menu)->addAction(
					u"Удалить"_q,
					[=] {
						if (!Core::GuardAllows(
								show,
								Core::GuardedAction::EditBases)) {
							return;
						}
						show->show(Ui::MakeConfirmBox({
							.text = u"Удалить список \""_q + meta.title
								+ u"\" ("_q + QString::number(meta.count)
								+ u")? Это нельзя отменить.\n\n"
									"Кому из этого списка уже писали - у тех "
									"останется пометка, из какого списка "
									"они были."_q,
							.confirmed = [=](Fn<void()> &&close) {
								close();
								Data::DeleteScrapedGroup(meta.id);
							},
							.confirmText = u"Удалить"_q,
						}));
					},
					&st::menuIconDelete);
				(*menu)->popup(QCursor::pos());
			};

			for (const auto &row : groups) {
				const auto meta = row.meta;
				const auto openGroups = [=] {
					const auto entries = Data::LoadScrapedGroupEntries(meta.id);
					ShowEditGroupsBaseBox(
						show, actions, meta.id, meta.title, entries, false);
				};
				const auto item = groupsBox->add(object_ptr<Ggtg::InfoRow>(
					groupsBox,
					rpl::single(Ggtg::InfoRowData{
						.title = meta.title,
						.subtitle = u"Сохранено "_q
							+ FormatScrapedAt(meta.savedAt),
						.chip = QString::number(meta.count) + u" групп"_q,
					})),
					st::boxRowPadding);
				item->setClickedCallback(openGroups);
				item->setContextMenuCallback([=] {
					showRowMenu(meta, false, openGroups);
				});
			}
			if (groups.empty()) {
				groupsBox->add(object_ptr<Ggtg::EmptyState>(
					groupsBox,
					query.isEmpty()
						? u"Групп пока нет. Добавьте список кнопкой "
							"«+ Список групп» выше."_q
						: u"Ничего не найдено."_q));
			}

			for (const auto &row : users) {
				const auto meta = row.meta;
				// "Осталось N из M" straight in the list - the same numbers
				// "Отправка SMS" shows for this base, refreshed live as a
				// campaign writes to the registry.
				const auto found = repliedByBase.find(meta.id);
				const auto repliedCount = (found != repliedByBase.end())
					? found->second
					: 0;
				const auto rowData = [=] {
					const auto stats = Data::ComputeBaseStats(meta.id);
					return Ggtg::InfoRowData{
						.title = meta.title,
						.subtitle = u"Всего "_q + QString::number(stats.total)
							+ u", написано "_q + QString::number(stats.sent)
							+ (repliedCount
								? (u", ответили "_q
									+ QString::number(repliedCount))
								: QString())
							+ u" · "_q + FormatScrapedAt(meta.savedAt),
						.chip = (stats.remaining > 0)
							? (u"осталось "_q
								+ QString::number(stats.remaining))
							: (stats.sent >= stats.total)
							? u"все написано"_q
							: u"нет доступных"_q,
						.chipGood = (stats.remaining == 0
							&& stats.sent >= stats.total),
					};
				};
				const auto openUsers = [=] {
					const auto usernames
						= Data::LoadScrapedGroupUsernames(meta.id);
					ShowEditBaseBox(
						show, actions, meta.id, meta.title, usernames, false);
				};
				const auto item = usersBox->add(object_ptr<Ggtg::InfoRow>(
					usersBox,
					rpl::single(rowData()) | rpl::then(
						Data::BasesChanged() | rpl::map([=] {
							return rowData();
						}))),
					st::boxRowPadding);
				item->setClickedCallback(openUsers);
				item->setContextMenuCallback([=] {
					showRowMenu(meta, true, openUsers);
				});
			}
			if (users.empty()) {
				usersBox->add(object_ptr<Ggtg::EmptyState>(
					usersBox,
					query.isEmpty()
						? u"Баз пользователей пока нет. Соберите участников "
							"группы через Сквидвард, импортируйте файл или "
							"создайте список вручную."_q
						: u"Ничего не найдено."_q));
			}
		};

		search->changes(
		) | rpl::on_next([=] {
			list->query = search->getLastText();
			fill();
		}, search->lifetime());
		// One row of buttons for everything that adds a base.
		const auto bar = box->addRow(
			object_ptr<Ui::RpWidget>(box),
			st::boxRowPadding);
		const auto barButtons = std::make_shared<
			std::vector<not_null<Ui::RoundButton*>>>();
		const auto addBarButton = [=](const QString &text, Fn<void()> go) {
			const auto button = Ui::CreateChild<Ui::RoundButton>(
				bar,
				rpl::single(text),
				st::defaultLightButton);
			button->setClickedCallback(std::move(go));
			barButtons->push_back(button);
		};
		addBarButton(u"+ Список людей"_q, newUsers);
		addBarButton(u"+ Список групп"_q, newGroups);
		addBarButton(u"Импорт из файла"_q, importFile);
		if (actions.searchGroups) {
			addBarButton(u"Поиск групп"_q, actions.searchGroups);
		}
		addBarButton(u"Ещё ▾"_q, showMore);
		bar->widthValue(
		) | rpl::on_next([=](int width) {
			const auto skip = st::defaultVerticalListSkip;
			auto x = 0;
			auto y = 0;
			auto rowHeight = 0;
			for (const auto &button : *barButtons) {
				if (x > 0 && x + button->width() > width) {
					x = 0;
					y += rowHeight + skip;
				}
				button->moveToLeft(x, y);
				x += button->width() + skip;
				rowHeight = button->height();
			}
			bar->resize(width, y + rowHeight);
		}, bar->lifetime());
		Ui::AddSkip(box->verticalLayout());
		box->addRow(object_ptr<Ggtg::ActionRow>(
			box, sortText->value(), u"≡"_q, &st::menuIconReorder)
		)->setClickedCallback([=] {
			BaseSortMode() = (BaseSortMode() + 1) % 4;
			*sortText = sortLabel(BaseSortMode());
			fill();
		});
		Ui::AddSkip(box->verticalLayout());

		Ui::AddSubsectionTitle(box->verticalLayout(), rpl::single(u"Группы"_q));
		list->groups = box->addRow(object_ptr<Ui::VerticalLayout>(box));

		Ui::AddSkip(box->verticalLayout());
		Ui::AddDivider(box->verticalLayout());
		Ui::AddSkip(box->verticalLayout());

		Ui::AddSubsectionTitle(
			box->verticalLayout(), rpl::single(u"Пользователи"_q));
		list->users = box->addRow(object_ptr<Ui::VerticalLayout>(box));
		fill();

		// A base created while this box is open - by a group search, a batch
		// collection, an import or an operation elsewhere - shows up in the
		// lists straight away. Coalesced and postponed: one change can fire
		// several times, and rebuilding must not happen inside a handler.
		Data::BasesChanged(
		) | rpl::on_next([=] {
			if (std::exchange(list->refreshQueued, true)) {
				return;
			}
			Ui::PostponeCall(box, [=] {
				list->refreshQueued = false;
				fill();
			});
		}, box->lifetime());

		Ggtg::AddCloseCross(box);
	});
}

// Parses pasted "host:port[:login[:password]]" lines, one proxy per line.
[[nodiscard]] std::vector<MTP::ProxyData> ParseProxyLines(
		const QString &text) {
	auto result = std::vector<MTP::ProxyData>();
	for (const auto &rawLine : text.split('\n')) {
		const auto line = rawLine.trimmed();
		if (line.isEmpty()) {
			continue;
		}
		const auto parts = line.split(':');
		if (parts.size() < 2) {
			continue;
		}
		const auto port = parts[1].trimmed().toUShort();
		if (!port || parts[0].trimmed().isEmpty()) {
			continue;
		}
		auto entry = MTP::ProxyData();
		entry.type = MTP::ProxyData::Type::Socks5;
		entry.host = parts[0].trimmed();
		entry.port = port;
		if (parts.size() > 2) {
			entry.user = parts[2].trimmed();
		}
		if (parts.size() > 3) {
			entry.password = QStringList(parts.mid(3)).join(':').trimmed();
		}
		result.push_back(entry);
	}
	return result;
}

// Replace proxies: one account goes straight to the usual single-account
// proxy window (type it in or pick from a saved list); several accounts get
// either a pasted list (the n-th proxy goes to the n-th account) or one of
// the saved lists (entries are taken from it, so none is used twice).
void ShowBulkProxyBox(
		not_null<Window::SessionController*> controller,
		Accounts accounts) {
	if (accounts.size() == 1) {
		const auto account = accounts.front();
		if (!Main::AccountIsLive(account)) {
			Ggtg::Notice(controller->uiShow(), u"Нет активной сессии"_q);
			return;
		}
		if (Core::GuardAllows(
				controller->uiShow(),
				Core::GuardedAction::ReplaceProxy)) {
			controller->show(MakeAccountProxyBox(account));
		}
		return;
	}
	if (!Core::GuardAllows(
			controller->uiShow(),
			Core::GuardedAction::ReplaceProxy)) {
		return;
	}
	const auto apply = [=](std::vector<MTP::ProxyData> proxies, int listIndex) {
		const auto queue = std::make_shared<std::vector<MTP::ProxyData>>(
			std::move(proxies));
		const auto next = std::make_shared<size_t>(0);
		RunBulkAction(controller, accounts, [=](
				not_null<Main::Account*> account,
				StepDone done) {
			auto proxy = std::optional<MTP::ProxyData>();
			if (listIndex >= 0) {
				proxy = MTP::GlobalProxyPool().take(listIndex);
			} else if (*next < queue->size()) {
				proxy = (*queue)[(*next)++];
			}
			if (!proxy) {
				done(false, u"не хватило прокси"_q);
				return;
			}
			account->setProxyOverride(*proxy);
			// The new proxy may sit in another country: refit language and
			// time zone to it.
			AutoDeviceGeo(account);
			done(true, proxy->host + ':' + QString::number(proxy->port));
		});
	};
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setWidth(st::accountsBoxWidth);
		box->setTitle(rpl::single(
			u"Заменить прокси ("_q + QString::number(accounts.size()) + ')'));
		Ui::AddDividerText(
			box->verticalLayout(),
			rpl::single(
				u"Вставьте список прокси (host:port:логин:пароль, по одной "
					"строке): первая строка достанется первому аккаунту, "
					"вторая - второму и т.д. Если прокси меньше, чем "
					"аккаунтов, остальные не будут изменены. Либо возьмите "
					"прокси из сохранённого списка ниже."_q));
		const auto field = box->addRow(object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			Ui::InputField::Mode::MultiLine,
			rpl::single(u"host:port:логин:пароль"_q)));
		box->setFocusCallback([=] { field->setFocusFast(); });
		Ui::AddSkip(box->verticalLayout());
		const auto &lists = MTP::GlobalProxyPool().lists();
		auto any = false;
		for (auto i = 0; i != int(lists.size()); ++i) {
			if (lists[i].entries.empty()) {
				continue;
			}
			if (!std::exchange(any, true)) {
				Ui::AddSubsectionTitle(
					box->verticalLayout(),
					rpl::single(u"Из сохранённого списка"_q));
			}
			const auto name = lists[i].name.isEmpty()
				? u"(без названия)"_q
				: lists[i].name;
			box->addRow(object_ptr<Ggtg::TextRow>(
				box,
				rpl::single(name
					+ u" ("_q + QString::number(lists[i].entries.size())
					+ ')'))
			)->setClickedCallback([=] {
				box->closeBox();
				apply({}, i);
			});
		}
		box->addButton(rpl::single(u"Применить"_q), [=] {
			auto proxies = ParseProxyLines(field->getLastText());
			if (proxies.empty()) {
				Ggtg::Notice(controller->uiShow(), 
					u"Не найдено ни одной строки host:port"_q);
				return;
			}
			box->closeBox();
			apply(std::move(proxies), -1);
		});
		Ggtg::AddBackButton(box);
	}));
}

namespace {

[[nodiscard]] rpl::event_stream<> &TrashChangesStream() {
	static auto result = rpl::event_stream<>();
	return result;
}

// Removes one account from the app and every trace the app kept of it.
void DeleteAccountForever(not_null<Main::Account*> account) {
	namespace Sessions = Core::GgtgSessions;
	const auto userId = account->sessionExists()
		? QString::number(account->session().userId().bare)
		: QString();
	if (!userId.isEmpty()) {
		Sessions::RemoveUser(userId);
		for (const auto &entry
				: Sessions::StoredForUser(Sessions::LoadVault(), userId)) {
			Sessions::RemoveStored(entry.id);
		}
	}
	// ggtg: Do NOT call setTrashed(false) here - that wakes the account,
	// reconnects MTP, and updates badges immediately before wiping it out.
	account->setTwoFactorPassword(QString());
	// Local only, like a session swap: wipes the account's local data and
	// caches and drops it from the app (see Domain::removeRedundantAccounts).
	account->forcedLogOut();
	Core::GgtgLog::Info(
		u"Корзина"_q,
		u"аккаунт удалён навсегда, id "_q
			+ (userId.isEmpty() ? u"?"_q : userId));
}

} // namespace

rpl::producer<> AccountTrashChanges() {
	return TrashChangesStream().events();
}

void MoveAccountsToTrash(const std::vector<not_null<Main::Account*>> &list) {
	for (const auto &account : list) {
		account->setTrashed(true);
	}
	TrashChangesStream().fire({});
}

void RestoreAccountsFromTrash(
		const std::vector<not_null<Main::Account*>> &list) {
	for (const auto &account : list) {
		account->setTrashed(false);
	}
	TrashChangesStream().fire({});
}

void ConfirmDeleteAccountsForever(
		std::shared_ptr<Ui::Show> show,
		const std::vector<not_null<Main::Account*>> &list) {
	if (!Core::GuardAllows(show, Core::GuardedAction::RemoveAccount)) {
		return;
	}
	const auto active = &Core::App().activeAccount();
	auto names = QStringList();
	auto chosen = std::vector<not_null<Main::Account*>>();
	auto skippedActive = false;
	for (const auto &account : list) {
		if (account.get() == active) {
			skippedActive = true;
			continue;
		}
		chosen.push_back(account);
		if (names.size() < 5) {
			names.push_back(AccountLabel(account));
		}
	}
	if (chosen.empty()) {
		Ggtg::Notice(show, skippedActive
			? u"Этот аккаунт сейчас открыт - сначала откройте другой"_q
			: u"Отметьте хотя бы один аккаунт"_q);
		return;
	}
	auto text = (chosen.size() == 1)
		? (u"Удалить аккаунт «"_q + names.front() + u"» навсегда?"_q)
		: (u"Удалить навсегда аккаунтов: "_q
			+ QString::number(chosen.size()) + u"?\n"_q
			+ names.join('\n')
			+ (int(chosen.size()) > names.size() ? u"\n..."_q : QString()));
	text += u"\n\nАккаунт будет полностью удалён из программы вместе со "
		"всеми данными, кэшем и резервными сессиями. Это нельзя отменить."_q;
	if (skippedActive) {
		text += u"\n\nОткрытый сейчас аккаунт пропущен."_q;
	}
	// Accounts may go away while the box is open: keep plain pointers and
	// check they are still in the app when it is confirmed.
	auto raw = std::vector<Main::Account*>();
	for (const auto &account : chosen) {
		raw.push_back(account.get());
	}
	show->show(Ui::MakeConfirmBox({
		.text = text,
		.confirmed = [=](Fn<void()> &&close) {
			close();
			const auto live = Core::App().domain().orderedAccounts();
			const auto now = &Core::App().activeAccount();
			auto deleted = 0;
			for (const auto account : raw) {
				if (account == now
					|| ranges::find(live, not_null(account)) == end(live)) {
					continue;
				}
				DeleteAccountForever(account);
				++deleted;
			}
			TrashChangesStream().fire({});
			Ggtg::Notice(show, u"Удалено навсегда: "_q
				+ QString::number(deleted));
		},
		.confirmText = u"Удалить навсегда"_q,
		.confirmStyle = &st::attentionBoxButton,
		.title = u"Удаление аккаунта"_q,
	}));
}

object_ptr<Ui::BoxContent> MakeBulkActionsChooserBox(
		not_null<Window::SessionController*> controller,
		Accounts accounts) {
	return Box([=](not_null<Ui::GenericBox*> box) {
		box->setWidth(st::accountsBoxWidth);
		box->setTitle(rpl::single(
			u"Действие для "_q + QString::number(accounts.size()) + u" акк."_q));

		Ui::AddSkip(box->verticalLayout());
		Ui::AddSubsectionTitle(box->verticalLayout(), rpl::single(u"Профиль"_q));
		box->addRow(object_ptr<Ui::SettingsButton>(
			box, rpl::single(u"Фото"_q))
		)->setClickedCallback([=] {
			ShowBulkPhotoBox(controller, accounts);
		});
		box->addRow(object_ptr<Ui::SettingsButton>(
			box, rpl::single(u"Имя и фамилия"_q))
		)->setClickedCallback([=] {
			ShowBulkNameBox(controller, accounts);
		});
		box->addRow(object_ptr<Ui::SettingsButton>(
			box, rpl::single(u"@username"_q))
		)->setClickedCallback([=] {
			ShowBulkUsernameBox(controller, accounts);
		});
		box->addRow(object_ptr<Ui::SettingsButton>(
			box, rpl::single(u"Описание"_q))
		)->setClickedCallback([=] {
			ShowBulkBioBox(controller, accounts);
		});
		box->addRow(object_ptr<Ui::SettingsButton>(
			box, rpl::single(u"2FA пароль"_q))
		)->setClickedCallback([=] {
			ShowBulkPasswordBox(controller, accounts);
		});
		box->addRow(object_ptr<Ui::SettingsButton>(
			box, rpl::single(u"Прогрев аккаунтов (повышение траста)"_q))
		)->setClickedCallback([=] {
			box->closeBox();
			controller->uiShow()->showBox(MakeWarmupBox(controller, accounts));
		});

		Ui::AddSkip(box->verticalLayout());
		Ui::AddDivider(box->verticalLayout());
		Ui::AddSkip(box->verticalLayout());
		Ui::AddSubsectionTitle(box->verticalLayout(), rpl::single(u"Управление"_q));
		box->addRow(object_ptr<Ui::SettingsButton>(
			box, rpl::single(u"Переместить в папку"_q))
		)->setClickedCallback([=] {
			ShowBulkMoveToFolderBox(controller, accounts);
		});
		box->addRow(object_ptr<Ui::SettingsButton>(
			box, rpl::single(u"Деактивировать другие сессии"_q))
		)->setClickedCallback([=] {
			ShowBulkTerminateSessionsBox(controller, accounts);
		});
		box->addRow(object_ptr<Ui::SettingsButton>(
			box, rpl::single(u"Заменить прокси"_q))
		)->setClickedCallback([=] {
			box->closeBox();
			ShowBulkProxyBox(controller, accounts);
		});
		box->addRow(object_ptr<Ui::SettingsButton>(
			box, rpl::single(u"Подобрать новое устройство (случайное)"_q))
		)->setClickedCallback([=] {
			box->closeBox();
			RunBulkAction(controller, accounts, [=](
					not_null<Main::Account*> account,
					StepDone done) {
				RandomizeAccountDevice(account, done);
			});
		});
		box->addRow(object_ptr<Ui::SettingsButton>(
			box, rpl::single(u"Создать резервную сессию"_q))
		)->setClickedCallback([=] {
			ShowBulkBackupBox(controller, accounts);
		});
		box->addRow(object_ptr<Ui::SettingsButton>(
			box, rpl::single(u"Перейти на резервную сессию"_q))
		)->setClickedCallback([=] {
			ShowBulkSwapToBackupBox(controller, accounts);
		});
		box->addRow(object_ptr<Ui::SettingsButton>(
			box, rpl::single(u"Защита от угона (Анти-угон)"_q))
		)->setClickedCallback([=] {
			ShowBulkAntiTheftBox(controller, accounts);
		});
		box->addRow(object_ptr<Ui::SettingsButton>(
			box, rpl::single(u"Сменить номер (миграция)..."_q))
		)->setClickedCallback([=] {
			box->closeBox();
			Settings::ShowPhoneMigrationBox(controller, accounts);
		});
		box->addRow(object_ptr<Ui::SettingsButton>(
			box, rpl::single(u"Проверить SpamBot"_q))
		)->setClickedCallback([=] {
			// The same live window as every bulk action: a row per account
			// and what @spambot answered, a pause of a few seconds between
			// the accounts, "Процессы" and the log. Accounts without a
			// connection are skipped.
			auto live = Main::LiveAccounts(
				base::flat_set<not_null<Main::Account*>>(
					accounts.begin(),
					accounts.end()));
			if (live.empty()) {
				Ggtg::Notice(
					controller->uiShow(),
					u"Нет подключённых аккаунтов"_q);
				return;
			}
			box->closeBox();
			RunBulkAction(controller, std::move(live), [=](
					not_null<Main::Account*> account,
					StepDone done) {
				StepCheckSpam(account, done);
			}, BulkOptions{
				.title = u"Проверка SpamBot"_q,
				.pause = [] {
					return crl::time(3000)
						+ crl::time(base::RandomIndex(2001));
				},
				.failPrefix = QString(),
			});
		});
		box->addRow(object_ptr<Ui::SettingsButton>(
			box, rpl::single(u"Авто-апелляция @SpamBot"_q))
		)->setClickedCallback([=] {
			auto live = Main::LiveAccounts(
				base::flat_set<not_null<Main::Account*>>(
					accounts.begin(),
					accounts.end()));
			if (live.empty()) {
				Ggtg::Notice(
					controller->uiShow(),
					u"Нет подключённых аккаунтов"_q);
				return;
			}
			box->closeBox();
			RunBulkAction(controller, std::move(live), [=](
					not_null<Main::Account*> account,
					StepDone done) {
				StepAutoAppealSpam(account, done);
			}, BulkOptions{
				.title = u"Авто-апелляция @SpamBot"_q,
				.pause = [] {
					return crl::time(4000)
						+ crl::time(base::RandomIndex(2001));
				},
				.failPrefix = QString(),
			});
		});

		Ui::AddSkip(box->verticalLayout());
		Ui::AddDivider(box->verticalLayout());
		Ui::AddSkip(box->verticalLayout());
		box->addRow(object_ptr<Ui::SettingsButton>(
			box,
			rpl::single(u"Удалить (в корзину)"_q),
			st::settingsAttentionButton)
		)->setClickedCallback([=] {
			const auto show = box->uiShow();
			MoveAccountsToTrash(accounts);
			box->closeBox();
			Ggtg::Notice(show, u"Перемещено в корзину: "_q
				+ QString::number(accounts.size())
				+ u". Восстановить или удалить навсегда - в карточке "
					"«Корзина»."_q);
		});

		Ggtg::AddBackButton(box);
	});
}

namespace {

// One small "type a folder name" box, same shape as ShowBulkMoveToFolderBox
// but shown through a Ui::Show instead of a SessionController so it opens
// inside whichever window the account manager itself is in.
void ShowAccountFolderNamePrompt(
		std::shared_ptr<Ui::Show> show,
		const QString &title,
		const QString &description,
		const QString &initial,
		Fn<void(QString folder)> done) {
	show->showBox(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(title));
		Ui::AddDividerText(box->verticalLayout(), rpl::single(description));
		const auto field = box->addRow(object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			rpl::single(u"Имя папки"_q),
			initial));
		box->setFocusCallback([=] { field->setFocusFast(); });
		const auto save = [=] {
			const auto folder = field->getLastText().trimmed();
			box->closeBox();
			done(folder);
		};
		field->submits() | rpl::on_next(save, field->lifetime());
		box->addButton(rpl::single(u"Сохранить"_q), save);
		box->addButton(rpl::single(u"Отмена"_q), [=] { box->closeBox(); });
	}));
}

void ShowAccountProfileBox(
		std::shared_ptr<Ui::Show> show,
		not_null<Main::Account*> account,
		bool current,
		Fn<void()> onSaved = nullptr);

// Single-account profile editor & inspector: unified window with avatar,
// first/last name, username, bio, and all account details.
void ShowEditAccountBox(
		std::shared_ptr<Ui::Show> show,
		not_null<Main::Account*> account) {
	ShowAccountProfileBox(show, account, false);
}

// ---- "Менеджер аккаунтов": a table laid out like the reference web panel
// (tg-web-panel's manager page) - folder cards with counters on top, a
// search field, a selection bar, then one row per account with the columns
// № / Фото / Аккаунт / Статус / Папка / Прокси / СМС / Действия.

struct ManagerColumns {
	int check = 0;
	int number = 0;
	int numberWidth = 0;
	int photo = 0;
	int photoWidth = 0;
	// Name / @username / phone / ID, one under another.
	int account = 0;
	int accountWidth = 0;
	int folder = 0;
	int folderWidth = 0;
	int proxy = 0;
	int proxyWidth = 0;
	int status = 0;
	int statusWidth = 0;
	int sms = 0;
	int smsWidth = 0;
	// The «Открыть» button switching to the account.
	int open = 0;
	int openWidth = 0;
	// Last: move to the trash (or, inside it, restore / delete for good).
	int trash = 0;
	int trashWidth = 0;
	int totalWidth = 0;
	int rowHeight = 0;
};

// Four lines (name, @username, phone, ID) in a row.
[[nodiscard]] int ManagerRowHeight() {
	return st::semiboldFont->height
		+ 3 * st::normalFont->height
		+ style::ConvertScale(16);
}

// The width the table has to fill, set by MakeAccountManagerBox as its
// window is resized (0 = not known yet: use the preferred widths).
[[nodiscard]] int &ManagerViewportWidth() {
	static auto result = 0;
	return result;
}

// How wide the widest account / status text of the rows currently shown is
// (0 = not measured yet): the text columns are as wide as that, no wider.
struct ManagerContentWidths {
	int account = 0;
	int status = 0;
};
[[nodiscard]] ManagerContentWidths &ManagerContent() {
	static auto result = ManagerContentWidths();
	return result;
}

[[nodiscard]] int ManagerOpenWidth() {
	return st::semiboldFont->width(u"Открыть"_q) + style::ConvertScale(28);
}

// One icon in the normal list, two (restore, delete) inside the trash.
[[nodiscard]] int ManagerTrashWidth() {
	return style::ConvertScale(72);
}

// The «Открыть» pill, centered in its column.
[[nodiscard]] QRect ManagerOpenRect(const ManagerColumns &columns, int height) {
	const auto h = st::semiboldFont->height + style::ConvertScale(12);
	return QRect(
		columns.open,
		(height - h) / 2,
		columns.openWidth,
		h);
}

// The trash column's icon buttons: one in the middle, or two side by side.
[[nodiscard]] std::pair<QRect, QRect> ManagerTrashRects(
		const ManagerColumns &columns,
		int height,
		bool two) {
	const auto size = style::ConvertScale(32);
	const auto top = (height - size) / 2;
	if (!two) {
		return {
			QRect(
				columns.trash + (columns.trashWidth - size) / 2,
				top,
				size,
				size),
			QRect(),
		};
	}
	const auto gap = (columns.trashWidth - 2 * size) / 3;
	return {
		QRect(columns.trash + gap, top, size, size),
		QRect(columns.trash + 2 * gap + size, top, size, size),
	};
}

// Lays the columns out for the current viewport: with room to spare the
// extra width goes to the text columns, with too little they shrink towards
// their minimum, then "№" drops out, and only below that does the table
// start scrolling sideways.
[[nodiscard]] ManagerColumns ComputeManagerColumns() {
	const auto scaled = [](int value) { return style::ConvertScale(value); };
	const auto pad = scaled(12);
	const auto gap = scaled(8);
	struct Flex {
		int preferred;
		int minimum;
		int width;
	};
	const auto content = ManagerContent();
	const auto accountWidth = content.account > 0
		? content.account
		: scaled(170);
	const auto statusWidth = content.status > 0
		? content.status
		: scaled(110);
	auto account = Flex{
		accountWidth,
		std::min(accountWidth, scaled(105)),
		0 };
	auto folder = Flex{ scaled(100), scaled(65), 0 };
	auto proxy = Flex{ scaled(140), scaled(90), 0 };
	auto status = Flex{
		statusWidth,
		std::min(statusWidth, scaled(75)),
		0 };
	auto sms = Flex{ scaled(56), scaled(46), 0 };
	auto flex = std::array<Flex*, 5>{ &account, &folder, &proxy, &status, &sms };
	const auto available = ManagerViewportWidth();

	const auto fixedFor = [&](bool number, bool showSms) {
		// pad + check + photo + open + trash + gaps + pad
		auto total = pad + scaled(24) + scaled(30) + scaled(48)
			+ ManagerOpenWidth() + ManagerTrashWidth();
		auto columns = (showSms ? 5 : 4) + 2;
		if (number) {
			total += scaled(34);
			++columns;
		}
		return total + columns * gap;
	};
	const auto sum = [&](bool minimum, bool showSms) {
		auto result = 0;
		const auto count = showSms ? 5 : 4;
		for (auto i = 0; i != count; ++i) {
			result += minimum ? flex[i]->minimum : flex[i]->preferred;
		}
		return result;
	};
	auto showNumber = true;
	auto showSms = true;
	auto fixed = fixedFor(true, true);
	if (available > 0 && fixed + sum(true, true) > available) {
		showNumber = false;
		fixed = fixedFor(false, true);
	}
	if (available > 0 && fixed + sum(true, true) > available) {
		showSms = false;
		fixed = fixedFor(false, false);
	}
	const auto activeFlexCount = showSms ? 5 : 4;
	const auto sumPreferred = sum(false, showSms);
	const auto sumMinimal = sum(true, showSms);
	const auto preferred = fixed + sumPreferred;
	const auto minimal = fixed + sumMinimal;
	auto total = preferred;
	if (available > 0) {
		total = std::max(available, minimal);
	}
	// Distribute: from the preferred widths, the difference to `total`
	// grows or shrinks every text column in proportion to its preferred
	// width (shrinking never goes below the minimum).
	const auto room = total - fixed;
	if (room >= sumPreferred) {
		// Spare room is shared out between the text columns in proportion
		// to their content, so the table always spans the whole window.
		const auto extra = room - sumPreferred;
		const auto base = std::max(sumPreferred, 1);
		auto given = 0;
		for (auto i = 0; i != activeFlexCount; ++i) {
			const auto add = extra * flex[i]->preferred / base;
			flex[i]->width = flex[i]->preferred + add;
			given += add;
		}
		flex[0]->width += extra - given;
	} else {
		const auto lack = sumPreferred - room;
		const auto slack = sumPreferred - sumMinimal;
		auto taken = 0;
		for (auto i = 0; i != activeFlexCount; ++i) {
			const auto cut = slack
				? lack * (flex[i]->preferred - flex[i]->minimum) / slack
				: 0;
			flex[i]->width = flex[i]->preferred - cut;
			taken += cut;
		}
		flex[0]->width -= std::max(lack - taken, 0);
		flex[0]->width = std::max(flex[0]->width, flex[0]->minimum);
	}
	if (!showSms) {
		sms.width = 0;
	}

	auto result = ManagerColumns();
	auto x = pad;
	result.check = x;
	x += scaled(30) + gap;
	result.number = x;
	result.numberWidth = showNumber ? scaled(34) : 0;
	if (showNumber) {
		x += result.numberWidth + gap;
	}
	result.photo = x;
	result.photoWidth = scaled(48);
	x += result.photoWidth + gap;
	result.account = x;
	result.accountWidth = account.width;
	x += account.width + gap;
	result.folder = x;
	result.folderWidth = folder.width;
	x += folder.width + gap;
	result.proxy = x;
	result.proxyWidth = proxy.width;
	x += proxy.width + gap;
	result.status = x;
	result.statusWidth = status.width;
	x += status.width + gap;
	result.sms = x;
	result.smsWidth = showSms ? sms.width : 0;
	if (showSms) {
		x += result.smsWidth + gap;
	}
	result.open = x;
	result.openWidth = ManagerOpenWidth();
	x += result.openWidth + gap;
	result.trash = x;
	result.trashWidth = ManagerTrashWidth();
	x += result.trashWidth;
	result.totalWidth = std::max(total, x);
	result.rowHeight = ManagerRowHeight();
	return result;
}

// Name, @username, phone and ID stacked in one cell, centered as a block.
void PaintAccountCell(
		Painter &p,
		int x,
		int width,
		int height,
		const QString &name,
		const QString &username,
		QColor usernameColor,
		const QString &phone,
		const QString &id,
		UserData *user = nullptr,
		Ui::PeerBadge *badge = nullptr,
		Fn<void()> repaint = nullptr) {
	if (width <= 0) {
		return;
	}
	const auto &bold = st::semiboldFont;
	const auto &normal = st::normalFont;
	auto y = (height - bold->height - 3 * normal->height) / 2;
	const auto line = [&](
			const style::font &font,
			QColor color,
			const QString &text) {
		p.setFont(font);
		p.setPen(color);
		p.drawText(
			QRect(x, y, width, font->height),
			Qt::AlignLeft | Qt::AlignVCenter,
			font->elided(text, width));
		y += font->height;
	};
	// Telegram's own Premium star / emoji status right after the name.
	const auto badgeWidth = (user && badge)
		? Ggtg::PremiumBadgeWidth(user)
		: 0;
	const auto nameTop = y;
	line(bold, st::windowBoldFg->c, name);
	if (badgeWidth > 0) {
		Ggtg::PaintPremiumBadge(
			p,
			*badge,
			user,
			QRect(x, nameTop, width, bold->height),
			std::min(bold->width(name), width - badgeWidth),
			x + width,
			repaint);
	}
	line(normal, usernameColor, username);
	line(normal, st::windowBoldFg->c, phone);
	line(normal, st::windowSubTextFg->c, id);
}


// The words of `text` in `count` lines, split where the widest line is the
// narrowest ("Спам-блок до" / "11.10 09:29"); empty if there are fewer words.
[[nodiscard]] QStringList BalancedLines(
		const QStringList &words,
		int count,
		const style::font &font) {
	if (count > int(words.size())) {
		return {};
	}
	auto best = QStringList();
	auto bestWidth = std::numeric_limits<int>::max();
	auto cuts = std::vector<int>(count - 1);
	const auto search = [&](auto &&self, int index, int from) -> void {
		if (index == count - 1) {
			auto lines = QStringList();
			auto widest = 0;
			auto start = 0;
			for (auto i = 0; i != count; ++i) {
				const auto end = (i < count - 1)
					? cuts[i]
					: int(words.size());
				lines.push_back(words.mid(start, end - start).join(u' '));
				widest = std::max(widest, font->width(lines.back()));
				start = end;
			}
			if (widest < bestWidth) {
				bestWidth = widest;
				best = lines;
			}
			return;
		}
		for (auto cut = from + 1; cut < int(words.size()); ++cut) {
			cuts[index] = cut;
			self(self, index + 1, cut);
		}
	};
	search(search, 0, 0);
	return best;
}

// The status pill, centered in its column. A long text goes onto two or
// three lines, and if it still doesn't fit the font gets smaller; only
// then is it elided. Returns the pill's rectangle (the click target).
QRect PaintStatusBadge(
		QPainter &p,
		const ManagerColumns &columns,
		int baseHeight,
		const QString &text,
		QColor color,
		float64 fillAlpha) {
	const auto padding = style::ConvertScale(8);
	const auto available = columns.statusWidth - 2 * padding;
	const auto maxHeight = baseHeight - 2 * style::ConvertScale(6);
	const auto words = text.split(u' ', Qt::SkipEmptyParts);
	const auto normal = st::normalFont;
	const auto smallest = std::max(
		normal->f.pixelSize() - style::ConvertScale(4),
		style::ConvertScale(9));

	auto font = normal;
	auto lines = QStringList{ text };
	auto found = false;
	for (auto size = normal->f.pixelSize(); !found && size >= smallest; --size) {
		font = (size == normal->f.pixelSize())
			? normal
			: style::font(size, normal->flags(), normal->family());
		for (auto count = 1; count <= 3; ++count) {
			if (count * font->height + padding > maxHeight) {
				break;
			}
			const auto split = (count == 1)
				? QStringList{ text }
				: BalancedLines(words, count, font);
			if (split.isEmpty()) {
				break;
			}
			const auto fits = std::all_of(split.begin(), split.end(), [&](
					const QString &line) {
				return font->width(line) <= available;
			});
			if (fits) {
				lines = split;
				found = true;
				break;
			}
		}
	}
	if (!found) {
		lines = QStringList{ text };
	}

	auto textWidth = 0;
	for (const auto &line : lines) {
		textWidth = std::max(textWidth, std::min(font->width(line), available));
	}
	const auto badgeWidth = textWidth + 2 * padding;
	const auto badgeHeight = int(lines.size()) * font->height + padding;
	const auto rect = QRect(
		columns.status + (columns.statusWidth - badgeWidth) / 2,
		(baseHeight - badgeHeight) / 2,
		badgeWidth,
		badgeHeight);
	auto fill = color;
	fill.setAlphaF(fillAlpha);
	auto hq = PainterHighQualityEnabler(p);
	p.setPen(Qt::NoPen);
	p.setBrush(fill);
	p.drawRoundedRect(rect, padding, padding);
	p.setPen(color);
	p.setFont(font);
	auto top = rect.y() + padding / 2;
	for (const auto &line : lines) {
		p.drawText(
			QRect(rect.x() + padding, top, textWidth, font->height),
			Qt::AlignCenter,
			font->elided(line, textWidth));
		top += font->height;
	}
	return rect;
}

QRect PaintFolderCell(
		QPainter &p,
		const ManagerColumns &columns,
		int baseHeight,
		const QString &tag,
		bool hovered) {
	if (columns.folderWidth <= 0) {
		return QRect();
	}
	const auto padding = style::ConvertScale(8);
	const auto available = columns.folderWidth - 2 * padding;
	const auto text = tag.isEmpty() ? u"Без папки"_q : tag;
	const auto font = st::normalFont;
	const auto textW = std::min(font->width(text), available);
	const auto h = font->height + style::ConvertScale(8);
	const auto w = std::min(columns.folderWidth, textW + 2 * padding);
	const auto rect = QRect(
		columns.folder,
		(baseHeight - h) / 2,
		w,
		h);
	const auto radius = style::ConvertScale(6);
	auto hq = PainterHighQualityEnabler(p);
	p.setPen(Qt::NoPen);
	if (hovered) {
		auto tint = st::windowBgActive->c;
		tint.setAlphaF(0.24);
		p.setBrush(tint);
	} else if (!tag.isEmpty()) {
		auto tint = st::windowBgActive->c;
		tint.setAlphaF(0.12);
		p.setBrush(tint);
	} else {
		auto fill = st::windowBoldFg->c;
		fill.setAlphaF(0.06);
		p.setBrush(fill);
	}
	p.drawRoundedRect(rect, radius, radius);

	p.setFont(font);
	p.setPen(!tag.isEmpty() ? st::windowActiveTextFg->c : st::windowSubTextFg->c);
	p.drawText(
		QRect(rect.x() + padding, rect.y(), w - 2 * padding, rect.height()),
		Qt::AlignLeft | Qt::AlignVCenter,
		font->elided(text, w - 2 * padding));
	return rect;
}

QRect PaintProxyCell(
		QPainter &p,
		const ManagerColumns &columns,
		int baseHeight,
		const MTP::ProxyData &proxy,
		int dcState,
		bool offline,
		bool hovered) {
	if (columns.proxyWidth <= 0) {
		return QRect();
	}
	const auto padding = style::ConvertScale(4);
	const auto cellH = st::semiboldFont->height + st::normalFont->height + style::ConvertScale(4);
	const auto rect = QRect(
		columns.proxy,
		(baseHeight - cellH) / 2,
		columns.proxyWidth,
		cellH);

	if (hovered) {
		auto tint = st::windowBgActive->c;
		tint.setAlphaF(0.18);
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		p.setBrush(tint);
		p.drawRoundedRect(
			rect.marginsAdded(QMargins(padding, padding / 2, padding, padding / 2)),
			style::ConvertScale(6),
			style::ConvertScale(6));
	}

	const auto hasProxy = MTP::HasRealProxy(proxy);
	const auto dotSize = style::ConvertScale(7);
	const auto dotY = rect.y() + (st::semiboldFont->height - dotSize) / 2 + style::ConvertScale(1);
	const auto dotX = rect.x() + padding;

	// Status dot
	{
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		if (!hasProxy) {
			p.setBrush(QColor(255, 159, 10)); // orange
		} else if (offline) {
			p.setBrush(QColor(120, 140, 160)); // gray-blue
		} else if (dcState == MTP::ConnectedState) {
			p.setBrush(QColor(76, 217, 100)); // green
		} else if (dcState == MTP::ConnectingState) {
			p.setBrush(QColor(255, 214, 10)); // yellow
		} else {
			p.setBrush(QColor(255, 159, 10)); // orange
		}
		p.drawEllipse(QRect(dotX, dotY, dotSize, dotSize));
	}

	const auto textX = dotX + dotSize + style::ConvertScale(6);
	const auto textW = std::max(0, rect.width() - (textX - rect.x()) - padding);

	if (hasProxy) {
		const auto hostPort = proxy.host + ':' + QString::number(proxy.port);
		p.setFont(st::semiboldFont);
		p.setPen(st::windowBoldFg);
		p.drawText(
			QRect(textX, rect.y(), textW, st::semiboldFont->height),
			Qt::AlignLeft | Qt::AlignVCenter,
			st::semiboldFont->elided(hostPort, textW));

		const auto typeStr = (proxy.type == MTP::ProxyData::Type::Socks5)
			? u"SOCKS5"_q
			: (proxy.type == MTP::ProxyData::Type::Http)
			? u"HTTP"_q
			: u"MTProto"_q;
		p.setFont(st::normalFont);
		p.setPen(st::windowSubTextFg);
		p.drawText(
			QRect(textX, rect.y() + st::semiboldFont->height + style::ConvertScale(2), textW, st::normalFont->height),
			Qt::AlignLeft | Qt::AlignVCenter,
			typeStr);
	} else {
		p.setFont(st::semiboldFont);
		p.setPen(QColor(255, 159, 10));
		p.drawText(
			QRect(textX, rect.y(), textW, st::semiboldFont->height),
			Qt::AlignLeft | Qt::AlignVCenter,
			u"Нет прокси"_q);

		p.setFont(st::normalFont);
		p.setPen(st::windowSubTextFg);
		p.drawText(
			QRect(textX, rect.y() + st::semiboldFont->height + style::ConvertScale(2), textW, st::normalFont->height),
			Qt::AlignLeft | Qt::AlignVCenter,
			u"нажмите для выбора"_q);
	}

	return rect;
}

// ---- What state an account is in ----
//
// One status per account, the most serious thing known about it. `rank` is
// the sort key of the manager's "Статус" column: low = ready to work with,
// high = trouble. From the app's own data:
//   0 Свободен            session live, connected, SpamBot shows no limits
//   1 Не проверен         live and connected, SpamBot never asked
//   2 Подключается        the connection is still being made
//   3 Нет связи           no connection to Telegram
//   4 Спам-блок до ...    SpamBot: limited for now
//   5 Спам-блок навсегда  SpamBot: limited for good
//   6 Заморожен           Telegram froze the account
//   7 Сессия разорвана    the session was ended (journal; not in the app)
//   8 Удалён              account deleted
//   9 Забанен             account banned
struct AccountStatus {
	int rank = 0;
	QString text;
	QColor color;
	QString detail;
};

// A freeze is known to the app only while the session is live (it comes
// with the server's config). To keep "Заморожен" after the account goes to
// the trash (it sleeps, and after a restart nothing says it was frozen),
// the freeze is remembered in the account's own storage.
struct SavedFreeze {
	int since = 0;
	int until = 0;
	QString appealUrl;

	[[nodiscard]] explicit operator bool() const {
		return since != 0 || until != 0;
	}
};

[[nodiscard]] SavedFreeze ReadSavedFreeze(not_null<Main::Account*> account) {
	const auto parts = account->local().readPref<QString>(
		std::string_view("ggtg_frozen")
	).split('|');
	auto result = SavedFreeze();
	if (parts.size() >= 2) {
		result.since = parts[0].toInt();
		result.until = parts[1].toInt();
		if (parts.size() >= 3) {
			result.appealUrl = parts.mid(2).join('|');
		}
	}
	return result;
}

void RememberFreeze(
		not_null<Main::Account*> account,
		const SavedFreeze &freeze) {
	const auto now = ReadSavedFreeze(account);
	if (!freeze) {
		if (now) {
			account->local().clearPref(std::string_view("ggtg_frozen"));
		}
		return;
	}
	if (now.since == freeze.since
		&& now.until == freeze.until
		&& now.appealUrl == freeze.appealUrl) {
		return;
	}
	account->local().writePref<QString>(
		std::string_view("ggtg_frozen"),
		QString::number(freeze.since)
			+ '|' + QString::number(freeze.until)
			+ '|' + freeze.appealUrl);
}

[[nodiscard]] AccountStatus FrozenStatus(const SavedFreeze &freeze) {
	auto detail = u"Telegram заморозил аккаунт"_q;
	if (freeze.since) {
		detail += u" с "_q + base::unixtime::parse(freeze.since)
			.toString(u"dd.MM.yyyy"_q);
	}
	if (freeze.until) {
		detail += u", до "_q + base::unixtime::parse(freeze.until)
			.toString(u"dd.MM.yyyy"_q);
	}
	detail += u". Пока он заморожен, писать и отправлять нельзя."_q;
	if (!freeze.appealUrl.isEmpty()) {
		detail += u" Обжалование: "_q + freeze.appealUrl;
	}
	return { 6, u"Заморожен"_q, QColor(90, 200, 250), detail };
}

// What is known about an account WITHOUT asking the connection: a freeze
// (live or remembered) and SpamBot's last answer. Works for the accounts in
// the trash too, so their status does not turn into a bare "В корзине".
[[nodiscard]] std::optional<AccountStatus> KnownAccountTrouble(
		not_null<Main::Account*> account,
		bool remember = true) {
	if (!account->sessionExists()) {
		return std::nullopt;
	}
	const auto session = &account->session();
	const auto userId = session->user()->id.value;
	auto freeze = SavedFreeze();
	if (const auto live = session->frozen()) {
		freeze = SavedFreeze{
			.since = live.since,
			.until = live.until,
			.appealUrl = live.appealUrl,
		};
		if (remember) {
			RememberFreeze(account, freeze);
		}
	} else if (account->trashed()) {
		freeze = ReadSavedFreeze(account);
		if (freeze
			&& freeze.until
			&& freeze.until < QDateTime::currentSecsSinceEpoch()) {
			freeze = SavedFreeze();
		}
	} else if (account->mtp().dcstate() == MTP::ConnectedState) {
		// Connected and the server says "not frozen": forget the old mark.
		if (remember) {
			RememberFreeze(account, SavedFreeze());
		}
	}
	if (freeze) {
		return FrozenStatus(freeze);
	}
	const auto spam = Data::FindAccountSpamStatus(userId);
	if (spam && spam->state == Data::AccountSpamState::Permanent) {
		return AccountStatus{
			5,
			u"Спам-блок навсегда"_q,
			QColor(255, 69, 58),
			u"SpamBot: аккаунт ограничен без срока - писать незнакомым "
				"нельзя."_q,
		};
	}
	if (spam
		&& spam->state == Data::AccountSpamState::Temporary
		&& spam->until > QDateTime::currentSecsSinceEpoch()) {
		const auto until = QDateTime::fromSecsSinceEpoch(spam->until)
			.toString(u"dd.MM.yyyy HH:mm"_q);
		return AccountStatus{
			4,
			u"Спам-блок до "_q + QDateTime::fromSecsSinceEpoch(spam->until)
				.toString(u"dd.MM HH:mm"_q),
			QColor(255, 159, 10),
			u"SpamBot: временное ограничение до "_q + until + '.',
		};
	}
	return std::nullopt;
}

[[nodiscard]] AccountStatus LiveAccountStatus(
		not_null<Main::Account*> account) {
	if (account->trashed()) {
		// The status stays: a frozen account in the trash is still
		// "Заморожен", one with a spam block still has it.
		if (auto trouble = KnownAccountTrouble(account)) {
			trouble->detail = u"Аккаунт в корзине (спит без соединения). "_q
				+ trouble->detail;
			return *trouble;
		}
		return {
			1,
			u"В корзине"_q,
			QColor(142, 142, 147),
			u"Аккаунт в корзине: он спит без соединения, поэтому проверки и "
				"спам-блок для него не считаются."_q,
		};
	}
	if (account->alwaysOffline()) {
		return {
			1,
			u"Офлайн"_q,
			QColor(120, 140, 160),
			u"Режим сна: аккаунт отключён от сети (офлайн)."_q,
		};
	}
	if (!account->sessionExists()) {
		return {
			2,
			u"Загружается"_q,
			QColor(255, 214, 10),
			u"Аккаунт ещё загружается."_q,
		};
	}
	if (!MTP::HasRealProxy(account->proxyOverride())) {
		return {
			3,
			u"Нет прокси"_q,
			QColor(255, 159, 10),
			u"У аккаунта не задан прокси, поэтому он не подключается. "
				"Задайте прокси (в этом окне, строка «Прокси», или "
				"«Массовое действие...» → «Заменить прокси») - аккаунт "
				"подключится сам."_q,
		};
	}
	if (const auto trouble = KnownAccountTrouble(account)) {
		return *trouble;
	}
	const auto spam = Data::FindAccountSpamStatus(
		account->session().user()->id.value);
	const auto dcState = account->mtp().dcstate();
	if (dcState == MTP::DisconnectedState) {
		return {
			3,
			u"Нет связи"_q,
			QColor(255, 159, 10),
			u"Нет соединения с Telegram (проверьте прокси и интернет)."_q,
		};
	} else if (dcState != MTP::ConnectedState) {
		return {
			2,
			u"Подключается"_q,
			QColor(255, 214, 10),
			u"Соединение с Telegram устанавливается."_q,
		};
	}
	if (!spam) {
		return {
			1,
			u"Не проверен"_q,
			st::windowSubTextFg->c,
			u"Сессия работает, но SpamBot ещё не проверял аккаунт."_q,
		};
	}
	return {
		0,
		u"Свободен"_q,
		QColor(76, 217, 100),
		u"Сессия работает, соединение есть, SpamBot ограничений не "
			"показал."_q,
	};
}

// For an account that is no longer in the app: what the journal knows.
[[nodiscard]] AccountStatus LostAccountStatus(
		const Core::GgtgSessions::LostSession &entry) {
	namespace Sessions = Core::GgtgSessions;
	if (entry.reason == u"USER_DEACTIVATED_BAN"_q) {
		return {
			9,
			u"Забанен"_q,
			QColor(200, 30, 30),
			Sessions::ReasonTitle(entry.reason),
		};
	} else if (entry.reason == u"USER_DEACTIVATED"_q) {
		return {
			8,
			u"Удалён"_q,
			QColor(142, 142, 147),
			Sessions::ReasonTitle(entry.reason),
		};
	} else if (entry.reason == u"SWAP"_q) {
		return {
			7,
			u"Переключение"_q,
			QColor(191, 90, 242),
			Sessions::ReasonTitle(entry.reason),
		};
	}
	return {
		7,
		u"Сессия разорвана"_q,
		QColor(191, 90, 242),
		Sessions::ReasonTitle(entry.reason),
	};
}

// Measures the widest account block and status badge among the rows about
// to be shown, so the table's columns fit them (see ComputeManagerColumns).
void MeasureManagerContent(
		const std::vector<not_null<Main::Account*>> &live,
		const std::vector<Core::GgtgSessions::LostSession> &lost,
		const not_null<Main::Account*> current) {
	const auto &bold = st::semiboldFont;
	const auto &normal = st::normalFont;
	auto account = bold->width(u"Аккаунт"_q);
	auto status = bold->width(u"Статус ▼"_q);
	const auto statusWidth = [&](const QString &text) {
		status = std::max(
			status,
			normal->width(text) + 2 * style::ConvertScale(8));
	};
	const auto block = [&](
			QString name,
			const QString &username,
			QString phone,
			const QString &id,
			int badgeWidth = 0) {
		if (name.isEmpty()) {
			name = u"Без имени"_q;
		}
		if (!phone.isEmpty() && !phone.startsWith('+')) {
			phone = '+' + phone;
		}
		account = std::max({
			account,
			bold->width(name) + badgeWidth,
			normal->width(username.isEmpty()
				? u"@no_user"_q
				: (u"@"_q + username)),
			normal->width(phone),
			normal->width(u"ID "_q + id) });
	};
	for (const auto &item : live) {
		const auto user = item->sessionExists()
			? item->session().user().get()
			: nullptr;
		auto name = user ? user->name() : u"(нет сессии)"_q;
		if (item == current) {
			name += u" (текущий)"_q;
		}
		block(
			name,
			user ? user->username() : QString(),
			user ? user->phone() : QString(),
			user ? QString::number(user->id.value) : QString(),
			user ? Ggtg::PremiumBadgeWidth(user) : 0);
		statusWidth(LiveAccountStatus(item).text);
	}
	for (const auto &entry : lost) {
		block(entry.name, entry.username, entry.phone, entry.userId);
		statusWidth(LostAccountStatus(entry).text);
	}
	ManagerContent().account = account + style::ConvertScale(4);
	ManagerContent().status = status;
}

// "Свободны 120 · Не проверены 40 · Спам-блок 5 ..." - how many accounts are
// in each state, zero groups left out.
[[nodiscard]] QString StatusSummaryText(
		const std::array<int, 10> &counts) {
	struct Group {
		QString title;
		int count;
	};
	const auto groups = std::array<Group, 9>{ {
		{ u"Свободны"_q, counts[0] },
		{ u"Не проверены"_q, counts[1] },
		{ u"Подключаются"_q, counts[2] },
		{ u"Нет связи"_q, counts[3] },
		{ u"Спам-блок"_q, counts[4] + counts[5] },
		{ u"Заморожены"_q, counts[6] },
		{ u"Сессия разорвана"_q, counts[7] },
		{ u"Удалены"_q, counts[8] },
		{ u"Забанены"_q, counts[9] },
	} };
	auto parts = QStringList();
	for (const auto &group : groups) {
		if (group.count > 0) {
			parts.push_back(group.title + ' ' + QString::number(group.count));
		}
	}
	return parts.isEmpty()
		? u"Нет аккаунтов"_q
		: parts.join(u" · "_q);
}

// The row's tick box. Its widget is exactly the square that is drawn (no
// invisible margin around it, unlike Ui::Checkbox), so what you see is what
// you can click.
class ManagerCheck final : public Ui::AbstractButton {
public:
	ManagerCheck(not_null<QWidget*> parent, bool checked)
	: AbstractButton(parent.get())
	, _checked(checked) {
		const auto size = style::ConvertScale(20);
		resize(size, size);
		setPointerCursor(true);
		setClickedCallback([=] {
			setChecked(!_checked);
		});
	}

	[[nodiscard]] bool checked() const {
		return _checked;
	}

	void setChecked(
			bool checked,
			Ui::Checkbox::NotifyAboutChange notify
				= Ui::Checkbox::NotifyAboutChange::Notify) {
		if (_checked == checked) {
			return;
		}
		_checked = checked;
		update();
		if (notify == Ui::Checkbox::NotifyAboutChange::Notify) {
			_changes.fire_copy(checked);
		}
	}

	[[nodiscard]] rpl::producer<bool> checkedChanges() const {
		return _changes.events();
	}

	// Checked shows a cross ("clear the selection") instead of a tick.
	void setCrossWhenChecked(bool cross) {
		_crossWhenChecked = cross;
		update();
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		auto p = QPainter(this);
		auto hq = PainterHighQualityEnabler(p);
		const auto line = style::ConvertScale(2);
		const auto half = line / 2.;
		const auto box = QRectF(rect()).marginsRemoved(
			QMarginsF(half, half, half, half));
		const auto radius = style::ConvertScale(5);
		if (_checked) {
			p.setPen(Qt::NoPen);
			p.setBrush(st::windowBgActive);
			p.drawRoundedRect(box, radius, radius);
			p.setPen(QPen(
				st::windowFgActive->c,
				line,
				Qt::SolidLine,
				Qt::RoundCap,
				Qt::RoundJoin));
			p.setBrush(Qt::NoBrush);
			const auto w = width();
			const auto h = height();
			auto path = QPainterPath();
			if (_crossWhenChecked) {
				path.moveTo(w * 0.32, h * 0.32);
				path.lineTo(w * 0.68, h * 0.68);
				path.moveTo(w * 0.68, h * 0.32);
				path.lineTo(w * 0.32, h * 0.68);
			} else {
				path.moveTo(w * 0.27, h * 0.52);
				path.lineTo(w * 0.44, h * 0.69);
				path.lineTo(w * 0.75, h * 0.32);
			}
			p.drawPath(path);
		} else {
			auto border = st::windowSubTextFg->c;
			border.setAlphaF(isOver() ? 1. : 0.7);
			p.setPen(QPen(border, line));
			p.setBrush(Qt::NoBrush);
			p.drawRoundedRect(box, radius, radius);
		}
	}

private:
	bool _checked = false;
	bool _crossWhenChecked = false;
	rpl::event_stream<bool> _changes;

};

class AccountTableHeader final
	: public Ui::RpWidget
	, public Ui::AbstractTooltipShower {
public:
	AccountTableHeader(
		not_null<QWidget*> parent,
		Fn<void(bool)> onSelectAll,
		Fn<void()> onSortStatus,
		Fn<void()> onSortSms)
	: RpWidget(parent.get())
	, _onSortStatus(std::move(onSortStatus))
	, _onSortSms(std::move(onSortSms)) {
		setMouseTracking(true);
		const auto columns = ComputeManagerColumns();
		resize(columns.totalWidth, style::ConvertScale(34));
		_checkbox = Ui::CreateChild<ManagerCheck>(this, false);
		_checkbox->setCrossWhenChecked(true);
		_checkbox->moveToLeft(
			columns.check + style::ConvertScale(5),
			(height() - _checkbox->height()) / 2);
		_checkbox->checkedChanges(
		) | rpl::on_next(std::move(onSelectAll), lifetime());
	}

	// 0 = the app's own order, 1 = best first (free ... banned), 2 = worst
	// first.
	void setSort(int mode) {
		_sort = mode;
		update();
	}

	// The hint shown over "Статус": how many accounts are in which state
	// and how the sorting works.
	void setTooltipProvider(Fn<QString()> provider) {
		_tooltip = std::move(provider);
	}

	// Counted at the moment the hint is shown, not on a timer.
	QString tooltipText() const override {
		return _tooltip ? _tooltip() : QString();
	}
	QPoint tooltipPos() const override {
		return QCursor::pos();
	}
	bool tooltipWindowActive() const override {
		return Ui::AppInFocus() && Ui::InFocusChain(window());
	}

	// The same for the sent-SMS counter: 0 = off, 1 = most first, 2 = least.
	void setSmsSort(int mode) {
		_smsSort = mode;
		update();
	}

	void setAllChecked(bool checked) {
		if (_checkbox->checked() != checked) {
			_checkbox->setChecked(checked, Ui::Checkbox::NotifyAboutChange::DontNotify);
		}
	}

	void setTrashMode(bool trash) {
		_trashMode = trash;
		update();
	}

	// How many accounts are ticked: shown next to "select all".
	void setSelectedCount(int count) {
		_selectedCount = count;
		update();
	}

protected:
	void mousePressEvent(QMouseEvent *e) override {
		if (e->button() == Qt::LeftButton
			&& _statusRect.contains(e->pos())
			&& _onSortStatus) {
			_onSortStatus();
		} else if (e->button() == Qt::LeftButton
			&& _smsRect.contains(e->pos())
			&& _onSortSms) {
			_onSortSms();
		}
	}

	void mouseMoveEvent(QMouseEvent *e) override {
		const auto over = _statusRect.contains(e->pos());
		setCursor((over || _smsRect.contains(e->pos()))
			? style::cur_pointer
			: style::cur_default);
		if (over && _tooltip) {
			Ui::Tooltip::Show(400, this);
		} else {
			Ui::Tooltip::Hide();
		}
		const auto columns = ComputeManagerColumns();
		if (e->pos().x() < columns.check + style::ConvertScale(28)) {
			setToolTip(u"Выбрать все / снять выделение"_q);
		} else if (columns.numberWidth > 0 && e->pos().x() < columns.number + columns.numberWidth) {
			setToolTip(u"Порядковый номер аккаунта"_q);
		} else if (columns.photoWidth > 0 && e->pos().x() < columns.photo + columns.photoWidth) {
			setToolTip(u"Аватар профиля"_q);
		} else if (e->pos().x() < columns.account + columns.accountWidth) {
			setToolTip(u"Аккаунт: имя, @username, телефон, ID"_q);
		} else if (columns.folderWidth > 0 && e->pos().x() < columns.folder + columns.folderWidth) {
			setToolTip(u"Папка аккаунта (кликните в строке для смены папки)"_q);
		} else if (columns.proxyWidth > 0 && e->pos().x() < columns.proxy + columns.proxyWidth) {
			setToolTip(u"Прокси-сервер и статус подключения (кликните для настройки)"_q);
		} else if (over) {
			setToolTip(u"Статус SpamBot (клик для сортировки по статусу)"_q);
		} else if (_smsRect.contains(e->pos())) {
			setToolTip(u"Отправлено сообщений за 24 ч (клик для сортировки)"_q);
		} else if (columns.openWidth > 0 && e->pos().x() < columns.open + columns.openWidth) {
			setToolTip(u"Открыть аккаунт (переключиться)"_q);
		} else {
			setToolTip(u"Действия с аккаунтом (корзина / удаление)"_q);
		}
	}

	void leaveEventHook(QEvent *e) override {
		Ui::Tooltip::Hide();
	}

	void paintEvent(QPaintEvent *e) override {
		auto p = QPainter(this);
		const auto columns = ComputeManagerColumns();
		_statusRect = QRect(
			columns.status,
			0,
			columns.statusWidth,
			height());
		_smsRect = QRect(columns.sms, 0, columns.smsWidth, height());
		const auto smsTitle = u"За 24 ч"_q
			+ ((_smsSort == 1)
				? u" ▼"_q
				: (_smsSort == 2)
				? u" ▲"_q
				: QString());
		const auto statusTitle = u"Статус"_q
			+ ((_sort == 1)
				? u" ▲"_q
				: (_sort == 2)
				? u" ▼"_q
				: QString());
		p.fillRect(
			0,
			height() - st::lineWidth,
			width(),
			st::lineWidth,
			st::shadowFg);
		p.setFont(st::semiboldFont);
		p.setPen(st::windowSubTextFg);
		const auto label = [&](int x, int w, const QString &text, int align) {
			if (w > 0) {
				p.drawText(
					QRect(x, 0, w, height()),
					align | Qt::AlignVCenter,
					text);
			}
		};
		if (_selectedCount > 0) {
			// The count right next to the tick box, in the accent color.
			const auto text = QString::number(_selectedCount);
			const auto x = columns.check + style::ConvertScale(30);
			p.setPen(st::windowActiveTextFg);
			p.drawText(
				QRect(
					x,
					0,
					std::max(
						columns.account - x - style::ConvertScale(4),
						st::semiboldFont->width(text)),
					height()),
				Qt::AlignLeft | Qt::AlignVCenter,
				text);
			p.setPen(st::windowSubTextFg);
		} else {
			label(columns.number, columns.numberWidth, u"№"_q, Qt::AlignHCenter);
		}
		label(columns.account, columns.accountWidth, u"Аккаунт"_q, Qt::AlignLeft);
		label(columns.folder, columns.folderWidth, u"Папка"_q, Qt::AlignLeft);
		label(columns.proxy, columns.proxyWidth, u"Прокси"_q, Qt::AlignLeft);
		label(columns.status, columns.statusWidth, statusTitle, Qt::AlignHCenter);
		label(columns.sms, columns.smsWidth, smsTitle, Qt::AlignHCenter);
		label(columns.open, columns.openWidth, u"Открыть"_q, Qt::AlignHCenter);
		label(
			columns.trash,
			columns.trashWidth,
			_trashMode ? u"Действия"_q : u"Корзина"_q,
			Qt::AlignHCenter);
	}

private:
	ManagerCheck *_checkbox = nullptr;
	Fn<void()> _onSortStatus;
	Fn<void()> _onSortSms;
	Fn<QString()> _tooltip;
	QRect _statusRect;
	QRect _smsRect;
	int _sort = 0;
	int _smsSort = 0;
	int _selectedCount = 0;
	bool _trashMode = false;

};

class AccountTableRow final : public Ui::AbstractButton {
public:
	AccountTableRow(
		not_null<QWidget*> parent,
		not_null<Main::Account*> account,
		int index,
		bool current,
		bool selected,
		Fn<void(bool)> onSelect,
		Fn<void()> onOpen,
		Fn<void()> onCheckSpam,
		Fn<void()> onEditAccount,
		Fn<void()> onEditProxy,
		Fn<void()> onMoveFolder,
		Fn<void()> onBulkActions,
		Fn<void()> onInfo,
		bool trashMode,
		Fn<void()> onTrash,
		Fn<void()> onDeleteForever,
		Fn<void(QPoint)> onContextMenu = nullptr)
	: AbstractButton(parent.get())
	, _account(account)
	, _index(index)
	, _current(current)
	, _trashMode(trashMode)
	, _selected(selected)
	, _onOpen(onOpen)
	, _onContextMenu(std::move(onContextMenu)) {
		const auto columns = ComputeManagerColumns();
		resize(columns.totalWidth, columns.rowHeight);
		setPointerCursor(false);
		_checkbox = Ui::CreateChild<ManagerCheck>(this, selected);
		_checkbox->moveToLeft(
			columns.check + style::ConvertScale(5),
			(columns.rowHeight - _checkbox->height()) / 2);
		_checkAreaRight = columns.check + style::ConvertScale(30);
		_checkbox->checkedChanges(
		) | rpl::on_next([=](bool checked) {
			_selected = checked;
			update();
			onSelect(checked);
		}, lifetime());
		// Right-click anywhere on the row opens context menu with full account actions.
		setAcceptBoth(true);
		clicks(
		) | rpl::on_next([=](Qt::MouseButton button) {
			const auto pos = mapFromGlobal(QCursor::pos());
			if (button == Qt::RightButton) {
				if (_onContextMenu) {
					_onContextMenu(QCursor::pos());
				} else {
					onBulkActions();
				}
				return;
			}
			if (pos.x() < _checkAreaRight) {
				return;
			} else if (!_photoRect.isNull() && _photoRect.contains(pos)) {
				onEditAccount();
			} else if (_badgeRect.contains(pos)) {
				onCheckSpam();
			} else if (_folderRect.contains(pos)) {
				onMoveFolder();
			} else if (_proxyRect.contains(pos)) {
				onEditProxy();
			} else if (_accountRect.contains(pos)) {
				onEditAccount();
			} else if (_openRect.contains(pos)) {
				if (!_current) {
					onOpen();
				}
			} else if (_trashRect.contains(pos)) {
				onTrash();
			} else if (_deleteRect.contains(pos)) {
				onDeleteForever();
			}
		}, lifetime());
		Data::AccountSpamStatusChanges(
		) | rpl::on_next([=](uint64) {
			update();
		}, lifetime());
	}

	void setSelected(bool selected) {
		if (_checkbox->checked() != selected) {
			_checkbox->setChecked(selected, Ui::Checkbox::NotifyAboutChange::DontNotify);
		}
		_selected = selected;
		update();
	}

protected:
	enum class HoverZone { None, Photo, Account, Folder, Proxy, Badge, Open, Trash, Delete };

	void mouseDoubleClickEvent(QMouseEvent *e) override {
		if (e->button() == Qt::LeftButton) {
			if (!_current && _onOpen) {
				_onOpen();
				e->accept();
				return;
			}
		}
		AbstractButton::mouseDoubleClickEvent(e);
	}

	void mouseMoveEvent(QMouseEvent *e) override {
		AbstractButton::mouseMoveEvent(e);
		const auto zone = _badgeRect.contains(e->pos())
			? HoverZone::Badge
			: _folderRect.contains(e->pos())
			? HoverZone::Folder
			: _proxyRect.contains(e->pos())
			? HoverZone::Proxy
			: _accountRect.contains(e->pos())
			? HoverZone::Account
			: (!_photoRect.isNull() && _photoRect.contains(e->pos()))
			? HoverZone::Photo
			: (_openRect.contains(e->pos()) && !_current)
			? HoverZone::Open
			: _trashRect.contains(e->pos())
			? HoverZone::Trash
			: _deleteRect.contains(e->pos())
			? HoverZone::Delete
			: HoverZone::None;
		if (_hoverZone != zone) {
			_hoverZone = zone;
			setCursor(style::cur_pointer);
			update();
		}
		updateTooltip(zone);
	}

	void leaveEventHook(QEvent *e) override {
		AbstractButton::leaveEventHook(e);
		if (_hoverZone != HoverZone::None) {
			_hoverZone = HoverZone::None;
			update();
		}
	}

	void updateTooltip(HoverZone zone) {
		const auto user = _account->sessionExists() ? _account->session().user().get() : nullptr;
		const auto name = user ? user->name() : u"Без имени"_q;
		const auto phone = user ? user->phone() : QString();
		const auto username = user ? user->username() : QString();

		switch (zone) {
		case HoverZone::Photo:
		case HoverZone::Account: {
			const auto handle = username.isEmpty() ? (phone.isEmpty() ? u"—"_q : phone) : (u"@"_q + username);
			setToolTip(u"%1 (%2)\nЛКМ: открыть карточку аккаунта и редактировать\nПКМ: контекстное меню аккаунта"_q
				.arg(name)
				.arg(handle));
			break;
		}
		case HoverZone::Folder: {
			const auto tag = _account->folderTag();
			setToolTip(tag.isEmpty()
				? u"Папка не задана\nЛКМ: выбрать папку\nПКМ: меню папки"_q
				: (u"Папка: «%1»\nЛКМ: сменить папку\nПКМ: меню управления папкой"_q.arg(tag)));
			break;
		}
		case HoverZone::Proxy: {
			const auto proxy = _account->proxyOverride();
			if (!MTP::HasRealProxy(proxy)) {
				setToolTip(u"Прямое подключение (без прокси)\nЛКМ: настроить прокси\nПКМ: контекстное меню"_q);
			} else {
				const auto hp = proxy.host + ':' + QString::number(proxy.port);
				setToolTip(u"Прокси: %1\nЛКМ: настройки прокси\nПКМ: скопировать / отключить"_q.arg(hp));
			}
			break;
		}
		case HoverZone::Badge: {
			const auto status = LiveAccountStatus(_account);
			setToolTip(u"Статус: %1\nЛКМ: проверить SpamBot сейчас\nПКМ: меню действий"_q.arg(status.text));
			break;
		}
		case HoverZone::Open:
			setToolTip(_current ? u"Этот аккаунт сейчас активен"_q : u"ЛКМ: переключиться на этот аккаунт"_q);
			break;
		case HoverZone::Trash:
			setToolTip(_trashMode ? u"Восстановить аккаунт из корзины"_q : u"Переместить аккаунт в корзину"_q);
			break;
		case HoverZone::Delete:
			setToolTip(u"Удалить аккаунт навсегда (сессия и данные будут удалены)"_q);
			break;
		case HoverZone::None:
		default:
			setToolTip(u"Двойной клик: открыть аккаунт | ПКМ: контекстное меню"_q);
			break;
		}
	}

	void paintEvent(QPaintEvent *e) override {
		auto p = Painter(this);
		const auto columns = ComputeManagerColumns();
		const auto baseHeight = columns.rowHeight;
		if (_selected) {
			auto tint = st::windowBgActive->c;
			tint.setAlphaF(0.14);
			p.fillRect(rect(), tint);
		} else if (isOver()) {
			p.fillRect(rect(), st::windowBgOver);
		}
		p.fillRect(
			0,
			height() - st::lineWidth,
			width(),
			st::lineWidth,
			st::shadowFg);

		const auto session = _account->sessionExists();
		const auto user = session ? _account->session().user().get() : nullptr;

		// A hovered cell's own highlight - the signal that this bit of an
		// already-clickable row is a *different* click (edit this, not
		// "switch to this account"). Painted first so the cell's text
		// draws on top of it.
		const auto paintHoverHighlight = [&](QRect cellRect) {
			if (cellRect.isNull()) {
				return;
			}
			// A distinct accent tint, not st::windowBgOver - that's the
			// same fill the whole row already gets on hover (below), so
			// using it here too would sit invisibly on top of itself and
			// defeat the entire point of marking this cell as different.
			auto tint = st::windowBgActive->c;
			tint.setAlphaF(0.22);
			auto hq = PainterHighQualityEnabler(p);
			p.setPen(Qt::NoPen);
			p.setBrush(tint);
			p.drawRoundedRect(
				cellRect.marginsAdded(QMargins(
					style::ConvertScale(4),
					style::ConvertScale(2),
					style::ConvertScale(4),
					style::ConvertScale(2))),
				style::ConvertScale(6),
				style::ConvertScale(6));
		};
		if (_hoverZone == HoverZone::Account) {
			paintHoverHighlight(_accountRect);
		}

		p.setFont(st::boxTitleFont);
		p.setPen(st::windowBoldFg);
		if (columns.numberWidth > 0) {
			p.drawText(
				QRect(columns.number, 0, columns.numberWidth, baseHeight),
				Qt::AlignHCenter | Qt::AlignVCenter,
				QString::number(_index));
		}

		if (user && columns.photoWidth > 0) {
			const auto size = style::ConvertScale(50);
			const auto photoY = (baseHeight - size) / 2;
			_photoRect = QRect(columns.photo, photoY, size, size);
			user->paintUserpicLeft(
				p,
				_view,
				columns.photo,
				photoY,
				width(),
				size);
			if (_hoverZone == HoverZone::Photo) {
				auto hq = PainterHighQualityEnabler(p);
				p.setBrush(Qt::NoBrush);
				auto ring = st::windowBgActive->c;
				ring.setAlphaF(0.45);
				p.setPen(QPen(ring, style::ConvertScale(2)));
				p.drawEllipse(_photoRect.adjusted(
					style::ConvertScale(1),
					style::ConvertScale(1),
					-style::ConvertScale(1),
					-style::ConvertScale(1)));
			}
		} else {
			_photoRect = QRect();
		}

		auto name = user ? user->name() : u"(нет сессии)"_q;
		if (name.isEmpty()) {
			name = u"Без имени"_q;
		}
		if (_current) {
			name += u" (текущий)"_q;
		}
		const auto username = user ? user->username() : QString();
		auto phone = user ? user->phone() : QString();
		if (!phone.isEmpty() && !phone.startsWith('+')) {
			phone = '+' + phone;
		}
		PaintAccountCell(
			p,
			columns.account,
			columns.accountWidth,
			baseHeight,
			name,
			username.isEmpty() ? u"@no_user"_q : (u"@"_q + username),
			username.isEmpty() ? st::windowSubTextFg->c : QColor(56, 189, 248),
			phone.isEmpty() ? u"—"_q : phone,
			user ? u"ID "_q + QString::number(user->id.value) : QString(),
			user,
			&_premiumBadge,
			[=] { update(); });
		// Click anywhere on this cell to edit name/@username - see
		// ShowEditAccountBox.
		_accountRect = QRect(
			columns.account, 0, columns.accountWidth, baseHeight);

		_folderRect = PaintFolderCell(
			p,
			columns,
			baseHeight,
			_account->folderTag(),
			_hoverZone == HoverZone::Folder);

		_proxyRect = PaintProxyCell(
			p,
			columns,
			baseHeight,
			_account->proxyOverride(),
			_account->mtp().dcstate(),
			_account->alwaysOffline(),
			_hoverZone == HoverZone::Proxy);

		// The status badge: the account's spam-limit verdict (or "no
		// session"), tinted with its own color - clicking it re-checks
		// with @spambot, same as the "Неизвестно" badge in the reference.
		{
			const auto status = LiveAccountStatus(_account);
			_badgeRect = PaintStatusBadge(
				p,
				columns,
				baseHeight,
				status.text,
				status.color,
				(_hoverZone == HoverZone::Badge) ? 0.32 : 0.18);
		}

		const auto sent = user ? Data::CountSmsSentToday(user->id.value) : 0;
		p.setFont(st::normalFont);
		p.setPen(sent ? st::windowBoldFg->c : st::windowSubTextFg->c);
		p.drawText(
			QRect(columns.sms, 0, columns.smsWidth, baseHeight),
			Qt::AlignHCenter | Qt::AlignVCenter,
			sent ? QString::number(sent) : u"—"_q);

		// «Открыть»: a filled pill; the open account gets a quiet outline.
		{
			_openRect = ManagerOpenRect(columns, baseHeight);
			const auto radius = _openRect.height() / 2.;
			auto hq = PainterHighQualityEnabler(p);
			if (_current) {
				auto border = st::windowSubTextFg->c;
				border.setAlphaF(0.5);
				p.setPen(QPen(border, st::lineWidth));
				p.setBrush(Qt::NoBrush);
				p.drawRoundedRect(
					QRectF(_openRect).marginsRemoved(
						QMarginsF(0.5, 0.5, 0.5, 0.5)),
					radius,
					radius);
				p.setPen(st::windowSubTextFg);
			} else {
				auto fill = QColor(10, 132, 255);
				if (_hoverZone == HoverZone::Open) {
					fill = fill.lighter(115);
				}
				p.setPen(Qt::NoPen);
				p.setBrush(fill);
				p.drawRoundedRect(_openRect, radius, radius);
				p.setPen(QColor(255, 255, 255));
			}
			p.setFont(st::semiboldFont);
			p.drawText(
				_openRect,
				Qt::AlignCenter,
				_current ? u"Открыт"_q : u"Открыть"_q);
		}

		// Trash column: move to the trash, or inside it restore / delete.
		{
			const auto [first, second] = ManagerTrashRects(
				columns,
				baseHeight,
				_trashMode);
			_trashRect = first;
			_deleteRect = second;
			const auto paintIconButton = [&](
					QRect rect,
					const style::icon &icon,
					bool over,
					QColor overColor) {
				if (rect.isEmpty()) {
					return;
				}
				if (over) {
					auto fill = overColor;
					fill.setAlphaF(0.16);
					auto hq = PainterHighQualityEnabler(p);
					p.setPen(Qt::NoPen);
					p.setBrush(fill);
					p.drawEllipse(rect);
				}
				icon.paintInCenter(p, rect);
			};
			const auto attention = st::attentionButtonFg->c;
			if (_trashMode) {
				paintIconButton(
					_trashRect,
					st::menuIconRestore,
					_hoverZone == HoverZone::Trash,
					st::windowBgActive->c);
				paintIconButton(
					_deleteRect,
					st::menuIconDeleteAttention,
					_hoverZone == HoverZone::Delete,
					attention);
			} else {
				paintIconButton(
					_trashRect,
					(_hoverZone == HoverZone::Trash)
						? st::menuIconDeleteAttention
						: st::menuIconDelete,
					_hoverZone == HoverZone::Trash,
					attention);
			}
		}
	}

private:
	int _checkAreaRight = 0;

	const not_null<Main::Account*> _account;
	const int _index;
	const bool _current;
	const bool _trashMode;
	bool _selected = false;
	Fn<void()> _onOpen;
	Fn<void(QPoint)> _onContextMenu;
	Ui::PeerUserpicView _view;
	Ui::PeerBadge _premiumBadge;
	ManagerCheck *_checkbox = nullptr;
	QRect _photoRect;
	QRect _badgeRect;
	QRect _accountRect;
	QRect _folderRect;
	QRect _proxyRect;
	QRect _openRect;
	QRect _trashRect;
	QRect _deleteRect;
	HoverZone _hoverZone = HoverZone::None;

};

// The account whose session was ended: not a live Account any more (the app
// had to log it out), so it is drawn from its journal entry instead - same
// columns, status "Сессия разорвана" - and stays in the list until removed.
class LostAccountRow final : public Ui::AbstractButton {
public:
	LostAccountRow(
		not_null<QWidget*> parent,
		Core::GgtgSessions::LostSession entry,
		Fn<void()> onInfo)
	: AbstractButton(parent.get())
	, _entry(std::move(entry)) {
		const auto columns = ComputeManagerColumns();
		resize(columns.totalWidth, columns.rowHeight);
		// A click anywhere on the row opens its info.
		setClickedCallback(std::move(onInfo));
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		auto p = Painter(this);
		const auto columns = ComputeManagerColumns();
		const auto baseHeight = columns.rowHeight;
		if (isOver()) {
			p.fillRect(rect(), st::windowBgOver);
		}
		p.fillRect(
			0,
			height() - st::lineWidth,
			width(),
			st::lineWidth,
			st::shadowFg);
		const auto status = LostAccountStatus(_entry);
		const auto red = status.color;
		const auto sub = st::windowSubTextFg->c;
		const auto bold = st::windowBoldFg->c;

		// A disc in the status colour with "!" where the photo would be.
		if (columns.photoWidth > 0) {
			const auto size = style::ConvertScale(50);
			auto fill = red;
			fill.setAlphaF(0.2);
			auto hq = PainterHighQualityEnabler(p);
			p.setPen(Qt::NoPen);
			p.setBrush(fill);
			const auto disc = QRect(
				columns.photo,
				(baseHeight - size) / 2,
				size,
				size);
			p.drawEllipse(disc);
			p.setPen(red);
			p.setFont(st::semiboldFont);
			p.drawText(disc, Qt::AlignCenter, u"!"_q);
		}

		auto name = _entry.name.isEmpty() ? u"Без имени"_q : _entry.name;
		auto phone = _entry.phone;
		if (!phone.isEmpty() && !phone.startsWith('+')) {
			phone = '+' + phone;
		}
		PaintAccountCell(
			p,
			columns.account,
			columns.accountWidth,
			baseHeight,
			name,
			_entry.username.isEmpty()
				? u"@no_user"_q
				: (u"@"_q + _entry.username),
			_entry.username.isEmpty() ? sub : QColor(56, 189, 248),
			phone.isEmpty() ? u"—"_q : phone,
			_entry.userId.isEmpty() ? QString() : u"ID "_q + _entry.userId);

		// Status: why the account is gone (banned, deleted, session lost).
		PaintFolderCell(
			p,
			columns,
			baseHeight,
			_entry.folder,
			false);

		if (columns.proxyWidth > 0) {
			const auto padding = style::ConvertScale(4);
			const auto cellH = st::semiboldFont->height + st::normalFont->height + style::ConvertScale(4);
			const auto rect = QRect(
				columns.proxy,
				(baseHeight - cellH) / 2,
				columns.proxyWidth,
				cellH);
			const auto dotSize = style::ConvertScale(7);
			auto dotY = rect.y() + (st::semiboldFont->height - dotSize) / 2 + style::ConvertScale(1);
			auto dotX = rect.x() + padding;
			{
				auto hq = PainterHighQualityEnabler(p);
				p.setPen(Qt::NoPen);
				p.setBrush(QColor(142, 142, 147));
				p.drawEllipse(QRect(dotX, dotY, dotSize, dotSize));
			}
			const auto textX = dotX + dotSize + style::ConvertScale(6);
			const auto textW = std::max(0, rect.width() - (textX - rect.x()) - padding);
			p.setFont(st::semiboldFont);
			p.setPen(st::windowBoldFg);
			p.drawText(
				QRect(textX, rect.y(), textW, st::semiboldFont->height),
				Qt::AlignLeft | Qt::AlignVCenter,
				st::semiboldFont->elided(_entry.proxy.isEmpty() ? u"Без прокси"_q : _entry.proxy, textW));
			p.setFont(st::normalFont);
			p.setPen(st::windowSubTextFg);
			p.drawText(
				QRect(textX, rect.y() + st::semiboldFont->height + style::ConvertScale(2), textW, st::normalFont->height),
				Qt::AlignLeft | Qt::AlignVCenter,
				u"сессия завершена"_q);
		}

		PaintStatusBadge(p, columns, baseHeight, status.text, red, 0.18);
	}

private:
	const Core::GgtgSessions::LostSession _entry;

};

// One card per folder (plus "ALL") with its name and account counter; the
// chosen one is filled with the accent color.
class FolderCard final : public Ui::AbstractButton {
public:
	FolderCard(
		not_null<QWidget*> parent,
		const QString &name,
		int count,
		bool active,
		bool hasCurrentAccount = false)
	: AbstractButton(parent.get())
	, _name(name)
	, _count(QString::number(count))
	, _active(active)
	, _hasCurrentAccount(hasCurrentAccount) {
		setPointerCursor(true);
		setToolTip(u"Папка «%1» (%2 акк.)\nЛКМ: фильтр по папке\nПКМ: меню управления папкой"_q
			.arg(_name)
			.arg(_count));
		const auto paddingH = style::ConvertScale(12);
		const auto countPad = style::ConvertScale(6);
		const auto textW = st::semiboldFont->width(_name);
		const auto countW = st::normalFont->width(_count) + 2 * countPad;
		const auto h = st::semiboldFont->height + style::ConvertScale(12);
		resize(
			textW + countW + 2 * paddingH + style::ConvertScale(6),
			h);
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		auto p = QPainter(this);
		auto hq = PainterHighQualityEnabler(p);
		const auto radius = height() / 2.;
		p.setPen(Qt::NoPen);
		if (_active) {
			p.setBrush(QColor(10, 132, 255));
		} else {
			auto fill = st::windowBoldFg->c;
			fill.setAlphaF(isOver() ? 0.12 : 0.06);
			p.setBrush(fill);
		}
		p.drawRoundedRect(rect(), radius, radius);

		if (!_active && _hasCurrentAccount) {
			auto border = QColor(10, 132, 255);
			border.setAlphaF(0.8);
			p.setBrush(Qt::NoBrush);
			p.setPen(QPen(border, style::ConvertScale(1.5)));
			p.drawRoundedRect(QRectF(rect()).marginsRemoved(QMarginsF(0.75, 0.75, 0.75, 0.75)), radius, radius);
		}

		const auto paddingH = style::ConvertScale(12);
		const auto countPad = style::ConvertScale(6);
		const auto countW = st::normalFont->width(_count) + 2 * countPad;
		const auto countH = st::normalFont->height + style::ConvertScale(2);

		// Name
		p.setFont(st::semiboldFont);
		p.setPen(_active ? QColor(255, 255, 255) : st::windowBoldFg->c);
		const auto nameRect = QRect(
			paddingH,
			0,
			width() - 2 * paddingH - countW - style::ConvertScale(4),
			height());
		p.drawText(nameRect, Qt::AlignLeft | Qt::AlignVCenter, _name);

		// Count badge
		const auto badgeRect = QRect(
			width() - paddingH - countW,
			(height() - countH) / 2,
			countW,
			countH);
		const auto badgeRadius = countH / 2.;
		p.setPen(Qt::NoPen);
		if (_active) {
			p.setBrush(QColor(255, 255, 255, 60));
		} else {
			p.setBrush(QColor(st::windowBoldFg->c.red(), st::windowBoldFg->c.green(), st::windowBoldFg->c.blue(), 20));
		}
		p.drawRoundedRect(badgeRect, badgeRadius, badgeRadius);

		p.setFont(st::normalFont);
		p.setPen(_active ? QColor(255, 255, 255) : st::windowSubTextFg->c);
		p.drawText(badgeRect, Qt::AlignCenter, _count);
	}

private:
	const QString _name;
	const QString _count;
	const bool _active;
	const bool _hasCurrentAccount = false;

};

class FolderCardsRow final : public Ui::RpWidget {
public:
	FolderCardsRow(not_null<QWidget*> parent, Ui::ScrollArea *scroll = nullptr)
	: RpWidget(parent.get())
	, _scroll(scroll) {
	}

	not_null<FolderCard*> addCard(
			const QString &name,
			int count,
			bool active,
			Fn<void()> onClick,
			Fn<void(QPoint)> onRightClick = nullptr,
			bool hasCurrentAccount = false) {
		const auto card = Ui::CreateChild<FolderCard>(
			this,
			name,
			count,
			active,
			hasCurrentAccount);
		card->setAcceptBoth(onRightClick != nullptr);
		card->clicks(
		) | rpl::on_next([=](Qt::MouseButton button) {
			if (button == Qt::RightButton) {
				if (onRightClick) {
					onRightClick(QCursor::pos());
				}
			} else if (onClick) {
				onClick();
			}
		}, card->lifetime());
		card->show();
		_cards.push_back(card);
		return card;
	}

	void layoutCards() {
		const auto gap = style::ConvertScale(8);
		auto x = 0;
		auto maxHeight = 0;
		for (const auto card : _cards) {
			card->moveToLeft(x, 0);
			x += card->width() + gap;
			maxHeight = std::max(maxHeight, card->height());
		}
		resize(std::max(x, 1), maxHeight);
	}

protected:
	int resizeGetHeight(int newWidth) override {
		layoutCards();
		return height();
	}

	void wheelEvent(QWheelEvent *e) override {
		if (_scroll) {
			const auto delta = e->angleDelta().y() ? e->angleDelta().y() : e->angleDelta().x();
			_scroll->scrollToX(_scroll->scrollLeft() - delta);
			e->accept();
			return;
		}
		RpWidget::wheelEvent(e);
	}

private:
	Ui::ScrollArea *_scroll = nullptr;
	std::vector<not_null<FolderCard*>> _cards;

};

// The selection toolbar above the account table: a count label on the
// left and action buttons right-aligned next to it. On a narrow window
// (previously) the buttons were simply positioned past x=0 and clipped
// by the box's left edge with no way to scroll to them - this instead
// drops the buttons to their own row(s) below the label once they no
// longer fit next to it, wrapping further if even one row of buttons
// doesn't fit, the same reflow behaviour FolderCardsRow above already
// uses for folder cards on a narrow window.
class SelectionBarRow final : public Ui::RpWidget {
public:
	explicit SelectionBarRow(not_null<QWidget*> parent)
	: RpWidget(parent.get()) {
	}

	// Buttons are laid out left to right in the order they are added,
	// wrapping onto another line when the window is too narrow; hidden
	// ones take no room.
	void addButton(not_null<Ui::RpWidget*> button) {
		_buttons.push_back(button.get());
	}

protected:
	int resizeGetHeight(int newWidth) override {
		const auto gap = style::ConvertScale(8);
		auto x = 0;
		auto y = 0;
		auto lineHeight = 0;
		for (const auto button : _buttons) {
			if (button->isHidden()) {
				continue;
			}
			if (x > 0 && x + button->width() > newWidth) {
				x = 0;
				y += lineHeight + gap;
				lineHeight = 0;
			}
			button->moveToLeft(x, y);
			x += button->width() + gap;
			lineHeight = std::max(lineHeight, button->height());
		}
		return y + lineHeight;
	}

private:
	std::vector<not_null<Ui::RpWidget*>> _buttons;

};

// ---- "(i)": what is known about an account ----

// One "label: value" line of an info box. With `copy` a click on the value
// copies it; with `action` a click on it does that instead.
struct InfoLine {
	InfoLine(
		const QString &label,
		const QString &value,
		bool copy = false,
		Fn<void()> action = nullptr)
	: label(label)
	, value(value)
	, copy(copy)
	, action(std::move(action)) {
	}

	QString label;
	QString value;
	bool copy = false;
	Fn<void()> action;
};
using InfoLines = std::vector<InfoLine>;

[[nodiscard]] QString WithPlus(QString phone) {
	if (!phone.isEmpty() && !phone.startsWith('+')) {
		phone = '+' + phone;
	}
	return phone;
}

[[nodiscard]] QString OrDash(const QString &text) {
	return text.isEmpty() ? u"—"_q : text;
}

[[nodiscard]] QString StampText(qint64 at) {
	return at
		? base::unixtime::parse(at).toString(u"dd.MM.yyyy HH:mm"_q)
		: u"—"_q;
}

// A box listing "label: value" lines (clicking a value copies it, or runs
// its own action), optional extra content and buttons and an optional
// explanation under the lines.
void ShowInfoBox(
		std::shared_ptr<Ui::Show> show,
		const QString &title,
		InfoLines lines,
		Fn<void(not_null<Ui::GenericBox*>)> addExtra,
		const QString &hint,
		Fn<void(not_null<Ui::GenericBox*>)> addButtons) {
	show->showBox(Box([=](not_null<Ui::GenericBox*> box) {
		box->setWidth(st::accountsBoxWidth);
		box->setTitle(rpl::single(title));
		auto anyClickable = false;
		for (const auto &line : lines) {
			const auto clickable = (line.copy || line.action)
				&& !line.value.isEmpty()
				&& (line.value != u"—"_q || line.action);
			if (!clickable) {
				box->addRow(object_ptr<Ui::FlatLabel>(
					box,
					line.label + u": "_q + line.value,
					st::boxLabel));
				continue;
			}
			anyClickable = true;
			// The value is a link: a tap copies it (or does what the line
			// says), nothing is opened.
			const auto row = box->addRow(object_ptr<Ui::FlatLabel>(
				box,
				rpl::single(TextWithEntities()),
				st::boxLabel));
			auto marked = TextWithEntities{ line.label + u": "_q };
			marked.append(Ui::Text::Link(line.value));
			row->setMarkedText(marked);
			const auto value = line.value;
			const auto label = line.label;
			const auto copy = line.copy;
			const auto action = line.action;
			row->setClickHandlerFilter([=](const auto &...) {
				if (action) {
					action();
				} else if (copy) {
					TextUtilities::SetClipboardText(
						TextForMimeData::Simple(value));
					Ggtg::Notice(box->uiShow(), label + u" скопирован(о)"_q);
				}
				return false;
			});
		}
		if (addExtra) {
			addExtra(box);
		}
		if (!hint.isEmpty()) {
			Ui::AddSkip(box->verticalLayout());
			box->addRow(object_ptr<Ui::FlatLabel>(
				box,
				hint,
				st::boxDividerLabel));
		}
		if (anyClickable) {
			Ui::AddSkip(box->verticalLayout());
			box->addRow(object_ptr<Ui::FlatLabel>(
				box,
				u"Нажмите на значение (ник, телефон, ID, пароль), чтобы "
					"скопировать его."_q,
				st::boxDividerLabel));
		}
		if (addButtons) {
			addButtons(box);
		}
		Ggtg::AddCloseCross(box);
	}));
}

// One line, the same shape the proxy lists use: host:port[:login[:password]].
[[nodiscard]] QString ProxyText(const MTP::ProxyData &proxy) {
	if (!MTP::HasRealProxy(proxy)) {
		return u"нет - аккаунт не подключается, пока не задан"_q;
	}
	auto result = proxy.host + ':' + QString::number(proxy.port);
	if (!proxy.user.isEmpty() || !proxy.password.isEmpty()) {
		result += ':' + proxy.user;
		if (!proxy.password.isEmpty()) {
			result += ':' + proxy.password;
		}
	}
	return result;
}

void ExportSpecificSession(
		std::shared_ptr<Ui::Show> show,
		not_null<Main::Account*> account,
		const Core::GgtgSessions::StoredSession &stored) {
	namespace Sessions = Core::GgtgSessions;
	const auto vaultTdata = Sessions::VaultFolder(stored) + u"/tdata"_q;
	if (!QDir(vaultTdata).exists()) {
		Ggtg::Notice(show, u"Файлы сессии не найдены в хранилище"_q);
		return;
	}

	show->showBox(Box([=](not_null<Ui::GenericBox*> box) {
		box->setWidth(style::ConvertScale(480));
		box->setTitle(rpl::single(u"Экспорт сессии"_q));

		const auto devModel = stored.device.value(u"deviceModel"_q).toString();
		const auto appVer = stored.device.value(u"appVersion"_q).toString();
		const auto devTitle = devModel.isEmpty()
			? Sessions::RoleTitle(stored.role)
			: devModel;

		auto descText = devTitle;
		if (!appVer.isEmpty()) {
			descText += u" · "_q + appVer;
		}
		descText += u"\nСохранена: "_q
			+ QDateTime::fromSecsSinceEpoch(stored.createdAt).toString(u"dd.MM.yyyy hh:mm"_q)
			+ u" ("_q + Sessions::RoleTitle(stored.role) + u")"_q;

		box->addRow(
			object_ptr<Ui::FlatLabel>(
				box,
				rpl::single(descText),
				st::boxLabel),
			st::boxRowPadding);

		Ui::AddSkip(box->verticalLayout());
		Ui::AddSubsectionTitle(box->verticalLayout(), rpl::single(u"Формат экспорта"_q));

		const auto group = std::make_shared<Ui::RadiobuttonGroup>(0);
		box->addRow(
			object_ptr<Ui::Radiobutton>(
				box,
				group,
				0,
				u"GGTG Android (.ggtg)"_q),
			st::ggtgChoiceRowPadding);
		box->addRow(
			object_ptr<Ui::FlatLabel>(
				box,
				rpl::single(u"Файл для импорта на Android без повторного входа и SMS"_q),
				st::boxDividerLabel),
			st::boxRowPadding);

		box->addRow(
			object_ptr<Ui::Radiobutton>(
				box,
				group,
				1,
				u"Session + JSON (.session / .json)"_q),
			st::ggtgChoiceRowPadding);
		box->addRow(
			object_ptr<Ui::FlatLabel>(
				box,
				rpl::single(u"Формат Telethon / Pyrogram с параметрами устройства"_q),
				st::boxDividerLabel),
			st::boxRowPadding);

		box->addRow(
			object_ptr<Ui::Radiobutton>(
				box,
				group,
				2,
				u"Папка tdata (Telegram Desktop)"_q),
			st::ggtgChoiceRowPadding);
		box->addRow(
			object_ptr<Ui::FlatLabel>(
				box,
				rpl::single(u"Папка tdata для запуска в Telegram Desktop"_q),
				st::boxDividerLabel),
			st::boxRowPadding);

		box->addButton(rpl::single(u"Экспортировать"_q), [=] {
			const auto auths = Storage::ReadForeignTdataAuthorizations(vaultTdata);
			if (auths.empty()) {
				Ggtg::Notice(show, u"Не удалось прочитать ключ сессии"_q);
				return;
			}
			const auto serializedAuth = auths.front().data;
			const auto choice = group->current();

			if (choice == 0) {
				// .ggtg
				auto phoneClean = stored.phone;
				phoneClean.remove('+');
				phoneClean = phoneClean.trimmed();
				if (phoneClean.isEmpty() && Main::AccountIsLive(account)) {
					phoneClean = account->session().user()->phone();
					phoneClean.remove('+');
					phoneClean = phoneClean.trimmed();
				}
				if (phoneClean.isEmpty()) {
					phoneClean = stored.userId.isEmpty() ? u"account"_q : stored.userId;
				}
				const auto initialName = phoneClean + u".ggtg"_q;
				FileDialog::GetWritePath(
					QPointer<QWidget>(box.get()),
					u"Экспорт сессии в .ggtg"_q,
					u"GGTG Pack (*.ggtg)"_q,
					initialName,
					[=](QString &&path) {
						if (path.isEmpty()) {
							return;
						}
						auto error = QString();
						const auto proxy = stored.proxy.isEmpty()
							? MTP::ProxyData()
							: ProxyFromObject(stored.proxy);
						const auto phone = !stored.phone.isEmpty()
							? stored.phone
							: (Main::AccountIsLive(account) ? account->session().user()->phone() : QString());
						const auto name = !stored.name.isEmpty()
							? stored.name
							: (Main::AccountIsLive(account) ? account->session().user()->name() : QString());
						const auto username = !stored.username.isEmpty()
							? stored.username
							: (Main::AccountIsLive(account) ? account->session().user()->username() : QString());
						const auto twoFactor = !stored.twoFactor.isEmpty()
							? stored.twoFactor
							: (Main::AccountIsLive(account) ? account->twoFactorPassword() : QString());

						if (!ExportAccountToGgtg(
								path,
								serializedAuth,
								phone,
								name,
								QString(),
								username,
								twoFactor,
								proxy,
								stored.device,
								stored.folder,
								false,
								&error)) {
							Ggtg::Notice(show, error.isEmpty() ? u"Не удалось экспортировать .ggtg"_q : error);
						} else {
							Ggtg::Notice(show, u"Сессия экспортирована в "_q + QFileInfo(path).fileName());
							box->closeBox();
						}
					});
			} else if (choice == 1) {
				// Session + JSON
				FileDialog::GetFolder(
					QPointer<QWidget>(box.get()),
					u"Выбрать папку для Session + JSON"_q,
					QString(),
					[=](QString &&folder) {
						if (folder.isEmpty() || !Main::AccountIsLive(account)) {
							return;
						}
						auto error = QString();
						auto customDev = std::optional<MTP::DeviceOverride>();
						if (!stored.device.isEmpty()) {
							customDev = DeviceOverrideFromJson(stored.device);
						}
						if (!ExportAccountAsSessionJson(
								account,
								folder,
								serializedAuth,
								customDev,
								&error)) {
							Ggtg::Notice(show, error.isEmpty() ? u"Не удалось экспортировать Session + JSON"_q : error);
						} else {
							Ggtg::Notice(show, u"Сессия успешно сохранена в Session + JSON"_q);
							box->closeBox();
						}
					});
			} else if (choice == 2) {
				// tdata
				FileDialog::GetFolder(
					QPointer<QWidget>(box.get()),
					u"Выбрать папку для tdata"_q,
					QString(),
					[=](QString &&folder) {
						if (folder.isEmpty()) {
							return;
						}
						const auto dest = folder + u"/tdata"_q;
						if (!CopyDirRecursive(vaultTdata, dest)) {
							Ggtg::Notice(show, u"Не удалось скопировать папку tdata"_q);
							return;
						}
						const auto twoFactor = !stored.twoFactor.isEmpty()
							? stored.twoFactor
							: (Main::AccountIsLive(account) ? account->twoFactorPassword() : QString());
						if (!twoFactor.isEmpty()) {
							auto file = QFile(folder + u"/2fa.txt"_q);
							if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
								file.write(twoFactor.toUtf8());
							}
						}
						if (!stored.device.isEmpty()) {
							auto file = QFile(folder + u"/ggtg.json"_q);
							if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
								auto info = QJsonObject();
								info["device"] = stored.device;
								info["phone"] = stored.phone;
								file.write(QJsonDocument(info).toJson(QJsonDocument::Indented));
							}
						}
						Ggtg::Notice(show, u"Папка tdata успешно сохранена"_q);
						box->closeBox();
					});
			}
		});

		box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
	}));
}

void ShowStoredSessionPicker(
		std::shared_ptr<Ui::Show> show,
		not_null<Main::Account*> account,
		const std::vector<Core::GgtgSessions::StoredSession> &stored) {
	namespace Sessions = Core::GgtgSessions;
	if (stored.empty()) {
		Ggtg::Notice(show, u"Нет сохранённых сессий для экспорта"_q);
		return;
	}
	if (stored.size() == 1) {
		ExportSpecificSession(show, account, stored.front());
		return;
	}
	show->showBox(Box([=](not_null<Ui::GenericBox*> box) {
		box->setWidth(style::ConvertScale(440));
		box->setTitle(rpl::single(u"Выбор сессии для экспорта"_q));

		const auto group = std::make_shared<Ui::RadiobuttonGroup>(0);
		for (size_t i = 0; i < stored.size(); ++i) {
			const auto &item = stored[i];
			const auto devModel = item.device.value(u"deviceModel"_q).toString();
			const auto title = (devModel.isEmpty() ? Sessions::RoleTitle(item.role) : devModel)
				+ u" ("_q + Sessions::RoleTitle(item.role) + u" · "_q
				+ QDateTime::fromSecsSinceEpoch(item.createdAt).toString(u"dd.MM.yyyy"_q) + u")"_q;
			box->addRow(
				object_ptr<Ui::Radiobutton>(
					box,
					group,
					int(i),
					title),
				st::ggtgChoiceRowPadding);
		}

		box->addButton(rpl::single(u"Далее"_q), [=] {
			const auto idx = size_t(group->current());
			if (idx < stored.size()) {
				const auto item = stored[idx];
				box->closeBox();
				ExportSpecificSession(show, account, item);
			}
		});
		box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
	}));
}

// The account's sessions as in Settings > Privacy > Active sessions: this
// one first, then the others by last activity - device, app, IP/location
// and when it was last active. Filled in as soon as Telegram answers.
void AddSessionsSection(
		not_null<Ui::GenericBox*> box,
		not_null<Main::Account*> account) {
	const auto authorizations = &account->session().api().authorizations();
	const auto wrap = box->addRow(object_ptr<Ui::VerticalLayout>(box));
	// hash 0 = every other session. Asks first, then ends it on the server;
	// the list refreshes by itself once Telegram confirms.
	const auto terminate = [=](uint64 hash, const QString &device) {
		const auto text = hash
			? (u"Завершить сессию"_q
				+ (device.isEmpty() ? QString() : (u" «"_q + device + u"»"_q))
				+ u"? Это устройство выйдет из аккаунта."_q)
			: u"Завершить все остальные сессии этого аккаунта? Другие "
				"устройства выйдут из него. Сессии, созданные или выданные "
				"программой (резервные, дублированные, Android, экспорты), "
				"останутся."_q;
		box->uiShow()->show(Ui::MakeConfirmBox({
			.text = text,
			.confirmed = [=](Fn<void()> &&close) {
				close();
				if (!Main::AccountIsLive(account)) {
					return;
				}
				const auto show = box->uiShow();
				if (!Core::GuardAllows(show, Core::GuardedAction::ChangeSessions)) {
					return;
				}
				if (!hash) {
					TerminateOtherSessions(account, [=](
							QString error,
							QString report) {
						Ggtg::Notice(show, error.isEmpty()
							? (u"Готово: "_q + report)
							: (u"Не удалось: "_q + error));
					});
					return;
				}
				account->session().api().authorizations().requestTerminate(
					[=](const MTPBool &) {
						// A session this app kept goes from it too.
						namespace Sessions = Core::GgtgSessions;
						for (const auto &entry : Sessions::LoadVault()) {
							if (entry.hash == hash) {
								Sessions::RemoveStored(entry.id);
							}
						}
						Ggtg::Notice(show, u"Сессия завершена"_q);
					},
					[=](const MTP::Error &error) {
						const auto type = error.type();
						Ggtg::Notice(show, 
							(type.startsWith(u"FRESH_RESET_AUTHORISATION"_q))
							? u"Telegram не даёт завершать другие сессии, "
								"пока эта сессия новая (меньше суток). "
								"Попробуйте позже."_q
							: (u"Не удалось завершить сессию: "_q + type));
					},
					hash ? std::optional<uint64>(hash) : std::nullopt);
			},
			.confirmText = u"Завершить"_q,
		}));
	};
	// Re-runs the list below after something changed (a spare made/removed).
	const auto fillAgain = box->lifetime().make_state<Fn<void()>>();
	// Runs the account on a stored session; the one it leaves becomes a
	// spare, so switching back is the same click on that one.
	const auto confirmSwitch = box->lifetime().make_state<
		Fn<void(Core::GgtgSessions::StoredSession)>>();
	*confirmSwitch = [=](Core::GgtgSessions::StoredSession stored) {
		const auto warning = SwitchWarning(stored);
		box->uiShow()->show(Ui::MakeConfirmBox({
			.text = u"Переключить аккаунт на эту сессию ("_q
				+ Core::GgtgSessions::RoleTitle(stored.role)
				+ u" от "_q + StampText(stored.createdAt)
				+ u")? Текущая основная станет резервной, ни одна сессия "
					"не завершается, переключиться обратно можно так же."_q
				+ (warning.isEmpty() ? QString() : (u"\n\nВнимание: "_q + warning)),
			.confirmed = [=](Fn<void()> &&close) {
				close();
				if (!Main::AccountIsLive(account)) {
					return;
				}
				const auto show = box->uiShow();
				if (!Core::GuardAllows(
						show,
						Core::GuardedAction::ChangeSessions)) {
					return;
				}
				SwapToStoredSession(account, stored.id, [=](QString error) {
					if (!error.isEmpty()) {
						Ggtg::Notice(show, u"Не удалось переключить: "_q + error);
						return;
					}
					Ggtg::Notice(show, u"Сессия переключена"_q);
					// The account is a new one in the app now.
					box->closeBox();
				});
			},
			.confirmText = u"Переключить"_q,
		}));
	};
	const auto fill = [=](const Api::Authorizations::List &list) {
		const Ggtg::ScrollKeeper keepScroll(wrap);
		wrap->clear();
		Ui::AddSkip(wrap);
		Ui::AddSubsectionTitle(
			wrap,
			rpl::single(u"Сессии (%1)"_q.arg(list.size())));
		if (list.empty()) {
			wrap->add(
				object_ptr<Ui::FlatLabel>(
					wrap,
					u"Загрузка... (если пусто - нет связи с Telegram)"_q,
					st::boxDividerLabel),
				st::boxRowPadding);
			return;
		}
		if (!Main::AccountIsLive(account)) {
			return;
		}
		namespace Sessions = Core::GgtgSessions;
		const auto userId = QString::number(account->session().userId().bare);
		auto stored = Sessions::StoredForUser(Sessions::LoadVault(), userId);

		// Helper to match device strings flexibly (collapsing multiple spaces,
		// case-insensitive, and handling brand prefixes like "Huawei  nova Y70 Plus" vs "Huawei nova Y70 Plus").
		const auto matchDev = [](const QString &liveName, const QString &storedModel) {
			const auto a = liveName.simplified().trimmed();
			const auto b = storedModel.simplified().trimmed();
			if (a.isEmpty() || b.isEmpty()) {
				return false;
			}
			if (a.compare(b, Qt::CaseInsensitive) == 0) {
				return true;
			}
			if (a.contains(b, Qt::CaseInsensitive) || b.contains(a, Qt::CaseInsensitive)) {
				return true;
			}
			return false;
		};

		// First pass: match any stored sessions with hash == 0 (or mismatch)
		// to an active session by matching device model
		for (auto &item : stored) {
			if (Sessions::HasKey(item)) {
				const auto devModel = item.device.value(u"deviceModel"_q).toString();
				if (!devModel.isEmpty()) {
					for (const auto &entry : list) {
						if (entry.hash != 0 && matchDev(entry.name, devModel)) {
							if (!item.hash || item.hash != entry.hash) {
								item.hash = entry.hash;
								item.state = u"ok"_q;
								Sessions::UpdateStoredHash(item.id, item.hash);
							}
							break;
						}
					}
				}
			}
		}

		// Ensure any stored session present in the live list is marked "ok"
		if (list.size() > 0) {
			for (auto &item : stored) {
				if (item.hash) {
					const auto aliveInList = ranges::any_of(list, [&](
							const Api::Authorizations::Entry &entry) {
						if (entry.hash && entry.hash == item.hash) {
							return true;
						}
						const auto devModel = item.device.value(u"deviceModel"_q).toString();
						return !devModel.isEmpty() && matchDev(entry.name, devModel);
					});
					if (aliveInList && item.state != u"ok"_q) {
						item.state = u"ok"_q;
						Sessions::UpdateStoredState(item.id, u"ok"_q);
					}
				}
			}
		}
		// "резервная", "резервная 2", ... - numbered by age within a role.
		const auto titleOf = [=](const Sessions::StoredSession &item) {
			auto number = 1;
			for (const auto &other : stored) {
				if (other.role == item.role
					&& other.id != item.id
					&& (other.createdAt < item.createdAt
						|| (other.createdAt == item.createdAt
							&& other.id < item.id))) {
					++number;
				}
			}
			auto baseRole = Sessions::RoleTitle(item.role);
			if (!item.note.isEmpty()) {
				if (item.note.contains(u"Android"_q, Qt::CaseInsensitive)) {
					baseRole = (item.role == u"exported"_q)
						? u"экспорт для Android"_q
						: (baseRole + u" (Android)"_q);
				} else if (item.note.contains(u"iOS"_q, Qt::CaseInsensitive)) {
					baseRole = (item.role == u"exported"_q)
						? u"экспорт для iOS"_q
						: (baseRole + u" (iOS)"_q);
				}
			}
			return baseRole
				+ ((number > 1) ? (u" "_q + QString::number(number)) : QString());
		};
		const auto storedByHash = [&](uint64 hash, const QString &devName)
				-> const Sessions::StoredSession* {
			if (hash) {
				for (const auto &item : stored) {
					if (item.hash && item.hash == hash) {
						return &item;
					}
				}
			}
			if (!devName.isEmpty()) {
				for (const auto &item : stored) {
					if (Sessions::HasKey(item)) {
						const auto devModel = item.device.value(u"deviceModel"_q).toString();
						if (!devModel.isEmpty() && matchDev(devName, devModel)) {
							return &item;
						}
					}
				}
			}
			return nullptr;
		};
		auto sorted = list;
		ranges::stable_sort(sorted, [](
				const Api::Authorizations::Entry &a,
				const Api::Authorizations::Entry &b) {
			if ((a.hash == 0) != (b.hash == 0)) {
				return a.hash == 0;
			}
			return a.activeTime > b.activeTime;
		});
		for (const auto &entry : sorted) {
			auto suffix = QString();
			const auto spare = storedByHash(entry.hash, entry.name);
			if (entry.hash == 0) {
				suffix = u" (основная · эта сессия)"_q;
			} else if (spare) {
				suffix = u" ("_q + titleOf(*spare)
					+ u" · "_q + QDateTime::fromSecsSinceEpoch(
						spare->createdAt).toString(u"dd.MM.yyyy"_q)
					+ u")"_q;
			}
			if (entry.incomplete) {
				suffix += u" · не подтверждена"_q;
			}
			// A row like in Telegram's own list: device icon, name, app,
			// place and time, a cross to end the session; a click on the
			// row opens its details. This app's own session has no cross
			// (ending it would just log the account out).
			const auto hash = entry.hash;
			const auto device = entry.name;
			const auto canSwitch = (spare
				&& Sessions::HasKey(*spare)
				&& QDir(Sessions::VaultFolder(*spare) + u"/tdata"_q).exists());
			const auto storedSpare = spare ? *spare : Sessions::StoredSession();
			wrap->add(MakeSessionRow(
				wrap,
				entry,
				suffix,
				[=] {
					ShowSessionInfo(
						box->uiShow(),
						entry,
						[=](uint64 one) {
							terminate(one, device);
						},
						spare ? Fn<void()>([=] {
							ExportSpecificSession(box->uiShow(), account, storedSpare);
						}) : Fn<void()>(),
						canSwitch ? Fn<void()>([=] {
							(*confirmSwitch)(storedSpare);
						}) : Fn<void()>());
				},
				hash
					? Fn<void()>([=] { terminate(hash, device); })
					: Fn<void()>(),
				canSwitch
					? Fn<void()>([=] { (*confirmSwitch)(storedSpare); })
					: Fn<void()>()));
		}
		if (sorted.size() > 1) {
			wrap->add(
				object_ptr<Ui::LinkButton>(
					wrap,
					u"Завершить все остальные сессии"_q),
				style::margins(
					st::boxRowPadding.left(),
					st::defaultVerticalListSkip,
					st::boxRowPadding.right(),
					st::defaultVerticalListSkip)
			)->setClickedCallback([=] {
				terminate(0, QString());
			});
		}

		// Stored vault sessions not matched to an active session above:
		// still switchable and removable from here.
		for (const auto &item : stored) {
			if (!Sessions::HasKey(item)
				|| !QDir(Sessions::VaultFolder(item) + u"/tdata"_q).exists()) {
				continue;
			}
			const auto matchedAbove = ranges::any_of(sorted, [&](
					const Api::Authorizations::Entry &entry) {
				if (entry.hash && item.hash && entry.hash == item.hash) {
					return true;
				}
				const auto devModel = item.device.value(u"deviceModel"_q).toString();
				return !devModel.isEmpty() && matchDev(entry.name, devModel);
			});
			if (matchedAbove) {
				continue;
			}
			const auto stored = item;
			Api::Authorizations::Entry synth;
			synth.name = titleOf(item);
			synth.platform = item.device.value(u"systemVersion"_q).toString();
			synth.system = synth.platform;
			synth.info = item.device.value(u"appVersion"_q).toString();
			synth.location = u"Сохранена в программе"_q;
			synth.activeTime = static_cast<TimeId>(item.createdAt);
			const auto createdDt = QDateTime::fromSecsSinceEpoch(item.createdAt);
			synth.active = createdDt.toString(u"dd.MM.yyyy hh:mm"_q);
			wrap->add(MakeSessionRow(
				wrap,
				synth,
				u" (резервная · "_q + createdDt.toString(u"dd.MM.yyyy"_q) + u")"_q,
				[=] {
					ShowSessionInfo(
						box->uiShow(),
						synth,
						[=](uint64) {
							Sessions::RemoveStored(stored.id);
							if (Main::AccountIsLive(account)) {
								(*fillAgain)();
							}
						},
						[=] {
							ExportSpecificSession(box->uiShow(), account, stored);
						},
						[=] {
							(*confirmSwitch)(stored);
						});
				},
				[=] {
					Sessions::RemoveStored(stored.id);
					if (Main::AccountIsLive(account)) {
						(*fillAgain)();
					}
				},
				[=] {
					(*confirmSwitch)(stored);
				}));
		}
		wrap->add(
			object_ptr<Ui::LinkButton>(
				wrap,
				u"Создать резервную сессию"_q),
			style::margins(
				st::boxRowPadding.left(),
				st::defaultVerticalListSkip,
				st::boxRowPadding.right(),
				st::defaultVerticalListSkip)
		)->setClickedCallback([=] {
			if (!Main::AccountIsLive(account)) {
				return;
			}
			const auto show = box->uiShow();
			if (!Core::GuardAllows(show, Core::GuardedAction::ChangeSessions)) {
				return;
			}
			Ggtg::Notice(show, u"Создаём резервную сессию..."_q);
			CreateBackupSession(account, QString(), [=](QString error) {
				Ggtg::Notice(show, error.isEmpty()
					? u"Резервная сессия создана"_q
					: (u"Не удалось создать: "_q + error));
				if (Main::AccountIsLive(account)) {
					account->session().api().authorizations().reload();
					(*fillAgain)();
				}
			});
		});

		wrap->add(
			object_ptr<Ui::LinkButton>(
				wrap,
				u"Экспортировать сохранённую сессию..."_q),
			style::margins(
				st::boxRowPadding.left(),
				0,
				st::boxRowPadding.right(),
				st::defaultVerticalListSkip)
		)->setClickedCallback([=] {
			if (!Main::AccountIsLive(account)) {
				return;
			}
			ShowStoredSessionPicker(box->uiShow(), account, stored);
		});

		wrap->add(
			object_ptr<Ui::LinkButton>(
				wrap,
				u"Защита от угона (Анти-угон)..."_q),
			style::margins(
				st::boxRowPadding.left(),
				0,
				st::boxRowPadding.right(),
				st::defaultVerticalListSkip)
		)->setClickedCallback([=] {
			ShowAccountAntiTheftBox(box->uiShow(), account);
		});
	};
	*fillAgain = [=] { fill(authorizations->list()); };
	fill(authorizations->list());
	authorizations->listValue(
	) | rpl::on_next(fill, wrap->lifetime());
	authorizations->reload();
}

[[nodiscard]] QString FormatUtcOffset(int seconds) {
	const auto minutes = std::abs(seconds) / 60;
	return u"%1%2:%3"_q
		.arg(seconds < 0 ? u"-"_q : u"+"_q)
		.arg(minutes / 60, 2, 10, QChar('0'))
		.arg(minutes % 60, 2, 10, QChar('0'));
}

struct DevicePreset {
	QString model;
	QString version;
};

struct DeviceGroup {
	QString title;
	std::vector<DevicePreset> items;
};

struct LangPreset {
	QString name;
	QString systemCode;
	QString cloudCode;
};

// Believable device / OS pairs, grouped by platform, so the OS always
// matches the model (no "MacBook" running Windows).
[[nodiscard]] const std::vector<DeviceGroup> &DevicePresets() {
	static const auto result = std::vector<DeviceGroup>{
		{ u"macOS"_q, {
			{ u"MacBook Pro 14\" (M1)"_q, u"macOS 14.7.6"_q },
			{ u"MacBook Pro 14\" (M2)"_q, u"macOS 15.3"_q },
			{ u"MacBook Pro 14\" (M3)"_q, u"macOS 15.6"_q },
			{ u"MacBook Pro 14\" (M4)"_q, u"macOS 15.7"_q },
			{ u"MacBook Pro 14\" (M5)"_q, u"macOS 26.1"_q },
			{ u"MacBook Pro 16\" (M1)"_q, u"macOS 14.6"_q },
			{ u"MacBook Pro 16\" (M2)"_q, u"macOS 15.2"_q },
			{ u"MacBook Pro 16\" (M3)"_q, u"macOS 15.5"_q },
			{ u"MacBook Pro 16\" (M4)"_q, u"macOS 26.0"_q },
			{ u"MacBook Air 13\" (M1)"_q, u"macOS 14.7.6"_q },
			{ u"MacBook Air 13\" (M2)"_q, u"macOS 15.1"_q },
			{ u"MacBook Air 13\" (M3)"_q, u"macOS 15.6"_q },
			{ u"MacBook Air 13\" (M4)"_q, u"macOS 26.0"_q },
			{ u"MacBook Air 15\" (M2)"_q, u"macOS 15.4"_q },
			{ u"MacBook Air 15\" (M3)"_q, u"macOS 15.6"_q },
			{ u"MacBook Air 15\" (M4)"_q, u"macOS 26.1"_q },
			{ u"iMac (M1)"_q, u"macOS 14.6"_q },
			{ u"iMac (M3)"_q, u"macOS 15.5"_q },
			{ u"iMac (M4)"_q, u"macOS 26.0"_q },
			{ u"Mac mini (M1)"_q, u"macOS 14.7.6"_q },
			{ u"Mac mini (M2)"_q, u"macOS 15.4"_q },
			{ u"Mac mini (M4)"_q, u"macOS 26.1"_q },
		} },
		{ u"Windows"_q, {
			{ u"PC 64bit"_q, u"Windows 11"_q },
			{ u"PC 64bit"_q, u"Windows 10"_q },
			{ u"Dell XPS 15"_q, u"Windows 11"_q },
			{ u"ASUS ROG Zephyrus"_q, u"Windows 11"_q },
			{ u"Lenovo ThinkPad X1"_q, u"Windows 10"_q },
			{ u"HP Pavilion"_q, u"Windows 11"_q },
		} },
		{ u"Linux"_q, {
			{ u"PC 64bit"_q, u"Ubuntu 24.04 (GNOME)"_q },
			{ u"PC 64bit"_q, u"Ubuntu 22.04 (X11)"_q },
			{ u"PC 64bit"_q, u"Fedora Linux 41 (GNOME)"_q },
			{ u"PC 64bit"_q, u"Debian GNU/Linux 12"_q },
			{ u"PC 64bit"_q, u"Arch Linux"_q },
		} },
	};
	return result;
}

// ggtg: every account gets its own device of the kind this client really is, so the session list
// shows a believable machine: Windows laptops (model codes of the reference generator) on Windows,
// MacBook / iMac / Mac mini on macOS, the Linux presets elsewhere.
[[nodiscard]] DevicePreset PickHostDevice() {
#ifdef Q_OS_WIN
	const auto laptop = Ggtg::DesktopProfiles::RandomWindowsDevice();
	return { laptop.model, laptop.system };
#else // Q_OS_WIN
#ifdef Q_OS_MAC
	const auto title = u"macOS"_q;
#else // Q_OS_MAC
	const auto title = u"Linux"_q;
#endif // Q_OS_MAC
	for (const auto &group : DevicePresets()) {
		if (group.title == title) {
			return group.items[base::RandomIndex(int(group.items.size()))];
		}
	}
	return DevicePresets().front().items.front();
#endif // Q_OS_WIN
}

[[nodiscard]] const std::vector<LangPreset> &LangPresets() {
	static const auto result = std::vector<LangPreset>{
		{ u"Русский"_q, u"ru-RU"_q, u"ru"_q },
		{ u"Українська"_q, u"uk-UA"_q, u"uk"_q },
		{ u"English (US)"_q, u"en-US"_q, u"en"_q },
		{ u"English (UK)"_q, u"en-GB"_q, u"en"_q },
		{ u"Deutsch"_q, u"de-DE"_q, u"de"_q },
		{ u"Español"_q, u"es-ES"_q, u"es"_q },
		{ u"Français"_q, u"fr-FR"_q, u"fr"_q },
		{ u"Italiano"_q, u"it-IT"_q, u"it"_q },
		{ u"Português (Brasil)"_q, u"pt-BR"_q, u"pt-br"_q },
		{ u"Türkçe"_q, u"tr-TR"_q, u"tr"_q },
		{ u"Polski"_q, u"pl-PL"_q, u"pl"_q },
		{ u"Nederlands"_q, u"nl-NL"_q, u"nl"_q },
		{ u"Čeština"_q, u"cs-CZ"_q, u"cs"_q },
		{ u"العربية"_q, u"ar-SA"_q, u"ar"_q },
		{ u"Bahasa Indonesia"_q, u"id-ID"_q, u"id"_q },
		{ u"한국어"_q, u"ko-KR"_q, u"ko"_q },
	};
	return result;
}

// Country (ISO 3166 alpha-2) -> the closest preset from LangPresets().
[[nodiscard]] const LangPreset *LangForCountry(const QString &country) {
	static const auto map = std::map<QString, QString>{
		{ u"RU"_q, u"ru-RU"_q }, { u"BY"_q, u"ru-RU"_q },
		{ u"KZ"_q, u"ru-RU"_q }, { u"KG"_q, u"ru-RU"_q },
		{ u"UA"_q, u"uk-UA"_q },
		{ u"GB"_q, u"en-GB"_q }, { u"IE"_q, u"en-GB"_q },
		{ u"DE"_q, u"de-DE"_q }, { u"AT"_q, u"de-DE"_q },
		{ u"CH"_q, u"de-DE"_q },
		{ u"ES"_q, u"es-ES"_q }, { u"MX"_q, u"es-ES"_q },
		{ u"AR"_q, u"es-ES"_q }, { u"CO"_q, u"es-ES"_q },
		{ u"CL"_q, u"es-ES"_q }, { u"PE"_q, u"es-ES"_q },
		{ u"FR"_q, u"fr-FR"_q }, { u"BE"_q, u"fr-FR"_q },
		{ u"IT"_q, u"it-IT"_q },
		{ u"BR"_q, u"pt-BR"_q }, { u"PT"_q, u"pt-BR"_q },
		{ u"TR"_q, u"tr-TR"_q },
		{ u"PL"_q, u"pl-PL"_q },
		{ u"NL"_q, u"nl-NL"_q },
		{ u"CZ"_q, u"cs-CZ"_q },
		{ u"SA"_q, u"ar-SA"_q }, { u"AE"_q, u"ar-SA"_q },
		{ u"EG"_q, u"ar-SA"_q },
		{ u"ID"_q, u"id-ID"_q },
		{ u"KR"_q, u"ko-KR"_q },
	};
	const auto i = map.find(country.toUpper());
	const auto code = (i != map.end()) ? i->second : u"en-US"_q;
	for (const auto &preset : LangPresets()) {
		if (preset.systemCode == code) {
			return &preset;
		}
	}
	return nullptr;
}

struct GeoResult {
	QString ip;
	QString country;
	LangPreset lang;
	int offset = 0;
};

// Asks ip-api.com which country / UTC offset the IP behind `proxy` (SOCKS5
// or HTTP; no proxy = this computer's own IP) belongs to. `done` always
// fires once: a result, or an error text.
void DetectGeoNow(
		const MTP::ProxyData &proxy,
		Fn<void(std::optional<GeoResult>, QString)> done) {
	const auto real = MTP::HasRealProxy(proxy);
	if (real
		&& proxy.type != MTP::ProxyData::Type::Socks5
		&& proxy.type != MTP::ProxyData::Type::Http) {
		done(
			std::nullopt,
			u"Прокси этого типа нельзя использовать для определения IP "
				"(нужен SOCKS5 или HTTP)"_q);
		return;
	}
	const auto network = new QNetworkAccessManager(qApp);
	if (real) {
		network->setProxy(QNetworkProxy(
			(proxy.type == MTP::ProxyData::Type::Socks5)
				? QNetworkProxy::Socks5Proxy
				: QNetworkProxy::HttpProxy,
			proxy.host,
			quint16(proxy.port),
			proxy.user,
			proxy.password));
	}
	auto request = QNetworkRequest(QUrl(
		u"http://ip-api.com/json/"
			"?fields=status,message,query,countryCode,offset"_q));
	request.setTransferTimeout(15000);
	const auto reply = network->get(request);
	QObject::connect(reply, &QNetworkReply::finished, network, [=] {
		const auto data = reply->readAll();
		const auto failed = (reply->error() != QNetworkReply::NoError);
		const auto json = QJsonDocument::fromJson(data).object();
		network->deleteLater();
		if (failed || json.value("status").toString() != u"success"_q) {
			done(
				std::nullopt,
				u"Не удалось определить IP — проверьте прокси"_q);
			return;
		}
		const auto country = json.value("countryCode").toString();
		const auto lang = LangForCountry(country);
		done(GeoResult{
			.ip = json.value("query").toString(),
			.country = country,
			.lang = lang ? *lang : LangPreset{
				u"English (US)"_q, u"en-US"_q, u"en"_q },
			.offset = json.value("offset").toInt(),
		}, QString());
	});
}

// ip-api.com counts requests per source IP, i.e. per proxy: only accounts
// sharing one proxy (or none) wait for each other, 1.5 s apart.
void DetectGeoForProxy(
		const MTP::ProxyData &proxy,
		Fn<void(std::optional<GeoResult>, QString)> done) {
	static auto nextFree = std::map<QString, crl::time>();
	const auto key = MTP::HasRealProxy(proxy)
		? (proxy.host + ':' + QString::number(proxy.port))
		: QString();
	const auto now = crl::now();
	auto &slot = nextFree[key];
	const auto start = std::max(slot, now);
	slot = start + 1500;
	if (start > now) {
		base::call_delayed(start - now, [=] {
			DetectGeoNow(proxy, done);
		});
	} else {
		DetectGeoNow(proxy, done);
	}
}

void ShowEditDeviceBox(
		std::shared_ptr<Ui::Show> show,
		not_null<Main::Account*> account,
		Fn<void()> onSaved) {
	if (!Main::AccountIsLive(account)) {
		Ggtg::Notice(show, u"Нет активной сессии"_q);
		return;
	}
	const auto current = account->deviceOverride();

	enum class PlatformMode {
		Windows,
		Mac,
		iPhone,
	};

	struct DeviceBoxState {
		PlatformMode mode = PlatformMode::Windows;
		QString deviceModel;
		QString systemVersion;
		QString appVersion;
		QString langPack;
		int apiId = 2040;
		QString apiHash;
	};

	auto initialMode = PlatformMode::Windows;
	if (IsIosSession(current)) {
		initialMode = PlatformMode::iPhone;
	} else if (current.systemVersion.startsWith(u"macOS"_q)
		|| current.deviceModel.startsWith(u"MacBook"_q)
		|| current.deviceModel.startsWith(u"iMac"_q)
		|| current.deviceModel.startsWith(u"Mac mini"_q)
		|| current.deviceModel.startsWith(u"Mac Studio"_q)
		|| (Platform::IsMac() && (IsAndroidSession(current) || current.deviceModel.isEmpty()))) {
		initialMode = PlatformMode::Mac;
	}

	const auto state = std::make_shared<DeviceBoxState>();
	state->mode = initialMode;
	state->deviceModel = current.deviceModel;
	state->systemVersion = current.systemVersion;
	state->appVersion = current.appVersion;
	state->langPack = current.langPackName;
	state->apiId = current.apiId > 0 ? current.apiId : Ggtg::DesktopProfiles::kAppId;
	state->apiHash = !current.apiHash.isEmpty()
		? current.apiHash
		: QString::fromLatin1(Ggtg::DesktopProfiles::kAppHash);

	// Ensure valid initial values if empty or if previously Android on desktop
	if (state->deviceModel.isEmpty() || current.langPackName == u"android"_q || current.systemVersion.startsWith(u"SDK "_q)) {
		if (state->mode == PlatformMode::Windows) {
			state->deviceModel = u"PC 64bit"_q;
			state->systemVersion = u"Windows 11 x64"_q;
			state->appVersion = u"7.3.1 x64"_q;
			state->langPack = u"tdesktop"_q;
			state->apiId = Ggtg::DesktopProfiles::kAppId;
			state->apiHash = QString::fromLatin1(Ggtg::DesktopProfiles::kAppHash);
		} else if (state->mode == PlatformMode::Mac) {
			state->deviceModel = u"MacBook Pro 14\""_q;
			state->systemVersion = u"macOS 15.1"_q;
			state->appVersion = u"7.3.1"_q;
			state->langPack = u"tdesktop"_q;
			state->apiId = Ggtg::DesktopProfiles::kAppId;
			state->apiHash = QString::fromLatin1(Ggtg::DesktopProfiles::kAppHash);
		} else {
			state->deviceModel = u"iPhone 18 Pro Max"_q;
			state->systemVersion = u"iOS 27.0.1"_q;
			state->appVersion = u"11.5.0"_q;
			state->langPack = QString::fromLatin1(Ggtg::IosProfiles::kLangPack);
			state->apiId = Ggtg::IosProfiles::kAppId;
			state->apiHash = QString::fromLatin1(Ggtg::IosProfiles::kAppHash);
		}
	}

	show->showBox(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"Параметры устройства"_q));
		Ui::AddDividerText(
			box->verticalLayout(),
			rpl::single(u"Выберите платформу. Все параметры (модель, ОС, версия Telegram и lang_pack) строго согласованы между собой, что исключает сбои и ошибки в Telegram."_q));

		Ui::AddSkip(box->verticalLayout());

		const auto summaryVar = std::make_shared<rpl::variable<QString>>();

		const auto updateSummary = [=] {
			auto text = QString();
			if (state->mode == PlatformMode::Windows) {
				text += u"Платформа: Windows (ПК / Ноутбук)\n"_q;
				text += u"Модель: "_q + Ggtg::DesktopProfiles::WindowsLabel(state->deviceModel) + u"\n"_q;
			} else if (state->mode == PlatformMode::Mac) {
				text += u"Платформа: macOS (MacBook / iMac)\n"_q;
				text += u"Модель: "_q + state->deviceModel + u"\n"_q;
			} else {
				text += u"Платформа: Apple iPhone (iOS)\n"_q;
				text += u"Модель: "_q + state->deviceModel + u"\n"_q;
			}
			text += u"Версия ОС: "_q + state->systemVersion + u"\n"_q;
			text += u"Версия Telegram: "_q + state->appVersion + u"\n"_q;
			text += u"Пакет языка: "_q + state->langPack + u"  ·  API ID: "_q + QString::number(state->apiId);
			*summaryVar = text;
		};

		const auto applyPlatform = [=](PlatformMode newMode) {
			state->mode = newMode;
			if (newMode == PlatformMode::Windows) {
				state->deviceModel = u"PC 64bit"_q;
				state->systemVersion = u"Windows 11 x64"_q;
				state->appVersion = u"7.3.1 x64"_q;
				state->langPack = u"tdesktop"_q;
				state->apiId = Ggtg::DesktopProfiles::kAppId;
				state->apiHash = QString::fromLatin1(Ggtg::DesktopProfiles::kAppHash);
			} else if (newMode == PlatformMode::Mac) {
				state->deviceModel = u"MacBook Pro 14\""_q;
				state->systemVersion = u"macOS 15.1"_q;
				state->appVersion = u"7.3.1"_q;
				state->langPack = u"tdesktop"_q;
				state->apiId = Ggtg::DesktopProfiles::kAppId;
				state->apiHash = QString::fromLatin1(Ggtg::DesktopProfiles::kAppHash);
			} else {
				state->deviceModel = u"iPhone 18 Pro Max"_q;
				state->systemVersion = u"iOS 27.0.1"_q;
				state->appVersion = u"11.5.0"_q;
				state->langPack = QString::fromLatin1(Ggtg::IosProfiles::kLangPack);
				state->apiId = Ggtg::IosProfiles::kAppId;
				state->apiHash = QString::fromLatin1(Ggtg::IosProfiles::kAppHash);
			}
			updateSummary();
		};

		updateSummary();

		Ui::AddSubsectionTitle(box->verticalLayout(), rpl::single(u"1. Выберите платформу"_q));

		const auto platformGroup = std::make_shared<Ui::RadioenumGroup<PlatformMode>>(state->mode);
		box->addRow(
			object_ptr<Ui::Radioenum<PlatformMode>>(
				box,
				platformGroup,
				PlatformMode::Windows,
				u"Windows (ПК / Ноутбук)"_q),
			style::margins(style::ConvertScale(22), style::ConvertScale(4), style::ConvertScale(22), style::ConvertScale(4)));
		box->addRow(
			object_ptr<Ui::Radioenum<PlatformMode>>(
				box,
				platformGroup,
				PlatformMode::Mac,
				u"macOS (MacBook / iMac)"_q),
			style::margins(style::ConvertScale(22), style::ConvertScale(4), style::ConvertScale(22), style::ConvertScale(4)));
		box->addRow(
			object_ptr<Ui::Radioenum<PlatformMode>>(
				box,
				platformGroup,
				PlatformMode::iPhone,
				u"Apple iPhone (iOS)"_q),
			style::margins(style::ConvertScale(22), style::ConvertScale(4), style::ConvertScale(22), style::ConvertScale(4)));

		platformGroup->setChangedCallback([=](PlatformMode mode) {
			applyPlatform(mode);
		});

		Ui::AddSkip(box->verticalLayout());
		Ui::AddSubsectionTitle(box->verticalLayout(), rpl::single(u"2. Текущие параметры"_q));

		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			summaryVar->value(),
			st::boxLabel));

		Ui::AddSkip(box->verticalLayout());
		Ui::AddSubsectionTitle(box->verticalLayout(), rpl::single(u"3. Заготовки для выбранной платформы"_q));

		box->addRow(object_ptr<Ui::SettingsButton>(
			box,
			rpl::single(u"Выбрать модель устройства..."_q),
			st::settingsButton
		))->setClickedCallback([=] {
			show->showBox(Box([=](not_null<Ui::GenericBox*> mBox) {
				mBox->setTitle(rpl::single(u"Выберите модель устройства"_q));
				if (state->mode == PlatformMode::Windows) {
					for (const auto &w : Ggtg::DesktopProfiles::kCuratedWindowsDevices) {
						const auto code = QString::fromUtf8(w.code);
						const auto lbl = QString::fromUtf8(w.label);
						mBox->addRow(object_ptr<Ui::SettingsButton>(
							mBox,
							rpl::single(lbl)
						))->setClickedCallback([=] {
							state->deviceModel = code;
							updateSummary();
							mBox->closeBox();
						});
					}
				} else if (state->mode == PlatformMode::Mac) {
					for (const auto name : Ggtg::DesktopProfiles::kMacDevices) {
						const auto dev = QString::fromUtf8(name);
						mBox->addRow(object_ptr<Ui::SettingsButton>(
							mBox,
							rpl::single(dev)
						))->setClickedCallback([=] {
							state->deviceModel = dev;
							updateSummary();
							mBox->closeBox();
						});
					}
				} else {
					for (const auto name : Ggtg::IosProfiles::kIosDevices) {
						const auto dev = QString::fromUtf8(name);
						mBox->addRow(object_ptr<Ui::SettingsButton>(
							mBox,
							rpl::single(dev)
						))->setClickedCallback([=] {
							state->deviceModel = dev;
							updateSummary();
							mBox->closeBox();
						});
					}
				}
				mBox->addButton(rpl::single(u"Назад"_q), [=] { mBox->closeBox(); });
			}));
		});

		box->addRow(object_ptr<Ui::SettingsButton>(
			box,
			rpl::single(u"Выбрать версию ОС..."_q),
			st::settingsButton
		))->setClickedCallback([=] {
			show->showBox(Box([=](not_null<Ui::GenericBox*> sBox) {
				sBox->setTitle(rpl::single(u"Выберите версию ОС"_q));
				if (state->mode == PlatformMode::Windows) {
					for (const auto sys : Ggtg::DesktopProfiles::kDesktopSystems) {
						const auto s = QString::fromUtf8(sys);
						sBox->addRow(object_ptr<Ui::SettingsButton>(
							sBox,
							rpl::single(s)
						))->setClickedCallback([=] {
							state->systemVersion = s;
							updateSummary();
							sBox->closeBox();
						});
					}
				} else if (state->mode == PlatformMode::Mac) {
					for (const auto sys : Ggtg::DesktopProfiles::kMacSystems) {
						const auto s = QString::fromUtf8(sys);
						sBox->addRow(object_ptr<Ui::SettingsButton>(
							sBox,
							rpl::single(s)
						))->setClickedCallback([=] {
							state->systemVersion = s;
							updateSummary();
							sBox->closeBox();
						});
					}
				} else {
					for (const auto sys : Ggtg::IosProfiles::kIosSystems) {
						const auto s = QString::fromUtf8(sys);
						sBox->addRow(object_ptr<Ui::SettingsButton>(
							sBox,
							rpl::single(s)
						))->setClickedCallback([=] {
							state->systemVersion = s;
							updateSummary();
							sBox->closeBox();
						});
					}
				}
				sBox->addButton(rpl::single(u"Назад"_q), [=] { sBox->closeBox(); });
			}));
		});

		box->addRow(object_ptr<Ui::SettingsButton>(
			box,
			rpl::single(u"Выбрать версию Telegram..."_q),
			st::settingsButton
		))->setClickedCallback([=] {
			show->showBox(Box([=](not_null<Ui::GenericBox*> vBox) {
				vBox->setTitle(rpl::single(u"Выберите версию Telegram"_q));
				if (state->mode == PlatformMode::Windows) {
					for (const auto ver : Ggtg::DesktopProfiles::kDesktopVersions) {
						const auto v = QString::fromUtf8(ver);
						vBox->addRow(object_ptr<Ui::SettingsButton>(
							vBox,
							rpl::single(v)
						))->setClickedCallback([=] {
							state->appVersion = v;
							updateSummary();
							vBox->closeBox();
						});
					}
				} else if (state->mode == PlatformMode::Mac) {
					for (const auto ver : Ggtg::DesktopProfiles::kMacVersions) {
						const auto v = QString::fromUtf8(ver);
						vBox->addRow(object_ptr<Ui::SettingsButton>(
							vBox,
							rpl::single(v)
						))->setClickedCallback([=] {
							state->appVersion = v;
							updateSummary();
							vBox->closeBox();
						});
					}
				} else {
					for (const auto ver : Ggtg::IosProfiles::kIosVersions) {
						const auto v = QString::fromUtf8(ver);
						vBox->addRow(object_ptr<Ui::SettingsButton>(
							vBox,
							rpl::single(v)
						))->setClickedCallback([=] {
							state->appVersion = v;
							updateSummary();
							vBox->closeBox();
						});
					}
				}
				vBox->addButton(rpl::single(u"Назад"_q), [=] { vBox->closeBox(); });
			}));
		});

		box->addRow(object_ptr<Ui::SettingsButton>(
			box,
			rpl::single(u"Случайный профиль этой платформы"_q),
			st::settingsButton
		))->setClickedCallback([=] {
			if (state->mode == PlatformMode::Windows) {
				const auto p = Ggtg::DesktopProfiles::RandomWindowsProfile();
				state->deviceModel = p.deviceModel;
				state->systemVersion = p.systemVersion;
				state->appVersion = p.appVersion;
			} else if (state->mode == PlatformMode::Mac) {
				const auto p = Ggtg::DesktopProfiles::RandomMacProfile();
				state->deviceModel = p.deviceModel;
				state->systemVersion = p.systemVersion;
				state->appVersion = p.appVersion;
			} else {
				const auto p = Ggtg::IosProfiles::RandomProfile();
				state->deviceModel = p.deviceModel;
				state->systemVersion = p.systemVersion;
				state->appVersion = p.appVersion;
			}
			updateSummary();
		});

		const auto save = [=] {
			auto updated = current;
			updated.deviceModel = state->deviceModel;
			updated.systemVersion = state->systemVersion;
			updated.appVersion = state->appVersion;
			updated.langPackName = state->langPack;
			updated.apiId = state->apiId;
			updated.apiHash = state->apiHash;

			account->setDeviceOverride(updated);
			box->closeBox();
			Ggtg::Notice(show, u"Параметры устройства сохранены"_q);
			if (onSaved) {
				onSaved();
			}
		};

		box->addButton(rpl::single(u"Сохранить"_q), save);
		box->addButton(rpl::single(u"Отмена"_q), [=] {
			box->closeBox();
			if (onSaved) {
				onSaved();
			}
		});
	}));
}

void ShowAccountProfileBox(
		std::shared_ptr<Ui::Show> show,
		not_null<Main::Account*> account,
		bool current,
		Fn<void()> onSaved) {
	if (!Main::AccountIsLive(account)) {
		Ggtg::Notice(show, u"Аккаунт больше не залогинен"_q);
		return;
	}
	namespace Sessions = Core::GgtgSessions;
	const auto user = account->session().user();
	const auto rawUserId = account->session().userId().bare;
	const auto userId = QString::number(rawUserId);
	const auto idDate = Ggtg::EstimateIdDate(rawUserId);
	const auto password = account->twoFactorPassword();

	show->showBox(Box([=](not_null<Ui::GenericBox*> box) {
		box->setWidth(style::ConvertScale(540));
		box->setTitle(rpl::single(u"Профиль аккаунта"_q));

		// Avatar and photo change/delete section
		const auto avatarWrap = box->addRow(
			object_ptr<Ui::RpWidget>(box),
			st::boxRowPadding);
		const auto avatarSize = style::ConvertScale(72);
		avatarWrap->resize(box->width(), avatarSize + style::ConvertScale(8));

		class ProfilePicWidget final : public Ui::RpWidget {
		public:
			ProfilePicWidget(
				not_null<QWidget*> parent,
				not_null<Main::Account*> account,
				int size)
			: RpWidget(parent.get())
			, _account(account)
			, _size(size) {
				resize(size, size);
			}
		protected:
			void paintEvent(QPaintEvent *e) override {
				auto p = Painter(this);
				if (_account->sessionExists()) {
					_account->session().user()->paintUserpicLeft(p, _view, 0, 0, _size, _size);
				}
			}
		private:
			not_null<Main::Account*> _account;
			int _size = 0;
			Ui::PeerUserpicView _view;
		};

		const auto picWidget = Ui::CreateChild<ProfilePicWidget>(
			avatarWrap,
			account,
			avatarSize);
		picWidget->move(0, 0);

		const auto photoButtonsWrap = Ui::CreateChild<Ui::VerticalLayout>(avatarWrap);
		photoButtonsWrap->move(avatarSize + style::ConvertScale(16), 0);

		const auto changePhotoBtn = photoButtonsWrap->add(
			object_ptr<Ui::LinkButton>(photoButtonsWrap, u"Сменить фото профиля…"_q));
		changePhotoBtn->setClickedCallback([=] {
			FileDialog::GetOpenPath(
				box.get(),
				u"Выберите фото"_q,
				FileDialog::ImagesFilter(),
				[=](FileDialog::OpenResult &&result) {
					if (result.paths.empty()) {
						return;
					}
					StepChangePhoto(account, result.paths.front(), true, [=](bool ok, QString err) {
						if (ok) {
							Ggtg::Notice(box->uiShow(), u"Фото профиля обновлено"_q);
							picWidget->update();
						} else {
							Ggtg::Notice(box->uiShow(), u"Ошибка загрузки фото: "_q + err);
						}
					});
				});
		});

		const auto deletePhotoBtn = photoButtonsWrap->add(
			object_ptr<Ui::LinkButton>(photoButtonsWrap, u"Удалить текущее фото"_q),
			style::margins(0, style::ConvertScale(6), 0, 0));
		deletePhotoBtn->setClickedCallback([=] {
			const auto oldPhotoId = account->sessionExists()
				? account->session().user()->userpicPhotoId()
				: PhotoId(0);
			if (oldPhotoId) {
				StepDeleteCurrentPhoto(account, oldPhotoId, [=] {
					Ggtg::Notice(box->uiShow(), u"Фото профиля удалено"_q);
					picWidget->update();
				});
			} else {
				Ggtg::Notice(box->uiShow(), u"У аккаунта нет фото"_q);
			}
		});

		Ui::AddSkip(box->verticalLayout(), style::ConvertScale(8));

		const auto firstInput = box->addRow(
			object_ptr<Ui::InputField>(
				box,
				st::defaultInputField,
				rpl::single(u"Имя"_q),
				user->firstName),
			st::boxRowPadding);
		const auto lastInput = box->addRow(
			object_ptr<Ui::InputField>(
				box,
				st::defaultInputField,
				rpl::single(u"Фамилия"_q),
				user->lastName),
			st::boxRowPadding);
		const auto usernameInput = box->addRow(
			object_ptr<Ui::InputField>(
				box,
				st::defaultInputField,
				rpl::single(u"Юзернейм (@username)"_q),
				user->username()),
			st::boxRowPadding);
		const auto aboutInput = box->addRow(
			object_ptr<Ui::InputField>(
				box,
				st::defaultInputField,
				rpl::single(u"О себе (био)"_q),
				user->about()),
			st::boxRowPadding);
		aboutInput->setMaxLength(70);

		const auto saveWrap = box->addRow(
			object_ptr<Ui::VerticalLayout>(box),
			st::boxRowPadding);
		const auto saveBtn = saveWrap->add(
			object_ptr<Ui::RoundButton>(
				saveWrap,
				rpl::single(u"Сохранить профиль"_q),
				st::defaultBoxButton));
		saveBtn->setClickedCallback([=] {
			const auto newFirst = firstInput->getLastText().trimmed();
			const auto newLast = lastInput->getLastText().trimmed();
			auto newUsername = usernameInput->getLastText().trimmed();
			if (newUsername.startsWith('@')) {
				newUsername = newUsername.mid(1);
			}
			const auto newAbout = aboutInput->getLastText().trimmed();

			const auto profileChanged = (newFirst != user->firstName)
				|| (newLast != user->lastName)
				|| (newAbout != user->about());
			const auto usernameChanged = (newUsername != user->username());

			if (!profileChanged && !usernameChanged) {
				Ggtg::Notice(box->uiShow(), u"Изменений нет"_q);
				return;
			}

			const auto finishSave = [=] {
				if (usernameChanged) {
					StepSetUsernameTo(account, newUsername, [=](bool ok, QString err) {
						if (ok) {
							Ggtg::Notice(box->uiShow(), u"Профиль и юзернейм сохранены"_q);
						} else {
							Ggtg::Notice(box->uiShow(), u"Профиль сохранён, ошибка юзернейма: "_q + err);
						}
						if (onSaved) onSaved();
					});
				} else {
					Ggtg::Notice(box->uiShow(), u"Профиль сохранён"_q);
					if (onSaved) onSaved();
				}
			};

			if (profileChanged) {
				StepUpdateProfile(account, newFirst, newLast, newAbout, [=](bool ok, QString err) {
					if (ok) {
						finishSave();
					} else {
						Ggtg::Notice(box->uiShow(), u"Не удалось обновить профиль: "_q + err);
					}
				});
			} else {
				finishSave();
			}
		});

		Ui::AddSkip(box->verticalLayout(), st::defaultVerticalListSkip);
		Ui::AddSubsectionTitle(box->verticalLayout(), rpl::single(u"Информация об аккаунте"_q));

		auto lines = InfoLines();
		lines.emplace_back(u"Телефон"_q, OrDash(WithPlus(user->phone())), true);
		lines.emplace_back(u"ID"_q, userId, false, [=] {
			TextUtilities::SetClipboardText(TextForMimeData::Simple(QString::number(rawUserId)));
			Ggtg::Notice(box->uiShow(), u"ID скопирован"_q);
		});
		if (!idDate.isEmpty()) {
			lines.emplace_back(u"Регистрация"_q, idDate, false, [=] {
				TextUtilities::SetClipboardText(TextForMimeData::Simple(idDate));
				Ggtg::Notice(box->uiShow(), u"Дата регистрации скопирована"_q);
			});
		}
		lines.emplace_back(u"Папка"_q, OrDash(account->folderTag()));
		lines.emplace_back(
			u"Прокси"_q,
			ProxyText(account->proxyOverride()),
			false,
			[=] {
				if (Main::AccountIsLive(account)
					&& Core::GuardAllows(box->uiShow(), Core::GuardedAction::ReplaceProxy)) {
					box->uiShow()->showBox(MakeAccountProxyBox(account));
				}
			});
		{
			const auto own = account->deviceOverride();
			const auto devTitle = own.deviceModel.isEmpty()
				? u"по умолчанию (нажмите, чтобы изменить)"_q
				: (own.deviceModel + u" · "_q + OrDash(own.systemVersion)
					+ (own.appVersion.isEmpty() ? QString() : (u" ("_q + own.appVersion + u")"_q))
					+ (own.langPackName.isEmpty() ? QString() : (u" ["_q + own.langPackName + u"]"_q)));
			lines.emplace_back(
				u"Устройство"_q,
				devTitle,
				false,
				[=] {
					ShowEditDeviceBox(box->uiShow(), account, [=] {
						ShowAccountProfileBox(box->uiShow(), account, current, onSaved);
					});
				});
			if (IsAndroidSession(own)) {
				lines.emplace_back(
					u"Исправить профиль"_q,
					u"У аккаунта профиль Android на компьютере. Нажмите, чтобы вернуть Mac/ПК"_q,
					false,
					[=] {
						const auto resetDev = Platform::IsMac()
							? RandomMacDeviceOverride()
							: RandomDesktopDeviceOverride();
						account->setDeviceOverride(resetDev);
						Ggtg::Notice(box->uiShow(), u"Устройство сброшено на "_q + resetDev.deviceModel);
						ShowAccountProfileBox(box->uiShow(), account, current, onSaved);
					});
			}
		}
		lines.emplace_back(
			u"Пароль 2FA"_q,
			password.isEmpty() ? u"не сохранён"_q : password,
			!password.isEmpty());
		const auto status = LiveAccountStatus(account);
		lines.emplace_back(u"Статус"_q, status.text);
		lines.emplace_back(u"Что это значит"_q, status.detail);
		lines.emplace_back(
			u"Сессия"_q,
			current ? u"активна (текущий аккаунт)"_q : u"активна"_q);
		lines.emplace_back(
			u"SpamBot"_q,
			Settings::AccountSpamStatusText(user->id.value));
		const auto ggr = Settings::CalculateAccountGgr(account);
		lines.emplace_back(
			u"Рейтинг GGR"_q,
			QString::number(ggr.score, 'f', 1) + u" ("_q + ggr.tier + u")"_q);
		const auto history = Sessions::ForUser(Sessions::Load(), userId);
		if (history.empty()) {
			lines.emplace_back(u"Разрывы сессии"_q, u"не было"_q);
		} else {
			const auto &last = history.back();
			lines.emplace_back(
				u"Разрывы сессии"_q,
				u"%1 (последний: %2, %3)"_q
					.arg(history.size())
					.arg(StampText(last.at))
					.arg(Sessions::ReasonTitle(last.reason)));
		}

		for (const auto &line : lines) {
			const auto clickable = (line.copy || line.action)
				&& !line.value.isEmpty()
				&& (line.value != u"—"_q || line.action);
			if (!clickable) {
				box->addRow(
					object_ptr<Ui::FlatLabel>(
						box,
						line.label + u": "_q + line.value,
						st::boxLabel),
					st::boxRowPadding);
				continue;
			}
			const auto row = box->addRow(
				object_ptr<Ui::FlatLabel>(
					box,
					rpl::single(TextWithEntities()),
					st::boxLabel),
				st::boxRowPadding);
			auto marked = TextWithEntities{ line.label + u": "_q };
			marked.append(Ui::Text::Link(line.value));
			row->setMarkedText(marked);
			const auto value = line.value;
			const auto label = line.label;
			const auto copy = line.copy;
			const auto action = line.action;
			row->setClickHandlerFilter([=](const auto &...) {
				if (action) {
					action();
				} else if (copy) {
					TextUtilities::SetClipboardText(
						TextForMimeData::Simple(value));
					Ggtg::Notice(box->uiShow(), label + u" скопирован(о)"_q);
				}
				return false;
			});
		}

		AddSessionsSection(box, account);

		box->addButton(tr::lng_close(), [=] { box->closeBox(); });
	}));
}

void ShowAccountInfoBox(
		std::shared_ptr<Ui::Show> show,
		not_null<Main::Account*> account,
		bool current) {
	ShowAccountProfileBox(std::move(show), account, current, nullptr);
}

// Spares of an account that is no longer in the app: one tap brings it back
// from one of them.
void AddRestoreSection(
		not_null<Ui::GenericBox*> box,
		const QString &userId,
		Fn<void()> changed) {
	namespace Sessions = Core::GgtgSessions;
	auto spares = std::vector<Sessions::StoredSession>();
	auto fallback = std::vector<Sessions::StoredSession>();
	for (const auto &entry : Sessions::StoredForUser(
			Sessions::LoadVault(),
			userId)) {
		if (Sessions::HasKey(entry) && QDir(Sessions::VaultFolder(entry) + u"/tdata"_q).exists()) {
			if (entry.state != u"revoked"_q) {
				spares.push_back(entry);
			} else {
				fallback.push_back(entry);
			}
		}
	}
	if (spares.empty()) {
		spares = std::move(fallback);
	}
	const auto wrap = box->addRow(object_ptr<Ui::VerticalLayout>(box));
	Ui::AddSkip(wrap);
	Ui::AddSubsectionTitle(wrap, rpl::single(u"Резервные сессии"_q));
	if (spares.empty()) {
		wrap->add(
			object_ptr<Ui::FlatLabel>(
				wrap,
				u"Для этого аккаунта нет резервной сессии - восстановить "
					"его нельзя, только войти заново по номеру. Чтобы так не "
					"бывало, создавайте резервные сессии заранее: «Массовое "
					"действие... → Создать резервную сессию»."_q,
				st::boxDividerLabel),
			st::boxRowPadding);
		return;
	}
	for (const auto &entry : spares) {
		wrap->add(
			object_ptr<Ui::FlatLabel>(
				wrap,
				u"Резервная от "_q + StampText(entry.createdAt)
					+ (entry.note.isEmpty()
						? QString()
						: (u" · "_q + entry.note)),
				st::boxLabel),
			st::boxRowPadding);
		const auto id = entry.id;
		wrap->add(
			object_ptr<Ui::LinkButton>(
				wrap,
				u"Восстановить аккаунт из этой сессии"_q),
			style::margins(
				st::boxRowPadding.left(),
				0,
				st::boxRowPadding.right(),
				st::defaultVerticalListSkip)
		)->setClickedCallback([=] {
			const auto show = box->uiShow();
			if (!Core::GuardAllows(show, Core::GuardedAction::ChangeSessions)) {
				return;
			}
			RestoreFromStoredSession(id, [=](QString error) {
				if (!error.isEmpty()) {
					Ggtg::Notice(show, u"Не удалось восстановить: "_q + error);
					return;
				}
				Ggtg::Notice(show, u"Аккаунт восстановлен"_q);
				box->closeBox();
				if (changed) {
					changed();
				}
			});
		});
	}
}

// `changed` is called after the entry was removed from the list.
void ShowLostAccountInfoBox(
		std::shared_ptr<Ui::Show> show,
		Core::GgtgSessions::LostSession entry,
		Fn<void()> changed) {
	namespace Sessions = Core::GgtgSessions;
	auto lines = InfoLines();
	lines.emplace_back(u"Статус"_q, LostAccountStatus(entry).text);
	lines.emplace_back(u"Когда"_q, StampText(entry.at));
	lines.emplace_back(u"Что случилось"_q, Sessions::ReasonTitle(entry.reason));
	lines.emplace_back(
		u"Код Telegram"_q,
		OrDash(entry.reason) + (entry.code
			? (u" ("_q + QString::number(entry.code) + ')')
			: QString()));
	lines.emplace_back(u"Имя"_q, OrDash(entry.name), true);
	lines.emplace_back(
		u"Username"_q,
		entry.username.isEmpty() ? u"—"_q : (u"@"_q + entry.username),
		true);
	lines.emplace_back(u"Телефон"_q, OrDash(WithPlus(entry.phone)), true);
	lines.emplace_back(u"ID"_q, OrDash(entry.userId), true);
	lines.emplace_back(u"Папка"_q, OrDash(entry.folder));
	lines.emplace_back(u"Прокси"_q, OrDash(entry.proxy), true);
	lines.emplace_back(
		u"Пароль 2FA"_q,
		entry.twoFactor.isEmpty() ? u"не был сохранён"_q : entry.twoFactor,
		!entry.twoFactor.isEmpty());
	lines.emplace_back(
		u"Живых аккаунтов в тот момент"_q,
		QString::number(entry.liveAccounts));
	const auto hint = Sessions::ReasonHint(entry.reason)
		+ u"\n\nЧтобы вернуть аккаунт, войдите в него заново по этому "
			"номеру."_q;
	const auto userId = entry.userId;
	ShowInfoBox(
		show,
		u"Аккаунт: сессия разорвана"_q,
		std::move(lines),
		[=](not_null<Ui::GenericBox*> box) {
			AddRestoreSection(box, userId, changed);
		},
		hint,
		[=](not_null<Ui::GenericBox*> box) {
			box->addButton(rpl::single(u"Убрать из списка"_q), [=] {
				box->uiShow()->show(Ui::MakeConfirmBox({
					.text = u"Убрать этот аккаунт из списка? Запись о разрыве "
						"сессии будет удалена."_q,
					.confirmed = [=](Fn<void()> &&close) {
						Sessions::RemoveUser(userId);
						close();
						box->closeBox();
						if (changed) {
							changed();
						}
					},
					.confirmText = u"Убрать"_q,
				}));
			});
		});
}

// Accounts the app had to log out because their session was ended and that
// haven't been logged in again since - newest first.
[[nodiscard]] std::vector<Core::GgtgSessions::LostSession> CurrentLostAccounts() {
	namespace Sessions = Core::GgtgSessions;
	auto live = base::flat_set<QString>();
	for (const auto &account : Core::App().domain().orderedAccounts()) {
		if (account->sessionExists()) {
			live.emplace(QString::number(account->session().userId().bare));
		}
	}
	auto result = Sessions::LatestPerUser(Sessions::Load());
	result.erase(
		std::remove_if(result.begin(), result.end(), [&](
				const Sessions::LostSession &entry) {
			return live.contains(entry.userId);
		}),
		result.end());
	return result;
}

[[nodiscard]] bool LostMatchesQuery(
		const Core::GgtgSessions::LostSession &entry,
		const QString &query) {
	if (query.isEmpty()) {
		return true;
	}
	auto needle = query.trimmed().toLower();
	if (needle.startsWith('@')) {
		needle = needle.mid(1);
	}
	const auto haystack = (entry.folder + ' ' + entry.name + ' '
		+ entry.username + ' ' + entry.phone + ' ' + entry.userId).toLower();
	return haystack.contains(needle);
}

struct AccountManagerState {
	// nullopt = every account ("ALL"), an empty string = "Без папки".
	std::optional<QString> folder;
	QString query;
	base::flat_set<not_null<Main::Account*>> selected;
	rpl::variable<int> selectedCount = 0;
	// The rows currently shown (after folder/search filtering) and their
	// widgets, for "select all" in the pinned table header.
	std::vector<not_null<Main::Account*>> shown;
	std::vector<not_null<AccountTableRow*>> rows;
	AccountTableHeader *header = nullptr;
	// The account last ticked by hand: the start of a Shift+click range.
	Main::Account *anchor = nullptr;
	// 0 = the app's own order, 1 = free first ... banned last, 2 = reverse.
	int statusSort = 0;
	// 0 = off, 1 = most SMS sent first, 2 = fewest first (one of the two
	// sorts is on at a time).
	int smsSort = 0;
	// Showing «Корзина» instead of the folder filter.
	bool trash = false;
	// The search re-filters a moment after the typing stops, never from
	// inside the field's own change notification.
	base::Timer searchTimer;
	// Statuses change by themselves (connecting, SpamBot answers, freezes):
	// the rows repaint and the summary recounts on this timer.
	base::Timer refreshTimer;
	Fn<void()> rebuildCards;
	Fn<void()> rebuildTable;
	// Re-sizes the header and the rows to the columns as they are now.
	Fn<void()> relayout;
};

[[nodiscard]] bool AccountMatchesQuery(
		not_null<Main::Account*> account,
		const QString &query) {
	if (query.isEmpty()) {
		return true;
	}
	auto needle = query.trimmed().toLower();
	if (needle.startsWith('@')) {
		needle = needle.mid(1);
	}
	auto haystack = account->folderTag().toLower();
	if (account->sessionExists()) {
		const auto user = account->session().user();
		haystack += ' ' + user->name().toLower()
			+ ' ' + user->username().toLower()
			+ ' ' + user->phone()
			+ ' ' + QString::number(user->id.value);
		// Status words, so "спам" or "свободен" filters the list.
		const auto status = Data::FindAccountSpamStatus(user->id.value);
		if (account->trashed()) {
			haystack += u" корзина"_q;
		} else if (account->alwaysOffline()) {
			haystack += u" офлайн sleep"_q;
		} else if (!status) {
			haystack += u" не проверен"_q;
		} else if (status->state == Data::AccountSpamState::Permanent
			|| (status->state == Data::AccountSpamState::Temporary
				&& status->until > QDateTime::currentSecsSinceEpoch())) {
			haystack += u" спам-блок спам блок"_q;
		} else {
			haystack += u" свободен"_q;
		}
	}
	if (MTP::HasRealProxy(account->proxyOverride())) {
		const auto &p = account->proxyOverride();
		haystack += ' ' + p.host + ' ' + QString::number(p.port);
	}
	return haystack.contains(needle);
}

} // namespace

std::optional<AccountTroubleLabel> AccountTrouble(
		not_null<Main::Account*> account) {
	// Read-only: this is asked by every row of the account list.
	if (const auto trouble = KnownAccountTrouble(account, false)) {
		return AccountTroubleLabel{ trouble->text, trouble->color };
	}
	return std::nullopt;
}

struct SavedManagerFilter {
	std::optional<QString> folder;
	QString query;
	bool trash = false;
	int statusSort = 0;
	int smsSort = 0;
};
inline SavedManagerFilter &GlobalSavedManagerFilter() {
	static auto instance = SavedManagerFilter();
	return instance;
}

object_ptr<Ui::BoxContent> MakeAccountManagerBox(
		not_null<Window::SessionController*> controller) {
	return Box([=](not_null<Ui::GenericBox*> box) {
		box->setWidth(st::accountsBoxWidth);
		box->setTitle(rpl::single(u"Менеджер аккаунтов"_q));

		const auto state = box->lifetime().make_state<AccountManagerState>();
		state->folder = GlobalSavedManagerFilter().folder;
		state->query = GlobalSavedManagerFilter().query;
		state->trash = GlobalSavedManagerFilter().trash;
		state->statusSort = GlobalSavedManagerFilter().statusSort;
		state->smsSort = GlobalSavedManagerFilter().smsSort;
		const auto show = [=] { return box->uiShow(); };

		// Everything above the rows (folder cards, search, selection bar and
		// the table header) is pinned: only the account rows scroll, so the
		// controls never scroll out of reach on a long list. GenericBox lays
		// the pinned part out once at its own declared width - keep it
		// following the real width as the window is resized.
		const auto pinned = box->setPinnedToTopContent(
			object_ptr<Ui::VerticalLayout>(box));
		box->widthValue(
		) | rpl::filter([](int width) {
			return width > 0;
		}) | rpl::on_next([=](int width) {
			pinned->resizeToWidth(width);
		}, pinned->lifetime());

		// The summary and the folder cards are the tall part of the top: they
		// fold away completely when the list is scrolled down, leaving only
		// search and the action button.
		const auto fullWrap = pinned->add(
			object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
				pinned,
				object_ptr<Ui::VerticalLayout>(pinned)));
		fullWrap->setDuration(140);
		const auto fullInner = fullWrap->entity();
		// One line of health: how many accounts, how many are free, how
		// many are stuck in a spam block.
		const auto summary = fullInner->add(
			object_ptr<Ui::FlatLabel>(fullInner, QString(), st::boxLabel),
			QMargins(st::boxRowPadding.left(), style::ConvertScale(2), st::boxRowPadding.right(), style::ConvertScale(2)));
		const auto refreshSummary = [=] {
			auto live = 0;
			auto blocked = 0;
			auto unchecked = 0;
			const auto now = QDateTime::currentSecsSinceEpoch();
			for (const auto &account
					: Core::App().domain().orderedAccounts()) {
				// «Корзина» is out of the count: its accounts sleep and
				// their blocks are nobody's concern.
				if (!Main::AccountIsLive(account) || account->trashed()) {
					continue;
				}
				++live;
				const auto status = Data::FindAccountSpamStatus(
					account->session().user()->id.value);
				if (!status) {
					++unchecked;
				} else if (status->state == Data::AccountSpamState::Permanent
					|| (status->state == Data::AccountSpamState::Temporary
						&& status->until > now)) {
					++blocked;
				}
			}
			summary->setText(u"Всего: "_q + QString::number(live)
				+ u" · свободны: "_q
				+ QString::number(live - blocked - unchecked)
				+ u" · спам-блок: "_q + QString::number(blocked)
				+ u" · не проверены: "_q + QString::number(unchecked));
		};
		refreshSummary();
		Data::AccountSpamStatusChanges(
		) | rpl::on_next(refreshSummary, summary->lifetime());
		auto cardsWrapOwned = object_ptr<Ui::RpWidget>(fullInner);
		const auto cardsWrap = cardsWrapOwned.data();
		const auto cardsScroll = Ui::CreateChild<Ui::ScrollArea>(
			cardsWrap, st::accountsHeaderScroll);
		cardsScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
		cardsScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
		cardsWrap->widthValue(
		) | rpl::on_next([=](int width) {
			cardsScroll->setGeometry(0, 0, width, cardsWrap->height());
		}, cardsWrap->lifetime());
		fullInner->add(
			std::move(cardsWrapOwned),
			QMargins(st::boxRowPadding.left(), style::ConvertScale(2), st::boxRowPadding.right(), style::ConvertScale(4)));

		const auto lastScrollTop = std::make_shared<int>(0);
		const auto accumulatedUpScroll = std::make_shared<int>(0);
		const auto fold = [=](bool value) {
			if (fullWrap->toggled() == !value) {
				return;
			}
			fullWrap->toggle(!value, anim::type::normal);
		};

		// Search (with a cross clearing it) + selection bar.
		const auto searchWrap = pinned->add(
			object_ptr<Ui::RpWidget>(pinned),
			st::boxRowPadding);
		const auto search = Ui::CreateChild<Ui::InputField>(
			searchWrap,
			st::defaultInputField,
			rpl::single(u"Поиск: имя, @username, телефон, ID, папка, «спам», «свободен»"_q));
		if (!state->query.isEmpty()) {
			search->setText(state->query);
		}
		const auto clear = Ui::CreateChild<Ui::CrossButton>(
			searchWrap,
			st::defaultMultiSelectSearchCancel);
		clear->setClickedCallback([=] {
			search->setText(QString());
			state->query = QString();
			GlobalSavedManagerFilter().query = QString();
			state->rebuildTable();
			search->setFocus();
		});
		clear->toggle(!state->query.isEmpty(), anim::type::instant);
		searchWrap->resize(searchWrap->width(), search->height());
		searchWrap->widthValue(
		) | rpl::on_next([=](int width) {
			search->resizeToWidth(width);
			search->moveToLeft(0, 0);
			clear->moveToRight(
				0,
				(search->height() - clear->height()) / 2);
		}, searchWrap->lifetime());
		state->searchTimer.setCallback([=] {
			const auto query = search->getLastText();
			if (state->query != query) {
				state->query = query;
				GlobalSavedManagerFilter().query = query;
				state->rebuildTable();
			}
		});
		search->changes(
		) | rpl::on_next([=] {
			clear->toggle(
				!search->getLastText().isEmpty(),
				anim::type::normal);
			// Rebuilding the table right inside the field's own change
			// notification (e.g. select all + Backspace) tore widgets down
			// under it: filter a moment later instead.
			state->searchTimer.callOnce(150);
		}, search->lifetime());

		const auto bar = pinned->add(
			object_ptr<SelectionBarRow>(pinned),
			st::boxRowPadding);
		const auto makeButton = [&](
				const QString &text,
				Fn<void()> callback,
				const style::RoundButton &st = st::defaultBoxButton) {
			const auto button = Ui::CreateChild<Ui::RoundButton>(
				bar,
				rpl::single(text),
				st);
			button->setTextTransform(Ui::RoundButtonTextTransform::NoTransform);
			button->setClickedCallback(std::move(callback));
			bar->addButton(button);
			return button;
		};
		const auto selectedAccounts = [=] {
			return Main::LiveAccounts(state->selected);
		};
		// Bulk actions on the ticked accounts (photo, name, @username, bio,
		// 2FA, move to a folder, end other sessions). While one is running
		// the button opens its live progress instead.
		const auto bulkButton = makeButton(u"Массовое действие..."_q, [=] {
			if (CurrentBulkRun()) {
				OpenBulkActions(controller);
				return;
			}
			auto chosen = selectedAccounts();
			if (chosen.empty()) {
				Ggtg::Notice(show(), u"Отметьте хотя бы один аккаунт"_q);
				return;
			}
			// Straight into this window's own layer: going through the
			// main window's redirect could land it behind this window.
			show()->showBox(
				MakeBulkActionsChooserBox(controller, std::move(chosen)));
		}, st::defaultActiveButton);
		// Inside «Корзина»: the ticked accounts back to the list, or gone
		// for good.
		const auto restoreButton = makeButton(u"Восстановить"_q, [=] {
			auto chosen = selectedAccounts();
			if (chosen.empty()) {
				Ggtg::Notice(show(), u"Отметьте хотя бы один аккаунт"_q);
				return;
			}
			RestoreAccountsFromTrash(chosen);
			Ggtg::Notice(show(), u"Восстановлено: "_q
				+ QString::number(chosen.size()));
		});
		const auto deleteButton = makeButton(u"Удалить навсегда"_q, [=] {
			auto chosen = selectedAccounts();
			if (chosen.empty()) {
				Ggtg::Notice(show(), u"Отметьте хотя бы один аккаунт"_q);
				return;
			}
			ConfirmDeleteAccountsForever(show(), chosen);
		});

		const auto folderMenu = box->lifetime().make_state<base::unique_qptr<Ui::PopupMenu>>();
		const auto folderContextMenu = box->lifetime().make_state<base::unique_qptr<Ui::PopupMenu>>();
		const auto accountContextMenu = box->lifetime().make_state<base::unique_qptr<Ui::PopupMenu>>();

		const auto updateBarButtons = [=] {
			bulkButton->setVisible(!state->trash);
			restoreButton->setVisible(state->trash);
			deleteButton->setVisible(state->trash);
			if (bar->width() > 0) {
				bar->resizeToWidth(bar->width());
			}
		};
		updateBarButtons();

		const auto showFolderContextMenu = [=](QString tag, QPoint pos) {
			*folderContextMenu = base::make_unique_q<Ui::PopupMenu>(box, st::folderContextMenu);
			const auto menu = folderContextMenu->get();
			if (tag.isEmpty()) {
				menu->addAction(u"Все в онлайн"_q, [=] {
					for (const auto &acc : Core::App().domain().orderedAccounts()) {
						if (!tag.isEmpty() && acc->folderTag() != tag) continue;
						acc->setAlwaysOffline(false);
					}
					Ggtg::Notice(show(), u"Все аккаунты переведены в онлайн"_q);
					state->rebuildCards();
					state->rebuildTable();
				}, &st::menuIconShowInChat);
				menu->addAction(u"Все в спящий режим"_q, [=] {
					for (const auto &acc : Core::App().domain().orderedAccounts()) {
						if (!tag.isEmpty() && acc->folderTag() != tag) continue;
						acc->setAlwaysOffline(true);
					}
					Ggtg::Notice(show(), u"Все аккаунты переведены в спящий режим"_q);
					state->rebuildCards();
					state->rebuildTable();
				}, &st::menuIconMute);
				menu->addAction(u"Проверить SpamBot"_q, [=] {
					auto list = std::vector<not_null<Main::Account*>>();
					for (const auto &acc : Core::App().domain().orderedAccounts()) {
						if (Main::AccountIsLive(acc) && !acc->trashed()) {
							list.push_back(acc);
						}
					}
					if (!list.empty()) {
						RunBulkAction(controller, std::move(list), [=](
								not_null<Main::Account*> account,
								StepDone done) {
							StepCheckSpam(account, done);
						}, BulkOptions{
							.title = u"Проверка SpamBot"_q,
							.failPrefix = QString(),
						});
					}
				}, &st::menuIconFactcheck);
			} else {
				menu->addAction(u"Переименовать..."_q, [=] {
					ShowAccountFolderNamePrompt(
						show(),
						u"Переименовать папку"_q,
						u"Все аккаунты этой папки перейдут в папку с новым именем."_q,
						tag,
						[=](QString folder) {
							if (folder.isEmpty() || folder == tag) return;
							for (const auto &account : Core::App().domain().orderedAccounts()) {
								if (account->folderTag() == tag) {
									account->setFolderTag(folder);
								}
							}
							if (state->folder && *state->folder == tag) {
								state->folder = folder;
							}
							state->rebuildCards();
							state->rebuildTable();
						});
				}, &st::menuIconEdit);
				menu->addAction(u"В онлайн"_q, [=] {
					auto count = 0;
					for (const auto &acc : Core::App().domain().orderedAccounts()) {
						if (acc->folderTag() == tag) {
							acc->setAlwaysOffline(false);
							++count;
						}
					}
					Ggtg::Notice(show(), u"В онлайне: %1 акк."_q.arg(count));
					state->rebuildCards();
					state->rebuildTable();
				}, &st::menuIconShowInChat);
				menu->addAction(u"В спящий режим"_q, [=] {
					auto count = 0;
					for (const auto &acc : Core::App().domain().orderedAccounts()) {
						if (acc->folderTag() == tag) {
							acc->setAlwaysOffline(true);
							++count;
						}
					}
					Ggtg::Notice(show(), u"В спящем режиме: %1 акк."_q.arg(count));
					state->rebuildCards();
					state->rebuildTable();
				}, &st::menuIconMute);
				menu->addAction(u"Назначить прокси..."_q, [=] {
					auto list = std::vector<not_null<Main::Account*>>();
					for (const auto &acc : Core::App().domain().orderedAccounts()) {
						if (acc->folderTag() == tag && Main::AccountIsLive(acc)) {
							list.push_back(acc);
						}
					}
					if (list.empty()) {
						Ggtg::Notice(show(), u"Нет подключённых аккаунтов в этой папке"_q);
						return;
					}
					ShowBulkProxyBox(controller, std::move(list));
				}, &st::menuIconSettings);
				menu->addAction(u"Проверить SpamBot"_q, [=] {
					auto list = std::vector<not_null<Main::Account*>>();
					for (const auto &acc : Core::App().domain().orderedAccounts()) {
						if (acc->folderTag() == tag && Main::AccountIsLive(acc)) {
							list.push_back(acc);
						}
					}
					if (list.empty()) {
						Ggtg::Notice(show(), u"Нет подключённых аккаунтов в этой папке"_q);
						return;
					}
					RunBulkAction(controller, std::move(list), [=](
							not_null<Main::Account*> account,
							StepDone done) {
						StepCheckSpam(account, done);
					}, BulkOptions{
						.title = u"Проверка SpamBot"_q,
						.failPrefix = QString(),
					});
				}, &st::menuIconFactcheck);
				menu->addAction(u"Убрать из папки"_q, [=] {
					for (const auto &acc : Core::App().domain().orderedAccounts()) {
						if (acc->folderTag() == tag) {
							acc->setFolderTag(QString());
						}
					}
					if (state->folder && *state->folder == tag) {
						state->folder = std::nullopt;
					}
					state->rebuildCards();
					state->rebuildTable();
				}, &st::menuIconRemove);
				menu->addAction(u"В корзину"_q, [=] {
					auto list = std::vector<not_null<Main::Account*>>();
					for (const auto &acc : Core::App().domain().orderedAccounts()) {
						if (acc->folderTag() == tag) {
							list.push_back(acc);
						}
					}
					if (!list.empty()) {
						MoveAccountsToTrash(list);
						Ggtg::Notice(show(), u"Перемещено в корзину: %1"_q.arg(list.size()));
						state->rebuildCards();
						state->rebuildTable();
					}
				}, &st::menuIconDeleteAttention);
			}
			menu->popup(pos);
		};

		const auto showFolderPicker = [=](not_null<Main::Account*> account) {
			*folderMenu = base::make_unique_q<Ui::PopupMenu>(box, st::folderContextMenu);
			auto folders = std::vector<QString>();
			for (const auto &acc : Core::App().domain().orderedAccounts()) {
				const auto tag = acc->folderTag();
				if (!tag.isEmpty() && !ranges::contains(folders, tag)) {
					folders.push_back(tag);
				}
			}
			for (const auto &folder : folders) {
				const auto isCurrent = (account->folderTag() == folder);
				const auto label = (isCurrent ? u"✓ "_q : u"   "_q) + folder;
				(*folderMenu)->addAction(label, [=] {
					account->setFolderTag(folder);
					state->rebuildCards();
					state->rebuildTable();
				});
			}
			(*folderMenu)->addAction(u"Без папки"_q, [=] {
				account->setFolderTag(QString());
				state->rebuildCards();
				state->rebuildTable();
			});
			(*folderMenu)->addAction(u"Новая папка..."_q, [=] {
				ShowBulkMoveToFolderBox(controller, { account });
			});
			(*folderMenu)->popup(QCursor::pos());
		};

		const auto showAccountContextMenu = [=](not_null<Main::Account*> account, QPoint pos) {
			*accountContextMenu = base::make_unique_q<Ui::PopupMenu>(box, st::folderContextMenu);
			const auto menu = accountContextMenu->get();
			const auto currentAccount = &controller->session().account();
			const auto isCurrent = (account.get() == currentAccount);
			const auto isLive = Main::AccountIsLive(account);
			const auto inTrash = account->trashed();

			if (!inTrash) {
				if (!isCurrent) {
					menu->addAction(u"Открыть аккаунт"_q, [=] {
						Ui::PostponeCall(box, [=] {
							const auto all = Core::App().domain().orderedAccounts();
							if (ranges::find(all, account) != end(all)) {
								Core::App().domain().maybeActivate(account);
							}
						});
					});
				} else {
					menu->addAction(u"✓ Активен сейчас"_q, [] {})->setEnabled(false);
				}
			}

			menu->addAction(u"Информация об аккаунте"_q, [=] {
				ShowAccountInfoBox(show(), account, isCurrent);
			});
			if (isLive && !inTrash) {
				menu->addAction(u"Редактировать профиль..."_q, [=] {
					ShowEditAccountBox(show(), account);
				});
			}

			const auto currentFolder = account->folderTag();
			menu->addAction(
				currentFolder.isEmpty() ? u"Назначить папку..."_q : (u"Папка: «%1»..."_q.arg(currentFolder)),
				[=] { showFolderPicker(account); });
			if (!currentFolder.isEmpty()) {
				menu->addAction(u"Убрать из папки"_q, [=] {
					account->setFolderTag(QString());
					state->rebuildCards();
					state->rebuildTable();
				});
			}

			const auto proxy = account->proxyOverride();
			menu->addAction(
				!MTP::HasRealProxy(proxy) ? u"Настроить прокси..."_q : (u"Прокси: %1:%2..."_q.arg(proxy.host).arg(proxy.port)),
				[=] {
					if (Core::GuardAllows(show(), Core::GuardedAction::ReplaceProxy)) {
						show()->showBox(MakeAccountProxyBox(account));
					}
				});
			if (MTP::HasRealProxy(proxy)) {
				menu->addAction(u"Скопировать прокси"_q, [=] {
					QString text;
					if (proxy.user.isEmpty()) {
						text = QString("%1:%2").arg(proxy.host).arg(proxy.port);
					} else {
						text = QString("%1:%2:%3:%4").arg(proxy.host).arg(proxy.port).arg(proxy.user).arg(proxy.password);
					}
					QGuiApplication::clipboard()->setText(text);
					Ggtg::Notice(show(), u"Прокси скопирован в буфер"_q);
				});
				menu->addAction(u"Отключить прокси"_q, [=] {
					account->setProxyOverride(MTP::ProxyData());
					Ggtg::Notice(show(), u"Прокси отключён"_q);
					state->rebuildTable();
				});
			}

			if (isLive && !inTrash) {
				const auto offline = account->alwaysOffline();
				menu->addAction(
					offline ? u"Пробудить (В онлайн)"_q : u"В спящий режим"_q,
					[=] {
						account->setAlwaysOffline(!offline);
						Ggtg::Notice(show(), !offline ? u"Аккаунт переведён в спящий режим"_q : u"Аккаунт пробуждён"_q);
						state->rebuildTable();
					});
				menu->addAction(u"Проверить SpamBot"_q, [=] {
					if (CurrentBulkRun()) {
						OpenBulkActions(controller);
						return;
					}
					RunBulkAction(controller, { account }, [=](
							not_null<Main::Account*> account,
							StepDone done) {
						StepCheckSpam(account, done);
					}, BulkOptions{
						.title = u"Проверка SpamBot"_q,
						.failPrefix = QString(),
					});
				});
			}

			if (!inTrash) {
				menu->addAction(u"Массовые действия..."_q, [=] {
					if (CurrentBulkRun()) {
						OpenBulkActions(controller);
						return;
					}
					show()->showBox(MakeBulkActionsChooserBox(controller, { account }));
				});
			}

			if (inTrash) {
				menu->addAction(u"Восстановить из корзины"_q, [=] {
					RestoreAccountsFromTrash({ account });
					Ggtg::Notice(show(), u"Аккаунт восстановлен"_q);
					state->rebuildCards();
					state->rebuildTable();
				});
				menu->addAction(u"Удалить навсегда"_q, [=] {
					ConfirmDeleteAccountsForever(show(), { account });
				});
			} else {
				menu->addAction(u"Переместить в корзину"_q, [=] {
					MoveAccountsToTrash({ account });
					Ggtg::Notice(show(), u"Аккаунт перемещён в корзину"_q);
					state->rebuildCards();
					state->rebuildTable();
				});
			}

			menu->popup(pos);
		};

		// Shared by the toolbar's "Управление папкой" button (acting on the
		// active folder filter) and a right-click on a folder card.
		const auto showFolderManagement = [=](QString tag) {
			showFolderContextMenu(tag, QCursor::pos());
		};

		// The columns fit the window (see ComputeManagerColumns), and only
		// scroll sideways once even the narrowest layout doesn't fit - both
		// the header and the rows below sit at that same width inside their own horizontal-only Ui::ScrollArea, and the two
		// scrollbars are wired together so scrolling one scrolls the other,
		// keeping the header lined up with its columns. The table header
		// (with "select all") itself still stays pinned, i.e. it never
		// scrolls *vertically* even as the rows below do.
		const auto tableWidth = ComputeManagerColumns().totalWidth;
		const auto headerHeight = style::ConvertScale(34);
		auto headerWrapOwned = object_ptr<Ui::RpWidget>(pinned);
		const auto headerWrap = headerWrapOwned.data();
		const auto headerScroll = Ui::CreateChild<Ui::ScrollArea>(
			headerWrap, st::accountsHeaderScroll);
		// The table always fits the window (see ComputeManagerColumns), so
		// there is nothing to scroll sideways.
		headerScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
		state->header = headerScroll->setOwnedWidget(
			object_ptr<AccountTableHeader>(
				headerScroll,
				[=](bool checked) {
					for (const auto &account : state->shown) {
						if (checked) {
							state->selected.emplace(account);
						} else {
							state->selected.remove(account);
						}
					}
					for (const auto &row : state->rows) {
						row->setSelected(checked);
					}
					state->selectedCount = int(state->selected.size());
				},
				[=] {
					// none -> free first -> problems first -> none
					state->statusSort = (state->statusSort + 1) % 3;
					state->smsSort = 0;
					GlobalSavedManagerFilter().statusSort = state->statusSort;
					GlobalSavedManagerFilter().smsSort = 0;
					state->header->setSort(state->statusSort);
					state->header->setSmsSort(0);
					state->rebuildTable();
				},
				[=] {
					// none -> most first -> fewest first -> none
					state->smsSort = (state->smsSort + 1) % 3;
					state->statusSort = 0;
					GlobalSavedManagerFilter().smsSort = state->smsSort;
					GlobalSavedManagerFilter().statusSort = 0;
					state->header->setSmsSort(state->smsSort);
					state->header->setSort(0);
					state->rebuildTable();
				})).data();
		state->header->setSort(state->statusSort);
		state->header->setSmsSort(state->smsSort);
		state->header->setTrashMode(state->trash);
		state->selectedCount.value(
		) | rpl::on_next([=](int count) {
			state->header->setSelectedCount(count);
		}, headerWrap->lifetime());
		headerWrap->widthValue(
		) | rpl::on_next([=](int width) {
			headerScroll->setGeometry(0, 0, width, headerHeight);
		}, headerWrap->lifetime());
		headerWrap->resize(headerWrap->width(), headerHeight);
		pinned->add(std::move(headerWrapOwned));

		// The rows themselves: same fixed width as the header above, only
		// scrolling vertically through the box's own outer scroll area (see
		// AddScrollAreaRow's comment on why a plain VerticalLayout can't go
		// through box->addRow directly here either).
		auto rowsWrapOwned = object_ptr<Ui::RpWidget>(box);
		const auto rowsWrap = rowsWrapOwned.data();
		const auto rowsScroll = Ui::CreateChild<Ui::ScrollArea>(
			rowsWrap, st::previewListScroll);
		rowsScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
		const auto table = rowsScroll->setOwnedWidget(
			object_ptr<Ui::VerticalLayout>(rowsScroll)).data();
		table->resizeToWidth(tableWidth);
		// table's height (many/few rows) decides rowsWrap's own height, so
		// the box's outer VerticalLayout reserves exactly that much space
		// for it - same pattern as ResizeFitChild in rp_widget.cpp, just
		// inlined since the "child" that drives the height here is one step
		// removed (inside the scroll area) from the widget being resized.
		table->heightValue(
		) | rpl::on_next([=](int height) {
			rowsWrap->resize(rowsWrap->width(), height);
			rowsScroll->setGeometry(0, 0, rowsWrap->width(), height);
		}, rowsWrap->lifetime());
		state->relayout = [=] {
			const auto columns = ComputeManagerColumns();
			const auto total = columns.totalWidth;
			if (state->header) {
				state->header->resize(total, state->header->height());
				state->header->update();
			}
			// Rows are resized before the layout re-flows around them.
			for (const auto &row : state->rows) {
				row->resize(total, columns.rowHeight);
				row->update();
			}
			table->resizeToWidth(total);
		};
		rowsWrap->widthValue(
		) | rpl::on_next([=](int width) {
			rowsScroll->setGeometry(0, 0, width, rowsWrap->height());
			if (width <= 0) {
				return;
			}
			// The columns follow the window: see ComputeManagerColumns.
			ManagerViewportWidth() = width;
			state->relayout();
		}, rowsWrap->lifetime());
		box->lifetime().add([] { ManagerViewportWidth() = 0; });
		box->addRow(std::move(rowsWrapOwned), style::margins());
		Ui::AddSkip(box->verticalLayout());

		// Scrolled down the list: fold the summary and the folder cards.
		// Folded when scrolling down past top; unfolds when scrolling back up.
		box->scrolls(
		) | rpl::on_next([=] {
			const auto top = box->scrollTop();
			const auto prev = *lastScrollTop;
			*lastScrollTop = top;

			if (top <= 12) {
				*accumulatedUpScroll = 0;
				fold(false);
			} else if (top > prev) {
				// Scrolling DOWN: fold to free vertical space for accounts table
				*accumulatedUpScroll = 0;
				if (top > 25) {
					fold(true);
				}
			} else if (top < prev) {
				// Scrolling UP: unfold smoothly
				*accumulatedUpScroll += (prev - top);
				if (*accumulatedUpScroll >= 30 || top <= 15) {
					*accumulatedUpScroll = 0;
					fold(false);
				}
			}
		}, box->lifetime());

		const auto inScrollSync = std::make_shared<bool>(false);
		QObject::connect(
			headerScroll->horizontalScrollBar(),
			&QScrollBar::valueChanged,
			[=](int value) {
				if (*inScrollSync) return;
				*inScrollSync = true;
				rowsScroll->horizontalScrollBar()->setValue(value);
				*inScrollSync = false;
			});
		QObject::connect(
			rowsScroll->horizontalScrollBar(),
			&QScrollBar::valueChanged,
			[=](int value) {
				if (*inScrollSync) return;
				*inScrollSync = true;
				headerScroll->horizontalScrollBar()->setValue(value);
				*inScrollSync = false;
			});

		const auto handleWheel = [=](not_null<QEvent*> e) {
			if (e->type() == QEvent::Wheel) {
				const auto wheel = static_cast<QWheelEvent*>(e.get());
				auto deltaX = wheel->angleDelta().x();
				if (deltaX == 0 && (wheel->modifiers() & Qt::ShiftModifier)) {
					deltaX = wheel->angleDelta().y();
				}
				if (deltaX != 0) {
					const auto bar = rowsScroll->horizontalScrollBar();
					if (bar && bar->maximum() > 0) {
						bar->setValue(bar->value() - deltaX);
						return base::EventFilterResult::Cancel;
					}
				}
			}
			return base::EventFilterResult::Continue;
		};
		base::install_event_filter(rowsScroll, handleWheel);
		base::install_event_filter(table, handleWheel);

		state->rebuildCards = [=] {
			const Ggtg::ScrollKeeper keepScroll(cardsScroll);
			auto counts = base::flat_map<QString, int>();
			auto total = 0;
			auto trashed = 0;
			for (const auto &account : Core::App().domain().orderedAccounts()) {
				if (account->trashed()) {
					++trashed;
					continue;
				}
				++counts[account->folderTag()];
				++total;
			}
			for (const auto &lost : CurrentLostAccounts()) {
				++counts[lost.folder];
				++total;
			}
			if (state->folder && !counts.contains(*state->folder)) {
				state->folder = std::nullopt;
			}
			// Fill the row *before* handing it to the layout: the layout
			// sizes it (and so positions its cards) the moment it's added,
			// and a card added afterwards would never get placed.
			auto rowOwned = object_ptr<FolderCardsRow>(cardsScroll, cardsScroll);
			if (state->trash && !trashed) {
				// The trash was emptied: back to the full list.
				state->trash = false;
				state->folder = std::nullopt;
				state->selected.clear();
				state->anchor = nullptr;
				state->header->setTrashMode(false);
				updateBarButtons();
			}
			const auto open = [=](std::optional<QString> folder, bool trash) {
				return [=] {
					const auto modeChanged = (state->trash != trash);
					state->folder = folder;
					state->trash = trash;
					GlobalSavedManagerFilter().folder = folder;
					GlobalSavedManagerFilter().trash = trash;
					if (modeChanged) {
						// What is ticked in one list means nothing in the
						// other.
						state->selected.clear();
						state->anchor = nullptr;
						state->header->setTrashMode(trash);
						updateBarButtons();
					}
					// Not in place: this runs from inside the very card
					// being rebuilt.
					Ui::PostponeCall(box, [=] {
						state->rebuildCards();
						state->rebuildTable();
					});
				};
			};
			const auto activeAcc = &controller->session().account();
			const auto currentFolder = activeAcc ? activeAcc->folderTag() : QString();

			rowOwned->addCard(
				u"Все"_q,
				total,
				!state->trash && !state->folder.has_value(),
				open(std::nullopt, false),
				[=](QPoint pos) { showFolderContextMenu(QString(), pos); });
			for (const auto &[tag, count] : counts) {
				const auto hasCurrent = (activeAcc && currentFolder == tag);
				rowOwned->addCard(
					tag.isEmpty() ? u"Без папки"_q : tag,
					count,
					(!state->trash && state->folder && *state->folder == tag),
					open(tag, false),
					[=](QPoint pos) { showFolderContextMenu(tag, pos); },
					hasCurrent);
			}
			if (trashed > 0) {
				rowOwned->addCard(
					u"Корзина"_q,
					trashed,
					state->trash,
					open(std::nullopt, true),
					[=](QPoint pos) {
						*folderContextMenu = base::make_unique_q<Ui::PopupMenu>(box, st::folderContextMenu);
						(*folderContextMenu)->addAction(u"Очистить корзину навсегда..."_q, [=] {
							auto trashedAccs = std::vector<not_null<Main::Account*>>();
							for (const auto &acc : Core::App().domain().orderedAccounts()) {
								if (acc->trashed()) trashedAccs.push_back(acc);
							}
							if (!trashedAccs.empty()) {
								ConfirmDeleteAccountsForever(show(), trashedAccs);
							}
						}, &st::menuIconDeleteAttention);
						(*folderContextMenu)->popup(pos);
					});
			}
			rowOwned->layoutCards();
			const auto h = rowOwned->height();
			cardsWrap->resize(cardsWrap->width(), h + style::ConvertScale(4));
			cardsScroll->setGeometry(0, 0, cardsWrap->width(), h + style::ConvertScale(4));
			cardsScroll->setOwnedWidget(std::move(rowOwned));
		};

		state->rebuildTable = [=] {
			const Ggtg::ScrollKeeper keepScroll(table);
			table->clear();
			state->rows.clear();
			state->shown.clear();
			auto everyone = base::flat_set<not_null<Main::Account*>>();
			for (const auto &account : Core::App().domain().orderedAccounts()) {
				everyone.emplace(account);
				if (account->trashed() != state->trash) {
					continue;
				}
				if (state->folder && account->folderTag() != *state->folder) {
					continue;
				}
				if (!AccountMatchesQuery(account, state->query)) {
					continue;
				}
				state->shown.push_back(account);
			}
			// The ticks follow what is on the screen: an account hidden by
			// another folder or the search is not ticked any more, so the
			// count and the bulk action always mean the rows you see.
			const auto visible = base::flat_set<not_null<Main::Account*>>(
				state->shown.begin(),
				state->shown.end());
			for (auto i = state->selected.begin(); i != state->selected.end();) {
				if (!visible.contains(*i)) {
					i = state->selected.erase(i);
				} else {
					++i;
				}
			}
			state->selectedCount = int(state->selected.size());

			auto allSelected = !state->shown.empty();
			for (const auto &account : state->shown) {
				if (!state->selected.contains(account)) {
					allSelected = false;
					break;
				}
			}
			state->header->setAllChecked(allSelected);

			auto lost = state->trash
				? std::vector<Core::GgtgSessions::LostSession>()
				: CurrentLostAccounts();
			lost.erase(
				std::remove_if(lost.begin(), lost.end(), [&](
						const Core::GgtgSessions::LostSession &entry) {
					return (state->folder && entry.folder != *state->folder)
						|| !LostMatchesQuery(entry, state->query);
				}),
				lost.end());
			if (state->shown.empty() && lost.empty()) {
				table->add(
					object_ptr<Ui::FlatLabel>(
						table,
						everyone.empty()
							? u"Нет добавленных аккаунтов"_q
							: (state->trash && state->query.isEmpty())
							? u"Корзина пуста"_q
							: (state->query.isEmpty() && !state->folder)
							? u"Все аккаунты в корзине"_q
							: u"Ничего не найдено"_q,
						st::boxLabel),
					st::boxRowPadding);
				return;
			}
			const auto currentAccount = &controller->session().account();
			MeasureManagerContent(state->shown, lost, currentAccount);
			auto index = 0;
			const auto addLiveRow = [&](not_null<Main::Account*> account) {
				const auto row = table->add(object_ptr<AccountTableRow>(
					table,
					account,
					++index,
					(account.get() == currentAccount),
					state->selected.contains(account),
					[=](bool checked) {
						// Shift + click: everything between the previously
						// clicked account and this one gets the same state.
						if ((QGuiApplication::keyboardModifiers()
								& Qt::ShiftModifier)
							&& state->anchor
							&& state->anchor != account.get()) {
							auto from = -1;
							auto to = -1;
							for (auto i = 0; i != int(state->shown.size()); ++i) {
								if (state->shown[i] == state->anchor) {
									from = i;
								}
								if (state->shown[i] == account) {
									to = i;
								}
							}
							if (from >= 0 && to >= 0) {
								const auto lo = std::min(from, to);
								const auto hi = std::max(from, to);
								for (auto i = lo; i <= hi; ++i) {
									if (checked) {
										state->selected.emplace(state->shown[i]);
									} else {
										state->selected.remove(state->shown[i]);
									}
									if (i < int(state->rows.size())) {
										state->rows[i]->setSelected(checked);
									}
								}
							}
						}
						state->anchor = account.get();
						if (checked) {
							state->selected.emplace(account);
						} else {
							state->selected.remove(account);
						}
						state->selectedCount = int(state->selected.size());
						// Keep "select all" in step with the rows, so its
						// first click after manual ticking does what it
						// shows instead of only "evening out" the state.
						auto all = !state->shown.empty();
						for (const auto &shown : state->shown) {
							if (!state->selected.contains(shown)) {
								all = false;
								break;
							}
						}
						state->header->setAllChecked(all);
					},
					[=] {
						// The GGTG window stays open and follows the newly
						// opened account (see OpenGgtgShell). Not from
						// inside this row's own click: switching rebuilds
						// this very table.
						Ui::PostponeCall(box, [=] {
							const auto all
								= Core::App().domain().orderedAccounts();
							if (ranges::find(all, account) != end(all)) {
								Core::App().domain().maybeActivate(account);
							}
						});
					},
					[=] {
						if (CurrentBulkRun()) {
							OpenBulkActions(controller);
							return;
						}
						if (!Main::AccountIsLive(account)) {
							return;
						}
						RunBulkAction(controller, { account }, [=](
								not_null<Main::Account*> account,
								StepDone done) {
							StepCheckSpam(account, done);
						}, BulkOptions{
							.title = u"Проверка SpamBot"_q,
							.failPrefix = QString(),
						});
					},
					[=] { ShowEditAccountBox(show(), account); },
					[=] {
						if (Core::GuardAllows(
								show(),
								Core::GuardedAction::ReplaceProxy)) {
							show()->showBox(MakeAccountProxyBox(account));
						}
					},
					[=] {
						showFolderPicker(account);
					},
					[=] {
						if (CurrentBulkRun()) {
							OpenBulkActions(controller);
							return;
						} else if (state->trash) {
							return; // «Восстановить» first.
						}
						auto chosen = Main::LiveAccounts({ account });
						if (chosen.empty()) {
							Ggtg::Notice(show(), u"Аккаунт не подключён"_q);
							return;
						}
						show()->showBox(MakeBulkActionsChooserBox(
							controller,
							std::move(chosen)));
					},
					[=] {
						ShowAccountInfoBox(
							show(),
							account,
							account.get() == currentAccount);
					},
					state->trash,
					[=] {
						// Not in place: this rebuilds the row's own table.
						const auto trash = state->trash;
						Ui::PostponeCall(box, [=] {
							if (trash) {
								RestoreAccountsFromTrash({ account });
								Ggtg::Notice(show(), u"Аккаунт восстановлен"_q);
							} else {
								MoveAccountsToTrash({ account });
								Ggtg::Notice(show(), 
									u"Аккаунт перемещён в корзину"_q);
							}
						});
					},
					[=] {
						ConfirmDeleteAccountsForever(show(), { account });
					},
					[=](QPoint pos) {
						showAccountContextMenu(account, pos);
					}));
				state->rows.push_back(row);
			};
			const auto addLostRow = [&](
					const Core::GgtgSessions::LostSession &entry) {
				table->add(object_ptr<LostAccountRow>(
					table,
					entry,
					[=] {
						ShowLostAccountInfoBox(show(), entry, [=] {
							state->rebuildCards();
							state->rebuildTable();
						});
					}));
			};

			if (state->statusSort == 0 && state->smsSort == 0) {
				// The app's own order: accounts in the app, then the ones
				// whose session is gone in a block of their own.
				for (const auto &account : state->shown) {
					addLiveRow(account);
				}
				if (!lost.empty()) {
					table->add(
						object_ptr<Ui::FlatLabel>(
							table,
							u"Нет в программе (сессия разорвана, аккаунт "
								"удалён или забанен): %1"_q.arg(lost.size()),
							st::boxLabel),
						st::boxRowPadding);
					for (const auto &entry : lost) {
						addLostRow(entry);
					}
				}
			} else {
				// By status, accounts in the app and lost ones together:
				// best first (mode 1) or the ones needing attention first
				// (mode 2); by name inside one status.
				struct Item {
					int rank = 0;
					QString name;
					Main::Account *account = nullptr;
					Core::GgtgSessions::LostSession lost;
				};
				auto items = std::vector<Item>();
				for (const auto &account : state->shown) {
					const auto user = account->sessionExists()
						? account->session().user().get()
						: nullptr;
					items.push_back({
						state->smsSort
							? (user
								? Data::CountSmsSentToday(user->id.value)
								: 0)
							: LiveAccountStatus(account).rank,
						account->sessionExists()
							? account->session().user()->name().toLower()
							: QString(),
						account.get(),
						{},
					});
				}
				for (const auto &entry : lost) {
					items.push_back({
						state->smsSort ? 0 : LostAccountStatus(entry).rank,
						entry.name.toLower(),
						nullptr,
						entry,
					});
				}
				// Status: free first in mode 1. SMS: most sent first in mode 1,
				// so the key counts upwards only in mode 2.
				const auto bestFirst = state->smsSort
					? (state->smsSort == 2)
					: (state->statusSort == 1);
				ranges::stable_sort(items, [&](
						const Item &a,
						const Item &b) {
					if (a.rank != b.rank) {
						return bestFirst ? (a.rank < b.rank) : (a.rank > b.rank);
					}
					return a.name < b.name;
				});
				// "Select all" and the numbering follow what is shown.
				state->shown.clear();
				for (const auto &item : items) {
					if (item.account) {
						state->shown.push_back(item.account);
					}
				}
				for (const auto &item : items) {
					if (item.account) {
						addLiveRow(item.account);
					} else {
						addLostRow(item.lost);
					}
				}
			}
			if (state->relayout) {
				state->relayout();
			}
			if (state->shown.size() < 6 && !fullWrap->toggled()) {
				fold(false);
			}
		};

		// Counts of accounts per status, over everything (not only what the
		// folder card / search currently shows).
		state->header->setTooltipProvider([=] {
			auto counts = std::array<int, 10>();
			for (const auto &account : Core::App().domain().orderedAccounts()) {
				if (account->trashed()) {
					continue;
				}
				++counts[LiveAccountStatus(account).rank];
			}
			for (const auto &entry : CurrentLostAccounts()) {
				++counts[LostAccountStatus(entry).rank];
			}
			return StatusSummaryText(counts)
				+ u"\nСортировка: нажмите «Статус»."_q;
		});
		state->refreshTimer.setCallback([=] {
			for (const auto &row : state->rows) {
				row->update();
			}
		});
		state->refreshTimer.callEach(2000);

		// Accounts put into / taken out of the trash (here or through
		// «Массовое действие...») or gone from the app.
		rpl::merge(
			AccountTrashChanges(),
			Core::App().domain().accountsChanges()
		) | rpl::on_next([=] {
			Ui::PostponeCall(box, [=] {
				state->rebuildCards();
				state->rebuildTable();
			});
		}, box->lifetime());

		state->rebuildCards();
		state->rebuildTable();
	});
}

rpl::producer<BulkActionIndicator> BulkActionIndicatorValue() {
	return BulkIndicatorState().value();
}

void OpenBulkActions(not_null<Window::SessionController*> controller) {
	if (const auto state = CurrentBulkRun()) {
		BuildBulkProgressBox(controller->uiShow(), state);
		return;
	}
	Ggtg::Notice(controller->uiShow(), 
		u"Массовое действие сейчас не идёт. Отметьте аккаунты в "
			"«Менеджере аккаунтов» и нажмите «Массовое действие...»."_q);
}

void ShowSavedBase(std::shared_ptr<Ui::Show> show, uint64 id) {
	for (const auto &meta : Data::LoadScrapedGroupsIndex()) {
		if (meta.id != id) {
			continue;
		}
		if (meta.kind == Data::BaseKind::Users) {
			ShowEditBaseBox(
				show,
				BasesActions(),
				id,
				meta.title,
				Data::LoadScrapedGroupUsernames(id),
				false);
		} else {
			ShowEditGroupsBaseBox(
				show,
				BasesActions(),
				id,
				meta.title,
				Data::LoadScrapedGroupEntries(id),
				false);
		}
		return;
	}
	Ggtg::Notice(show, u"База не найдена"_q);
}

void RebuildAccountAccordion(
		not_null<Ui::VerticalLayout*> accordion,
		const AccountAccordionHooks &hooks) {
	const Ggtg::ScrollKeeper keepScroll(accordion);
	accordion->clear();

	auto grouped = base::flat_map<QString, Accounts>();
	auto folderOrder = std::vector<QString>();
	auto live = base::flat_set<not_null<Main::Account*>>();
	for (const auto &account : Core::App().domain().orderedAccounts()) {
		if (!account->sessionExists()) {
			continue;
		}
		live.emplace(account);
		// An account put in the «Корзина» is listed under it here, not in
		// the folder it used to be in.
		const auto tag = account->trashed()
			? TrashGroupTag()
			: account->folderTag();
		auto &bucket = grouped[tag];
		if (bucket.empty()) {
			folderOrder.push_back(tag);
		}
		bucket.push_back(account);
	}
	// Accounts removed since they were ticked must not stay selected (the
	// selection keeps raw pointers).
	auto pruned = false;
	for (auto i = hooks.selected->begin(); i != hooks.selected->end();) {
		if (!live.contains(*i)) {
			i = hooks.selected->erase(i);
			pruned = true;
		} else {
			++i;
		}
	}
	if (pruned && hooks.selectionChanged) {
		hooks.selectionChanged();
	}
	ranges::stable_sort(folderOrder, [](const QString &a, const QString &b) {
		const auto trash = TrashGroupTag();
		if ((a == trash) != (b == trash)) {
			return b == trash; // the trash always goes last
		}
		return !a.isEmpty() && b.isEmpty();
	});
	if (folderOrder.empty()) {
		accordion->add(object_ptr<Ui::FlatLabel>(
			accordion, hooks.emptyText, st::boxLabel));
		return;
	}

	// Not a direct rebuild: a handler of a widget this very rebuild is
	// about to destroy must finish first (a rebuild from inside a
	// checkbox's own change handler killed every click after the first).
	const auto rebuildLater = [=] {
		Ui::PostponeCall(accordion, [=] {
			RebuildAccountAccordion(accordion, hooks);
			// Qt re-evaluates the widget under the pointer only on a real
			// mouse move; without this a checkbox that appeared under a
			// resting pointer looks dead until the mouse is nudged.
			Ui::SendSynteticMouseEvent(
				accordion,
				QEvent::MouseMove,
				Qt::NoButton);
		});
	};
	const auto setSelected = [=](
			not_null<Main::Account*> account,
			bool checked) {
		if (checked) {
			hooks.selected->emplace(account);
		} else {
			hooks.selected->remove(account);
		}
	};
	const auto notify = [=] {
		if (hooks.selectionChanged) {
			hooks.selectionChanged();
		}
	};

	for (const auto &tag : folderOrder) {
		auto accounts = grouped[tag];
		if (hooks.sortFolder) {
			hooks.sortFolder(accounts);
		}
		const auto isTrash = (tag == TrashGroupTag());
		const auto name = isTrash
			? u"Корзина"_q
			: tag.isEmpty() ? u"Без папки"_q : tag;
		const auto expanded = hooks.expandedFolders->contains(tag);

		// A plain row, not a Ui::SettingsButton: a button's own click
		// handling covers its whole rect, checkbox included, so a click on
		// the checkbox both folded the folder and toggled the tick.
		const auto folderIconSize = style::ConvertScale(28);
		const auto rowHeight = style::ConvertScale(kPickerUserpicSize)
			+ style::ConvertScale(8);
		auto headerOwned = object_ptr<Ui::RpWidget>(accordion);
		const auto header = headerOwned.data();
		header->resize(header->width(), rowHeight);

		const auto allSelected = ranges::all_of(accounts, [&](
				not_null<Main::Account*> account) {
			return hooks.selected->contains(account);
		});
		const auto checkbox = Ui::CreateChild<Ui::Checkbox>(
			header, QString(), allSelected);
		const auto checkboxSize = st::defaultCheckbox.checkPosition.x()
			+ st::defaultCheck.diameter;
		checkbox->resize(checkboxSize, st::defaultCheck.diameter);
		checkbox->moveToLeft(
			0,
			(header->height() - checkbox->height()) / 2
				+ style::ConvertScale(7));

		const auto folderIcon = MakeFolderIcon(
			header,
			name,
			folderIconSize,
			isTrash);
		const auto folderIconLeft = checkboxSize + style::ConvertScale(8);
		folderIcon->moveToLeft(
			folderIconLeft, (header->height() - folderIconSize) / 2);

		const auto textLeft = folderIconLeft + folderIconSize
			+ style::ConvertScale(12);
		const auto label = Ui::CreateChild<Ui::FlatLabel>(
			header,
			(expanded ? u"▾ "_q : u"▸ "_q) + name
				+ u" — "_q + QString::number(accounts.size()),
			st::boxLabel);
		label->setAttribute(Qt::WA_TransparentForMouseEvents);
		label->moveToLeft(textLeft, (header->height() - label->height()) / 2);

		const auto extra = hooks.addHeaderWidget
			? hooks.addHeaderWidget(header, accounts)
			: nullptr;

		// A long folder name wraps (never clipped by the row or hidden under
		// the "Проверить" link) and the header grows to fit it.
		header->widthValue(
		) | rpl::on_next([=](int width) {
			const auto side = style::ConvertScale(8);
			const auto reserved = extra
				? (extra->width() + 2 * side)
				: side;
			label->resizeToWidth(std::max(
				width - textLeft - reserved,
				style::ConvertScale(60)));
			const auto height = std::max(
				rowHeight,
				label->height() + 2 * style::ConvertScale(8));
			if (header->height() != height) {
				header->resize(width, height);
			}
			checkbox->moveToLeft(
				0,
				(height - checkbox->height()) / 2
					+ style::ConvertScale(7));
			folderIcon->moveToLeft(
				folderIconLeft,
				(height - folderIconSize) / 2);
			label->moveToLeft(textLeft, (height - label->height()) / 2, width);
			if (extra) {
				extra->move(
					width - extra->width() - side,
					(height - extra->height()) / 2);
			}
		}, header->lifetime());

		// A click on the folder row folds/unfolds it, a click on its tick
		// selects (or clears) every account in it.
		Ggtg::SetupRowClicks(header, { checkbox }, [=](QPoint pos) {
			if (extra && extra->geometry().contains(pos)) {
				return;
			}
			if (checkbox->geometry().contains(pos)) {
				checkbox->setChecked(!checkbox->checked());
				return;
			}
			if (!hooks.expandedFolders->emplace(tag).second) {
				hooks.expandedFolders->remove(tag);
			}
			rebuildLater();
		});

		checkbox->checkedChanges(
		) | rpl::on_next([=](bool checked) {
			for (const auto &account : accounts) {
				setSelected(account, checked);
			}
			notify();
			rebuildLater();
		}, checkbox->lifetime());

		accordion->add(std::move(headerOwned));

		if (!expanded) {
			continue;
		}
		for (const auto &account : accounts) {
			if (hooks.addAccountRow) {
				hooks.addAccountRow(accordion, account);
				continue;
			}
			const auto row = AddAccountPickerRow(accordion, account);
			row->setChecked(hooks.selected->contains(account));
			row->checkedChanges(
			) | rpl::on_next([=](bool checked) {
				setSelected(account, checked);
				notify();
				// The folder's own tick may need to flip.
				rebuildLater();
			}, row->lifetime());
		}
	}
}

namespace {

// What identifies a live account, kept with a stored session so the account
// can be brought back with it.
void FillStoredMeta(
		Core::GgtgSessions::StoredSession &entry,
		not_null<Main::Account*> account) {
	const auto user = account->session().user();
	entry.userId = QString::number(account->session().userId().bare);
	entry.phone = user->phone();
	entry.name = user->name();
	entry.username = user->username();
	entry.folder = account->folderTag();
	entry.twoFactor = account->twoFactorPassword();
	entry.proxy = ProxyToObject(account->proxyOverride());
	if (entry.proxy.isEmpty()) {
		const auto selected = Core::App().settings().proxy().selected();
		if (selected && Core::App().settings().proxy().isEnabled()) {
			entry.proxy = ProxyToObject(selected);
		}
	}
	entry.device = DeviceOverrideToJson(EffectiveDeviceOverride(account));
}

[[nodiscard]] std::optional<Core::GgtgSessions::StoredSession> FindStored(
		const QString &id) {
	for (const auto &entry : Core::GgtgSessions::LoadVault()) {
		if (entry.id == id) {
			return entry;
		}
	}
	return std::nullopt;
}

// Runs an account on the session in `folder`'s "tdata", keeping what the
// account carries (folder tag, 2FA note, proxy, pin).
void AddAccountFromFolder(
		const QString &folder,
		const QString &tag,
		const QString &password,
		const MTP::ProxyData &proxy,
		bool pinned,
		bool activate,
		Fn<void(QString error)> done,
		std::optional<MTP::DeviceOverride> deviceOverride = std::nullopt) {
	const auto added = Core::App().domain().addFromTdata(
		folder,
		MTP::Environment::Production,
		[=] { return proxy; },
		[=](not_null<Main::Account*> account, const QString &) {
			if (!tag.isEmpty()) {
				account->setFolderTag(tag);
			}
			if (!password.isEmpty()) {
				account->setTwoFactorPassword(password);
			}
			if (pinned) {
				account->setPinned(true);
			}
			if (deviceOverride) {
				account->setDeviceOverride(*deviceOverride);
			}
		});
	if (added.empty()) {
		done(u"не удалось добавить аккаунт из этой сессии (нет файлов "
			"сессии или такой аккаунт уже есть в программе)"_q);
		return;
	}
	if (activate) {
		Core::App().domain().maybeActivate(added.front());
	}
	done(QString());
}

} // namespace

void CreateBackupSession(
		not_null<Main::Account*> account,
		const QString &note,
		Fn<void(QString error)> done) {
	// When the saved 2FA password is missing or wrong the box asks for it;
	// skipping it reports just this account and the queue moves on.
	CreateBackupSessionWith(
		account,
		note,
		MakeSessionPasswordPrompt(account),
		[=](QString error, QString) {
			done(error);
		});
}

void CreateBackupSessionWith(
		not_null<Main::Account*> account,
		const QString &note,
		Fn<void(Fn<void(QString password)> submit, Fn<void()> cancel)>
			requestPassword,
		Fn<void(QString error, QString id)> done,
		bool asAndroid) {
	namespace Sessions = Core::GgtgSessions;
	if (!Main::AccountIsLive(account)) {
		done(u"аккаунт не в сети"_q, QString());
		return;
	}
	const auto weak = base::make_weak(account.get());
	auto device = asAndroid
		? DeviceOverrideFromJson(RandomAndroidDeviceJson(account->deviceOverride()))
		: RandomDeviceOverrideForAccount(weak.get());
	const auto phone = account->sessionExists()
		? account->session().user()->phone()
		: QString();
	if (!phone.isEmpty()) {
		const auto geo = Core::GgtgGeo::MatchForPhone(phone);
		device.systemLangCode = geo.systemLang;
		device.cloudLangCode = geo.cloudLang;
		device.tzOffset = geo.tzOffset;
	}
	Main::DuplicateSessionTracked(account, requestPassword, [=](
			QString error,
			QByteArray serialized,
			uint64 hash) {
		if (!error.isEmpty()) {
			done(error, QString());
			return;
		}
		if (!weak || !Main::AccountIsLive(weak.get())) {
			done(u"аккаунт больше не в сети"_q, QString());
			return;
		}
		auto entry = Sessions::StoredSession();
		FillStoredMeta(entry, weak.get());
		entry.device = DeviceOverrideToJson(device);
		entry.role = asAndroid ? u"android"_q : u"backup"_q;
		entry.source = asAndroid ? u"duplicate_android"_q : u"duplicate"_q;
		entry.note = note;
		entry.hash = hash;
		entry.state = hash ? u"ok"_q : QString();
		entry = Sessions::AddStored(entry);
		auto saveError = QString();
		if (!Storage::ExportRawAuthorizationAsTdata(
				serialized,
				Sessions::VaultFolder(entry),
				&saveError)) {
			Sessions::RemoveStored(entry.id);
			done(saveError, QString());
			return;
		}
		Sessions::WriteSessionVaultSidecar(entry);
		Core::GgtgLog::Info(
			u"Сессии"_q,
			u"Создана %1 сессия: +%2 %3"_q
				.arg(asAndroid ? u"мобильная Android"_q : u"резервная"_q)
				.arg(entry.phone)
				.arg(entry.name));
		done(QString(), entry.id);
	}, device, asAndroid);
}

void CreateMobileSessionWith(
		not_null<Main::Account*> account,
		const QString &note,
		Fn<void(Fn<void(QString password)> submit, Fn<void()> cancel)>
			requestPassword,
		Fn<void(QString error, QString id)> done) {
	CreateBackupSessionWith(account, note, requestPassword, done, /*asAndroid=*/true);
}

void RunSequentialAccountsAction(
		not_null<Window::SessionController*> controller,
		std::vector<not_null<Main::Account*>> accounts,
		StepWithProgress step,
		const QString &title) {
	RunBulkAction(controller, std::move(accounts), std::move(step), BulkOptions{ .title = title });
}

void RunSequentialAccountsAction(
		not_null<Window::SessionController*> controller,
		std::vector<not_null<Main::Account*>> accounts,
		Step step,
		const QString &title) {
	RunBulkAction(controller, std::move(accounts), std::move(step), BulkOptions{ .title = title });
}

QString StoreSessionKey(
		not_null<Main::Account*> account,
		const QString &role,
		const QString &note,
		const QByteArray &serialized,
		uint64 hash,
		QString *error,
		std::optional<MTP::DeviceOverride> device) {
	namespace Sessions = Core::GgtgSessions;
	auto entry = Sessions::StoredSession();
	FillStoredMeta(entry, account);
	if (device) {
		entry.device = DeviceOverrideToJson(*device);
	}
	entry.role = role;
	entry.source = role;
	entry.note = note;
	entry.hash = hash;
	entry.state = hash ? u"ok"_q : QString();
	entry = Sessions::AddStored(entry);
	if (!Storage::ExportRawAuthorizationAsTdata(
			serialized,
			Sessions::VaultFolder(entry),
			error)) {
		Sessions::RemoveStored(entry.id);
		return QString();
	}
	Sessions::WriteSessionVaultSidecar(entry);
	return entry.id;
}

void TerminateOtherSessions(
		not_null<Main::Account*> account,
		Fn<void(QString error, QString report)> done) {
	namespace Sessions = Core::GgtgSessions;
	if (!Main::AccountIsLive(account)) {
		done(u"нет активной сессии"_q, QString());
		return;
	}
	// One by one instead of "terminate all": every session this app made or
	// handed out (spares, duplicates, Android, exports - known by their
	// numbers) must survive.
	const auto userId = QString::number(account->session().userId().bare);
	auto keep = base::flat_set<uint64>();
	auto unknown = 0;
	for (const auto &entry
			: Sessions::StoredForUser(Sessions::LoadVault(), userId)) {
		if (entry.state == u"revoked"_q) {
			continue;
		} else if (entry.hash) {
			keep.emplace(entry.hash);
		} else if (Sessions::HasKey(entry)) {
			++unknown;
		}
	}
	const auto weak = base::make_weak(account.get());
	Main::FetchSessionHashes(account, [=](
			std::vector<uint64> hashes,
			QString error) {
		if (!error.isEmpty()) {
			done(error, QString());
			return;
		}
		auto terminate = std::make_shared<std::vector<uint64>>();
		auto kept = 0;
		for (const auto hash : hashes) {
			if (!hash) {
				continue;
			} else if (keep.contains(hash)) {
				++kept;
			} else {
				terminate->push_back(hash);
			}
		}
		const auto report = [=](int ended) {
			auto text = u"завершено: %1"_q.arg(ended);
			if (kept > 0) {
				text += u", сессии GGTG оставлены: %1"_q.arg(kept);
			}
			if (unknown > 0) {
				text += u" (у %1 сохранённой номер неизвестен - она могла "
					"быть завершена)"_q.arg(unknown);
			}
			return text;
		};
		const auto ended = std::make_shared<int>(0);
		const auto next = std::make_shared<Fn<void()>>();
		*next = [=] {
			const auto alive = weak.get();
			if (!alive || !Main::AccountIsLive(alive)) {
				done(u"аккаунт больше не в сети"_q, QString());
				*next = nullptr;
				return;
			}
			if (terminate->empty()) {
				done(QString(), report(*ended));
				*next = nullptr;
				return;
			}
			const auto hash = terminate->back();
			terminate->pop_back();
			alive->session().api().authorizations().requestTerminate(
				[=](const MTPBool &) {
					++*ended;
					if (*next) {
						(*next)();
					}
				},
				[=](const MTP::Error &error) {
					if (error.type().startsWith(
							u"FRESH_RESET_AUTHORISATION"_q)) {
						done(u"Telegram не даёт завершать другие сессии, "
							"пока эта сессия новая (меньше суток)"_q,
							QString());
						*next = nullptr;
					} else if (*next) {
						(*next)();
					}
				},
				hash);
		};
		(*next)();
	});
}

QString SwitchWarning(const Core::GgtgSessions::StoredSession &entry) {
	if (entry.role == u"android"_q || entry.source.contains(u"android"_q)) {
		return u"Эта сессия создана на Android. Если аккаунт сейчас активен "
			"на телефоне, переключение приведёт к одновременной работе "
			"одного ключа авторизации и разрыву сессии (AUTH_KEY_DUPLICATED). "
			"Перед переключением убедитесь, что приложение на телефоне закрыто "
			"или аккаунт не используется."_q;
	} else if (entry.role == u"exported"_q || entry.source.contains(u"export"_q)) {
		return u"Эта сессия была экспортирована. Если она сейчас активна "
			"на другом устройстве или в стороннем софте, одновременная "
			"работа приведёт к разрыву сессии (AUTH_KEY_DUPLICATED)."_q;
	} else if (entry.role == u"duplicate"_q) {
		return u"Это дублированная сессия"_q
			+ (entry.note.isEmpty() ? QString() : (u" ("_q + entry.note + ')'))
			+ u". Если она где-то уже работает, после переключения Telegram "
				"разорвёт её в обоих местах - сначала уберите её там."_q;
	}
	return QString();
}

void SwitchAccountToFolderTracked(
		not_null<Main::Account*> account,
		const QString &folder,
		const QString &leftStoredId,
		Fn<void(QString error)> done,
		std::optional<MTP::ProxyData> targetProxy,
		std::optional<MTP::DeviceOverride> targetDevice) {
	namespace Sessions = Core::GgtgSessions;
	const auto userId = QString::number(account->session().userId().bare);
	// The session being left has no number of its own in its list; seen
	// from the next one it does. So: the list now, and the one entry that
	// is new in the list after the switch is it.
	Main::FetchSessionHashes(account, [=](
			std::vector<uint64> before,
			QString) {
		if (!Main::AccountIsLive(account)) {
			done(u"аккаунт не в сети"_q);
			return;
		}
		SwitchAccountToFolder(account, folder, [=](QString error) {
			done(error);
			if (!error.isEmpty() || leftStoredId.isEmpty()) {
				return;
			}
			for (const auto &now : Core::App().domain().orderedAccounts()) {
				if (!now->sessionExists()
					|| QString::number(now->session().userId().bare)
						!= userId) {
					continue;
				}
				Main::FetchSessionHashes(now, [=](
						std::vector<uint64> after,
						QString error) {
					if (!error.isEmpty()) {
						return;
					}
					auto appeared = std::vector<uint64>();
					for (const auto hash : after) {
						if (hash && !ranges::contains(before, hash)) {
							appeared.push_back(hash);
						}
					}
					if (appeared.size() == 1) {
						Sessions::UpdateStoredHash(
							leftStoredId,
							appeared.front());
					}
				});
				break;
			}
		}, targetProxy, targetDevice);
	});
}

void SwitchAccountToFolder(
		not_null<Main::Account*> account,
		const QString &folder,
		Fn<void(QString error)> done,
		std::optional<MTP::ProxyData> targetProxy,
		std::optional<MTP::DeviceOverride> targetDevice) {
	namespace Sessions = Core::GgtgSessions;
	if (!Main::AccountIsLive(account)) {
		done(u"аккаунт не в сети"_q);
		return;
	}
	const auto user = account->session().user();
	const auto userId = QString::number(account->session().userId().bare);
	const auto tag = account->folderTag();
	const auto password = account->twoFactorPassword();
	const auto proxy = (targetProxy && MTP::HasRealProxy(*targetProxy))
		? *targetProxy
		: account->proxyOverride();
	const auto pinned = account->pinned();
	const auto wasActive = (&Core::App().activeAccount() == account.get());

	// If this is cut short the account would just disappear: leave a note
	// that shows it in the manager with its spares, removed on success.
	auto note = Sessions::LostSession();
	note.at = base::unixtime::now();
	note.userId = userId;
	note.phone = user->phone();
	note.name = user->name();
	note.username = user->username();
	note.folder = tag;
	note.twoFactor = password;
	note.reason = u"SWAP"_q;
	Sessions::RecordQuiet(std::move(note));

	// Local only - nothing is sent to Telegram, so the session being left
	// stays valid for whoever takes it (a spare, an export).
	account->forcedLogOut();
	AddAccountFromFolder(
		folder,
		tag,
		password,
		proxy,
		pinned,
		wasActive,
		[=](QString error) {
			if (error.isEmpty()) {
				Sessions::RemoveReason(userId, u"SWAP"_q);
			}
			done(error);
		},
		targetDevice);
}

void SwapToStoredSession(
		not_null<Main::Account*> account,
		const QString &id,
		Fn<void(QString error)> done) {
	namespace Sessions = Core::GgtgSessions;
	if (!Main::AccountIsLive(account)) {
		done(u"аккаунт не в сети"_q);
		return;
	}
	const auto target = FindStored(id);
	const auto userId = QString::number(account->session().userId().bare);
	if (!target
		|| !Sessions::HasKey(*target)
		|| target->userId != userId) {
		done(u"сохранённая сессия не найдена"_q);
		return;
	}
	const auto targetFolder = Sessions::VaultFolder(*target);
	if (!QDir(targetFolder + u"/tdata"_q).exists()) {
		done(u"файлы резервной сессии не найдены"_q);
		return;
	}
	// First keep the session being left as a spare of its own.
	auto old = Sessions::StoredSession();
	FillStoredMeta(old, account);
	old.role = u"previous"_q;
	old.source = u"swap"_q;
	old.note = u"бывшая рабочая"_q;
	old = Sessions::AddStored(old);
	auto error = QString();
	if (!Storage::ExportRawAuthorizationAsTdata(
			account->serializeMtpAuthorization(),
			Sessions::VaultFolder(old),
			&error)) {
		Sessions::RemoveStored(old.id);
		done(error);
		return;
	}
	Sessions::WriteSessionVaultSidecar(old);
	const auto targetId = target->id;
	const auto oldId = old.id;
	const auto weak = base::make_weak(account.get());

	const auto targetProxy = (!target->proxy.isEmpty())
		? std::make_optional(ProxyFromObject(target->proxy))
		: std::nullopt;
	const auto targetDevice = (!target->device.isEmpty())
		? std::make_optional(DeviceOverrideFromJson(target->device))
		: std::nullopt;

	SwitchAccountToFolderTracked(
		account,
		targetFolder,
		oldId,
		[=](QString switchError) {
			if (switchError.isEmpty()) {
				// Mark target session as active in vault, never remove it.
				Sessions::UpdateStoredRole(targetId, target->role, u"активная (текущая)"_q);
			} else if (const auto alive = weak.get()
				; alive && Main::AccountIsLive(alive)) {
				// Still running on it: no need for the copy. (If the
				// switch was cut short, the copy is how it comes back.)
				Sessions::RemoveStored(oldId);
			}
			done(switchError);
		},
		targetProxy,
		targetDevice);
}

void MoveToNewSession(
		not_null<Main::Account*> account,
		const QByteArray &newKey,
		uint64 newHash,
		const QString &handedOutNote,
		Fn<void(QString error)> done) {
	namespace Sessions = Core::GgtgSessions;
	if (!Main::AccountIsLive(account)) {
		done(u"аккаунт не в сети"_q);
		return;
	}
	auto spare = Sessions::StoredSession();
	FillStoredMeta(spare, account);
	spare.role = u"backup"_q;
	spare.source = u"handover"_q;
	spare.note = u"новая рабочая (оригинал передан)"_q;
	spare.hash = newHash;
	spare.state = newHash ? u"ok"_q : QString();
	spare = Sessions::AddStored(spare);
	auto error = QString();
	if (!Storage::ExportRawAuthorizationAsTdata(
			newKey,
			Sessions::VaultFolder(spare),
			&error)) {
		Sessions::RemoveStored(spare.id);
		done(error);
		return;
	}
	Sessions::WriteSessionVaultSidecar(spare);

	// The original session is saved in the vault with its raw key
	auto handedOut = Sessions::StoredSession();
	FillStoredMeta(handedOut, account);
	handedOut.role = u"exported"_q;
	handedOut.source = u"export"_q;
	handedOut.note = handedOutNote;
	handedOut = Sessions::AddStored(handedOut);
	auto handedErr = QString();
	Storage::ExportRawAuthorizationAsTdata(
		account->serializeMtpAuthorization(),
		Sessions::VaultFolder(handedOut),
		&handedErr);
	Sessions::WriteSessionVaultSidecar(handedOut);

	const auto spareId = spare.id;
	SwitchAccountToFolder(
		account,
		Sessions::VaultFolder(spare),
		[=](QString switchError) {
			if (switchError.isEmpty()) {
				Sessions::UpdateStoredRole(spareId, u"backup"_q, u"активная (текущая)"_q);
			}
			done(switchError);
		});
}

void RestoreFromStoredSession(
		const QString &id,
		Fn<void(QString error)> done) {
	namespace Sessions = Core::GgtgSessions;
	const auto entry = FindStored(id);
	if (!entry || !Sessions::HasKey(*entry)) {
		done(u"сохранённая сессия не найдена"_q);
		return;
	}
	const auto folder = Sessions::VaultFolder(*entry);
	if (!QDir(folder + u"/tdata"_q).exists()) {
		done(u"файлы резервной сессии не найдены"_q);
		return;
	}
	const auto userId = entry->userId;
	AddAccountFromFolder(
		folder,
		entry->folder,
		entry->twoFactor,
		ProxyFromObject(entry->proxy),
		false,
		false,
		[=](QString error) {
			if (error.isEmpty()) {
				Sessions::RemoveStored(id);
				Sessions::RemoveReason(userId, u"SWAP"_q);
				Sessions::RemoveUser(userId);
			}
			done(error);
		});
}

void AddAccountPicker(
		not_null<Ui::GenericBox*> box,
		not_null<AccountPickerState*> state,
		const QString &emptyText) {
	const auto refresh = [=] {
		const auto count = int(state->selected.size());
		state->selectedCount = count;
		state->selectAllText = (count == 0)
			? u"Выбрать всех"_q
			: u"Снять выбор"_q;
		if (const auto label = state->selectedLabel.data()) {
			label->setText(u"Выбрано аккаунтов: "_q + QString::number(count));
		}
	};
	const auto rebuild = [=] {
		RebuildAccountAccordion(state->accordion, AccountAccordionHooks{
			.selected = &state->selected,
			.expandedFolders = &state->expandedFolders,
			.selectionChanged = refresh,
			.emptyText = emptyText.isEmpty()
				? u"Нет добавленных аккаунтов"_q
				: emptyText,
		});
	};
	box->addRow(object_ptr<Ggtg::ActionRow>(
		box, state->selectAllText.value(), u"✓"_q, &st::menuIconSelect)
	)->setClickedCallback([=] {
		const auto select = state->selected.empty();
		for (const auto &account : Core::App().domain().orderedAccounts()) {
			if (!Main::AccountIsLive(account)) {
				continue;
			}
			if (select) {
				state->selected.emplace(account);
			} else {
				state->selected.remove(account);
			}
		}
		rebuild();
		refresh();
	});
	state->accordion = box->addRow(object_ptr<Ui::VerticalLayout>(box));
	rebuild();
	state->selectedLabel = box->addRow(
		object_ptr<Ui::FlatLabel>(box, QString(), st::boxLabel),
		style::margins(
			st::boxRowPadding.left(),
			st::defaultVerticalListSkip,
			st::boxRowPadding.right(),
			st::defaultVerticalListSkip));
	refresh();
}

Window::SessionController *CurrentSessionController(
		Window::SessionController *fallback) {
	if (const auto window = Core::App().activeWindow()) {
		if (const auto controller = window->sessionController()) {
			return controller;
		}
	}
	return fallback;
}

void AutoDeviceGeo(
		not_null<Main::Account*> account,
		Fn<void(bool ok, QString info)> done) {
	if (!MTP::HasRealProxy(account->proxyOverride())) {
		if (done) {
			done(false, u"у аккаунта нет прокси"_q);
		}
		return;
	}
	const auto weak = base::make_weak(account.get());
	DetectGeoForProxy(account->proxyOverride(), [=](
			std::optional<GeoResult> geo,
			QString error) {
		const auto strong = weak.get();
		if (!strong || !Main::AccountIsLive(strong)) {
			if (done) {
				done(false, u"аккаунт больше не залогинен"_q);
			}
			return;
		} else if (!geo) {
			if (done) {
				done(false, error);
			}
			return;
		}
		auto value = strong->deviceOverride();
		value.systemLangCode = geo->lang.systemCode;
		value.cloudLangCode = geo->lang.cloudCode;
		value.tzOffset = geo->offset;
		strong->setDeviceOverride(value);
		if (done) {
			done(true, geo->ip + u", "_q + geo->country + u" → "_q
				+ geo->lang.systemCode + u", UTC"_q
				+ FormatUtcOffset(geo->offset));
		}
	});
}

bool IsAndroidSession(const MTP::DeviceOverride &device) {
	if (device.langPackName == u"android"_q || device.langPackName == u"android_x"_q) {
		return true;
	}
	if (device.apiId == 4 || device.apiId == 6 || device.apiId == 21724) {
		return true;
	}
	if (device.systemVersion.startsWith(u"SDK"_q, Qt::CaseInsensitive)) {
		return true;
	}
	const auto dev = device.deviceModel.toLower();
	for (const auto brand : { "samsung", "xiaomi", "huawei", "motorola", "realme", "pixel", "oneplus", "honor", "oppo", "vivo" }) {
		if (dev.startsWith(brand)) {
			return true;
		}
	}
	return false;
}

bool IsMacSession(const MTP::DeviceOverride &device) {
	if (device.systemVersion.startsWith(u"macOS"_q)
		|| device.systemVersion.startsWith(u"OS X"_q)
		|| device.langPackName == u"macos"_q) {
		return true;
	}
	const auto dev = device.deviceModel.toLower();
	for (const auto prefix : { "macbook", "imac", "mac mini", "mac studio", "mac pro" }) {
		if (dev.startsWith(QLatin1String(prefix))) {
			return true;
		}
	}
	return false;
}

bool IsIosSession(const MTP::DeviceOverride &device) {
	if (device.langPackName == u"ios"_q) {
		return true;
	}
	if (device.apiId == 10840 || device.apiId == 16352 || device.apiId == 8 || device.apiId == 2834) {
		return true;
	}
	if (device.systemVersion.startsWith(u"iOS"_q, Qt::CaseInsensitive)) {
		return true;
	}
	const auto dev = device.deviceModel.toLower();
	return dev.contains("iphone") || dev.contains("ipad");
}

MTP::DeviceOverride RandomWindowsDeviceOverride(const QString &phone) {
	const auto profile = Ggtg::DesktopProfiles::RandomWindowsProfile(phone);
	auto result = MTP::DeviceOverride();
	result.apiId = Ggtg::DesktopProfiles::kAppId;
	result.apiHash = QString::fromLatin1(Ggtg::DesktopProfiles::kAppHash);
	result.deviceModel = profile.deviceModel;
	result.systemVersion = profile.systemVersion;
	result.appVersion = profile.appVersion;
	result.systemLangCode = profile.systemLang;
	result.cloudLangCode = profile.cloudLang;
	result.langPackName = QString::fromLatin1(Ggtg::DesktopProfiles::kLangPack);
	const auto geo = Core::GgtgGeo::MatchForPhone(phone);
	result.tzOffset = !phone.isEmpty() ? geo.tzOffset : QDateTime::currentDateTime().offsetFromUtc();
	if (!phone.isEmpty()) {
		result.systemLangCode = geo.systemLang;
		result.cloudLangCode = geo.cloudLang;
	}
	return result;
}

MTP::DeviceOverride RandomIosDeviceOverride(const QString &phone) {
	const auto profile = Ggtg::IosProfiles::RandomProfile(phone);
	auto result = MTP::DeviceOverride();
	result.apiId = Ggtg::IosProfiles::kAppId;
	result.apiHash = QString::fromLatin1(Ggtg::IosProfiles::kAppHash);
	result.deviceModel = profile.deviceModel;
	result.systemVersion = profile.systemVersion;
	result.appVersion = profile.appVersion;
	result.systemLangCode = profile.systemLang;
	result.cloudLangCode = profile.cloudLang;
	result.langPackName = QString::fromLatin1(Ggtg::IosProfiles::kLangPack);
	const auto geo = Core::GgtgGeo::MatchForPhone(phone);
	result.tzOffset = !phone.isEmpty() ? geo.tzOffset : QDateTime::currentDateTime().offsetFromUtc();
	if (!phone.isEmpty()) {
		result.systemLangCode = geo.systemLang;
		result.cloudLangCode = geo.cloudLang;
	}
	return result;
}

MTP::DeviceOverride RandomMacDeviceOverride(const QString &phone) {
	const auto profile = Ggtg::DesktopProfiles::RandomMacProfile(phone);
	auto result = MTP::DeviceOverride();
	result.apiId = Ggtg::DesktopProfiles::kAppId;
	result.apiHash = QString::fromLatin1(Ggtg::DesktopProfiles::kAppHash);
	result.deviceModel = profile.deviceModel;
	result.systemVersion = profile.systemVersion;
	result.appVersion = profile.appVersion;
	result.systemLangCode = profile.systemLang;
	result.cloudLangCode = profile.cloudLang;
	result.langPackName = QString::fromLatin1(Ggtg::DesktopProfiles::kLangPack);
	const auto geo = Core::GgtgGeo::MatchForPhone(phone);
	result.tzOffset = !phone.isEmpty() ? geo.tzOffset : QDateTime::currentDateTime().offsetFromUtc();
	if (!phone.isEmpty()) {
		result.systemLangCode = geo.systemLang;
		result.cloudLangCode = geo.cloudLang;
	}
	return result;
}

MTP::DeviceOverride RandomDesktopDeviceOverride(const QString &phone) {
	const auto profile = Ggtg::DesktopProfiles::RandomProfile(phone);
	auto result = MTP::DeviceOverride();
	result.apiId = Ggtg::DesktopProfiles::kAppId;
	result.apiHash = QString::fromLatin1(Ggtg::DesktopProfiles::kAppHash);
	result.deviceModel = profile.deviceModel;
	result.systemVersion = profile.systemVersion;
	result.appVersion = profile.appVersion;
	result.systemLangCode = profile.systemLang;
	result.cloudLangCode = profile.cloudLang;
	result.langPackName = QString::fromLatin1(Ggtg::DesktopProfiles::kLangPack);
	const auto geo = Core::GgtgGeo::MatchForPhone(phone);
	result.tzOffset = !phone.isEmpty() ? geo.tzOffset : QDateTime::currentDateTime().offsetFromUtc();
	if (!phone.isEmpty()) {
		result.systemLangCode = geo.systemLang;
		result.cloudLangCode = geo.cloudLang;
	}
	return result;
}

MTP::DeviceOverride RandomAndroidDeviceOverride(const QString &phone) {
	const auto profile = Ggtg::AndroidProfiles::RandomProfile(phone);
	auto result = MTP::DeviceOverride();
	result.apiId = Ggtg::AndroidProfiles::kAppId;
	result.apiHash = QString::fromLatin1(Ggtg::AndroidProfiles::kAppHash);
	result.deviceModel = profile.deviceModel;
	result.systemVersion = profile.systemVersion;
	result.appVersion = profile.appVersion;
	result.systemLangCode = profile.systemLang;
	result.cloudLangCode = profile.cloudLang;
	result.langPackName = QString::fromLatin1(Ggtg::AndroidProfiles::kLangPack);
	const auto geo = Core::GgtgGeo::MatchForPhone(phone);
	result.tzOffset = !phone.isEmpty() ? geo.tzOffset : QDateTime::currentDateTime().offsetFromUtc();
	if (!phone.isEmpty()) {
		result.systemLangCode = geo.systemLang;
		result.cloudLangCode = geo.cloudLang;
	}
	return result;
}

MTP::DeviceOverride RandomDeviceOverrideForAccount(not_null<Main::Account*> account) {
	const auto &current = account->deviceOverride();
	const auto phone = account->sessionExists()
		? account->session().user()->phone()
		: QString();
	if (IsIosSession(current)) {
		return RandomIosDeviceOverride(phone);
	}
	if (IsMacSession(current)) {
		return RandomMacDeviceOverride(phone);
	}
	if (Platform::IsMac() && !IsAndroidSession(current)) {
		return RandomMacDeviceOverride(phone);
	}
	return RandomDesktopDeviceOverride(phone);
}

void RandomizeAccountDevice(
		not_null<Main::Account*> account,
		Fn<void(bool ok, QString info)> done) {
	if (!Main::AccountIsLive(account)) {
		if (done) {
			done(false, u"аккаунт больше не залогинен"_q);
		}
		return;
	}
	const auto pick = RandomDeviceOverrideForAccount(account);
	account->setDeviceOverride(pick);
	const auto text = pick.deviceModel + u" · "_q + pick.systemVersion + u" ("_q + pick.appVersion + u")"_q;
	if (!MTP::HasRealProxy(account->proxyOverride())) {
		if (done) {
			done(true, text);
		}
		return;
	}
	// Language and time zone follow the proxy's location.
	AutoDeviceGeo(account, [=](bool ok, QString info) {
		if (done) {
			done(true, text + (ok ? (u"; "_q + info) : QString()));
		}
	});
}

MTP::DeviceOverride RandomDeviceOverride(const QString &phone) {
	return ((base::RandomValue<uint32>() % 2) == 0)
		? RandomMacDeviceOverride(phone)
		: RandomDesktopDeviceOverride(phone);
}

QJsonObject DeviceOverrideToJson(const MTP::DeviceOverride &value) {
	auto result = QJsonObject();
	if (value == MTP::DeviceOverride()) {
		return result;
	}
	result["deviceModel"] = value.deviceModel;
	result["systemVersion"] = value.systemVersion;
	result["systemLangCode"] = value.systemLangCode;
	result["langPackName"] = value.langPackName;
	result["cloudLangCode"] = value.cloudLangCode;
	if (!value.appVersion.isEmpty()) {
		result["appVersion"] = value.appVersion;
	}
	if (value.tzOffset) {
		result["tzOffset"] = *value.tzOffset;
	}
	if (value.apiId > 0) {
		result["apiId"] = value.apiId;
	}
	if (!value.apiHash.isEmpty()) {
		result["apiHash"] = value.apiHash;
	}
	return result;
}

QJsonObject RandomAndroidDeviceJson(const MTP::DeviceOverride &effective) {
	// Exactly what the Android client sends: Build.MANUFACTURER + Build.MODEL,
	// "SDK <api level>" that really belongs to that model, and a real Telegram
	// build (see ggtg_android_profiles.h). The language is the account's own
	// (fitted to its proxy) when the catalog knows it, else a catalog one; the
	// time zone is the account's own, so Telegram sees one and the same
	// identity when the session is minted and when the phone uses it.
	const auto profile = Ggtg::AndroidProfiles::RandomProfile();
	auto cloud = profile.cloudLang;
	auto system = profile.systemLang;
	if (const auto known = Ggtg::AndroidProfiles::LanguageForSystemCode(
			effective.systemLangCode)) {
		cloud = known->cloud;
		system = known->system;
	}
	auto result = QJsonObject();
	result["apiId"] = Ggtg::AndroidProfiles::kAppId;
	result["apiHash"] = QString::fromLatin1(Ggtg::AndroidProfiles::kAppHash);
	result["deviceModel"] = profile.deviceModel;
	result["systemVersion"] = profile.systemVersion;
	result["appVersion"] = profile.appVersion;
	result["systemLangCode"] = system;
	result["cloudLangCode"] = cloud;
	result["langPackName"] = QString::fromLatin1(
		Ggtg::AndroidProfiles::kLangPack);
	if (effective.tzOffset) {
		result["tzOffset"] = *effective.tzOffset;
	}
	return result;
}

QJsonObject RandomDesktopDeviceJson(const MTP::DeviceOverride &effective) {
	const auto profile = Ggtg::DesktopProfiles::RandomProfile();
	auto cloud = profile.cloudLang;
	auto system = profile.systemLang;
	if (const auto known = Ggtg::AndroidProfiles::LanguageForSystemCode(
			effective.systemLangCode)) {
		cloud = known->cloud;
		system = known->system;
	}
	auto result = QJsonObject();
	result["apiId"] = Ggtg::DesktopProfiles::kAppId;
	result["apiHash"] = QString::fromLatin1(Ggtg::DesktopProfiles::kAppHash);
	result["deviceModel"] = profile.deviceModel;
	result["systemVersion"] = profile.systemVersion;
	result["appVersion"] = profile.appVersion;
	result["systemLangCode"] = system;
	result["cloudLangCode"] = cloud;
	result["langPackName"] = QString::fromLatin1(
		Ggtg::DesktopProfiles::kLangPack);
	if (effective.tzOffset) {
		result["tzOffset"] = *effective.tzOffset;
	}
	return result;
}

QJsonObject RandomIosDeviceJson(const MTP::DeviceOverride &effective) {
	const auto profile = Ggtg::IosProfiles::RandomProfile();
	auto cloud = profile.cloudLang;
	auto system = profile.systemLang;
	if (const auto known = Ggtg::AndroidProfiles::LanguageForSystemCode(
			effective.systemLangCode)) {
		cloud = known->cloud;
		system = known->system;
	}
	auto result = QJsonObject();
	result["apiId"] = Ggtg::IosProfiles::kAppId;
	result["apiHash"] = QString::fromLatin1(Ggtg::IosProfiles::kAppHash);
	result["deviceModel"] = profile.deviceModel;
	result["systemVersion"] = profile.systemVersion;
	result["appVersion"] = profile.appVersion;
	result["systemLangCode"] = system;
	result["cloudLangCode"] = cloud;
	result["langPackName"] = QString::fromLatin1(
		Ggtg::IosProfiles::kLangPack);
	if (effective.tzOffset) {
		result["tzOffset"] = *effective.tzOffset;
	}
	return result;
}

MTP::DeviceOverride EffectiveDeviceOverride(
		not_null<Main::Account*> account) {
	// What Telegram is told for this account right now: its own values,
	// and the app's usual ones where nothing is set.
	auto result = account->deviceOverride();
	if (result.deviceModel.isEmpty() || result.systemVersion.isEmpty()) {
		result = RandomDeviceOverrideForAccount(account);
		account->setDeviceOverride(result);
		return result;
	}
	if (result.appVersion.isEmpty()) {
		result.appVersion = IsIosSession(result)
			? Ggtg::IosProfiles::RandomVersion()
			: IsAndroidSession(result)
			? Ggtg::AndroidProfiles::RandomBuild()
			: Ggtg::DesktopProfiles::RandomDesktopVersion();
	}
	if (result.langPackName.isEmpty()) {
		result.langPackName = IsIosSession(result)
			? QString::fromLatin1(Ggtg::IosProfiles::kLangPack)
			: IsAndroidSession(result)
			? QString::fromLatin1(Ggtg::AndroidProfiles::kLangPack)
			: QString::fromLatin1(Ggtg::DesktopProfiles::kLangPack);
	}
	if (result.apiId <= 0) {
		result.apiId = IsIosSession(result)
			? Ggtg::IosProfiles::kAppId
			: IsAndroidSession(result)
			? Ggtg::AndroidProfiles::kAppId
			: Ggtg::DesktopProfiles::kAppId;
	}
	if (result.apiHash.isEmpty()) {
		result.apiHash = IsIosSession(result)
			? QString::fromLatin1(Ggtg::IosProfiles::kAppHash)
			: IsAndroidSession(result)
			? QString::fromLatin1(Ggtg::AndroidProfiles::kAppHash)
			: QString::fromLatin1(Ggtg::DesktopProfiles::kAppHash);
	}
	if (result.systemLangCode.isEmpty() || !result.tzOffset) {
		const auto phone = account->sessionExists()
			? account->session().user()->phone()
			: QString();
		if (!phone.isEmpty()) {
			const auto geo = Core::GgtgGeo::MatchForPhone(phone);
			if (result.systemLangCode.isEmpty()) {
				result.systemLangCode = geo.systemLang;
			}
			if (result.cloudLangCode.isEmpty()) {
				result.cloudLangCode = geo.cloudLang;
			}
			if (!result.tzOffset) {
				result.tzOffset = geo.tzOffset;
			}
		}
	}
	if (result.systemLangCode.isEmpty()) {
		result.systemLangCode = Lang::GetInstance().systemLangCode();
	}
	if (result.cloudLangCode.isEmpty()) {
		result.cloudLangCode = Lang::GetInstance().cloudLangCode(
			Lang::Pack::Current);
	}
	if (!result.tzOffset) {
		result.tzOffset = QDateTime::currentDateTime().offsetFromUtc();
	}
	return result;
}

MTP::DeviceOverride DeviceOverrideFromJson(const QJsonObject &object) {
	auto result = MTP::DeviceOverride();
	result.deviceModel = object.value("deviceModel").toString();
	result.systemVersion = object.value("systemVersion").toString();
	result.systemLangCode = object.value("systemLangCode").toString();
	result.langPackName = object.value("langPackName").toString();
	result.cloudLangCode = object.value("cloudLangCode").toString();
	result.appVersion = object.value("appVersion").toString();
	if (object.contains("tzOffset")) {
		result.tzOffset = object.value("tzOffset").toInt();
	}
	if (object.contains("apiId")) {
		result.apiId = object.value("apiId").toInt();
	}
	if (object.contains("apiHash")) {
		result.apiHash = object.value("apiHash").toString();
	}
	return result;
}

} // namespace Settings
