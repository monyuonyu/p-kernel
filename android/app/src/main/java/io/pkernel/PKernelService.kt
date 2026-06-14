/*
 * PKernelService.kt — UMP foreground service.
 *
 * Owns the libpkernel.so kernel thread for the lifetime of the
 * service. Posts a persistent notification ("yurikago · your star")
 * so Android keeps the process alive past the 5-minute background
 * limit. (User-facing text carries no "kernel" jargon — see
 * docs/product-soul.md; the engineer-facing LogActivity is the one
 * place jargon is allowed.)
 *
 * Battery-safe floor (formerly "charge-only"): if the user hasn't
 * disabled it, the service runs on battery down to a USER-CONFIGURABLE
 * floor (PREF_BATTERY_FLOOR, default BATTERY_FLOOR_PCT = 30%, clamped
 * 10..50), then pauses to protect the battery and resumes when the
 * phone is plugged in. (The old name PREF_CHARGE_ONLY is kept for
 * back-compat with installs that already wrote that pref; its meaning
 * is now "battery-safe mode", default on.) Users who want always-on
 * can flip the SharedPreferences boolean off.
 *
 * WiFi-only (PREF_WIFI_ONLY, default off): when on, the node runs ONLY
 * on unmetered WiFi. The run-gate is powerAllowed() && networkAllowed();
 * a ConnectivityManager.NetworkCallback observes connectivity so the
 * node pauses when WiFi is lost and resumes when it returns. The gate
 * fails OPEN on unknown connectivity state (don't pause on missing data,
 * matching batteryPct() returning 100 on unknown).
 *
 * Because there is NO native kernel shutdown (PKernel only exposes
 * nativeBoot), the only way to pause is stopSelf() — which kills the
 * whole foreground service/process and its runtime receiver. So the
 * policy is split:
 *   - LEVEL-based PAUSE uses a RUNTIME ACTION_BATTERY_CHANGED receiver
 *     (that broadcast cannot be manifest-declared); while the service
 *     is alive it watches the level and stopSelf()s at the floor. At the
 *     moment of pause it schedules a JobScheduler job constrained on
 *     charging (setRequiresCharging(true), setPersisted(true)).
 *   - CHARGE-based RESUME uses that JobScheduler job (ResumeJobService),
 *     which survives the process death and, on a reboot-while-charging,
 *     survives a reboot too. A running JobService is an allowed context
 *     to start a foreground service on Android 12+, whereas a background
 *     ACTION_POWER_CONNECTED receiver calling startForegroundService()
 *     throws ForegroundServiceStartNotAllowedException — which is why the
 *     old manifest receiver could never resume. On a successful (re)boot
 *     the pending job is cancelled so it can't double-fire.
 *
 * Tail log: drains stdout from the kernel via PKernel.readStdout()
 * on a worker thread and keeps the last ~64 KB in a ring buffer so
 * MainActivity can render it.
 */

package io.pkernel

import android.app.Notification
import android.app.NotificationManager
import android.app.Service
import android.app.job.JobInfo
import android.app.job.JobScheduler
import android.content.BroadcastReceiver
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.SharedPreferences
import android.net.ConnectivityManager
import android.net.Network
import android.net.NetworkCapabilities
import android.net.NetworkRequest
import android.os.BatteryManager
import android.os.Build
import android.os.IBinder
import androidx.core.app.NotificationCompat
import java.nio.charset.StandardCharsets
import java.util.concurrent.atomic.AtomicBoolean

class PKernelService : Service() {

    private var pollerThread: Thread? = null
    private val running = AtomicBoolean(false)
    private lateinit var prefs: SharedPreferences
    private var powerReceiver: BroadcastReceiver? = null
    private var netCallback: ConnectivityManager.NetworkCallback? = null
    // Boot params for this service instance, captured on first start so the
    // runtime power receiver can (re)boot the kernel without a fresh intent.
    private var bootNodeId = 1
    private var bootRelayHost = ""
    private var bootRelayPort = 7400
    private var bootRelayKey = ""

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        prefs = getSharedPreferences("ump", Context.MODE_PRIVATE)
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        // Boot params: prefer the launching intent; if relaunched by the
        // manifest power-connected receiver (or sticky restart) the intent may
        // be null/empty, so fall back to the persisted "ump" prefs.
        bootNodeId    = intent?.getIntExtra(EXTRA_NODE_ID, prefs.getInt(EXTRA_NODE_ID, 1))
                            ?: prefs.getInt(EXTRA_NODE_ID, 1)
        bootRelayHost = intent?.getStringExtra(EXTRA_RELAY_HOST)
                            ?: prefs.getString(EXTRA_RELAY_HOST, "") ?: ""
        bootRelayPort = intent?.getIntExtra(EXTRA_RELAY_PORT, prefs.getInt(EXTRA_RELAY_PORT, 7400))
                            ?: prefs.getInt(EXTRA_RELAY_PORT, 7400)
        bootRelayKey  = intent?.getStringExtra(EXTRA_RELAY_KEY)
                            ?: prefs.getString(EXTRA_RELAY_KEY, "") ?: ""

        // Persist the boot params so the charge-resume path (the JobScheduler
        // ResumeJobService) can relaunch this service after a low-battery
        // stopSelf() (which kills the process + its runtime receiver). Same
        // "ump" prefs file MainActivity already writes.
        prefs.edit()
            .putInt(EXTRA_NODE_ID, bootNodeId)
            .putString(EXTRA_RELAY_HOST, bootRelayHost)
            .putInt(EXTRA_RELAY_PORT, bootRelayPort)
            .putString(EXTRA_RELAY_KEY, bootRelayKey)
            .apply()

        startForeground(NOTIF_ID, buildNotification(bootNodeId, statusText(bootRelayHost, bootRelayPort)))

        // The runtime receiver lives for the whole service lifetime: it both
        // boots when power becomes allowed and pauses (stopSelf) at the floor.
        registerPowerReceiver()
        // The network callback observes connectivity changes for WiFi-only mode.
        registerNetworkCallback()

        if (runAllowed()) {
            bootKernelOnce(bootNodeId, bootRelayHost, bootRelayPort, bootRelayKey)
        } else if (!powerAllowed()) {
            appendLog("[ump] battery-safe mode: battery is low and unplugged — " +
                      "the star will relight when you plug in.\n")
        } else {
            appendLog("[ump] WiFi-only mode: not on unmetered WiFi — " +
                      "the star will relight when you're back on WiFi.\n")
        }
        return START_STICKY
    }

    override fun onDestroy() {
        powerReceiver?.let {
            try { unregisterReceiver(it) } catch (_: IllegalArgumentException) {}
            powerReceiver = null
        }
        netCallback?.let {
            val cm = getSystemService(Context.CONNECTIVITY_SERVICE) as? ConnectivityManager
            try { cm?.unregisterNetworkCallback(it) } catch (_: IllegalArgumentException) {}
            netCallback = null
        }
        pollerThread?.interrupt()
        snapRunning = false
        appendLog("[ump] service stopped.\n")
        super.onDestroy()
    }

    /* --- kernel boot ---------------------------------------------------- */

    private fun bootKernelOnce(nodeId: Int, relayHost: String,
                               relayPort: Int, relayKey: String) {
        if (!running.compareAndSet(false, true)) return
        // The kernel is (re)booting, so any pending charge-resume job is stale.
        // Cancel it so it can't double-fire when the phone is next plugged in.
        cancelResumeJob()
        appendLog("[ump] starting kernel (node $nodeId, relay='$relayHost:$relayPort')\n")
        val pk = PKernel()
        // persistence SLICE 1/2 (docs/architecture/persistence.md): point the
        // durable p-fs store at app-private storage so the identity (Self
        // layer) and the learned mind (rw[]) survive a process death / reboot.
        // getFilesDir() is app-private + backup-excluded (no permission, no
        // cloud sync — the ark survives by replicating across the mesh, not by
        // Google Drive). MUST precede boot so the boot-time restore sees it.
        pk.setDataDir(filesDir.absolutePath)
        appendLog("[ump] durable store: ${filesDir.absolutePath}/ark\n")
        // Engineer-page snapshot of the live boot facts (read by LogActivity).
        snapNodeId    = nodeId
        snapRelayHost = relayHost
        snapRelayPort = relayPort
        snapDataDir   = "${filesDir.absolutePath}/ark"
        snapRunning   = true
        if (relayHost.isNotEmpty()) {
            pk.bootWithRelay(nodeId, relayHost, relayPort,
                             relayKey.ifEmpty { null })
        } else {
            pk.boot(nodeId)
        }
        startLogPoller(pk)
    }

    private fun startLogPoller(pk: PKernel) {
        pollerThread = Thread({
            val buf = ByteArray(4096)
            while (!Thread.currentThread().isInterrupted) {
                val n = try { pk.readStdout(buf, buf.size) } catch (e: Throwable) { 0 }
                if (n > 0) {
                    appendLog(String(buf, 0, n, StandardCharsets.UTF_8))
                } else {
                    try { Thread.sleep(50) } catch (_: InterruptedException) { return@Thread }
                }
            }
        }, "pkernel-log-poller").also { it.isDaemon = true; it.start() }
    }

    /* --- battery-safe floor gate --------------------------------------- */

    /** Current battery percentage [0..100] from the sticky battery intent,
     *  or 100 if it can't be read (don't pause on unknown state). */
    private fun batteryPct(): Int {
        val i = registerReceiver(null, IntentFilter(Intent.ACTION_BATTERY_CHANGED))
            ?: return 100
        val level = i.getIntExtra(BatteryManager.EXTRA_LEVEL, -1)
        val scale = i.getIntExtra(BatteryManager.EXTRA_SCALE, -1)
        if (level < 0 || scale <= 0) return 100
        return level * 100 / scale
    }

    /** True if the phone is plugged in (AC/USB/wireless). */
    private fun isPlugged(): Boolean {
        val i = registerReceiver(null, IntentFilter(Intent.ACTION_BATTERY_CHANGED))
            ?: return false
        return i.getIntExtra(BatteryManager.EXTRA_PLUGGED, 0) != 0
    }

    /** The configured battery floor %, clamped to the SeekBar's range so a
     *  bad pref can never push the floor out of [10..50]. Defaults to the
     *  BATTERY_FLOOR_PCT const. */
    private fun batteryFloor(): Int =
        prefs.getInt(PREF_BATTERY_FLOOR, BATTERY_FLOOR_PCT).coerceIn(10, 50)

    /**
     * Battery-safe floor policy. If the user turned battery-safe mode OFF,
     * always run. Otherwise run while plugged in OR while the battery is
     * strictly above the (user-configurable) floor. (Strictly-above so the
     * pause condition — at/below floor AND unplugged — is the exact complement
     * and never flaps.)
     */
    private fun powerAllowed(): Boolean {
        if (!prefs.getBoolean(PREF_CHARGE_ONLY, true)) return true
        return isPlugged() || batteryPct() > batteryFloor()
    }

    /* --- WiFi-only network gate ---------------------------------------- */

    /**
     * WiFi-only policy. If the user did NOT turn WiFi-only on, always allow.
     * Otherwise allow ONLY when the active network is unmetered WiFi
     * (TRANSPORT_WIFI && NET_CAPABILITY_NOT_METERED).
     *
     * Fail-OPEN on unknown: if ConnectivityManager or the active network's
     * capabilities can't be read, we do NOT pause (return true) — matching
     * batteryPct() returning 100 on an unreadable battery state. This avoids
     * pausing the node on transient/unknown connectivity, at the honest cost
     * that a momentary "unknown" while on cellular would not pause.
     */
    private fun networkAllowed(): Boolean {
        if (!prefs.getBoolean(PREF_WIFI_ONLY, false)) return true
        return onUnmeteredWifi()
    }

    /** True iff the active network is WiFi AND not metered. Fail-OPEN (true)
     *  when the state is unreadable (no ConnectivityManager / no caps). */
    private fun onUnmeteredWifi(): Boolean {
        val cm = getSystemService(Context.CONNECTIVITY_SERVICE) as? ConnectivityManager
            ?: return true   // unknown -> fail open (don't pause)
        val net = cm.activeNetwork ?: return false  // no network -> not on wifi
        val caps = cm.getNetworkCapabilities(net) ?: return true  // unknown caps -> fail open
        return caps.hasTransport(NetworkCapabilities.TRANSPORT_WIFI) &&
               caps.hasCapability(NetworkCapabilities.NET_CAPABILITY_NOT_METERED)
    }

    /** The overall run-gate: power AND network must both allow. */
    private fun runAllowed(): Boolean = powerAllowed() && networkAllowed()

    /**
     * One pause/resume tick shared by the battery receiver and the network
     * callback. Boots when the (combined) gate is newly allowed and we aren't
     * running; pauses (stopSelf) when it is no longer allowed and we are.
     * stopSelf() is the only stop available (no native shutdown).
     */
    private fun reconcileGate() {
        val allowed = runAllowed()
        if (allowed && !running.get()) {
            appendLog("[ump] run-gate OK (battery above ${batteryFloor()}% or plugged; " +
                      "network OK) — relighting the star.\n")
            bootKernelOnce(bootNodeId, bootRelayHost, bootRelayPort, bootRelayKey)
        } else if (!allowed && running.get()) {
            if (!powerAllowed()) {
                appendLog("[ump] battery ≤${batteryFloor()}% and unplugged — pausing to " +
                          "protect the battery; will resume when charging.\n")
                // Charge-constrained resume job survives the stopSelf() process
                // death (the network gate is re-evaluated on the next boot).
                scheduleResumeJob()
            } else {
                appendLog("[ump] WiFi-only is on and you're not on unmetered WiFi — " +
                          "pausing; will resume when WiFi is back.\n")
                // No charge job here: the resume trigger is connectivity, which
                // the manifest receiver can't observe after process death. The
                // node resumes when the app/galaxy is reopened on WiFi, or via
                // the charge job if it was ALSO low on battery. (Honest bound,
                // see the report; matches the no-native-shutdown constraint.)
            }
            stopSelf()
        }
    }

    /**
     * Runtime ACTION_BATTERY_CHANGED receiver, registered for the whole
     * service lifetime. On each tick: boot if power is (newly) allowed and we
     * aren't running; pause (stopSelf) if power is no longer allowed and we
     * are running. stopSelf() is the only stop available (no native shutdown).
     */
    private fun registerPowerReceiver() {
        if (powerReceiver != null) return
        powerReceiver = object : BroadcastReceiver() {
            override fun onReceive(ctx: Context?, intent: Intent?) {
                // Combined battery + WiFi gate; scheduleResumeJob() is called
                // (only on the low-battery branch) BEFORE stopSelf(), while we
                // are still foreground, so scheduling is unambiguous. This
                // replaces the old (broken on Android 12+) ACTION_POWER_CONNECTED
                // → startForegroundService() resume path.
                reconcileGate()
            }
        }
        registerReceiver(powerReceiver, IntentFilter(Intent.ACTION_BATTERY_CHANGED))
    }

    /* --- WiFi-only network callback ------------------------------------ */

    /**
     * Observe connectivity changes so WiFi-only mode pauses when WiFi is lost
     * and resumes when it returns. The callback delegates to the same
     * reconcileGate() the battery receiver uses, so the two gates stay
     * consistent. Wrapped: a callback-registration failure must never crash
     * the service (then the battery-receiver tick is the only re-check).
     */
    private fun registerNetworkCallback() {
        if (netCallback != null) return
        val cm = getSystemService(Context.CONNECTIVITY_SERVICE) as? ConnectivityManager
            ?: return
        val cb = object : ConnectivityManager.NetworkCallback() {
            override fun onAvailable(network: Network) = reconcileGate()
            override fun onLost(network: Network) = reconcileGate()
            override fun onCapabilitiesChanged(network: Network,
                                               caps: NetworkCapabilities) = reconcileGate()
        }
        val req = NetworkRequest.Builder()
            .addCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
            .build()
        try {
            cm.registerNetworkCallback(req, cb)
            netCallback = cb
        } catch (_: Exception) {
        }
    }

    /* --- charge-constrained resume job --------------------------------- */

    /**
     * Schedule the ResumeJobService to run when the phone is charging. Called
     * at the moment of the low-battery pause (while still foreground). The job
     * is the Android-12+-safe replacement for the old ACTION_POWER_CONNECTED
     * receiver: a running JobService is an allowed context to start an FGS.
     *
     * Guard: only schedule when battery-safe mode is on and boot params exist
     * (mirrors the job's own resume guard). setPersisted(true) survives a
     * reboot-while-charging (needs RECEIVE_BOOT_COMPLETED). Wrapped so a
     * scheduling failure can never crash the pausing receiver.
     */
    private fun scheduleResumeJob() {
        if (!prefs.getBoolean(PREF_CHARGE_ONLY, true)) return
        if (!prefs.contains(EXTRA_NODE_ID)) return
        try {
            val js = getSystemService(Context.JOB_SCHEDULER_SERVICE) as? JobScheduler
                ?: return
            val job = JobInfo.Builder(
                    RESUME_JOB_ID,
                    ComponentName(this, ResumeJobService::class.java))
                .setRequiresCharging(true)
                .setPersisted(true)
                .build()
            js.schedule(job)
            appendLog("[ump] scheduled charge-resume job (fires when plugged in).\n")
        } catch (_: Exception) {
        }
    }

    /** Cancel any pending charge-resume job (called when the kernel (re)boots
     *  so a stale job can't double-fire). */
    private fun cancelResumeJob() {
        try {
            val js = getSystemService(Context.JOB_SCHEDULER_SERVICE) as? JobScheduler
                ?: return
            js.cancel(RESUME_JOB_ID)
        } catch (_: Exception) {
        }
    }

    /* --- notification --------------------------------------------------- */

    private fun statusText(relayHost: String, relayPort: Int): String =
        if (relayHost.isEmpty()) "local-loopback mesh"
        else                     "relay $relayHost:$relayPort"

    private fun buildNotification(nodeId: Int, status: String): Notification {
        // De-jargon: the surface (lockscreen/shade) shows no "kernel" and no
        // node number. The relay/loopback status stays as the (collapsed)
        // second line for the curious; it never says "kernel".
        val builder = NotificationCompat.Builder(this, CHANNEL_ID)
            .setSmallIcon(android.R.drawable.sym_def_app_icon)
            .setContentTitle(getString(R.string.notif_content))
            .setContentText(status)
            .setOngoing(true)
            .setPriority(NotificationCompat.PRIORITY_LOW)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            builder.setChannelId(CHANNEL_ID)
        }
        return builder.build()
    }

    /* --- ring-buffered tail log (read by MainActivity) ----------------- */

    companion object {
        const val CHANNEL_ID    = "ump-kernel"
        const val NOTIF_ID      = 1
        const val PREF_CHARGE_ONLY = "charge_only"   // now means "battery-safe mode"
        const val BATTERY_FLOOR_PCT = 30             // DEFAULT floor; the live value is PREF_BATTERY_FLOOR
        const val PREF_BATTERY_FLOOR = "battery_floor" // int %, user-configurable (10..50); default BATTERY_FLOOR_PCT
        const val PREF_WIFI_ONLY   = "wifi_only"     // bool; when on, run only on unmetered WiFi (default off)
        const val PREF_START_ON_BOOT = "start_on_boot" // bool; auto-start on device boot (default off)
        const val RESUME_JOB_ID = 7401               // fixed JobScheduler id for the charge-resume job

        const val EXTRA_NODE_ID    = "node_id"
        const val EXTRA_RELAY_HOST = "relay_host"
        const val EXTRA_RELAY_PORT = "relay_port"
        const val EXTRA_RELAY_KEY  = "relay_key"

        private const val LOG_CAP = 64 * 1024
        private val logBuf = StringBuilder(LOG_CAP)
        private val pending = StringBuilder()

        /* --- engineer-page snapshot (read by LogActivity) -----------------
         * The most recent boot params + durable store path, captured the
         * moment the kernel (re)boots. These are read-only facts a developer
         * may want to see; they are NOT shown on the friendly surface. */
        @Volatile var snapNodeId    = 0
            private set
        @Volatile var snapRelayHost = ""
            private set
        @Volatile var snapRelayPort = 0
            private set
        @Volatile var snapDataDir   = ""
            private set
        @Volatile var snapRunning   = false
            private set

        @Synchronized
        fun appendLog(s: String) {
            if (logBuf.length + s.length > LOG_CAP) {
                val drop = logBuf.length + s.length - LOG_CAP
                logBuf.delete(0, drop.coerceAtMost(logBuf.length))
            }
            logBuf.append(s)
            pending.append(s)
        }

        @Synchronized
        fun drainLog(): String {
            if (pending.isEmpty()) return ""
            val out = pending.toString()
            pending.setLength(0)
            return out
        }

        /** The whole ring buffer (engineer page renders the full tail). */
        @Synchronized
        fun snapshotLog(): String = logBuf.toString()
    }
}
