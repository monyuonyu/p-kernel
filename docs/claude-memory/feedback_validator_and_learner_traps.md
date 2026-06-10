---
name: feedback-validator-and-learner-traps
description: "Two recurring traps when fixing a control system — the validator can model a different system than ships, and the online learner can keep optimizing the old objective after you flip a sign."
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 53149c59-d57f-4589-aa45-bb25220e2df2
---

Surfaced in p-kernel wave 13 (G20 sign fix) by the standing audit organ. Two
generalizable traps when changing a feedback/control system:

**1. The validator can be the wrong system (G27).** A host simulation or
"oracle" can validate a *different dynamical system* than the kernel ships
(continuous Laplacian diffusion with gain k, unbounded pressure) vs the real
code (discrete argmax + hysteresis, MOE_SWITCH_MARGIN, EWMA). Worse, it may not
even model the axis you're fixing. A green from such a sim certifies nothing.
**Demote sims to "illustration"; the certificate must be a production-code
self-test** (here: `[moe-protect]` calls the real `expert_utility` + `deadband_pick`).

**2. Flip a control sign? Check what the online learner optimizes (G29).** When
you invert a sign (load-shed → rally), any homeostat / online adaptation that
was tuning the *old* objective's gain will keep pushing against your fix unless
you retarget it. Here `learned_conserve` had to be re-pointed at the rally axis
so `reflex_deliberate` (dwell-too-long → raise gain) now means "rally harder,"
not "flee faster." Verify the learner's objective sign moved with everything else.

**Why:** both traps pass "all green" while the system does the wrong thing —
exactly the failure mode mk_pino forbids (honesty over a rubber stamp,
[[feedback-engagement-style]]).

**How to apply:** (a) when an audit hands you an acceptance test, verify the
real code + run it yourself, don't trust the agent's self-report
([[feedback-dynamic-workflow-integration]]); (b) make the green CI-enforced on
the production path, not sim/doc-only; (c) after any sign flip, grep for every
adaptive rule / learned gain that reads the changed signal and confirm its
objective flipped too. Pattern in action: have the audit隊 produce the pass/fail
criteria, then the commander reads the gate formula line-by-line — used for the
wave-12 close-loop verdict, the wave-13 G20 verdict, and the wave-14 G28 verdict.

**Wave-14 G28 proof that this is not paranoia:** three different agents reported
"all green" on the protected-object loop; running the LIVE 3-node kill-test
myself caught it failing **twice** (1/3, then still flaky) before it was real.
The disease (audit's own words) is "green self-test, dead live path": an
in-process plant that calls the counting hook in a loop passes while the live
distributed path silently undercounts (depth-1 LATEST_ONLY topic dropped
concurrent announce-backs) and the "control" leaks (boot-SYNC replicated the
object regardless of the actuator). **Rule: for any distributed/closed-loop
claim, the acceptance test is a LIVE multi-node kill-test you run yourself N
times (≥5 for flakiness), wired into CI — never the in-process self-test alone,
and never the agent's word.**

Related: [[project_regions_architecture]] [[project_survival_network]]
[[feedback_visualization_means_observability]] (the sim's PNG is illustration;
the ASCII/self-test signal is the real observable).
