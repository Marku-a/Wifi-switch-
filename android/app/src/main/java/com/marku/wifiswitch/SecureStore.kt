package com.marku.wifiswitch

import android.content.Context
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import android.security.keystore.KeyProtection
import android.util.Base64
import org.json.JSONObject
import java.security.Key
import java.security.KeyStore
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey
import javax.crypto.spec.GCMParameterSpec
import javax.crypto.spec.SecretKeySpec

// Everything learned while pairing (never the Wi-Fi / router / app PIN).
class Paired(
    val deviceId: String,
    val kid: Int,
    val address: String,
    val mqttHost: String,
    val mqttPort: Int,
    val mqttUser: String,
    val mqttPass: String
) {
    fun relayConfigured(): Boolean = mqttHost.isNotEmpty() && mqttPort > 0
}

class SecureStore(context: Context) {

    private val prefs = context.applicationContext.getSharedPreferences("secure", Context.MODE_PRIVATE)

    private fun keyStore(): KeyStore {
        val ks = KeyStore.getInstance("AndroidKeyStore")
        ks.load(null)
        return ks
    }

    fun isPaired(): Boolean =
        prefs.contains("blob") && try {
            keyStore().containsAlias(AUTH_ALIAS)
        } catch (e: Exception) {
            false
        }

    // Imports K_auth into the Keystore (non-exportable), then zeroes the array.
    fun putAuthKey(k: ByteArray) {
        try {
            keyStore().setEntry(
                AUTH_ALIAS,
                KeyStore.SecretKeyEntry(SecretKeySpec(k, "HmacSHA256")),
                KeyProtection.Builder(KeyProperties.PURPOSE_SIGN).build()
            )
        } finally {
            k.fill(0)
        }
    }

    fun authKey(): Key? = try {
        keyStore().getKey(AUTH_ALIAS, null)
    } catch (e: Exception) {
        null
    }

    private fun storeKey(): SecretKey {
        val ks = keyStore()
        val existing = ks.getKey(STORE_ALIAS, null) as? SecretKey
        if (existing != null) return existing
        val gen = KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore")
        gen.init(
            KeyGenParameterSpec.Builder(
                STORE_ALIAS,
                KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT
            )
                .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                .setKeySize(256)
                .build()
        )
        return gen.generateKey()
    }

    fun save(p: Paired) {
        val json = JSONObject()
            .put("device_id", p.deviceId)
            .put("kid", p.kid)
            .put("address", p.address)
            .put("mqtt_host", p.mqttHost)
            .put("mqtt_port", p.mqttPort)
            .put("mqtt_user", p.mqttUser)
            .put("mqtt_pass", p.mqttPass)
        val cipher = Cipher.getInstance("AES/GCM/NoPadding")
        cipher.init(Cipher.ENCRYPT_MODE, storeKey())
        val iv = cipher.iv
        val ct = cipher.doFinal(json.toString().toByteArray(Charsets.UTF_8))
        val blob = ByteArray(iv.size + ct.size)
        System.arraycopy(iv, 0, blob, 0, iv.size)
        System.arraycopy(ct, 0, blob, iv.size, ct.size)
        prefs.edit().putString("blob", Base64.encodeToString(blob, Base64.NO_WRAP)).commit()
    }

    fun load(): Paired? {
        try {
            val s = prefs.getString("blob", null) ?: return null
            val blob = Base64.decode(s, Base64.NO_WRAP)
            if (blob.size < 12 + 16) return null
            val cipher = Cipher.getInstance("AES/GCM/NoPadding")
            val key = keyStore().getKey(STORE_ALIAS, null) as? SecretKey ?: return null
            cipher.init(Cipher.DECRYPT_MODE, key, GCMParameterSpec(128, blob, 0, 12))
            val plain = cipher.doFinal(blob, 12, blob.size - 12)
            val j = JSONObject(String(plain, Charsets.UTF_8))
            return Paired(
                j.getString("device_id"),
                j.getInt("kid"),
                j.getString("address"),
                j.optString("mqtt_host", ""),
                j.optInt("mqtt_port", 0),
                j.optString("mqtt_user", ""),
                j.optString("mqtt_pass", "")
            )
        } catch (e: Exception) {
            return null
        }
    }

    fun clear() {
        try {
            val ks = keyStore()
            if (ks.containsAlias(AUTH_ALIAS)) ks.deleteEntry(AUTH_ALIAS)
            if (ks.containsAlias(STORE_ALIAS)) ks.deleteEntry(STORE_ALIAS)
        } catch (e: Exception) {
            // nothing more we can do
        }
        prefs.edit().clear().commit()
    }

    companion object {
        private const val AUTH_ALIAS = "ws_auth"
        private const val STORE_ALIAS = "ws_store"
    }
}
