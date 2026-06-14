/*
 * ResumeJobService.kt — battery-safe RESUME after a low-battery pause.
 *
 * When PKernelService hits the battery-safe floor while unplugged it calls
 * stopSelf(), which kills the whole foreground service/process AND its
 * runtime ACTION_BATTERY_CHANGED receiver. So nothing alive is left to notice
 * when the phone is plugged back in.
 *
 * The old design used a manifest ACTION_POWER_CONNECTED BroadcastReceiver that
 * called startForegroundService() from the background. On Android 12+ (this app
 * is targetSdk 34) that throws ForegroundServiceStartNotAllowedException —
 * ACTION_POWER_CONNECTED is NOT in Android's closed exemption list for
 * background FGS starts — so the node would pause at 30% and never resume.
 *
 * The fix: schedule a JobScheduler job with setRequiresCharging(true) at the
 * moment of the low-battery pause (while still foreground, so scheduling is
 * unambiguous). When the phone is plugged in, the framework runs this
 * JobService — and a running JobService IS an allowed context to start a
 * foreground service (jobs are exempt from the background-FGS-start
 * restriction). We read the persisted boot params and (re)start the FGS.
 *
 * setPersisted(true) (set on the JobInfo at schedule time) lets a paused node
 * resume after a reboot-while-charging; it requires RECEIVE_BOOT_COMPLETED.
 *
 * Guards (mirrors the schedule-side guard): only resume when battery-safe mode
 * (PREF_CHARGE_ONLY) is ON and persisted boot params exist.
 */
package io.pkernel

import android.app.job.JobParameters
import android.app.job.JobService
import android.content.Context
import android.content.Intent
import android.os.Build

class ResumeJobService : JobService() {

    /**
     * Runs (on the main thread) when the charging constraint is met. A
     * JobService is an allowed context to start an FGS, so this is the
     * Android-12+-safe replacement for the old ACTION_POWER_CONNECTED receiver.
     * Work is synchronous, so we jobFinished() and return false.
     */
    override fun onStartJob(params: JobParameters?): Boolean {
        try {
            val prefs = getSharedPreferences("ump", Context.MODE_PRIVATE)

            // Respect the user's opt-out: if battery-safe mode is off the
            // service is meant to be always-on and isn't being paused.
            // And: need persisted boot params or there is nothing to resume.
            if (prefs.getBoolean(PKernelService.PREF_CHARGE_ONLY, true) &&
                prefs.contains(PKernelService.EXTRA_NODE_ID)) {

                val svc = Intent(this, PKernelService::class.java).apply {
                    putExtra(PKernelService.EXTRA_NODE_ID,
                             prefs.getInt(PKernelService.EXTRA_NODE_ID, 1))
                    putExtra(PKernelService.EXTRA_RELAY_HOST,
                             prefs.getString(PKernelService.EXTRA_RELAY_HOST, "") ?: "")
                    putExtra(PKernelService.EXTRA_RELAY_PORT,
                             prefs.getInt(PKernelService.EXTRA_RELAY_PORT, 7400))
                    putExtra(PKernelService.EXTRA_RELAY_KEY,
                             prefs.getString(PKernelService.EXTRA_RELAY_KEY, "") ?: "")
                }
                // Defense-in-depth: a blocked start must never crash the job.
                try {
                    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                        startForegroundService(svc)
                    } else {
                        startService(svc)
                    }
                } catch (_: Exception) {
                }
            }
        } catch (_: Exception) {
        }
        // Synchronous work done; no reschedule.
        jobFinished(params, false)
        return false
    }

    /** Nothing async is running, so there is nothing to stop. */
    override fun onStopJob(params: JobParameters?): Boolean = false
}
