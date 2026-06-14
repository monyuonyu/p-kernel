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

import android.content.ClipData
import android.content.ClipboardManager
import android.content.Intent
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.widget.Button
import android.widget.EditText
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import org.json.JSONObject
import java.net.HttpURLConnection
import java.net.URL

class LogActivity : AppCompatActivity() {

    private lateinit var logView: TextView
    private lateinit var internalsView: TextView
    private lateinit var modulesView: TextView
    private lateinit var nodeIdField: EditText
    private lateinit var relayHostField: EditText
    private lateinit var relayPortField: EditText
    private lateinit var relayKeyField: EditText
    private val ui = Handler(Looper.getMainLooper())
    @Volatile private var stopped = false
    private var galaxyPort = BASE_PORT

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_log)
        title = getString(R.string.engineer_title)

        logView       = findViewById(R.id.log_view)
        internalsView = findViewById(R.id.internals_view)
        modulesView   = findViewById(R.id.modules_view)
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

        /* Seed the log with the whole ring buffer, then tail it. */
        logView.text = PKernelService.snapshotLog()

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

    override fun onResume() {
        super.onResume()
        stopped = false
        /* Tail the live log while foregrounded (same cadence as MainActivity). */
        ui.post(object : Runnable {
            override fun run() {
                if (stopped) return
                val tail = PKernelService.drainLog()
                if (tail.isNotEmpty()) logView.append(tail)
                renderInternals()   // running/relay can change (pause/resume)
                ui.postDelayed(this, 500)
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
    }
}
