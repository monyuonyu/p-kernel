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
 *
 * The PIERCE: when the owner taps 灯す while the gate is closed, the star
 * lights anyway (EXTRA_USER_EXPLICIT). Before this class the service did not
 * REMEMBER that: ACTION_BATTERY_CHANGED is sticky, so the receiver registered
 * in onStartCommand fired at once, saw "closed gate + running" and stopSelf()ed
 * ~100 ms after the boot (CI emulator smoke, run 44774377700). Now the pierce
 * holds until the gate opens once (plugged in / above the floor / back on
 * WiFi); from then on a closing gate pauses the node as before. Background
 * starts (BootReceiver, ResumeJobService, sticky restart) never pierce.
 */

package io.pkernel

import java.util.concurrent.atomic.AtomicBoolean

class RunGate {
    enum class Action { BOOT, STOP, NONE }

    // Ticks come from the main looper (battery receiver) AND the
    // ConnectivityManager callback thread, so the flag is atomic.
    private val pierced = AtomicBoolean(false)

    /** True while an owner's tap is holding the star lit through a closed gate. */
    val isPierced: Boolean get() = pierced.get()

    /** onStartCommand: should this start boot the kernel? */
    fun onStart(allowed: Boolean, userExplicit: Boolean): Boolean {
        if (allowed) {
            pierced.set(false)
            return true
        }
        if (userExplicit) {   // NC-EXPLICIT-ONLY
            pierced.set(true)
            return true
        }
        return false
    }

    /** One reconcile tick (battery receiver / network callback). */
    fun reconcile(allowed: Boolean, running: Boolean): Action {
        if (allowed) {
            pierced.set(false)   // NC-CLEAR-PIERCE: the gate opened once, the pierce is spent
            return if (running) Action.NONE else Action.BOOT
        }
        if (!running) return Action.NONE
        if (pierced.get()) return Action.NONE   // NC-KEEP-PIERCE
        return Action.STOP
    }
}

/*
 * B5Gate — when the B5 round may run on a phone (research/b5-android-gate.md
 * §2 3): only while charging AND on unmetered Wi-Fi. Unlike the run-gate,
 * an unknown network state CLOSES it (a round costs data; the run-gate
 * fails open so a node is not paused by a transient). PKernelService sends
 * the answer to the kernel (PKernel.nativeSetB5Allowed) on every tick.
 */
object B5Gate {
    enum class Net { UNMETERED_WIFI, METERED, NONE, UNKNOWN }

    fun allowed(charging: Boolean, net: Net): Boolean {
        if (!charging) return false              // NC-B5-CHARGING
        if (net == Net.UNKNOWN) return false     // NC-B5-UNKNOWN-CLOSED
        return net == Net.UNMETERED_WIFI
    }
}
