/*
 * GalaxyActivity.kt — galaxy.md D4: the owner's window into their star.
 *
 * Opens a full-screen WebView at http://127.0.0.1:<galaxyPort>/ where
 * galaxy.c (running inside libpkernel.so via PKernelService) serves the
 * embedded canvas star-field page + /galaxy.json + /events (SSE) +
 * /manifesto (the i18n manifesto auto-selected from the device locale via
 * the WebView's Accept-Language header) + /teach + /ask.
 *
 * The galaxy port is 7800 + (node_id - 1) — the per-node offset baked into
 * galaxy.c (§D1). For a single phone (node 1) that is 7800.
 *
 * The page is served by the kernel, which boots asynchronously in the
 * foreground service. So we show a "kernel starting…" splash and probe the
 * loopback port in the background; only once the port answers do we load the
 * WebView. A dead/slow kernel never wedges the UI — the probe is on its own
 * thread with a short connect timeout, retried on a cadence.
 *
 * Honesty: this is a FACE on the running organism (galaxy.md §1.2). If the
 * kernel is not up (e.g. charge-only gate is holding it off), the splash
 * says so plainly rather than pretending.
 */
package io.pkernel

import android.annotation.SuppressLint
import android.content.Intent
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.Gravity
import android.view.View
import android.webkit.WebResourceError
import android.webkit.WebResourceRequest
import android.webkit.WebSettings
import android.webkit.WebView
import android.webkit.WebViewClient
import android.widget.Button
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.TextView
import androidx.activity.OnBackPressedCallback
import androidx.appcompat.app.AppCompatActivity
import java.net.InetSocketAddress
import java.net.Socket

class GalaxyActivity : AppCompatActivity() {

    private lateinit var web: WebView
    private lateinit var splash: TextView
    private lateinit var splashBox: LinearLayout
    private lateinit var relightBtn: Button
    private var loaded = false
    private val ui = Handler(Looper.getMainLooper())
    @Volatile private var stopped = false
    /* Guards against stacking probe threads: a main-frame error re-arms the
     * probe, but only one poll loop may run at a time (else recovery would
     * spawn a new thread on every error/retry). */
    @Volatile private var probing = false

    private var galaxyPort = DEFAULT_PORT
    private var nodeId = 1
    private var showIntro = false

    @SuppressLint("SetJavaScriptEnabled")
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        nodeId = intent.getIntExtra(EXTRA_NODE_ID, 1).coerceIn(1, 255)
        galaxyPort = BASE_PORT + (nodeId - 1)
        showIntro = intent.getBooleanExtra(EXTRA_SHOW_INTRO, false)

        val root = FrameLayout(this)

        web = WebView(this).apply {
            visibility = View.GONE
            settings.javaScriptEnabled = true          // the canvas page needs JS
            settings.domStorageEnabled = true
            settings.cacheMode = WebSettings.LOAD_NO_CACHE   // always the live page
            settings.mediaPlaybackRequiresUserGesture = false
            webViewClient = resilientClient()          // keep nav in-app + never show the raw error page
        }
        /* The waiting screen speaks human, not kernel (UX constitution):
         * no ports, no "kernel", no console. Just the star being lit — and
         * if the charge-only gate is holding it, say THAT kindly.
         *
         * Bug 1 (state-aware splash): when the star is asleep BY CHOICE (the
         * owner tapped 眠らせる, which stopSelf()s and kills the process), the
         * old splash showed "Lighting your star…" forever — a lie: nothing is
         * lighting it, and there was no way to relight from here. So the splash
         * is a vertical box: the message + a 灯す (relight) button that is shown
         * ONLY when the star is dark by choice (not battery/WiFi gated). The
         * button restarts the foreground service and re-arms the port probe.
         *
         * relight-fix (this wave): "asleep by choice" is now read from the
         * EXPLICIT PREF_SLEPT_BY_CHOICE flag (set by 眠らせる, cleared on every
         * real boot), NOT inferred from "the port has been dead for ~4s". The
         * inference was wrong: the galaxy HTTP port opens late in the node's
         * boot, so a genuine cold/first-run boot (which can take many seconds)
         * was misclassified as asleep and the 灯す button appeared mid-boot. A
         * still-waking node now stays "Lighting your star…" with no button. */
        splash = TextView(this).apply {
            text = getString(R.string.galaxy_lighting)
            textSize = 17f
            gravity = Gravity.CENTER
            setTextColor(0xFFAAB6FF.toInt())
        }
        relightBtn = Button(this).apply {
            text = getString(R.string.btn_start)       // "灯す" (localized)
            textSize = 18f
            setTextColor(0xFFAAB6FF.toInt())
            visibility = View.GONE                     // shown only when asleep-by-choice
            setOnClickListener { relight() }
        }
        splashBox = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.CENTER
            setPadding(64, 96, 64, 96)
            setBackgroundColor(0xFF05060A.toInt())     // the dark galaxy field
            addView(splash, LinearLayout.LayoutParams(-1, -2))
            addView(relightBtn, LinearLayout.LayoutParams(-2, -2).apply { topMargin = 48 })
        }
        root.addView(web, FrameLayout.LayoutParams(-1, -1))
        root.addView(splashBox, FrameLayout.LayoutParams(-1, -1))

        /* ⚙ — the visible settings button: a clear corner gear into the
         * Settings layer (MainActivity in advanced mode). Tasteful but
         * FINDABLE — a first-time owner must be able to reach settings without
         * a treasure hunt (0.6.3 feedback: the old ⋮ at ~40% alpha was
         * invisible). High-alpha glyph, generous tap padding; everything else
         * is sky. */
        val menu = TextView(this).apply {
            text = "⚙"
            textSize = 24f
            contentDescription = getString(R.string.galaxy_menu_desc)
            setTextColor(0xCCCDD6F4.toInt())
            setPadding(40, 28, 40, 28)
            setOnClickListener {
                startActivity(Intent(this@GalaxyActivity, MainActivity::class.java)
                    .putExtra(MainActivity.EXTRA_ADVANCED, true))
            }
        }
        root.addView(menu, FrameLayout.LayoutParams(-2, -2,
            android.view.Gravity.TOP or android.view.Gravity.END).apply {
            topMargin = (44 * resources.displayMetrics.density).toInt()
        })  // sits BELOW the page's own top-right language selector
        setContentView(root)

        // Back button: walk the WebView history first, then leave.
        onBackPressedDispatcher.addCallback(this, object : OnBackPressedCallback(true) {
            override fun handleOnBackPressed() {
                if (loaded && web.canGoBack()) web.goBack() else finish()
            }
        })

        startProbe()
    }

    /**
     * Poll 127.0.0.1:<galaxyPort> until the kernel's galaxy task answers, then
     * show (or re-show) the galaxy page.
     *
     * Re-entrant: it is called once at startup AND again whenever a main-frame
     * load fails (the star "rested" — battery floor / WiFi-only pause / a
     * transient mid-navigation). Only ONE poll thread runs at a time (the
     * `probing` guard), so repeated errors don't stack threads; `stopped`
     * stays false across re-probes (only onDestroy sets it) so recovery works.
     */
    private fun startProbe() {
        if (stopped) return
        synchronized(this) {
            if (probing) return          // a poll loop is already running; don't stack
            probing = true
        }
        loaded = false                   // we are (re)entering the waiting state
        Thread({
            var attempt = 0
            try {
            while (!stopped && !loaded) {
                attempt++
                val up = portOpen("127.0.0.1", galaxyPort, CONNECT_TIMEOUT_MS)
                if (up) {
                    ui.post { showGalaxy() }
                    return@Thread
                }
                if (attempt >= HINT_AFTER_ATTEMPTS && attempt % 4 == 0) {
                    /* relight-fix: the port being dead is NOT proof the star is
                     * "asleep by choice" — a genuine cold boot / first-run can
                     * take well past the old ~4s hint window (the galaxy HTTP
                     * port only opens late in usermain), so the old code lit a
                     * premature, lying 灯す button mid-boot. Now we drive the
                     * splash from the REAL reasons: the battery/WiFi gate
                     * branches (still inferred, since those are observable), OR
                     * the EXPLICIT slept-by-choice flag the service/Settings
                     * set when the owner tapped 眠らせる. With no gate holding
                     * AND no explicit sleep, we are simply still WAKING, so the
                     * splash stays "Lighting your star…" (no 灯す button). */
                    ui.post { if (!loaded) applySplashState(asleepByChoice = sleptByChoice()) }
                }
                try { Thread.sleep(POLL_MS) } catch (_: InterruptedException) { return@Thread }
            }
            } finally {
                probing = false          // allow a future re-probe to start a fresh loop
            }
        }, "galaxy-port-probe").apply { isDaemon = true; start() }
    }

    /**
     * Drive the splash's message AND the relight button from the REAL reason
     * the star is dark right now — so it never lies (Bug 1). Mirrors
     * PKernelService's run-gate:
     *   · charge-only floor holding -> "plug in"   (no relight button: the
     *     gate, not the owner, is holding it — relighting would just re-pause)
     *   · WiFi-only on & not on WiFi -> "waiting for Wi-Fi"  (likewise)
     *   · otherwise, while still WAKING -> "Lighting your star…" (no button)
     *   · otherwise, AFTER several dead probes (asleepByChoice) -> "Your star
     *     is asleep right now" + a 灯す button that restarts the node.
     * Said in the user's language, no kernel jargon (UX constitution).
     */
    private fun applySplashState(asleepByChoice: Boolean = false) {
        when {
            batteryLowAndUnplugged() -> {
                splash.text = getString(R.string.galaxy_charge)
                relightBtn.visibility = View.GONE
            }
            wifiOnlyAndNotOnWifi() -> {
                splash.text = getString(R.string.galaxy_wifi_wait)
                relightBtn.visibility = View.GONE
            }
            asleepByChoice -> {
                // No gate is holding it AND it has stayed dark: the owner put it
                // to sleep. Tell the truth and offer to relight it from here.
                splash.text = getString(R.string.star_dark)
                relightBtn.visibility = View.VISIBLE
            }
            else -> {
                splash.text = getString(R.string.galaxy_lighting)
                relightBtn.visibility = View.GONE
            }
        }
    }

    /**
     * 灯す from the galaxy splash: restart the foreground service with the
     * persisted "ump" prefs (the SAME extras MainActivity/LogActivity use),
     * hide the button, show "Lighting…", and re-arm the port probe so the page
     * loads automatically once the node answers again.
     */
    private fun relight() {
        val prefs = getSharedPreferences("ump", MODE_PRIVATE)
        val intent = Intent(this, PKernelService::class.java).apply {
            // Relight the SAME node this galaxy is watching, so the port we
            // probe (galaxyPort = BASE_PORT + nodeId-1) is the one it serves.
            putExtra(PKernelService.EXTRA_NODE_ID,    nodeId)
            putExtra(PKernelService.EXTRA_RELAY_HOST, prefs.getString(PKernelService.EXTRA_RELAY_HOST, "") ?: "")
            putExtra(PKernelService.EXTRA_RELAY_PORT, prefs.getInt(PKernelService.EXTRA_RELAY_PORT, 7400))
            putExtra(PKernelService.EXTRA_RELAY_KEY,  prefs.getString(PKernelService.EXTRA_RELAY_KEY, "") ?: "")
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) startForegroundService(intent)
        else startService(intent)
        relightBtn.visibility = View.GONE
        splash.text = getString(R.string.galaxy_lighting)
        startProbe()
    }

    private fun showGalaxy() {
        if (loaded) return
        loaded = true
        // ?intro=1 replays the picture-book intro (pictures only, no re-consent)
        // for a returning user who tapped "view introduction again" in settings.
        val url = "http://127.0.0.1:$galaxyPort/" + if (showIntro) "?intro=1" else ""
        web.loadUrl(url)
        web.visibility = View.VISIBLE
        splashBox.visibility = View.GONE
    }

    /**
     * relight-fix: the ONLY truthful source for "the owner put the star to
     * sleep". Read the explicit PREF_SLEPT_BY_CHOICE flag the service clears on
     * every boot and MainActivity.stopKernel() sets on 眠らせる. A dead port
     * alone (slow boot) must NEVER be read as asleep-by-choice.
     */
    private fun sleptByChoice(): Boolean =
        getSharedPreferences("ump", MODE_PRIVATE)
            .getBoolean(PKernelService.PREF_SLEPT_BY_CHOICE, false)

    /**
     * sticky-intent battery check — no permission needed. Mirrors the
     * battery-safe floor in PKernelService.powerAllowed(): the gate that
     * keeps the star dark only holds when the battery is at/below the floor
     * AND the phone is unplugged. (When >floor or plugged in, the node runs
     * on battery, so the star is merely waking, not gated.)
     *
     * The floor is a USER SETTING (PREF_BATTERY_FLOOR) since the
     * settings-expansion wave; read the SAME dynamic value powerAllowed's
     * batteryFloor() does (default BATTERY_FLOOR_PCT, clamped 10..50) so this
     * "battery low" hint always matches the actual gate.
     */
    private fun batteryLowAndUnplugged(): Boolean {
        val i = registerReceiver(null,
            android.content.IntentFilter(android.content.Intent.ACTION_BATTERY_CHANGED))
            ?: return false                      // unknown -> don't nag
        val plugged = i.getIntExtra(android.os.BatteryManager.EXTRA_PLUGGED, 0)
        if (plugged != 0) return false           // charging/plugged: never gated
        val level = i.getIntExtra(android.os.BatteryManager.EXTRA_LEVEL, -1)
        val scale = i.getIntExtra(android.os.BatteryManager.EXTRA_SCALE, -1)
        if (level < 0 || scale <= 0) return false // unknown -> don't nag
        val pct = level * 100 / scale
        val floor = getSharedPreferences("ump", MODE_PRIVATE)
            .getInt(PKernelService.PREF_BATTERY_FLOOR, PKernelService.BATTERY_FLOOR_PCT)
            .coerceIn(10, 50)
        return pct <= floor
    }

    /**
     * Mirrors PKernelService's WiFi-only gate (PREF_WIFI_ONLY + onUnmeteredWifi):
     * the network gate that pauses the node holds only when the user turned
     * WiFi-only ON and the device is NOT on unmetered WiFi. Read-only, no
     * permission; fail-OPEN (return false = "not the reason") when the network
     * state is unreadable, exactly like the service's own fail-open. This only
     * drives a kinder splash message — never a gate decision.
     */
    private fun wifiOnlyAndNotOnWifi(): Boolean {
        val on = getSharedPreferences("ump", MODE_PRIVATE)
            .getBoolean(PKernelService.PREF_WIFI_ONLY, false)
        if (!on) return false                    // WiFi-only off -> not the reason
        val cm = getSystemService(android.content.Context.CONNECTIVITY_SERVICE)
            as? android.net.ConnectivityManager ?: return false   // unknown -> don't claim it
        val net = cm.activeNetwork ?: return true               // no network -> not on WiFi
        val caps = cm.getNetworkCapabilities(net) ?: return false // unknown caps -> don't claim it
        val onUnmeteredWifi =
            caps.hasTransport(android.net.NetworkCapabilities.TRANSPORT_WIFI) &&
            caps.hasCapability(android.net.NetworkCapabilities.NET_CAPABILITY_NOT_METERED)
        return !onUnmeteredWifi
    }

    /**
     * A WebViewClient that keeps navigation in-app AND — critically — NEVER
     * lets the system "webpage not available / cannot access" error frame show.
     *
     * Why: galaxy.c serves the page from inside the node. When the star
     * "rests" mid-session (battery-safe floor, WiFi-only pause, or a transient
     * during navigation), a reload / sub-resource / SSE-reconnect can fail and
     * the default client renders the raw browser error page. The owner sees a
     * scary "cannot access" screen for what is really their star simply
     * sleeping. (mk_pino sees this a lot while tapping around.)
     *
     * Policy:
     *  - MAIN-FRAME failure (request.isForMainFrame): the page itself is
     *    unreachable -> stop, blank any half-rendered error frame, fall back to
     *    the friendly splash with a kind reason, and RE-ARM the port probe so
     *    the page reloads automatically when the node comes back up.
     *  - SUB-RESOURCE failure (!isForMainFrame): a /events SSE drop or a
     *    /galaxy.json blip — the page's own JS already tolerates these
     *    (auto-reconnecting SSE). Do NOT splash-flap on every blip; leave it
     *    to the page. (Default client behaviour: ignore.)
     */
    private fun resilientClient(): WebViewClient = object : WebViewClient() {
        override fun onReceivedError(
            view: WebView,
            request: WebResourceRequest,
            error: WebResourceError
        ) {
            // Only the MAIN frame going unreachable warrants the splash; sub-
            // resource blips (SSE/fetch) are the page's JS to recover from.
            if (!request.isForMainFrame) return
            view.stopLoading()
            // Blank the WebView so no stale error frame lingers behind the splash.
            view.loadUrl("about:blank")
            view.visibility = View.GONE
            // relight-fix: respect the explicit slept-by-choice flag here too, so a
            // main-frame drop right after the owner tapped 眠らせる offers 灯す, while a
            // transient mid-session drop stays "Lighting…" until the probe recovers.
            applySplashState(asleepByChoice = sleptByChoice())
            splashBox.visibility = View.VISIBLE
            loaded = false
            startProbe()                         // reload automatically when the node is back
        }
    }

    override fun onDestroy() {
        stopped = true
        if (::web.isInitialized) web.destroy()
        super.onDestroy()
    }

    companion object {
        const val EXTRA_NODE_ID = "node_id"
        const val EXTRA_SHOW_INTRO = "show_intro"
        private const val BASE_PORT = 7800            // galaxy.c §D1
        private const val DEFAULT_PORT = BASE_PORT
        private const val CONNECT_TIMEOUT_MS = 400
        private const val POLL_MS = 500L
        private const val HINT_AFTER_ATTEMPTS = 8     // ~4s before the "still starting" hint

        private fun portOpen(host: String, port: Int, timeoutMs: Int): Boolean =
            try {
                Socket().use { s ->
                    s.connect(InetSocketAddress(host, port), timeoutMs)
                    true
                }
            } catch (_: Throwable) { false }
    }
}
