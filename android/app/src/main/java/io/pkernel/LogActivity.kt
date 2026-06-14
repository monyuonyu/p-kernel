/*
 * LogActivity.kt — the ENGINEER page (layer 3 of the 3-layer settings).
 *
 * Deliberately technical (docs/product-soul.md says the SURFACE must carry no
 * jargon; this deep page is the ONE place jargon is welcome). It shows three
 * things for a developer poking at a live node:
 *
 *   1. Raw kernel log — the full stdout ring buffer from PKernelService
 *      (snapshotLog()), tailed live via drainLog(), with a copy button.
 *   2. Node internals — node id, relay host/port, durable store path, and
 *      whether the kernel is currently running (the snapshot the service
 *      captures at boot).
 *   3. Module versions — GET http://127.0.0.1:<galaxyPort>/modules.json
 *      (a sibling wave adds the endpoint; shape
 *      {"node":id,"build":"...","modules":[{"name":"swim","version":1},...]}).
 *      If it isn't reachable yet, the rows degrade to "—" — never a crash.
 *
 *   4. App version — "yurikago <versionName>", read off the PackageManager
 *      (no BuildConfig needed), so the engineer can see exactly which build.
 *
 *   5. Fleet / relay (advanced) — node id + relay host/port/key, moved here
 *      from the friendly Settings layer (3-layer-discover wave). They persist
 *      to the "ump" prefs and feed PKernelService via the SAME intent extras;
 *      "Save & relight" restarts the node with the new values.
 *
 * Reached only from the "Engineer / advanced" button on the Settings layer;
 * manifest-declared exported=false.
 */
package io.pkernel

import android.annotation.SuppressLint
import android.content.ClipData
import android.content.ClipboardManager
import android.content.Intent
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.system.Os
import android.system.OsConstants
import android.view.MotionEvent
import android.widget.Button
import android.widget.EditText
import android.widget.ScrollView
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import org.json.JSONObject
import java.io.RandomAccessFile
import java.net.HttpURLConnection
import java.net.URL

class LogActivity : AppCompatActivity() {

    private lateinit var logView: TextView
    private lateinit var logScroll: ScrollView
    private lateinit var internalsView: TextView
    private lateinit var modulesView: TextView
    private lateinit var resourcesView: TextView
    private lateinit var nodeIdField: EditText
    private lateinit var relayHostField: EditText
    private lateinit var relayPortField: EditText
    private lateinit var relayKeyField: EditText
    private val ui = Handler(Looper.getMainLooper())
    @Volatile private var stopped = false
    private var galaxyPort = BASE_PORT

    /* Previous CPU jiffy snapshots, so each refresh reports a DELTA (busy% over
     * the interval) rather than a since-boot average. prevCpu = (idle, total)
     * from /proc/stat; prevProc = (utime+stime) from /proc/self/stat.
     *
     * IMPORTANT (UI-fixes wave): /proc/stat's aggregate "cpu" line is NOT
     * readable by a third-party app on API 26+ — SELinux denies it — so the
     * system% may stay unavailable on real devices. The PROCESS% therefore must
     * NOT depend on /proc/stat's total jiffies: it is timed against WALL CLOCK
     * (prevWallMs, via SystemClock.elapsedRealtime) and the kernel tick rate
     * (clkTck = sysconf(_SC_CLK_TCK), Hz). That makes "this node" resolve even
     * when the system line is blocked. */
    private var prevCpuIdle = -1L
    private var prevCpuTotal = -1L
    private var prevProcJiffies = -1L
    private var prevWallMs = -1L
    private val clkTck: Long =
        try { Os.sysconf(OsConstants._SC_CLK_TCK).coerceAtLeast(1L) }
        catch (_: Throwable) { 100L }   // 100 Hz is the near-universal default

    @SuppressLint("ClickableViewAccessibility")  // touch listener forwards to the ScrollView; it is not a click target
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_log)
        title = getString(R.string.engineer_title)

        logView       = findViewById(R.id.log_view)
        logScroll     = findViewById(R.id.log_scroll)
        internalsView = findViewById(R.id.internals_view)
        modulesView   = findViewById(R.id.modules_view)
        resourcesView = findViewById(R.id.resources_view)
        nodeIdField    = findViewById(R.id.field_node_id)
        relayHostField = findViewById(R.id.field_relay_host)
        relayPortField = findViewById(R.id.field_relay_port)
        relayKeyField  = findViewById(R.id.field_relay_key)

        /* App version — straight off the PackageManager (no BuildConfig). */
        findViewById<TextView>(R.id.app_version).text =
            getString(R.string.app_version_fmt, appVersionName())

        /* The galaxy/modules port follows the live node id (7800 + id - 1). */
        val nodeId = PKernelService.snapNodeId.let { if (it > 0) it else 1 }
        galaxyPort = BASE_PORT + (nodeId - 1)

        /* Seed the log with the whole ring buffer, then tail it. The inner
         * ScrollView starts pinned to the bottom so the latest line shows. */
        logView.text = PKernelService.snapshotLog()
        logScroll.post { logScroll.fullScroll(ScrollView.FOCUS_DOWN) }

        /* Bug 4 (log can't scroll): the fixed-height log ScrollView is nested
         * INSIDE the page's outer ScrollView. Two vertically-nested ScrollViews
         * fight for the vertical drag, and the OUTER one wins by default — so
         * the inner log never scrolls. Claim the gesture for the inner view: on
         * touch-down/move ask the parent NOT to intercept, releasing it on
         * up/cancel so the rest of the page still scrolls normally. The copy
         * button is a separate view and is unaffected. */
        logScroll.setOnTouchListener { v, ev ->
            when (ev.actionMasked) {
                MotionEvent.ACTION_DOWN, MotionEvent.ACTION_MOVE ->
                    v.parent?.requestDisallowInterceptTouchEvent(true)
                MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL ->
                    v.parent?.requestDisallowInterceptTouchEvent(false)
            }
            false              // do NOT consume — let ScrollView scroll itself
        }

        /* Resources/CPU: seed with the immediate readable facts (cores + the
         * honest GPU line); the % numbers fill in on the first refresh tick. */
        resourcesView.text = getString(R.string.engineer_measuring)

        findViewById<Button>(R.id.btn_copy_log).setOnClickListener {
            val cm = getSystemService(ClipboardManager::class.java)
            cm?.setPrimaryClip(ClipData.newPlainText("yurikago log", logView.text))
        }

        /* Fleet / relay fields, prefilled from the persisted "ump" prefs (the
         * same keys the service reads). "Save & relight" persists + restarts. */
        val prefs = getSharedPreferences(PREFS, MODE_PRIVATE)
        nodeIdField.setText(prefs.getInt(PKernelService.EXTRA_NODE_ID, 1).toString())
        relayHostField.setText(prefs.getString(PKernelService.EXTRA_RELAY_HOST, "") ?: "")
        relayPortField.setText(prefs.getInt(PKernelService.EXTRA_RELAY_PORT, 7400).toString())
        relayKeyField.setText(prefs.getString(PKernelService.EXTRA_RELAY_KEY, "") ?: "")
        findViewById<Button>(R.id.btn_relay_save).setOnClickListener { saveAndRelight() }

        renderInternals()
        fetchModules()
    }

    /** "yurikago 0.6.3" — read off the PackageManager (no BuildConfig). */
    private fun appVersionName(): String =
        try { packageManager.getPackageInfo(packageName, 0).versionName ?: "?" }
        catch (_: Throwable) { "?" }

    /**
     * Persist the fleet/relay fields to the "ump" prefs and (re)start the
     * service with them — the SAME intent-extra plumbing the friendly layer
     * used before these fields moved here. Does NOT touch the power gate.
     */
    private fun saveAndRelight() {
        val nodeId = nodeIdField.text.toString().toIntOrNull() ?: 1
        val host   = relayHostField.text.toString().trim()
        val port   = relayPortField.text.toString().toIntOrNull() ?: 7400
        val key    = relayKeyField.text.toString().trim()
        getSharedPreferences(PREFS, MODE_PRIVATE).edit()
            .putInt(PKernelService.EXTRA_NODE_ID, nodeId)
            .putString(PKernelService.EXTRA_RELAY_HOST, host)
            .putInt(PKernelService.EXTRA_RELAY_PORT, port)
            .putString(PKernelService.EXTRA_RELAY_KEY, key)
            .apply()
        val intent = Intent(this, PKernelService::class.java).apply {
            putExtra(PKernelService.EXTRA_NODE_ID,    nodeId)
            putExtra(PKernelService.EXTRA_RELAY_HOST, host)
            putExtra(PKernelService.EXTRA_RELAY_PORT, port)
            putExtra(PKernelService.EXTRA_RELAY_KEY,  key)
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) startForegroundService(intent)
        else startService(intent)
        /* The galaxy/modules port follows the new id; refresh the table. */
        galaxyPort = BASE_PORT + (nodeId - 1)
        renderInternals()
        fetchModules()
    }

    /** Node internals — engineer-facing facts, jargon OK here. */
    private fun renderInternals() {
        val s = PKernelService
        val relay = if (s.snapRelayHost.isEmpty()) "local-loopback mesh"
                    else "${s.snapRelayHost}:${s.snapRelayPort}"
        val store = s.snapDataDir.ifEmpty { getString(R.string.engineer_unavailable) }
        internalsView.text = buildString {
            append("node id     : ").append(if (s.snapNodeId > 0) s.snapNodeId else "—").append('\n')
            append("kernel state: ").append(if (s.snapRunning) "running" else "stopped").append('\n')
            append("relay       : ").append(relay).append('\n')
            append("galaxy port : ").append(galaxyPort).append('\n')
            append("durable store: ").append(store)
        }
    }

    /**
     * GET /modules.json off the loopback galaxy port on a worker thread.
     * Endpoint may not exist yet (sibling wave) — any failure renders "—".
     */
    private fun fetchModules() {
        modulesView.text = getString(R.string.engineer_unavailable)
        Thread({
            val rendered = try {
                val url = URL("http://127.0.0.1:$galaxyPort/modules.json")
                val conn = (url.openConnection() as HttpURLConnection).apply {
                    connectTimeout = 800
                    readTimeout = 800
                    requestMethod = "GET"
                }
                val body = try {
                    if (conn.responseCode == 200)
                        conn.inputStream.bufferedReader().use { it.readText() }
                    else null
                } finally { conn.disconnect() }
                if (body != null) renderModules(body) else null
            } catch (_: Throwable) { null }
            if (!stopped) ui.post {
                if (rendered != null) modulesView.text = rendered
                // else: leave the "—" placeholder already set.
            }
        }, "modules-fetch").apply { isDaemon = true; start() }
    }

    /** Parse {"node":id,"build":"...","modules":[{"name","version"}]} -> rows. */
    private fun renderModules(json: String): String? = try {
        val obj = JSONObject(json)
        val build = obj.optString("build", "—")
        val arr = obj.optJSONArray("modules")
        buildString {
            append("build: ").append(build).append('\n')
            if (arr != null) {
                for (i in 0 until arr.length()) {
                    val m = arr.optJSONObject(i) ?: continue
                    val name = m.optString("name", "?")
                    val ver  = if (m.has("version")) m.opt("version").toString() else "—"
                    append("  ").append(name.padEnd(16)).append(ver).append('\n')
                }
            }
        }.trimEnd('\n')
    } catch (_: Throwable) { null }

    /**
     * Resources / CPU — REAL numbers, no fabrication.
     *
     *   · this node CPU% : delta of /proc/self/stat utime+stime (jiffies)
     *     over the WALL-CLOCK interval (elapsedRealtime), converted to seconds
     *     via clkTck and expressed as a % of one whole core (so 100% = a core).
     *     /proc/self is OUR process, always readable — this resolves on every
     *     device. (Bug 5 root cause: the old code divided by /proc/stat's total
     *     jiffies, which is unreadable on API 26+, so this stayed "計算中"
     *     forever. Now it is independent of /proc/stat.)
     *   · system CPU% : delta off /proc/stat's aggregate "cpu" line. This file
     *     is DENIED to third-party apps by SELinux on API 26+, so on most real
     *     phones it is honestly "unavailable" rather than a perpetual
     *     placeholder. On devices/builds where it IS readable, a live % shows.
     *   · cores : Runtime.availableProcessors().
     *   · GPU : HONEST — Android has no portable GPU-usage API and inference
     *     runs on the CPU, so we state that rather than invent a %.
     *
     * The first call has no prior snapshot, so the % rows read "measuring…";
     * every subsequent call (≈ the 500ms poller cadence) shows a live delta,
     * EXCEPT system% which reads "unavailable" when /proc/stat is blocked.
     */
    private fun renderResources() {
        val cores = Runtime.getRuntime().availableProcessors()
        val nowMs = SystemClock.elapsedRealtime()

        /* /proc/stat aggregate cpu line: "cpu user nice system idle iowait
         * irq softirq steal ...". idle = idle+iowait; total = sum of all.
         * sysState: -2 = file unreadable (blocked) -> "unavailable";
         *           -1 = readable but no prior snapshot yet -> "measuring…";
         *          >=0 = a live %. */
        var sysPct = -1
        val stat = readProcStatCpu()
        if (stat == null) {
            sysPct = -2                       // /proc/stat denied (API 26+ SELinux)
        } else {
            val (idle, total) = stat
            if (prevCpuTotal >= 0) {
                val dTotal = total - prevCpuTotal
                val dIdle = idle - prevCpuIdle
                if (dTotal > 0) {
                    val busy = (100.0 * (dTotal - dIdle) / dTotal)
                    sysPct = busy.coerceIn(0.0, 100.0).toInt()
                }
            }
            prevCpuIdle = idle
            prevCpuTotal = total
        }

        /* /proc/self/stat field 14 (utime) + 15 (stime), in jiffies. Scale the
         * delta against the WALL-CLOCK delta (elapsedRealtime) — NOT against
         * /proc/stat's total — so this is computable even when the system line
         * is blocked. procJiffies/clkTck = CPU-seconds; / wallSeconds = cores
         * busy; *100 = % of one core. */
        var procPct = -1
        readProcSelfJiffies()?.let { jiffies ->
            if (prevProcJiffies >= 0 && prevWallMs >= 0) {
                val dProc = jiffies - prevProcJiffies            // CPU jiffies
                val dWallMs = nowMs - prevWallMs                 // wall ms
                if (dProc >= 0 && dWallMs > 0) {
                    val cpuSec = dProc.toDouble() / clkTck
                    val wallSec = dWallMs.toDouble() / 1000.0
                    procPct = (100.0 * cpuSec / wallSec)
                        .coerceIn(0.0, 100.0 * cores).toInt()
                }
            }
            prevProcJiffies = jiffies
        }
        prevWallMs = nowMs

        val measuring = getString(R.string.engineer_measuring)
        resourcesView.text = buildString {
            append("system CPU  : ")
                .append(when {
                    sysPct >= 0 -> "$sysPct%"
                    sysPct == -2 -> "n/a (restricted)"  // /proc/stat denied by SELinux (API 26+)
                    else -> measuring
                })
                .append('\n')
            append("this node   : ")
                .append(if (procPct >= 0) "$procPct%" else measuring)
                .append('\n')
            append("CPU cores   : ").append(cores).append('\n')
            append("GPU         : ").append(gpuStatusLine())
        }
    }

    /**
     * HONEST GPU status, read from the GPU-1/GPU-2 natives (gpu_compute.h):
     *
     *   · not available -> "not available on this device"
     *   · available     -> "<gpu_name()> · available · <enabled|off> ·
     *                        not yet used for inference (CPU)"
     *
     * The mind STILL computes inference on the CPU — GPU-3 wires the matmul to
     * the GPU. So the available line ALWAYS ends with "not yet used for
     * inference (CPU)": we never claim GPU inference work that isn't happening
     * (product-soul: no fake progress). The natives are crash-free at any time;
     * on a Vulkan-less device available() is false and we say so.
     */
    private fun gpuStatusLine(): String =
        /* "available vs not" keys off CAPABILITY (capable()), not in-use
         * (available()). A capable GPU is honestly "available" even with the
         * enable flag OFF — that is the DEFAULT state. The on/off sub-state is
         * a SEPARATE read (isEnabled()): so by default a capable device shows
         * "<name> · available · off · not yet used for inference (CPU)", and
         * after toggling "<name> · available · enabled (settings) · …". The
         * engineer_gpu_off branch used to be dead (the available() branch
         * required the flag ON); splitting capability from enablement reaches
         * it. The suffix stays honestly "not yet used for inference (CPU)" —
         * GPU-3 wires the matmul; this wave only surfaces status. */
        if (!PKernel.Gpu.capable()) {
            getString(R.string.engineer_gpu_unavailable)
        } else {
            val name = PKernel.Gpu.name().ifBlank { "GPU" }
            val state = getString(
                if (PKernel.Gpu.isEnabled()) R.string.engineer_gpu_enabled
                else R.string.engineer_gpu_off)
            getString(R.string.engineer_gpu_available, name, state)
        }

    /** Aggregate /proc/stat cpu line -> (idleJiffies, totalJiffies), or null.
     *  Returns null when the file is unreadable — which is the NORMAL case for a
     *  third-party app on API 26+ (SELinux denies /proc/stat). renderResources()
     *  maps that null to an honest "n/a (restricted)" for the system% row, while
     *  the process% row (driven by /proc/self/stat + wall clock) stays live. */
    private fun readProcStatCpu(): Pair<Long, Long>? = try {
        RandomAccessFile("/proc/stat", "r").use { raf ->
            val line = raf.readLine() ?: return null   // first line = "cpu ..."
            if (!line.startsWith("cpu ")) return null
            val parts = line.trim().split(Regex("\\s+"))
            // parts[0]="cpu"; user nice system idle iowait irq softirq steal...
            val nums = parts.drop(1).mapNotNull { it.toLongOrNull() }
            if (nums.size < 5) return null
            val idle = nums[3] + nums[4]            // idle + iowait
            val total = nums.sum()
            idle to total
        }
    } catch (_: Throwable) { null }

    /** /proc/self/stat utime+stime (fields 14,15) in jiffies, or null. */
    private fun readProcSelfJiffies(): Long? = try {
        RandomAccessFile("/proc/self/stat", "r").use { raf ->
            val line = raf.readLine() ?: return null
            /* comm (field 2) may contain spaces inside "(...)"; split after the
             * trailing ')' so field indices line up regardless. */
            val rParen = line.lastIndexOf(')')
            if (rParen < 0) return null
            val rest = line.substring(rParen + 2).trim().split(Regex("\\s+"))
            // after comm, field 3 = state; so utime is rest[11], stime rest[12].
            val utime = rest.getOrNull(11)?.toLongOrNull() ?: return null
            val stime = rest.getOrNull(12)?.toLongOrNull() ?: return null
            utime + stime
        }
    } catch (_: Throwable) { null }

    override fun onResume() {
        super.onResume()
        stopped = false
        /* Fresh CPU baseline each time the page is shown (deltas start now). */
        prevCpuIdle = -1L; prevCpuTotal = -1L; prevProcJiffies = -1L; prevWallMs = -1L
        renderResources()   // primes the snapshots; shows cores + GPU + measuring…

        /* Tail the live log while foregrounded (same cadence as MainActivity). */
        ui.post(object : Runnable {
            override fun run() {
                if (stopped) return
                val tail = PKernelService.drainLog()
                if (tail.isNotEmpty()) {
                    /* Auto-scroll to bottom only if the reader is already near
                     * it — so scrolling up to read history is not yanked back. */
                    val atBottom = logScroll.run {
                        val child = getChildAt(0)
                        child != null && scrollY + height >= child.height - BOTTOM_SLACK
                    }
                    logView.append(tail)
                    if (atBottom) logScroll.post { logScroll.fullScroll(ScrollView.FOCUS_DOWN) }
                }
                renderInternals()   // running/relay can change (pause/resume)
                renderResources()   // live system + process CPU% (real deltas)
                ui.postDelayed(this, REFRESH_MS)
            }
        })
    }

    override fun onPause() {
        stopped = true
        super.onPause()
    }

    companion object {
        private const val BASE_PORT = 7800   // galaxy.c §D1 (modules.json sibling)
        private const val PREFS = "ump"      // same prefs the service reads
        private const val REFRESH_MS = 500L  // log tail + CPU% refresh cadence
        private const val BOTTOM_SLACK = 24  // px tolerance for "at bottom"
    }
}
