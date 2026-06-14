/*
 * PKernelService.kt — UMP foreground service.
 *
 * Owns the libpkernel.so kernel thread for the lifetime of the
 * service. Posts a persistent notification ("p-kernel node #N —
 * connected to relay") so Android keeps the process alive past
 * the 5-minute background limit.
 *
 * Battery-safe floor (formerly "charge-only"): if the user hasn't
 * disabled it, the service runs on battery down to BATTERY_FLOOR_PCT
 * (~30%), then pauses to protect the battery and resumes when the
 * phone is plugged in. (The old name PREF_CHARGE_ONLY is kept for
 * back-compat with installs that already wrote that pref; its meaning
 * is now "battery-safe mode", default on.) Users who want always-on
 * can flip the SharedPreferences boolean off.
 *
 * Because there is NO native kernel shutdown (PKernel only exposes
 * nativeBoot), the only way to pause is stopSelf() — which kills the
 * whole foreground service/process and its runtime receiver. So the
 * policy is split:
 *   - LEVEL-based PAUSE uses a RUNTIME ACTION_BATTERY_CHANGED receiver
 *     (that broadcast cannot be manifest-declared); while the service
 *     is alive it watches the level and stopSelf()s at the floor.
 *   - CHARGE-based RESUME uses a MANIFEST-declared receiver for
 *     ACTION_POWER_CONNECTED (which survives the process death); it
 *     re-launches the service with the persisted boot params.
 *
 * Tail log: drains stdout from the kernel via PKernel.readStdout()
 * on a worker thread and keeps the last ~64 KB in a ring buffer so
 * MainActivity can render it.
 */

package io.pkernel

import android.app.Notification
import android.app.NotificationManager
import android.app.Service
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.SharedPreferences
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

        // Persist the boot params so the manifest-declared PowerConnectedReceiver
        // can relaunch this service after a low-battery stopSelf() (which kills
        // the process). Same "ump" prefs file MainActivity already writes.
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

        if (powerAllowed()) {
            bootKernelOnce(bootNodeId, bootRelayHost, bootRelayPort, bootRelayKey)
        } else {
            appendLog("[ump] battery-safe mode: battery is low and unplugged — " +
                      "the star will relight when you plug in.\n")
        }
        return START_STICKY
    }

    override fun onDestroy() {
        powerReceiver?.let {
            try { unregisterReceiver(it) } catch (_: IllegalArgumentException) {}
            powerReceiver = null
        }
        pollerThread?.interrupt()
        appendLog("[ump] service stopped.\n")
        super.onDestroy()
    }

    /* --- kernel boot ---------------------------------------------------- */

    private fun bootKernelOnce(nodeId: Int, relayHost: String,
                               relayPort: Int, relayKey: String) {
        if (!running.compareAndSet(false, true)) return
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

    /**
     * Battery-safe floor policy. If the user turned battery-safe mode OFF,
     * always run. Otherwise run while plugged in OR while the battery is
     * strictly above the floor. (Strictly-above so the pause condition —
     * at/below floor AND unplugged — is the exact complement and never flaps.)
     */
    private fun powerAllowed(): Boolean {
        if (!prefs.getBoolean(PREF_CHARGE_ONLY, true)) return true
        return isPlugged() || batteryPct() > BATTERY_FLOOR_PCT
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
                val allowed = powerAllowed()
                if (allowed && !running.get()) {
                    appendLog("[ump] power OK (plugged or battery above ${BATTERY_FLOOR_PCT}%) — relighting the star.\n")
                    bootKernelOnce(bootNodeId, bootRelayHost, bootRelayPort, bootRelayKey)
                } else if (!allowed && running.get()) {
                    appendLog("[ump] battery ≤${BATTERY_FLOOR_PCT}% and unplugged — pausing to protect the battery; will resume when charging.\n")
                    stopSelf()
                }
            }
        }
        registerReceiver(powerReceiver, IntentFilter(Intent.ACTION_BATTERY_CHANGED))
    }

    /* --- notification --------------------------------------------------- */

    private fun statusText(relayHost: String, relayPort: Int): String =
        if (relayHost.isEmpty()) "local-loopback mesh"
        else                     "relay $relayHost:$relayPort"

    private fun buildNotification(nodeId: Int, status: String): Notification {
        val builder = NotificationCompat.Builder(this, CHANNEL_ID)
            .setSmallIcon(android.R.drawable.sym_def_app_icon)
            .setContentTitle("p-kernel node #$nodeId")
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
        const val BATTERY_FLOOR_PCT = 30             // pause below this when unplugged

        const val EXTRA_NODE_ID    = "node_id"
        const val EXTRA_RELAY_HOST = "relay_host"
        const val EXTRA_RELAY_PORT = "relay_port"
        const val EXTRA_RELAY_KEY  = "relay_key"

        private const val LOG_CAP = 64 * 1024
        private val logBuf = StringBuilder(LOG_CAP)
        private val pending = StringBuilder()

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
    }
}
