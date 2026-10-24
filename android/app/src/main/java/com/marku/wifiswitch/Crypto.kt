package com.marku.wifiswitch

import java.math.BigInteger
import java.security.Key
import java.security.KeyFactory
import java.security.KeyPair
import java.security.KeyPairGenerator
import java.security.PrivateKey
import java.security.PublicKey
import java.security.SecureRandom
import java.security.interfaces.ECPublicKey
import java.security.spec.ECFieldFp
import java.security.spec.ECGenParameterSpec
import java.security.spec.ECParameterSpec
import java.security.spec.ECPoint
import java.security.spec.ECPublicKeySpec
import javax.crypto.Cipher
import javax.crypto.KeyAgreement
import javax.crypto.Mac
import javax.crypto.spec.GCMParameterSpec
import javax.crypto.spec.SecretKeySpec

// Primitives of docs/SECURITY.md (protocol v1).
object Crypto {
    private val rng = SecureRandom()
    private val AUTH_LABEL = "wifiswitch-auth-v1".toByteArray(Charsets.UTF_8)
    private val WRAP_LABEL = "wifiswitch-wrap-v1".toByteArray(Charsets.UTF_8)

    fun generateKeyPair(): KeyPair {
        val gen = KeyPairGenerator.getInstance("EC")
        gen.initialize(ECGenParameterSpec("secp256r1"), rng)
        return gen.generateKeyPair()
    }

    // 32-byte big-endian, left-padded / leading-zero-trimmed.
    private fun fixed32(v: BigInteger): ByteArray {
        val b = v.toByteArray()
        if (b.size == 32) return b
        val out = ByteArray(32)
        if (b.size > 32) {
            System.arraycopy(b, b.size - 32, out, 0, 32)
        } else {
            System.arraycopy(b, 0, out, 32 - b.size, b.size)
        }
        return out
    }

    // 0x04 || X || Y
    fun encodePoint(pub: PublicKey): ByteArray {
        val w = (pub as ECPublicKey).w
        val out = ByteArray(65)
        out[0] = 4
        System.arraycopy(fixed32(w.affineX), 0, out, 1, 32)
        System.arraycopy(fixed32(w.affineY), 0, out, 33, 32)
        return out
    }

    // Decodes a raw point and rejects anything not on the curve.
    fun decodePoint(raw: ByteArray, params: ECParameterSpec): PublicKey {
        require(raw.size == 65 && raw[0].toInt() == 4) { "bad point" }
        val x = BigInteger(1, raw.copyOfRange(1, 33))
        val y = BigInteger(1, raw.copyOfRange(33, 65))
        val field = params.curve.field as? ECFieldFp ?: throw IllegalArgumentException("bad curve")
        val p = field.p
        require(x.signum() >= 0 && x < p && y.signum() >= 0 && y < p) { "bad point" }
        val lhs = y.multiply(y).mod(p)
        val rhs = x.multiply(x).multiply(x)
            .add(params.curve.a.multiply(x))
            .add(params.curve.b)
            .mod(p)
        require(lhs == rhs) { "point not on curve" }
        return KeyFactory.getInstance("EC").generatePublic(ECPublicKeySpec(ECPoint(x, y), params))
    }

    // Z = 32-byte X coordinate
    fun ecdh(priv: PrivateKey, pub: PublicKey): ByteArray {
        val ka = KeyAgreement.getInstance("ECDH")
        ka.init(priv)
        ka.doPhase(pub, true)
        return ka.generateSecret()
    }

    fun hmac(key: ByteArray, vararg parts: ByteArray): ByteArray {
        val mac = Mac.getInstance("HmacSHA256")
        mac.init(SecretKeySpec(key, "HmacSHA256"))
        for (p in parts) mac.update(p)
        return mac.doFinal()
    }

    // HMAC with a (Keystore) key object.
    fun hmacWithKey(key: Key, msg: ByteArray): ByteArray {
        val mac = Mac.getInstance("HmacSHA256")
        mac.init(key)
        return mac.doFinal(msg)
    }

    // Returns (K_auth, K_wrap).
    fun deriveKeys(z: ByteArray, appPub: ByteArray, espPub: ByteArray): Pair<ByteArray, ByteArray> {
        val kAuth = hmac(z, AUTH_LABEL, appPub, espPub)
        val kWrap = hmac(z, WRAP_LABEL, appPub, espPub)
        return Pair(kAuth, kWrap)
    }

    // box = iv(12) || ciphertext || tag(16), AES-256-GCM, no AAD.
    fun openBox(kWrap: ByteArray, box: ByteArray): ByteArray {
        require(box.size >= 12 + 16) { "box too short" }
        val cipher = Cipher.getInstance("AES/GCM/NoPadding")
        cipher.init(Cipher.DECRYPT_MODE, SecretKeySpec(kWrap, "AES"), GCMParameterSpec(128, box, 0, 12))
        return cipher.doFinal(box, 12, box.size - 12)
    }

    fun toHex(b: ByteArray): String {
        val sb = StringBuilder(b.size * 2)
        for (x in b) {
            val v = x.toInt() and 0xff
            sb.append("0123456789abcdef"[v shr 4])
            sb.append("0123456789abcdef"[v and 15])
        }
        return sb.toString()
    }

    fun fromHex(s: String): ByteArray {
        require(s.length % 2 == 0) { "bad hex" }
        val out = ByteArray(s.length / 2)
        for (i in out.indices) {
            val hi = Character.digit(s[2 * i], 16)
            val lo = Character.digit(s[2 * i + 1], 16)
            require(hi >= 0 && lo >= 0) { "bad hex" }
            out[i] = ((hi shl 4) or lo).toByte()
        }
        return out
    }

    fun randomHex(nBytes: Int): String {
        val b = ByteArray(nBytes)
        rng.nextBytes(b)
        return toHex(b)
    }
}
