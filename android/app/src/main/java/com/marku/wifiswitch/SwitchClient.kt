package com.marku.wifiswitch

import android.content.Context
import android.util.Base64
import org.eclipse.paho.client.mqttv3.IMqttDeliveryToken
import org.eclipse.paho.client.mqttv3.MqttCallback
import org.eclipse.paho.client.mqttv3.MqttClient
import org.eclipse.paho.client.mqttv3.MqttConnectOptions
import org.eclipse.paho.client.mqttv3.MqttMessage
import org.eclipse.paho.client.mqttv3.persist.MemoryPersistence
import org.json.JSONException
import org.json.JSONObject
import java.net.HttpURLConnection
import java.net.URL
import java.security.KeyStore
import java.security.MessageDigest
import java.security.cert.CertificateFactory
import java.security.interfaces.ECPublicKey
import javax.net.ssl.SSLContext
import javax.net.ssl.SSLSocketFactory
import javax.net.ssl.TrustManagerFactory

// A state message whose MAC has been verified.
class SwitchState(val internet: String, val led: String, val routerOk: Boolean, val ts: Long)

enum class Kind { OK, SENT, UNREACHABLE, INVALID, ERROR }

class Reply(
    val kind: Kind,
    val state: SwitchState? = null,
    val message: String = "",
    val viaRelay: Boolean = false
)

class PairResult(val ok: Boolean, val message: String)

class SwitchClient(context: Context, private val store: SecureStore) {

    private val appContext = context.applicationContext

    private class Http(val code: Int, val json: JSONObject?)

    // Relay state (touched from the Paho thread and the executor thread).
    @Volatile private var relayState: SwitchState? = null
    @Volatile private var lastRelayTs: Long = 0
    @Volatile var relayListener: ((SwitchState) -> Unit)? = null
    private var mqtt: MqttClient? = null
    private var relayDevice: String? = null

    // ---------- HTTP ----------

    private fun http(method: String, url: String, body: String?, connectMs: Int, readMs: Int): Http {
        val conn = URL(url).openConnection() as HttpURLConnection
        try {
            conn.requestMethod = method
            conn.connectTimeout = connectMs
            conn.readTimeout = readMs
            conn.useCaches = false
            if (body != null) {
                val bytes = body.toByteArray(Charsets.UTF_8)
                conn.doOutput = true
                conn.setRequestProperty("Content-Type", "application/json")
                conn.setFixedLengthStreamingMode(bytes.size)
                conn.outputStream.use { it.write(bytes) }
            }
            val code = conn.responseCode
            val stream = if (code in 200..299) conn.inputStream else conn.errorStream
            val text = stream?.bufferedReader()?.use { it.readText() } ?: ""
            val json = try {
                JSONObject(text)
            } catch (e: JSONException) {
                null
            }
            return Http(code, json)
        } finally {
            conn.disconnect()
        }
    }

    // ---------- state verification ----------

    private fun macOk(p: Paired, j: JSONObject, expectEcho: String): SwitchState? {
        try {
            if (j.getInt("v") != 1) return null
            val internet = j.getString("internet")
            val led = j.getString("led")
            val routerOk = j.getBoolean("router_ok")
            val ts = j.getLong("ts")
            val echo = j.getString("echo")
            if (echo != expectEcho) return null
            val key = store.authKey() ?: return null
            val canonical = "v1|state|${p.deviceId}|$internet|$led|${if (routerOk) 1 else 0}|$ts|$echo"
            val expected = Crypto.hmacWithKey(key, canonical.toByteArray(Charsets.UTF_8))
            val got = Crypto.fromHex(j.getJSONObject("macs").getString(p.kid.toString()))
            if (!MessageDigest.isEqual(expected, got)) return null
            return SwitchState(internet, led, routerOk, ts)
        } catch (e: Exception) {
            return null
        }
    }

    private fun toReply(h: Http, p: Paired, echo: String): Reply {
        if (h.code == 200) {
            val s = h.json?.let { macOk(p, it, echo) }
            return if (s != null) Reply(Kind.OK, s) else Reply(Kind.INVALID)
        }
        val err = h.json?.optString("error", "") ?: ""
        return Reply(Kind.ERROR, null, if (err.isEmpty()) "HTTP ${h.code}" else err)
    }

    // ---------- LAN ----------

    fun challenge(address: String): String? {
        val h = http("GET", "http://$address/api/challenge", null, 3000, 15000)
        if (h.code != 200) return null
        val n = h.json?.optString("nonce", "") ?: ""
        return if (n.isEmpty()) null else n
    }

    fun state(p: Paired): Reply {
        val echo = Crypto.randomHex(8)
        val h = try {
            http("GET", "http://${p.address}/api/state?echo=$echo", null, 3000, 15000)
        } catch (e: Exception) {
            return Reply(Kind.UNREACHABLE)
        }
        return toReply(h, p, echo)
    }

    private fun commandJson(p: Paired, key: java.security.Key, op: String, arg: String, ts: Long, nonce: String): String {
        val canonical = "v1|${p.kid}|$op|$arg|$ts|$nonce"
        val mac = Crypto.toHex(Crypto.hmacWithKey(key, canonical.toByteArray(Charsets.UTF_8)))
        return JSONObject()
            .put("v", 1)
            .put("kid", p.kid)
            .put("op", op)
            .put("arg", arg)
            .put("ts", ts)
            .put("nonce", nonce)
            .put("mac", mac)
            .toString()
    }

    fun sendCommand(p: Paired, op: String, arg: String): Reply {
        val key = store.authKey() ?: return Reply(Kind.INVALID)
        try {
            val nonce = challenge(p.address)
            if (nonce == null) return Reply(Kind.ERROR, null, "no challenge")
            val body = commandJson(p, key, op, arg, 0L, nonce)
            val h = http("POST", "http://${p.address}/api/cmd", body, 3000, 60000)
            return toReply(h, p, nonce)
        } catch (e: Exception) {
            // LAN not reachable: fall back to the relay below.
        }
        return sendRelay(p, key, op, arg)
    }

    // ---------- pairing ----------

    fun pair(address: String, pin: String): PairResult {
        var priv: java.security.PrivateKey? = null
        var z: ByteArray? = null
        var kAuth: ByteArray? = null
        var kWrap: ByteArray? = null
        try {
            val kp = Crypto.generateKeyPair()
            priv = kp.private
            val appPub = Crypto.encodePoint(kp.public)
            val proof = Crypto.toHex(Crypto.hmac(pin.toByteArray(Charsets.UTF_8), appPub))
            val req = JSONObject()
                .put("v", 1)
                .put("pub", Base64.encodeToString(appPub, Base64.NO_WRAP))
                .put("pin_proof", proof)
                .toString()
            val h = try {
                http("POST", "http://$address/api/pair", req, 5000, 40000)
            } catch (e: Exception) {
                return PairResult(false, "Can't reach the switch. Are you on the home Wi-Fi?")
            }
            when (h.code) {
                200 -> {}
                403 -> return PairResult(false, "Wrong PIN")
                408 -> return PairResult(false, "You didn't press BOOT on the switch in time")
                429 -> return PairResult(false, "Too many wrong PINs, wait 10 minutes")
                507 -> return PairResult(false, "4 phones already paired — hold BOOT 10 s on the switch to reset")
                else -> return PairResult(false, "Pairing failed (HTTP ${h.code})")
            }
            val j = h.json ?: return PairResult(false, "Pairing failed (bad reply)")
            val espPub = Base64.decode(j.getString("pub"), Base64.NO_WRAP)
            val box = Base64.decode(j.getString("box"), Base64.NO_WRAP)
            val params = (kp.public as ECPublicKey).params
            val espKey = Crypto.decodePoint(espPub, params)
            z = Crypto.ecdh(kp.private, espKey)
            val keys = Crypto.deriveKeys(z, appPub, espPub)
            kAuth = keys.first
            kWrap = keys.second
            val plain = Crypto.openBox(kWrap, box)
            val d = JSONObject(String(plain, Charsets.UTF_8))
            plain.fill(0)
            val deviceId = d.getString("device_id")
            val kid = d.getInt("kid")
            if (deviceId.length != 32 || kid < 0 || kid > 3) {
                return PairResult(false, "Pairing failed (bad reply)")
            }
            val paired = Paired(
                deviceId,
                kid,
                address,
                d.optString("mqtt_host", ""),
                d.optInt("mqtt_port", 0),
                d.optString("mqtt_user", ""),
                d.optString("mqtt_pass", "")
            )
            try {
                store.putAuthKey(kAuth)  // zeroes kAuth
                store.save(paired)
            } catch (e: Exception) {
                store.clear()
                return PairResult(false, "Could not store the key securely")
            }
            return PairResult(true, "")
        } catch (e: Exception) {
            return PairResult(false, "Pairing failed (invalid reply from switch)")
        } finally {
            z?.fill(0)
            kWrap?.fill(0)
            kAuth?.fill(0)
            priv = null
        }
    }

    // ---------- relay ----------

    private fun trustedSocketFactory(): SSLSocketFactory {
        val certs = appContext.resources.openRawResource(R.raw.isrg_roots).use {
            CertificateFactory.getInstance("X.509").generateCertificates(it)
        }
        val ks = KeyStore.getInstance(KeyStore.getDefaultType())
        ks.load(null, null)
        var i = 0
        for (c in certs) {
            ks.setCertificateEntry("isrg$i", c)
            i++
        }
        val tmf = TrustManagerFactory.getInstance(TrustManagerFactory.getDefaultAlgorithm())
        tmf.init(ks)
        val ctx = SSLContext.getInstance("TLS")
        ctx.init(null, tmf.trustManagers, null)
        return ctx.socketFactory
    }

    private fun handleRelayMessage(p: Paired, payload: ByteArray) {
        try {
            val j = JSONObject(String(payload, Charsets.UTF_8))
            val s = macOk(p, j, "") ?: return
            val now = System.currentTimeMillis() / 1000
            if (Math.abs(now - s.ts) > 900) return
            if (s.ts <= lastRelayTs) return
            lastRelayTs = s.ts
            relayState = s
            relayListener?.invoke(s)
        } catch (e: Exception) {
            // ignore malformed messages
        }
    }

    @Synchronized
    private fun ensureRelay(p: Paired): Boolean {
        if (!p.relayConfigured()) return false
        val existing = mqtt
        if (existing != null && existing.isConnected && relayDevice == p.deviceId) return true
        disconnectRelayLocked()
        var c: MqttClient? = null
        try {
            c = MqttClient("ssl://${p.mqttHost}:${p.mqttPort}", "ws-" + Crypto.randomHex(8), MemoryPersistence())
            c.setCallback(object : MqttCallback {
                override fun connectionLost(cause: Throwable?) {}

                override fun messageArrived(topic: String?, message: MqttMessage?) {
                    if (message != null) handleRelayMessage(p, message.payload)
                }

                override fun deliveryComplete(token: IMqttDeliveryToken?) {}
            })
            val opts = MqttConnectOptions()
            opts.isCleanSession = true
            opts.userName = p.mqttUser
            opts.password = p.mqttPass.toCharArray()
            opts.connectionTimeout = 10
            opts.keepAliveInterval = 60
            opts.socketFactory = trustedSocketFactory()
            opts.isHttpsHostnameVerificationEnabled = true
            c.connect(opts)
            c.subscribe("wifiswitch/${p.deviceId}/state", 1)
            if (relayDevice != p.deviceId) {
                relayState = null
                lastRelayTs = 0
            }
            mqtt = c
            relayDevice = p.deviceId
            return true
        } catch (e: Exception) {
            try {
                c?.close()
            } catch (e2: Exception) {
            }
            return false
        }
    }

    @Synchronized
    private fun disconnectRelayLocked() {
        val c = mqtt
        mqtt = null
        if (c != null) {
            try {
                c.disconnect(1000)
            } catch (e: Exception) {
            }
            try {
                c.close()
            } catch (e: Exception) {
            }
        }
    }

    fun disconnectRelay() {
        disconnectRelayLocked()
    }

    // Connects lazily; returns the newest verified, fresh relayed state (may be null
    // right after subscribing, the retained message arrives via relayListener).
    fun relayLatest(p: Paired): SwitchState? {
        if (!ensureRelay(p)) return null
        val s = relayState ?: return null
        val now = System.currentTimeMillis() / 1000
        return if (Math.abs(now - s.ts) <= 900) s else null
    }

    private fun sendRelay(p: Paired, key: java.security.Key, op: String, arg: String): Reply {
        if (!p.relayConfigured()) return Reply(Kind.UNREACHABLE)
        if (!ensureRelay(p)) return Reply(Kind.UNREACHABLE)
        return try {
            val ts = System.currentTimeMillis() / 1000
            val body = commandJson(p, key, op, arg, ts, Crypto.randomHex(8))
            val msg = MqttMessage(body.toByteArray(Charsets.UTF_8))
            msg.qos = 1
            msg.isRetained = false
            val c = mqtt
            if (c == null) {
                Reply(Kind.UNREACHABLE)
            } else {
                c.publish("wifiswitch/${p.deviceId}/cmd", msg)
                Reply(Kind.SENT, null, "", true)
            }
        } catch (e: Exception) {
            Reply(Kind.UNREACHABLE)
        }
    }
}
