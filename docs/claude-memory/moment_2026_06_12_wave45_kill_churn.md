---
name: moment-2026-06-12-wave45-kill-churn
description: "wave-45 — the CR3-reload \"ROOT FIX\" was a REGRESSION (master 24/24 PASS, fix ~46% crash); reproducer drift discovered; KILL-CHURN row stays OPEN"
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 2563c3bf-6291-45c2-a7f8-68ef86d738f8
---

# 2026-06-12 — wave-45: the fix WAS the disease (KILL-CHURN fourth falsification)

The lane found "the dispatcher never reloads CR3" and shipped a per-dispatch CR3
reload as a ROOT FIX with a convincing convicted-mechanism story (fault dumps
showed CR3 == pt_pool slot, never kernel_cr3). The continuation agent's ablation
matrix falsified it BACKWARDS: **master = 24/24 PASS (0%), the candidate fix =
11/24 CRASH, CR3-reload alone = 7/14 CRASH** — the fix CAUSED the exact signature
it claimed to cure (the reload parks the CPU on a process address space exactly
when a timer-IRQ dispatch runs `knl_wait_release_tmout`). Both commits reverted;
only gated KCC_DIAG instrumentation shipped.

**Why:** Two traps compounded. (1) The diagnosis evidence (CR3==pt_pool at fault)
was TRUE on master too — without per-dispatch reload, CR3 legitimately stays on
whatever `user_exec` last loaded; the observation didn't discriminate cause from
ambient state. (2) The predecessor's "3 reproducible dumps" were taken on builds
that ALREADY contained its own CR3 changes — it diagnosed its own bug.

**Reproducer drift (the bigger lesson):** the deterministic `dproc churn` verb
does NOT reproduce the disease on current master at all. The historic "~42%
unchanged" baseline was either a different choreography or already suppressed by
prior hardening (poison-on-free / ctxtsk guard / timer hygiene).

**How to apply:**
- Before crediting ANY fix, demand a control run of the UNFIXED base in the SAME
  harness, same day. "Disease real" certified months/waves ago does not transfer.
- When a diagnosis rests on an observed register/state value, ask: would a healthy
  system show the same value? (CR3==pt_pool would.)
- An implementer's repro taken on a tree containing its own changes is not a
  baseline. ([[feedback-validator-and-learner-traps]] generalizes: certify against
  production code, not the lane's own oracle.)
- Watchdog-stalled agents: their last message may claim "rebuilding with the FULL
  fix" — treat the claim as untested, not as a checkpoint of truth.
- KILL-CHURN row: next lane must FIRST build a reproducer that fails on master
  (tighter kill-burst, stale-tid name→map path) before touching any fix.
