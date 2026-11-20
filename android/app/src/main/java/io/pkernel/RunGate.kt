/*
 * RunGate.kt — the pure battery/WiFi run-gate decision of PKernelService.
 *
 * Split out of PKernelService so the decision can be unit-tested on a plain
 * JVM (no Android imports here): tests/android/run_gate_unit.sh compiles this
 * file together with tests/android/rungate/RunGateTest.kt.
 *
 * The service asks two questions:
 *   onStart(allowed, userExplicit)  — onStartCommand: boot the kernel now?
 *   reconcile(allowed, running)     — each battery/network tick: boot, stop
 *                                     (stopSelf), or leave it alone?
 * `allowed` is PKernelService.runAllowed() (battery floor AND WiFi-only).
 */

package io.pkernel

class RunGate {
    enum class Action { BOOT, STOP, NONE }

    /** onStartCommand: should this start boot the kernel? */
    fun onStart(allowed: Boolean, userExplicit: Boolean): Boolean =
        allowed || userExplicit

    /** One reconcile tick (battery receiver / network callback). */
    fun reconcile(allowed: Boolean, running: Boolean): Action = when {
        allowed && !running -> Action.BOOT
        !allowed && running -> Action.STOP
        else -> Action.NONE
    }
}
