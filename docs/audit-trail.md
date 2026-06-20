# p-kernel independent audit trail

This file is the project's independent-auditor ledger: each row records that a
SEPARATE actor (an independent adversarial subagent, NOT the implementer)
reproduced the evidence for a shipped wave, in git. It exists because the
2026-06-20 harsh review found that "implementer != auditor" left NO independent
trace in the repository's own history. A row here is that trace.

## SS-6-live — commit 019ae96d (the [live] cross-node forward)
- Auditor: independent adversarial subagent (this run), 2026-06-20. Did NOT write the code.
- Verdict: **PASS (ledger row CLOSED).** This is a GENUINE live multi-process
  cross-node forward, byte-identical to single-node, and the byte-identity is
  REAL — proven by breaking the responder (below), NOT gamed.
- BASE NIT: the audit task said HEAD~1 MUST == 14783beb, but the ACTUAL direct
  parent of 019ae96d is 57ae7623 ("SS-6-live ... skeleton"), itself a child of
  14783beb. Chain: 14783beb -> 57ae7623 (skeleton transport) -> 019ae96d (the
  cert). 14783beb IS an ancestor (`git merge-base --is-ancestor` = YES); the
  skeleton commit was simply separate from the cert commit. Benign — NOT a
  base-mismatch in substance. The full 14783beb..019ae96d diff touches NO math
  TU (student.c is absent from the diff: the SS-6 hook at student.c:655-663 was
  already in 14783beb); the live wave ADDS only ss6_live.{c,h}, the ss6live verb
  in student_shell.c, the two usermains, the two hosted Makefiles, scripts, docs.
- [LIVE] REPRODUCED (3 runs, stable), fresh build at 019ae96d, aarch64 host:
  `bash samples/11_distributed/run_ss6_live.sh` -> exit 0, PASS.
  - 4 real p-kernel PROCESSES (node1 requester + node2,3,4 responders) + ./relay.
  - single-node oracle hash = LIVE cross-node hash = `00d6136e6d4676a3` (IDENTICAL).
  - remote_fired=266, fallback=0, wire_sent=266. Stable across all 3 runs.
  - RELAY FRAME EVIDENCE (the proof it crossed the wire, NOT local): /tmp/ss6l_relay.log
    frame-size histogram shows EXACTLY **266 frames of 1118 B (REQ) and 266 of
    1114 B (REP)** — the [D]=256-float REQ/REP vectors. 266 out + 266 back == the
    counters. Real UDP-over-relay traffic, not fabrication.
- **BYTE-IDENTITY IS REAL, NOT GAMED — I broke the responder (the decisive test):**
  I patched `st_expert_forward_ref` (student.c:780) to add `out[0] += 1.0f`, which
  corrupts ONLY the responder's [D] reply (the single-node ORACLE inlines its own
  SwiGLU at student.c:667-694 and never calls st_expert_forward_ref, so the oracle
  is UNAFFECTED). Rebuilt, re-ran the live cert:
  - oracle hash stayed `00d6136e6d4676a3` (UNCHANGED — confirms oracle path is independent)
  - **LIVE hash DIVERGED to `7c40a555fda2e89e`**, remote_fired=298, fallback=0;
    the script reported "DIVERGENCE" exactly as designed.
  => the wire output is GENUINELY summed into moe[] (student.c:702-705); if the path
  secretly recomputed locally, the live hash would have stayed identical. It did NOT.
  The cert is NOT all-local-dressed-as-live. (Sabotage reverted; clean rebuild restored
  `00d6136e6d4676a3`.)
- FALLBACK ARM REPRODUCED: `bash samples/11_distributed/run_ss6_live_fallback.sh`
  -> exit 0, PASS. A peer killed but still ALIVE in placement -> experts time out
  -> recomputed LOCALLY -> STILL byte-identical (`00d6136e6d4676a3`), no stall.
  Observed fallback=3 / remote_fired=197 (commit/docs say fallback=4 — benign
  run-to-run variance in WHICH experts the killed node owned; the load-bearing
  claim — local recompute, byte-identical, no stall — holds EXACTLY).
- CANONICAL ORDER + CROWN: the remote [D] writes into eo_all[j] (student.c:657, =eo)
  and the canonical ascending-slot sum (student.c:696-705) reduces eo_all[j] for
  local AND remote alike -> same order as single-node. rw[]/gl_merge/kv_step/backward
  UNTOUCHED (student.c not in the diff at all). `run_kv` 18/18 byte-identical (all
  [kv-equivalence] cases MATCH); `run_ss6` 7/7 (incl. EQ_HASH L MATCH — L byte-identity
  still covered in-proc).
- st_forward-ONLY HONEST (NOT live chat): the hook fires only inside st_forward;
  kv_step is untouched. ss6_live.h:24-27 AND the F4 doc row AND BACKLOG all state
  in plain text: "live cross-node forward" != "live chat is distributed"; R3's
  `ask` is still LOCAL (m_ask -> r_forward). NO overclaim.
- NOCENTRAL / GATE / no-VLA: owner = st_expert_owner (placement.c:113, HRW
  rendezvous-hash via the shared lookup primitive — no vote/leader/broadcast).
  Gate (ss6_live.c:248-257) fail-closed: sl_enabled AND j>=kmin AND
  degrade==FULL AND region_size()>=2 AND !st_expert_is_local. Off by default ->
  single-node standalone re-verified byte-unchanged (`00d6136e6d4676a3`,
  remote_experts=off). No VLA in ss6_live.c (all ST_D_MAX/SL_PENDING fixed); run_ss2
  [no-vla] still PASS. Wire scratch (sl_serve fin_aln/eo_aln, the requester rq) is
  file-static, off the stack — sound.
- TWO DEBUG FIXES — SOUND, not papered-over: (1) the env read moved to the init/net
  task (`env_uint("PKERNEL_REMOTE_EXPERTS")` in cmd_net, usermain) via
  ss6_live_set_enabled() instead of getenv() on the shell task — reads env where
  the ABI is safe, the documented shell-task getenv SIGILL hazard. (2) the responder
  model build + st_forward run on a dedicated 256KB task (`create_task(...262144)`),
  off the deep boot/init stack — the documented feedback_hosted_relay_stack_overflow
  class. Both are correct structural moves, not band-aids.
- BUILDS: all FOUR boots build at 019ae96d. boot/linux (aarch64 hosted) +
  boot/linux_x86_64 (x86_64 hosted) link ss6_live.o; boot/aarch64 + boot/x86
  (bare-metal) build CLEAN with ss6_live.o ABSENT (hosted-only in COMMON_C_SRCS,
  confirmed by `find` finding no ss6_live.o under the bare-metal dirs). ss6_live.c
  compiles with only the pre-existing project-wide NULL-redefine warning — NO new
  warnings. REGRESSIONS GREEN: run_ss1 6/6, run_ss2 8/8, run_ss3 6/6, run_ss5 8/8,
  run_ss6 7/7, run_kv 18/18, placement self-test PASS.
- HONEST GAPS (disclosed, no overclaim): single-host 4-process floor (no SSH
  2-machine); cert model is M-tier (L overflows the small shell-task stack — L
  byte-identity covered in-proc by run_ss6, honestly stated); kv_step/live-chat
  wiring is a named follow-up. The fallback=4-vs-3 doc nit is the only count
  discrepancy and is benign.
- LEDGER: the SS-6 [live] claim is TRUE, falsifiable, and independently broken-then-
  restored. **Row CLOSED.** No OPEN coverage gap introduced (the SSH 2-machine run
  and kv_step wiring are pre-disclosed future rows, not gaps in THIS claim).

## SS-6 cross-node expert firing — commit 4356e95f (parent 9e50e4a8)
- Auditor: independent subagent (this run), 2026-06-20
- Verdict: PASS-WITH-NITS (ledger row CLOSED; one hardening recommendation logged)
- Reproduced (exact commands, fresh worktree at 4356e95f, HEAD~1 == 9e50e4a8 verified):
  - `bash tests/llm/run_ss6.sh` -> 7/7 PASS. aarch64 (native host) hashes:
    S=0a5bf44c131b5439, M=63e8de333e995913, L=67f2434f50e791b6 (M/L match the
    commit's claimed hashes exactly). M fired 506 experts remote, L fired 940.
  - Cross-arch: `x86_64-linux-gnu-gcc -O1 -ffp-contract=off -static ... | qemu-x86_64`
    -> S/M/L EQ_HASH BYTE-IDENTICAL to aarch64 (single==remote==fallback, all MATCH).
  - Falsifier reproduced two ways: the cert's own mode-2 (1e-6 perturb on the
    remote-only output) -> DIFFER; plus an independent hand-injected 1e-7 into the
    canonical sum -> hash changed (e7c5fd... vs 63e8de...). The cert has teeth.
  - Single-node byte-UNCHANGED vs parent: a neutral forward-only driver compiled
    against BOTH 4356e95f and 9e50e4a8 student.c produced identical logit hashes
    for all 3 tiers (tier0 8c9fbe3d902fe619, tier1 0f28c83ee13e4133,
    tier2 a42d4734dd211823). The inline->two-pass reduction refactor is order-preserving.
  - Regressions: run_kv 18/18, run_ss1 6/6, run_ss2 PASS (no-vla), run_ss3 6/6,
    run_ss5 8/8 -- all PASS.
  - 4 builds (linux, linux_x86_64, x86, aarch64) exit 0, ZERO new warnings from
    student.c (only pre-existing NULL-redefined / write-unused-result noise).
  - Canonical order (CRITIQUE GATE #3): student.c:702-705 sums `moe[i] += wj*eo[i]`
    over ASCENDING slot j, no reversed/sorted loop; identical FP sequence to the
    pre-SS-6 inline `moe[i] += wj*acc` (eo[i] == the old acc, no extra rounding).
  - Crown invariants: SS-6 diff touches ONLY student.c/.h + docs + tests. grep of
    the diff for rw[]/gl_merge/r_backward/dtr_train = none (only doc prose). kv_step
    byte-identical to parent; eo_all is file-static [KMAX][DMAX], -Werror=vla clean.
    No vote/leader/quorum -- deterministic fixed-order canonical sum (NOCENTRAL).
- Key finding:
  - REACHABILITY: SS-6's remote hook is consumed ONLY inside st_forward
    (student.c:655-657). `st_set_remote_expert` has ZERO callers outside the test;
    kv_step / m_ask / r_forward never touch it. So SS-6 is a REAL capability in
    st_forward, but the LIVE chat generation path (kv_step incremental decode, and
    R3's ask->r_forward) does NOT reach it. Honest verdict: "a real capability in a
    code path the live chat does not currently call." The commit + docs state this
    plainly and do NOT claim SS-6 makes the live chat distributed -- no overclaim.
  - BACKWARD-AFTER-REMOTE CONTRACT: safe today, but UNENFORCED. st_backward
    (student.c:1326+) unconditionally reads c->e_g/e_u/e_h for every slot j; a
    remote slot leaves that cache stale. Safety rests entirely on the contract
    "training runs with the hook clear." No runtime assert. It is one refactor away
    from a silent gradient-corruption bug. RECOMMENDATION: add a cheap hard guard --
    st_backward early-fault if st_last_remote_fired() > 0 from the last forward (the
    counter already exists). Logged as a follow-up, not a blocker (no path installs
    the hook during training today).
- Honest bound carried (as the commit states, confirmed accurate):
  - The true multi-process cross-node forward over the relay/mesh is DEFERRED and
    correctly tagged `[live]` in special-structure-mind.md / BACKLOG.md.
  - The hook is on st_forward; wiring it into kv_step (chat-speed generation) is a
    documented follow-up. run_kv byte-identity is unchanged by SS-6.
  - The in-process cert drives the REAL canonical-sum + fallback code with a STUB
    peer (no network). S tier is proven INERT (E=2==K_min, no widening), not skipped.

## Federation R0 — commit 3df129d0 (audit: wave-fed-r0-audit)

Independent adversarial audit. BASE ok: HEAD==3df129d0, HEAD~1==eac5e7bd (trunk).
One commit. All 4 builds clean (linux, linux_x86_64, aarch64 bare, x86 bootloader);
no new warnings from dkva.c. Regressions green: region-super 8/8, region-teacher 6/6,
kdds-delcluster PASS, dkva g13-parallel/g13-arrival/self-test PASS. check_parity OK.

VERDICT: **PASS — this is a GENUINE [live] cert, not in-process-dressed-as-live.**

[live] REPRODUCED on REAL processes (3/3 runs, STABLE, byte-identical numbers):
  Counted 4 distinct ./boot/linux/p-kernel PIDs (24024/24025/24027/24029) + ./relay
  in one run; relay -v log shows each node registering a DISTINCT UDP source port
  (127.0.0.1:51625/36944/44627/53704) over wire v2 — real UDP, not a fake.
  zoned [locality] delta: far=6  near=3   (summary-only crossing + own-region fan-in)
  flat  [locality] delta: far=0  near=9   (no boundary -> direct fan-in to all N)
  (a) O(#region): zoned far 6 <= 1*K(6)=6 : PASS
  (b1) boundary: zoned far 6>0, flat far 0==0 : PASS
  (b2) FALSIFIER: zoned near 3 < flat near 9 : PASS
  (c) ONE-MATH: => class 0 identical zoned vs flat : PASS
  -> [fed-2cluster][live] PASS

FALSIFIER HONEST (not gamed) — verified by TWO independent break-the-hierarchy tests:
  * [live] I forced region_is_member()->TRUE (collapse all into one region). The
    zoned run then produced far=0/near=9 (== flat) and BOTH (b1) and (b2) went RED ->
    [fed-2cluster][live] FAIL. The gate has teeth against the degeneration it claims.
  * [in-proc] I degenerated dkva_expect_core() to wait per-NODE instead of
    per-REGION. rc_cnt0 became 2 and the "FALSIFIER O(#region)" line went RED ->
    [fed-2cluster] FAIL failures=1. Because dkva_infer (:558) and the self-test
    (:1191/:1269) share the SAME dkva_expect_core (:263), this falsifier genuinely
    guards the PRODUCTION expect-set (trap A2 satisfied, empirically).
  The (b2) near-comparison is a REAL measure: near = node1's same-region deliveries;
  a smaller region => smaller fan-out. Not a coincidental counter.

COVERAGE GAP (ledger OPEN, honest — does NOT make the cert gamed):
  A THIRD sabotage — flipping resp/<n> from REGION to GLOBAL — did NOT trip the
  [live] gate (numbers unchanged). Reason: the gate measures only node1 (the
  requester); resp-scope degeneration manifests on the RESPONDER nodes' TX, which
  node1 never sees. Neither arm exercises resp delivery-scope directly (the in-proc
  arm tests the expect-set, not scoped fanout). The rsum=GLOBAL/resp=REGION scope
  assignment IS statically correct (dkva.c:861-869) and verified, but it is NOT
  dynamically falsified. Recommend R0.1 add a responder-side or all-node locality
  gate (or measure coordB's far/near). FLAGGED, not blocking.

rsum scope GLOBAL: CONFIRMED. dkva.c:866-869 rsum=KDDS_SCOPE_GLOBAL, :861-864
  resp=KDDS_SCOPE_REGION, :856-859 q=KDDS_SCOPE_GLOBAL. (Note: the commit message's
  "dkva.c:809-818" citation is slightly off — scopes are at :853-869; cosmetic.)

expect-core shared (trap A2): CONFIRMED + empirically proven (sabotage above).
coord-crash HONEST TAG: [in-proc] only; [live] explicitly deferred to R0.1; no
  false live-kill claim in commit/docs/plan.
NOCENTRAL/no-VLA/one-math: nm tripwire clean (no vote/elect symbol; only
  supernode_select/teacher_select). All cert arrays [DNODE_MAX] fixed, no VLA.
  one-math: => class identical zoned vs flat (live) + summary==dense fold (in-proc).

GAPS DISCLOSED / NO OVERCLAIM: single-host (no SSH 2-machine); in-proc uses
  synthetic seeded membership (does not exercise egocentric-region disagreement);
  10k-dream gap (256 node_id wall / MTU / array fan-out) stays WIDE OPEN. federation.md
  §1/§5 corrected 32->64, "未実装"->"R0 LIVE+certified; F1-F3 design-only", title still
  "橋を架ける" (bridge under construction). No doc reads "federation works" / "10k works".
  BACKLOG/commit say "8 OS processes" = 4+4 across the two sequential runs (a single
  run is 4 p-kernel + 1 relay = 5); loose phrasing, not false.

LEDGER: the [fed-2cluster][in-proc] + [live] + [coord-crash][in-proc] claim is TRUE
  and falsifiable. Row CLOSED. One coverage gap (resp-scope not dynamically
  falsified) ledgered OPEN for R0.1.

## MC-0 deterministic parallel matmul — commit 1a8f2e79

Independent adversarial audit (auditor != implementer). Base verified:
HEAD=1a8f2e79 (MC-0), HEAD~1=03481a52 (parent) — matches the mandate.

VERDICT: **PASS — ledger CLOSE.** No race, no byte mismatch, no busy-spin, no
regression found. The "one mind, one math" byte-identity crown holds under
hostile testing.

[par-matmul-equiv] BYTE-IDENTITY — held for EVERY worker count I tried:
  - Official cert run_mc0.sh PASS: memcmp==0 + FNV hash equal for NW {1,2,4,8}
    on out {1536, 49152} and the ragged shapes out=1530, out=1031 (neither /4
    nor /8). Reproduced 3x identically.
  - HAMMER (own harness, full qz_matmul_q8_0): NW in {1,2,3,5,7,8,16} INCLUDING
    odd / non-dividing / over-cap, shapes {65,128,1536,1530,1031,2048},
    2000 iterations each = 84,000 dispatches -> TOTAL MISMATCHES = 0.
  - RACE-STRESS (timing-skewed body to force gen-N/gen-N+1 overlap): 300,000
    dispatches, NW {1..8,16}, shapes {64,65,100,127,257,1000,1536,2048,4096}
    -> 0 fails. 4 concurrent processes hammering -> all byte-identical.
  ZERO mismatches at any NW, any partition, any iteration.

REALLY PARALLEL (not secretly serial): PROVEN. A per-thread TID probe shows
  4 DISTINCT worker threads each ran a slice for NW=4 on out=2000. sysconf(
  _SC_NPROCESSORS_ONLN)=8 here (nproc cmd shows 4 due to cgroup, but the pool
  sees 8) -> nhelpers=7, so the cert's NW=8 is a real 8-way dispatch, NOT
  clamped. Cert shapes (>=1031) all exceed PK_PARALLEL_MIN_ROWS=64, so they
  genuinely go parallel; byte-identity is NOT vacuous. Coverage probe: every
  output index written exactly once for NW 1..16 (no uncovered, no double-write).

FALSIFIER HAS TEETH (reproduced its FAIL): the reassociating (strided
  partial-sum) variant DIFFERS from serial in ~93-95% of rows for EVERY
  NW in {2..8} (e.g. NW=2: 1456/1536 rows differ). Independently reimplemented
  and confirmed memcmp != 0 -> the equiv cert can SEE a rounding-order bug.
  Could NOT make the falsifier match serial. Byte-identity is a real, killable
  property.

RACE VERDICT: **No race found. Could NOT induce a hang or non-deterministic
  garbage.**
  - tsan: UNAVAILABLE in this PRoot/proot-loader env ("ThreadSanitizer: memory
    layout is incompatible" — environmental, harness builds fine; reported
    honestly, not a code defect).
  - Stress: 84k + 300k + concurrent dispatches, 0 mismatches. Pure rapid-
    dispatch loop made steady progress (~2900 dispatches/s under PRoot) and
    NEVER stalled — apparent "timeouts" at 900k/5M were throughput, not
    deadlock (verified: 90k completes in <60s, 300k in 65s, fast2 prints
    monotonic progress 0..44000). No missed wakeup, no hang.
  - By-inspection (pk_parallel.c): the join-counter fix is SOUND. The helper
    holds g_pool.mtx CONTINUOUSLY from `++g_pool.done` (line 98) through the
    re-entry to `while(g_pool.gen==seen)` (line 75) — it does not unlock
    between increment and wait. Therefore the dispatcher cannot reset done=0 /
    bump gen until that helper is already blocked in pthread_cond_wait with
    seen==current gen; on the next dispatch gen!=seen and the helper runs
    exactly once. EVERY helper (even slot>=nw, which does no work, line 91)
    still does `++done` (line 98), so the join counter == nhelpers is
    unambiguous regardless of nw. Disjoint output ranges (pk_slice) => no
    shared accumulator, no false-sharing of any y[i]. pthread_once guards lazy
    init. g_force_nw is a cert-only hook touched between (never during)
    serialized dispatches; single caller forward.c:164 on the uniprocessor
    kernel -> no re-entrant or concurrent pool use.

[mc0-idle] NO BUSY-SPIN: reproduced 3x — wake-delta=0 and 0.0000s process CPU
  across the 300ms idle gap; wakes advance only on real dispatch. Helpers block
  in pthread_cond_wait. wave-idle-yield contract respected.

SCHEDULER UNTOUCHED: pk_parallel.{c,h} reference knl_ctxtsk/knl_schedtsk only
  in a comment; never call the dispatcher. Math-only pool, T-Kernel stays
  single-threaded.

BARE-METAL = INLINE, ZERO PTHREADS: nm on boot/x86/kloader.bin and
  boot/aarch64/kernel.elf shows NO pthread / pk_parallel / qz_matmul symbols
  (student_stub path, serial). Hosted boot/linux/p-kernel DOES carry
  pk_parallel_rows + pthread_cond_wait. ALL 4 BUILDS GREEN (linux, linux_x86_64,
  x86 bare, aarch64 bare).

NO REGRESSION: NW=1 qz_matmul_q8_0 is BYTE-IDENTICAL to the parent (03481a52)
  serial loop (verified vs a verbatim copy of the old code, out {1536,1530,1031,
  49152}). run_qmatmul PASS (q8_0 max abs err 0.000e+00 vs independent ref;
  python oracle PASS). run_ss6 PASS (all S/M/L logit-hash MATCH). ss6_live.c is
  NOT in commit 1a8f2e79 -> any check_parity drift there is PRE-EXISTING, not
  introduced by MC-0.

REAL BUGS FOUND: none.

LEDGER: the [par-matmul-equiv] + [par-matmul-falsifier] + [mc0-idle] claim is
  TRUE and falsifiable. The byte-identity crown is intact across all NW. Row
  CLOSED. (tsan-under-PRoot remains the one tool I could not run; covered by
  high-iteration stress + structural proof.)

## idle-yield (dispatcher idle busy-spin → sigsuspend) — commit 6eaa1407
- Auditor: independent adversarial subagent (this run), 2026-06-20. Did NOT write
  the code. Base verified: HEAD==6eaa1407, HEAD~1==7df9ba96 (the claimed parent).
- Verdict: **PASS (ledger row CLOSED).** The dispatcher idle busy-spin is really
  gone, the fix is confined to arch/linux/, and I could NOT wedge the scheduler.

MISSED-WAKEUP / HANG (the #1 risk) — FALSIFICATION FAILED (good):
- I could NOT construct a wedge. Adversarial test: 15 rounds of `tcbchurn 30 3` +
  `moe` interleaved with sub-tick (50ms) idle gaps to hammer the race window, then
  a 6s FULL idle, then a final `dtr eval` + echo probe. The kernel woke and answered
  EVERY time (dtr-eval response present, FINAL_MARKER echoed). No hang, no fault,
  "sched alive" on every churn round.
- TOCTOU analysis (preempt.c aarch64:142 / x86_64:105): the re-check
  `if (knl_schedtsk == 0)` runs AFTER `sigprocmask(SIG_BLOCK, SIGALRM)` and BEFORE
  `sigsuspend()`. knl_schedtsk can be set only from the SIGALRM handler, which is
  blocked across the re-check → sigsuspend window. If the tick fires in that window
  it stays PENDING and sigsuspend returns at once. The re-check is genuinely inside
  the blocked window — TOCTOU-free. Confirmed correct.
- Bounded latency = 1 tick CONFIRMED. .Lidle does IRQ_FLAG_CLEAR (arch_irq_disabled_flag=0)
  before the wait, so while idle every SIGALRM runs knl_timer_handler_startup DIRECTLY
  (handler's `if (arch_irq_disabled_flag) defer` branch is not taken) — no new ticks
  are deferred while sleeping, and a tk_dly_tsk timeout is serviced by the very next
  tick (≤10ms @100Hz). Empirically: idle-entry rate is EXACTLY one wake per tick (see
  below) — if any wakeup were ever missed the rate would not be a clean 100/s.
  No non-signal off-tick wakeup path exists (idle ⇒ nothing runnable; only the timer
  handler sets schedtsk).

[idle-cpu] HEADLINE REPRODUCED (instrumented idle-entry counter, 5s window):
- aarch64 (native):  BEFORE 5976/s  →  AFTER 100/s   (≈60×; matches commit's ~6000→~101)
- x86_64 (qemu):     BEFORE 655800/s →  AFTER 100/s  (qemu sched_yield spins tighter;
  AFTER is exactly one wake per 100Hz tick = sleeping, not spinning).
- Process CPU (aarch64, FIFO-held idle): 5.7% → 0.5%. The busy-spin is GONE on both arches.
  The fix is NOT fake.

SIGNAL-MASK CORRECTNESS — CONFIRMED:
- sigsuspend wakes on SIGALRM (the 100/s steady wake rate proves it). /proc SigBlk
  sampled while idle == 0 on BOTH before & after (no permanent mask leak; prev is
  restored by sigprocmask(SIG_SETMASK,&prev)). SigCgt=0x24c0 = handlers for
  SIGBUS/SIGFPE/SIGSEGV/SIGALRM all installed and deliverable.
- Fault deliverable from idle: `dtr crash` from the idle build → SIGSEGV(11) caught,
  task isolated/killed, recover_fn ran, worker respawned, kernel kept running.
- SIGINT/SIGQUIT are process-IGNORED (SigIgn=0x06) — the kernel takes Ctrl-C as raw
  terminal INPUT, not a kill. This is PRE-EXISTING and IDENTICAL before & after (I
  verified the 7df9ba96 build behaves the same). The patch does NOT block term/fault
  signals; behavior is unchanged, exactly as the commit claims.

IRQ BRACKETING — INTACT: the only change inside the IRQ_FLAG_CLEAR / IRQ_DISABLE
  bracket is `bl/call sched_yield` → `knl_idle_wait`. FLAG_CLEAR (no drain) keeps
  arch_irq_disabled_flag=0 during the wait so the tick runs directly; IRQ_DISABLE
  restores the dispatcher's IRQ-off invariant before re-reading schedtsk. Nothing
  clobbers knl_schedtsk/knl_ctxtsk/knl_dispatch_disabled.

FOUNDATION (all run on the AFTER build):
- poc_preempt: PASS (both tasks progressed; preemption intact).
- tcbchurn 200 4: PASS (800 create/kill, "no UAF crash, sched alive"). KILL-CHURN clean.
- run_kv.sh: 18 PASS / 0 FAIL ([result] PASS; byte-identical S/M/L tiers + 5.14× speedup).
- moe test: ALL PASS; dmn test: PASS (forgetting + consolidation); dtr eval: clean.
- Cadence unstretched: dtr train 50 epochs = 140ms (after) vs 150ms (before) — within
  noise. The idle change does not touch the runnable-task path, so compute cadence is
  unaffected.

SCOPE / BARE-METAL: patch touches ONLY arch/linux/{aarch64,x86_64}/{cpu_support.S,
  preempt.c} + new tests/idle_cpu.sh. `git show 6eaa1407 -- arch/x86 arch/aarch64`
  is EMPTY — bare-metal hlt/wfi idle untouched. Both arches get the fix identically
  in spirit (same C helper, same bracketing).

BUILDS: all 4 green — linux (aarch64), linux_x86_64, bare x86 (kloader.bin), bare
  aarch64 (kernel.elf).

LEDGER: row CLOSED. No missed-wakeup path found; worst-case latency provably bounded
  to one tick. No real bug found.
