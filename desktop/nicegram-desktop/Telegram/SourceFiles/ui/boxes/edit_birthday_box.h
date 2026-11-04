/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace Data {
class Birthday;
} // namespace Data

namespace Ui {

class GenericBox;

enum class EditBirthdayType {
	Edit,
	Suggest,
	ConfirmSuggestion,
};

void EditBirthdayBox(
	not_null<Ui::GenericBox*> box,
	Data::Birthday current,
	Fn<void(Data::Birthday)> save,
	EditBirthdayType type = EditBirthdayType::Edit);

// ggtg: the same wheels without the day - month and year only (registration date of an account).
void EditMonthYearBox(
	not_null<Ui::GenericBox*> box,
	int month,
	int year,
	Fn<void(int month, int year)> save);

} // namespace Ui
