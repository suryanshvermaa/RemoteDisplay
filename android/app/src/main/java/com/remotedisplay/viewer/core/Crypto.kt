package com.remotedisplay.viewer.core

import java.math.BigInteger
import java.security.KeyFactory
import java.security.KeyPair
import java.security.KeyPairGenerator
import java.security.MessageDigest
import java.security.PrivateKey
import java.security.PublicKey
import java.security.SecureRandom
import java.security.Signature
import java.security.interfaces.ECPublicKey
import java.security.spec.ECGenParameterSpec
import java.security.spec.ECParameterSpec
import java.security.spec.ECPoint
import java.security.spec.ECPublicKeySpec
import java.security.spec.PKCS8EncodedKeySpec
import java.security.spec.X509EncodedKeySpec
import javax.crypto.Cipher
import javax.crypto.KeyAgreement
import javax.crypto.Mac
import javax.crypto.spec.GCMParameterSpec
import javax.crypto.spec.SecretKeySpec

// The handshake and packet encryption of service/include/session.h, with the JDK / Android crypto providers.
// Two details differ from Windows CNG and are converted here:
//  - CNG returns the raw ECDH secret little-endian; Java returns it big-endian (so it is reversed).
//  - CNG ECDSA signatures are raw r|s (64 bytes); Java's are DER (so they are re-encoded).
object Crypto {
    val random = SecureRandom()

    fun randomBytes(n: Int) = ByteArray(n).also { random.nextBytes(it) }

    fun sha256(vararg parts: ByteArray): ByteArray {
        val md = MessageDigest.getInstance("SHA-256")
        for (p in parts) md.update(p)
        return md.digest()
    }

    fun hmac(key: ByteArray, vararg parts: ByteArray): ByteArray {
        val mac = Mac.getInstance("HmacSHA256")
        mac.init(SecretKeySpec(key, "HmacSHA256"))
        for (p in parts) mac.update(p)
        return mac.doFinal()
    }

    fun constEq(a: ByteArray, b: ByteArray): Boolean = MessageDigest.isEqual(a, b)

    fun newEcKeyPair(): KeyPair =
        KeyPairGenerator.getInstance("EC").apply { initialize(ECGenParameterSpec("secp256r1"), random) }.generateKeyPair()

    private fun to32(v: BigInteger): ByteArray {
        val b = v.toByteArray()
        return when {
            b.size == 32 -> b
            b.size > 32 -> b.copyOfRange(b.size - 32, b.size)  // drop the sign byte
            else -> ByteArray(32 - b.size) + b
        }
    }

    // X|Y, 32 bytes each, big-endian: the layout of a CNG ECC public blob after its 8-byte header.
    fun rawPublic(k: PublicKey): ByteArray {
        val w = (k as ECPublicKey).w
        return to32(w.affineX) + to32(w.affineY)
    }

    fun publicFromRaw(raw: ByteArray, params: ECParameterSpec): PublicKey? = try {
        val x = BigInteger(1, raw.copyOfRange(0, 32))
        val y = BigInteger(1, raw.copyOfRange(32, 64))
        KeyFactory.getInstance("EC").generatePublic(ECPublicKeySpec(ECPoint(x, y), params))
    } catch (e: Exception) {
        null
    }

    // DER SEQUENCE { INTEGER r, INTEGER s } -> r|s, 32 bytes each.
    fun derToRaw(der: ByteArray): ByteArray? {
        var i = 0
        fun len(): Int {
            var l = der[i++].toInt() and 0xFF
            if (l and 0x80 != 0) {
                val n = l and 0x7F
                l = 0
                repeat(n) { l = (l shl 8) or (der[i++].toInt() and 0xFF) }
            }
            return l
        }
        return try {
            if (der[i++] != 0x30.toByte()) return null
            len()
            if (der[i++] != 0x02.toByte()) return null
            val rl = len()
            val r = BigInteger(1, der.copyOfRange(i, i + rl)); i += rl
            if (der[i++] != 0x02.toByte()) return null
            val sl = len()
            val s = BigInteger(1, der.copyOfRange(i, i + sl))
            to32(r) + to32(s)
        } catch (e: Exception) {
            null
        }
    }
}

// Ephemeral ECDH P-256.
class Ecdh {
    private val kp = Crypto.newEcKeyPair()
    val pub: ByteArray = Crypto.rawPublic(kp.public)

    // The 32-byte shared secret in CNG's byte order, or null for an invalid peer key.
    fun derive(peerRaw: ByteArray): ByteArray? {
        val peer = Crypto.publicFromRaw(peerRaw, (kp.public as ECPublicKey).params) ?: return null
        return try {
            val ka = KeyAgreement.getInstance("ECDH")
            ka.init(kp.private)
            ka.doPhase(peer, true)
            ka.generateSecret().reversedArray()  // CNG's BCRYPT_KDF_RAW_SECRET is little-endian
        } catch (e: Exception) {
            null
        }
    }
}

// AES-128-GCM with the 64-bit sequence number as nonce (4 zero bytes, then seq little-endian).
private class Aead(key: ByteArray) {
    private val keySpec = SecretKeySpec(key, 0, Proto.KEY_LEN, "AES")
    private val enc = Cipher.getInstance("AES/GCM/NoPadding")
    private val dec = Cipher.getInstance("AES/GCM/NoPadding")

    private fun nonce(seq: Long) = le(12).apply { putInt(0); putLong(seq) }.array()

    // Returns ciphertext | tag.
    fun seal(seq: Long, aad: ByteArray, plain: ByteArray): ByteArray {
        enc.init(Cipher.ENCRYPT_MODE, keySpec, GCMParameterSpec(128, nonce(seq)))
        enc.updateAAD(aad)
        return enc.doFinal(plain)
    }

    // `ct` = ciphertext | tag. Null if it does not authenticate.
    fun open(seq: Long, aad: ByteArray, ct: ByteArray, off: Int, len: Int): ByteArray? = try {
        dec.init(Cipher.DECRYPT_MODE, keySpec, GCMParameterSpec(128, nonce(seq)))
        dec.updateAAD(aad)
        dec.doFinal(ct, off, len)
    } catch (e: Exception) {
        null
    }
}

// One authenticated, encrypted session (the result of a handshake), with replay protection.
class Session(txKey: ByteArray, rxKey: ByteArray, val transcriptHash: ByteArray) {
    private val tx = Aead(txKey)
    private val rx = Aead(rxKey)
    private var txSeq = 0L
    private var maxSeq = 0L
    private var window = 0L
    private var any = false

    private fun wireHeader(type: Int, seq: Long) =
        le(Proto.WIRE_HEADER_LEN).apply { putInt(Proto.WIRE_MAGIC); put(type.toByte()); put(0); put(0); put(0); putLong(seq) }.array()

    // plain -> WireHeader | ciphertext | tag
    @Synchronized
    fun seal(plain: ByteArray): ByteArray {
        val seq = txSeq++
        val h = wireHeader(Proto.W_DATA, seq)
        return h + tx.seal(seq, h, plain)
    }

    // wire -> plain, or null for forgeries and replays.
    fun open(wire: ByteArray, len: Int): ByteArray? {
        if (len < Proto.WIRE_HEADER_LEN + Proto.TAG_LEN) return null
        val seq = le(wire, 8, 8).long
        if (!notReplayed(seq)) return null
        val aad = wire.copyOfRange(0, Proto.WIRE_HEADER_LEN)
        val plain = rx.open(seq, aad, wire, Proto.WIRE_HEADER_LEN, len - Proto.WIRE_HEADER_LEN) ?: return null
        markSeen(seq)
        return plain
    }

    private fun notReplayed(s: Long): Boolean {
        if (!any || s > maxSeq) return true
        val d = maxSeq - s
        return d < 64 && (window and (1L shl d.toInt())) == 0L
    }

    private fun markSeen(s: Long) {
        if (!any || s > maxSeq) {
            val shift = if (any) s - maxSeq else 0
            window = if (shift >= 64) 0 else window shl shift.toInt()
            window = window or 1
            maxSeq = s
            any = true
        } else {
            window = window or (1L shl (maxSeq - s).toInt())
        }
    }

    companion object {
        // Keys from the handshake, exactly as rd::detail::DeriveSession.
        fun derive(psk: ByteArray, hs1: ByteArray, hs2NonceAndPub: ByteArray, shared: ByteArray, isHost: Boolean): Session {
            val th = Crypto.sha256(hs1, hs2NonceAndPub)
            val prk = Crypto.hmac(psk, th, shared)
            val h2c = Crypto.hmac(prk, "rd-h2c".toByteArray()).copyOf(Proto.KEY_LEN)
            val c2h = Crypto.hmac(prk, "rd-c2h".toByteArray()).copyOf(Proto.KEY_LEN)
            return if (isHost) Session(h2c, c2h, th) else Session(c2h, h2c, th)
        }

        fun hostMac(psk: ByteArray, th: ByteArray): ByteArray = Crypto.hmac(psk, "rd-host".toByteArray(), th)
    }
}

// Client side of the handshake: hs1() is sent (and resent) until Hs2 comes back; finish() checks it.
class ClientHandshake {
    private val ecdh = Ecdh()
    val hs1: ByteArray = Crypto.randomBytes(Proto.NONCE_LEN) + ecdh.pub

    // The full Hs1 datagram: WireHeader (type Hs1, seq 0) | hs1.
    fun hs1Wire(): ByteArray =
        le(Proto.WIRE_HEADER_LEN).apply { putInt(Proto.WIRE_MAGIC); put(Proto.W_HS1.toByte()); put(0); put(0); put(0); putLong(0) }.array() + hs1

    // Null when the host could not prove it knows the pairing key (wrong key).
    fun finish(psk: ByteArray, hs2: ByteArray): Session? {
        if (hs2.size != Proto.HS2_LEN) return null
        val hNonceAndPub = hs2.copyOfRange(0, Proto.HS1_LEN)
        val shared = ecdh.derive(hNonceAndPub.copyOfRange(Proto.NONCE_LEN, Proto.HS1_LEN)) ?: return null
        val s = Session.derive(psk, hs1, hNonceAndPub, shared, isHost = false)
        val want = Session.hostMac(psk, s.transcriptHash)
        return if (Crypto.constEq(want, hs2.copyOfRange(Proto.HS1_LEN, Proto.HS2_LEN))) s else null
    }
}

// This device's permanent ECDSA P-256 key: it signs each session so the host can remember ("Always allow") it.
class DeviceIdentity(private val keys: KeyPair) {
    val pub: ByteArray = Crypto.rawPublic(keys.public)

    // Identify payload: pub | signature over this session | name (zero padded).
    fun buildIdentify(transcriptHash: ByteArray, name: String): ByteArray? {
        val digest = Crypto.sha256("rd-device".toByteArray(), transcriptHash)
        val der = try {
            Signature.getInstance("NONEwithECDSA").run {
                initSign(keys.private)
                update(digest)
                sign()
            }
        } catch (e: Exception) {
            return null
        }
        val sig = Crypto.derToRaw(der) ?: return null
        val nameBytes = ByteArray(Proto.DEVICE_NAME_LEN)
        val n = name.filter { it.code in 0x20..0x7E }.toByteArray(Charsets.US_ASCII)
        System.arraycopy(n, 0, nameBytes, 0, minOf(n.size, Proto.DEVICE_NAME_LEN - 1))
        return pub + sig + nameBytes
    }

    fun encodedPrivate(): ByteArray = keys.private.encoded
    fun encodedPublic(): ByteArray = keys.public.encoded

    companion object {
        fun generate() = DeviceIdentity(Crypto.newEcKeyPair())

        fun load(privateEncoded: ByteArray, publicEncoded: ByteArray): DeviceIdentity? = try {
            val kf = KeyFactory.getInstance("EC")
            val priv: PrivateKey = kf.generatePrivate(PKCS8EncodedKeySpec(privateEncoded))
            val pub: PublicKey = kf.generatePublic(X509EncodedKeySpec(publicEncoded))
            DeviceIdentity(KeyPair(pub, priv))
        } catch (e: Exception) {
            null
        }
    }
}
