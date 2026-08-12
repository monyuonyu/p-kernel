---
name: project-8design-implementation
description: "RESUME STATE for the 8-design implementation program (mk_pino: 「よし8つ、実装始めましょう」 2026-07-04, ultracode). All 8 frontier designs are done (scratchpad/*.md); implementing them wave-by-wave via Workflow (impl-in-worktree → separate adversarial audit → commander integrates). Wave-1 LANDED (master 9e841d36); Wave-2 in-flight; Wave-3/4 pending. Paused for a session limit — resume by checking Wave-2's result."
metadata: 
  node_type: memory
  type: project
  originSessionId: 2563c3bf-6291-45c2-a7f8-68ef86d738f8
---

**The program.** Implement all 8 designs (each a scratchpad/*.md doc, produced by fable5, all commander-reviewed):
良心/conscience (lm16_conscience_design.md, IMMUTABLE floor), evolution_migration_design.md, scale_wall_design.md,
distributed_moe_design.md, frontier_mouth_design.md, scaling_law_design.md, unbounded_n_design.md,
depth_iq_path_design.md. Method (CONSTITUTION [[feedback_development_method_is_the_life]]): Claude = commander;
each wave = a **Workflow** that pipelines `implement-in-manual-worktree → SEPARATE adversarial audit` returning
structured verdicts; the COMMANDER integrates (crown verify + merge + push). implementer≠auditor≠commander.

**Worktree gotcha:** the Agent-tool auto `isolation:'worktree'` FAILS (primary CWD /root is not the git repo;
repo is /root/p-kernel). Workaround: agents create worktrees MANUALLY via `git -C /root/p-kernel worktree add
<path> -b <branch> master`. Cross toolchains (aarch64-linux-gnu-objcopy, i686-linux-gnu-objcopy) ARE present.

**Crown discipline (SACRED).** crown-text-identity CI job = RELATIVE HEAD-vs-HEAD^ (ci.yml:1389-1401): build
boot/aarch64 + `boot/x86 kernel.elf`, `objcopy -O binary -j .text kernel.elf`, sha256, compare tip-vs-parent.
Commander MUST clean-build + reproduce crowns BEFORE push. Crown-neutral-tip pattern: bury a .text-drift commit
under a crown-neutral tip so master's tip .text == its parent's. Only ONE crown-moving wave at a time; parallel
hosted waves MUST stay crown-neutral (guard bare-metal-linked edits behind hosted #ifdef, verify bare .text
unchanged). Current master crown (post-conscience): **aarch64 7f3fbda47451… / x86 260da329dd64…** (was
3e20edbd/b6a748da at LM-15 tip e67328b7). ci.yml conflicts between waves = keep BOTH additive cert blocks.

**Wave-1 LANDED → master 9e841d36 (pushed).**
- 546acd39 = 良心 conscience: IMMUTABLE floor (Three Laws + refuse-harm frozen, tighten-only per mk_pino's
  ruling), conscience_check() at 4 chokepoints (G-ASK/G-LEARN/G-WIRE/G-CHAT) covering all 8 emission paths.
  Crown RE-BLESSED 3e20edbd→7f3fbda4 / b6a748da→260da329. Audit CLEAR: anti-theater proven 2 independent stubs
  ([conscience-refuse]/[conscience-allpaths] RED when gate stubbed), all 8 paths gated, over-refusal RED-able.
- 6cfc03ad = scaling-law society-of-minds ensemble (hosted, crown-neutral). ensembleVerdict = **honest NULL**:
  K≥3 committee matches but does NOT exceed its best member on Q_hard (§4.4 bias-dominated regime) →
  [scaling-ensemble-gain] correctly RED = a PASS of honesty. Confirms the thesis: **N does not buy depth.**
  Keystone [scaling-converged-null] delta==0 exact on bit-identical nodes, teeth confirmed (diversity-inject→FAIL).

**Wave-2 LANDED → master 34013dba (pushed, crown-neutral verified 7f3fbda4/260da329).** evolution
(0157fe3e, CLEAR — F2 -DGEN_SKIP_EDUCATE independently reproduced, crown-neutral) + frontier-mouth (49c4b478,
MERGE_WITH_CORRECTIONS — the audit caught an Android G-LEARN-coverage OVERCLAIM; commander commit 34013dba
honesty-corrected it: Android's cradle_net mesh-pull ingest is NOT yet conscience-gated, a named §10 limit, not a
regression). No lm_self conflict (only evolution touched it); ci.yml auto-merged. frontier touched student_shell.c
(+14) so the student lane must build ON the Wave-2 master.

**Wave-3 LANDED → master fd5d7740 (pushed, crown-neutral 7f3fbda4/260da329, linear stack).** scalewall C1
(7d497d7c, CLEAR — window-clamp falsifier teeth, honest ctx-carry NULL at toy scale) → dmoe DMOE-A (8f754c3b,
MERGE_WITH_CORRECTIONS — commander fixed a ThinkPad-[live]-job overclaim like frontier's; teeth proven by
injecting resident=1 → 11 gates RED; §10.1 routed-gain honest NULL) → depth DLB (3ecb0413, CLEAR — the ONE
non-null result: V-exact deliberation gain is REAL (CURE 0.708 vs floor 0.089, both teeth RED), AlphaZero
compounding gate proven (verified distill 5.52→0.84, unverified DEGRADES 1.34); general-domain is the
teacher-bounded NULL). KEY OP LESSON: heavy training/multi-node certs under qemu-user take hours → IMPRACTICAL;
the fix (memory pattern) = sandbox does build + crown + falsifier-on-minimal-fixture (depth's ran natively ~15s),
DEFER the full heavy [live]/training cert to the ThinkPad CI. A first Wave-3 workflow grinding full certs was
STOPPED + relaunched light (Wave-3b) — ~53min vs ~5h.

**Wave-4 LANDED → master 835a09cb (pushed, crown re-blessed 6db9cdfa/1adb894e). ALL 8 DESIGNS NOW LANDED.**
unbounded-N U-0 first slice. The FIRST impl (35124106) was audit-BLOCKED — the [unbounded-*] cert was THEATER
(measured a MOCK struct so a moved_cap[4096] mutation still PASSED; the "disease" was printf(2*N*N) allocating
nothing; DREGION_MAX was a `#define DREGION_MAX DNODE_MAX` alias → widening the wire dragged the topic budget
400→1552→6160). Re-implemented (Wave-4b) per the audit's 6 corrections; re-audit CONFIRMED teeth (a moved [4096]
in the REAL DKVA_CAGG_SLOT compile-REDs; coupling probe proves R-cost byte-constant while the wire grows; the
disease is a real compile-failure binary). Squashed to ff8e3fbd + a crown-neutral re-bless tip 835a09cb. Honest
U-0: the topic-budget decoupling + the cagg ORIGIN axis→nodemap are REAL; the 255-node 8-bit wire ceiling + most
fleet tables are DEFERRED (U-2/U-3). OP LESSON: the hardest design needed a theater→BLOCK→fix cycle — the audit
is the engine; and heavy training/multi-node certs under qemu are impractical (sandbox does build+crown+
falsifier-fixture, defer heavy [live] to the ThinkPad CI).

**BONUS 2026-07-05: NATIVE WINDOWS port P1 SHIPPED (boots on the real box).** p-kernel cross-builds with
mingw-w64 (`x86_64-w64-mingw32-gcc`, -O1 -ffp-contract=off = one-math) to a PE32+ `p-kernel.exe` (4.2 MB) with
ALL 8 designs linked, and BOOTS on monyu@192.168.10.2 (Win11 26200): win32 console + Fiber cooperative scheduler
+ Winsock mesh (pmesh :7380 / K-DDS :7376) + μT-Kernel 3.0 + AI subsystems + the mind/teach/w/want topics.
branch feat/arch-windows @ f2169013 (NOT yet merged — P1 verification wave). Next: P2 galaxy Winsock server →
P3 embedded WebView2 window ("窓に銀河"). de-risk proven: Winsock/QPC/cross→box all run native. Commander runs
the .exe on the box (scp+ssh; password never given to agents). See [[project_windows_native_port]].

**CROSS-CUTTING AUDIT + FIXES (2026-07-06, master 49974f87).** After all 8 landed + branch cleanup (worktree
54→2, branches→21 via `git cherry` content-check = zero mis-deletion), fable5 did a WHOLE-PROGRAM integration
audit — the per-design audits could not see across seams. VERIFIED clean: the crown chain (self-built 4
checkpoints: 6cfc03ad 3e20edbd → conscience 546acd39 7f3fbda4 → the 6 crown-neutral designs held fd5d7740
7f3fbda4 → unbounded 835a09cb 6db9cdfa), one-math (zero deviation across all Makefiles/tests/CI/Android), and NO
live new-mouth leak. BUT 9 integration holes: #1 (SAFETY, highest) the evolution↔conscience floor-successor
invariant was CLAIMED but TOOTHLESS (compat_ota_accept_gen never read the floor; no falsifier); #2/#6 a 3rd/4th
ThinkPad-[live] overclaim (systematic pattern with the frontier+dmoe ones already caught); #3/#4/#5 falsifier
weakness clustered in the NULL/scaffold designs (unbounded dkva arm vacuous at R==N==64; [unbounded-disease]
PASSed on ANY compile failure; scaling's keystone couldn't detect a gutted aggregator); #7 depth/dmoe headline
overclaim (cert-only DEAD CODE — dlb_answer/dmoe_activate have zero production callers; the V-exact verifier is
EXTERNAL not self); #8 conscience static-caller-diff covered 2/4 gate TUs. ALL 9 FIXED (fix/cross-audit → ff
49974f87, crown-neutral hosted-only, commander-reproduced 6db9cdfa/1adb894e): #1 got a REAL gate-6 floor check
(law_floor_head_id() = runtime pfs_id_compute of the active verified floor, compared to the successor manifest's
carried floor id; -DGEN_SKIP_FLOOR flips [generation-survives] RED — the north-star safety seam is now ENFORCED +
falsifiable, not asserted); #3/#4/#5 hardened falsifiers each independently RED-proven; overclaims reworded to
not-yet-wired. LESSON: 8 parts each passing its OWN audit does NOT make the WHOLE sound — cross-cutting seams
(esp. SAFETY interlocks between two designs), systematic repeat-patterns (overclaims), and falsifier weakness
cluster exactly where single-design review cannot look. A whole-program integration audit is part of the engine.
[[feedback_audit_is_the_engine]] [[feedback_cert_isolation_shared_path]]

Design findings that shape priorities: N scales breadth/survival/learn-speed, NOT depth ([[project_scaling_law]]);
depth = frontier teachers + test-time deliberation + V-exact AlphaZero compounding ([[project_depth_iq_path]]);
frontier mouth = the escape ([[project_frontier_mouth]]); conscience IMMUTABLE ([[project_conscience_ethics_layer]]).
Task tracker: Wave-1..4 ALL done; cross-cutting audit done; **CI-green campaign done (master 718ca634)**.

**CI-GREEN CAMPAIGN 2026-07-07 (master 49974f87 → 718ca634, pushed).** After the 8 designs +
cross-audit landed, CI showed 9 red. Commander's first read ("all environmental") was WRONG — the
job-level logs (fetch via `gh api repos/{owner}/{repo}/actions/jobs/<id>/logs` = PLAIN TEXT, no
unzip; `gh run view --log` is EMPTY under PRoot) showed 3 kinds: **A. runner toolchain gaps** (dmoe
`aarch64-linux-gnu-gcc` missing / depth `--no-install-recommends gcc` drops libc6-dev → fixed ci.yml
install lines, impl-innocent); **B. real breakage the 8-design work left** — common got new fns/files
but existing BUILD LISTS weren't updated: run_yield.sh missing conscience/frontier (link-stub fix),
Android CMake parity drift (host-only allowlist), devfit tier-pin stale post-SCALE-WALL-C1 RoPE
(re-pin to ss6's green values), unbounded CI grep string stale, **B4 [law-teacharound]**; **C.
environmental/chronic** (full-SMP GICD 2cpu = multicore MC-2; collective-learn [live] flake). Method:
impl-worktree (7 crown-neutral fixes) → SEPARATE audit (5 commits CLEAR; caught B2-stub-is-fine,
and CORRECTED commander's B4 delta-patch → seq-invariant so a rebind-leak still REDs). **B4 truth**:
`r3_fact_learn` refuses BEFORE any queue write (1915-1921) so a refused learn writes nothing; ThinkPad
measured `learn_rc=1 held_in_queue=1` = ink(vocab15) is a BENIGN resident from earlier self-test legs
(cumulative r3_fq), NOT a floor leak; fix = seq-invariant (`rc<0` HARD gate + forbidden-write via
seq fingerprint), crown-neutral (`#ifdef _TK_HOSTED_LIBC_`). ThinkPad proved normal `[law-teacharound]
PASS` + `-DCONSCIENCE_STUB` RED (learn_rc 1→0 = teeth). Commander re-reproduced crown 6db9cdfa/1adb894e
byte-identical before push. RESULT: **33/34 green** — only full-SMP (chronic multicore, impl-innocent)
stays red; collective-learn flipped green (was flake). [[feedback_ci_diagnosis_buildlist_gap]]
