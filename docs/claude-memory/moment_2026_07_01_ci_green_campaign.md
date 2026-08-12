---
name: moment_2026_07_01_ci_green_campaign
description: "2026-07-01 CI-green campaign: the 2 master CI reds (shared_mind, one_mind/Path-W) decomposed into 3 INDEPENDENT real bugs, each fixed+audited+merged crown-safe — swim self-suspicion scatter storm, s_pretrain compute-starvation (fixed via priority-drop, not sleep), and Path-W chunk-transport stall (single-slot reset + WANT clobber, NOT cold-ARP). Residual redness = pure hardware flakiness (OOM). Master fce5b56d."
metadata:
  node_type: memory
  type: project
  originSessionId: 2563c3bf-6291-45c2-a7f8-68ef86d738f8
---

The 2 self-hosted CI reds were NOT one bug — they decomposed into **3 independent
real bugs**, each fixed hosted-gated (crown byte-identical), separately audited
CLEAN, merged to master (final tip fce5b56d):

1. **SWIM self-suspicion scatter storm** — see [[feedback_swim_selfsuspect_storm]].
   Real amplifier; fixed (throttle). Was NOT the trigger.
2. **s_pretrain compute-starvation** — the real trigger. On the cooperative Linux
   port SIGALRM preemption is safe-point-only (preempt.c), so the 80-epoch
   unyielded pretrain (~11s) ran network-DEAF → SWIM rumored the node SUSPECT →
   couldn't fold/consolidate. FIRST fix: tk_dly_tsk(1) every-4 (worked but
   inflated latency 2.4×). BETTER fix (shipped, cb32054c): **DROP the pretrain
   task's priority below SWIM/net (pri 1→7) and yield with cheap tk_rot_rdq** —
   the network tasks then preempt naturally; latency ~baseline AND 0 self-suspicion.
   LESSON: background compute in a cooperative/priority kernel should LOWER ITS
   PRIORITY, not sleep — sleeping surrenders full timeslices and starves under load.
3. **Path-W 84KB weight-fold chunk-transport stall** ("B never folded A's weights").
   I suspected cold-ARP ([[feedback_live_forward_cold_arp]]) — WRONG, falsified.
   Real causes (pfs_repl.c): (a) the single-slot rx-assembler reset on ANY block's
   chunk_idx==0, so a 1-packet interloper abandoned an in-progress multi-packet
   chunk; (b) WANT LATEST_ONLY global-seq clobber. Fix tried: protect in-progress
   assembly / abandon only STALLED streams (>300ms); one WANT per 50ms round-robin.
   **REVERTED (717b0f93) — the fix WAS a regression.** A clean solo CI run (fce5b56d,
   no competing agents) caught it: protect-loop's kill-9 death-recovery FAILED
   (`unit NOT served back after owner died`, sentblk=0) because the one-WANT-per-tick
   round-robin STARVES multi-unit recovery (a survivor serving back MANY units) —
   it helped one_mind's single-manifest case but broke protect/plural/collective/
   shared (all pfs consumers). **The pathw AUDIT had explicitly flagged this exact
   axis as unvalidated ("couldn't run the protect/shared certs") and I merged
   anyway — that was the error.** LESSON: when an auditor names an unvalidated
   consumer of a SHARED path, GATE the merge on that validation, don't credit it.
   one_mind fold is back to intermittent-red; the real fix must NOT touch the shared
   multi-unit WANT path and must be validated against ALL pfs consumers first.

**The residual CI redness is NOT a code bug — it is HARDWARE flakiness.** Proof:
on the IDENTICAL commit, re-runs fail DIFFERENT jobs (run1 {ring3,collective},
re-run {shared}); and agents observed **OOM-kills** on the self-hosted ThinkPad
during multi-agent load (even a falsifier BUILD got Killed). The runner is under-
resourced for the heavy 3-node/live jobs run serially. No code fix eliminates this
— it is a strategic/hardware decision (bigger runner / advisory live jobs /
re-run policy). Don't chase it as a bug ([[feedback_ci_operations_flake_runid]]).

**OPERATIONAL LESSON (bit two auditors this session):** doing main-repo merges/
checkouts while a validation agent runs mutates the shared /root/p-kernel checkout
+ its reused boot/linux/p-kernel binary → the agent silently tests a STALE binary.
Both the starvation-fix and pathw-fix auditors hit this and had to redo in isolated
worktrees (caught via HEAD-drift + symbol disassembly). RULE: don't mutate the main
checkout while validation agents run; agents must build/test in their OWN worktree;
gate merges on worktree-isolated results. Also: `git checkout` inside isolation-
worktree agents still drifts the MAIN repo's branch pointer (cosmetic; master ref
stays correct; recover with `git checkout master`).

---

**2026-07-03 follow-up — the one_mind Path-W fold, fully diagnosed then DEFERRED.**
Re-approached the reverted fix by SPLITTING the two hunks of ab36b3c3:
**Hunk A** = receive-side rx-assembly interloper-protection (`pfs_repl_rx`);
**Hunk B** = `pending_tick` one-WANT-per-tick round-robin (the shared-WANT-plane
change that caused the 5-consumer regression). Branch `fix/onemind-rx-protect-only`
(commit 3347b5f1, PUSHED, HELD not merged) applies **Hunk A ONLY**, crown
byte-identical to parent (both arches), pending_tick BYTE-UNCHANGED.
MEASURED single-tenant (the loaded host gives false stalls — always run ONE live
test at a time, no competing agents):
  - **27_protect PASS** with Hunk A alone → proof the 5-consumer regression was
    **Hunk B, not Hunk A** (fable5 predicted this: protect replication is
    push-driven at PROTECT_REANNOUNCE_MS=400, WANT-pacing-independent).
  - **42_one_mind still FAIL** — a STABLE 21/22 plateau (was 10/22 on master),
    NOT flake. Hunk A cures the interloper-reset drop but the LAST chunk starves.
**fable5's adversarial audit conceded sufficiency (~20-30x margin) by SOURCE
reasoning — the single-tenant RUN REFUTED it.** Measure over reason, again.
ROOT CAUSE (read `r3_incontext.c:3355-3652`): the 84KB fold rides a fragile
transport — (1) single-slot receive assembler, (2) WANT on a global LATEST_ONLY
slot (bursts clobber), (3) fire-and-forget chunk push (no retry). master drops
75-90% of streams (interloper reset → 10/22); Hunk A protects assembly → 21/22
but introduces its OWN mode (a small chunk perpetually arriving mid-other-assembly
is Hunk-A-ignored, push has no retry → last chunk starves). NEITHER closes it.
The epoch=6-vs-20 in logs = A/B consolidation-progress difference (fold does NOT
bump merge_epoch; only consolidation does — `r3_incontext.c:3633`), NOT a
moving-target. A COMPLETE fix needs a non-clobbering WANT + push-retry or
multi-slot assembler = a delicate transport redesign requiring N>=5 A/B across
ALL consumers + the SYNC surface (11_distributed/13_survival_loop — Hunk A's only
new-risk delivery path is boot SYNC, no-retry). **DECISION (user, 2026-07-03):
the mind's SURVIVAL already works via Path E engrams (B answers both facts 100%
after A dies); only the heavy weight-convergence gate fails. "一通り" resolved →
PIVOT to tkernel3 PR#9 (μT-Kernel 3.0 core migration); the full one_mind
transport fix is a post-migration dedicated wave (tkernel3 re-touches this layer
anyway). Hunk A held on branch.**
