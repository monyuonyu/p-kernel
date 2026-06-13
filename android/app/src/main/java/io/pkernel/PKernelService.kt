/*
 * PKernelService.kt — UMP foreground service.
 *
 * Owns the libpkernel.so kernel thread for the lifetime of the
 * service. Posts a persistent notification ("p-kernel node #N —
 * connected to relay") so Android keeps the process alive past
 * the 5-minute background limit.
 *
 * Charge-only gate: if the user hasn't disabled it, the service
 * refuses to actually boot the kernel until the phone is plugged in.
 * Implements the "battery-safe by default" promise from the project
 * README; users who want always-on can flip a SharedPreferences
 * boolean (UI for that lands in a later commit).
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
    private var pluggedReceiver: BroadcastReceiver? = null

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        prefs = getSharedPreferences("ump", Context.MODE_PRIVATE)
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        val nodeId    = intent?.getIntExtra(EXTRA_NODE_ID, 1) ?: 1
        val relayHost = intent?.getStringExtra(EXTRA_RELAY_HOST) ?: ""
        val relayPort = intent?.getIntExtra(EXTRA_RELAY_PORT, 7400) ?: 7400
        val relayKey  = intent?.getStringExtra(EXTRA_RELAY_KEY) ?: ""

        startForeground(NOTIF_ID, buildNotification(nodeId, statusText(relayHost, relayPort)))

        if (!chargeAllowed()) {
            appendLog("[ump] charge-only mode active — phone is not plugged in. " +
                      "Kernel will start when external power is connected.\n")
            registerPluggedReceiver(nodeId, relayHost, relayPort, relayKey)
            return START_STICKY
        }

        bootKernelOnce(nodeId, relayHost, relayPort, relayKey)
        return START_STICKY
    }

    override fun onDestroy() {
        pluggedReceiver?.let {
            try { unregisterReceiver(it) } catch (_: IllegalArgumentException) {}
            pluggedReceiver = null
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

    /* --- charge-only gate ---------------------------------------------- */

    private fun chargeAllowed(): Boolean {
        if (!prefs.getBoolean(PREF_CHARGE_ONLY, true)) return true
        val intent = registerReceiver(null, IntentFilter(Intent.ACTION_BATTERY_CHANGED))
        val plugged = intent?.getIntExtra(BatteryManager.EXTRA_PLUGGED, 0) ?: 0
        return plugged != 0
    }

    private fun registerPluggedReceiver(nodeId: Int, relayHost: String,
                                        relayPort: Int, relayKey: String) {
        if (pluggedReceiver != null) return
        pluggedReceiver = object : BroadcastReceiver() {
            override fun onReceive(ctx: Context?, intent: Intent?) {
                val plugged = intent?.getIntExtra(BatteryManager.EXTRA_PLUGGED, 0) ?: 0
                if (plugged != 0 && !running.get()) {
                    appendLog("[ump] external power connected — starting kernel.\n")
                    bootKernelOnce(nodeId, relayHost, relayPort, relayKey)
                }
            }
        }
        registerReceiver(pluggedReceiver, IntentFilter(Intent.ACTION_BATTERY_CHANGED))
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
        const val PREF_CHARGE_ONLY = "charge_only"

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
