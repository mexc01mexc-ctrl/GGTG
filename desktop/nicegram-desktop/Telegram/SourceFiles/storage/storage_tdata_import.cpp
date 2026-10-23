/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "storage/storage_tdata_import.h"

#include "storage/details/storage_file_utilities.h"
#include "storage/serialize_common.h"
#include "mtproto/mtproto_auth_key.h"

#include <QtCore/QDir>

namespace Storage {
namespace {

using namespace details;

// Matches Storage::Account::writeMtpData()'s single-block file exactly -
// this file only ever contains this one dbi block, nothing else.
constexpr auto kDbiMtpAuthorization = quint32(0x4b);

[[nodiscard]] QString ComposeDataString(const QString &dataName, int index) {
	auto result = dataName;
	result.replace('#', QString());
	if (index > 0) {
		result += '#' + QString::number(index + 1);
	}
	return result;
}

[[nodiscard]] FileKey ComputeDataNameKey(const QString &dataName) {
	const auto utf8 = dataName.toUtf8();
	FileKey hash[2] = { 0 };
	hashMd5(utf8.constData(), utf8.size(), hash);
	return hash[0];
}

// ReadFile() already tries name+'s' then name+'0'/'1' internally, so the
// logical (unsuffixed) key name is what we must pass in - "data" is the
// standard default dataName used by the vast majority of installs. If a
// folder was written under a different dataName, fall back to scanning
// for "key_*" files on disk and stripping their write-safety suffix.
[[nodiscard]] QStringList CandidateDataNames(const QString &folderPath) {
	auto result = QStringList{ u"data"_q };
	QDir dir(folderPath);
	const auto entries = dir.entryList(
		QStringList{ u"key_*"_q },
		QDir::Files);
	for (const auto &entry : entries) {
		auto name = entry.mid(4); // strip "key_"
		if (!name.isEmpty()
			&& (name.endsWith('s') || name.endsWith('0') || name.endsWith('1'))) {
			name.chop(1);
		}
		if (!name.isEmpty() && !result.contains(name)) {
			result.push_back(name);
		}
	}
	return result;
}

constexpr auto kLocateMaxDepth = 4;

// The user may have picked a folder that merely *contains* the real tdata
// folder somewhere inside it (e.g. an export tool wrapped it in its own
// "Telegram Desktop" folder, or an older single-account export sits
// alongside the real multi-account one) rather than the tdata folder
// itself - search a few levels down and collect *every* directory that
// looks like a tdata folder (has key_* files in it), instead of requiring
// an exact match or stopping at the first candidate found.
void CollectTdataFolderCandidates(
		const QString &root,
		int depthLeft,
		std::vector<QString> &out) {
	QDir dir(root);
	if (!dir.entryList(QStringList{ u"key_*"_q }, QDir::Files).isEmpty()) {
		out.push_back(root);
	}
	if (depthLeft <= 0) {
		return;
	}
	for (const auto &subdir
			: dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
		CollectTdataFolderCandidates(
			dir.filePath(subdir),
			depthLeft - 1,
			out);
	}
}

// Reads every account out of a single tdata folder (not recursing into
// subfolders) - the actual folder must be exactly "path".
[[nodiscard]] std::vector<QByteArray> ReadOneTdataFolder(
		const QString &folderPath,
		const QByteArray &passcode) {
	const auto path = folderPath.endsWith('/')
		? folderPath
		: (folderPath + '/');

	for (const auto &dataName : CandidateDataNames(path)) {
		FileReadDescriptor keyData;
		if (!ReadFile(keyData, "key_" + dataName, path)) {
			continue;
		}

		QByteArray salt, keyEncrypted, infoEncrypted;
		keyData.stream >> salt >> keyEncrypted >> infoEncrypted;
		if (!CheckStreamStatus(keyData.stream)) {
			continue;
		}

		const auto passcodeKey = CreateLocalKey(passcode, salt);
		EncryptedDescriptor keyInnerData;
		if (!DecryptLocal(keyInnerData, keyEncrypted, passcodeKey)) {
			continue; // Wrong dataName guess, or this folder needs a passcode.
		}
		const auto key = Serialize::read<MTP::AuthKey::Data>(
			keyInnerData.stream);
		if (keyInnerData.stream.status() != QDataStream::Ok
			|| !keyInnerData.stream.atEnd()) {
			continue;
		}
		const auto localKey = std::make_shared<MTP::AuthKey>(key);

		EncryptedDescriptor info;
		if (!DecryptLocal(info, infoEncrypted, localKey)) {
			continue;
		}
		auto count = qint32();
		info.stream >> count;
		if (count <= 0 || !CheckStreamStatus(info.stream)) {
			continue;
		}
		LOG(("TdataImport: dataName '%1', info lists count=%2"
			).arg(dataName).arg(count));

		// A tdata folder can hold several accounts - read every index it
		// lists, not just the first, so all of them get imported.
		auto indices = std::vector<qint32>();
		indices.reserve(count);
		for (auto i = 0; i != count; ++i) {
			auto index = qint32();
			info.stream >> index;
			if (!CheckStreamStatus(info.stream)) {
				LOG(("TdataImport: failed reading index #%1 of %2"
					).arg(i).arg(count));
				break;
			}
			indices.push_back(index);
		}
		LOG(("TdataImport: read %1 indices: %2"
			).arg(indices.size()
			).arg([&] {
				auto list = QStringList();
				for (const auto index : indices) {
					list << QString::number(index);
				}
				return list.join(", ");
			}()));
		if (indices.empty()) {
			continue;
		}

		auto result = std::vector<QByteArray>();
		for (const auto index : indices) {
			const auto fileKey = ComputeDataNameKey(
				ComposeDataString(dataName, index));

			FileReadDescriptor mtp;
			if (!ReadEncryptedFile(mtp, ToFilePart(fileKey), path, localKey)) {
				LOG(("TdataImport: index %1 - could not read/decrypt file %2"
					).arg(index).arg(ToFilePart(fileKey)));
				continue;
			}
			auto blockId = quint32();
			mtp.stream >> blockId;
			if (!CheckStreamStatus(mtp.stream)
				|| blockId != kDbiMtpAuthorization) {
				LOG(("TdataImport: index %1 - bad block id (got %2, want %3)"
					).arg(index).arg(blockId).arg(kDbiMtpAuthorization));
				continue;
			}
			auto serialized = QByteArray();
			mtp.stream >> serialized;
			if (!CheckStreamStatus(mtp.stream) || serialized.isEmpty()) {
				LOG(("TdataImport: index %1 - empty/bad serialized blob"
					).arg(index));
				continue;
			}
			LOG(("TdataImport: index %1 - OK, %2 bytes"
				).arg(index).arg(serialized.size()));
			result.push_back(serialized);
		}
		LOG(("TdataImport: dataName '%1' yielded %2 of %3 accounts"
			).arg(dataName).arg(result.size()).arg(indices.size()));
		if (!result.empty()) {
			return result;
		}
	}
	return {};
}

} // namespace

std::vector<ForeignTdataAuthorization> ReadForeignTdataAuthorizations(
		const QString &folderPath,
		const QByteArray &passcode) {
	auto candidates = std::vector<QString>();
	CollectTdataFolderCandidates(folderPath, kLocateMaxDepth, candidates);

	// Several candidate folders can turn up either way: one real tdata
	// folder plus a stray smaller export sitting alongside it, or several
	// entirely separate single/few-account tdata folders (e.g. copies of
	// multiple "Telegram Desktop" installs) grouped under one parent
	// folder the user picked. Read every candidate and combine everything
	// found - any genuine duplicate account gets caught later by
	// Domain::addFromTdata's own userId check, so combining here is safe.
	auto combined = std::vector<ForeignTdataAuthorization>();
	for (const auto &candidate : candidates) {
		LOG(("TdataImport: trying candidate folder '%1'").arg(candidate));
		auto read = ReadOneTdataFolder(candidate, passcode);
		LOG(("TdataImport: candidate '%1' yielded %2 accounts"
			).arg(candidate).arg(read.size()));
		for (auto &blob : read) {
			combined.push_back({ std::move(blob), candidate });
		}
	}
	return combined;
}

} // namespace Storage
