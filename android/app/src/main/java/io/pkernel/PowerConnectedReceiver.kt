/*
 * PowerConnectedReceiver.kt — battery-safe RESUME after a low-battery pause.
 *
 * When PKernelService hits the battery-safe floor while unplugged it calls
 * stopSelf(), which kills the whole foreground service/process AND its
 * runtime ACTION_BATTERY_CHANGED receiver. So nothing alive is left to notice
 * when the phone is plugged back in.
 *
 * This MANIFEST-declared receiver survives the process death. On
 * ACTION_POWER_CONNECTED it reads the boot params PKernelService persisted
 * into the shared "ump" prefs and relaunches the foreground service, which
 * boots the kernel again (powerAllowed() is true while plugged in).
 *
 * Guards:
 *   - only acts when battery-safe mode (PREF_CHARGE_ONLY) is ON; if the user
 *     opted out, the service is already always-on and there's nothing to do.
 *   - the service's own bootKernelOnce() compareAndSet protects against any
 *     double-start, so a redundant startForegroundService here is harmless.
 *   - if no boot params were ever persisted (service never started), do
 *     nothing — there is nothing to resume.
 *
 * Note: ACTION_BATTERY_CHANGED (the LEVEL-based PAUSE trigger) cannot be
 * manifest-declared, which is why the pause lives in the runtime receiver and
 * only the CHARGE-based resume lives here.
 */
package io.pkernel

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.os.Build

class PowerConnectedReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent?) {
        if (intent?.action != Intent.ACTION_POWER_CONNECTED) return

        val prefs = context.getSharedPreferences("ump", Context.MODE_PRIVATE)

        // Respect the user's opt-out: if battery-safe mode is off the service
        // is meant to be always-on and isn't being paused, so don't interfere.
        if (!prefs.getBoolean(PKernelService.PREF_CHARGE_ONLY, true)) return

        // Need persisted boot params; if the service never started there is
        // nothing to resume (don't spin up a kernel the user never asked for).
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
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            context.startForegroundService(svc)
        } else {
            context.startService(svc)
        }
    }
}
