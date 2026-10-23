/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/ggtg_sync.h"

#include <QtCore/QByteArray>

#include <openssl/evp.h>
#include <openssl/rand.h>

namespace Core::GgtgSync {
namespace {

constexpr auto kMagic = "GGSY\x01";
constexpr auto kMagicSize = 5;
constexpr auto kSaltSize = 16;
constexpr auto kIvSize = 12;
constexpr auto kTagSize = 16;
constexpr auto kPbkdfRounds = 100000;

[[nodiscard]] QByteArray Derive(
		const QString &passphrase,
		const QByteArray &salt) {
	auto key = QByteArray(32, 0);
	const auto utf8 = passphrase.toUtf8();
	PKCS5_PBKDF2_HMAC(
		utf8.constData(),
		utf8.size(),
		reinterpret_cast<const unsigned char*>(salt.constData()),
		salt.size(),
		kPbkdfRounds,
		EVP_sha256(),
		key.size(),
		reinterpret_cast<unsigned char*>(key.data()));
	return key;
}

} // namespace

QByteArray Pack(const QByteArray &json, const QString &passphrase) {
	const auto packed = qCompress(json, 6);
	auto salt = QByteArray(kSaltSize, 0);
	auto iv = QByteArray(kIvSize, 0);
	RAND_bytes(reinterpret_cast<unsigned char*>(salt.data()), salt.size());
	RAND_bytes(reinterpret_cast<unsigned char*>(iv.data()), iv.size());
	const auto key = Derive(passphrase, salt);

	auto result = QByteArray(kMagic, kMagicSize);
	result.append(salt);
	result.append(iv);
	auto cipher = QByteArray(packed.size() + 16, 0);
	auto tag = QByteArray(kTagSize, 0);
	auto length = 0;
	auto total = 0;
	const auto context = EVP_CIPHER_CTX_new();
	EVP_EncryptInit_ex(context, EVP_aes_256_gcm(), nullptr, nullptr, nullptr);
	EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_SET_IVLEN, kIvSize, nullptr);
	EVP_EncryptInit_ex(
		context,
		nullptr,
		nullptr,
		reinterpret_cast<const unsigned char*>(key.constData()),
		reinterpret_cast<const unsigned char*>(iv.constData()));
	EVP_EncryptUpdate(
		context,
		reinterpret_cast<unsigned char*>(cipher.data()),
		&length,
		reinterpret_cast<const unsigned char*>(packed.constData()),
		packed.size());
	total = length;
	EVP_EncryptFinal_ex(
		context,
		reinterpret_cast<unsigned char*>(cipher.data()) + total,
		&length);
	total += length;
	EVP_CIPHER_CTX_ctrl(
		context,
		EVP_CTRL_GCM_GET_TAG,
		kTagSize,
		tag.data());
	EVP_CIPHER_CTX_free(context);
	cipher.resize(total);
	result.append(cipher);
	result.append(tag);
	return result;
}

std::optional<QByteArray> Unpack(
		const QByteArray &blob,
		const QString &passphrase) {
	const auto header = kMagicSize + kSaltSize + kIvSize;
	if (blob.size() < header + kTagSize
		|| !blob.startsWith(QByteArray(kMagic, kMagicSize))) {
		return std::nullopt;
	}
	const auto salt = blob.mid(kMagicSize, kSaltSize);
	const auto iv = blob.mid(kMagicSize + kSaltSize, kIvSize);
	const auto cipherSize = blob.size() - header - kTagSize;
	const auto cipher = blob.mid(header, cipherSize);
	auto tag = blob.right(kTagSize);
	const auto key = Derive(passphrase, salt);

	auto plain = QByteArray(cipherSize + 16, 0);
	auto length = 0;
	auto total = 0;
	const auto context = EVP_CIPHER_CTX_new();
	EVP_DecryptInit_ex(context, EVP_aes_256_gcm(), nullptr, nullptr, nullptr);
	EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_SET_IVLEN, kIvSize, nullptr);
	EVP_DecryptInit_ex(
		context,
		nullptr,
		nullptr,
		reinterpret_cast<const unsigned char*>(key.constData()),
		reinterpret_cast<const unsigned char*>(iv.constData()));
	EVP_DecryptUpdate(
		context,
		reinterpret_cast<unsigned char*>(plain.data()),
		&length,
		reinterpret_cast<const unsigned char*>(cipher.constData()),
		cipher.size());
	total = length;
	EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_SET_TAG, kTagSize, tag.data());
	const auto good = EVP_DecryptFinal_ex(
		context,
		reinterpret_cast<unsigned char*>(plain.data()) + total,
		&length) > 0;
	EVP_CIPHER_CTX_free(context);
	if (!good) {
		return std::nullopt;
	}
	total += length;
	plain.resize(total);
	const auto json = qUncompress(plain);
	if (json.isEmpty()) {
		return std::nullopt;
	}
	return json;
}

} // namespace Core::GgtgSync
