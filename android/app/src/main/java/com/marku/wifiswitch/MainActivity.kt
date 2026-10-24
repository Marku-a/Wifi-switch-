package com.marku.wifiswitch

import android.app.Activity
import android.app.AlertDialog
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
import android.view.WindowManager
import android.widget.Button
import android.widget.EditText
import android.widget.TextView
import android.widget.Toast
import java.util.concurrent.Executors

class MainActivity : Activity() {

    private val colorGreen = Color.parseColor("#2E7D32")
    private val colorRed = Color.parseColor("#C62828")
    private val colorYellow = Color.parseColor("#F9A825")
    private val colorGrey = Color.parseColor("#9E9E9E")

    private val executor = Executors.newSingleThreadExecutor()
    private val handler = Handler(Looper.getMainLooper())

    private lateinit var store: SecureStore
    private lateinit var client: SwitchClient

    private lateinit var unpairedGroup: View
    private lateinit var pairedGroup: View
    private lateinit var statusDot: View
    private lateinit var statusText: TextView
    private lateinit var viaText: TextView
    private lateinit var toggleButton: Button
    private lateinit var unpairButton: Button
    private lateinit var addressInput: EditText
    private lateinit var pinInput: EditText
    private lateinit var findButton: Button
    private lateinit var pairButton: Button
    private lateinit var pairStatus: TextView
    private val dotDrawable = GradientDrawable()

    // Main-thread state
    private var paired: Paired? = null
    private var pollBusy = false
    private var cmdBusy = false
    private var pairing = false
    private var failures = 0
    private var lastState: SwitchState? = null  // always a verified state
    private var lastViaRelay = false
    private var blinking = false
    private var blinkLow = false

    // NSD discovery state (main thread only, except where noted)
    private var nsdManager: NsdManager? = null
    private var discoveryListener: NsdManager.DiscoveryListener? = null
    private var multicastLock: WifiManager.MulticastLock? = null
    private var resolving = false

    private val pollRunnable = object : Runnable {
        override fun run() {
            poll()
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
        window.setFlags(
            WindowManager.LayoutParams.FLAG_SECURE,
            WindowManager.LayoutParams.FLAG_SECURE
        )
        setContentView(R.layout.activity_main)

        store = SecureStore(this)
        client = SwitchClient(this, store)

        unpairedGroup = findViewById(R.id.unpairedGroup)
        pairedGroup = findViewById(R.id.pairedGroup)
        statusDot = findViewById(R.id.statusDot)
        statusText = findViewById(R.id.statusText)
        viaText = findViewById(R.id.viaText)
        toggleButton = findViewById(R.id.toggleButton)
        unpairButton = findViewById(R.id.unpairButton)
        addressInput = findViewById(R.id.addressInput)
        pinInput = findViewById(R.id.pinInput)
        findButton = findViewById(R.id.findButton)
        pairButton = findViewById(R.id.pairButton)
        pairStatus = findViewById(R.id.pairStatus)

        dotDrawable.shape = GradientDrawable.OVAL
        dotDrawable.setColor(colorGrey)
        statusDot.background = dotDrawable

        addressInput.setText(DEFAULT_ADDRESS)

        findButton.setOnClickListener { startDiscovery() }
        pairButton.setOnClickListener { onPairClicked() }
        toggleButton.setOnClickListener { onToggleClicked() }
        unpairButton.setOnClickListener { onUnpairClicked() }

        client.relayListener = { s -> runOnUiThread { onRelayState(s) } }

        if (store.isPaired()) paired = store.load()
        if (paired != null) showPaired() else showUnpaired()
    }

    override fun onResume() {
        super.onResume()
        if (paired != null) handler.post(pollRunnable)
    }

    override fun onPause() {
        handler.removeCallbacks(pollRunnable)
        stopDiscovery(false)
        executor.execute { client.disconnectRelay() }
        super.onPause()
    }

    override fun onDestroy() {
        client.relayListener = null
        executor.shutdown()
        super.onDestroy()
    }

    // ---------- modes ----------

    private fun showUnpaired() {
        paired = null
        lastState = null
        failures = 0
        handler.removeCallbacks(pollRunnable)
        setStatus(colorGrey, false, "…")
        pairedGroup.visibility = View.GONE
        unpairedGroup.visibility = View.VISIBLE
        pairButton.isEnabled = true
        pairStatus.text = ""
    }

    private fun showPaired() {
        lastState = null
        failures = 0
        unpairedGroup.visibility = View.GONE
        pairedGroup.visibility = View.VISIBLE
        setStatus(colorGrey, false, "Checking…")
        viaText.text = if (paired?.relayConfigured() == true) "" else "relay not set up"
        toggleButton.text = "Turn internet ON"
        toggleButton.isEnabled = false
        handler.removeCallbacks(pollRunnable)
        handler.post(pollRunnable)
    }

    // ---------- pairing ----------

    private fun onPairClicked() {
        if (pairing) return
        val address = addressInput.text.toString().trim().ifEmpty { DEFAULT_ADDRESS }
        val pin = pinInput.text.toString()
        pinInput.setText("")
        if (pin.isEmpty()) {
            pairStatus.text = "Enter the PIN"
            return
        }
        pairing = true
        pairButton.isEnabled = false
        pairStatus.text = "Press the BOOT button on the switch now (30 s)…"
        executor.execute {
            val result = client.pair(address, pin)
            runOnUiThread {
                pairing = false
                pinInput.setText("")
                if (result.ok) {
                    paired = store.load()
                    if (paired != null) {
                        showPaired()
                    } else {
                        pairButton.isEnabled = true
                        pairStatus.text = "Could not read the stored pairing"
                    }
                } else {
                    pairButton.isEnabled = true
                    pairStatus.text = result.message
                }
            }
        }
    }

    // ---------- unpair ----------

    private fun onUnpairClicked() {
        AlertDialog.Builder(this)
            .setTitle("Unpair this phone?")
            .setMessage("This phone will no longer be able to control the switch.")
            .setNegativeButton("Cancel", null)
            .setPositiveButton("Unpair") { _, _ -> doUnpair() }
            .show()
    }

    private fun doUnpair() {
        val p = paired ?: return
        handler.removeCallbacks(pollRunnable)
        cmdBusy = true
        toggleButton.isEnabled = false
        unpairButton.isEnabled = false
        setStatus(colorYellow, false, "Unpairing…")
        executor.execute {
            try {
                client.sendCommand(p, "unpair", "")  // best effort
            } catch (e: Exception) {
                // ignore
            }
            client.disconnectRelay()
            store.clear()
            runOnUiThread {
                cmdBusy = false
                unpairButton.isEnabled = true
                showUnpaired()
            }
        }
    }

    // ---------- polling / commands ----------

    private fun poll() {
        val p = paired ?: return
        if (pollBusy || cmdBusy) return
        pollBusy = true
        executor.execute {
            val lan = client.state(p)
            var relay: SwitchState? = null
            if (lan.kind != Kind.OK && lan.kind != Kind.ERROR) {
                relay = client.relayLatest(p)
            }
            runOnUiThread {
                pollBusy = false
                if (paired === p) onPollResult(p, lan, relay)
            }
        }
    }

    private fun onPollResult(p: Paired, lan: Reply, relay: SwitchState?) {
        when (lan.kind) {
            Kind.OK -> {
                failures = 0
                showState(lan.state, false)
            }
            Kind.ERROR -> {
                failures = 0
                lastState = null
                showProblem("Switch error: ${lan.message}")
            }
            else -> {
                failures++
                if (relay != null) {
                    showState(relay, true)
                } else if (failures >= 3) {
                    lastState = null
                    showProblem(
                        if (lan.kind == Kind.INVALID) "Reply from the switch could not be verified"
                        else "Can't reach the switch. Are you on the home Wi-Fi?"
                    )
                }
                // otherwise keep showing the last verified state
            }
        }
    }

    // Relay delivered a verified, fresh state; use it only while the LAN is failing.
    private fun onRelayState(s: SwitchState) {
        if (paired == null || failures == 0) return
        showState(s, true)
    }

    private fun onToggleClicked() {
        val p = paired ?: return
        val current = lastState ?: return
        if (cmdBusy) return
        val target = if (current.internet == "on") "off" else "on"
        cmdBusy = true
        toggleButton.isEnabled = false
        setStatus(colorYellow, false, "Working…")
        executor.execute {
            val r = client.sendCommand(p, "set", target)
            runOnUiThread {
                cmdBusy = false
                if (paired !== p) return@runOnUiThread
                when (r.kind) {
                    Kind.OK -> {
                        failures = 0
                        showState(r.state, false)
                    }
                    Kind.SENT -> {
                        // The verified answer arrives through the relay subscription.
                        failures = maxOf(failures, 1)
                        setStatus(colorYellow, false, "Working…")
                        viaText.text = "via remote relay"
                    }
                    Kind.ERROR -> {
                        lastState = null
                        showProblem("Switch error: ${r.message}")
                    }
                    Kind.INVALID -> {
                        lastState = null
                        showProblem("Reply from the switch could not be verified")
                    }
                    Kind.UNREACHABLE -> {
                        lastState = null
                        showProblem("Can't reach the switch. Are you on the home Wi-Fi?")
                    }
                }
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

    private fun showProblem(text: String) {
        setStatus(colorGrey, false, text)
        toggleButton.text = "Turn internet ON"
        toggleButton.isEnabled = false
        viaText.text = if (paired?.relayConfigured() == true) "" else "relay not set up"
    }

    // Only called with verified states.
    private fun showState(s: SwitchState?, viaRelay: Boolean) {
        if (s == null) return
        lastState = s
        lastViaRelay = viaRelay
        when (s.led) {
            "online" -> setStatus(colorGreen, false, "Internet is ON")
            "blocked" -> setStatus(colorRed, false, "Internet is OFF")
            "no_internet" ->
                setStatus(colorRed, true, "Internet ON, but no connection from provider")
            "problem" -> setStatus(colorYellow, false, "Problem: switch can't control the router")
            "busy" -> setStatus(colorYellow, false, "Working…")
            else -> setStatus(colorGrey, false, "Status unknown")
        }
        viaText.text = if (viaRelay) "via remote relay" else "via home Wi-Fi"
        toggleButton.text =
            if (s.internet == "on") "Turn internet OFF" else "Turn internet ON"
        toggleButton.isEnabled = !cmdBusy
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
                val ip = (serviceInfo.host as? java.net.Inet4Address)?.hostAddress
                runOnUiThread {
                    if (ip != null && discoveryListener != null) {
                        addressInput.setText(ip)
                        toast("Found switch at $ip")
                        stopDiscovery(false)
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
