// A stand-in for the application's settings.h for the QtCore-only tests of
// the AI logic: just the working directory (a temporary folder).
#pragma once

#include "base/basic_types.h"
#include <range/v3/algorithm/remove_if.hpp>

#include <QtCore/QDir>
#include <QtCore/QString>

inline QString cWorkingDir() {
	static const auto result = QDir::tempPath() + u"/ggtg-ai-test-work/";
	return result;
}
