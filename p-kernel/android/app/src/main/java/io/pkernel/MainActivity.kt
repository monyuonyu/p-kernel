/*
 * MainActivity.kt — UMP entry screen.
 *
 * Three EditText fields (node_id, relay host:port, key hex) plus
 * a "Start" / "Stop" button pair. Tapping Start hands the values
 * to PKernelService via Intent extras; the service is what actually
 * loads libpkernel.so and calls PKernel.bootWithRelay.
 *
 * A TextView at the bottom tail-streams the kernel's stdout (read
 * back through PKernel.readStdout). Minimal, deliberate — the goal
 * here is "first APK that turns a phone into a node," not a polished
 * terminal. v1.0 of the dashboard.
 */

package io.pkernel

import android.app.NotificationChannel
import android.app.NotificationManager
import android.content.Intent
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.text.method.ScrollingMovementMethod
import android.widget.Button
import android.widget.EditText
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity

class MainActivity : AppCompatActivity() {

    private lateinit var nodeIdField: EditText
    private lateinit var relayHostField: EditText
    private lateinit var relayPortField: EditText
    private lateinit var relayKeyField: EditText
    private lateinit var logView: TextView

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        ensureNotificationChannel()

        nodeIdField    = findViewById(R.id.field_node_id)
        relayHostField = findViewById(R.id.field_relay_host)
        relayPortField = findViewById(R.id.field_relay_port)
        relayKeyField  = findViewById(R.id.field_relay_key)
        logView        = findViewById(R.id.log_view)
        logView.movementMethod = ScrollingMovementMethod()

        findViewById<Button>(R.id.btn_start).setOnClickListener { startKernel() }
        findViewById<Button>(R.id.btn_stop ).setOnClickListener { stopKernel()  }

        /* Pull the service's tail-log every 250 ms while we're foregrounded. */
        val handler = Handler(Looper.getMainLooper())
        handler.post(object : Runnable {
            override fun run() {
                val tail = PKernelService.drainLog()
                if (tail.isNotEmpty()) {
                    logView.append(tail)
                    /* Auto-scroll to the bottom. */
                    val scrollAmount = logView.layout?.getLineTop(logView.lineCount)?.minus(logView.height) ?: 0
                    if (scrollAmount > 0) logView.scrollTo(0, scrollAmount)
                }
                handler.postDelayed(this, 250)
            }
        })
    }

    private fun startKernel() {
        val intent = Intent(this, PKernelService::class.java).apply {
            putExtra(PKernelService.EXTRA_NODE_ID,    nodeIdField.text.toString().toIntOrNull() ?: 1)
            putExtra(PKernelService.EXTRA_RELAY_HOST, relayHostField.text.toString().trim())
            putExtra(PKernelService.EXTRA_RELAY_PORT, relayPortField.text.toString().toIntOrNull() ?: 7400)
            putExtra(PKernelService.EXTRA_RELAY_KEY,  relayKeyField.text.toString().trim())
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            startForegroundService(intent)
        } else {
            startService(intent)
        }
    }

    private fun stopKernel() {
        stopService(Intent(this, PKernelService::class.java))
    }

    private fun ensureNotificationChannel() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val nm = getSystemService(NotificationManager::class.java)
            val ch = NotificationChannel(
                PKernelService.CHANNEL_ID,
                getString(R.string.notif_channel_name),
                NotificationManager.IMPORTANCE_LOW
            ).apply {
                description = getString(R.string.notif_channel_desc)
                setShowBadge(false)
            }
            nm?.createNotificationChannel(ch)
        }
    }
}
