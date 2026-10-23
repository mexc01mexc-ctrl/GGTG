/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "settings/bulk_accounts_actions.h"
#include "settings/sms_campaign.h"

#include "api/api_authorizations.h"
#include "api/api_cloud_password.h"
#include "api/api_peer_photo.h"
#include "apiwrap.h"
#include "base/call_delayed.h"
#include "base/random.h"
#include "base/weak_ptr.h"
#include "boxes/connection_box.h" // MakeAccountProxyBox, for the "click the Прокси cell to edit it" flow
#include "core/application.h"
#include "core/file_utilities.h"
#include "core/process_manager.h"
#include "data/data_photo.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "data/account_spam_status.h"
#include "data/scraped_members_db.h"
#include "lang/lang_keys.h"
#include "main/main_account.h"
#include "main/main_domain.h"
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
#include "ui/widgets/checkbox.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/fields/password_input.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/scroll_area.h"

#include <array>

#include <QtCore/QLocale>
#include <QtWidgets/QScrollBar> // horizontal-scroll sync between the account manager's table header and its rows
#include "ui/wrap/vertical_layout.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"
#include "window/window_session_controller_link_info.h"
#include "boxes/abstract_box.h" // Ui::show().
#include "styles/style_boxes.h"
#include "styles/palette.h"
#include "styles/style_layers.h"

#include <QtCore/QDateTime>
#include <QtCore/QTimer>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QRegularExpression>
#include "ui/widgets/separate_panel.h"
#include "core/ggtg_log.h"
#include "window/ggtg_click.h"
#include "window/ggtg_widgets.h"
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>
#include <QtGui/QMouseEvent>

namespace Settings {
namespace {

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
	if (!account->sessionExists()) {
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
	if (!account->sessionExists()) {
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

	if (account->sessionExists()) {
		const auto view = row->lifetime().make_state<Ui::PeerUserpicView>();
		const auto userpic = Ui::CreateChild<Ui::RpWidget>(row);
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

	// Name/@username/phone as three fixed-position columns, not one
	// flowing "Name · @username · phone" string - a row with no username
	// (common on freshly-added accounts) used to pull its phone number
	// straight up against the name, so the phone column visibly jumped
	// left/right from row to row instead of lining up in a straight
	// column down the list ("horizontally uneven", as reported).
	const auto nameText = account->sessionExists()
		? account->session().user()->name()
		: u"(нет сессии)"_q;
	const auto usernameText = account->sessionExists()
		? account->session().user()->username()
		: QString();
	const auto phoneText = account->sessionExists()
		? account->session().user()->phone()
		: QString();
	const auto nameColumnWidth = style::ConvertScale(132);
	const auto usernameColumnWidth = style::ConvertScale(118);
	const auto columnGap = style::ConvertScale(8);
	const auto usernameLeft = textLeft + nameColumnWidth;
	const auto phoneLeft = usernameLeft + usernameColumnWidth;
	const auto nameLabel = Ui::CreateChild<Ui::FlatLabel>(
		row,
		st::boxLabel.style.font->elided(
			nameText.isEmpty() ? u"Аккаунт"_q : nameText,
			nameColumnWidth - columnGap),
		st::boxLabel);
	nameLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
	nameLabel->moveToLeft(textLeft, (row->height() - nameLabel->height()) / 2);
	const auto usernameLabel = Ui::CreateChild<Ui::FlatLabel>(
		row,
		usernameText.isEmpty()
			? QString()
			: st::boxLabel.style.font->elided(
				u"@"_q + usernameText, usernameColumnWidth - columnGap),
		st::boxLabel);
	usernameLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
	usernameLabel->moveToLeft(
		usernameLeft, (row->height() - usernameLabel->height()) / 2);
	const auto phoneLabel = Ui::CreateChild<Ui::FlatLabel>(
		row,
		phoneText,
		st::boxLabel);
	phoneLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
	phoneLabel->moveToLeft(phoneLeft, (row->height() - phoneLabel->height()) / 2);
	row->widthValue(
	) | rpl::on_next([=](int width) {
		// Elide (not wrap) so a long phone/username can never grow the
		// row past its fixed single-line height.
		const auto available = std::max(width - phoneLeft - columnGap, 0);
		phoneLabel->setText(
			st::boxLabel.style.font->elided(phoneText, available));
		phoneLabel->moveToLeft(
			phoneLeft, (row->height() - phoneLabel->height()) / 2, width);
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

not_null<Ui::RpWidget*> MakeFolderIcon(
		not_null<Ui::RpWidget*> parent,
		const QString &folderName,
		int size) {
	const auto icon = Ui::CreateChild<Ui::RpWidget>(parent.get());
	icon->setAttribute(Qt::WA_TransparentForMouseEvents);
	icon->resize(size, size);
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
		not_null<Window::SessionController*> controller,
		std::shared_ptr<BulkRunState> state) {
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setWidth(st::accountsBoxWidth);
		box->setTitle(rpl::single(u"Массовое действие"_q));
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
		Step step) {
	Expects(!accounts.empty());

	const auto state = std::make_shared<BulkRunState>();
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
		u"Массовое действие (%1)"_q.arg(state->total),
		[=] {
			if (const auto alive = weakController.get()) {
				OpenBulkActions(alive);
			}
		});

	BuildBulkProgressBox(controller, state);

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
		setRowText(
			i,
			AccountLabel(account) + u" — выполняется..."_q,
			std::nullopt);
		step(account, [=](bool success, QString message) {
			setRowText(
				i,
				AccountLabel(account) + u" — "_q + (success
					? (u"успешно"_q + (message.isEmpty()
						? QString()
						: (u" ("_q + message + ')')))
					: (u"ошибка: "_q + (message.isEmpty()
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
			const auto remaining = std::make_shared<int>(
				int(NextBulkPause() / 1000));
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

// ---- Step implementations (one account, MTP request(s), then done()) ----

void StepUpdateProfile(
		not_null<Main::Account*> account,
		std::optional<QString> firstName,
		std::optional<QString> lastName,
		std::optional<QString> about,
		StepDone done) {
	if (!account->sessionExists()) {
		done(false, u"нет активной сессии"_q);
		return;
	}
	done = GuardedDone(done).done;
	using Flag = MTPaccount_UpdateProfile::Flag;
	auto flags = MTPaccount_UpdateProfile::Flags(0);
	if (firstName) flags |= Flag::f_first_name;
	if (lastName) flags |= Flag::f_last_name;
	if (about) flags |= Flag::f_about;
	const auto api = std::make_shared<MTP::Sender>(&account->session().mtp());
	const auto session = &account->session();
	api->request(MTPaccount_UpdateProfile(
		MTP_flags(flags),
		MTP_string(firstName.value_or(QString())),
		MTP_string(lastName.value_or(QString())),
		MTP_string(about.value_or(QString()))
	// api is otherwise unused in these bodies, so [=] alone wouldn't
	// capture it - it would be destroyed (along with the Sender that the
	// request's internal handler keeps a raw pointer to) the instant this
	// function returns, before the async reply ever arrives.
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
	if (!account->sessionExists()) {
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
	const auto api = std::make_shared<MTP::Sender>(&session->mtp());
	// See the comment on the equivalent api = api captures in
	// StepUpdateProfile above - same dangling-Sender risk here.
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
	if (!account->sessionExists()) {
		done(false, u"нет активной сессии"_q);
		return;
	}
	done = GuardedDone(done).done;
	const auto session = &account->session();
	const auto api = std::make_shared<MTP::Sender>(&session->mtp());
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
	if (!account->sessionExists()) {
		done(false, u"нет активной сессии"_q);
		return;
	}
	done = GuardedDone(done).done;
	// No hash = every session but this one (mirrors the "Terminate all
	// other sessions" button in Settings > Privacy > Active Sessions).
	account->session().api().authorizations().requestTerminate(
		[=](const MTPBool &) {
			done(true, QString());
		},
		[=](const MTP::Error &error) {
			done(false, error.type());
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
	const auto api = std::make_shared<MTP::Sender>(&session->mtp());
	// See the comment on the equivalent api = api captures in
	// StepUpdateProfile above - same dangling-Sender risk here.
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
	if (!account->sessionExists()) {
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
	if (!account->sessionExists()) {
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
			u"Просто добавить новое"_q));
		box->addRow(object_ptr<Ui::Radioenum<Mode>>(
			box,
			state->mode,
			Mode::Replace,
			u"Удалить старое и добавить новое"_q));
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
				box->uiShow()->showToast(u"Сначала выберите файл"_q);
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
		box->addButton(rpl::single(u"Назад"_q), [=] { box->closeBox(); });
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
			u"Одно имя и фамилия для всех"_q));
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
				"в порядке отображения ниже"_q));
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
					box->uiShow()->showToast(u"Введите имя"_q);
					return;
				}
				names.assign(accounts.size(), { first, last });
			} else {
				const auto lines = state->list->getLastText().split('\n');
				if (int(lines.size()) != int(accounts.size())) {
					box->uiShow()->showToast(
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
		box->addButton(rpl::single(u"Назад"_q), [=] { box->closeBox(); });
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
			box, state->mode, Mode::Delete, u"Удалить описание"_q));
		box->addRow(object_ptr<Ui::Radioenum<Mode>>(
			box, state->mode, Mode::SameForAll, u"Один текст для всех"_q));
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
				"отображения ниже"_q));
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
					box->uiShow()->showToast(
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
		box->addButton(rpl::single(u"Назад"_q), [=] { box->closeBox(); });
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
				"буква фамилии + id аккаунта - он у каждого свой)"_q));
		box->addRow(object_ptr<Ui::Radioenum<Mode>>(
			box, mode, Mode::Delete, u"Удалить username"_q));
		box->addButton(rpl::single(u"Начать"_q), [=] {
			const auto remove = (mode->current() == Mode::Delete);
			box->closeBox();
			RunBulkAction(controller, accounts, [=](
					not_null<Main::Account*> account,
					StepDone done) {
				StepSetUsername(account, remove, done);
			});
		});
		box->addButton(rpl::single(u"Назад"_q), [=] { box->closeBox(); });
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
			controller->uiShow()->showToast(
				u"Перемещено аккаунтов: "_q
					+ QString::number(accounts.size()));
		};
		field->submits() | rpl::on_next(save, field->lifetime());
		box->addButton(rpl::single(u"Сохранить"_q), save);
		box->addButton(rpl::single(u"Назад"_q), [=] { box->closeBox(); });
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
					"этот аккаунт здесь, в NiceGram, останется подключён."_q));
		box->addButton(rpl::single(u"Деактивировать"_q), [=] {
			box->closeBox();
			RunBulkAction(controller, accounts, [=](
					not_null<Main::Account*> account,
					StepDone done) {
				StepTerminateOtherSessions(account, done);
			});
		});
		box->addButton(rpl::single(u"Назад"_q), [=] { box->closeBox(); });
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
			u"Установить/сменить пароль"_q));
		state->password = AddPasswordRow(box, rpl::single(u"Новый пароль"_q));
		state->hint = box->addRow(object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			rpl::single(u"Подсказка (необязательно)"_q)));
		box->addRow(object_ptr<Ui::Radioenum<Mode>>(
			box,
			state->mode,
			Mode::Delete,
			u"Удалить пароль (выключить 2FA)"_q));
		box->addButton(rpl::single(u"Начать"_q), [=] {
			const auto deleting = (state->mode->current() == Mode::Delete);
			const auto newPassword = deleting
				? QString()
				: state->password->getLastText();
			if (!deleting && newPassword.isEmpty()) {
				box->uiShow()->showToast(u"Введите новый пароль"_q);
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
		box->addButton(rpl::single(u"Назад"_q), [=] { box->closeBox(); });
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

[[nodiscard]] QStringList ShuffledCopy(QStringList list) {
	for (auto i = int(list.size()) - 1; i > 0; --i) {
		std::swap(list[i], list[base::RandomIndex(i + 1)]);
	}
	return list;
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
					box->addRow(object_ptr<Ui::Checkbox>(box, label, false)),
				});
			} else {
				const auto id = meta.id;
				box->addRow(object_ptr<Ui::SettingsButton>(
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
					box->uiShow()->showToast(
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
				box->uiShow()->showToast(u"Введите число больше нуля"_q);
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
void OpenUsernamePeer(const QString &rawUsername) {
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
	const auto controller = Core::App().activeWindow()
		? Core::App().activeWindow()->sessionController()
		: nullptr;
	if (!controller) {
		return;
	}
	controller->showPeerByLink(Window::PeerByLinkInfo{
		.usernameOrId = username,
		.resolveType = Window::ResolveType::Mention,
	});
}

[[nodiscard]] QString SentStatusText(Data::SmsSendStatus status) {
	switch (status) {
	case Data::SmsSendStatus::SpamAcc: return u"SPAM_ACC"_q;
	case Data::SmsSendStatus::NotFound: return u"не найден"_q;
	case Data::SmsSendStatus::Restricted: return u"нельзя писать"_q;
	case Data::SmsSendStatus::Sent: break;
	}
	return u"OK"_q;
}

// A read-only, click-to-open view of a base's usernames - the bulk editor
// just below it is a plain text field (by design: fast paste/edit of
// hundreds of lines at once), which can't carry a per-line click handler,
// so this is a separate list instead of a mode of that same field. Capped
// at the same size as the editor itself (see kEditorLimit) - a box with
// thousands of individually-clickable rows is real construction cost for
// something a search would find faster anyway.
void ShowClickableUsernamesBox(
		std::shared_ptr<Ui::Show> show,
		const QString &title,
		const QStringList &usernames,
		Fn<void(const QString &username)> remove = nullptr,
		Fn<QString(const QString &username)> extraMark = nullptr,
		Fn<QString(const QString &username)> displayName = nullptr,
		Fn<void(const QString &username)> download = nullptr,
		Fn<void(const QString &username)> viaSquidward = nullptr,
		Fn<bool(const QString &username)> isClosed = nullptr) {
	show->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setWidth(st::accountsBoxWidth);
		box->setTitle(rpl::single(title));
		if (usernames.isEmpty()) {
			box->addRow(object_ptr<Ui::FlatLabel>(
				box, u"Список пуст."_q, st::boxLabel));
		} else if (int(usernames.size()) > kEditorLimit) {
			box->addRow(object_ptr<Ui::FlatLabel>(
				box,
				u"Список слишком большой для этого просмотра ("_q
					+ QString::number(usernames.size())
					+ u" юзернеймов)."_q,
				st::boxLabel));
		} else {
			for (const auto &username : usernames) {
				// Per-user delivery status from the shared registry,
				// refreshed live: writing to someone (a campaign, or by hand
				// in a chat) puts the check next to them right here.
				const auto labelFor = [=] {
					auto label = displayName
						? displayName(username)
						: (username.startsWith('@')
							? username
							: ('@' + username));
					if (extraMark) {
						label += extraMark(username);
					} else if (const auto record
							= Data::FindSmsSendRecord(username)) {
						label += (record->status == Data::SmsSendStatus::Sent)
							? u"  ✓ OK"_q
							: (u"  — "_q + SentStatusText(record->status));
					}
					return label;
				};
				// One row = the username button + (for a base that can be
				// edited) a "✕" that drops the user from the base at once.
				auto wrapOwned = object_ptr<Ui::RpWidget>(box);
				const auto wrap = wrapOwned.data();
				const auto button = Ui::CreateChild<Ui::SettingsButton>(
					wrap,
					rpl::single(labelFor()) | rpl::then(
						Data::BasesChanged() | rpl::map(labelFor)));
				const auto cross = remove
					? Ui::CreateChild<Ggtg::RemoveButton>(wrap)
					: nullptr;
				// Left of the "✕": download the group's members, and the
				// same through Сквидвард for a group that hides them.
				const auto squid = viaSquidward
					? Ui::CreateChild<Ggtg::GlyphButton>(
						wrap,
						u"🦑"_q,
						u"Собрать участников через Сквидвард"_q)
					: nullptr;
				// A closed group can't be read directly - no ⬇ for it.
				const auto closed = isClosed && isClosed(username);
				const auto load = (download && !closed)
					? Ui::CreateChild<Ggtg::GlyphButton>(
						wrap,
						u"⬇"_q,
						u"Скачать участников группы"_q)
					: nullptr;
				// A fixed height: a SettingsButton has none of its own yet
				// at this point.
				const auto rowHeight = style::ConvertScale(44);
				wrap->resize(wrap->width(), rowHeight);
				wrap->widthValue(
				) | rpl::on_next([=](int width) {
					auto right = width;
					const auto widgets = std::array<QWidget*, 3>{
						cross, squid, load };
					for (const auto widget : widgets) {
						if (!widget) {
							continue;
						}
						right -= widget->width();
						widget->move(
							right,
							(rowHeight - widget->height()) / 2);
					}
					button->setGeometry(0, 0, std::max(right, 0), rowHeight);
					wrap->resize(width, rowHeight);
				}, wrap->lifetime());
				if (load) {
					load->setClickedCallback([=] { download(username); });
				}
				if (squid) {
					squid->setClickedCallback([=] { viaSquidward(username); });
				}
				if (cross) {
					cross->setClickedCallback([=] {
						remove(username);
						box->uiShow()->showToast(
							u"@"_q + username.trimmed().replace('@', QString())
								+ u" удалён из базы"_q);
						wrap->deleteLater();
					});
				}
				box->addRow(std::move(wrapOwned)
				);
				button->setClickedCallback([=] {
					if (closed) {
						// Nothing to open by name - Сквидвард is the only way.
						if (viaSquidward) {
							viaSquidward(username);
						}
						return;
					}
					// The chat opens in the main window; this list stays open
					// where it is (main-window navigation no longer closes the
					// GGTG window's dialogs, see MainWindow::
					// ui_hideSettingsAndLayer). This window is raised over the
					// chat once opening it has settled - it keeps activating
					// the main window for a while.
					const auto window = QPointer<QWidget>(box->window());
					OpenUsernamePeer(username);
					QTimer::singleShot(1500, [=] {
						const auto strong = window.data();
						if (const auto panel
							= dynamic_cast<Ui::SeparatePanel*>(strong)) {
							panel->showAndActivate();
						} else if (strong) {
							strong->raise();
							strong->activateWindow();
						}
					});
				});
			}
		}
		box->addButton(rpl::single(u"Закрыть"_q), [=] { box->closeBox(); });
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
				return u"Юзернеймов: "_q + QString::number(stats.total)
					+ u". Осталось написать: "_q
					+ QString::number(stats.remaining)
					+ u"\nOK (уже написано): "_q + QString::number(stats.sent)
					+ u"\nSPAM_ACC: "_q + QString::number(stats.spamAcc)
					+ u", не найдено: "_q + QString::number(stats.notFound)
					+ u", нельзя писать: "_q
					+ QString::number(stats.restricted)
					+ u", боты: "_q + QString::number(stats.bots);
			};
			const auto statsLabel = box->addRow(object_ptr<Ui::FlatLabel>(
				box,
				describe(),
				st::boxLabel));
			Data::BasesChanged(
			) | rpl::on_next([=] {
				statsLabel->setText(describe());
			}, box->lifetime());
			box->addRow(object_ptr<Ui::SettingsButton>(
				box, rpl::single(u"👁 Открыть списком (кликабельно)"_q))
			)->setClickedCallback([=] {
				// "✕" in that list removes a user from the stored base right
				// away (no confirmation, no need to have written to them) and
				// from this box's own editor text, so a later "Сохранить"
				// here can't bring them back.
				const auto removeUser = [=](const QString &username) {
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
					removeUser);
			});
			if (actions.sendToBase) {
				box->addRow(object_ptr<Ui::SettingsButton>(
					box, rpl::single(u"Отправить SMS по этой базе"_q))
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
						"выше не учитываются). Всё, кроме \"Убрать "
						"ботов\", создаёт новую базу и ничего не меняет "
						"в этой."_q));
			const auto reopen = [=] {
				box->closeBox();
			};
			const auto createBase = [=](
					const QString &newTitle,
					const QStringList &list) {
				if (list.isEmpty()) {
					show->showToast(
						u"Результат пустой - база не создана"_q);
					return false;
				}
				Data::SaveScrapedGroupUsernames(
					Data::GenerateManualBaseId(),
					newTitle,
					list);
				return true;
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
							show->showToast(u"Не удалось записать файл"_q);
							return;
						}
						const auto csv = path.endsWith(
							u".csv"_q,
							Qt::CaseInsensitive);
						auto text = csv ? u"username\n"_q : QString();
						text += usernames.join('\n');
						text += '\n';
						file.write(text.toUtf8());
						show->showToast(u"Экспортировано: "_q
							+ QString::number(usernames.size()));
					});
			});
			addOp(u"Дублировать"_q, [=] {
				if (createBase(title + u" (копия)"_q, usernames)) {
					show->showToast(u"Создана копия"_q);
					reopen();
				}
			});
			addOp(u"Убрать ботов (username на \"bot\")"_q, [=] {
				auto kept = QStringList();
				for (const auto &username : usernames) {
					if (!Data::IsBotUsername(username)) {
						kept.push_back(username);
					}
				}
				const auto removed = int(usernames.size() - kept.size());
				if (!removed) {
					show->showToast(u"Ботов в списке нет"_q);
					return;
				}
				show->show(Ui::MakeConfirmBox({
					.text = u"Убрать из списка ботов: "_q
						+ QString::number(removed)
						+ u"? Список изменится насовсем."_q,
					.confirmed = [=](Fn<void()> &&close) {
						close();
						Data::SaveScrapedGroupUsernames(id, title, kept);
						show->showToast(u"Убрано ботов: "_q
							+ QString::number(removed));
						reopen();
					},
					.confirmText = u"Убрать"_q,
				}));
			});
			addOp(u"Вычесть другую базу -> новая..."_q, [=] {
				ShowBasePicker(
					show,
					u"Вычесть базу"_q,
					id,
					false,
					[=](std::vector<uint64> ids) {
						auto other = base::flat_set<QString>();
						for (const auto &username
								: Data::LoadScrapedGroupUsernames(ids.front())) {
							other.emplace(username.toLower());
						}
						auto result = QStringList();
						for (const auto &username : usernames) {
							if (!other.contains(username.toLower())) {
								result.push_back(username);
							}
						}
						const auto removed = int(
							usernames.size() - result.size());
						if (createBase(
								title + u" без "_q
									+ BaseTitleById(ids.front()),
								result)) {
							show->showToast(u"Создана база: "_q
								+ QString::number(result.size())
								+ u" (вычтено "_q + QString::number(removed)
								+ ')');
							reopen();
						}
					});
			});
			addOp(u"Объединить с другими базами -> новая..."_q, [=] {
				ShowBasePicker(
					show,
					u"Объединить с..."_q,
					id,
					true,
					[=](std::vector<uint64> ids) {
						auto merged = usernames;
						for (const auto other : ids) {
							merged.append(
								Data::LoadScrapedGroupUsernames(other));
						}
						if (createBase(
								u"Объединение: "_q + title + u" + "_q
									+ QString::number(ids.size()),
								merged)) {
							show->showToast(u"Создана объединённая база"_q);
							reopen();
						}
					});
			});
			addOp(u"Разбить на части..."_q, [=] {
				ShowNumberPrompt(
					show,
					u"Разбить на части"_q,
					u"На сколько частей"_q,
					u"2"_q,
					[=](int parts) {
						const auto total = int(usernames.size());
						if (parts < 2 || parts > total) {
							show->showToast(
								u"Частей должно быть от 2 до "_q
									+ QString::number(total));
							return;
						}
						const auto size = total / parts;
						const auto extra = total % parts;
						auto from = 0;
						for (auto i = 0; i != parts; ++i) {
							const auto length = size + (i < extra ? 1 : 0);
							Data::SaveScrapedGroupUsernames(
								Data::GenerateManualBaseId(),
								title + u" (часть %1/%2)"_q
									.arg(i + 1)
									.arg(parts),
								usernames.mid(from, length));
							from += length;
						}
						show->showToast(u"Создано баз: "_q
							+ QString::number(parts));
						reopen();
					});
			});
			addOp(u"Случайная выборка -> новая..."_q, [=] {
				ShowNumberPrompt(
					show,
					u"Случайная выборка"_q,
					u"Сколько юзернеймов взять"_q,
					QString::number(std::min(100, int(usernames.size()))),
					[=](int count) {
						if (count >= int(usernames.size())) {
							show->showToast(
								u"Выборка должна быть меньше списка ("_q
									+ QString::number(usernames.size())
									+ ')');
							return;
						}
						if (createBase(
								title + u" (выборка %1)"_q.arg(count),
								ShuffledCopy(usernames).mid(0, count))) {
							show->showToast(u"Создана база-выборка"_q);
							reopen();
						}
					});
			});
		}
		box->setFocusCallback([=] { state->title->setFocusFast(); });
		box->addButton(rpl::single(u"Сохранить"_q), [=] {
			const auto newTitle = state->title->getLastText().trimmed();
			if (newTitle.isEmpty()) {
				box->uiShow()->showToast(u"Введите название"_q);
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
					show->showToast(
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
		box->addButton(rpl::single(u"Назад"_q), [=] { box->closeBox(); });
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
				box->addRow(object_ptr<Ui::SettingsButton>(
					box, rpl::single(u"👁 Открыть списком (кликабельно)"_q))
				)->setClickedCallback([=] {
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
						const auto name = groupTitle(username);
						if (isClosed(username)) {
							// 🔒: closed group, only Сквидвард can open it.
							return u"🔒 "_q + (name.isEmpty()
								? username.trimmed()
								: name);
						}
						const auto handle = '@' + bare(username);
						auto result = (name.isEmpty()
							|| bare(name) == bare(username))
							? handle
							: (name + u" ("_q + handle + ')');
						// Size of the group, as seen when it was found.
						const auto it = members->find(bare(username));
						if (it != members->end() && it->second > 0) {
							result += u"  ·  "_q
								+ QLocale().toString(it->second)
								+ u" уч."_q;
						}
						return result;
					};
					const auto removeGroup = [=](const QString &username) {
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
			const auto newTitle = state->title->getLastText().trimmed();
			if (newTitle.isEmpty()) {
				box->uiShow()->showToast(u"Введите название"_q);
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
		box->addButton(rpl::single(u"Назад"_q), [=] { box->closeBox(); });
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
					+ u"\nАккаунт: "_q + record->account
					+ u"\nКогда: "_q + FormatScrapedAt(record->at),
				st::boxLabel));
		}
		box->addRow(object_ptr<Ui::SettingsButton>(
			box, rpl::single(u"Открыть чат"_q))
		)->setClickedCallback([=] {
			OpenUsernamePeer(username);
		});
		box->addRow(object_ptr<Ui::SettingsButton>(
			box, rpl::single(u"Копировать @username"_q))
		)->setClickedCallback([=] {
			QGuiApplication::clipboard()->setText('@' + username);
			box->uiShow()->showToast(u"Скопировано"_q);
		});
		box->addRow(object_ptr<Ui::SettingsButton>(
			box, rpl::single(u"Сбросить статус (можно писать снова)"_q))
		)->setClickedCallback([=] {
			box->uiShow()->show(Ui::MakeConfirmBox({
				.text = u"Сбросить статус @"_q + username
					+ u"? Рассылка снова сможет ему написать."_q,
				.confirmed = [=](Fn<void()> &&close) {
					close();
					Data::DeleteSmsSendRecord(username);
					box->uiShow()->showToast(
						u"Статус @"_q + username + u" сброшен"_q);
					(*refresh)();
					box->closeBox();
				},
				.confirmText = u"Сбросить"_q,
			}));
		});
		box->addButton(rpl::single(u"Назад"_q), [=] { box->closeBox(); });
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
			case 2: return u"Фильтр: SPAM_ACC"_q;
			case 3: return u"Фильтр: не найден"_q;
			case 4: return u"Фильтр: нельзя писать"_q;
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
						&& record.status == Data::SmsSendStatus::Restricted);
				if (!wanted) {
					continue;
				}
				if (!query.isEmpty()
					&& !record.username.contains(query)
					&& !record.account.toLower().contains(query)) {
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
				rows->add(object_ptr<Ui::SettingsButton>(
					rows,
					rpl::single('@' + record.username + u" — "_q
						+ SentStatusText(record.status)
						+ u" ("_q + record.account + u", "_q
						+ FormatScrapedAt(record.at) + u')'))
				)->setClickedCallback([=] {
					ShowSentRecordActionsBox(show, username, refresh);
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
			rpl::single(u"Поиск по @username или аккаунту"_q)));
		search->changes(
		) | rpl::on_next([=] {
			state->query = search->getLastText();
			state->shown = kSentPageSize;
			(*refresh)();
		}, search->lifetime());
		box->addRow(object_ptr<Ui::SettingsButton>(
			box, filterText->value())
		)->setClickedCallback([=] {
			state->filter = (state->filter + 1) % 5;
			state->shown = kSentPageSize;
			*filterText = filterLabel(state->filter);
			(*refresh)();
		});
		Ui::AddSkip(box->verticalLayout());
		Ui::AddDivider(box->verticalLayout());
		Ui::AddSkip(box->verticalLayout());

		state->rows = AddScrollableRowsArea(box->verticalLayout());
		fill();

		box->addButton(rpl::single(u"Закрыть"_q), [=] { box->closeBox(); });
	});
}

object_ptr<Ui::BoxContent> MakeScrapedGroupsDbBox(
		std::shared_ptr<Ui::Show> show,
		BasesActions actions) {
	return Box([=](not_null<Ui::GenericBox*> box) {
		box->setWidth(st::accountsBoxWidth);
		box->setTitle(rpl::single(u"База"_q));

		const auto spamAccCount = Data::CountSmsResettable();
		if (spamAccCount > 0) {
			box->addRow(object_ptr<Ui::SettingsButton>(
				box,
				rpl::single(u"Сбросить SPAM_ACC / не найденных ("_q
					+ QString::number(spamAccCount)
					+ ')'))
			)->setClickedCallback([=] {
				Data::ClearAllSmsResettable();
				box->uiShow()->showToast(
					u"Статусы SPAM_ACC / не найден / нельзя писать сброшены"_q);
				// The "База" box underneath refreshes itself on BasesChanged - no
				// need to open a second copy (which, inside the GGTG window,
				// stacked up instead of replacing it).
			});
			Ui::AddSkip(box->verticalLayout());
		}

		box->addRow(object_ptr<Ggtg::ActionRow>(
			box,
			rpl::single(u"Отправленные ("_q
				+ QString::number(Data::CountSmsSendRecords())
				+ ')')
				| rpl::then(Data::BasesChanged() | rpl::map([] {
					return u"Отправленные ("_q
						+ QString::number(Data::CountSmsSendRecords())
						+ ')';
				})),
			u"✓"_q)
		)->setClickedCallback([=] {
			show->show(MakeSmsSentUsersBox(show));
		});
		Ui::AddSkip(box->verticalLayout());

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

			for (const auto &row : groups) {
				const auto meta = row.meta;
				groupsBox->add(object_ptr<Ggtg::InfoRow>(
					groupsBox,
					rpl::single(Ggtg::InfoRowData{
						.title = meta.title,
						.subtitle = u"Сохранено "_q
							+ FormatScrapedAt(meta.savedAt),
						.chip = QString::number(meta.count) + u" групп"_q,
					})),
					st::boxRowPadding
				)->setClickedCallback([=] {
					const auto entries = Data::LoadScrapedGroupEntries(meta.id);
					ShowEditGroupsBaseBox(
						show, actions, meta.id, meta.title, entries, false);
				});
			}
			if (groups.empty()) {
				groupsBox->add(object_ptr<Ggtg::EmptyState>(
					groupsBox,
					query.isEmpty()
						? u"Групп пока нет. Добавьте список через "
							"«Новый список групп» выше."_q
						: u"Ничего не найдено."_q));
			}

			for (const auto &row : users) {
				const auto meta = row.meta;
				// "Осталось N из M" straight in the list - the same numbers
				// "Отправка SMS" shows for this base, refreshed live as a
				// campaign writes to the registry.
				const auto rowData = [=] {
					const auto stats = Data::ComputeBaseStats(meta.id);
					return Ggtg::InfoRowData{
						.title = meta.title,
						.subtitle = u"Всего "_q + QString::number(stats.total)
							+ u", написано "_q + QString::number(stats.sent)
							+ u" · "_q + FormatScrapedAt(meta.savedAt),
						.chip = (stats.remaining > 0)
							? (u"осталось "_q
								+ QString::number(stats.remaining))
							: u"все написано"_q,
						.chipGood = (stats.remaining == 0),
					};
				};
				usersBox->add(object_ptr<Ggtg::InfoRow>(
					usersBox,
					rpl::single(rowData()) | rpl::then(
						Data::BasesChanged() | rpl::map([=] {
							return rowData();
						}))),
					st::boxRowPadding
				)->setClickedCallback([=] {
					const auto usernames
						= Data::LoadScrapedGroupUsernames(meta.id);
					ShowEditBaseBox(
						show, actions, meta.id, meta.title, usernames, false);
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
		box->addRow(object_ptr<Ggtg::ActionRow>(
			box, sortText->value(), u"≡"_q)
		)->setClickedCallback([=] {
			BaseSortMode() = (BaseSortMode() + 1) % 4;
			*sortText = sortLabel(BaseSortMode());
			fill();
		});
		Ui::AddSkip(box->verticalLayout());

		Ui::AddSubsectionTitle(box->verticalLayout(), rpl::single(u"Группы"_q));
		box->addRow(object_ptr<Ggtg::ActionRow>(
			box, rpl::single(u"Новый список групп"_q), u"+"_q)
		)->setClickedCallback([=] {
			ShowEditGroupsBaseBox(
				show,
				actions,
				Data::GenerateManualBaseId(),
				QString(),
				{},
				true);
		});
		if (actions.searchGroups) {
			box->addRow(object_ptr<Ggtg::ActionRow>(
				box, rpl::single(u"Поиск групп по ключевому слову..."_q), u"⌕"_q)
			)->setClickedCallback([=] {
				actions.searchGroups();
			});
		}
		list->groups = box->addRow(object_ptr<Ui::VerticalLayout>(box));

		Ui::AddSkip(box->verticalLayout());
		Ui::AddDivider(box->verticalLayout());
		Ui::AddSkip(box->verticalLayout());

		Ui::AddSubsectionTitle(
			box->verticalLayout(), rpl::single(u"Пользователи"_q));
		box->addRow(object_ptr<Ggtg::ActionRow>(
			box, rpl::single(u"Новый список пользователей"_q), u"+"_q)
		)->setClickedCallback([=] {
			ShowEditBaseBox(
				show,
				actions,
				Data::GenerateManualBaseId(),
				QString(),
				QStringList(),
				true);
		});
		box->addRow(object_ptr<Ggtg::ActionRow>(
			box, rpl::single(u"Импорт из файла (txt / csv)..."_q), u"↓"_q)
		)->setClickedCallback([=] {
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
						show->showToast(u"Не удалось прочитать файл"_q);
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
						show->showToast(
							u"В файле не найдено ни одного юзернейма"_q);
						return;
					}
					Data::SaveScrapedGroupUsernames(
						Data::GenerateManualBaseId(),
						QFileInfo(path).completeBaseName(),
						parsed.valid);
					show->showToast(
						u"Импортировано: "_q + QString::number(parsed.valid.size())
							+ u", отклонено: "_q
							+ QString::number(parsed.rejected.size())
							+ u", дубликатов: "_q
							+ QString::number(parsed.duplicates));
				});
		});
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

		box->addButton(rpl::single(u"Закрыть"_q), [=] { box->closeBox(); });
	});
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

		box->addButton(rpl::single(u"Назад"_q), [=] { box->closeBox(); });
	});
}

object_ptr<Ui::BoxContent> MakeBulkFolderAccountsBox(
		not_null<Window::SessionController*> controller,
		QString folderTag,
		Accounts accounts) {
	return Box([=](not_null<Ui::GenericBox*> box) {
		box->setWidth(st::accountsBoxWidth);
		box->setTitle(rpl::single(
			folderTag.isEmpty() ? u"Без папки"_q : folderTag));

		Ui::AddSkip(box->verticalLayout());
		Ui::AddSubsectionTitle(
			box->verticalLayout(),
			rpl::single(u"Аккаунты ("_q + QString::number(accounts.size()) + ')'));
		struct Row {
			not_null<Main::Account*> account;
			not_null<Ui::Checkbox*> checkbox;
		};
		const auto rows = std::make_shared<std::vector<Row>>();
		// One toggling button whose label and action come from the rows'
		// real state (not a separate flag that drifts once someone ticks
		// rows by hand): all ticked -> "Отменить всех", otherwise
		// "Выбрать всех".
		const auto allTicked = [=] {
			return !rows->empty() && ranges::all_of(*rows, [](const Row &row) {
				return row.checkbox->checked();
			});
		};
		const auto toggleText = std::make_shared<rpl::variable<QString>>(
			u"Выбрать всех"_q);
		const auto syncToggleText = [=] {
			*toggleText = allTicked()
				? u"Отменить всех"_q
				: u"Выбрать всех"_q;
		};
		box->addRow(object_ptr<Ui::SettingsButton>(
			box, toggleText->value())
		)->setClickedCallback([=] {
			const auto target = !allTicked();
			for (const auto &row : *rows) {
				row.checkbox->setChecked(target);
			}
			syncToggleText();
		});
		for (const auto &account : accounts) {
			const auto checkbox = AddAccountPickerRow(
				box->verticalLayout(),
				account);
			rows->push_back({ account, checkbox });
			checkbox->checkedChanges(
			) | rpl::on_next([=](bool) {
				syncToggleText();
			}, checkbox->lifetime());
		}
		Ui::AddSkip(box->verticalLayout());

		box->addButton(rpl::single(u"Далее"_q), [=] {
			auto chosen = Accounts();
			for (const auto &row : *rows) {
				if (row.checkbox->checked()) {
					chosen.push_back(row.account);
				}
			}
			if (chosen.empty()) {
				box->uiShow()->showToast(u"Выберите хотя бы один аккаунт"_q);
				return;
			}
			box->closeBox();
			controller->show(
				MakeBulkActionsChooserBox(controller, std::move(chosen)));
		});
		box->addButton(rpl::single(u"Назад"_q), [=] { box->closeBox(); });
	});
}

object_ptr<Ui::BoxContent> MakeBulkFoldersBox(
		not_null<Window::SessionController*> controller) {
	return Box([=](not_null<Ui::GenericBox*> box) {
		box->setWidth(st::accountsBoxWidth);
		box->setTitle(rpl::single(u"Массовые действия"_q));

		auto grouped = base::flat_map<QString, Accounts>();
		for (const auto &account : Core::App().domain().orderedAccounts()) {
			grouped[account->folderTag()].push_back(account);
		}
		if (grouped.empty()) {
			box->addRow(object_ptr<Ui::FlatLabel>(
				box, u"Нет добавленных аккаунтов"_q, st::boxLabel));
		}
		for (const auto &[tag, accounts] : grouped) {
			const auto name = tag.isEmpty() ? u"Без папки"_q : tag;
			const auto folderIconSize = style::ConvertScale(28);
			const auto header = box->addRow(object_ptr<Ggtg::InfoRow>(
				box,
				rpl::single(Ggtg::InfoRowData{
					.title = name,
					.subtitle = QString::number(accounts.size())
						+ u" аккаунт(ов)"_q,
				}),
				folderIconSize + style::ConvertScale(14)));
			header->setClickedCallback([=] {
				controller->show(MakeBulkFolderAccountsBox(
					controller,
					tag,
					accounts));
			});
			const auto icon = MakeFolderIcon(header, name, folderIconSize);
			// header->height() isn't reliably its final value synchronously
			// right after construction - track it reactively instead of
			// reading it once (same issue documented on the account
			// switcher's own folder icon in settings_information.cpp).
			header->heightValue(
			) | rpl::on_next([=](int height) {
				icon->moveToLeft(
					style::ConvertScale(16), (height - folderIconSize) / 2);
			}, icon->lifetime());
		}

		box->addButton(rpl::single(u"Закрыть"_q), [=] { box->closeBox(); });
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

// Single-account name/username editor - opened by clicking the "Аккаунт"
// cell of one row in the manager table (see AccountTableRow below).
// Deliberately not ShowBulkNameBox/ShowBulkUsernameBox reused for one
// account: those show a "same for all / per-account list" mode picker that
// makes no sense for exactly one account, and a name field plus a separate
// username field is what a click on either part of that one cell should
// open either way.
void ShowEditAccountBox(
		std::shared_ptr<Ui::Show> show,
		not_null<Main::Account*> account) {
	if (!account->sessionExists()) {
		show->showToast(u"Нет активной сессии"_q);
		return;
	}
	const auto user = account->session().user();
	show->showBox(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"Изменить профиль"_q));
		const auto first = box->addRow(object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			rpl::single(u"Имя"_q),
			user->firstName));
		const auto last = box->addRow(object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			rpl::single(u"Фамилия"_q),
			user->lastName));
		Ui::AddSkip(box->verticalLayout());
		const auto username = box->addRow(object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			rpl::single(u"@username"_q),
			user->username()));
		box->setFocusCallback([=] { first->setFocusFast(); });
		const auto save = [=] {
			const auto firstText = first->getLastText().trimmed();
			const auto lastText = last->getLastText().trimmed();
			if (firstText.isEmpty()) {
				show->showToast(u"Введите имя"_q);
				return;
			}
			auto usernameText = username->getLastText().trimmed();
			if (usernameText.startsWith('@')) {
				usernameText = usernameText.mid(1);
			}
			const auto previousUsername = user->username();
			box->closeBox();
			if (firstText != user->firstName || lastText != user->lastName) {
				StepUpdateProfile(
					account,
					firstText,
					lastText,
					std::nullopt,
					[=](bool ok, QString error) {
						if (!ok) {
							show->showToast(
								u"Не удалось изменить имя: "_q + error);
						}
					});
			}
			if (usernameText != previousUsername) {
				StepSetUsernameTo(
					account,
					usernameText,
					[=](bool ok, QString error) {
						show->showToast(ok
							? u"Username обновлён"_q
							: (u"Не удалось изменить username: "_q
								+ error));
					});
			}
		};
		username->submits() | rpl::on_next(save, username->lifetime());
		box->addButton(rpl::single(u"Сохранить"_q), save);
		box->addButton(rpl::single(u"Отмена"_q), [=] { box->closeBox(); });
	}));
}

// ---- "Менеджер аккаунтов": a table laid out like the reference web panel
// (tg-web-panel's manager page) - folder cards with counters on top, a
// search field, a selection bar, then one row per account with the columns
// № / Фото / Аккаунт / Телефон и ID / Статус / Папка / Прокси / СМС.

// Where each column of the table sits. Every column has a fixed width
// regardless of how much room is actually available - the table never
// reflows or hides a column, it just gets wider than the visible viewport
// and the two horizontal Ui::ScrollArea wrappers around the header and the
// rows (see MakeAccountManagerBox) scroll it sideways in that case, kept in
// sync with each other so the header stays lined up with the columns below
// it. Vertical scrolling (many accounts) still goes through the box's own
// scroll area, untouched by any of this.
struct ManagerColumns {
	int check = 0;
	int number = 0;
	int photo = 0;
	int account = 0;
	int accountWidth = 0;
	int phone = 0;
	int phoneWidth = 0;
	int status = 0;
	int statusWidth = 0;
	int folder = 0;
	int folderWidth = 0;
	int proxy = 0;
	int proxyWidth = 0;
	int sms = 0;
	int smsWidth = 0;
	int totalWidth = 0;
};

[[nodiscard]] ManagerColumns ComputeManagerColumns() {
	const auto scaled = [](int value) { return style::ConvertScale(value); };
	const auto pad = scaled(12);
	const auto gap = scaled(8);
	const auto accountWidth = scaled(190);
	const auto phoneWidth = scaled(124);
	const auto statusWidth = scaled(132);
	const auto folderWidth = scaled(96);
	const auto proxyWidth = scaled(140);
	const auto smsWidth = scaled(56);

	auto result = ManagerColumns();
	auto x = pad;
	result.check = x;
	x += scaled(30) + gap;
	result.number = x;
	x += scaled(28) + gap;
	result.photo = x;
	x += scaled(40) + gap;
	result.account = x;
	result.accountWidth = accountWidth;
	x += accountWidth + gap;
	result.phone = x;
	result.phoneWidth = phoneWidth;
	x += phoneWidth + gap;
	result.status = x;
	result.statusWidth = statusWidth;
	x += statusWidth + gap;
	result.folder = x;
	result.folderWidth = folderWidth;
	x += folderWidth + gap;
	result.proxy = x;
	result.proxyWidth = proxyWidth;
	x += proxyWidth + gap;
	result.sms = x;
	result.smsWidth = smsWidth;
	x += smsWidth;
	result.totalWidth = x + pad;
	return result;
}

[[nodiscard]] int ManagerRowHeight() {
	return style::ConvertScale(48);
}

// Two small texts stacked in one cell, vertically centered as a pair.
void PaintManagerCell(
		QPainter &p,
		int x,
		int width,
		int height,
		const QString &top,
		const style::font &topFont,
		QColor topColor,
		const QString &bottom,
		const style::font &bottomFont,
		QColor bottomColor) {
	if (width <= 0) {
		return;
	}
	const auto topHeight = topFont->height;
	const auto bottomHeight = bottom.isEmpty() ? 0 : bottomFont->height;
	const auto y = (height - topHeight - bottomHeight) / 2;
	p.setFont(topFont);
	p.setPen(topColor);
	p.drawText(
		QRect(x, y, width, topHeight),
		Qt::AlignLeft | Qt::AlignVCenter,
		topFont->elided(top, width));
	if (!bottom.isEmpty()) {
		p.setFont(bottomFont);
		p.setPen(bottomColor);
		p.drawText(
			QRect(x, y + topHeight, width, bottomHeight),
			Qt::AlignLeft | Qt::AlignVCenter,
			bottomFont->elided(bottom, width));
	}
}

class AccountTableHeader final : public Ui::RpWidget {
public:
	AccountTableHeader(
		not_null<QWidget*> parent,
		Fn<void(bool)> onSelectAll)
	: RpWidget(parent.get()) {
		const auto columns = ComputeManagerColumns();
		resize(columns.totalWidth, style::ConvertScale(34));
		_checkbox = Ui::CreateChild<Ui::Checkbox>(this, QString(), false);
		_checkbox->resize(
			st::defaultCheckbox.checkPosition.x() + st::defaultCheck.diameter,
			st::defaultCheck.diameter);
		_checkbox->moveToLeft(
			columns.check,
			(height() - _checkbox->height()) / 2
				+ style::ConvertScale(3));
		_checkbox->checkedChanges(
		) | rpl::on_next(std::move(onSelectAll), lifetime());
	}

	void setAllChecked(bool checked) {
		if (_checkbox->checked() != checked) {
			_checkbox->setChecked(checked, Ui::Checkbox::NotifyAboutChange::DontNotify);
		}
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		auto p = QPainter(this);
		const auto columns = ComputeManagerColumns();
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
		label(columns.number, style::ConvertScale(28), u"№"_q, Qt::AlignHCenter);
		label(columns.photo, style::ConvertScale(40), u"Фото"_q, Qt::AlignLeft);
		label(columns.account, columns.accountWidth, u"Аккаунт"_q, Qt::AlignLeft);
		label(columns.phone, columns.phoneWidth, u"Телефон / ID"_q, Qt::AlignLeft);
		label(columns.status, columns.statusWidth, u"Статус"_q, Qt::AlignLeft);
		label(columns.folder, columns.folderWidth, u"Папка"_q, Qt::AlignLeft);
		label(columns.proxy, columns.proxyWidth, u"Прокси"_q, Qt::AlignLeft);
		label(columns.sms, columns.smsWidth, u"СМС"_q, Qt::AlignLeft);
	}

private:
	Ui::Checkbox *_checkbox = nullptr;

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
		Fn<void()> onManageFolder)
	: AbstractButton(parent.get())
	, _account(account)
	, _index(index)
	, _current(current)
	, _selected(selected) {
		const auto columns = ComputeManagerColumns();
		resize(columns.totalWidth, ManagerRowHeight());
		_checkbox = Ui::CreateChild<Ui::Checkbox>(this, QString(), selected);
		_checkbox->resize(
			st::defaultCheckbox.checkPosition.x() + st::defaultCheck.diameter,
			st::defaultCheck.diameter);
		// The +3px a plain geometric centering would give here undershoots:
		// a small square checkbox centered at the exact same midpoint as
		// the row's round photo (see the (baseHeight-size)/2 line in
		// paintEvent below) still reads as sitting a couple pixels above
		// it, not level with it.
		_checkbox->moveToLeft(
			columns.check,
			(ManagerRowHeight() - _checkbox->height()) / 2
				+ style::ConvertScale(7));
		_checkAreaRight = columns.check
			+ _checkbox->width()
			+ style::ConvertScale(14);
		_checkbox->checkedChanges(
		) | rpl::on_next([=](bool checked) {
			_selected = checked;
			update();
			onSelect(checked);
		}, lifetime());
		// Right-click anywhere on the row opens folder management for this
		// row's own folder - everything else here stays a left-click-only
		// action, so both buttons need routing.
		setAcceptBoth(true);
		clicks(
		) | rpl::on_next([=](Qt::MouseButton button) {
			const auto pos = mapFromGlobal(QCursor::pos());
			if (button == Qt::RightButton) {
				onManageFolder();
				return;
			}
			if (pos.x() < _checkAreaRight) {
				// The whole check column is the checkbox's target: a click
				// a few pixels off the glyph used to switch the account
				// (and close the window) instead of ticking it.
				_checkbox->setChecked(!_checkbox->checked());
			} else if (_badgeRect.contains(pos)) {
				onCheckSpam();
			} else if (_accountRect.contains(pos)) {
				onEditAccount();
			} else if (_proxyRect.contains(pos)) {
				onEditProxy();
			} else if (!_current) {
				onOpen();
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
	// Tracks which of the row's own click targets (account/proxy/status)
	// the mouse is over, repainting a highlight behind it when that
	// changes - the only visual sign otherwise that a cell inside an
	// already-clickable row is its own separate click target, distinct
	// from "click the row to switch account".
	void mouseMoveEvent(QMouseEvent *e) override {
		AbstractButton::mouseMoveEvent(e);
		const auto zone = _badgeRect.contains(e->pos())
			? HoverZone::Badge
			: _accountRect.contains(e->pos())
			? HoverZone::Account
			: _proxyRect.contains(e->pos())
			? HoverZone::Proxy
			: HoverZone::None;
		if (_hoverZone != zone) {
			_hoverZone = zone;
			update();
		}
	}

	void leaveEventHook(QEvent *e) override {
		AbstractButton::leaveEventHook(e);
		if (_hoverZone != HoverZone::None) {
			_hoverZone = HoverZone::None;
			update();
		}
	}

	void paintEvent(QPaintEvent *e) override {
		auto p = Painter(this);
		const auto columns = ComputeManagerColumns();
		const auto baseHeight = ManagerRowHeight();
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
		} else if (_hoverZone == HoverZone::Proxy) {
			paintHoverHighlight(_proxyRect);
		}

		p.setFont(st::normalFont);
		p.setPen(st::windowSubTextFg);
		p.drawText(
			QRect(columns.number, 0, style::ConvertScale(28), baseHeight),
			Qt::AlignHCenter | Qt::AlignVCenter,
			QString::number(_index));

		if (user) {
			const auto size = style::ConvertScale(kPickerUserpicSize) * 3 / 4;
			user->paintUserpicLeft(
				p,
				_view,
				columns.photo,
				(baseHeight - size) / 2,
				width(),
				size);
		}

		auto name = user ? user->name() : u"(нет сессии)"_q;
		if (name.isEmpty()) {
			name = u"Без имени"_q;
		}
		if (_current) {
			name += u" (текущий)"_q;
		}
		const auto username = user ? user->username() : QString();
		PaintManagerCell(
			p,
			columns.account,
			columns.accountWidth,
			baseHeight,
			name,
			st::semiboldFont,
			st::windowBoldFg->c,
			username.isEmpty() ? u"@no_user"_q : (u"@"_q + username),
			st::normalFont,
			username.isEmpty() ? st::windowSubTextFg->c : QColor(56, 189, 248));
		// Click anywhere on the name/@username cell (either line) to edit
		// them - see ShowEditAccountBox.
		_accountRect = QRect(
			columns.account, 0, columns.accountWidth, baseHeight);

		auto phone = user ? user->phone() : QString();
		if (!phone.isEmpty() && !phone.startsWith('+')) {
			phone = '+' + phone;
		}
		PaintManagerCell(
			p,
			columns.phone,
			columns.phoneWidth,
			baseHeight,
			phone.isEmpty() ? u"—"_q : phone,
			st::semiboldFont,
			st::windowBoldFg->c,
			user ? QString::number(user->id.value) : QString(),
			st::normalFont,
			st::windowSubTextFg->c);

		// The status badge: the account's spam-limit verdict (or "no
		// session"), tinted with its own color - clicking it re-checks
		// with @spambot, same as the "Неизвестно" badge in the reference.
		{
			const auto userId = user ? user->id.value : uint64(0);
			const auto text = user
				? Settings::AccountSpamStatusText(userId)
				: u"Нет сессии"_q;
			const auto color = user
				? Settings::AccountSpamStatusColor(userId)
				: QColor(st::boxTextFgError->c);
			const auto padding = style::ConvertScale(8);
			const auto badgeHeight = st::normalFont->height + padding;
			const auto textWidth = std::min(
				st::normalFont->width(text),
				columns.statusWidth - 2 * padding);
			const auto badgeWidth = textWidth + 2 * padding;
			_badgeRect = QRect(
				columns.status,
				(baseHeight - badgeHeight) / 2,
				badgeWidth,
				badgeHeight);
			auto fill = color;
			fill.setAlphaF(
				(_hoverZone == HoverZone::Badge) ? 0.32 : 0.18);
			auto hq = PainterHighQualityEnabler(p);
			p.setPen(Qt::NoPen);
			p.setBrush(fill);
			p.drawRoundedRect(_badgeRect, padding, padding);
			p.setPen(color);
			p.setFont(st::normalFont);
			p.drawText(
				_badgeRect,
				Qt::AlignCenter,
				st::normalFont->elided(text, textWidth));
		}

		const auto folder = _account->folderTag();
		p.setFont(st::normalFont);
		p.setPen(folder.isEmpty()
			? st::windowSubTextFg->c
			: st::windowBoldFg->c);
		p.drawText(
			QRect(columns.folder, 0, columns.folderWidth, baseHeight),
			Qt::AlignLeft | Qt::AlignVCenter,
			st::normalFont->elided(
				folder.isEmpty() ? u"—"_q : folder,
				columns.folderWidth));

		const auto proxy = _account->proxyOverride();
		const auto hasProxy = proxy.valid() && !proxy.host.isEmpty();
		p.setFont(st::normalFont);
		p.setPen(hasProxy ? QColor(255, 214, 10) : st::windowSubTextFg->c);
		p.drawText(
			QRect(columns.proxy, 0, columns.proxyWidth, baseHeight),
			Qt::AlignLeft | Qt::AlignVCenter,
			st::normalFont->elided(
				hasProxy
					? (proxy.host + ':' + QString::number(proxy.port))
					: u"—"_q,
				columns.proxyWidth));
		// Click here to edit this account's proxy - see MakeAccountProxyBox.
		_proxyRect = QRect(columns.proxy, 0, columns.proxyWidth, baseHeight);

		const auto sent = user ? Data::CountSmsSentToday(user->id.value) : 0;
		p.setFont(st::normalFont);
		p.setPen(sent ? st::windowBoldFg->c : st::windowSubTextFg->c);
		p.drawText(
			QRect(columns.sms, 0, columns.smsWidth, baseHeight),
			Qt::AlignLeft | Qt::AlignVCenter,
			sent ? QString::number(sent) : u"—"_q);
	}

private:
	int _checkAreaRight = 0;
	enum class HoverZone { None, Account, Proxy, Badge };

	const not_null<Main::Account*> _account;
	const int _index;
	const bool _current;
	bool _selected = false;
	Ui::PeerUserpicView _view;
	Ui::Checkbox *_checkbox = nullptr;
	QRect _badgeRect;
	QRect _accountRect;
	QRect _proxyRect;
	HoverZone _hoverZone = HoverZone::None;

};

// One card per folder (plus "ALL") with its name and account counter; the
// chosen one is filled with the accent color. Wraps onto further lines when
// the window is narrow.
class FolderCard final : public Ui::AbstractButton {
public:
	FolderCard(
		not_null<QWidget*> parent,
		const QString &name,
		int count,
		bool active)
	: AbstractButton(parent.get())
	, _name(name)
	, _count(QString::number(count))
	, _active(active) {
		const auto padding = style::ConvertScale(14);
		const auto textWidth = std::max(
			st::semiboldFont->width(_name),
			st::normalFont->width(_count));
		resize(
			std::max(textWidth + 2 * padding, style::ConvertScale(56)),
			st::semiboldFont->height + st::normalFont->height
				+ style::ConvertScale(14));
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		auto p = QPainter(this);
		auto hq = PainterHighQualityEnabler(p);
		const auto radius = style::ConvertScale(12);
		p.setPen(Qt::NoPen);
		if (_active) {
			p.setBrush(QColor(10, 132, 255));
		} else {
			auto fill = st::windowBoldFg->c;
			fill.setAlphaF(isOver() ? 0.12 : 0.06);
			p.setBrush(fill);
		}
		p.drawRoundedRect(rect(), radius, radius);
		const auto top = (height()
			- st::semiboldFont->height
			- st::normalFont->height) / 2;
		p.setFont(st::semiboldFont);
		p.setPen(_active ? QColor(255, 255, 255) : st::windowBoldFg->c);
		p.drawText(
			QRect(0, top, width(), st::semiboldFont->height),
			Qt::AlignHCenter | Qt::AlignVCenter,
			_name);
		p.setFont(st::normalFont);
		p.setPen(_active ? QColor(255, 255, 255, 200) : st::windowSubTextFg->c);
		p.drawText(
			QRect(0, top + st::semiboldFont->height, width(), st::normalFont->height),
			Qt::AlignHCenter | Qt::AlignVCenter,
			_count);
	}

private:
	const QString _name;
	const QString _count;
	const bool _active;

};

class FolderCardsRow final : public Ui::RpWidget {
public:
	explicit FolderCardsRow(not_null<QWidget*> parent)
	: RpWidget(parent.get()) {
	}

	not_null<FolderCard*> addCard(
			const QString &name,
			int count,
			bool active,
			Fn<void()> onClick) {
		const auto card = Ui::CreateChild<FolderCard>(
			this,
			name,
			count,
			active);
		card->setClickedCallback(std::move(onClick));
		card->show();
		_cards.push_back(card);
		return card;
	}

protected:
	int resizeGetHeight(int newWidth) override {
		const auto gap = style::ConvertScale(8);
		auto x = 0;
		auto y = 0;
		auto rowHeight = 0;
		for (const auto card : _cards) {
			if (x > 0 && x + card->width() > newWidth) {
				x = 0;
				y += rowHeight + gap;
				rowHeight = 0;
			}
			card->move(x, y);
			x += card->width() + gap;
			rowHeight = std::max(rowHeight, card->height());
		}
		return y + rowHeight;
	}

private:
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

	void setLabel(not_null<Ui::RpWidget*> label) {
		_label = label.get();
	}

	// Buttons are added left-to-right in the order they should appear
	// visually; they end up right-aligned as a group.
	void addButton(not_null<Ui::RpWidget*> button) {
		_buttons.push_back(button.get());
	}

protected:
	int resizeGetHeight(int newWidth) override {
		const auto gap = style::ConvertScale(8);
		auto buttonsWidth = 0;
		for (const auto button : _buttons) {
			buttonsWidth += button->width() + (buttonsWidth ? gap : 0);
		}
		const auto labelWidth = _label ? _label->width() : 0;
		auto rowHeight = _label ? _label->height() : 0;
		for (const auto button : _buttons) {
			rowHeight = std::max(rowHeight, button->height());
		}
		const auto placeRowRightAligned = [&](
				const std::vector<not_null<Ui::RpWidget*>> &items,
				int y,
				int height,
				int rightEdge) {
			auto right = rightEdge;
			for (auto it = items.rbegin(); it != items.rend(); ++it) {
				const auto item = *it;
				right -= item->width();
				item->move(right, y + (height - item->height()) / 2);
				right -= gap;
			}
		};
		if (!_label || labelWidth + gap + buttonsWidth <= newWidth) {
			// Everything fits on one row: label pinned left, buttons
			// right-aligned, same layout as before this fix.
			if (_label) {
				_label->move(0, (rowHeight - _label->height()) / 2);
			}
			placeRowRightAligned(_buttons, 0, rowHeight, newWidth);
			return rowHeight;
		}
		auto y = 0;
		if (_label) {
			_label->move(0, 0);
			y += _label->height() + gap;
		}
		auto x = 0;
		auto lineHeight = 0;
		auto line = std::vector<not_null<Ui::RpWidget*>>();
		for (const auto button : _buttons) {
			if (x > 0 && x + button->width() > newWidth) {
				placeRowRightAligned(line, y, lineHeight, newWidth);
				y += lineHeight + gap;
				x = 0;
				lineHeight = 0;
				line.clear();
			}
			x += button->width() + gap;
			lineHeight = std::max(lineHeight, button->height());
			line.push_back(button);
		}
		placeRowRightAligned(line, y, lineHeight, newWidth);
		y += lineHeight;
		return y;
	}

private:
	Ui::RpWidget *_label = nullptr;
	std::vector<not_null<Ui::RpWidget*>> _buttons;

};

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
	Fn<void()> rebuildCards;
	Fn<void()> rebuildTable;
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
	}
	return haystack.contains(needle);
}

} // namespace

object_ptr<Ui::BoxContent> MakeAccountManagerBox(
		not_null<Window::SessionController*> controller,
		Fn<void()> closeShell) {
	return Box([=](not_null<Ui::GenericBox*> box) {
		box->setWidth(st::accountsBoxWidth);
		box->setTitle(rpl::single(u"Менеджер аккаунтов"_q));

		const auto state = box->lifetime().make_state<AccountManagerState>();
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

		Ui::AddSkip(pinned);
		const auto cardsWrap = pinned->add(
			object_ptr<Ui::VerticalLayout>(pinned),
			st::boxRowPadding);
		Ui::AddSkip(pinned);

		// Search + selection bar.
		const auto search = pinned->add(
			object_ptr<Ui::InputField>(
				pinned,
				st::defaultInputField,
				rpl::single(u"Поиск: имя, @username, телефон, ID, папка"_q)),
			st::boxRowPadding);
		search->changes(
		) | rpl::on_next([=] {
			state->query = search->getLastText();
			state->rebuildTable();
		}, search->lifetime());

		const auto bar = pinned->add(
			object_ptr<SelectionBarRow>(pinned),
			st::boxRowPadding);
		const auto countLabel = Ui::CreateChild<Ui::FlatLabel>(
			bar,
			state->selectedCount.value() | rpl::map([](int count) {
				return QString::number(count) + u" выбрано"_q;
			}),
			st::boxLabel);
		bar->setLabel(countLabel);
		const auto makeButton = [&](const QString &text, Fn<void()> callback) {
			const auto button = Ui::CreateChild<Ui::RoundButton>(
				bar,
				rpl::single(text),
				st::defaultBoxButton);
			button->setTextTransform(Ui::RoundButtonTextTransform::NoTransform);
			button->setClickedCallback(std::move(callback));
			bar->addButton(button);
			return button;
		};
		const auto selectedAccounts = [=] {
			auto result = Accounts();
			for (const auto &account : state->selected) {
				result.push_back(account);
			}
			return result;
		};
		const auto moveButton = makeButton(u"Переместить"_q, [=] {
			const auto chosen = selectedAccounts();
			if (chosen.empty()) {
				show()->showToast(u"Отметьте хотя бы один аккаунт"_q);
				return;
			}
			ShowAccountFolderNamePrompt(
				show(),
				u"Переместить в папку"_q,
				u"Впишите имя папки (новой или уже существующей). Пустое "
					"имя уберёт выбранные аккаунты из папок."_q,
				QString(),
				[=](QString folder) {
					for (const auto &account : chosen) {
						account->setFolderTag(folder);
					}
					state->selected.clear();
					state->selectedCount = 0;
					show()->showToast(
						u"Перемещено аккаунтов: "_q
							+ QString::number(chosen.size()));
					state->rebuildCards();
					state->rebuildTable();
				});
		});
		const auto spamButton = makeButton(u"Проверить SpamBot"_q, [=] {
			auto started = 0;
			for (const auto &account : state->selected) {
				if (account->sessionExists()) {
					CheckAccountSpamStatus(account);
					++started;
				}
			}
			show()->showToast(started
				? (u"Проверка запущена: "_q + QString::number(started))
				: u"Отметьте хотя бы один аккаунт"_q);
		});
		// Shared by the toolbar's "Управление папкой" button (acting on the
		// active folder filter) and a row's own right-click on its Папка
		// cell (acting on that one row's folder, whatever the active
		// filter happens to be) - see AccountTableRow's onManageFolder.
		const auto showFolderManagement = [=](QString tag) {
			show()->showBox(Box([=](not_null<Ui::GenericBox*> actions) {
				actions->setTitle(rpl::single(u"Папка «"_q + tag + u"»"_q));
				actions->addRow(object_ptr<Ui::SettingsButton>(
					actions,
					rpl::single(u"Переименовать папку"_q))
				)->setClickedCallback([=] {
					actions->closeBox();
					ShowAccountFolderNamePrompt(
						show(),
						u"Переименовать папку"_q,
						u"Все аккаунты этой папки перейдут в папку с новым "
							"именем (если такая уже есть - объединятся с ней)."_q,
						tag,
						[=](QString folder) {
							if (folder.isEmpty() || folder == tag) {
								return;
							}
							for (const auto &account
									: Core::App().domain().orderedAccounts()) {
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
				});
				actions->addRow(object_ptr<Ui::SettingsButton>(
					actions,
					rpl::single(u"Убрать папку (аккаунты останутся)"_q))
				)->setClickedCallback([=] {
					actions->closeBox();
					for (const auto &account
							: Core::App().domain().orderedAccounts()) {
						if (account->folderTag() == tag) {
							account->setFolderTag(QString());
						}
					}
					if (state->folder && *state->folder == tag) {
						state->folder = std::nullopt;
					}
					state->rebuildCards();
					state->rebuildTable();
				});
				actions->addButton(rpl::single(u"Закрыть"_q), [=] {
					actions->closeBox();
				});
			}));
		};

		// The table never reflows or hides a column (see ComputeManagerColumns) -
		// both the header and the rows below sit at that same fixed full
		// width inside their own horizontal-only Ui::ScrollArea, and the two
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
				})).data();
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
		rowsWrap->widthValue(
		) | rpl::on_next([=](int width) {
			rowsScroll->setGeometry(0, 0, width, rowsWrap->height());
		}, rowsWrap->lifetime());
		box->addRow(std::move(rowsWrapOwned), style::margins());
		Ui::AddSkip(box->verticalLayout());

		QObject::connect(
			headerScroll->horizontalScrollBar(),
			&QScrollBar::valueChanged,
			rowsScroll->horizontalScrollBar(),
			&QScrollBar::setValue);
		QObject::connect(
			rowsScroll->horizontalScrollBar(),
			&QScrollBar::valueChanged,
			headerScroll->horizontalScrollBar(),
			&QScrollBar::setValue);

		state->rebuildCards = [=] {
			cardsWrap->clear();
			auto counts = base::flat_map<QString, int>();
			auto total = 0;
			for (const auto &account : Core::App().domain().orderedAccounts()) {
				++counts[account->folderTag()];
				++total;
			}
			if (state->folder && !counts.contains(*state->folder)) {
				state->folder = std::nullopt;
			}
			// Fill the row *before* handing it to the layout: the layout
			// sizes it (and so positions its cards) the moment it's added,
			// and a card added afterwards would never get placed.
			auto rowOwned = object_ptr<FolderCardsRow>(cardsWrap);
			const auto pick = [=](std::optional<QString> folder) {
				return [=] {
					state->folder = folder;
					// Not in place: this runs from inside the very card
					// being rebuilt.
					Ui::PostponeCall(box, [=] {
						state->rebuildCards();
						state->rebuildTable();
					});
				};
			};
			rowOwned->addCard(
				u"ALL"_q,
				total,
				!state->folder.has_value(),
				pick(std::nullopt));
			for (const auto &[tag, count] : counts) {
				rowOwned->addCard(
					tag.isEmpty() ? u"Без папки"_q : tag,
					count,
					(state->folder && *state->folder == tag),
					pick(tag));
			}
			cardsWrap->add(std::move(rowOwned));
		};

		state->rebuildTable = [=] {
			table->clear();
			state->rows.clear();
			state->shown.clear();
			auto everyone = base::flat_set<not_null<Main::Account*>>();
			for (const auto &account : Core::App().domain().orderedAccounts()) {
				everyone.emplace(account);
				if (state->folder && account->folderTag() != *state->folder) {
					continue;
				}
				if (!AccountMatchesQuery(account, state->query)) {
					continue;
				}
				state->shown.push_back(account);
			}
			for (auto i = state->selected.begin(); i != state->selected.end();) {
				if (!everyone.contains(*i)) {
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

			if (state->shown.empty()) {
				table->add(
					object_ptr<Ui::FlatLabel>(
						table,
						everyone.empty()
							? u"Нет добавленных аккаунтов"_q
							: u"Ничего не найдено"_q,
						st::boxLabel),
					st::boxRowPadding);
				return;
			}
			const auto currentAccount = &controller->session().account();
			auto index = 0;
			for (const auto &account : state->shown) {
				const auto row = table->add(object_ptr<AccountTableRow>(
					table,
					account,
					++index,
					(account.get() == currentAccount),
					state->selected.contains(account),
					[=](bool checked) {
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
						closeShell();
						Core::App().domain().maybeActivate(account);
					},
					[=] {
						if (!account->sessionExists()) {
							return;
						}
						CheckAccountSpamStatus(account);
						show()->showToast(u"Проверка запущена"_q);
					},
					[=] { ShowEditAccountBox(show(), account); },
					[=] { show()->showBox(MakeAccountProxyBox(account)); },
					[=] {
						const auto tag = account->folderTag();
						if (tag.isEmpty()) {
							show()->showToast(
								u"У аккаунта нет папки - переместите его "
									"через \"Переместить\""_q);
							return;
						}
						showFolderManagement(tag);
					}));
				state->rows.push_back(row);
			}
		};

		state->rebuildCards();
		state->rebuildTable();
	});
}

rpl::producer<BulkActionIndicator> BulkActionIndicatorValue() {
	return BulkIndicatorState().value();
}

void OpenBulkActions(not_null<Window::SessionController*> controller) {
	if (const auto state = CurrentBulkRun()) {
		BuildBulkProgressBox(controller, state);
		return;
	}
	controller->show(MakeBulkFoldersBox(controller));
}

} // namespace Settings
