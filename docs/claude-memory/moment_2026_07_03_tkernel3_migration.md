---
name: moment_2026_07_03_tkernel3_migration
description: "2026-07-03 — the kernel core itself was swapped: micro T-Kernel 2.0 → μT-Kernel 3.0 landed on master (merge bbc0d216, tip 1639faed). First INTENTIONAL crown re-bless. A 5-phase adversarial campaign (verify-build / recon / integrate / audit / re-bless) driven entirely by delegated worktree agents + fable5. All 4 targets + the full ThinkPad live suite green on the new core; arch/common (the distributed brain) untouched."
metadata:
  node_type: memory
  type: project
  originSessionId: 2563c3bf-6291-45c2-a7f8-68ef86d738f8
---

The **body was replaced under the living mind without the mind noticing** — PR #9
swapped the kernel core micro T-Kernel 2.0 → **μT-Kernel 3.0** (IEEE 2050-2018,
702 files, +44903/-34858, vendored `kernel/mtkernel3/`, old 2.0 core + unused
rl78/h8300 ports deleted), and **`arch/common/` — swim/dtr/moe/dmn/kdds/pfs, the
entire distributed brain — has ZERO diff**. The mind rides a new body unchanged.

Run as a 5-phase adversarial campaign, commander orchestrating, EVERY step a
delegated worktree agent (impl) or fable5/general agent (audit), never the
commander's own hands:
- **A verify-build**: the PR's "全4ターゲット通過" was FALSE on modern GCC — 3
  implicit-decl defects (GCC14+ = error; author built on ≤13). Mechanical.
- **B recon**: `git merge-tree` clean (0 conflicts); all 6 hard-won port lessons
  PRESERVED (x86_64 mcontext-fpregs, DI/EI shadow, SP 16B align, LP64 knl_memset,
  aarch64 IRQ C-ABI, TCB _Static_asserts) with file:line; deletions carry nothing
  p-kernel-specific.
- **C integrate**: clean merge + the 3 build fixes; pathw stays reverted / Hunk A
  held (the merge auto-took master's pfs_repl.c).
- **D audit + full test**: independent auditor GO-WITH-CARE (re-verified the 3.0
  scheduler/time/DI-EI/SYSTIM semantics the brain depends on); ring3 was the ONE
  red — diagnosed NOT a fork/scheduler wedge but a **build regression**: the 3.0
  `kernel.h` relocation wasn't followed in `userland/x86/Makefile` (needed the
  mtkernel3 -I set AND `-D_X86_PC_`). Fixed (Makefile-only, crown-neutral).
  Final ThinkPad CI: **19 success / 2 failure** = crown (intentional) + smp
  (advisory) — every real live gate green on 3.0.
- **E re-bless**: the FIRST intentional crown change. New dev crowns
  **aarch64 5e42f853… / x86 a52c8701…** (was 755a20fa… / 4064d8a9…), reproduced
  byte-identical by FOUR independent clean builds, recorded in
  [[feedback_memory_mirror_to_repo]]-style docs/audit-trail.md + the ci.yml gate
  comment. The gate is relative (HEAD-vs-parent), so master's TIP stays crown-GREEN
  (the drift is at the buried merge commit, documented) — no lowered bar.

**TRAPS that bit / lessons:**
- **Shallow clone → "refusing to merge unrelated histories".** The main checkout
  was a shallow clone; the integration branch's ancestry (bbc0d216 → e4dcc1b2) was
  cut at the shallow boundary so git saw NO common ancestor. Cure: `git fetch
  --unshallow origin`, THEN the FF/merge works.
- **x86 crown needs `make -C boot/x86 clean` FIRST.** A stale 2.0-core incremental
  build yields a DIFFERENT .text (I got a wrong hash until I cleaned). aarch64 I'd
  cleaned; x86 I hadn't. The crown-identity CI job always cleans — so must any
  manual re-bless check. (Extends [[feedback_x86_64_mcontext_fpregs]]-era care.)
- The self-hosted ThinkPad CI runner is a **docker container `pkernel_gh_runner`**;
  it had OOM-died (Exit 137) after a maintenance reboot → CI queued ~5.7h. Fix:
  `docker start` + `docker update --restart unless-stopped` (mk_pino authorized).

Same day: fable5 delivered 3 design-first slices (LM-12 belief revision, `recip`
reciprocal mutual-aid [§7], the follow-ups plan) — see
[[project_living_mind_vision]] / [[project_survival_network]]. The `recip`
philosophy call (should the aid economy self-defend vs free-riders?) is left to
mk_pino. LM-12 + follow-ups dispatched to implementers post-merge.
