/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "storage/storage_tdata_export.h"

#include "storage/details/storage_file_utilities.h"
#include "storage/serialize_common.h"
#include "mtproto/mtproto_auth_key.h"
#include "main/main_account.h"
#include "base/random.h"
#include "config.h"
#include "core/ggtg_log.h"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>

namespace Storage {
namespace {

using namespace details;

// Matches Storage::Account::writeMtpData()'s single-block file exactly.
constexpr auto kDbiMtpAuthorization = quint32(0x4b);

// Mirrors Main::Account's private ComputeDataNameKey() for the default,
// single-account dataName - same hash storage_tdata_import.cpp reads back.
[[nodiscard]] FileKey ComputeDataNameKey(const QString &dataName) {
	const auto utf8 = dataName.toUtf8();
	FileKey hash[2] = { 0 };
	hashMd5(utf8.constData(), utf8.size(), hash);
	return hash[0];
}

// The key every exported tdata folder is protected with. Deriving it is slow
// (about half a second each), which froze the window for minutes on a batch
// of a hundred accounts, so it is made once per run and reused. It does not
// weaken anything: the data key is stored in each folder wrapped with an
// *empty* passcode (so re-importing needs no password), i.e. anyone holding
// an exported folder can already read it.
struct SharedExportKeys {
	MTP::AuthKeyPtr localKey;
	QByteArray passcodeSalt;
	QByteArray passcodeKeyEncrypted;
};

[[nodiscard]] const SharedExportKeys &ExportKeys() {
	static const auto keys = [] {
		// A fresh, random data-encryption key, unrelated to the live
		// app's own local key, so the export stands on its own.
		auto seedPass = QByteArray(MTP::AuthKey::kSize, Qt::Uninitialized);
		auto seedSalt = QByteArray(LocalEncryptSaltSize, Qt::Uninitialized);
		base::RandomFill(seedPass.data(), seedPass.size());
		base::RandomFill(seedSalt.data(), seedSalt.size());
		auto result = SharedExportKeys();
		result.localKey = CreateLocalKey(seedPass, seedSalt);

		// Wrap it with an empty passcode - mirrors Domain::
		// generateLocalKey() seeding a fresh, lockless install.
		result.passcodeSalt = QByteArray(
			LocalEncryptSaltSize,
			Qt::Uninitialized);
		base::RandomFill(
			result.passcodeSalt.data(),
			result.passcodeSalt.size());
		const auto passcodeKey = CreateLocalKey(
			QByteArray(),
			result.passcodeSalt);
		EncryptedDescriptor passKeyData(MTP::AuthKey::kSize);
		result.localKey->write(passKeyData.stream);
		result.passcodeKeyEncrypted = PrepareEncrypted(
			passKeyData,
			passcodeKey);
		return result;
	}();
	return keys;
}

} // namespace

bool ExportRawAuthorizationAsTdata(
		const QByteArray &serializedAuth,
		const QString &folderPath,
		QString *error) {
	// Nested under its own "tdata" subfolder, with the caller's
	// WriteTwoFactorSidecar() sibling to *that* (not mixed in with these
	// files) - matches the layout findSiblingTwoFactor() already expects
	// when reading a purchased/scraped account back in (tdata folder plus
	// a 2FA note beside it), so round-tripping an export back through
	// import just works.
	const auto path = (folderPath.endsWith('/')
		? folderPath
		: (folderPath + '/')) + u"tdata/"_q;
	const auto fail = [&](const QString &why) {
		const auto text = why
			+ u" ("_q
			+ QDir::toNativeSeparators(path)
			+ ')';
		Core::GgtgLog::Error(u"Экспорт tdata"_q, text);
		if (error) {
			*error = text;
		}
		return false;
	};
	if (!QDir().mkpath(path)) {
		return fail(u"не удалось создать папку"_q);
	}
	if (!QFileInfo(path).isDir()) {
		return fail(u"путь не является папкой"_q);
	}

	const auto &keys = ExportKeys();
	const auto &localKey = keys.localKey;
	const auto &passcodeSalt = keys.passcodeSalt;
	const auto &passcodeKeyEncrypted = keys.passcodeKeyEncrypted;

	// Each descriptor writes its file when it goes out of scope, so the
	// blocks below end before the files are checked.
	{
		FileWriteDescriptor key(u"key_data"_q, path, true);
		key.writeData(passcodeSalt);
		key.writeData(passcodeKeyEncrypted);

		const auto keySize = sizeof(qint32) * 3;
		EncryptedDescriptor keyData(keySize);
		keyData.stream << qint32(1) << qint32(0) << qint32(0); // count/index/active
		key.writeEncrypted(keyData, localKey);
	}
	{
		const auto mtpSize = sizeof(quint32)
			+ Serialize::bytearraySize(serializedAuth);
		FileWriteDescriptor mtp(
			ToFilePart(ComputeDataNameKey(u"data"_q)),
			path,
			true);
		EncryptedDescriptor data(mtpSize);
		data.stream << kDbiMtpAuthorization << serializedAuth;
		mtp.writeEncrypted(data, localKey);
	}
	if (!QFileInfo::exists(path + u"key_datas"_q)) {
		return fail(u"файл key_datas не записался"_q);
	}
	return true;
}

bool ExportAccountAsTdata(
		not_null<Main::Account*> account,
		const QString &folderPath,
		QString *error) {
	if (!account->sessionExists()) {
		if (error) {
			*error = u"нет активной сессии"_q;
		}
		return false;
	}
	return ExportRawAuthorizationAsTdata(
		account->serializeMtpAuthorization(),
		folderPath,
		error);
}

} // namespace Storage
