package com.marku.wifiswitch

import android.app.Activity
import android.content.Context
import android.graphics.Color
import android.graphics.drawable.GradientDrawable
import android.net.nsd.NsdManager
import android.net.nsd.NsdServiceInfo
import android.net.wifi.WifiManager
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.View
import android.widget.Button
import android.widget.EditText
import android.widget.TextView
import android.widget.Toast
import org.json.JSONObject
import java.net.HttpURLConnection
import java.net.URL
import java.util.concurrent.Executors

class MainActivity : Activity() {

    // Result of one HTTP call. code = -1 means the switch could not be reached.
    private class ApiResult(val code: Int, val json: JSONObject?)

    private val colorGreen = Color.parseColor("#2E7D32")
    private val colorRed = Color.parseColor("#C62828")
    private val colorYellow = Color.parseColor("#F9A825")
    private val colorGrey = Color.parseColor("#9E9E9E")

    private val executor = Executors.newSingleThreadExecutor()
    private val handler = Handler(Looper.getMainLooper())

    private lateinit var statusDot: View
    private lateinit var statusText: TextView
    private lateinit var toggleButton: Button
    private lateinit var addressInput: EditText
    private lateinit var pinInput: EditText
    private lateinit var findButton: Button
    private val dotDrawable = GradientDrawable()

    private var inFlight = false
    private var currentInternet: String? = null  // "on" / "off" / null when unknown
    private var blinking = false
    private var blinkLow = false

    // NSD discovery state (main thread only, except where noted)
    private var nsdManager: NsdManager? = null
    private var discoveryListener: NsdManager.DiscoveryListener? = null
    private var multicastLock: WifiManager.MulticastLock? = null
    private var resolving = false

    private val pollRunnable = object : Runnable {
        override fun run() {
            fetchStatus()
            handler.postDelayed(this, 5000)
        }
    }

    private val blinkRunnable = object : Runnable {
        override fun run() {
            if (blinking) {
                blinkLow = !blinkLow
                statusDot.alpha = if (blinkLow) 0.25f else 1f
                handler.postDelayed(this, 500)
            }
        }
    }

    private val stopDiscoveryRunnable = Runnable { stopDiscovery(true) }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        statusDot = findViewById(R.id.statusDot)
        statusText = findViewById(R.id.statusText)
        toggleButton = findViewById(R.id.toggleButton)
        addressInput = findViewById(R.id.addressInput)
        pinInput = findViewById(R.id.pinInput)
        findButton = findViewById(R.id.findButton)
        val saveButton: Button = findViewById(R.id.saveButton)

        dotDrawable.shape = GradientDrawable.OVAL
        dotDrawable.setColor(colorGrey)
        statusDot.background = dotDrawable

        addressInput.setText(address())
        pinInput.setText(pin())

        saveButton.setOnClickListener {
            saveSetting("address", addressInput.text.toString().trim().ifEmpty { DEFAULT_ADDRESS })
            saveSetting("pin", pinInput.text.toString())
            Toast.makeText(this, "Saved", Toast.LENGTH_SHORT).show()
            fetchStatus()
        }
        findButton.setOnClickListener { startDiscovery() }
        toggleButton.setOnClickListener { onToggleClicked() }
    }

    override fun onResume() {
        super.onResume()
        handler.post(pollRunnable)
    }

    override fun onPause() {
        handler.removeCallbacks(pollRunnable)
        stopDiscovery(false)
        super.onPause()
    }

    override fun onDestroy() {
        executor.shutdown()
        super.onDestroy()
    }

    // ---------- settings ----------

    private fun prefs() = getSharedPreferences("settings", Context.MODE_PRIVATE)

    private fun address(): String = prefs().getString("address", DEFAULT_ADDRESS) ?: DEFAULT_ADDRESS

    private fun pin(): String = prefs().getString("pin", "") ?: ""

    private fun saveSetting(key: String, value: String) {
        prefs().edit().putString(key, value).apply()
    }

    // ---------- networking ----------

    private fun call(method: String, path: String, timeoutMs: Int): ApiResult {
        var conn: HttpURLConnection? = null
        try {
            conn = URL("http://${address()}$path").openConnection() as HttpURLConnection
            conn.requestMethod = method
            conn.connectTimeout = timeoutMs
            conn.readTimeout = timeoutMs
            conn.setRequestProperty("X-Pin", pin())
            val code = conn.responseCode
            if (code == 401) return ApiResult(401, null)
            val stream = if (code in 200..299) conn.inputStream else conn.errorStream
            val body = stream?.bufferedReader()?.use { it.readText() } ?: ""
            return ApiResult(code, JSONObject(body))
        } catch (e: Exception) {
            return ApiResult(-1, null)
        } finally {
            conn?.disconnect()
        }
    }

    private fun fetchStatus() {
        if (inFlight) return
        executor.execute {
            val result = call("GET", "/api/status", 5000)
            runOnUiThread { if (!inFlight) render(result) }
        }
    }

    private fun onToggleClicked() {
        val target = if (currentInternet == "on") "off" else "on"
        inFlight = true
        toggleButton.isEnabled = false
        setStatus(colorYellow, false, "Working…")
        executor.execute {
            val result = call("POST", "/api/set?state=$target", 60000)
            runOnUiThread {
                inFlight = false
                render(result)
            }
        }
    }

    // ---------- rendering ----------

    private fun setStatus(color: Int, blink: Boolean, text: String) {
        dotDrawable.setColor(color)
        statusText.text = text
        handler.removeCallbacks(blinkRunnable)
        blinking = blink
        blinkLow = false
        statusDot.alpha = 1f
        if (blink) handler.postDelayed(blinkRunnable, 500)
    }

    private fun render(result: ApiResult) {
        if (result.code == 401) {
            currentInternet = null
            setStatus(colorGrey, false, "Wrong PIN")
            toggleButton.text = "Turn internet ON"
            toggleButton.isEnabled = false
            return
        }
        val json = result.json
        if (result.code != 200 || json == null) {
            currentInternet = null
            setStatus(colorGrey, false, "Can't reach the switch. Are you on the home Wi-Fi?")
            toggleButton.text = "Turn internet ON"
            toggleButton.isEnabled = false
            return
        }
        currentInternet = json.optString("internet", "")
        when (json.optString("led", "")) {
            "online" -> setStatus(colorGreen, false, "Internet is ON")
            "blocked" -> setStatus(colorRed, false, "Internet is OFF")
            "no_internet" ->
                setStatus(colorRed, true, "Internet ON, but no connection from provider")
            "problem" -> setStatus(colorYellow, false, "Problem: switch can't control the router")
            else -> setStatus(colorYellow, false, "Working…")
        }
        toggleButton.text =
            if (currentInternet == "on") "Turn internet OFF" else "Turn internet ON"
        toggleButton.isEnabled = !inFlight
    }

    // ---------- discovery ----------

    private fun startDiscovery() {
        if (discoveryListener != null) return
        val nsd = getSystemService(Context.NSD_SERVICE) as NsdManager
        nsdManager = nsd

        val wifi = applicationContext.getSystemService(Context.WIFI_SERVICE) as WifiManager
        val lock = wifi.createMulticastLock("wifiswitch")
        lock.setReferenceCounted(false)
        lock.acquire()
        multicastLock = lock

        findButton.isEnabled = false
        resolving = false

        val listener = object : NsdManager.DiscoveryListener {
            override fun onStartDiscoveryFailed(serviceType: String, errorCode: Int) {
                runOnUiThread {
                    toast("Search failed (error $errorCode)")
                    stopDiscovery(false)
                }
            }

            override fun onStopDiscoveryFailed(serviceType: String, errorCode: Int) {}

            override fun onDiscoveryStarted(serviceType: String) {}

            override fun onDiscoveryStopped(serviceType: String) {}

            override fun onServiceFound(serviceInfo: NsdServiceInfo) {
                if (!serviceInfo.serviceName.startsWith("wifiswitch")) return
                runOnUiThread { resolve(serviceInfo) }
            }

            override fun onServiceLost(serviceInfo: NsdServiceInfo) {}
        }
        discoveryListener = listener
        try {
            nsd.discoverServices("_http._tcp.", NsdManager.PROTOCOL_DNS_SD, listener)
        } catch (e: Exception) {
            toast("Search failed")
            stopDiscovery(false)
            return
        }
        toast("Searching…")
        handler.postDelayed(stopDiscoveryRunnable, 10000)
    }

    private fun resolve(info: NsdServiceInfo) {
        if (discoveryListener == null || resolving) return
        resolving = true
        val nsd = nsdManager ?: return
        nsd.resolveService(info, object : NsdManager.ResolveListener {
            override fun onResolveFailed(serviceInfo: NsdServiceInfo, errorCode: Int) {
                runOnUiThread { resolving = false }
            }

            override fun onServiceResolved(serviceInfo: NsdServiceInfo) {
                val host = serviceInfo.host
                val ip = host?.hostAddress
                runOnUiThread {
                    if (ip != null && discoveryListener != null) {
                        saveSetting("address", ip)
                        addressInput.setText(ip)
                        toast("Found switch at $ip")
                        stopDiscovery(false)
                        fetchStatus()
                    } else {
                        resolving = false
                    }
                }
            }
        })
    }

    // reportNotFound=true only for the 10 s timeout.
    private fun stopDiscovery(reportNotFound: Boolean) {
        handler.removeCallbacks(stopDiscoveryRunnable)
        val listener = discoveryListener
        if (listener != null) {
            discoveryListener = null
            try {
                nsdManager?.stopServiceDiscovery(listener)
            } catch (e: Exception) {
                // already stopped
            }
            if (reportNotFound) toast("Switch not found. Enter its address by hand.")
        }
        multicastLock?.let { if (it.isHeld) it.release() }
        multicastLock = null
        resolving = false
        findButton.isEnabled = true
    }

    private fun toast(msg: String) {
        Toast.makeText(this, msg, Toast.LENGTH_SHORT).show()
    }

    companion object {
        private const val DEFAULT_ADDRESS = "wifiswitch.local"
    }
}
