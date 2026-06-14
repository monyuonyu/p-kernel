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
import android.view.View
import android.webkit.WebSettings
import android.webkit.WebView
import android.webkit.WebViewClient
import android.widget.FrameLayout
import android.widget.TextView
import androidx.activity.OnBackPressedCallback
import androidx.appcompat.app.AppCompatActivity
import java.net.InetSocketAddress
import java.net.Socket

class GalaxyActivity : AppCompatActivity() {

    private lateinit var web: WebView
    private lateinit var splash: TextView
    private var loaded = false
    private val ui = Handler(Looper.getMainLooper())
    @Volatile private var stopped = false

    private var galaxyPort = DEFAULT_PORT

    @SuppressLint("SetJavaScriptEnabled")
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        val nodeId = intent.getIntExtra(EXTRA_NODE_ID, 1).coerceIn(1, 255)
        galaxyPort = BASE_PORT + (nodeId - 1)

        val root = FrameLayout(this)

        web = WebView(this).apply {
            visibility = View.GONE
            settings.javaScriptEnabled = true          // the canvas page needs JS
            settings.domStorageEnabled = true
            settings.cacheMode = WebSettings.LOAD_NO_CACHE   // always the live page
            settings.mediaPlaybackRequiresUserGesture = false
            webViewClient = WebViewClient()            // keep navigation in-app
        }
        /* The waiting screen speaks human, not kernel (UX constitution):
         * no ports, no "kernel", no console. Just the star being lit — and
         * if the charge-only gate is holding it, say THAT kindly. */
        splash = TextView(this).apply {
            text = getString(R.string.galaxy_lighting)
            textSize = 17f
            gravity = android.view.Gravity.CENTER
            setPadding(64, 96, 64, 96)
            setBackgroundColor(0xFF05060A.toInt())     // the dark galaxy field
            setTextColor(0xFFAAB6FF.toInt())
        }
        root.addView(web, FrameLayout.LayoutParams(-1, -1))
        root.addView(splash, FrameLayout.LayoutParams(-1, -1))

        /* ⋮ — the only visible piece of machinery: a dim corner button to the
         * advanced screen (node id / fleet relay). Everything else is sky. */
        val menu = TextView(this).apply {
            text = "⋮"
            textSize = 22f
            contentDescription = getString(R.string.galaxy_menu_desc)
            setTextColor(0x66CDD6F4)
            setPadding(36, 24, 36, 24)
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

    /** Poll 127.0.0.1:<galaxyPort> until the kernel's galaxy task answers. */
    private fun startProbe() {
        Thread({
            var attempt = 0
            while (!stopped && !loaded) {
                attempt++
                val up = portOpen("127.0.0.1", galaxyPort, CONNECT_TIMEOUT_MS)
                if (up) {
                    ui.post { showGalaxy() }
                    return@Thread
                }
                if (attempt >= HINT_AFTER_ATTEMPTS && attempt % 4 == 0) {
                    /* the likeliest reason the star isn't lit yet is the
                     * battery-safe floor — the node now runs on battery down
                     * to ~30% and only pauses below that while unplugged. So
                     * only show the "plug in" nudge when the floor is actually
                     * holding (low AND unplugged); otherwise it's just waking.
                     * Said kindly, in the user's language, no kernel jargon. */
                    val lowAndUnplugged = batteryLowAndUnplugged()
                    ui.post {
                        if (!loaded) splash.text = getString(
                            if (lowAndUnplugged) R.string.galaxy_charge
                            else R.string.galaxy_lighting)
                    }
                }
                try { Thread.sleep(POLL_MS) } catch (_: InterruptedException) { return@Thread }
            }
        }, "galaxy-port-probe").apply { isDaemon = true; start() }
    }

    private fun showGalaxy() {
        if (loaded) return
        loaded = true
        web.loadUrl("http://127.0.0.1:$galaxyPort/")
        web.visibility = View.VISIBLE
        splash.visibility = View.GONE
    }

    /**
     * sticky-intent battery check — no permission needed. Mirrors the
     * battery-safe floor in PKernelService.powerAllowed(): the gate that
     * keeps the star dark only holds when the battery is at/below the floor
     * AND the phone is unplugged. (When >floor or plugged in, the node runs
     * on battery, so the star is merely waking, not gated.)
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
        return pct <= PKernelService.BATTERY_FLOOR_PCT
    }

    override fun onDestroy() {
        stopped = true
        if (::web.isInitialized) web.destroy()
        super.onDestroy()
    }

    companion object {
        const val EXTRA_NODE_ID = "node_id"
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
