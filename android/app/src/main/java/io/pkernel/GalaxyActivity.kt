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
        splash = TextView(this).apply {
            text = getString(R.string.galaxy_starting, galaxyPort)
            textSize = 16f
            setPadding(48, 96, 48, 48)
            setBackgroundColor(0xFF05060A.toInt())     // the dark galaxy field
            setTextColor(0xFFAAB6FF.toInt())
        }
        root.addView(web, FrameLayout.LayoutParams(-1, -1))
        root.addView(splash, FrameLayout.LayoutParams(-1, -1))
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
                if (attempt == HINT_AFTER_ATTEMPTS) {
                    ui.post {
                        if (!loaded) splash.text =
                            getString(R.string.galaxy_still_starting, galaxyPort)
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
