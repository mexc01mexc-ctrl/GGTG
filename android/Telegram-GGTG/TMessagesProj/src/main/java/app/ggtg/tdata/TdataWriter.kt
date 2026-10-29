package app.ggtg.tdata

import java.io.ByteArrayOutputStream
import java.io.File
import java.security.SecureRandom
import javax.crypto.Cipher
import javax.crypto.spec.SecretKeySpec

// Exact inverse of TdataReader: writes a Telegram Desktop "tdata" folder (TDF files, AES-256-IGE local
// encryption, empty passcode) holding ONE account, mirroring Storage::ExportRawAuthorizationAsTdata
// from the GGTG macOS client. Output is round-trip verified through TdataReader.

object TdataWriter {

    private val rnd = SecureRandom()
    private const val TDF_VERSION = 5_000_000
    private const val DBI_MTP_AUTHORIZATION = 0x4b
    private const val WIDE_IDS_TAG = -1L

    private class QWriter {
        val out = ByteArrayOutputStream()
        fun i32(v: Int) { for (s in intArrayOf(24, 16, 8, 0)) out.write((v shr s) and 0xFF) }
        fun u64(v: Long) { for (s in 56 downTo 0 step 8) out.write(((v ushr s) and 0xFF).toInt()) }
        fun raw(b: ByteArray) = out.write(b)
        fun bytes(b: ByteArray) { i32(b.size); raw(b) }
        fun toByteArray(): ByteArray = out.toByteArray()
    }

    private fun random(n: Int) = ByteArray(n).also { rnd.nextBytes(it) }

    private fun igeEncrypt(data: ByteArray, key: ByteArray, iv: ByteArray): ByteArray {
        require(data.size % 16 == 0)
        val cipher = Cipher.getInstance("AES/ECB/NoPadding")
        cipher.init(Cipher.ENCRYPT_MODE, SecretKeySpec(key, "AES"))
        var prevCipher = iv.copyOfRange(0, 16)
        var prevPlain = iv.copyOfRange(16, 32)
        val out = ByteArray(data.size)
        var i = 0
        while (i < data.size) {
            val plain = data.copyOfRange(i, i + 16)
            val x = ByteArray(16) { (plain[it].toInt() xor prevCipher[it].toInt()).toByte() }
            val enc = cipher.doFinal(x)
            val block = ByteArray(16) { (enc[it].toInt() xor prevPlain[it].toInt()).toByte() }
            System.arraycopy(block, 0, out, i, 16)
            prevCipher = block
            prevPlain = plain
            i += 16
        }
        return out
    }

    /** Storage::details::PrepareEncrypted: [msgKey(16)] + IGE([len LE u32][payload][random pad]). */
    private fun prepareEncrypted(payload: ByteArray, key: ByteArray): ByteArray {
        val dataLen = 4 + payload.size
        val full = if (dataLen % 16 == 0) dataLen else dataLen + (16 - dataLen % 16)
        val buf = ByteArray(full)
        System.arraycopy(leInt32(dataLen), 0, buf, 0, 4)
        System.arraycopy(payload, 0, buf, 4, payload.size)
        val pad = random(full - dataLen)
        System.arraycopy(pad, 0, buf, dataLen, pad.size)
        val msgKey = sha1(buf).copyOfRange(0, 16)
        val (aesKey, aesIv) = prepareAesOldMtp(key, msgKey) // Desktop uses send=false for both directions of local storage
        return msgKey + igeEncrypt(buf, aesKey, aesIv)
    }

    private fun writeTdf(file: File, payload: ByteArray) {
        val version = leInt32(TDF_VERSION)
        val sig = md5(payload, leInt32(payload.size), version, TDF_MAGIC)
        file.writeBytes(TDF_MAGIC + version + payload + sig)
    }

    private fun serializeAuthorization(account: TdataAccount): ByteArray {
        val w = QWriter()
        w.u64(WIDE_IDS_TAG)
        w.u64(account.userId)
        w.i32(account.mainDcId)
        w.i32(account.keys.size)
        for ((dcId, key) in account.keys) { w.i32(dcId); w.raw(key) }
        w.i32(0) // keysToDestroy
        return w.toByteArray()
    }

    /** Writes `<folder>/tdata/{key_datas, <hash>s}`; returns the tdata dir. */
    fun write(account: TdataAccount, folder: File): File {
        val dir = File(folder, "tdata").also { it.mkdirs() }

        // fresh random local key, wrapped with an empty passcode (no password needed on import)
        val localKey = createLocalKey(random(256), random(32))
        val passcodeSalt = random(32)
        val passcodeKey = createLocalKey(ByteArray(0), passcodeSalt)
        val passKeyEncrypted = prepareEncrypted(localKey, passcodeKey)

        val keyPayload = QWriter().apply {
            bytes(passcodeSalt)
            bytes(passKeyEncrypted)
            bytes(prepareEncrypted(QWriter().apply { i32(1); i32(0); i32(0) }.toByteArray(), localKey))
        }.toByteArray()
        writeTdf(File(dir, "key_datas"), keyPayload)

        val mtpInner = QWriter().apply { i32(DBI_MTP_AUTHORIZATION); bytes(serializeAuthorization(account)) }.toByteArray()
        val mtpPayload = QWriter().apply { bytes(prepareEncrypted(mtpInner, localKey)) }.toByteArray()
        writeTdf(File(dir, toFilePart(computeDataNameKey("data")) + "s"), mtpPayload)
        return dir
    }
}
