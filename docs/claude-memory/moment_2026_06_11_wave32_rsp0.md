---
name: moment_2026_06_11_wave32_rsp0
description: "wave-32 — the kill-churn crash mechanism CORRECTED and mitigated: the dispatcher never refreshed TSS.RSP0 (stale-timer theory falsified empirically). +21 asm lines, ~10x rarer crashes, row stays honestly OPEN (IST stack = principled close). Audit prevented a master-reverting merge."
metadata:
  node_type: memory
  type: project
---

**2026-06-11, wave-32.** The KILL-TIMER-RACE follow-up resolves into a better truth:
the ledger's "stale timer fires on a dead TCB" theory was **falsified** (timer queue intact
at every fault; the "garbage callbacks" were `event->time` misread at the callback offset —
TMEB layout, timer.h:52). **Real mechanism:** `knl_dispatch` never refreshed TSS.RSP0 (set
only at user_exec launch); killing a ring3 daemon frees its kernel stack; the next
ring3→ring0 trap pushes the CPU frame into freed memory → garbage-PC ring0 #PF. Fix:
`gdt_set_kernel_stack(ctxtsk->isstack)` every dispatch (+21 lines cpu_support.S, liveness
audited, RSP0 written before sti). Cherry-picked to master as wave-32; disease reproduced by
BOTH implementer and auditor on unfixed master; 20-kill bursts now survive; all 4 x86 CI
gates green. **Row stays OPEN honestly:** residual ~10× rarer same-mode crash under heavy
churn = IRQ-frame-on-deep-stack; the principled close is an IST stack (TSS IST1).

**Two method lessons:**
1. **The audit prevented a master-reverting merge.** The fix branch was cut from pre-galaxy
   master; `git merge` would have REVERTED galaxy v1 + wave-31 (3921 deletions). The auditor
   caught it and prescribed cherry-pick-only (`9cc5a5f`). When lanes run long, ALWAYS check
   the branch base vs current master before merging — `git diff master <branch> --stat`
   looking for unexpected deletions.
2. The implementer (Fable, 502 tool uses) found the real mechanism by REFUSING the ledger's
   theory when evidence contradicted it ([[feedback_validator_and_learner_traps]] energy),
   and reported the unmet bar instead of weakening it. The auditor then independently
   confirmed mechanism + disease + residual, and judged the wtmeb latent bug correctly
   non-causal (timer.c:130 QueInit self-heals on first arm).

Sub-note for future task-creation work: init `wtmeb.queue` in tk_cre_tsk (latent hygiene).
