package app.ggtg.tdata

import java.io.File
import java.security.MessageDigest
import javax.crypto.Cipher
import javax.crypto.Mac
import javax.crypto.spec.SecretKeySpec

// Reads Telegram Desktop's on-disk "tdata" session folder format (TDF files,
// AES-256-IGE local encryption) and extracts (userId, mainDcId, per-DC auth
// keys) for each account it contains - the same data Desktop itself would
// hand off via Main::Account::setMtpAuthorization.
//
// Ported line-for-line from the real Desktop source (storage/storage_tdata_import.cpp,
// storage/details/storage_file_utilities.cpp, mtproto/mtproto_auth_key.h/.cpp) and
// verified against real tdata samples end-to-end (every TDF signature and the
// AES-IGE self-check both passed) before being trusted in this port.

class TdataParseException(message: String) : Exception(message)

data class TdataAccount(
    val userId: Long,
    val mainDcId: Int,
    val keys: List<Pair<Int, ByteArray>>,
    val sourceFolder: File = File("")
)

internal val TDF_MAGIC = byteArrayOf('T'.code.toByte(), 'D'.code.toByte(), 'F'.code.toByte(), '$'.code.toByte())

private class QReader(val data: ByteArray) {
    var pos = 0

    fun readU32(): Long {
        val v = ((data[pos].toLong() and 0xFF) shl 24) or
            ((data[pos + 1].toLong() and 0xFF) shl 16) or
            ((data[pos + 2].toLong() and 0xFF) shl 8) or
            (data[pos + 3].toLong() and 0xFF)
        pos += 4
        return v
    }

    fun readI32(): Int = readU32().toInt()

    fun readU64(): Long {
        var v = 0L
        for (i in 0 until 8) {
            v = (v shl 8) or (data[pos + i].toLong() and 0xFF)
        }
        pos += 8
        return v
    }

    fun readByteArray(): ByteArray? {
        val n = readU32()
        if (n == 0xFFFFFFFFL) return null
        val b = data.copyOfRange(pos, pos + n.toInt())
        pos += n.toInt()
        return b
    }

    fun readRaw(n: Int): ByteArray {
        val b = data.copyOfRange(pos, pos + n)
        pos += n
        return b
    }
}

internal fun md5(vararg parts: ByteArray): ByteArray {
    val md = MessageDigest.getInstance("MD5")
    for (p in parts) md.update(p)
    return md.digest()
}

internal fun sha1(vararg parts: ByteArray): ByteArray {
    val md = MessageDigest.getInstance("SHA-1")
    for (p in parts) md.update(p)
    return md.digest()
}

internal fun sha512(vararg parts: ByteArray): ByteArray {
    val md = MessageDigest.getInstance("SHA-512")
    for (p in parts) md.update(p)
    return md.digest()
}

internal fun leInt32(v: Int): ByteArray {
    return byteArrayOf(
        (v and 0xFF).toByte(),
        ((v shr 8) and 0xFF).toByte(),
        ((v shr 16) and 0xFF).toByte(),
        ((v shr 24) and 0xFF).toByte()
    )
}

/** Reads + verifies a "TDF$"-framed file (tries the `s` write-safety suffix, then `0`). */
private fun readTdf(base: File): ByteArray {
    val f = File(base.parentFile, base.name + "s").takeIf { it.exists() }
        ?: File(base.parentFile, base.name + "0").takeIf { it.exists() }
        ?: throw TdataParseException("no TDF file at ${base.path}[s|0]")
    val bytes = f.readBytes()
    if (bytes.size < 8 || !bytes.copyOfRange(0, 4).contentEquals(TDF_MAGIC)) {
        throw TdataParseException("bad TDF magic in ${f.path}")
    }
    val versionBytes = bytes.copyOfRange(4, 8) // native (LE) int32, not QDataStream
    val body = bytes.copyOfRange(8, bytes.size)
    val dataSize = body.size - 16
    if (dataSize < 0) throw TdataParseException("TDF file too short: ${f.path}")
    val payload = body.copyOfRange(0, dataSize)
    val sig = body.copyOfRange(dataSize, dataSize + 16)
    val check = md5(payload, leInt32(dataSize), versionBytes, TDF_MAGIC)
    if (!check.contentEquals(sig)) {
        throw TdataParseException("TDF signature mismatch in ${f.path}")
    }
    return payload
}

private fun hmacSha512(key: ByteArray, data: ByteArray): ByteArray {
    val mac = Mac.getInstance("HmacSHA512")
    mac.init(SecretKeySpec(if (key.isEmpty()) ByteArray(1) else key, "HmacSHA512"))
    return mac.doFinal(data)
}

// Manual PBKDF2-HMAC-SHA512 on raw bytes. Android/JCE's PBEKeySpec-based
// SecretKeyFactory takes a char[] password and some providers re-encode it as
// UTF-8 internally, which silently mangles any byte >= 0x80 - wrong here since
// the "password" is binary key material (a SHA-512 hash), not text. Verified
// against a from-scratch Python/pycryptodome reference implementation and real
// tdata samples (recovered auth keys matched byte-for-byte) before trusting this.
private fun pbkdf2HmacSha512(password: ByteArray, salt: ByteArray, iterations: Int, dkLenBytes: Int): ByteArray {
    val hLen = 64
    val blocks = (dkLenBytes + hLen - 1) / hLen
    val dk = ByteArray(blocks * hLen)
    for (i in 1..blocks) {
        val saltI = salt + byteArrayOf((i shr 24).toByte(), (i shr 16).toByte(), (i shr 8).toByte(), i.toByte())
        var u = hmacSha512(password, saltI)
        val t = u.copyOf()
        for (j in 2..iterations) {
            u = hmacSha512(password, u)
            for (k in 0 until hLen) t[k] = (t[k].toInt() xor u[k].toInt()).toByte()
        }
        System.arraycopy(t, 0, dk, (i - 1) * hLen, hLen)
    }
    return dk.copyOfRange(0, dkLenBytes)
}

/** Storage::details::CreateLocalKey - PBKDF2-HMAC-SHA512(SHA512(salt+passcode+salt), salt). */
internal fun createLocalKey(passcode: ByteArray, salt: ByteArray): ByteArray {
    val hash = sha512(salt, passcode, salt)
    val iterations = if (passcode.isEmpty()) 1 else 100_000
    return pbkdf2HmacSha512(hash, salt, iterations, 256)
}

/** MTP::AuthKey::prepareAES_oldmtp(msgKey, send=false) - the classic MTProto 1.0 local-encryption KDF. */
internal fun prepareAesOldMtp(key: ByteArray, msgKey: ByteArray, send: Boolean = false): Pair<ByteArray, ByteArray> {
    val x = if (send) 0 else 8
    val sha1A = sha1(msgKey, key.copyOfRange(x, x + 32))
    val sha1B = sha1(key.copyOfRange(32 + x, 32 + x + 16), msgKey, key.copyOfRange(48 + x, 48 + x + 16))
    val sha1C = sha1(key.copyOfRange(64 + x, 64 + x + 32), msgKey)
    val sha1D = sha1(msgKey, key.copyOfRange(96 + x, 96 + x + 32))
    val aesKey = sha1A.copyOfRange(0, 8) + sha1B.copyOfRange(8, 20) + sha1C.copyOfRange(4, 16)
    val aesIv = sha1A.copyOfRange(8, 20) + sha1B.copyOfRange(0, 8) + sha1C.copyOfRange(16, 20) + sha1D.copyOfRange(0, 8)
    return aesKey to aesIv
}

private fun igeDecrypt(data: ByteArray, key: ByteArray, iv: ByteArray): ByteArray {
    require(data.size % 16 == 0)
    val cipher = Cipher.getInstance("AES/ECB/NoPadding")
    cipher.init(Cipher.DECRYPT_MODE, SecretKeySpec(key, "AES"))
    var prevCipher = iv.copyOfRange(0, 16)
    var prevPlain = iv.copyOfRange(16, 32)
    val out = ByteArray(data.size)
    var i = 0
    while (i < data.size) {
        val block = data.copyOfRange(i, i + 16)
        val x = ByteArray(16) { (block[it].toInt() xor prevPlain[it].toInt()).toByte() }
        val dec = cipher.doFinal(x)
        val plain = ByteArray(16) { (dec[it].toInt() xor prevCipher[it].toInt()).toByte() }
        System.arraycopy(plain, 0, out, i, 16)
        prevCipher = block
        prevPlain = plain
        i += 16
    }
    return out
}

/** Storage::details::DecryptLocal - decrypts + verifies one AES-IGE-wrapped local blob. */
private fun decryptLocal(encrypted: ByteArray, key: ByteArray): ByteArray {
    if (encrypted.size <= 16 || encrypted.size % 16 != 0) {
        throw TdataParseException("bad encrypted blob size ${encrypted.size}")
    }
    val msgKey = encrypted.copyOfRange(0, 16)
    val encData = encrypted.copyOfRange(16, encrypted.size)
    val (aesKey, aesIv) = prepareAesOldMtp(key, msgKey)
    val decrypted = igeDecrypt(encData, aesKey, aesIv)
    val check = sha1(decrypted).copyOfRange(0, 16)
    if (!check.contentEquals(msgKey)) {
        throw TdataParseException("checksum mismatch - wrong local key/passcode")
    }
    // dataLen is a raw native (LE) uint32 prefix, not a QDataStream field.
    val dataLen = (decrypted[0].toInt() and 0xFF) or
        ((decrypted[1].toInt() and 0xFF) shl 8) or
        ((decrypted[2].toInt() and 0xFF) shl 16) or
        ((decrypted[3].toInt() and 0xFF) shl 24)
    return decrypted.copyOfRange(4, dataLen)
}

private fun composeDataString(dataName: String, index: Int): String {
    var result = dataName.replace("#", "")
    if (index > 0) result += "#" + (index + 1)
    return result
}

/** Storage::ComputeDataNameKey - MD5(utf8(name)), first 8 bytes as a little-endian uint64. */
internal fun computeDataNameKey(dataName: String): Long {
    val digest = md5(dataName.toByteArray(Charsets.UTF_8))
    var v = 0L
    for (i in 7 downTo 0) {
        v = (v shl 8) or (digest[i].toLong() and 0xFF)
    }
    return v
}

/** Storage::details::ToFilePart - 16 lowercase-source hex nibbles, LSB first, matching Desktop's filenames. */
internal fun toFilePart(value: Long): String {
    val sb = StringBuilder(16)
    var v = value
    repeat(16) {
        val nibble = (v and 0x0F).toInt()
        sb.append(if (nibble < 0xA) ('0' + nibble) else ('A' + (nibble - 0xA)))
        v = v ushr 4
    }
    return sb.toString()
}

private const val DBI_MTP_AUTHORIZATION = 0x4bL
private const val WIDE_IDS_TAG = -1L // 0xFFFFFFFFFFFFFFFF as a signed Long

object TdataReader {

    /** Every directory under [root] (up to [maxDepth] levels down) that has key_* files in it. */
    fun collectCandidateFolders(root: File, maxDepth: Int = 4): List<File> {
        val out = mutableListOf<File>()
        fun walk(dir: File, depthLeft: Int) {
            val files = dir.listFiles() ?: return
            if (files.any { it.isFile && it.name.startsWith("key_") }) {
                out.add(dir)
            }
            if (depthLeft <= 0) return
            for (f in files) {
                if (f.isDirectory) walk(f, depthLeft - 1)
            }
        }
        walk(root, maxDepth)
        return out
    }

    /** Reads every account found across every candidate tdata folder under [root]. */
    fun readForeignTdataAuthorizations(root: File, passcode: ByteArray = ByteArray(0)): List<TdataAccount> {
        val combined = mutableListOf<TdataAccount>()
        for (folder in collectCandidateFolders(root)) {
            combined.addAll(readOneTdataFolder(folder, passcode))
        }
        return combined
    }

    private fun candidateDataNames(folder: File): List<String> {
        val result = mutableListOf("data")
        val files = folder.listFiles() ?: return result
        for (f in files) {
            if (f.isFile && f.name.startsWith("key_")) {
                var name = f.name.substring(4)
                if (name.isNotEmpty() && (name.endsWith("s") || name.endsWith("0") || name.endsWith("1"))) {
                    name = name.dropLast(1)
                }
                if (name.isNotEmpty() && name !in result) result.add(name)
            }
        }
        return result
    }

    fun readOneTdataFolder(folder: File, passcode: ByteArray = ByteArray(0)): List<TdataAccount> {
        for (dataName in candidateDataNames(folder)) {
            val payload = try {
                readTdf(File(folder, "key_$dataName"))
            } catch (e: TdataParseException) {
                continue
            }
            val outer = QReader(payload)
            val salt = outer.readByteArray() ?: continue
            val keyEncrypted = outer.readByteArray() ?: continue
            val infoEncrypted = outer.readByteArray() ?: continue

            val passcodeKey = createLocalKey(passcode, salt)
            val keyInner = try {
                decryptLocal(keyEncrypted, passcodeKey)
            } catch (e: TdataParseException) {
                continue // wrong dataName guess, or this folder needs a passcode
            }
            if (keyInner.size < 256) continue
            val localKey = keyInner.copyOfRange(0, 256)

            val info = try {
                decryptLocal(infoEncrypted, localKey)
            } catch (e: TdataParseException) {
                continue
            }
            val infoReader = QReader(info)
            val count = infoReader.readI32()
            if (count <= 0) continue
            val indices = (0 until count).map { infoReader.readI32() }

            val results = mutableListOf<TdataAccount>()
            for (index in indices) {
                val dataString = composeDataString(dataName, index)
                val fileName = toFilePart(computeDataNameKey(dataString))
                val payload2 = try {
                    readTdf(File(folder, fileName))
                } catch (e: TdataParseException) {
                    continue
                }
                val outer2 = QReader(payload2)
                val encrypted2 = outer2.readByteArray() ?: continue
                val mtpInner = try {
                    decryptLocal(encrypted2, localKey)
                } catch (e: TdataParseException) {
                    continue
                }
                val mtpReader = QReader(mtpInner)
                val blockId = mtpReader.readU32()
                if (blockId != DBI_MTP_AUTHORIZATION) continue
                val serialized = mtpReader.readByteArray() ?: continue
                results.add(parseMtpAuthorization(serialized).copy(sourceFolder = folder))
            }
            if (results.isNotEmpty()) return results
        }
        return emptyList()
    }

    private fun parseMtpAuthorization(serialized: ByteArray): TdataAccount {
        var s = QReader(serialized)
        val wideTagCandidate = s.readU64()
        val userId: Long
        val mainDcId: Int
        if (wideTagCandidate == WIDE_IDS_TAG) {
            userId = s.readU64()
            mainDcId = s.readI32()
        } else {
            s = QReader(serialized) // legacy: two consecutive qint32 fields instead
            userId = s.readI32().toLong()
            mainDcId = s.readI32()
        }
        fun readKeys(): List<Pair<Int, ByteArray>> {
            val count = s.readI32()
            return (0 until count).map {
                val dcId = s.readI32()
                val keyData = s.readRaw(256)
                dcId to keyData
            }
        }
        val keys = readKeys()
        readKeys() // keysToDestroy - irrelevant for import, just consume them
        return TdataAccount(userId, mainDcId, keys)
    }
}
