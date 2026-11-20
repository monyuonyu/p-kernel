/*
 * RunGateTest.kt — JVM unit test for android/.../RunGate.kt (the run-gate
 * decision of PKernelService). Run by tests/android/run_gate_unit.sh.
 *
 * The bug this pins (CI emulator smoke, run 44774377700): the owner taps 灯す
 * below the battery floor -> onStartCommand pierces the gate and boots ->
 * ACTION_BATTERY_CHANGED is STICKY, so the freshly registered receiver fires
 * at once -> reconcile(allowed=false, running=true) -> stopSelf() ~100 ms
 * after the boot. The pierce must survive the gate ticks until the gate
 * opens once; after that the floor pauses the node again as before.
 */

package io.pkernel

import kotlin.system.exitProcess

private var fails = 0
private var passes = 0

private fun check(name: String, ok: Boolean, detail: String) {
    if (ok) { passes++; println("PASS $name") }
    else    { fails++;  println("FAIL $name — $detail") }
}

fun main() {
    val B = RunGate.Action.BOOT
    val S = RunGate.Action.STOP
    val N = RunGate.Action.NONE

    // G1: the smoke bug. Explicit tap below the floor boots, and the sticky
    // battery tick right after must NOT stop the pierced node.
    RunGate().let { g ->
        val boot = g.onStart(allowed = false, userExplicit = true)
        val a = g.reconcile(allowed = false, running = true)
        check("G1-pierce-survives-sticky-tick", boot && a == N, "boot=$boot tick=$a (want true, NONE)")
    }
    // G2: many closed-gate ticks (battery level drifting down) keep the pierce.
    RunGate().let { g ->
        g.onStart(false, true)
        val acts = (1..5).map { g.reconcile(false, true) }
        check("G2-pierce-survives-many-ticks", acts.all { it == N }, "ticks=$acts")
    }
    // G3: background/auto start (BootReceiver, ResumeJobService, sticky restart)
    // with a closed gate never boots and never pierces.
    RunGate().let { g ->
        val boot = g.onStart(allowed = false, userExplicit = false)
        val a = g.reconcile(false, false)
        check("G3-background-start-holds", !boot && a == N, "boot=$boot tick=$a (want false, NONE)")
    }
    // G4: normal floor pause is unchanged: started with the gate open, the gate
    // closes -> STOP.
    RunGate().let { g ->
        val boot = g.onStart(allowed = true, userExplicit = false)
        val a = g.reconcile(false, true)
        check("G4-floor-pauses-unpierced", boot && a == S, "boot=$boot tick=$a (want true, STOP)")
    }
    // G5: the pierce ends once the gate opens (plugged in / above the floor);
    // the next floor crossing pauses as usual.
    RunGate().let { g ->
        g.onStart(false, true)
        val a1 = g.reconcile(true, true)
        val a2 = g.reconcile(false, true)
        check("G5-pierce-ends-when-gate-opens", a1 == N && a2 == S, "open=$a1 close=$a2 (want NONE, STOP)")
    }
    // G6: an explicit tap while the gate is ALREADY open is not a pierce.
    RunGate().let { g ->
        g.onStart(allowed = true, userExplicit = true)
        val a = g.reconcile(false, true)
        check("G6-explicit-tap-on-open-gate-no-pierce", a == S, "tick=$a (want STOP)")
    }
    // G7: resume — gate opens while the service waits unbooted -> BOOT.
    RunGate().let { g ->
        g.onStart(false, false)
        val a = g.reconcile(true, false)
        check("G7-resume-boots", a == B, "tick=$a (want BOOT)")
    }

    // G8 (audit-202): a later start that finds the gate OPEN spends the pierce
    // too (not only a reconcile tick); the next floor crossing pauses.
    RunGate().let { g ->
        g.onStart(false, true)
        val boot = g.onStart(allowed = true, userExplicit = false)
        val a = g.reconcile(false, true)
        check("G8-open-start-ends-pierce", boot && a == S, "boot=$boot close=$a (want true, STOP)")
    }
    // G9 (audit-202): a pierce never boots from a closed-gate tick by itself
    // (booting is onStart's job; a tick with the gate closed only holds).
    RunGate().let { g ->
        g.onStart(false, true)
        val a = g.reconcile(false, false)
        check("G9-closed-tick-never-boots", a == N, "tick=$a (want NONE)")
    }

    println("[rungate] $passes passed, $fails failed")
    exitProcess(if (fails == 0) 0 else 1)
}
