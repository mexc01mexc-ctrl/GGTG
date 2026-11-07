// Tests of settings/ggtg_table_layout.h - run with ggtg-tests/run.sh.
#include "settings/ggtg_table_layout.h"

#include <cstdio>

namespace {

int failures = 0;

void Check(bool ok, const char *what, int line) {
	if (!ok) {
		std::fprintf(stderr, "FAIL line %d: %s\n", line, what);
		++failures;
	}
}
#define CHECK(x) Check((x), #x, __LINE__)

using namespace Settings::TableLayout;

void TestDefaults() {
	const auto s = Defaults();
	CHECK(IsDefault(s));
	CHECK(int(s.order.size()) == kColCount);
	CHECK(s.order[kColCount - 2] == Col::Open);
	CHECK(s.order[kColCount - 1] == Col::Trash);
	CHECK(ToggleableCount() == 13);
	CHECK(ToggleableShown(s) == 5);
	CHECK(CountText(s) == u"5 из 13"_q);
}

void TestHideAndAlwaysOn() {
	auto s = Defaults();
	SetVisible(s, Col::Folder, false);
	CHECK(!s.visible[int(Col::Folder)]);
	SetVisible(s, Col::Account, false);
	SetVisible(s, Col::Open, false);
	CHECK(s.visible[int(Col::Account)]);
	CHECK(s.visible[int(Col::Open)]);
	CHECK(!IsDefault(s));
}

void TestMove() {
	auto s = Defaults();
	MoveBefore(s, Col::Status, Col::Folder);
	const auto shown = Shown(s);
	CHECK(shown[2] == Col::Status);
	CHECK(shown[3] == Col::Folder);
	MoveBefore(s, Col::Open, Col::Number); // tail does not move
	CHECK(s.order[kColCount - 2] == Col::Open);
	MoveBefore(s, Col::Age, Col::Trash); // ends before the tail
	CHECK(s.order[kColCount - 3] == Col::Age);
}

void TestWidth() {
	auto s = Defaults();
	CHECK(WidthOf(s, Col::Proxy) == 140);
	SetWidth(s, Col::Proxy, 10);
	CHECK(WidthOf(s, Col::Proxy) == 90); // not below the minimum
	SetWidth(s, Col::Proxy, 200);
	CHECK(WidthOf(s, Col::Proxy) == 200);
	SetWidth(s, Col::Trash, 300); // fixed
	CHECK(WidthOf(s, Col::Trash) == 72);
}

void TestRoundTrip() {
	auto s = Defaults();
	SetVisible(s, Col::TwoFa, true);
	SetVisible(s, Col::Sms24h, false);
	SetWidth(s, Col::Account, 240);
	MoveBefore(s, Col::Proxy, Col::Number);
	s.density = Density::Compact;
	s.pinFirst = false;
	const auto back = FromJson(ToJson(s));
	CHECK(back.order == s.order);
	CHECK(back.visible == s.visible);
	CHECK(back.width == s.width);
	CHECK(back.density == Density::Compact);
	CHECK(!back.pinFirst);
}

void TestBadData() {
	CHECK(IsDefault(FromJson(QByteArray())));
	CHECK(IsDefault(FromJson("garbage")));
	// An old file without the newer columns, and an unknown key.
	const auto old = QByteArray(
		"{\"order\":[\"account\",\"zzz\",\"number\"],"
		"\"visible\":{\"number\":false,\"zzz\":true}}");
	const auto s = FromJson(old);
	CHECK(int(s.order.size()) == kColCount);
	CHECK(s.order[0] == Col::Account);
	CHECK(s.order[kColCount - 1] == Col::Trash);
	CHECK(!s.visible[int(Col::Number)]);
	CHECK(s.visible[int(Col::Account)]);
}

} // namespace

int main() {
	TestDefaults();
	TestHideAndAlwaysOn();
	TestMove();
	TestWidth();
	TestRoundTrip();
	TestBadData();
	if (failures) {
		std::fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	std::printf("ggtg_table_layout_test: ok\n");
	return 0;
}
