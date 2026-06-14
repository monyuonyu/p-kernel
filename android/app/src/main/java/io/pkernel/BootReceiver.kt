/*
 * BootReceiver.kt — optional start-on-boot for the node.
 *
 * When PREF_START_ON_BOOT is ON (default OFF) AND persisted node params
 * exist, relight the star automatically after the device finishes booting.
 *
 * Why a BroadcastReceiver for ACTION_BOOT_COMPLETED is allowed to start a
 * foreground service: BOOT_COMPLETED is on Android's closed exemption list
 * for background FGS starts (unlike ACTION_POWER_CONNECTED, which throws
 * ForegroundServiceStartNotAllowedException on Android 12+ — that is why the
 * battery RESUME path uses a JobScheduler job instead, see ResumeJobService).
 *
 * Guards (mirror the resume-job guard): only start when the user opted in
 * (PREF_START_ON_BOOT) and persisted boot params exist. Everything is wrapped
 * in try/catch so a blocked or failed start can never crash the boot receiver.
 *
 * Declared exported=false in the manifest; only the system delivers
 * BOOT_COMPLETED. RECEIVE_BOOT_COMPLETED is already declared (it is also
 * needed by the battery-resume job's setPersisted(true)).
 */
package io.pkernel

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.os.Build

class BootReceiver : BroadcastReceiver() {

    override fun onReceive(context: Context, intent: Intent?) {
        if (intent?.action != Intent.ACTION_BOOT_COMPLETED) return
        try {
            val prefs = context.getSharedPreferences("ump", Context.MODE_PRIVATE)

            // Opt-in only, and only if there is a node to relight.
            if (!prefs.getBoolean(PKernelService.PREF_START_ON_BOOT, false)) return
            if (!prefs.contains(PKernelService.EXTRA_NODE_ID)) return

            val svc = Intent(context, PKernelService::class.java).apply {
                putExtra(PKernelService.EXTRA_NODE_ID,
                         prefs.getInt(PKernelService.EXTRA_NODE_ID, 1))
                putExtra(PKernelService.EXTRA_RELAY_HOST,
                         prefs.getString(PKernelService.EXTRA_RELAY_HOST, "") ?: "")
                putExtra(PKernelService.EXTRA_RELAY_PORT,
                         prefs.getInt(PKernelService.EXTRA_RELAY_PORT, 7400))
                putExtra(PKernelService.EXTRA_RELAY_KEY,
                         prefs.getString(PKernelService.EXTRA_RELAY_KEY, "") ?: "")
            }
            // Defense-in-depth: a blocked start must never crash the receiver.
            try {
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                    context.startForegroundService(svc)
                } else {
                    context.startService(svc)
                }
            } catch (_: Exception) {
            }
        } catch (_: Exception) {
        }
    }
}
