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

## MC-1 size-gated parallel teacher matmul — commit 0481b5ed

VERDICT: PASS. Independent adversarial reproduction. BASE ok (HEAD 0481b5ed,
HEAD~1 == efec4748). The one-mind byte-identity crown HOLDS; speedup is real
(not gamed); the gate is correct in both directions; no regression; salvage
complete (no TODO/stub/half-applied hunk).

BYTE-IDENTITY (the crown) — GGUF-REAL, not synthetic:
- /tmp/smollm2-135m.gguf (144MB, real SmolLM2-135M) present -> the FULL teacher
  forward equivalence is REAL. Full-forward FNV hash = 0xfed93d680c1dcb55,
  argmax 3807, IDENTICAL across NW{1,2,4,8} (matches the claimed 0xfed93d68...).
- HAMMERED: odd/non-dividing NW{3,5,7} and over-cap NW{16,32} via
  PKERNEL_MATMUL_THREADS -> all 0xfed93d680c1dcb55 (over-cap clamps to host
  cores in pk_effective_nw, partition still deterministic).
- LOOPED: synthetic F32+Q4_0 equiv 35x + full forward 30x at NW=4 -> ZERO
  mismatches. No race-induced nondeterminism.
- NW=1 (and NW=4) byte-identical to the PARENT serial efec4748 full forward
  (parent hash also 0xfed93d680c1dcb55). The serial math is bit-unchanged.
- Synthetic F32 + Q4_0 memcmp==EQUAL across NW incl ragged (out=1530, 1031).

REALLY PARALLEL (not secretly serial): instrumented gettid in the body during a
teacher-head dispatch at NW=4 -> 4 DISTINCT TIDs ran the body. Real multi-thread
dispatch confirmed.

SPEEDUP (reproduced, host 4 cores, -O1 -ffp-contract=off):
- teacher head 28.3M-MAC matmul: NW1 45.07ms -> NW2 2.02x / NW4 2.67x / NW8 2.66x
  (claim NW4 ~2.22x reproduced and exceeded; honest sublinear plateau at NW8).
- FULL teacher forward (real GGUF): NW1 ~215-245ms -> NW4 ~157-177ms (~1.4x);
  NW2 ~= NW1 (wash). The 2.22x headline is correctly scoped to the isolated head
  matmul in the commit msg, NOT the whole forward — honest labeling.
- teacher ffn (884736 MACs): NW2 LOSES (0.88x) but NW4 WINS (1.48x) at in=576.

GATE (both directions correct):
- default threshold 524288. Student M-tier (16384), L-tier (131072), R3 48x48
  (2304) all route SERIAL (pk_parallel_last_was_parallel()==0). Teacher ffn
  (884736) + head (28.3M) route PARALLEL. PASS.
- Crossover honesty: at the synthetic in=256 sweep, NW=4 first wins at 2^20
  (1048576) — ABOVE the shipped 524288 gate. BUT at the REAL teacher contraction
  in=576, NW=4 first wins at ~589824 (out=1024), just above 524288. The teacher
  matmuls (in=576) genuinely win at the production NW=4. The gate is defensible
  for the in=576 regime the teacher actually uses; the header's "2^19 is the first
  pow2 where NW=4 reliably wins" is true for in=576, a layout artifact for in=256.
  Not a crown issue (byte-identity holds regardless of gate decision).

F32 FALSIFIER HAS TEETH: injected a reverse-order (reassociated) contraction into
the F32 body -> hash 0x6b2d33c13989a949 != serial 0xf3c9ecd398dc1382, memcmp
DIFFER. The cert's byte-identity is non-vacuous on the F32 path (not just Q8).
Verified by-construction: f32_mm_body (forward.c:174) + qz_q4_body (quant.c:118) +
qz_q8_body partition by OUTPUT row only; inner acc+= left-fold is the unmodified
serial order; contraction never split.

CROWN UNTOUCHED: git show 0481b5ed | grep rw[]/gl_merge/r_backward/dtr_train/dmn_
-> NONE.

NO REGRESSION: run_mc0 PASS, run_qmatmul PASS (oracle max abs 2.46e-05),
run_kv 18/18 PASS, run_ss6 7/7 PASS. run_mc1.sh PASS end-to-end (real GGUF).

IDLE: pool wake-count delta 0 across a 300ms gap, process CPU 0.0000s -> no
busy-spin regression (reproduced twice).

BARE-METAL + 4 BUILDS + PARITY: pk_parallel.c/quant.c/forward.c stay hosted-tier.
Bare-metal x86 + aarch64 do NOT compile the LLM tier (student_stub.o weak
fallbacks); kernel.elf (x86) + kernel.elf (aarch64) link clean with ZERO
pk_parallel/pthread symbols (nm). Hosted linux_x86_64 p-kernel DOES export
pk_parallel_rows_gated/min_macs. All 4 builds green. check_parity OK.

NOTE (cosmetic, not a bug): pk_parallel.h header comment claims an "inline
fallback below" runs body() serially when PK_PARALLEL_HAVE_POOL is undefined, but
there is no such inline fallback in the header. Harmless because bare-metal never
references these symbols (LLM tier not compiled). Stale doc, not a defect.

LEDGER: row CLOSED. The one mind stays one — byte-identical across every worker
count including odd/over-cap/looped, on the REAL teacher forward. No real bug found.

## MC-2.0 bare-metal secondary-core bringup — commit b5bacd89 (impl 488d4232)
- Auditor: independent adversarial WORKFLOW (5 parallel dimensions, each in its
  own isolated worktree, + a per-finding adversarial-verify stage), 2026-06-20.
  Did NOT write the code (the implementer crashed; a commander smoke-verified +
  salvaged; this row is the SEPARATE trace). The boot/reset path is the repo's
  highest-risk class — every dimension reproduced from a fresh build, not trust.
- Verdict: **MERGEABLE (ledger row CLOSED).** All 5 dimension gates PASS;
  confirmed_material_findings = [] ; blockers = []. MC-2.0 correctly claims ONLY
  [mc2-boot-survives] (a parked core wakes + the kernel survives), never
  byte-identity (that is MC-2.1).
- [cert-teeth] The [mc2-boot-survives] cert is FALSIFIABLE, not paper — proven to
  turn red FOUR independent ways under -smp 4, primary surviving every time:
  - corrupt the worker tile -> "MC2-BOOT: FAIL tile-mismatch@0"
  - stub the PSCI CPU_ON release -> "MC2-BOOT: FAIL join-timeout (primary survives)"
  - built-in -DMC2_FAULTING_TILE -> "MC2-BOOT: FAIL join-timeout (primary survives)"
  - worker wakes+done but writes no tile -> "MC2-BOOT: FAIL tile-mismatch@0"
  In ALL four, "[BOOT] Starting T-Kernel..." + "[T-Kernel] Initial task started"
  still printed (the primary NEVER wedged). The cert asserts REAL work via a
  salted index fn (0x5A5A0000) so an untouched all-zero BSS buffer FAILS; the
  primary's reads of done/woken/tile sit behind real `dmb ld` acquire barriers
  (verified in disassembly), paired with the worker's `dmb st` releases and
  SMPEN=1 on both cores (s3_1_c15_c2_1 bit 6, confirmed in objdump).
- [safety-boundary] The HARD BOUNDARY holds: `git diff 280c9887..488d4232 --
  arch/aarch64/cpu_support.S` is EMPTY (dispatcher .Ldispatch_loop/.Lidle
  byte-for-byte untouched). The secondary path references knl_ctxtsk/knl_schedtsk
  ONLY in a doc comment (mc2_smp.c:23), never in code. The primary boot path
  (_from_el2/_el1_entry) is byte-for-byte identical to trunk; the secondary
  re-derives the SAME EL1 bits in a parallel path and additionally sets SMPEN.
  PSCI CPU_ON is issued in main() after core init and is fully gated behind
  #ifdef MC2_SMP_SELFTEST — a plain build wakes no secondary.
- [smp-correctness] SMPEN bit 6 set on BOTH cores before any shared access;
  the publish/handoff chain is textbook message-passing: primary `dsb ish` before
  CPU_ON, worker `dmb st` (release) after the tile, primary `dmb ld` (acquire)
  before reading g_done and again before reading output. PSCI uses fid 0xC4000003
  (SMC64) over hvc, hands off entry PA + context_id=&g_cpu[1] + own stack; the
  secondary lands at EL1h with its own SP. The _Static_assert offsets (0/8/16/24)
  match the start.S [x19,#0] stack load. Worker parks on `wfe` (no busy-spin); the
  join has a hard bounded fallback (MAX_TRIES=200000000) so a dead worker can't
  wedge the primary.
- [no-regression] Plain build (no flag, no -smp) boots to "[T-Kernel] Initial
  task started" + the p-kernel> shell with ZERO MC2/secondary output (selftest
  correctly gated). Selftest flag on a 1-CPU VM fails GRACEFULLY ("MC2-BOOT: FAIL
  cpu_on rc=-2") rather than falsely passing, and still boots. check_parity OK.
- [diff-discipline] Touches ONLY arch/aarch64, boot/aarch64, tests/aarch64 (6
  files, 620 insertions). No new compiler warnings (base 280c9887 = 206 warning
  lines; mc2 selftest = 206; comm diff EMPTY). Linker stacks: _stack_top_cpu1
  16KB contiguous after the 64KB primary stack, no overlap, below _kernel_end.
- HONEST BOUND (disclosed by the design doc itself, §4.1/§8.2, NOT hidden): QEMU
  TCG models memory strongly and may MASK a missing-barrier / SMPEN=0 race. So the
  QEMU PASS proves the bringup MECHANISM + the partition/FP determinism, but the
  barrier discipline's TEETH are only fully load-bearing [live] on real RPi3
  hardware — correctly DEFERRED to MC-2.2. A green QEMU run is NOT "barriers
  verified". This bound is a property of the emulator, not a defect in MC-2.0.
LEDGER: row CLOSED. A parked aarch64 secondary wakes via PSCI CPU_ON, runs a
deterministic tile, and the kernel survives it — the literal first step of ②
full SMP, with the dispatcher untouched. No real bug found.

## MC-2.1a N-core byte-identity matmul — commit bba18e19 (impl b7d1f4f0)
- Auditor: independent adversarial WORKFLOW (5 parallel dimensions, each in its
  own isolated worktree, + per-finding adversarial-verify), 2026-06-20. Did NOT
  write the code. This is the CROWN determinism cert: "one mind, one math" across
  PHYSICAL cores. Every dimension reproduced from a fresh build, not trust.
- Verdict: **MERGEABLE (ledger row CLOSED).** All 5 dimension gates PASS;
  confirmed_material_findings = [] ; blockers = [].
- [equiv-is-real-NOT-vacuous] The deadliest failure mode (a "byte-identical" cert
  that secretly ran SERIALLY on one core, proving nothing) is REFUTED. Per-slot
  row-execution instrumentation proves PHYSICAL cores 2 and 3 computed DISTINCT,
  non-overlapping, correct ranges of the nw=4 matmul ([1024,1536) and [1536,2048))
  — not one core doing everything. The three FNV hashes are byte-identical
  (0xd4c96f986cbafb17 for nw=1,2,4). Byte-identity attacked FOUR ways: nw=3
  odd/ragged stayed identical; a different seed kept identity but produced a
  GENUINELY different hash (data-dependence, not a constant); corrupting ONE row
  (1100) correctly produced MC2-EQUIV: FAIL @1100 (the hash observes real output);
  Tooth A (reduction reassociation) correctly FAILs. The §1.6 MPIDR-derived-slot
  fix is verified — each core ran its OWN slice, no slot-1 stomping.
- [the-two-bugfixes] BOTH implementer-self-reported fixes VERIFIED real + correct.
  JOIN-ON-NHELPERS: the auditor EMPIRICALLY reproduced the original hazard —
  reverting the join to `done>=nw-1` made the cert FLAKE 4/6 runs on QEMU itself
  (mismatches in slot-1's range, reading the poison -987654.0 before slot 1 wrote;
  a SCHEDULING race a do-nothing slot 2/3 wins, which TCG DOES expose, unlike
  Tooth B). The fix (join on all released secondaries) ran 6/6 PASS, introduces NO
  hang (every secondary ++done unconditionally each gen, gated by wait_woken +
  the MAX_TRIES=2e8 watchdog; slot>=nw workers still execute the dmb-ld/dmb-st
  around an empty body so the primary's post-join acquire can't read stale stores).
  DOUBLE-SELFTEST: the combined -DMC2_SMP_SELFTEST -DMC2_EQUIV_SELFTEST build was
  confirmed to print "MC2-EQUIV: FAIL secondary-not-woken"; the two self-tests are
  now disjoint (equiv = EQUIV flag only, boot-survives = SMP flag only), each
  independently green.
- [barriers-and-boundary] start.S AND cpu_support.S diffs EMPTY (reused unchanged).
  Every §3.3 barrier present + correctly ordered in the DISASSEMBLY: dispatch
  done=0 -> `dsb ish` (before gen++) -> gen++ -> `stlr` unlock -> sev -> slice-0 ->
  `dmb st` -> bounded join -> `dmb ld` acquire; worker wfe-on-gen -> `dmb ld` ->
  compute -> `dmb st` -> ++done under the ldaxr/stlr lock -> sev. SMPEN bit6 set on
  every core. Stacks (nm-verified) contiguous 16KB, zero overlap, below
  _kernel_end. pk_slice_bm byte-identical to the hosted golden (pk_parallel.c
  :57-63). No knl_ctxtsk/knl_schedtsk reference in the worker path.
- [no-regression-scope] Single-core boots clean, ZERO MC2 output (the 4.2MB
  synthetic-W is gated — default BSS 14.68MB vs equiv 18.89MB). [mc2-boot-survives]
  still green. dtr.c/moe.c/r3_incontext.c/pk_parallel.c UNTOUCHED. check_parity OK.
- [honesty] Tooth B (barrier/SMPEN teeth, obligation O4) is recorded as
  PASS-because-masked-on-QEMU, deferred to RPi3 MC-2.2 — the harness prints "does
  NOT mean 'barriers verified'". No speedup is claimed (the cert matmul is
  synthetic; dt_linear NOT wired to the seam — the real R3 48x48 is below the gate).
  The pk_slice_bm hand-copy drift surface is guarded by self-consistency; a
  standalone partition unit-check is a named MC-2.1b follow-up, NOT silently dropped.
LEDGER: row CLOSED. Across 1/2/4 physical cores the parallel matmul is BYTE-
IDENTICAL to serial (0xd4c96f986cbafb17) — the mind stays one across cores. The
parallelism is PROVEN real (cores 2,3 ran distinct ranges), the cert has teeth
(Tooth A bites; 4 attacks caught), and the only un-certified obligation (the
barrier teeth) is honestly deferred to RPi3. No real bug survived (two were found
+ fixed during impl, both independently re-verified here).

## MC-2.1b pk_slice_bm partition unit-check — commit 0ab9e548 (impl 44363611)
- Auditor: independent focused auditor (single agent, proportionate to the small
  test-only scope), 2026-06-20. Did NOT write the code. The named MC-2.1a
  follow-up: a STANDALONE pure-integer drift guard on pk_slice_bm (the hand-copied
  partition fn that keeps the bare-metal multicore matmul byte-identical to the
  hosted golden — "one mind, one math"), guarding it DIRECTLY where the equiv cert
  only exercised it via the matmul.
- Verdict: **MERGEABLE (ledger row CLOSED).** PASS, no findings above minor.
- REPRODUCED: `-DMC2_SLICE_SELFTEST` -> "MC2-SLICE: PASS" over 31 (out,nw) cases
  (DISJOINT+TOTAL, ORDER, MATCHES-GOLDEN); falsifier `-DMC2_SLICE_BREAK` ->
  "MC2-SLICE: FAIL out=7 nw=2 @1 reason=order" (teeth).
- THE LOAD-BEARING CHECK (MATCHES-GOLDEN is NOT circular): the auditor confirmed
  the inline slice_golden (mc2_smp.c:927-933) is a verbatim copy of the hosted
  pk_slice formula (pk_parallel.c:57-63), does NOT call pk_slice_bm, and has no
  MC2_SLICE_BREAK guard — and PROVED the golden branch isn't dead code by authoring
  its OWN independent probe (MC2_SLICE_GOLDEN_ONLY: contiguous+total but diverging
  from floor-division) -> "MC2-SLICE: FAIL @0 reason=golden-mismatch". So the
  golden check catches drift the order/coverage checks miss; it is independent and
  bites.
- NO REGRESSION: default build .text BYTE-IDENTICAL to base 457e959d (md5
  7fd674967ee622c1411829b65d13260b both; sizes 419442/1304/14676716). Plain boot:
  0 MC2 output, reaches the T-Kernel banner + shell. [mc2-smp-equiv] still PASS (3
  identical FNV 0xd4c96f986cbafb17). [mc2-boot-survives] ALL PASS. cpu_support.S +
  start.S diffs EMPTY. check_parity OK. No committed binaries.
- COVERAGE HONESTY: the nw>out skip is justified (traced both dispatch gates —
  out<64 falls back to serial, nw capped 8/4 — so nw>out can NEVER reach
  pk_slice_bm in production). A documented mirror of an enforced contract, not a gap.
LEDGER: row CLOSED. The partition that keeps the mind one across cores is now
drift-guarded DIRECTLY (not only via the matmul), with a golden reference proven
independent. The ③ multicore arc (MC-0 → MC-1 → MC-2.0 → MC-2.1 → MC-2.1b) is
complete and audited; the ② full-SMP bringup foundation is laid.

## ②.0 full-SMP slice (2-CPU Big Kernel Lock) — commit 78c5d222 (impl 97ab61ed)
- Auditor: independent adversarial WORKFLOW (5 parallel dimensions, each in its own
  isolated worktree, + per-finding adversarial-verify), 2026-06-21. Did NOT write
  the code. The FIRST real step of ② full SMP — the repo's HIGHEST-RISK change.
  (NOTE: a first audit run died entirely on a session-limit with ZERO tool uses —
  its "MERGEABLE" was a phantom; this is the re-run that actually executed: 205 tool
  uses, ~407k tokens, 45 reproduced boots.)
- Verdict: **MERGEABLE (ledger row CLOSED).** All 5 gates PASS; blockers=[];
  confirmed_material_findings=[]. The only finding is minor harness fragility (below).
- [smp-is-real-NOT-vacuous] The deadliest failure (certs PASS but only ONE core ran)
  is REFUTED: MPIDR instrumentation shows task0 on core 0x80000000 (Aff0 0) and task1
  on 0x80000001 (Aff0 1) — two DISTINCT physical cores. The -DSMP_MUTEX_NOLOCK
  falsifier loses updates NON-deterministically (203170/208919/212138/232530/213859
  across runs = a real race, not a fixed delta — and if only one core ran, removing
  the lock could not lose updates). Control: -smp 1 makes ALL certs FAIL (a sentinel),
  proving 2 real cores are needed. Locked counter = exactly 400000 across 5 runs.
- [bkl-correctness] The BKL raw spinlock (smp.c:193-212) is byte-identical to the
  already-audited mc2_lock (ldaxr-acquire + stxr-retry + stlr-release + sevl/wfe);
  the recursive owner+depth layer is correct; EVERY multi-writer mutation (ready
  push/pull+claim, per-CPU sched state, the shared + barrier counters) is under the
  BKL; the only outside-lock writes are single-writer-per-location with dmb. The
  unbounded-skip deadlock is genuinely fixed (smp_ready_pull:281-295 bounded, atomic
  claim, NULL when drained); drained CPUs wfe WITHOUT holding the BKL. 45/45 clean
  snapshot-based runs reached a verdict with ZERO hangs. KEY: an intermittent
  SMP-MUTEX FAIL the auditor first saw was proven (by exact timestamp correlation) to
  be SELF-INFLICTED — the auditor's own parallel NOLOCK `make` overwrote the running
  kernel.elf — NOT a BKL bug.
- [shipped-kernel-byte-identical] The crown constraint HOLDS: the DEFAULT build (no
  SMP flag) is byte-identical to base 457e959d across every loadable section AND the
  full loadable binary image (421496 B, cmp-identical, PHDRS identical); the ONLY
  delta is +24 B of non-loadable .symtab (one entry from the always-compiled-but-fully
  -gated smp.o). task.c is UNCHANGED (empty diff). cpu_support.S/start.S edits are
  append-only EOF hunks that do NOT touch .Ldispatch_loop / _secondary_worker. MC-2
  bringup intact (mc2-test ALL PASS, mc2-equiv byte-identity PASS).
- [one-mind-untouched] The diff touches exactly 6 files (arch/aarch64/{cpu_support.S,
  smp.c,start.S}, boot/aarch64/{Makefile,main.c}, tests/aarch64/run_smp0.sh) — NONE a
  mind TU. The self-test workload is a bare shared-counter loop (no fn-ptr, no mind
  state); the secondary's reachable call graph is closed and contains ZERO calls into
  the mind/merge path. ②.0 is pure kernel scheduling.
- [honest-scope-and-decoupling] Scope honestly represented: ②.0 runs its OWN per-CPU
  dispatcher over self-test tasks (NOT the production knl_ctxtsk — that conversion is
  the next lift); nothing overclaims "the T-Kernel is now SMP". The decoupling is
  clean — smp.c's own copies of psci_cpu_on/mc2_set_smpen/mpidr_aff0 are byte-identical
  to mc2_smp.c's originals (documented maintenance debt: merge g_cpu[]/g_smpcpu[] later).
- MINOR FINDING (follow-up, non-blocking): run_smp0.sh rebuilds kernel.elf in the same
  dir it boots from, so a CONCURRENT build silently corrupts the run (the source of the
  auditor's self-inflicted flake). Fix = snapshot the elf before boot (a ②.1/harness
  follow-up). Not a code defect.
- HONEST BOUND: ②.0 proves the SMP MECHANISM (BKL + per-CPU dispatch + true 2-CPU
  concurrency + mutual exclusion). It does NOT convert the production scheduler, and
  the barrier/race teeth are only fully [live] on RPi3 (QEMU TCG masks weak-ordering).
  IPIs/cross-CPU preempt = ②.1; the [smp-one-mind] crown cert = ②.2; finer locks = ②.3.
LEDGER: row CLOSED. Two physical aarch64 cores run the T-Kernel dispatcher under one
Big Kernel Lock, provably concurrent (distinct MPIDRs, falsifier loses ~half the
updates), with the shipped uniprocessor kernel byte-identical and the mind untouched.
The first real step of full SMP is on trunk.

## ②.1a cross-CPU preemption via GIC SGI IPIs — commit c2bba938 (impl 30cca55b)
- Auditor: independent adversarial WORKFLOW (5 parallel dimensions, isolated
  worktrees, + per-finding adversarial-verify), 2026-06-21. Did NOT write the code.
  p-kernel's FIRST inter-processor interrupt — the GIC/IRQ-vector path is the repo's
  recurring aarch64 C-ABI trap.
- Verdict: **MERGEABLE (ledger row CLOSED).** All 5 gates PASS; blockers=[];
  confirmed_material_findings=[]. One NIT only (a stale BOARD_RPI3 comment).
- [preempt-is-real-AND-sgi-load-bearing] The cross-CPU preempt is REAL and the SGI
  is genuinely LOAD-BEARING, not cosmetic. SMP-PREEMPT: PASS reproduced 5x
  (sgi_taken=1, ran_high-prio=1, ctxtsk==highprio_taskptr); preempted_at JITTERS ~20x
  (51384/82349/349106/930346/46071 = real async, not a deterministic fake). THE KEY
  PROOF: B's ONLY switch path (smp_ready_pull) is gated solely on g_resched_pending
  (smp.c:665), which is set ONLY by the SGI handler (smp.c:247, grep-confirmed sole
  writer) — there is NO independent ready-list poll, so the SGI is the SOLE trigger;
  the -DSMP_NO_IPI falsifier (B never switches, sgi_taken=0, FAIL rc=-20, no crash)
  confirms it. g_sgi_taken is incremented ONLY in smp_resched_sgi_handler (smp.c:246),
  reached ONLY via knl_intvec[0] <- the production _vec_el1_irq blr (cpu_support.S:
  264-269) -> sgi_taken=1 is UNFORGEABLE proof a real SGI traversed the production
  IRQ vector.
- [gic-irq-correctness] The GIC SGI path is correctly plumbed; the aarch64 IRQ-vector
  C-ABI trap is genuinely avoided. SEND: smp_send_reschedule (smp.c:213-218) writes
  GICD_SGIR = ((1<<cpu)<<16)|INTID with dsb ish before/after. EOIR: the production
  _vec_el1_irq writes the FULL IAR value (including the source-CPU field — required for
  SGIs) to GICC_EOIR -> no active-priority corruption (shell/timer/raft alive AFTER
  the cert). INIT-ORDERING (§2.5 linchpin): smp_gic_selftest_setup stands up GICD_CTLR
  + knl_intvec[0] + gicc_base_ptr=GICC_BASE (the vector reads IAR/EOIR through it;
  gic_init normally sets it only at T-Kernel boot, AFTER the cert) + the boot CPU
  interface BEFORE the secondary fires; SGI ids 0-15 are always-distributor-enabled so
  no GICD_ISENABLER write is needed; the later gic_init is idempotent. ABI: the handler
  disassembles to a pure leaf (no stack, no nested call, clobbers only caller-saved
  x0-x2) -> cannot corrupt the vector's saved context. Zero SYNC/SERR/FIQ markers in
  any run.
- [shipped-kernel-byte-identical] The DEFAULT build is byte-identical to base aaa2e093
  across the WHOLE kernel.elf (sha256 df327396... both; .text b48d7da7...; smp.o is a
  0/0/0 empty object md5-equal to base; cpu_support.o byte-identical). The cpu_support.S
  diff is ONLY the gated SMPCPU_SIZE 40->56 (inside #ifdef SMP_SELFTEST); the production
  _vec_el1_irq dispatch is byte-unchanged. tkdev_conf.h adds only #define GICD_SGIR
  0xF00 (unused in the default build). task.c untouched.
- [one-mind-and-no-regression] Mind path UNTOUCHED (no mind TU in the diff). ②.0 still
  green (smp0-test SMP-RUN/MUTEX/BOOT PASS + NOLOCK falsifier RED); MC-2 still green
  (mc2-test PASS, mc2-equiv 3 identical FNV 0xd4c96f98... + Tooth A bites). check_parity OK.
- [honest-scope] Honest: this is COOPERATIVE-at-a-checkpoint preemption (the SGI handler
  only SETS g_resched_pending; B re-selects at loop boundaries under the BKL) — NOT a
  true async register-context stack switch. That + the production knl_ctxtsk conversion
  (~166 sites) is ②.2, correctly deferred. RPi3 uses the BCM2837 mailbox (GICD_SGIR is
  QEMU-virt-GICv2-only; guarded for BOARD_RPI3); the RPi3 mailbox port is deferred. The
  QEMU PASS proves the PLUMBING is load-bearing, NOT the barrier discipline on weak
  hardware (RPi3-[live] only). NIT (non-blocking): a stale BOARD_RPI3 comment promises a
  no-op marker the code doesn't emit.
LEDGER: row CLOSED. p-kernel's first inter-processor interrupt: one aarch64 core writes
GICD_SGIR, another takes the SGI through the production IRQ vector (unforgeable
sgi_taken proof) and preempts to a higher-prio task — provably the SOLE trigger (the
-DSMP_NO_IPI falsifier never switches). Shipped kernel byte-identical, mind untouched.
True async switch + production scheduler conversion = ②.2.

## ②.1b SMP self-test polish (N=4 + harness-snapshot fix) — commit 6b24aa97 (impl 90d753aa)
- Auditor: independent focused auditor (single agent, proportionate to the small
  sandbox-only scope), 2026-06-21. Did NOT write the code. Verdict: **MERGEABLE
  (ledger row CLOSED).** No findings above informational.
- N=4 is REAL: -smp 4 wakes 3 secondaries (cpu1/2/3 enter the dispatcher, order
  varies = real concurrency); all 4 per-CPU exec_count>0; shared counter == 800000
  EXACTLY (=4*200000); the NOLOCK falsifier loses a VARIABLE ~70% (212705/249442
  across runs = genuine 4-core race, not a fixed fake), SMP-MUTEX FAIL, still boots.
  Per-CPU secondary stacks are distinct/non-overlapping (cpu1/2/3 each 0x4000,
  _Static_assert guards the offset/size).
- PRODUCTION SCHEDULER UNTOUCHED: task.c diff EMPTY; cpu_support.S lines 1-381
  (incl. .Ldispatch_loop, _vec_el1_irq, all knl_ctxtsk loads) sha256 IDENTICAL
  base<->impl; start.S lines 1-281 IDENTICAL; both changes strictly inside
  #ifdef SMP_SELFTEST (the SMPCPU_SIZE 56->64 mirror + the per-CPU stack load).
- DEFAULT build BYTE-IDENTICAL to base (.text + full ELF sha256 match); plain boot
  zero SMP output.
- The HARNESS-SNAPSHOT FIX (the ②.0-audit follow-up) works: all 4 harnesses now
  cp kernel.elf to a unique mktemp snapshot and boot -kernel <snapshot> (not the
  live build-dir elf), so a concurrent rebuild can no longer corrupt an in-flight
  run; set -eu + real greps retained. No regression (smp1/mc2 all PASS). Honest
  N=2 preempt note in-code (cpus 2,3 would only spin to the watchdog cap — no added
  proof; robust N=4 preempt deferred to ②.2).
LEDGER: row CLOSED. The SMP sandbox now scales to 4 cores (counter exact, falsifier
bites) and the cert harnesses are concurrency-safe; the production scheduler stays
byte-for-byte untouched.

## ②.2a production scheduler -> SMP (~211 sites) — commit 9e476954 (impl a20985b8)
- Auditor: independent adversarial WORKFLOW (5 dimensions + per-finding verify),
  2026-06-21. Did NOT write the code. THE LARGEST, most invasive change in the repo:
  the production T-Kernel scheduler converted to SMP.
- Verdict: **the ②.2a impl is SOUND (byte-identity + conversion-correctness +
  2-real-tasks all PASS); the workflow gated overall=BLOCKED ONLY on a ②.1b MERGE
  COLLISION, which the COMMANDER resolved + empirically validated (this commit).**
  confirmed_material_findings=[].
- [byte-identical-N1-crown] PASS — the crown held at the STRONGEST level: the default
  build's .text (sha256 b48d7da7…), full objcopy binary, AND full ELF are ALL byte-
  identical to a FRESHLY-rebuilt base; per-object cmp on the 3 dangerous files
  (cpu_support.o b090206e, task.o 1f8aa03b, memory.o 4d16591b) byte-identical. The
  macros expand to parenthesized plain globals when SMP off; cross-arch fallbacks keep
  linux/x86 unchanged (linux boots identically). No site silently changed the N=1
  scheduler.
- [conversion-correctness] PASS — a per-file 1:1 CONSERVATION CHECK proves NO
  ctxtsk<->schedtsk mis-swap anywhere (each file's old knl_ctxtsk count == new
  CUR_CTXTSK, old knl_schedtsk == new CUR_SCHEDTSK); no missed sites in the aarch64
  build set; the 4 dangerous sites (asm dispatcher LD_PERCPU_BASE, END_CRITICAL_SECTION,
  timer, in_indp) + the 4 bare-DI allocator sites (memory.c) BKL-wrapped across the
  full DI/EI window; BKL recursive-safe + balanced. The deferred-scope incompleteness
  (a general multi-CPU syscalling workload) is ②.2b/②.3, ledgered, not a blocker.
- [2-real-tasks-on-2-cpus] PASS — A=&knl_tcb_table[0], B=&knl_tcb_table[1] (distinct
  real TCBs), B tskid=2 (real tk_cre_tsk); a GENUINE register-context switch
  (smp_prod_enter_dispatch -> .Ldispatch_loop -> TCB_SSP 112-byte frame restore ->
  ret into B); B records its TCB via CUR_CTXTSK indexed by CPU 1's MPIDR (unforgeable,
  not a flag CPU 0 set); claimed under the BKL (knl_make_non_ready) so the two CPUs
  can't race the same task; live shell after. Stable 4 runs.
- THE MERGE (commander integration, the audit's flagged BLOCKER): ②.2a (base 80925b61)
  and ②.1b (on trunk) BOTH appended a struct smp_cpu field at offset 56 and bumped
  SMPCPU_SIZE 56->64 — a naive cherry-pick would silently corrupt one field. Resolved:
  struct = base(0..55) + stack_top@56 (②.1b) + dispatch_disabled@64 (②.2a), sizeof 72,
  SMP_MAX_CPUS=4; synced across smp.c + smp_percpu.h (the typed view kernel/common
  indexes — a desync would read the wrong CPU's slot) + cpu_support.S; fixed
  LD_PERCPU_BASE from `lsl #6` (x64) to `(id<<6)+(id<<3)` (x72, non-power-of-2).
  EMPIRICALLY VALIDATED on the merged tree: default .text byte-identical (b48d7da7…) +
  smp2 [smp-2tasks-prod] PASS (exercises the new stride + dispatch_disabled) + smp0
  N=4 counter==800000 PASS (exercises stack_top) + smp1 preempt PASS + mc2-slice PASS.
- HONEST BOUND: ②.2a proves the MECHANISM. The production kernel runs only on CPU 0 in
  this slice; the secondary timer/WAIT path is unwired (task B parks on wfe; a blocking
  syscall faults — the impl hit + fixed a real SYNC fault); cross-CPU preempt is still
  cooperative (the ②.1a SGI sets a flag). True async preempt = ②.2b; the [smp-one-mind]
  crown cert = ②.2c. QEMU masks barriers (RPi3-[live] only).
LEDGER: row CLOSED. The production T-Kernel scheduler runs 2 real tasks on 2 physical
cores under one Big Kernel Lock, with the shipped uniprocessor kernel BYTE-IDENTICAL
and the mind untouched. p-kernel is genuinely SMP at the scheduler level — the heart of
mk_pino's "最終目標". (②.2b true-async + ②.2c one-mind-crown remain.)

## SMP-N8 (4 -> 8 physical cores) — commit 5922fa91 (impl 0f105f44)
- Auditor: independent focused auditor (single agent, proportionate to the sandbox
  expansion), 2026-06-21. Did NOT write the code. Verdict: **MERGEABLE.**
- 8 CORES REAL: -smp 8 wakes 7 secondaries (cpu1..7 enter, scrambled order = real
  concurrency); 8 per-CPU exec_count>0 keyed off MPIDR (smp.c:123/899); the barrier
  waits for exactly 8 arrivals (smp.c:701) — unreachable unless 8 distinct cores run;
  shared counter == 1600000 (=8*200000) EXACTLY; NOLOCK falsifier loses VARIABLY
  (382089/361389/351423 across runs = true race). Per-CPU stacks cpu1..7 distinct,
  16-aligned, non-overlapping (0x4000 stride), 14.4MB below the heap base — no collision.
- DEFAULT build BYTE-IDENTICAL: .text + full objcopy binary + full kernel.elf ALL
  cmp-identical to base (sha b48d7da7…); smp.o is 0/0/0 in the default build. The
  struct LAYOUT is unchanged (SMPCPU_SIZE 72, stack_top@56, dispatch_disabled@64) —
  only g_smpcpu[] COUNT grew 4->8.
- THE cpu4..7-STACKS-IN-BSS DECISION (validated): _kernel_end is baked into shipped
  .text via cpu_init.c:32 (knl_lowmem_top); extending linker.ld would move it + break
  byte-identity — the impl correctly put the stacks in static SMP_SELFTEST-gated BSS
  instead (linker.ld change is comment-only), preserving identity.
- THE -smp 8 HARDCODE (honest): smp_bringup_secondary now releases cores 1..7, so the
  SMP build REQUIRES -smp 8 (fails under -smp 4 — boot CPU starves at the 8-arrival
  barrier); the run_smp2 harness was bumped 4->8 (the honest fix; smp2 still uses only
  CPUs 0,1, cores 2..7 idle). GICv2 ceiling = 8 (SGI target-list is 8 bits; >8 = GICv3).
  THE PROPER FOLLOW-UP that makes the woken-count ADAPT to the device (RPi3=4, phone=8,
  same binary) is the GICD_TYPER runtime-autodetect designed in
  docs/architecture/device-autodetect-plan.md (now on trunk) — NOT a blocker.
- NO REGRESSION: smp1-test (preempt + NO_IPI falsifier), smp2-test (@-smp 8), mc2-test
  all PASS. Production scheduler/mind/x86/linux untouched. check_parity OK.
LEDGER: row CLOSED. The SMP machinery scales to 8 physical cores (the real phone target /
GICv2 ceiling) with exact mutual exclusion (1600000) and a real variable-loss falsifier;
the shipped uniprocessor kernel stays byte-identical. Runtime core-count autodetect
(GICD_TYPER) is the designed follow-up that makes the woken-count adapt per device.

## federation R0.1 (responder-side locality gate + coord-crash-live verify) — commit fc04e36c (impl 4cc68cfc)
- Auditor: independent focused auditor (single agent), 2026-06-21. Did NOT write the
  code. Closes the two audit-flagged OPEN items of the shipped federation R0.
  Verdict: **MERGEABLE.**
- ITEM 1 (the genuinely-new work) — RESPONDER-side locality gate, REAL with teeth
  (3/3 stable): the R0 [live] gate measured only node1-the-REQUESTER, so a resp/<n>
  REGION→GLOBAL degeneration on the responder/coordB nodes was statically-correct but
  NOT dynamically falsified. R0.1 reads node3(coordB)+node4(non-coord responder)
  locality. HEALTHY (resp=REGION): node4 far=6/near=103. FALSIFIER
  (PKERNEL_DKVA_RESP_GLOBAL=1 on the responders): node4 far 6→206 (+200) → the new gate
  goes RED. Mechanism (kdds.c:271-283): a REGION topic delivers only to region members
  (all near); GLOBAL fans out → non-members count far. THE GAP R0.1 CLOSES, proven
  dynamically: the REQUESTER's far-delta is resp-INVARIANT (zoned node1 far=8 ==
  respglobal node1 far=8) — the requester genuinely cannot see this; only the
  responders can.
- ITEM 2 ([coord-crash][live]): re-verified PASS (run_coord_crash.sh — kill the
  coordinator → min-id re-delegation "node 0 DEAD heir=1", no election, DKVA
  reconverges via the survivor, no SPOF), and CONFIRMED already-shipped-at-base (both
  source commits 0fcad504+3215687d are git-ancestors of base 82d0295a; not in the R0.1
  diff). So R0.1's net-new code is item 1 only.
- BYTE-IDENTICAL production path: dkva_resp_scope_global defaults 0 (dkva.c:129) →
  resp opens KDDS_SCOPE_REGION, the O(#region) property (rsum=GLOBAL, resp=REGION)
  intact; the falsifier knob is env-only (PKERNEL_DKVA_RESP_GLOBAL).
- THE K 6→10 LOOSENING (scrutinized hardest, HONEST + SAFE): the requester gate's
  bound (a) was raised 6→10 because keeping region-B responders alive+polled across the
  window adds ~2 background GLOBAL crossings (zoned far now consistently 8; K=6 would
  false-FAIL). SAFE: a real per-node O(#region)→O(N) degeneration drives far into the
  ~200s (20x above K=10 — no-man's-land); the LOAD-BEARING falsifiers stay green + bite
  (b1 flat far-delta==0; b2 near-collapse is magnitude-independent). The loosening
  relaxes only bound (a), not the teeth.
- NOCENTRAL/one-mind/no-regression: in-proc dkva test PASS on aarch64 AND x86_64
  (bit-identical); rep selection is pure min-id recompute (region.c:68), no vote/
  election on the dkva/region path (raft.c off-path); only new symbol
  dkva_set_resp_global; mind-math + aarch64 SMP untouched; 4 builds clean; check_parity OK.
- HONEST FLOOR (expected): [live] ran 4-node/2-zone as 8 OS processes on ONE host
  through ./relay; SSH 2-machine deferred. The 10k-dream gap (256 node_id wall) stays open.
LEDGER: row CLOSED. Both R0 OPEN items are genuinely closed — a dynamically-falsifiable
responder-side gate that catches the degeneration requester-only measurement was blind
to, and coord-crash-live re-verified. The K bump is necessary + safe.

## SMP-AUTODETECT (GICD_TYPER runtime core-count) — commit d31f7457 (impl 25a7eae5)
- Auditor: independent focused auditor, 2026-06-21. Did NOT write the code. Verdict: **MERGEABLE.**
- THE HEADLINE (reproduced): ONE -DSMP_SELFTEST binary booted under -smp 1/2/4/8 detects
  1/2/4/8 cpus via GICD_TYPER and wakes exactly that many — shared counter
  200000/400000/800000/1600000, RUN/MUTEX/BOOT PASS each, T-Kernel boots. No recompile.
- GICD_TYPER decode correct + overflow-safe: ((typer>>5)&0x7)+1 clamped to SMP_MAX_CPUS=8
  (smp.c:234-251); FORCE_NCPU=16 disassembles to mov w0,#8 → a >8 value cannot overflow
  the ceiling-sized arrays. g_smp_ncpu drives bringup/barrier/join/cert; array sizing stays 8.
- FORCE_NCPU=8 @-smp 2 falsifier FAILs (SMP-RUN/MUTEX/BOOT FAIL, watchdog, no hang) — proves
  GICD_TYPER detection is load-bearing.
- DEFAULT BYTE-IDENTICAL: confirmed against the CURRENT trunk baseline (e3b14ee3 default .text
  == d31f7457 default .text == 755a20fae2d9b741; the earlier b48d7da7 baseline shifted when
  fed R0.1's dkva.c — compiled into the bare-metal kernel — legitimately changed it). All
  autodetect code is #ifdef SMP_SELFTEST; smp.o byte-identical in the default build.
- RPi3 honesty: BOARD_RPI3 → smp_detect_ncpu() = mov w0,#4 (BCM2837 is the ARM Local
  Interrupt Controller, NOT a GIC — no GICD_TYPER); the DTB /cpus path + GICv3 (>8) DEFERRED.
- No regression: smp1/smp2/mc2 + check_parity PASS. LOW (auditor process note, not a defect):
  the aarch64 harnesses snapshot kernel.elf AFTER make, so running several in PARALLEL against
  one build tree races (a sibling build clobbers between make + snapshot) — run them serially.
LEDGER: row CLOSED. mk_pino's "measure the device + auto-fit": the SAME binary now adapts its
woken-core count to the device via GICD_TYPER (RPi3=4, phone=8) — no recompile. The full
measure-specs->size-the-mind (device-capacity) stays the deferred bigger thread.

## T-fix-b / T-1 (the lesson BRIDGE) — commit (cherry-picked to trunk) (impl 0f8997f5)
- Auditor: independent focused auditor, 2026-06-21. Did NOT write the code. Verdict: **MERGEABLE.**
- THE CERT [cradle-teach] is REAL (generalization, NOT rote): a teacher TEXT lesson, pulled by
  the student over the KDDS CRADLE_TEACH beacon + p-fs DAG body, trained → a NEVER-trained
  held-out PROBE occurrence's loss drops 5.5220→1.8358 (3.69 nats), starting near chance
  (ln256=5.5452). The auditor reconstructed the geometry: train_end=960, probe window [960,992)
  is entirely in the never-trained tail; the held sentence appears NOWHERE in [0,960) → genuine
  generalization, weight-resident.
- 3 FALSIFIERS RED + sabotage-proven non-vacuous: ARM A teaching-OFF (drop 0.61<<3.69), ARM B
  scrambled (drop −0.32, loss ROSE), ARM C never-taught probe (6.58 vs 1.84). Sabotage tests:
  neutering cradle_lesson_ingest → cert FAILED ("ring not live"); disabling Arm B's scramble →
  cert FAILED ("scrambled lesson still taught"). Both teeth bite.
- BYTE-IDENTITY crown: run_kv 18/18 (byte+logit-hash identical, S/M/L); run_ss3 6/6; student.c
  MATH untouched (not in the diff). With NO lesson cradle_window_src returns NULL → window()
  reads TEACHER_FIXTURE via the IDENTICAL modular math → byte-identical training to base.
- ONE-MIND/NOCENTRAL: no gl_merge/st_merge_cohort in cradle (grep [cradle-nocentral]); student
  trains via its OWN st_backward/st_adam_step; teacher selection = local region_teacher() (T-fix-a,
  no vote); vocab_fp refuse-on-mismatch.
- PARITY: cradle_net.c in all 3 COMMON lists + cradle.c in all 3 LLM lists; check_parity OK; both
  hosted ports build clean; bare-metal builds (student_shell absent there → seam never referenced).
- HONEST [in-proc]: the cert feeds the ring directly; the KDDS+p-fs carrier is real in-kernel code
  but the multi-process live teacher-convergence over ./relay = DEFERRED [live] row; the GGUF
  teacher harvest = DEFERRED (CT-2); LESSON_FMT_SOFT reserved. Informational only: a cosmetic
  comment imprecision (cradle.c:342), no correctness impact.
LEDGER: row CLOSED. A teacher's TEXT lesson becomes WEIGHT-RESIDENT in the student (generalizes
to a never-trained probe), over the real KDDS+p-fs carrier, byte-deterministically, NOCENTRAL —
the teaching THREAD's transport now exists. Live multi-process teacher-convergence deferred.

## N-2c (supernode packet forwarding) — commit 9c3a1c52 (impl 0e4c8aca)
- Auditor: independent focused auditor, 2026-06-21. Did NOT write the code. Verdict: **MERGEABLE,
  no findings at any severity.**
- THE FORWARD IS REAL + byte-identical: A→B routed THROUGH the elected supernode S (region fwd:
  S.forwarded_count=1, payload_len=20, B BYTE-IDENTICAL to A, origin A preserved, via_super=1);
  20 PASS cross-arch (linux + linux_x86_64). Route decision snf_route_target (supernode.c:86) is a
  pure integer fn of (supernode,self,dst), fail-closed (self/dst/0xFF/oob → DIRECT).
- FALSIFIERS degrade correctly: (a) no supernode 0xFF → DIRECT, S forwards 0, bytes delivered;
  (b) unreachable/DEAD S → fail-closed DIRECT (runtime check dnode_table[sn].state==DNODE_DEAD,
  supernode.c:131), no packet lost. PRODUCTION-PATH SABOTAGE goes RED: injecting payload[0]^=0x01
  into the SHIPPED snf_forward re-wrap → 19/20 (the byte-identical sub-arm FAILs) = teeth on the
  real path, not a toy.
- NOCENTRAL/one-mind: S = the min-id region_supernode() (zero vote/elect/quorum in supernode.c);
  forwarded payload byte-identical; no VLA (fixed SNF_PKT=524B, file-static). DEFAULT byte-unchanged
  (my_supernode=none, port unbound, counters 0). check_parity OK (supernode.c hosted-only,
  allowlisted like ss6_live.c; the auditor sabotage-tested the parity guard → DRIFT when removed).
- No regression: region 8/8, region-teacher 6/6, swim certs clean; both hosted ports build; mind/
  region.c/swim.c/dkva.c/SMP untouched.
- HONEST [in-proc]: the cert drives the real snf_rx/snf_forward/deliver + counters (only the UDP
  socket hop stubbed); the true 3-process [live] run_supernode_fwd.sh is WRITTEN + DEFERRED (the
  PRoot sandbox kills backgrounded children — the ss6_live/4node_regions wall). >>> COMMANDER TODO:
  cash this [live] row on the real SSH host (ThinkPad). NAT (N-3)/seed (N-4)/ACK-retry deferred.
LEDGER: row CLOSED. The deterministically-elected supernode now FORWARDS region traffic
byte-identically (A→S→B, not via the central relay), fail-closed to the relay when no/dead super —
the first real step of the decentralized P2P overlay. Live multi-process forward = SSH-host TODO.

## N-2c [live] — RUN ON THE REAL SSH HOST (ThinkPad), 2026-06-21 — OPEN (real bug surfaced)
- Commander cashed the deferred [live] row on the real x86_64 host (sandbox can't run it —
  PRoot kills backgrounded children). tar-over-ssh'd the latest source → built
  boot/linux_x86_64/p-kernel (3.9MB) + relay → ran samples/11_distributed/run_supernode_fwd.sh
  (real OS processes: A=node1, S=node2 supernode, B=node3, relay).
- RESULT: the [in-proc] cert (merged, valid) does NOT match the [live] behavior. The election +
  routing DECISION works (A sets via_super_cnt=1 = it chose to route through the supernode) and
  the degrade falsifiers behave, BUT the actual FORWARD does NOT deliver: S.forwarded=0,
  node3 delivered=0, payload empty. The A→S SNF_FWD datagram (udp_send to the overlay IP
  10.1.0.x on SNF_PORT 7380) does not arrive at S over the real ./relay transport.
- DIAGNOSIS (likely): in the relay-overlay mesh, udp_send to a 10.1.0.x overlay address must be
  carried by the relay (REL_DATA by node-id); the SNF_PORT 7380 forwarding traffic appears NOT
  to be routed through the relay (or an addressing mismatch), so the forward silently drops. The
  in-proc cert STUBBED the socket hop (fed bytes directly), so it could not catch this — exactly
  what the real-host [live] run is for. This is the SS-6→SS-6-live pattern: the [in-proc] is
  honest about its stub; the [live] reveals the real-transport gap.
- STATUS: N-2c [in-proc] stays MERGEABLE/valid (the route decision + counters + byte-identity of
  the forward LOGIC are correct). N-2c [live] is OPEN — a real follow-up: make snf forwarding
  actually traverse the ./relay (like ss6_live.c's remote-expert path does), then re-run on the
  ThinkPad. THE DEBUG ENV PAID OFF on first use: it found a real distributed-transport bug the
  sandbox structurally cannot.

## N-2c [live] — re-verified on the SSH host AFTER the port fix, 2026-06-21 — STILL OPEN (honest)
- The SNF/PMESH port collision (SNF_PORT 7380 == PMESH_PORT 7380; netstack udp_input delivers to
  the FIRST matching socket + returns, and pmesh binds 7380 at boot → SNF traffic was eaten by
  pmesh_rx) was a REAL bug, fixed (SNF_PORT → 7377, a verified-free slot) and MERGED (commit
  4ae1c130 area). In-proc cert 20/20 + no-regression confirmed.
- BUT the real-host [live] re-run STILL FAILS: S now binds 7377, but snf_rx never fires (S
  forwarded=0, B delivered=0); the relay log shows NO REL_DATA dst=S for the SNF_FWD. So the port
  was ONE bug, not the whole story. Remaining [live] issues (honest, do-NOT-fudge-green):
  1. A's udp_send(snf_node_ip(S)=10.1.0.2, 7377) does not reach S over the ./relay even at the
     unique port — likely a MEMBERSHIP-ROUTING/TIMING issue (the 3-node startup flaps
     FULL→REDUCED→FULL; the harness probes ~11s in, possibly before A has a stable relay route /
     drpc-seeded ARP to S). ss6_live works [live] because its cert runs after stable convergence.
  2. A HARNESS verdict mismatch: run_supernode_fwd.sh asserts my_supernode=2 (the PKERNEL_NODE_ID),
     but the code correctly reports my_supernode=1 (the internal 0-indexed node-id = S). The
     election is RIGHT; the harness's expected value is off-by-mapping.
- STATUS: N-2c [in-proc] valid + merged (route logic, counters, byte-identity, the port fix). N-2c
  [live] is OPEN — a dedicated [live] BRING-UP follow-up: fix the harness verdict mapping + wait for
  stable convergence before the probe + confirm A's overlay udp_send reaches S over the relay (the
  membership-routing path). The DEBUG ENV's value is proven: it found the [live] failure AND a real
  port-collision bug the in-proc cert (socket stubbed) structurally could not. NOT fudged green.

## ②.2b-i true async preempt + the certified BKL-held deadlock guard — commit a76f3606 (impl 50a2ed3a)
- Auditors: TWO independent rounds (impl≠auditor≠commander). (1) The original 5-dim adversarial
  WORKFLOW on ②.2b-i (f44aac86): 4 dims PASS (async-preempt-is-REAL, irq-vector-C-ABI, byte-
  identical, honest-scope) but GATED the 5th (deadlock-avoidance) UNCERTAIN — the BKL-held guard
  `if(g_bkl_owner==me) return 0` (smp_irq_need_resched) was correct-by-reasoning but UNCERTIFIED
  (deleting it failed no cert; the design-mandated [smp-no-deadlock] falsifier was missing).
  (2) A focused auditor on the CLOSURE (50a2ed3a): the guard is now CERTIFIED. Verdict: **MERGEABLE.**
- THE HARD WIN (mk_pino's directive "諦めてやらないのはNG" — do NOT defer it): rather than
  code-review-certify + defer the runtime falsifier to RPi3 (the easy out), a REAL [smp-no-deadlock]
  cert was constructed (it took the implementer ~3.6h; commander prematurely mis-flagged it "dead"
  at a 21-min idle — it was a legitimately long run, finished clean).
- [smp-async-preempt] (the ②.2b-i core): a tight no-poll loop task on a secondary is preempted
  MID-LOOP by a real register-context switch from IRQ context (hook between EOIR and
  restore_caller_regs in _vec_el1_irq; capture ELR/SPSR; two-frame nest + .Lirq_resume_tramp) and
  RESUMES (observed_counter ∈(0,cap), final==cap). Falsifier -DSMP_NO_ASYNC: sgi_taken=1 but no
  switch → FAIL. (4-dim audit PASS.)
- [smp-no-deadlock] (the certified guard, NON-VACUOUS, reproduced 3/3 each way): a REAL task L
  (tk_cre_tsk, run through the PRODUCTION dispatcher) acquires the BKL via a LEGITIMATE bounded
  critical section; the driver fires the reschedule SGI DETERMINISTICALLY while L holds the BKL
  (sgi_taken handshake; observed_crit(at H) STRICTLY in (0,cap) proves the SGI landed mid-section).
  WITH the guard → the switch DEFERS, L finishes + releases, the deferred reschedule (pending flag
  retained) fires, H runs + acquires the BKL cleanly, L resumes → SMP-NO-DEADLOCK: PASS (reaches
  the shell ~14s, vs the 45s budget = 3.2x headroom). WITHOUT (-DSMP_NO_BKL_GUARD, removes only the
  one clause) → the switch fires mid-crit, H wedges forever waiting for the BKL L stranded →
  PERMANENT deadlock (no PASS, never reaches the shell, L provably acquired the BKL) → caught as a
  fail-closed timeout-FAIL (robust: the guard'd run finishes well within the timeout; the harness
  requires POSITIVE evidence so a boot failure fails RED, never a false PASS).
- THE 2 TOCTOU WINDOWS the original audit flagged are FIXED: bkl_acquire published g_bkl_owner
  AFTER raw_lock (gap: raw-held, owner!=me); bkl_release cleared it BEFORE raw_unlock. An SGI in
  either gap mis-reads g_bkl_owner → skips the guard → switches holding the raw lock → deadlock.
  Fix: IRQ-mask each tiny publish/clear window (smp_bkl_di/ei = a faithful clone of production
  disint/enaint), so the guard only ever observes g_bkl_owner consistent with raw ownership. ONLY
  the windows are masked, NOT the critical-section body (the cert holds the BKL IRQ-unmasked on
  purpose). The helpers have no loops/locks → cannot wedge.
- DEFAULT BYTE-IDENTICAL (.text 755a20fae2d9b741, smp_deadlock.o/smp_async.o excluded from the
  default link) + NO REGRESSION (async PASS + NO_ASYNC FAIL; smp0/smp1/smp2/mc2 PASS; check_parity
  OK). HONEST: ②.2b-ii (secondary CNTP timer/WAIT) deferred; QEMU faithfully models the control-flow
  self-deadlock so the falsifier bites on QEMU (NOT a barrier-discipline claim — that stays RPi3).
LEDGER: row CLOSED. The repo's hardest C-ABI slice — a true async register-context preempt from
interrupt context — ships WITH its safety guard CERTIFIED by a real, load-bearing deadlock falsifier
(not deferred). honest > green, and 諦めない. ②.2b-ii / ②.2c (the [smp-one-mind] crown) / ②.3 remain.

## ②.2c [smp-one-mind] CROWN — the mind is byte-identical under SMP — commit eaa98f75 (crown impl 50abf7a3 + falsifier fix eaa98f75)
The PAYOFF of the ② full-SMP arc: a REAL bare-metal mind forward (`r_forward`, a 4-head Transformer,
R_NP=21568, pure -ffp-contract=off math over the shared static rw[]/rc) produces a BYTE-IDENTICAL
output hash whether run uniprocessor (H_uni, CPU 0) or scheduled as a real `tk_cre_tsk` task on an SMP
secondary (H_smp, CPU 1, via the ②.2a smp_prod pattern through the production .Ldispatch_loop, CPUs
2/3 busy fillers = genuine concurrency). H_uni == H_smp ⇒ the SMP scheduler did not perturb one bit of
the mind's math: "the mind stays one across the SMP scheduler."
- Roles (impl≠auditor≠commander, per the constitution): the IMPLEMENTER built the crown (50abf7a3)
  then died on a 529 mid-report; an INDEPENDENT clean-room AUDITOR (own worktree) gave the verdict; the
  COMMANDER persisted the verified falsifier-fix diff (the implementer of that fix ALSO died on a 529
  pre-commit — known Opus pattern, work was uncommitted in the worktree) and ran the final verification.
- AUDIT VERDICT: **MERGEABLE-WITH-NITS.** The crown's POSITIVE claim is rock-solid; the ONE nit was the
  negative control, and it was FIXED BEFORE MERGE (諦めない / honest>green) — not waved through.
- CHECK 1 — CROWN byte-identity: PASS. Default `.text` sha = `755a20fae2d9b741…` (smp_onemind.o excluded
  from the default link); `r3_incontext.o` = `3edc0d0777fa587d…` byte-identical base-2f395561 vs impl
  (the SMP_ONE_MIND gating does NOT perturb the default object). Re-confirmed on trunk post-merge.
- CHECK 2 — [smp-one-mind] PASS + STABILITY: PASS. H_uni == H_smp == `0x2856a99b23880b4c`, zero variance.
  Stability ≈ 50+ clean samples with ZERO spurious failure (auditor 24/24 + commander 20/20 raw + 9/9 in
  3 full harness runs). The implementer's "one early failure" was build contention (a stale/parallel
  half-link), reproduced only under a dirty tree, never under `make clean; make -j1`.
- CHECK 3 — FALSIFIER bites + NOW DETERMINISTIC: PASS. `-DSMP_ONEMIND_RACE` (a 2nd CPU scribbles the
  shared static rc/rw[] mid-forward) → `SMP-ONE-MIND: FAIL hashes-differ`, H_smp a DIFFERENT garbage value
  each run (e.g. 0xe2bd9c59, 0x00d2fe5c, 0xbfe9d1af…), ALWAYS ≠ H_uni — proof the cert observes the REAL
  mind output. THE NIT (audit-found): the original racer was a fixed one-shot 50M-iter burst with no sync
  to M's forward window → ~13%/boot it landed entirely before the re-seed or after the reads → spurious
  PASS → `make smp4-test` self-aborted with a ~34% FALSE "cert vacuous" BLOCK (never a false GREEN, always
  safe-side). FIX (confined to falsifier-only code: `#ifdef SMP_ONEMIND_RACE` + smp_onemind.o, default
  byte-identity preserved by construction): bracket EXACTLY r_forward's read window with a
  `g_om_forward_inflight` flag; the racer scribbles in small 100k chunks gated on the flag, continuously
  until it observes the forward complete (1→0), with a bounded 200M cap so it can NEVER hang. Result
  (commander-verified): **20/20 FAIL hashes-differ, 0 spurious PASS, 0 hang.**
- CHECK 4 — NON-VACUITY: PASS. M is a real `tk_cre_tsk` TCB (pri 8) claimed for CPU 1 under the BKL and
  run via the production dispatcher; per-CPU index is genuine hardware MPIDR_EL1 Aff0; `M ran=1` is written
  only by M's body, reachable only after the secondary's register-context switch (triple TCB-location
  guards confirm g_smpcpu[1].ctxtsk == mtcb). Not faked on CPU 0. Concurrency real (cpu2/3 fillers advance).
- CHECK 5 — NO REGRESSION + honest scope: PASS. The `-DSMP_SELFTEST` binary is byte-identical base vs impl
  (so smp0/1/2/3 + mc2 provably cannot change); honest narrowing is stated in source, the verdict print,
  the design doc, and the commit: the crown is SINGLE-forward byte-identity; CONCURRENT forwards race the
  shared static rc/rw[] and need a mind-lock — DEFERRED (exactly what the falsifier exploits). No
  concurrent-mind-safety overclaim.
LEDGER: row CLOSED. The ② full-SMP arc reaches its payoff: a REAL mind, run as a task on a true
async-preemptive SMP scheduler, is byte-for-byte the same mind as uniprocessor (0x2856a99b23880b4c). The
shipped uniprocessor kernel is untouched (755a20fa). The audit's one nit (a flaky negative control) was
fixed to deterministic-RED before merge, not deferred. HONEST scope: single-forward; concurrent minds
need a mind-lock (DEFERRED); QEMU green ≠ hardware green (RPi3 barrier/SMPEN teeth stay future). Remaining
②: ②.2b-ii (secondary CNTP timer/WAIT — design hardened, plan at smp-2b-ii-secondary-timer-plan.md),
②.3 (finer locks), hosted-port SMP, RPi3 [live].

## SS-4 function-preserving expert growth — commit 7e358bac (impl 9dfc568b, cert 7e358bac)
The student MoE can GROW its expert count (m->nexpert) as the fleet's cap_experts_of(N) rises — WITHOUT
changing one bit of what it computes. The Evolution-layer "brain scales with nodes" capability, made
EXACT. Independent clean-room auditor verdict: **MERGEABLE** (decisive checks passed under adversarial
falsification beyond the cert's own input).
- THE MECHANISM (and the design's corrected textbook mistake): this router is top-K-then-softmax-over-
  the-chosen-set with MARGIN WIDENING (router_pick, student.c:478-512), NOT a global softmax — so the
  standard "clone an expert + halve its router column" recipe is WRONG here (a clone is admitted by margin
  widening + steals softmax mass). Correct function-preserving growth = add a DEAD expert: router row=0,
  W2=0, AND an alive[] mask giving it an EFFECTIVE LOGIT OF −∞ (the widening ceiling is n_alive not ne;
  dead experts tail-park in order[]) so it is PROVABLY never in the chosen top-K for ANY input. The −∞
  mask — NOT the zeroed row — is the load-bearing exactness mechanism.
- CHECK [expert-growth-preserves] EXACT ε=0: logits FNV `85ed22a0482f0b81` byte-identical pre/post grow
  (E=4→8 DEAD), nk/firing-width unchanged for every token; auditor reproduced + extended — **40 diverse
  inputs + 4000 random (125,828 tokens): 0 drift, a DEAD expert NEVER chosen (max_width==n_alive==4)**.
- FALSIFIER bites: -DSS4_GROW_NAIVE (random-init LIVE experts) → hash moves `b0c5ad0ec067ae24` → RED. The
  design's central claim independently re-confirmed: router-row=0+W2=0 but alive=1 (NO −∞ mask) → hash
  MOVES `e61bf9c52255b507` (a 0 logit IS admitted by margin widening) → only the mask makes it exact.
- HEAP RESHARD INTEGRITY (auditor, beyond the impl): incumbent W1/W2/W3/router AND Adam moments mu/vu are
  BIT-IDENTICAL at the new strides (0 diffs); grown model still trains (loss 2.42→2.19). No silent
  incumbent corruption hiding behind the single-input hash.
- SS-6 STOP gate: the shipped-student hashes M=`63e8de333e995913` / L=`67f2434f50e791b6` UNMOVED (the only
  shared-function edit — the widening cap nk<ne→nk<n_alive — is byte-identical when alive==NULL). CROWN
  untouched: student.c is NOT in either bare-metal link (grep -c = 0; student_stub.o linked); the st_model
  field addition is inert on bare metal. 755a20fa / 0x2856a99b unaffected.
- NO REGRESSION: SS-1/2/3/5, KV --machine byte-identity 18/0, [grow-cohort] (E=8 blob refused by E=4 via
  st_blob_tier_ok's n_expert AND n_params gate). Honest scope: EXACT for the ADD-DEAD step ONLY;
  resurrection (DEAD→specialized) is deliberately ε (separate looser [grow-then-learn], NOT claimed exact)
  + shrink are DEFERRED; no production caller yet (mechanism wave, as designed).
LEDGER: row CLOSED. The mind can grow its capacity as the fleet grows, byte-exactly — the "brain scales
with nodes" worldview is now a falsifiable, audited cert, not a slogan. The audit went BEYOND the
implementer (4000 random inputs + deep weight/Adam comparison) and confirmed exactness holds off-cert.
Honest deferrals: resurrection/shrink/x86_64-runtime-reverify. Routine hygiene: the teacher-dependent
student selftest (orthogonal to the change surface) confirmed post-merge PASS.

## ②.2b-ii secondary CNTP timer + cross-CPU WAIT wake — merge commit 201e6315 (impl 76f7c7b9, base 6316155c)
The next ② slice: an SMP secondary becomes a FIRST-CLASS scheduler, not just a compute engine — a task
running on a secondary can take its OWN timer tick (tk_dly_tsk) AND block on a semaphore woken by ANOTHER
CPU (cross-CPU wake → SGI → reschedule). This is the FIRST ② slice to EDIT shared-core code linked into
the default shipped kernel (kernel/common/task.c +1 via a macro hook, include/kernel/tkernel/task.h +15,
arch/aarch64/cpu_support.S +58), so the byte-identity crown was at maximum risk. Independent clean-room
auditor verdict: **MERGEABLE-WITH-NITS** (the only non-green is pre-existing + environmental).
- CHECK 1 CROWN byte-identity (decisive): PASS. Default `.text` = `755a20fae2d9b741…` base==HEAD; the two
  touched objects cmp-CLEAN base vs HEAD — `cpu_support.o`=`f3bad9a2…`, `task.o`=`f9ee9f21…` did NOT move
  one bit. Re-derived 5× by the auditor + a 6th time on the MERGED trunk (201e6315) by the commander.
- CHECK 2 the hook vanishes: PASS. `knl_smp_wake_hook(tcb)` expands to ZERO TOKENS when SMP_SELFTEST is
  off (an empty preprocessor macro, NOT an empty inline taking tcb); the shared task.h carries the empty
  `#ifndef` fallback so x86/linux/rl78 still build (linux + x86_64 ports built clean; `nm task.o` = zero
  `knl_smp_wake` refs). The one shared-core edit is provably inert in the shipped kernel.
- CHECK 3 the 3 new dispatcher mechanisms (`.Ldispatch_loop` live-guard, `.Lsmp_idle` BKL-handoff,
  `smp_irq_need_resched` clause-2b) are all `#ifdef SMP_SELFTEST`-gated; the production `knl_dispatch` arm
  is unchanged (Check 1's byte-identity proves the gating is effective).
- CHECK 4 cert + non-vacuity: PASS 10/10. Half (i): a CPU-1 task woken by CPU 1's OWN tick
  (current_time_delta=60≥50, CPU 0's timer masked out via disint()). Half (ii): a CPU-1 task on
  tk_wai_sem(TMO_FEVR) woken ONLY by CPU 0's cross-CPU signal (sgi_taken proof).
- CHECK 5 both falsifiers load-bearing (the credibility headline): PASS. `-DSMP_NO_SEC_TIMER` 5/5 RED
  (half i woke=0, delta=0 — CPU 0 demonstrably cannot wake it). `-DSMP_NO_XWAKE` 5/5 RED with the
  DISCRIMINATING signature — half (i) intact (woke=1) while half (ii) specifically dead (sem_woke=0).
  NOTE: the implementer's own verification CAUGHT that NO_XWAKE was originally VACUOUS (the idle wfe woke
  on the incidental sev from the caller's bkl_release + saw the still-published schedtsk) and fixed it by
  suppressing BOTH the publish and the SGI; the auditor independently confirmed the fix genuinely bites.
- CHECK 6 deadlock/race: PASS (reasoned + empirical). The `.Lsmp_idle` BKL-handoff (a blocking task
  leaves its CPU owning the BKL; idle drops it before wfe + re-acquires to the per-CPU-saved depth on
  wake) cannot deadlock a cross-CPU waker; clause-2b (idle ctxtsk==NULL does not async-switch) is ordered
  after the §5.4 BKL guard. smp3 [smp-no-deadlock] PASS with clause-2b present; both deadlock falsifiers RED.
- CHECK 7 no regression: smp0/1/2/3(async+deadlock)/4(onemind crown 0x2856a99b)/mc2 all PASS.
- NIT (does NOT block; pre-existing + ledgered): a `-smp 8` boot flake (GICD_TYPER intermittently fails to
  detect 8 cpus) reproduces on the BASE commit too — it is QEMU-10.1.0 8-CPU PSCI bringup timing
  instability, orthogonal to this slice (which touches neither detection nor bringup); -smp 2/4 are rock
  solid. `knl_taskindp` global concurrent-tick window resolved this slice via design option (b) — the cert
  disables CPU 1's CNTP after half (i) so the two CPUs' task-indep brackets provably never overlap;
  per-CPU-izing the global is ledgered to ②.3.
LEDGER: row CLOSED. A secondary CPU now runs the full WAIT machinery — its own timer ticks + cross-CPU
semaphore wakes — so it is a genuine peer scheduler, not just a compute slave. The crown survived the
first edit to default-linked shared core (zero-token macro discipline; cpu_support.o + task.o cmp-clean).
Both falsifiers bite (the implementer caught + fixed a vacuous one before the audit). HONEST: directed
single-target wake only (general affinity/migration = ②.3); QEMU green ≠ hardware green (RPi3 BCM2837
per-core timer/mailbox is a [live] follow-up); the -smp 8 environmental flake is pre-existing. Remaining
②: ②.3 (finer locks + knl_taskindp per-CPU + affinity/migration), hosted-port SMP, RPi3 [live].

## compat [migrate-forward] R3_WP — the first migration-chain slice — commit ac9ed78e (impl 6a656d33+6219771b, base da5e3f8d→cherry-picked to ad252ac7)
The first REAL slice of the compat/evolution migration chain (the ark surviving its OWN version changes):
a v1 R3-weight blob (`R3_WP`) carrying a TAUGHT FACT is migrated forward to the v2 format and the fact
STILL ANSWERS — losslessly. "Deathless" requires upgrade-without-loss; this proves it for one state axis.
Independent clean-room auditor verdict: **MERGEABLE** (tried to falsify, could not).
- THE MECHANISM: a `R3_WP_MIGRATE_STEP` registry `r3_wp_steps[]` of pairwise `vN→v{N+1}` functions
  (`r3_incontext.c`); the one shipped step `r3_wp_migrate_v1_v2` adds a trailing header field `merge_epoch`
  (header 112→120B, both `_Static_assert`-pinned) + shifts the float payload forward back-to-front (no
  clobber). `r3_wp_migrate_chain`: blob already at CURRENT → ZERO steps (fast path unchanged);
  future/too-old/missing-step → refuse + print, NEVER silent corruption; the production load path
  re-validates magic/r_np/vocab AND the payload sha256 AFTER migration (a bad migration is caught + refused).
  Generalizes the already-LIVE LM_SELF_ENTRY dual-width walker. Mind-FORMAT changes still migrate by
  re-education (Path E), per the decision; this is the FLAT-BLOB axis.
- CHECK 1 CROWN byte-identity (decisive): PASS. R3_WP is HOSTED-ONLY — all of it behind `#ifdef
  _TK_HOSTED_LIBC_`, which the bare-metal Makefiles never define (grep -c = 0,0). Auditor, from a
  `git clean -fdx` state (the stale-build trap is real — leftover -DSMP_SELFTEST objects once gave a wrong
  sha): default `.text` = `755a20fa…` base==HEAD (cmp byte-identical) AND `r3_incontext.o` = `3edc0d07…`
  base==HEAD (cmp byte-identical) — the +325 lines emit ZERO bare-metal `.text`. Crown re-derived on the
  cherry-picked trunk (ac9ed78e) by the commander: `755a20fa…`. one-mind cert PASS `0x2856a99b` 3× stable.
- CHECK 3 [migrate-forward] PASS + NON-VACUITY (3/3): taught 88.3% (chance 1.6%) → wipe to a fixed seed
  0.0% (the fact does NOT survive on its own) → chain runs (header.version 1→2, merge_epoch=0) → payload
  integrity MATCH → post-migrate 88.3% (lossless). The auditor READ the cert: the v1-width blob is wiped
  then ONLY the migration chain runs before the v2-offset reader — the migration is provably the SOLE cause
  of recovery, not a passive path.
- CHECK 4 FALSIFIER load-bearing (3/3): `-DR3WP_SKIP_MIGRATE` → the v1 blob is read at the v2 width →
  merge_epoch=garbage (4 float bytes misread) → payload integrity MISMATCH → fact LOST (0.0%) → FAIL.
- CHECK 6 no regression: `[persist-identity]` PASS; `[persist-mind]`/`[persist-mind-stale]` FAIL
  IDENTICALLY on base da5e3f8d AND HEAD (DMN consolidation doesn't fire within `mind wait 120` under
  PRoot) — pre-existing environmental, NOT a regression (auditor ran both). r3 test 94.7%, handoff PASS,
  cert symbol absent from the default hosted binary.
LEDGER: row CLOSED. The ark can now carry a taught mind forward across a weight-blob format version,
losslessly + with a sha256-validated refuse-on-bad-migration safety net — the first concrete tread of
"survive your own upgrade." Crown untouched (hosted-only, byte-identical base/HEAD). Both certs
load-bearing. HONEST: ONE flat-blob axis only — the Self-lineage hash-chain leg, arkfs deep-version-gap
(its log clean-rejects on version → reject-not-migrate), [signed-ota-gate], and [no-fleet-split] are the
named LATER slices (compat-migration-chain-plan.md). Harmless note: the R3_WP_VER_MIN too-old branch is
currently unreachable (dead-safe code).

## device-capacity mind-sizing (DEVFIT-1) — merge commit 66201a25 (impl a6aa7c2c) + comment-nit fix 17b78c3e
mk_pino's explicit "measure the device + auto-fit the mind": each node MEASURES its RAM+cores at boot and
AUTO-FITS its student tier (S/M/L) — the SMP-AUTODETECT cores-half's mind-sizing sibling. The S/M/L tier
machinery already existed; this supplies the missing `tier = tier_of(ram,cores)` boot-time caller.
Independent clean-room auditor verdict: **MERGEABLE-WITH-NITS** (one comment-accuracy nit, no functional
defect; FALSIFIED, every decisive gate held).
- CHECK 1 CROWN byte-identity (decisive): PASS. `student.c` diff EMPTY (untouched); from `git clean -fdx`,
  bare-metal default `.text` == `755a20fa…` (re-derived on the merged trunk by the commander). ALL tier-pick
  symbols (`st_init_device`/`dev_capacity*`/`tier_of`/`st_arena_bytes_for_tier`/`st_fleet_expert_target`)
  == 0 in `kernel.elf` — student.c/student_shell.c/dev_capacity.c are HOSTED-ONLY (student_stub.o on bare
  metal), so the crown is structurally safe.
- CHECK 2 M/L UNMOVED + S PINNED-AND-ASSERTED: PASS. SS-6 --machine: S=`0a5bf44c131b5439`,
  M=`63e8de333e995913` (unmoved), L=`67f2434f50e791b6` (unmoved). S is now production-selectable so its
  forward-hash is PINNED + ASSERTED (`student_devfit_test.c:292` `CHECK(hS==PIN_S)`); the tier_forward_hash
  recipe is byte-identical to SS-6 → the S pin is independently reproduced by the pre-existing SS-6 harness.
- CHECK 3 [device-fit] 14/14 (3× stable): PASS. RAM is the bottleneck (512MB/8core→S not L); thresholds are
  COMPUTED from each tier's arena cost (n_params*4, ×20 headroom), not magic; cores-only fallback on a
  low-trust bare-metal RAM constant; alloc-fail L→M→S step-down.
- CHECK 4 FALSIFIER load-bearing (3×): PASS. `-DDEVFIT_IGNORE_MEASURE` (hardcode L, no step-down) under
  `ulimit -v 256MB` on the 512MB profile → OOM → RED; production (measure+step-down) → tier=S, no OOM. The
  auditor confirmed it OOMs even at a 128MB cap where M (29MB) WOULD fit — proving the step-down is the
  disabled mechanism, non-vacuous.
- CHECK 5 SS-4 reconciliation (open-risk #7): PASS. `st_fleet_expert_target(N,tier)` =
  `min(cap_experts_of(N), ST_E_<tier>)` — exhaustively probed over caps {-5…1,000,000}: S clamps at 2,
  M at 4, L at 8, NEVER 16, never < K_min, never > the tier ceiling. HONEST: there is NO production
  `st_grow_experts(cap_experts_of(N))` caller yet — the clamp is a seam-closer (not over-claimed).
- CHECK 6 no regression: SS-1/2/3/5/6, SS-4 [expert-growth-preserves]+[grow-cohort] PASS; hosted linux
  links clean; KV byte-identical base==HEAD by construction (compiles only the untouched student.c).
- THE NIT (fixed at merge, 17b78c3e): a code comment said "no-fixture → selects M, default fleet
  byte-identical" — INACCURATE on a capable host (the CI host's 10.9GB/8core correctly boots L; that IS the
  intended measure+auto-fit behavior). The comment over-stated byte-identity; tightened to "modest host→M,
  capable→L, tiny→S; the invariant is each tier's pinned hash + the untouched R3 crown." Comment-only,
  hosted files → bare-metal `.text` unaffected (re-verified 755a20fa).
LEDGER: row CLOSED. The mind now sizes itself to its host — a 512MB device runs S, an 8GB device L — the
SAME binary, fail-closed (a lying RAM steps down rather than OOMs), reconciled with SS-4's fleet-growth via
a hard `min` to the device ceiling. Crown structurally safe (hosted-only); S pinned before becoming
selectable. HONEST: boot-time only (no runtime re-tiering — thermal/pressure → S_n + SS-4 shrink deferred);
device-sizing fragments the fleet into ≤3 student cohorts and the cross-cohort distillation LEARNING bridge
remains UNVERIFIED (this slice = tier-pick + cohort isolation + R3-crown share, NOT solved distillation);
bare-metal real-RAM (FDT parse) deferred (ships M via a build constant + cores-only fallback);
Android totalMem wiring deferred.

## compat [signed-ota-gate] — merge commit 63032e06 (impl 09e3062a+63032e06, base 60b56611)
The OTA trust gate (compat-migration-chain-plan.md §4): a node accepts an update artifact IFF it is
correctly signed AND a legal version successor — no downgrade, no body-swap, no impersonation. The second
compat slice (after [migrate-forward]); together they let the ark ship updates without being hijacked.
Independent clean-room auditor verdict: **MERGEABLE-WITH-NITS** (a SECURITY gate, audited maximally
adversarially — 9 forge attempts, none succeeded).
- THE GATE: a 4-gate AND in `compat_ota_accept` (`compat_ota.c`) — the SHIPPED `sign_manifest_verify` 3
  gates (recomputed artifact_id + Ed25519 against an ADOPTED key + allowlist) PLUS gate 4 (artifact_ver >
  running_ver). The version lives INSIDE the signed body `{artifact_id||artifact_ver}` (sign.c:140-143),
  so a downgrade or relabel breaks gate 1-3. sign.c/ed25519.c REUSED VERBATIM (not in the diff).
- CHECK 1 CROWN byte-identity + verbatim reuse: PASS. `grep -c compat_ota` bare-metal Makefiles = 0,0
  (hosted-only); clean `.text` = `755a20fa…` (reproduced on a 2nd clean build; re-derived on the merged
  trunk by the commander); compat_ota.o absent from the bare-metal link; sign.c/ed25519.c UNTOUCHED.
- CHECK 2 cert PASS, every refuse at the RIGHT gate (3×): ACCEPT good v2-over-v1; REJECT tampered body
  (gate1-3), non-adopted key (gate1-3), downgrade v1-over-v1 + v0<v1 (gate4), relabel v1→v99 no-resign
  (gate1-3). CHECK 3 FALSIFIER load-bearing: `-DOTA_SKIP_VERIFY` → all 5 malicious artifacts ACCEPT → FAIL.
- CHECK 4 ADVERSARIAL FORGE (the core security question): PASS — could NOT forge an accept. A standalone
  harness (real compat_ota.o/sign.o/ed25519.o), 9 forge attempts + 2 legit controls: the version compare
  is `U4 > U4` (unsigned, NO arithmetic → no integer wrap reachable); artifact_ver is cryptographically in
  the signed body (relabel→UINT_MAX, body-swap→v2, and impersonation-with-evil-key ALL die at gate1-3);
  both legit controls (genuine v2; legitimately-signed UINT_MAX) correctly ACCEPT (gate not over-tight).
- CHECK 5 no regression: shipped `sign test` (roundtrip/selflayer/unit/keyrotation/live/genome) ALL PASS —
  the 3-gate verify unweakened; cert symbol absent from the default hosted binary. CHECK 6 human-identity
  boundary HELD: the gate reads ONLY artifact/key fields — NO author/handle/identity/profile check crept
  in (the ark never verifies humans; signing is code/weights PROVENANCE only).
- NITS (non-blocking): (1) `boot/linux_x86_64/Makefile` has NO `EXTRA_CFLAGS` support so it can drive NO
  cert (the cert+falsifier can't run there) — but this is PRE-EXISTING (base 60b56611 = 0; it hits
  R3WP_MIGRATE_CERT identically), the production gate IS in the default x86_64 binary + is ABI-independent,
  and the cert is fully exercisable on aarch64. Follow-up: thread EXTRA_CFLAGS into the x86_64 Makefile for
  cross-ABI cert parity. (2) `legal_successor` collapses to the `>` check — a documented decision (every
  migration step is +1 ⇒ any strictly-greater version is chain-reachable; the named predicate is the hook
  for a future per-axis step cap), matches design §4.3.
LEDGER: row CLOSED. The ark refuses a tampered / downgraded / impersonated update — provably unforgeable
(version cryptographically in the signed body; unsigned compare with no wrap), reusing the shipped signing
verbatim, with the human-identity boundary intact. HONEST: this is the ACCEPT gate only — artifact
DELIVERY/transport (mesh small-update / KLOAD deep-update) is NOT wired, key REVOCATION (CRL/fleet
broadcast) is deferred (A/B rollback is the only recovery). Remaining compat: Self-lineage hash-chain leg,
arkfs deep-version-gap, [no-fleet-split], + the x86_64-Makefile cert-parity follow-up.

## compat [selflineage-migrate] — Self-lineage v1->v2 migration-chain leg — merge 2026-06-23 (impl 6b85d22e, base ac59eb43)
- Auditor: independent focused auditor, 2026-06-23. Did NOT write the code. Verdict: **MERGEABLE** (6/6 adversarial checks).
- THE LEG IS REAL: formalizes the EXISTING dual-width v1(116B)->v2(148B) LM_SELF_ENTRY walker as an explicit
  lm_self_steps[] migration chain (mirrors the shipped r3_wp_steps[]). The HARD part — hash-chain preservation —
  solved by plan §3.3 option 1: COHERENT whole-chain migration genesis->head, re-linking each migrated entry's
  prev_entry to its migrated predecessor's NEW v2 content-id. cert [selflineage-migrate] re-walks the FULL migrated
  chain via the PRODUCTION self_walk (not a private walker) → verifies=yes len=4/4; narrative preserved (seq/self_id/
  age_ms/eng_digest/model_ver == v1, human_ref all-zero); head readback ok.
- FALSIFIER -DLMSELF_SKIP_MIGRATE skips ONLY the re-link → re-walk verifies=no len=1/4 (head's prev_entry points at
  the old v1 id, never stored as v2 → self_walk rejects). migrate/narrative/readback all stay yes in falsifier mode →
  the re-link is the SOLE load-bearing gate. Non-vacuous.
- Multi-step composition refuses future/below-floor/missing-step (never silent misread). No-regression: [self-continuity]/
  [self-tamperevident]/[self-ownerless] PASS; pure append at EOF, production dual-width walker byte-untouched.
- CROWN: hosted-only (#ifdef _TK_HOSTED_LIBC_); bare-metal aarch64 lm_self.o has ZERO migration symbols; .text
  755a20fae2d9b7415045193ea8287623dbeb906963609a63dce8a19c8a130513 byte-identical (auditor + commander re-derived on merged trunk).
LEDGER: row CLOSED. The Self-lineage hash chain survives a format bump — migrated coherently, verified end-to-end by
the unmodified production walker, falsifiably. Second compat migration-chain leg after R3_WP.

## x86_64 cross-ABI cert parity — EXTRA_CFLAGS knob + compat verb — merge 2026-06-23 (impl 343466b4, base 35abbfcc)
- Auditor: independent focused auditor, 2026-06-23. Did NOT write the code. Verdict: **MERGEABLE** (no defect at any severity).
- THE GAP WAS REAL: trunk boot/linux_x86_64/Makefile had 0 EXTRA_CFLAGS + x86_64 usermain.c had 0 compat verb, so the
  x86_64 hosted build could NOT compile OR run ANY build-time cert (cross-ABI cert parity was not real). FIX: (1) Makefile
  EXTRA_CFLAGS ?= threaded CFLAGS->KERNEL_CFLAGS (reaches r3_incontext.o/compat_ota.o/usermain.o; LLM_CFLAGS excluded —
  matches aarch64 exactly, no parity hole); (2) the gated `compat test`/`compat test ota` verb mirrored faithfully from
  arch/linux/aarch64/usermain.c.
- VERIFIED END-TO-END on x86_64 (NATIVE ThinkPad by commander AND qemu-x86_64 by implementer, agreeing): [migrate-forward]
  PASS (post-migrate masked acc 88.3%); falsifier -DR3WP_SKIP_MIGRATE → payload MISMATCH (wrong-width read), acc 0.0% →
  FAIL (non-vacuous); bonus -DOTA_GATE_CERT → [signed-ota-gate] PASS. Default build INERT: nm proves usermain.o has no
  reference to r3_migrate_forward_test without the define (#if gate load-bearing); the verb only prints a rebuild hint.
- CROWN: hosted-x86_64-only (Makefile + arch/linux/x86_64/usermain.c); bare-metal .text 755a20fa byte-identical.
- HONEST: the gated-cert surface of the two usermains is now identical (both expose R3WP_MIGRATE_CERT + OTA_GATE_CERT);
  no cert the aarch64 usermain drives is missing from x86_64. (Discharges the long-standing "x86_64 can't drive ANY cert"
  follow-up flagged by the R3_WP and device-capacity rows.)
LEDGER: row CLOSED. x86_64 hosted now compiles AND runs build-time certs end-to-end — cross-ABI cert parity is REAL.

## N-2c [live] — CLOSED on the real ThinkPad host — merge 2026-06-23 (impl 1b0eeba4, base 35abbfcc)
- THE ONE genuinely-open [live] row (carried OPEN since 2026-06-21) is now CLOSED with a real-host PASS.
- COMMANDER real-host run (ThinkPad x86_64, native — sandbox cannot, PRoot kills the children): 3 OS
  processes (A + supernode S + B) + ./relay. VERDICT PASS: MAIN `delivered: via_super=1
  payload=BYTE-IDENTICAL` (`via_super_cnt=1 acks=1 retx=0`); falsifier-a (no supernode) → DIRECT
  via_super=0; falsifier-b (elected S killed) → fail-closed DIRECT, no loss.
- ROOT CAUSE (found on the real host, NOT the 2-day-old audit's "harness timing" guess — PRESEND
  11s→30s did NOT fix it): the relay overlay is a BROADCAST medium (net_relay_send only emits
  REL_BROADCAST); udp_send drops on the FIRST-time ARP miss (netstack.c:101); snf_send was a SINGLE
  fire-and-forget so A's first SNF_FWD to S was silently lost. ss6_live survives the identical race
  via SL_RETRIES 3×200ms; snf_send's own comment admitted "ACK/retry is a deferred hardening."
- FIX (Lane C, mirrors ss6_live): A retransmits SNF_FWD 3×200ms keyed by a new SNF_PKT.seq
  (repurposes _pad2, wire 524 B UNCHANGED, now pinned by _Static_assert at integration), blocking on
  a per-send semaphore until the END-TO-END SNF_ACK (B→S→A over warm ARP); timeout → DIRECT (no loss);
  B dedups (origin,seq) for EXACTLY-ONCE. A delivery-time self-report (REAL byte compare vs SNF_PROBE)
  makes the verdict survive the node's [moe] console spam (the old harness's interactive `snf recv`
  was starved). Harness: id-mapping fix (my_supernode=1 internal, snf send 2) + a real convergence wait.
- Auditor (independent, 2026-06-23): MERGEABLE-WITH-NITS, 8/8 adversarial axes PASS — crown 755a20fa
  EXACT (supernode.c hosted-only, absent from bare-metal link), wire 524 B (compile-time proof, seq at
  off 10), exactly-once (storm-bounded ≤3≤8-ring), fail-closed (tk_del_sem on every path, DIRECT on all
  timeout/dead-S), self-report HONEST (no delivery→no line→harness FAILs), in-proc cert 20/20 ×2 +
  sabotage-RED (non-vacuous), default byte-unchanged (benign delta: nonzero seq stamp + harmless DIRECT
  ACK no-op). NIT (now fixed at integration): added _Static_assert pinning sizeof(SNF_PKT)==524.
- HONEST: the [live] forward reaches the snf transport path; it ran 3-process on ONE host (single-host
  floor — same as the federation/ss6 [live] rows). DEBUG ENV value re-proven: it found the real
  transport gap (broadcast/ARP/no-retry) that the in-proc cert (socket stubbed) structurally could not.
LEDGER: row CLOSED. A packet routes A→B THROUGH the elected supernode over the real ./relay transport,
byte-identical, exactly-once, fail-closed — N-2c is real on hardware. NAT hole-punch (N-3) + seed
bootstrap (N-4) remain the Thread-N queue.

## compat [no-fleet-split] — SWIM cross-version membership cert — merge 2026-06-23 (impl d78eaeb8, base 0c200555)
- Design-harden first (in-proc-feasibility + 4-leg falsifiable spec), then impl, then independent audit. Verdict: **MERGEABLE**.
- THE CERT IS REAL + falsifiable: `swim_nofleetsplit_self_test` drives the REAL `swim_rx()` (swim.c:1401/1430/1447; the
  version gate lives at swim.c:418 INSIDE swim_rx, ABOVE gossip_apply — a gossip_apply-only cert would be blind to it).
  4 legs: [split-membership-crosses] (vN+1 additive `capability` packet, version==SWIM_VERSION → P ALIVE);
  [split-additive-crosses] (the capability byte propagates → region_is_super_capable(P)); [split-partition-on-bump]
  (THE falsifier: version==SWIM_VERSION+1 → hard-dropped → P NOT ALIVE → fleet partitions); [split-degrade-not-drop]
  (version==SWIM_VERSION, capability==0 OLD emitter still marks P ALIVE — the "生存情報は必ず通す" lifeline).
- SABOTAGE (auditor, independent): delete the `pkt->version != SWIM_VERSION` clause at swim.c:418 → ONLY leg 2 goes RED
  ("breaking-version peer wrongly ALIVE"), other 3 stay green; revert (git diff empty) → all 4 green. Load-bearing,
  uniquely keyed to the single `version` byte (leg 2 differs from leg 1 by exactly that byte; P reset to UNKNOWN
  immediately before the bumped delivery → not vacuous). Twice-runnable + side-effect-free (save/restore mirrors cap-gossip).
- No-regression: [cap-gossip]/[teacher-gossip]/[region-super 8/8]/[region-teacher 6/6] PASS. Flag -DNOFLEETSPLIT_CERT,
  verb `compat test wire` on BOTH usermains (x86_64 4 legs PASS under qemu — cross-ABI). CROWN: cert gated
  (NOFLEETSPLIT_CERT && _TK_HOSTED_LIBC_); bare-metal aarch64 .text 755a20fa byte-identical (auditor + commander re-derived).
- HONEST: does NOT retire compatibility.md §7 (replica.c:282 still hard-drops on version — reject→degrade is a separate
  slice; THIS proves the SWIM membership lifeline degrades correctly + the partition falsifier shows WHY §7 is
  release-blocking); does NOT prove a 2-OS-process teach→answer ([live] deferred, needs a pinned-vN binary); downgrade-auth
  out of scope. NITS (non-blocking): x86_64 usermain lacked the `self` verb (PRE-EXISTING — LMSELF was aarch64-only;
  being closed in the stacked arkfs slice); teacher-capable bit not save/restored (matches cap-gossip convention).
LEDGER: row CLOSED. The SWIM membership/gossip layer keeps a vN and a v{N+1} additive-change node in ONE fleet and
partitions on a breaking bump — "no fleet split" is now a falsifiable in-proc property.

## compat [arkfs-version-gap] — crash-safe log reject+reformat — merge 2026-06-23 (impl 5bade35f, base d78eaeb8) — COMPAT THREAD COMPLETE
- Design-harden (resolved migrate-vs-reject) → impl → independent audit. Verdict: **MERGEABLE** (7/7 adversarial axes).
- THE DECISION (resolved, grounded in arkfs.c): arkfs is a CRASH-SAFE APPEND-ONLY LOG; v1 ships clean REJECT+REFORMAT on a
  format-version mismatch, NOT in-place log migration (transcoding a foreign record stream would multiply the crash-window;
  the codebase already comments the bumps "clean reject, not a mis-mount"). The mind SURVIVES via the Self-lineage (truly
  migrated by [selflineage-migrate]) + Path-E re-education — arkfs holds durable p-fs backing bytes, NOT identity (the
  load-bearing coupling). Closes today's gap: version-mismatch used to reject-and-DISABLE durable storage for the run;
  now reject→REFORMAT→boot clean.
- POLICY in the hosted caller pfs_ark.c (`pfs_ark_mount_or_reformat`): valid-but-foreign super → no replay → ark_format
  (epoch bump so stale records can't replay) → re-mount → ONE honest emit ("durable bytes reset; identity survives via
  Self-lineage + re-education"); true corruption / native image → unchanged reject-and-disable / plain mount. No path
  reformats a native or recoverable image (peek checks both super copies) — auditor found no data-loss bug.
- CERT [arkfs-version-gap] (new hosted-only TU compat_arkfs_gap.c, RAM bdev): cure PASS for BOTH +1 (future) and -1
  (fossil) version sub-cases; NEGATIVE-SPACE load-bearing (writes a distinguishing payload, asserts !ark_block_has + 
  ARK_E_NOTFOUND after reformat — the foreign payload is provably GONE). Falsifier -DARKFS_GAP_SKIP_VERCHECK → foreign super
  validates as native → foreign log REPLAYED → "old payload survived!" → RED (sabotage is exactly the version-gate bypass).
- CROWN: arkfs.c IS bare-metal, so ALL changes hosted-gated — super_valid falsifier behind ARKFS_GAP_SKIP_VERCHECK &&
  _TK_HOSTED_LIBC_ with #else preserving the bare-metal line VERBATIM; ark_super_version_peek under _TK_HOSTED_LIBC_;
  compat_arkfs_gap.c in NEITHER bare-metal Makefile. bare-metal aarch64 .text 755a20fa EXACT + x86 base==HEAD byte-identical
  (auditor + commander, twice). No-regression: native arkfs CRUD/dedup/crash-consistency suite (sample 25) PASS; inherited
  [no-fleet-split] PASS. Also folded in the x86_64 `compat test self` parity (was aarch64-only — [selflineage-migrate] now
  runs on x86_64 too under qemu).
- HONEST: NOT in-place migration; on-disk arkfs bytes ARE lost across a version gap (only SILENT loss prevented — the honest
  deathless bound); survival DEPENDS on the Self-lineage being intact; single-node in-proc; the gap is SIMULATED by patching
  the superblock version int + crc (the gap is exactly the compared int at arkfs.c:561), not a 2-binary harness.
LEDGER: row CLOSED. ***The compat migration-chain thread is COMPLETE***: flat blobs ([migrate-forward] R3_WP +
[selflineage-migrate] Self-lineage) truly migrate; the SWIM wire ([no-fleet-split]) keeps mixed-version nodes in one fleet;
the signed-OTA gate ([signed-ota-gate]) refuses tampered/downgraded updates; the crash-safe log (arkfs) cleanly
reject+reformats — every path is honest, falsifiable, never silently corrupts, and the bare-metal crown 755a20fa never moved.

## teacher SELF-ELECTION fix (a [live]-exposed production bug) — merge 69a69387 (impl c81628a0, base 6b68f142)
- Found by GOING [live]: the commander ran the new cradle-live [live] harness on the real ThinkPad and the CURE arm
  FAILED at the first hurdle — "T never emitted (not elected teacher)". Root-caused on the host: `region_teacher()`/
  `region_supernode()` consult only the `teacher_capable[]`/`super_capable[]` TABLE; a node's OWN entry was set ONLY by
  the gossip-APPLY path for RECEIVED gossip (swim.c ~285/308). The self-beacon ORIGINATION sites (swim.c ~298 self-
  refutation, ~638 periodic) only ENQUEUED the capability for broadcast (`gossip_add`->`gq[]`) and NEVER self-applied it.
  So a teacher made its PEERS elect it, but `region_teacher()==self` was never true on the teacher ITSELF -> the
  `cradle_teach_emit` gate never fired live -> live teacher-convergence was structurally broken. THE IN-PROC [cradle-teach]
  CERT MASKED IT by calling region_set_teacher_capable(SELF) directly. (N-2c/supernode never hit this: the SENDER elects
  the supernode; a node never needed to self-recognize. cradle is the FIRST mechanism that requires self-recognition.)
  This is exactly the 2026-06-20 harsh-review prediction made real: the in-proc "safe half" hid a real bug the [live] half
  exposes.
- FIX: `SELF_APPLY_OWN_CAPABILITY()` (region_set_{super,teacher}_capable(self, {cap,teacher}_self())) after BOTH self-
  origination gossip_adds — the only two (swim_init does no gossip_add). region_teacher() still picks the LOWEST-id capable
  member, so single-teacher-per-region holds (no split-brain). Regression cert `nodes selfelect`: PASS (self-elects after
  self-beacon); falsifier -DSELFELECT_SKIP nulls the self-apply -> region_teacher()!=self -> FAIL (reproduces the exact bug,
  load-bearing). Hooked via a hosted-only `selfelect_force_teacher` flag (cert's GGUF stand-in, save/restored, production
  teacher_self() honesty intact).
- CROWN: ALL new code #ifdef _TK_HOSTED_LIBC_; bare-metal swim.o has ZERO selfelect/self_apply symbols (the macro is
  do{}while(0) bare-metal); bare-metal aarch64 .text 755a20fa byte-identical + x86 base==HEAD (auditor + commander).
  No-regress: [swim-incarn]/[cap-gossip]/[teacher-gossip]/[region-super 8/0]/[region-teacher 6/0]/[no-fleet-split]/
  [cradle-teach] all PASS. Independent audit: MERGEABLE (6/6 axes).
LEDGER: row CLOSED. A teacher-capable node now SELF-RECOGNIZES its role, so the cradle teacher-gate fires on the real
teacher. The bug a future regression would reintroduce is pinned by `nodes selfelect` + its falsifier.

## cradle-live [cradle-teach] [live] — the mind learns across the wire — CLOSED locally (formal VERDICT green host-independent; real-ThinkPad re-confirm still open)
- The DEFERRED [live] row of T-fix-b (multi-process teacher-convergence over ./relay) — its harness
  `samples/11_distributed/run_cradle_live.sh` (3 procs: teacher T + student S + witness W + relay) + the hosted observability
  verbs (`cradle probe`/`off`/`emit-scramble`) + the cert-scoped PKERNEL_TEACHER_CERT teacher path SHIPPED on 69a69387
  (in-sandbox: builds clean, `bash -n` sound, `cradle probe` works, crown 755a20fa byte-identical, the in-proc [cradle-teach]
  cert still PASS). The harness has 4 arms: CURE (T emits a deterministic held-probe lesson -> S pulls it over the relay ->
  consolidates -> the never-trained probe loss drops below chance) + teaching-OFF + scrambled-bytes + teacher-death.
- STATUS (2026-06-25): the self-election re-run drove the chain further and EXPOSED TWO MORE real, in-proc-masked bugs —
  both now FIXED + independently audited MERGEABLE + integrated (crown 755a20fa re-derived byte-identical on the integrated
  trunk). See the two detail rows immediately below (T-fix-c lesson-format; DMN-stack). The single-node + in-proc layers of
  the flagship ("a teacher composes a trainable lesson, it crosses the wire, and a fresh student's autonomous DMN sleep
  consolidates it and learns") are now CERTIFIED. The L3 multi-node CURE break is now ALSO root-caused + fixed + audited
  MERGEABLE (see the L3 detail row below). THE CORE FLAGSHIP IS PROVEN: a fresh student S pulls a relay-delivered lesson and
  its AUTONOMOUS DMN sleep consolidates it — the held probe drops 5.5915 -> 2.6025 (~2.99 nats below chance), generalization
  intact. The harness's FORMAL multi-node VERDICT now ALSO prints green host-independently — the autoprobe-cert row below
  replaced the forced `baby 16` (which starved the 180s cap on this cooperative-single-core PRoot host) with an assertion on the
  student's own AUTONOMOUS DMN idle probe (the production mechanism). Independently audited MERGEABLE (commit 6d491756) — see the
  autoprobe-cert detail row immediately below.
- NEXT (commander): a genuine real-NAT/real-ThinkPad re-confirm once the machine is reachable (the earlier `baby 16` harness
  loaded it and it went SSH-unreachable mid-run; mk_pino to clean stray procs) — this is now a re-confirm, not a gate; the local
  formal VERDICT is green and the fix correctness was already CLOSED.

## SS-3 [live] transport — BLOCKED — the deferred row's "transport ships" premise is FALSE (no code change; finding recorded) — 2026-06-26
- Scoping the SS-3 [live] row (peer↔peer cohort-merge convergence over ./relay) to dispatch it as the next [live] win uncovered —
  and the commander empirically VERIFIED — that the transport it claims to defer to has NEVER been functional:
  - `gl_student_publish`/`gl_student_fetch` (arch/common/gossip_learn.c:202/228) save ONE NAMED p-fs ref per 4096-B chunk
    (`st/<node>/<c>` via pfs_dag_save), but the named-ref table is `PFS_REF_MAX=16` (arch/common/include/pfs_dag.h:62). The
    smallest student tier (S: n_params≈164,416) is a 1,973,036-B blob = ceil/4096 = 482 chunks + 1 header = 483 DISTINCT names.
    `pfs_dag_save` does `if (!cur) cur = ref_alloc(); if (!cur) return PFS_E_FULL;` → the 17th distinct name fails, so
    `gl_student_publish` returns -1 at chunk ~16. M tier = ~5,578 chunks. There is NO tier/env knob that fits (S is the floor;
    PFS_BLOCK_MAX=4096 is a hard per-block cap → a 1-chunk student is impossible).
  - Both functions have ZERO callers (declared-only in gossip_learn.h:140/148; the `baby merge` verb is purely in-process and
    never touches the relay). They are exercised by NO cert. `GL_ST_MAXCHUNK=8192` (gossip_learn.h:133) advertises 512× the
    actual 16-slot ref capacity — a contradiction never reconciled because the row was deferred before anyone called the code.
  - Even with a bigger ref table, the `pfs/ref` gossip plane carries `PFSD_REF_PER_PKT=3` names per `PFSD_BEACON_MS=800` beacon
    (~128s to advertise S-tier names, ~25min for M) and the P1 content-announce is a single clobberable LATEST_ONLY slot that
    loses discovery under concurrent multi-block puts (same CLASS as the cradle L3 race, far more severe: one 1,280-B body there
    vs hundreds-to-thousands of 4 KB blocks here). The whole p-fs name/replication plane is sized for ~16 small named objects.
- CROWN UNAFFECTED (no code changed): SS-3's merge math (`st_merge_cohort` student.c:1925, `st_blob_tier_ok` :1850) is shipped,
  certified in-proc (tests/llm/run_ss3.sh), student-only (never gl_merge/rw[]) — that half is real. Only the DISTRIBUTED
  transport is dead. This is the harsh-review's "shipped the safe (in-proc) half, the load-bearing distributed half is dead code"
  finding made concrete and falsifiable.
LEDGER: row OPEN — a REAL gap, honestly logged (not silently left as a green "deferred"). SS-3 [live] requires a C transport
REDESIGN first: one named manifest ref + content-addressed chunks via pfs_repl_put/pfs_repl_want (bypassing the nameplane), a
chunked/indirect manifest (482 ids ×32B ≈ 15 KB > one block), an in-proc publish→fetch→st_merge_cohort round-trip cert BEFORE any
[live] harness; gossip_learn.c is BARE-METAL-linked (boot/x86 + boot/aarch64) so the redesign is crown-sensitive (keep bare metal
building, do not perturb the G22 gl_pfs_publish/gl_merge mesh, re-derive .text 755a20fa). Design-doc-first per project norm —
✅ DONE: `docs/architecture/student-blob-transport.md`. STEPS 1+2 NOW SHIPPED + AUDITED (next row); only step-3 [live] remains.

## SS-3 student-blob transport (steps 1+2) — content-addressed transport + in-proc cert — merge e655539d (impl 43b2ad3f, base 6606ae1a) — CLOSED
- The fix for the BLOCKED transport above. Per the hardened design doc: `gl_student_publish`/`gl_student_fetch` rewritten to
  store each 4 KB weight chunk as a content-addressed block (`pfs_repl_put`, NO name), index them in a 2-level tree (leaf blocks
  ≤127 ids, one root), publish a small descriptor under ONE named ref `st/<node>` — collapsing 483 names → 1; the fetcher reads
  the 1 ref → descriptor → index and pulls chunks by EXPLICIT WINDOWED `pfs_repl_want` (fail-closed, never truncate). Step 3 (the
  [live] 3-proc relay harness) intentionally NOT built — deferred (prioritization vs federation F1).
- REAL + falsifiable (INDEPENDENT audit MERGEABLE, separate auditor re-derived everything): in-proc `tests/llm/run_ss3_blob.sh`
  12/12 PASS — a 482-chunk depth-2 S-tier blob round-trips `memcmp==0` into a SEPARATE buffer filled only by the index-walking
  fetch (not aliased), then feeds a real `st_merge_cohort` (merge-of-transported == merge-of-direct BYTES, merged loss ≤ worse
  parent, peer-symmetric). FALSIFIER PROVEN LOAD-BEARING: the auditor DISARMED the dropped-chunk (drop→no-drop) and the cert went
  10/2 FAIL exit 1 — so the RED is genuinely caused by the missing chunk, fail-closed leaves `out` at the 0xAB sentinel.
- CROWN (DECISIVE, independently re-derived by the auditor AND by the commander on integrated trunk): aarch64 .text
  `755a20fa…0513` and x86 .text `4064d8a9…0413` BYTE-IDENTICAL base==head. Mechanism (not luck): ALL new code + ALL new static
  scratch under `#ifdef _TK_HOSTED_LIBC_`; the bare-metal `#else` `gl_student_*` bodies are byte-for-byte trunk; `nm` shows ZERO
  new bare-metal symbols (gl_st_idx/gl_st_desc/gl_st_leafids/gl_student_test_drop_chunk present only in hosted boots).
  pfs_repl.c/pfs_dag.c/pfs_block.c/student.c UNTOUCHED; G22 gl_merge/gl_pfs_publish/_fetch byte-unchanged. The cert link's
  `--gc-sections` is the TEST link only — NO kernel boot uses it (proven by the exact crown match). All 4 boots build; the hosted
  kernel carries the new transport.
- FINDING (audit-confirmed, recorded in the design doc §2.4, NOT a blocker): the in-memory P0 store is `PFS_MAX_BLOCKS=64`
  (pfs_block.h:29) but an S blob is 482 blocks → a memory-only receiver drops blocks at #65 (`pfs_put`→PFS_E_FULL). The cert
  legitimately mounts the eviction-capable ARK durable backend (RAM=64-slot cache + ARK log fall-through, all in the UNTOUCHED
  pfs_block.c); the fetcher probes presence with `pfs_get(id,0,0)` (ARK-aware), NOT `pfs_has` (RAM-only). LOAD-BEARING for
  step-3: every [live] receiver of a multi-MB blob must mount ARK or it silently drops chunks.
- HONEST BOUND: the windowed-want retry/yield path is only TRIVIALLY exercised in SOLO (all blocks resident at pass 0); real
  packet-loss / multi-node convergence + the want-storm risk are the deferred step-3 [live] row. M-tier (5577 chunks, depth-2) is
  structurally supported but not run (cert is S-only).
LEDGER: row CLOSED (steps 1+2 — the transport is real and certified, crown untouched). The dead-code transport is now a working,
falsifiable, content-addressed blob mover. Remaining: the step-3 [live] harness (with the §2.4 ARK requirement).

## cradle-live autoprobe-cert — the formal multi-node VERDICT goes green host-independent — commit 6d491756 (base 2e495a8c) — CLOSED
- With L1+L2+L3 the mind PROVABLY learns over the wire, but the harness's overall VERDICT still printed OPEN: the cure/scramble/
  death arms forced an explicit `baby 16` (16 sync sleep rounds over a ~247MB student) that does not finish inside the 180s cap
  on this cooperative-single-core PRoot host — the post-probe was starved, a host-speed artifact, NOT a fix failure. FIX (shell-
  only, harness): the cure/scramble/death arms now certify on the student's OWN AUTONOMOUS DMN idle probe (the production sleep
  mechanism) instead of the forced sync rounds — read the `[cradle-live] ring_len=.. probe_loss=..` line the `cradle probe` verb
  emits as the DMN consolidates on its own. SCRAM_BUDGET=8 (= 2× cure's ~4-cycle convergence) lets the scramble arm genuinely
  exercise the DMN on the junk before asserting it stayed at chance; reduced from a larger budget that re-introduced post-probe
  starvation.
- REAL + falsifiable (independent audit MERGEABLE — the auditor refuted the masking concern at SOURCE and witnessed its own clean
  GREEN first-try): CURE pre 5.5915 -> post 2.6025 (ring 1279, DMN drove it at idle cycle 4); OFF ring 0 / 5.5452 (exact chance);
  SCRAMBLE ring 1280 (full 8-cycle budget, NOT vacuous) / 5.8184 >= chance; DEATH post-kill 2.6025 (mind survives T's death).
  VERDICT: `PASS held probe 5.5915->2.6025 over the wire; teaching-OFF / scrambled stayed at chance; the mind survived the
  teacher's death`, DONE_RC=0. FALLBACK SOUNDNESS (the decisive audit gate): when the dedicated POST-probe MARK window is empty,
  s_post_probe takes `tail -1` of the `[cradle-live] ring_len=.. probe_loss=..` lines — which have a SINGLE emit site
  (student_shell.c:674, `cradle_live_probe`, called ONLY by the `cradle probe` verb; the autonomous DMN never emits it). Training
  is monotone (weights never un-train), so `tail -1` reads the MOST-trained state = the WORST case for the scramble `>=chance`
  assert => the fallback is conservative for every falsifier arm; it cannot mask a real failure (empirically: a starved scramble
  post comes back blank and FAILS honestly, it does not silently pass). Thresholds byte-identical to parent 2e495a8c
  (CHANCE=5.5452, CURE_FLOOR=0.5, cure/death flt_lt 5.0452, off/scramble flt_ge 5.0) — none weakened.
- CROWN: the commit touches ONLY samples/11_distributed/run_cradle_live.sh (+124/-38) — no .c/.h/.S/.mk/Makefile; bare-metal
  .text 755a20fa untouched by construction (shell-only).
- CROSS-ARCH REAL-HARDWARE RE-CONFIRM 2026-06-26 (on mk_pino's ThinkPad X1, x86_64, gcc 13.3 -O1 -ffp-contract=off): the SAME
  autoprobe harness PASS on a genuinely different ISA + real machine (not the aarch64 PRoot sandbox): CURE 5.5331->2.6848 (ring
  1279), OFF ring0/5.5452, SCRAMBLE ring1280/5.7788 (>=chance), DEATH 1.8537 post-kill; VERDICT PASS "teaching-OFF / scrambled
  stayed at chance; the mind survived the teacher's death". So the flagship is NOT aarch64/sandbox-specific — it reproduces on
  x86_64 real hardware. (Numbers differ slightly from aarch64 as expected: hosted float student math, per-ISA FP rounding even at
  -ffp-contract=off; the crown byte-identity invariant is on the BARE-METAL .text only, not the hosted student.)
- HONEST BOUND (updated): proven on a SINGLE machine, 3 procs + relay over loopback, on BOTH arches independently. A literal
  TWO-PHYSICAL-MACHINE simultaneous mesh was attempted (aarch64 phone-class sandbox student <-> x86_64 ThinkPad teacher+relay over
  the real LAN) and is BLOCKED BY THE SANDBOX'S ENVIRONMENT, not by p-kernel: the relay+teacher elected + emitted 16 canonical
  lessons on x86_64 and the aarch64 student booted + was born + targeted the relay, but the Termux/PRoot-on-Android device cannot
  send UDP to a LAN host (verified: TCP/SSH device->ThinkPad WORKS, UDP device->internet 8.8.8.8:53 WORKS, but UDP
  device->ThinkPad:7420/7421 receives NOTHING — Android LAN-UDP egress block). A UDP-over-TCP tunnel won't fix it cleanly (TCP
  doesn't preserve datagram boundaries -> corrupts the relay wire). A true two-machine run needs two UDP-LAN-capable hosts (e.g.
  two PCs) or a public/internet-reachable relay; recorded as a follow-up, NOT a gate.
LEDGER: row CLOSED. The formal multi-node VERDICT is green host-independently AND cross-arch on real hardware (aarch64 sandbox +
x86_64 ThinkPad). The flagship "the mind learns across the wire" is proven end-to-end: a fresh student pulls a relay-delivered
lesson, its autonomous DMN sleep consolidates it, the held probe drops below chance, teaching-OFF/scrambled stay at chance, and
the mind survives the teacher's death. Remaining (follow-up, not a gate): a literal two-UDP-capable-machine / real-NAT mesh.

## cradle-live L3 — pull the newest RESOLVABLE lesson seq (beacon-vs-ref race) — commit d8eb6710 (base ff5cee8b) — CLOSED
- Found by GOING [live] (a THIRD in-proc-masked bug, the harsh-review prediction a third time): with L1+L2 the lesson reached
  S and the DMN no longer crashed, but a fresh S still never ingested it. ROOT CAUSE (empirical, reproduces locally AND on the
  ThinkPad): the teacher re-emits ~12×, each `pfs_dag_save("ct/<t>/<seq>")` + a CRADLE_TEACH beacon that is KDDS_QOS_LATEST_ONLY
  (newest seq). The name->manifest ref BINDING propagates via the LOSSY+LAGGING region "pfs/ref" gossip, so the beacon outruns
  ref adoption — S adopts ct/1/1,2,3,5,6,7,8,9 while the beacon already points at ct/1/11, and `pfs_dag_read("ct/1/11")` returns
  NOTFOUND at `ref_find` (S lacks that binding) even though S HAS the (identical, deterministic) content+manifest blocks and the
  earlier resolvable refs. cradle only ever tried the single newest ref.
- FIX (crown-safe, cradle_net.c hosted-only — the ref-gossip reliability lives in pfs_dag.c/pfs_repl.c which are bare-metal/
  crown, deliberately NOT touched): in `cradle_poll_and_pull`, when the beacon's newest ref fails to resolve, scan seq DOWNWARD
  from the beacon seq to hw+1, rebuild `ct/<t>/<seq>` via the existing `ct_body_ref`, and ingest the NEWEST RESOLVABLE seq;
  advance `ct_seen_hw[org]` to the seq actually ingested (monotonic, dup-free). The fix also added `if (!cradle_get_enabled())
  return;` at the top of the pull — an HONEST falsifier restoration: Arm A (teaching OFF) previously passed only VACUOUSLY (the
  broken pull suppressed all ingests); with the working pull the OFF gate must be honored or `cradle off` would fill the ring.
- REAL + falsifiable (independent audit MERGEABLE): CURE — `fallback: pulled resolvable seq=6 (beacon seq=11)` -> `ingest len=
  1279 -> ring_len=1279`, held probe 5.5915 -> 2.6025 (~2.99 nats below chance), generalization intact (trainer [0,train_end)
  disjoint from probe [train_end,...)). OFF — legitimately RED: ring_len=0, probe 5.5452 (exact chance). SCRAMBLE — ring fills
  to 1280 (not vacuous) but idle probe 5.8184 >= chance (sequence, not bytes). DEATH — S ingests via fallback (seq=10) + still
  answers below chance after T dies. Fallback logic audited: bounded [hw+1, seq], picks highest resolvable, unsigned + entry
  guard => no underflow/infinite-loop, no re-ingest. CROWN: cradle_net.c absent from bare-metal Makefiles; .text 755a20fa
  RE-DERIVED byte-identical (auditor + commander, on the integrated trunk).
- HONEST BOUND: the harness's overall VERDICT prints OPEN because the cure/scramble/death arms' explicit `baby 16` times out the
  180s cap in PRoot (cooperative-scheduler starvation) so the FORMAL post-probe is starved — NOT a fix failure; the autonomous-
  DMN idle probe proves the learning. Formal multi-node green awaits a faster host or a harness that reads the autonomous probe.
LEDGER: row CLOSED (the fix). The mind learns a fact across the wire: a fresh student pulls a relay-delivered lesson despite a
lossy ref-gossip race and its autonomous sleep consolidates it. Remaining: the formal multi-node VERDICT on a faster host.

## cradle-live L1 — T-fix-c lesson-format — the teacher emits a TRAINABLE lesson — merge a8fce20f (base d3a204fb) — CLOSED
- Found by GOING [live] (the harsh-review prediction, a 3rd time): the self-election re-run got T emitting + S receiving the
  beacon + the p-fs body replicating to S, but S's lesson ring stayed 0. ROOT CAUSE (empirical, local reproduction beat two
  confident-but-wrong static-analysis hypotheses about region-scope/RTT): the live lesson body was ~115 bytes < CRADLE_MIN_LIVE
  (4*CRADLE_SEQLEN=128), so `cradle_lesson_ingest` REFUSED it ("too small to train"). The harness built a 1280-byte string but
  passed it as a `cradle emit <text>` shell argument, which the kernel shell line-buffer TRUNCATED to ~115 B. The in-proc cert
  never saw this — it composes its own 1280-byte lesson and trains on the 8 MB host stack.
- FIX (unify live == cert, one lesson format / one math): a new teacher verb `cradle emit-canon` composes the lesson IN-KERNEL
  via `cradle_compose_canon()` (a thin wrapper over the cert's static `ct_build_lesson`, CT_CERT_BUDGET=1280) — BYTE-IDENTICAL
  to the certified in-proc lesson — bypassing the line buffer. The harness CURE+DEATH arms now teach via `cradle emit-canon`.
- REAL + falsifiable (independent audit MERGEABLE): (1) ingest unit — 114 B/127 B REFUSED (rc=-1, ring 0), 1279 B ACCEPTED
  (ring 1279). (2) in-proc cert `run_cradle_teach.sh` (certified -O1 -ffp-contract=off, the IDENTICAL canonical bytes) PASS:
  MAIN held-probe drop 3.6862; Arm A (teaching-OFF) 0.61 RED; Arm B (scrambled) −0.32 i.e. probe ROSE — genuinely stays at
  chance, falsifier honest; Arm C (never-taught) 6.58 vs 1.84 RED; [cradle-nocentral] PASS. (3) over-the-wire: T emits the
  canonical 12×, S's beacon advertises body len=1279 (the old bug showed 115). Generalization INTACT: trainer windows [0,960)
  are byte-DISJOINT from the probed held region [960,1280); composer/trainer/probe all derive train_end=960 from
  cradle_corpus_len() via the SAME total*3/4 split.
- CROWN: cradle.c/student.h/usermain.c/harness all hosted-only (absent from bare-metal Makefiles; bare-metal links the WEAK
  student_stub.o); new symbols absent from any bare-metal build; .text 755a20fa byte-identical (auditor + commander). Arch
  parity: the emit-canon verb block byte-identical across aarch64/x86_64.
LEDGER: row CLOSED. A teacher now emits a trainable lesson the wire carries intact.

## cradle-live L2 — DMN-stack — the mind's SLEEP can actually run in-kernel — merge ba450a02 (base a8fce20f) — CLOSED
- Found by GOING [live] (same effort, one layer deeper): with L1 the lesson reached S, but a fresh student node still never
  learned. ROOT CAUSE: the student's training math (st_forward/st_backward, per-layer float[DMAX=256]/float[DFFMAX=512]
  scratch) OVERFLOWS an 8 KB task stack. The autonomous DMN sleep task (`dmn_task`, prio 13) — and `galaxy_task` (chat) and the
  init/shell task that runs `baby`/`dmn distill`/`cradle test` — were all created with only 8192 bytes, while the SS-6 responder
  was already deliberately given 256 KB for this EXACT reason (usermain.c "need real stack; NOT the deep init stack"). LATENT
  because every prior living-mind cert exercised the training MATH from a host binary (8 MB main stack), never the in-kernel
  8 KB task path. So the living-mind's core sleep-consolidation could not actually run on a real node — it crashed the moment
  it trained.
- FIX: raise the three hosted tasks that can reach st_forward to 262144 (256 KB) — `dmn_task` + `galaxy_task` (arch/linux/*/
  usermain.c) and the init/shell task (a HOSTED-ONLY INITTASK_STKSZ override in arch/linux/*/inittask_def.c, NOT the shared
  header). cradle_net_task / mind_net_task / mind_merge_task left unchanged (audited: they never call st_forward).
- REAL + falsifiable (independent audit MERGEABLE): DISEASE — on the parent (8 KB) `baby 2` and the autonomous idle path both
  EXIT 139 (SIGSEGV, garbage-addr stack-overflow signature). CURE — on the fix (256 KB) `baby 2` held-out loss 5.5409→2.9286
  (47% of chance), `dmn distill 2` "the baby LEARNED while it slept" exit 0. FLAGSHIP GATE — the auditor exercised the REAL
  prio-13 dmn_task: left a cured node idle >5s → `[dmn] -> IDLE` → `[dmn] sleep: distilled` on its own 256 KB stack, exit 0, 2
  consolidations / 30s, no fault; the parent crashes (139) on the identical scenario. So the AUTONOMOUS production sleep path
  (not just a manual verb) is genuinely fixed.
- CROWN: 4 hosted arch/linux files only; shared include/kernel/tkernel/inittask_def.h untouched (still 8 KB); bare-metal
  compiles its OWN arch/{aarch64,x86}/inittask_def.c + the weak no-op student_stub.o, so its 8 KB tasks never run st_forward;
  crown .text 755a20fa RE-DERIVED byte-identical (auditor + commander, on the integrated trunk). Arch parity byte-identical.
LEDGER: row CLOSED. The living-mind's DMN sleep-consolidation can now actually run on a real in-kernel node.

## N-4 seed bootstrap (PKERNEL_SEED) — merge 5041325a (impl 61dd9528 + 4d780961, base 95ec9da2)
- Design-harden (the existing ha_tick failover IS the try-next model; PKERNEL_RELAY is already a list) → impl → independent
  audit. Verdict: **MERGEABLE-WITH-NITS**. The last core Thread-N decentralization slice: a PKERNEL_SEED list demotes the
  relay to ONE optional seed (NOCENTRAL — no mandatory seed), not a hardcoded central dependency.
- REAL + falsifiable: pure `seed_select_next(cur, alive[], count)` (lowest-index-not-failed, wraps, -1 exhausted, bounded by
  count → no hang for ANY alive vector — auditor's exhaustive harness: count 0/1/MAX, all-dead/all-live/alternating). The
  PKERNEL_SEED branch in net_relay_init + the net_dispatch gate (`|| PKERNEL_SEED`) make it ENGAGE at runtime; seed-mode with
  no usable seed BOOTS SOLO (logs "no usable seed — running solo", exit 0, no hang) instead of the relay path's hard return-1.
- Cert [seed-bootstrap] 9/9 PASS (CURE: alive={0,0,1} → joins via the LIVE seed idx 2, NOT the dead head idx 0; Falsifier-A:
  all-dead → solo, bounded step-counter, no hang). Falsifier-B -DSEED_NO_ADVANCE → RESULT: FAIL (the nulled advance makes
  sb_join break immediately → chosen=-1 → load-bearing). BACK-COMPAT byte-identical (PKERNEL_RELAY-only path + hard return-1
  unchanged at source AND runtime; neither-env → loopback == base). Both net_relay.c/net_dispatch.c copies byte-identical
  except line 2. CROWN: hosted-only (absent from bare-metal Makefiles); bare-metal aarch64 .text 755a20fa byte-identical
  (auditor + commander). 6-file diff.
- HONEST: proves pure selection + solo-degrade + runtime-engage on a SINGLE node; NOT N-3 NAT; the TRUE cross-host seed
  convergence (A boots PKERNEL_SEED=B, unicasts B, joins over real sockets) is a DEFERRED [live] row (host-dependent);
  SWIM broadcast discovery untouched.
- NIT (pre-existing, NOT introduced — filed as a follow-up row below): an UNRESOLVABLE hostname
  (PKERNEL_SEED/RELAY=garbage:notaport) SIGSEGVs (exit 139) in the shared, unchanged resolve_relay/parse_relay_list; base
  95ec9da2 crashes identically. The seed wave merely routes a new env into the same code — does NOT block this merge.
LEDGER: row CLOSED (in-proc gate). The relay is now one optional seed, not a central requirement — a PKERNEL_SEED node
bootstraps + degrades safely. Cross-host join = the deferred [live] row.

## resolve_relay degrade-not-crash — CLOSED — merge d20a87c5 (impl c4b67845, base e8fe9557)
- Surfaced by the N-4 audit, FIXED + independently audited. `PKERNEL_RELAY/SEED=garbage:notaport` SIGSEGV'd (exit 139).
- DIAGNOSIS (sound, auditor-confirmed): the fault is INSIDE glibc `getaddrinfo()` called from a T-Kernel task (small stack /
  fixed mmap arena) on an NSS name — numeric IPs early-return via `inet_pton` and never reach it (so `127.0.0.1:1` was always
  fine). The single variable flipping the crash is whether getaddrinfo runs on a DNS name.
- FIX: `resolve_relay` no longer calls getaddrinfo — a non-dotted-quad host logs `unresolvable host '<h>' (numeric IP required
  in this build) — skipping` + returns -1; `parse_relay_list` drops the entry and `continue`s; an all-unresolvable list
  degrades (seed→solo, relay→loopback). **Numeric-IP-only is the chosen crash-safe contract** (DNS relay names unsupported in
  the hosted build; verified ALL in-tree real configs use IPs — only doc illustrations use a hostname).
- PROOF: cert `samples/11_distributed/run_resolve_crash.sh` 8/8 PASS; all hostile inputs (garbage / foo.bar.baz / a,b,c / ::: /
  mixed numeric+junk) → exit 0, no hang, mixed uses the numeric entry. Falsifier -DRESOLVE_NO_GUARD restores the getaddrinfo
  call → exit 139 on native aarch64 (load-bearing; qemu-x86_64 doesn't fault → the crash is environment-specific, honestly
  noted). Back-compat: valid numeric relay registers as base; N-4 [seed-bootstrap] 9/9. Crown 755a20fa byte-identical
  (hosted-only). Independent audit: MERGEABLE-WITH-NITS.
- NIT→follow-up (non-blocking, STRICTLY SAFER than base which SIGSEGV'd): 3 Android user-facing surfaces still advertise
  hostnames as acceptable — `android/app/.../pkernel_jni.c:120`, `PKernel.java:48`, the Kotlin relay-host EditText
  (`LogActivity.kt:60/98`). A phone user typing a DNS relay name now silently falls to loopback. Update those doc strings /
  numeric-validate the field. OPEN (small Android-boundary doc/UX row).
LEDGER: row CLOSED. A node handed a bad relay/seed hostname now degrades gracefully instead of crashing — "degrade not crash"
holds at the bootstrap boundary.

## N-3 NAT hole-punching (in-proc gate) — merge c125379f (impl adf9e6f9, base 4d3ccc75) — THREAD-N IN-PROC COMPLETE
- Design-harden (rendezvous = the already-elected supernode; cone-vs-symmetric is a pure function) -> impl -> independent
  audit. Verdict: MERGEABLE. The last open Thread-N item.
- REAL + falsifiable: pure np_classify (cone if a peer's two observed mappings share a port; symmetric if they differ;
  UNKNOWN on a NULL mapping), np_decide (PUNCH iff BOTH cone — full truth table audited, every non-(cone,cone) cell -> RELAY,
  fail-closed), np_broker_swap (hands each peer the OTHER's ep + the shared verdict). NP_REQ/NP_INFO/NP_PRB ride SNF_PORT via
  an EXTENDED snf_rx magic switch (NOT a second udp_bind -> avoids the documented port-steal trap); on punch-timeout OR
  symmetric OR no-broker -> the N-2c snf_send relay path (connectivity NEVER lost — auditor enumerated all 5 returns, no drop).
- Cert [nat-punch] 17 PASS / 0 FAIL (CURE: two cone peers -> broker swaps eps + verdict PUNCH, real production-path arm via
  np_rx/np_brokered() counter; FALSIFIER-A: symmetric -> RELAY; FALSIFIER-B: punch-timeout -> relay, byte-identical delivery,
  no loss; fail-closed edges). Sabotage -DNP_SABOTAGE_CLASSIFY -> 14 PASS / 3 FAIL, flipping EXACTLY the 3 classifier asserts
  (symmetric classify/decide/verdict) -> load-bearing. NO-REGRESSION: N-2c region fwd still 20 PASS; NP_PKT has its OWN
  _Static_assert(44) so the 524-B SNF_PKT contract is untouched; 4-target build clean.
- CROWN: supernode.c hosted-only (absent from bare-metal Makefiles); bare-metal aarch64 .text 755a20fa byte-identical
  (auditor + commander). 4-file diff (supernode.c/.h + both usermains, parity-wired region punch verb).
- HONEST: cone-NAT ONLY (symmetric stays relayed — a real bound, NOT a bug); proves the rendezvous PROTOCOL + classification +
  punch/relay DECISION, NOT real NAT traversal (a two-distinct-NAT topology = a DEFERRED [live] row, harder than N-2c's
  single-host one); NOCENTRAL (reuses the N-2 election as broker, no new authority).
LEDGER: row CLOSED (in-proc gate). ***Thread N's in-proc surface is COMPLETE***: N-0 node-id, N-1 LAN-direct, N-2/N-2b
supernode select + capability gossip, N-2c forward ([in-proc]+[live] on real hardware), N-3 NAT punch (in-proc), N-4 seed
bootstrap. The remaining Thread-N work is all real-host/real-NAT [live] rows (N-2c re-confirm done; cradle-live, N-3 two-NAT,
N-4 cross-host deferred to the ThinkPad).

## connect-anywhere SLICE 1 + SLICE 3 — commits 4f1eb07e (heartbeat) & 42ac0c54 (TCP fallback)
- Two waves, each: design (connect-anywhere.md) -> isolated-worktree implementer -> SEPARATE adversarial auditor.
  Commander integrated ONLY on PASS. 2026-06-27. Both implementers + both auditors were distinct subagents.
- SLICE 1 (unconditional relay heartbeat, 4f1eb07e): auditor independently re-derived BOTH crowns (aarch64
  755a20fa / x86 4064d8a9 — MATCH), confirmed 5-file hosted-only diff (net_relay.c + usermain.c twins +
  tests/run_heartbeat.sh). In-proc cert 3/3 PASS with a load-bearing FALSIFIER (heartbeat gated on admission ->
  emits 0 keepalives -> cert FAILs). Unconditionality verified by code-read: net_relay_heartbeat() depends only on
  sock_fd/cur_relay, NOT peers/drpc_my_node (the swim.c:603 admission gate was the "silent after 4 pkts" deadlock).
  KEEPALIVE_SEC 25->15 < NAT_TIMEOUT_FLOOR 30. Row CLOSED.
- SLICE 3 (plain-TCP relay fallback, 42ac0c54; recovered from WIP bb849747 after the FIRST implementer API-errored
  mid-integration — build was broken, TCP listener defined-but-unwired; a continuation implementer finished it):
  auditor independently re-derived BOTH crowns (MATCH), confirmed 9-file hosted/standalone diff (relay/relay.c +
  tcp_frame.h + test_relay.c, net_relay_tcp.c twins, net_dispatch.c twins, two hosted Makefiles — NO arch/common,
  NO bare-metal TU). `make test` 8/8: the 6 original UDP scenarios STILL green (UDP recvfrom path lifted VERBATIM
  into process_packet, byte-identical logic + rx_us stamp preserved), + [deframer] PASS with a TOOTHFUL falsifier
  (p1[0..1]=0x0005 so a prefix-less naive concat mis-splits -> recovered=0 -> FAILs on the missing boundary
  specifically, not an unrelated reason), + [tcp_roundtrip] PASS (real fork/exec ./relay; a real loopback TCP
  client + UDP client exchange A<->B BOTH directions through one shared node table). TCP framing [u16 BE len][v2
  pkt]: per-connection reassembly is static (not a task-stack local), bounded (flen>outcap -> drop conn;
  1416+2048 < 4096 cannot overflow), reaps on POLLHUP/POLLERR/orderly-close. HONEST BOUND (accurately disclosed,
  no overclaim): the kernel-side net_relay_tcp.c twins are COMPILE-verified only (opt-in PKERNEL_RELAY_TCP=1) —
  NOT exercised by a booted ./p-kernel against a live relay; no cert touches the kernel TCP symbols. Row CLOSED.
- SLICE 2 (public relay) is DEPLOYED (pkernel_relay container on mk_pino's helloidea.org home server, 7400/udp)
  with EXTERNAL reachability PROVEN over mobile (3/3 probes mobile 49.x -> home router 7400/udp fwd -> relay rx).
- HONEST FOLLOW-UPS (named, NOT gaps in these CLOSED claims): (a) live booted-kernel<->relay TCP join [live];
  (b) redeploy the public relay container with the TCP-capable relay.c + forward 7400/tcp; (c) kernel-twin
  net_relay_tcp_recv could check the reasm-push truncation return like its relay.c sibling (safe today by the size
  invariant); (d) 443/TLS-via-Caddy-SNI sub-slice (deferred — do NOT repoint the router's 443 off Caddy).
- SLICE 4 (automatic UDP<->TCP relay-transport fallback, 0b94f6a0; the FIRST concrete impl of the §4 runtime
  ladder, narrowed to the relay-transport axis: rung 3 relay-UDP <-> rung 4 relay-TCP to the SAME endpoint, NO
  manual PKERNEL_RELAY_TCP): design->implement->audit all SEPARATE agents. Auditor independently re-derived BOTH
  crowns from a fresh worktree (aarch64 755a20fa..., x86 4064d8a9... — MATCH, byte-identical: every TU is hosted),
  confirmed the 10-file diff is hosted-only (net_dispatch/net_relay/net_relay_tcp/usermain twins + 2 harnesses,
  ZERO arch/common, ZERO relay/relay.c source). "relay contact" predicate = the relay ECHOED our keepalive
  (relay-HA pong, relay.c:813 UDP / :792 TCP framed): UDP sets relay_contacted in ha_mark_rx (post-HMAC inbound),
  TCP sets it on the first complete framed inbound pop (a bare connect() is deliberately NOT contact). Happy-eyeballs
  windows UDP_HEADSTART_MS=300 / RACE_CONNECT_TMO_MS=700 / ADOPT_DEADLINE_MS=2500 (concurrent, NOT serialized);
  re-eval 30s with UDP_RECOVER_K_S=20 hysteresis (no flap). IN-PROC cert `autoxport test` 8/8 PASS on BOTH arches
  (CASE A UDP-open -> adopted=udp, tcp_init=0 no wasted connect; CASE B UDP-blocked+TCP-open -> auto-adopts TCP,
  adopt_ms<=2500; +3 hysteresis asserts). FALSIFIER -DAUTOXPORT_NOFALLBACK (#ifndef-excludes the whole TCP race ->
  UDP-only) prints RESULT: FAIL; auditor ALSO ran a LIVE SABOTAGE (no-op'd xp_tcp_init in the cure build -> CASE B
  flipped to FAIL -> reverted) proving the cert is not a tautology. Mock seam fully #ifdef AUTOXPORT_CERT-gated
  (crown match is the proof it's inert in production). NO regression: relay make test 8/8, slice-3 run_relay_tcp_live
  4/4 (the new selector now sits under the default UDP path and still meshes over UDP, 0 TCP regs). Adversarial:
  relay-down boot does NOT wedge — selector bounds at ~2.55s then "no relay contact -> provisional relay-udp, no
  mesh" and boot continues. LIVE harness run_relay_autofallback_live.sh SKIPs cleanly here (unshare -rn EINVAL in
  PRoot); its PASS path asserts registered.*(tcp)>=2 + alive=2 + adopted relay-tcp + udp_regs==0, with TEETH 1
  (block both -> no join) + TEETH 2 (AUTOFALLBACK=0 -> no join). Row CLOSED.
- HONEST BOUND on Slice 4 (named, NOT a gap in the CLOSED claim): v1 covers ONLY the relay-transport axis; direct-P2P
  rungs 1/2 (LAN broadcast, N-3 cone-NAT punch) and the TLS/443 flavour of rung 4 remain later slices. The live
  netns+iptables UDP-blocked join was NOT executed here (PRoot lacks unshare -rn) — it is a deferred [live] row.
- N-2d (measured-capability SUPERNODE AUTO-PROMOTION, 23811db7; Skype-style dynamic supernodes — a hosted
  evaluator measures self-fitness and AUTO-PROMOTES the supernode bit, replacing the explicit-opt-in-only model):
  design->implement->audit ALL SEPARATE agents. CROWN-SAFE BY CONSTRUCTION (the load-bearing claim): the new
  evaluator lives in a NEW hosted TU arch/linux/<arch>/supernode_autopromote.c, called from net_heartbeat_task
  (5s), and its ONLY shared-state write is the EXISTING setter region_set_super_capable(self,...) -> existing
  cap_self() gossip -> existing NOCENTRAL min-id election -> existing N-2c forwarding, ALL unchanged. Auditor
  re-derived BOTH crowns from a fresh worktree (aarch64 755a20fa..., x86 4064d8a9... -- MATCH, byte-identical),
  confirmed the 11-file diff is hosted + host-binary relay.c only (ZERO arch/common/* edits: region.c/swim.c/
  supernode.c/drpc.c/interocept.c/degrade.c untouched; teacher bit untouched). Fitness = relay_contacted AND
  (refl_is_public OR refl_classify==CONE) AND !=SYMMETRIC AND !metered AND stress<200 AND degrade<max; dwell
  60s promote / 30s demote (asymmetric anti-flap); SYMMETRIC = hard block independent of dwell; PKERNEL_SUPERNODE=1
  force > measured. Measurement needs a NEW reflexive signal: a STUN-like REFL1 echo added to relay/relay.c (the
  relay appends the observed src ip:port to a keepalive echo, magic-gated "REF1", reusing the PRB1 probe-stamp
  append path -> non-REFL echoes BYTE-IDENTICAL, relay make test 8/8). net_relay.c (twins) captures reflexive_ip/
  port per relay vantage point and exposes net_relay_reflexive_classify (CONE iff same external port across >=2
  vantage points, reimplementing supernode.c:768 cert-pinned) / _public (refl IP == net_my_ip); <2 vantage points
  -> UNKNOWN -> fail-closed no-promote. REFL1 SECURITY (audited, no hole): the trailer is OUTSIDE the HMAC but
  net_relay_recv strips it BEFORE compute_mac (capture strictly POST-auth); trailer is a FIXED 6-byte const (no
  attacker length field), bounds-checked, index-safe; worst case = a spoofed reflexive addr -> a wrong promotion
  bounded fail-closed by min-id select (blast radius 1) + snf_send DEAD->DIRECT + 30s demote. IN-PROC cert
  `autopromote test` 6/6 PASS both arches (A hold-then-promote@60s / B symmetric-never / C1 good-blip-no-promote /
  C2 bad-blip-no-demote / D env-force). FALSIFIER -DSAP_NO_SYMBLOCK (#ifndef-removes the !=SYMMETRIC clause; case B
  feeds public=1+SYMMETRIC so the block is the SOLE gate) prints RESULT: FAIL; auditor ALSO ran an independent
  sabotage (SAP_PROMOTE_K_S 60->1 -> cert flipped to FAIL -> reverted). NO regression: relay 8/8, slice-3
  run_relay_tcp_live PASS, slice-4 run_relay_autofallback_live SKIP-clean, non-REFL recv path byte-identical.
  Row CLOSED.
- HONEST BOUND on N-2d (named, NOT a gap in the CLOSED claim): (a) still min-id among the auto-capable, NOT
  best-RTT/bandwidth (bandwidth unmeasured); (b) teacher bit unchanged (GGUF-gated, out of scope); (c) bare-metal
  unchanged (no env, no hosted net -> no auto-promote, by construction); (d) the live netns+iptables/socat join was
  NOT executed here (PRoot lacks unshare -rn) -- deferred [live] row; (e) PRODUCTION needs the public relay
  REDEPLOYED with the REFL1 echo for real reflexive measurement (operational follow-up, like the Slice-3 TCP
  redeploy).
- CI-PARITY (Source-list parity, 2026-06-27): the check was chronically RED (predates this session) on 4 drifting
  basenames in the host Makefiles but MISSING from the Android CMake: compat_arkfs_gap.c + compat_ota.c [COMMON,
  pre-existing], net_relay_tcp.c [ARCH, Slice-3 last session], supernode_autopromote.c [ARCH, N-2d this session —
  the one entry THIS session added; an honest loose-end: when I added it to both host Makefiles I did not also add
  it to the Android CMake]. RESOLUTION (honest-green, NOT hidden): declared all four in check_parity.sh
  ALLOW_MK_ONLY as DOCUMENTED host-only-FOR-NOW exceptions (matching the existing ss6_live.c/supernode.c pattern)
  — the red becomes a green-WITH-a-TODO, a reviewed exception rather than silent drift. The sandbox has NO Android
  SDK/NDK so the APK build cannot be verified here; blindly adding the TUs to the CMake risks breaking the real
  Android compile. HIGH-PRIORITY TODO (a future session WITH the Android SDK): lock-step these into the CMake
  COMMON_SRC/ARCH_SRC and NDK-verify, so the features (TCP fallback, supernode auto-promotion, OTA/compat) actually
  reach phones — compat_ota.c/compat_arkfs_gap.c especially likely BELONG in the APK, not held back. Parity now
  GREEN locally (check_parity: OK, exit 0). HONEST BOUND: this greens the CHECK; it does NOT yet put the features
  in the APK (that needs the NDK pipeline).
- CI-HEALTH NOTE (2026-06-27): of the 5 red CI checks, 4 are NOT regressions from this session (confirmed: the same
  5 were red on 63b6a5f5, the pre-session tip). 3 are the live-3node tests (Collective learning / One mind Path W /
  Protect loop) that time out on the slow GitHub runner ("Formal multi-node VERDICT pending faster host") — NOT a
  code regression; the elegant fix is a self-hosted runner (mk_pino's ThinkPad) or per-test budget, NOT demoting
  them blindly (could hide a real regression — needs investigation first). 1 (UMP x86_64) fails inside the Galaxy
  observation-window cert (external-URL ref / no drpc_in SSE event / teach-not-ok / broken pipe) — a galaxy/WebView
  cert env issue, also pre-existing and unrelated to this session. PLAN for next session ("kicker CI" per mk_pino):
  (1) green the live-3node via a self-hosted runner; (2) fix-or-quarantine the galaxy cert with a documented reason;
  (3) THEN add high-signal strict gates — crown byte-identity as a BLOCKING job, every falsifiable in-proc cert
  (autoxport/autopromote/heartbeat/relay) wired as a required gate, and ASan/UBSan hosted builds to catch the
  recurring stack-overflow class. Principle: "strict" = never-let-green-break, NOT more-red-lines (avoid
  normalization-of-deviance).
- CI-LIVE3NODE-WINDOWS (v1 b9f84332 + v2 bf9cdac4, 2026-06-27): the 4 live-3-node jobs (protect-loop/
  collective-learn/shared-mind/one-mind) flaked on the self-hosted runner not from a code bug but from TIGHT
  multi-stage timing windows. SWIM death-detection ≈ 15-17s (SUSPECT_ROUNDS 2 + DEAD_ROUNDS 3 = 5 missed probes
  ~1.9s/round). v1 widened only the POST-KILL serve windows (10/20s->60s, 240->300s); CI then PROVED v1 INCOMPLETE
  — 27_protect failed at `node1 did not report the unit SAFE` BEFORE the kill, i.e. the unit never replicated to R
  replicas inside the tight 20s PRE-KILL window, so the survivor had nothing to serve (`[pfs] get: NOT FOUND`) and
  no post-kill window could help. v2 widened ALL the upstream convergence/replication/pre-kill windows too:
  cluster-FULL 30->60s, region size=3 20->60s, *** SAFE replicate 20->60s, the racing one-shot `protect stat`
  grep -> a 60s retry loop (predicate UNCHANGED), shared/one-mind `mind_net_task up` 30->60s, shared-arrival
  40->60s, teach-consolidated 20->60s, collective Phase-A 200->300s + solo_ceiling 30->60s; outer ci.yml timeouts
  + job timeout-minutes raised to contain them. ASSERTIONS BYTE-IDENTICAL across both passes (only patience widened
  / one-shot->retry; the "node dies but the network serves/remembers" property stays BLOCKING-gated). CROWN trivially
  safe (only samples/*.sh + ci.yml; zero .c/.h/.S — bare-metal .text byte-identical). The aarch64 PRoot sandbox
  cannot reproduce the x86_64 live-3node path, so the self-hosted pkernel-thinkpad CI run is the real verification
  (in flight). HONEST BOUND: no already-generous (>=60s) window failed in CI, so no real convergence bug is
  implicated — this was genuinely tight caps; IF v2's generous windows still flake, that is the signal it is
  fundamental multi-node fragility (then -> advisory with the deterministic-cert half kept blocking).
  CONTEXT: the self-hosted runner (isolated Docker, --network host, KVM) already greened 7 of 11 heavy jobs
  (ring3 + plural-protect/twolayer/parallel-infer/composite/ARK/survival-loop) that were pure contention/timeout
  flakes on GitHub's shared runners.
- FOLLOW-UP (pre-existing, NOT a regression; from the v1 audit): samples/41_shared_mind/run.sh's post-kill
  `wait_for [teach-consolidated] (PASS|FAIL)` is STALE-SATISFIABLE (a pre-kill line already matches -> wait returns
  immediately; the `grep 'ask "sun"' | tail -1` can read pre-kill data => latent FALSE-PASS). Fix = match a fresh
  post-kill-specific marker like 42_one_mind does. Does not block; tracked.
- STAGE-2 PROOF — `tests/x86/run_killchurn.sh` GOES RED ON AN UNFIXED TREE (2026-08-12,
  `docs/killchurn-cert-proof`, harness at master `468a1c67`). The wave-45 rule says a gate must be shown to
  FAIL before it is wired into CI; the previous `dproc churn` verb was never shown to fail, was a blank
  round on master by wave-45, and its "wire it into CI" follow-up died — 41 days of green. This entry is the
  falsification the new harness was missing. It does NOT wire anything into CI (that is Stage 3).
  METHOD: `git archive 74e23947` (= `339a66a2^`, the tree BEFORE the hardening was restored) into a scratch
  dir, then dropped master's `tests/x86/run_killchurn.sh` into it unmodified (sha256
  `f85d3491bead7be71979be4db1c315611f329499cd1f43eef59cb23d0915715f`, byte-identical in both arms). The
  harness's `$0`-derived `ROOT` claim is CONFIRMED tree-agnostic — zero harness edits were needed. `git
  archive 468a1c67` gave the matched FIXED arm. The two arms differ ONLY in the 6 hardening files
  (`kernel/mtkernel3/kernel/tkernel/{wait,timer,task,task_manage}.{c,h}`); the `arch/x86/shell.c` delta
  between them is comment-only and the rest is docs — a clean 2-arm matched control. Both arms were run
  INSIDE the `pkernel_gh_runner` container (agentName `thinkpad-pkernel`, 4 CPU / 15 GB) because the host
  has no `qemu-system-x86_64` and no i686 cross-gcc — i.e. **on the exact machine CI runs on**, so the
  timings below are CI timings. Every arm was gated on "no workflow run in_progress/queued AND no
  Runner.Worker" so nothing collided with CI.
  RAW NUMBERS (710 boots, same day, same harness, same host, arms interleaved):
    condition                          UNFIXED 74e23947        FIXED 468a1c67 (control)
    idle serial, N=40                  sigA  1/40   rc=1       sigA 0/40   rc=0
    idle serial, N=240                 sigA 17/240  rc=1       sigA 0/240  rc=0
    3 concurrent arms, N=25, SKIP_BUILD sigA  5/75   rc=1,1,1   sigA 0/75   rc=0,0,0
    POOLED                             sigA 23/355 (6.48%)     sigA 0/355
  Fisher two-sided p = 1.65e-07 pooled; p = 5.75e-06 on the idle-serial subset alone. **5 of 5 harness
  invocations on the unfixed tree exited 1; 5 of 5 on master exited 0.** The observed fault is the real
  one, not a classifier artifact: `err=0x00000002 CS=0x00000008 EIP=0x0015382E`, i.e.
  `knl_wait_release_tmout+0x11` inside the resolved window `[0x0015381d,0x00153866)`, emitted immediately
  after `[elf] task started (tid=15)` — the TCB slot REUSE, the historic signature, at the historic offset.
  MEASURED ANSWER TO THE HARNESS AUTHOR'S OPEN QUESTION ("whether signature A tracks load the way
  signature B appears to"): **it does not.** Signature A is 18/280 (6.43%) idle-serial vs 5/75 (6.67%)
  under 3-way concurrency — Fisher p = 1.00. The 3-parallel + `KILLCHURN_SKIP_BUILD=1` recipe was NOT
  needed to redden the unfixed tree; plain idle N=40 reddened on the first try. Signature B in this
  session is 11/560 idle vs 6/150 loaded (Fisher p = 0.22) — same DIRECTION as the author's 2.5%-vs-10.7%,
  still NOT significant, so the load effect on B remains unproven, and it is now clear it was never
  load-bearing for A. Signature B is also confirmed tree-independent here: 10/355 unfixed vs 7/355 fixed,
  p = 0.63 — consistent with the audited "untouched by the fix" and with leaving it non-fatal.
  **N=40 IS NOT ENOUGH FOR CI — RECOMMEND `KILLCHURN_N=150`.** The N=40 default was derived from the
  audited 8.0%/boot. My idle-serial measurement is 18/280 = 6.43%, Wilson 95% CI [4.10%, 9.93%] — the
  audited 8% sits inside it, but the CI's LOWER end is what a gate must survive, and at p = 4.10% a single
  N=40 run misses a live regression **18.7%** of the time. Miss probability (1-p)^N at the 95% lower bound
  / at the point estimate: N=40 18.7%/7.0%, N=80 3.5%/0.5%, N=100 1.5%/0.13%, N=120 0.65%/0.03%,
  N=150 0.19%/0.004%. Cost is not the constraint: this runner does 2.86-2.91 s per boot INCLUDING the
  clean build (240 boots + build = 687 s), so N=150 is ~7.8 min — well inside `ring3-survival`'s existing
  `timeout-minutes: 25`, and the self-hosted jobs are already chained serially so nothing else is displaced.
  **N=120 is the floor I would accept (0.65% miss); N=150 is what I recommend; N=40 would have been a
  1-in-5 blank round.** Harness stability over 710 boots: `incomplete=0` and `other=0` in every single run —
  no stall, no lock contention, no unclassified fault, so a red is a red.
  WHAT IS STILL NOT KNOWN (do not let Stage 3 paper over these): (1) the 6.43% idle rate is ONE host on ONE
  day — the GitHub-hosted `ubuntu-latest` runners are a different machine class and this gate has never run
  there, so the N recommendation is only justified for `[self-hosted, pkernel-thinkpad]`. (2) "idle" is a
  misnomer: the harness's own 0.2 s marker-polling loops drive host loadavg to ~4.5 on 4 cores, so the
  "idle" arm is not a quiescent machine — it is the harness's own steady state, which is at least the
  steady state CI would see. (3) an unrelated CPU-heavy `dotnet`/`dcbench` workload appeared on the host
  during part of the FIXED N=240 control (loadavg peaked ~15); that makes the green control CONSERVATIVE
  (more load, still 0/240), not weaker, but it means the two N=240 arms were not perfectly load-matched.
  (4) signature B's load dependence is still unresolved (p = 0.22 here, p ~ 0.05 for the author) and
  `KCC-WILDPC` still has no mechanism. (5) this proves the harness reddens on THIS unfixed tree; it does
  not prove it would redden on a FUTURE vendor-patch loss that deletes a different one of the 9 hardening
  classes — the gate watches one signature, not the class of regressions the `VENDOR-PATCH-LOSS` row names.
- CROWN RE-BLESS — IRQ stub SS reload, the `KCC-WILDPC` root fix (2026-09-03, `fix/irq-stub-ss-reload`,
  `b328011c`, fast-forwarded onto master). The bare-metal `.text` changes DELIBERATELY and on ONE target
  only: the fix is +12 lines of `boot/x86/isr.S` — `irq_call32_stub` now reloads SS, mirroring
  `exc_call32_stub` and `syscall_call32_stub`, which already did. **aarch64 is byte-identical and is NOT
  re-blessed** — `isr.S` is x86-only, and the aarch64 crown reproducing UNCHANGED across the two arms is
  itself a check that the arms differ only where intended. NEW dev crown `.text` sha256 (audit container
  gcc 13.3.0, `make clean` first, both arms built from the same container in one session; each arm's
  `isr.S` sha256-matched to its source tree before building):
    aarch64  c4e255f11941cd90b2c038bd69f21948be0e59dcc59416bfc72f7efe57c3fa5a   (UNCHANGED)
    x86      d71839d1ba36f8719e44725e1804039d6c767bd833b52a26f8cad1945ae0cde2   (was 4f6f0dc5…48623f13)
  `.text` 367430 → 367438 B (+8), re-measured with `size -A -x` at land time (0x59b46 → 0x59b4e), not
  carried over from the build log. Accounted for exactly: `irq_call32_stub` grows **0x0d → 0x18** (+11 —
  its extent measured to the next symbol `irq_back64_stub`, which is what sits between it and
  `syscall_call32_stub`), every later symbol shifts by exactly 11 (`irq_back64_stub` 0x100f40→0x100f4b,
  `syscall_isr` 0x100f4c→0x100f57, `syscall_call32_stub` 0x100f60→0x100f6b), and section alignment
  padding absorbs 3.
  HONEST BOUND ON THE REPRODUCTION: the previous crown (below) was made with CI-container gcc 13.2.0 and
  this one with 13.3.0. The master arm re-built here reproduced 4f6f0dc5… byte-identically, which is what
  licenses comparing the two — but that makes this a SAME-MAJOR-SERIES rebuild, **not an independent-
  toolchain reproduction**, and unlike the 2026-08-12 entry below it was reproduced ONCE, by one party.
  WHY: see the `KCC-WILDPC` row in `docs/architecture/gap-ledger.md` — on an interrupt taken from ring 3
  the CPU loads SS=null, so the 32-bit compat-mode kernel IRQ path ran on a stack split into a 16-bit and
  a 32-bit half. Evidence: signature B 3/300 master, 7/300 sham, 0/300 fixed (sham vs fix p=0.0151;
  **master vs fix alone p=0.2487 — underpowered, stated plainly**); and on `-DKCC_DIAG`, INCOMPLETE boots
  13/150 master vs 0/150 fixed, p=1.86e-04.
- CROWN RE-BLESS — KILL-CHURN hardening restored after the μT3.0 migration ate it (2026-08-12,
  `fix/kill-churn-restore`). The bare-metal `.text` changes DELIBERATELY: the fix lives in
  `kernel/mtkernel3/kernel/tkernel/{wait,timer,task,task_manage}.c`, which all 5 targets compile,
  and hosted-gating it behind `_TK_HOSTED_LIBC_` was NOT an option because the bug fires on
  bare-metal x86. This is the documented re-baseline the `crown-text-identity` gate's own procedure
  calls for; the gate is NOT weakened, and the drift is buried under a crown-neutral docs tip so
  master's TIP stays `.text`-identical to its parent. NEW dev crown `.text` sha256 (CI container
  gcc 13.2.0 — REPRODUCED byte-identical THREE times: the implementer's clean build, the independent
  auditor's own clean build, and the commander's re-verification after applying the audit
  corrections; `make -C boot/x86 clean` FIRST every time — a stale incremental build yields a
  different hash, the trap that bit the 3.0 re-bless):
    aarch64  c4e255f11941cd90b2c038bd69f21948be0e59dcc59416bfc72f7efe57c3fa5a   (was f23f719f…bcbc887)
    x86      4f6f0dc5c971ef5e4afeea2f7dd32176e2a76d3915e9ffc1d909702d48623f13   (was 7ab20ceb…31d932aa)
  WHY (the honest version): a cure that shipped 2026-06-14 with an independent audit and a 400/400
  matched control was deleted 18 days later by `f50c30a0`, and this project's own ledger said CURED
  for 41 days while the crash ran at ~8% of boots and CI reported green. Proof the restored guard is
  load-bearing — independent 3-arm audit, 674 boots: signature A master 18/225 → fixed 0/225
  (p=5.4e-6) → **sham (the fix minus only the 6-byte guard) 15/224**, statistically indistinguishable
  from master (p=0.72). See the KILL-CHURN-CRASH row and the OPEN `VENDOR-PATCH-LOSS` row in
  `docs/architecture/gap-ledger.md`.
- CROWN RE-BLESS — μT-Kernel 3.0 core migration (2026-07-03, PR #9 + #10, migration merge commit
  bbc0d216, FF-landed on master). The kernel core was INTENTIONALLY swapped micro T-Kernel 2.0 →
  μT-Kernel 3.0 (IEEE 2050-2018 / T-License 2.2), so the bare-metal `.text` LEGITIMATELY changes.
  This is the reviewed, documented re-baseline the `crown-text-identity` gate's own procedure calls
  for; the gate is NOT weakened. NEW dev crown `.text` sha256 (sandbox gcc 15.2.0, the per-wave audit
  anchor — REPRODUCED byte-identical by FOUR independent builds: Phase-A build-verify, Phase-C
  integration, Phase-D independent audit, and the commander's own clean build on master before push):
    aarch64  5e42f8532be1852d57f86506738946a21bce69048b9b1d4b988e282305e7fda0   (was 755a20fa…c8a130513)
    x86      a52c8701047ef639858b76ea3f55a8c216acfe12805d2fd268de2f6e4663660a   (was 4064d8a9…7ee0413)
  (NOTE: x86 must be built after `make -C boot/x86 clean` — a stale 2.0-core incremental build yields a
  DIFFERENT .text; the crown-identity CI job cleans, and so must any manual re-bless check.) The 3 build
  fixes carried in the merge (IMPORT knl_startup_hw ×2 hosted mains; #include "task.h" in the vendored
  timer.c; ring3 userland CORE_INC → mtkernel3 include set + -D_X86_PC_) are prototype/include/Makefile
  only → ZERO codegen change, so these crown values equal a properly-prototyped build. VERIFICATION:
  merge textually clean (arch/common/ — the whole distributed brain — untouched; pfs_repl.c byte-identical
  to pre-merge master with pathw reverted / Hunk A held); all 4 targets build canonically on gcc 15 (no
  -Wno-error); all boot to shell; x86 self-test 13/13; the 3.0 scheduler/time syscalls the brain relies on
  (tk_chg_pri / tk_rot_rdq(TPRI_RUN) / tk_dly_tsk / SCHED_RR tick / DI-EI safe-point shadow / SYSTIM
  {W hi;UW lo} 32-bit) are standard-correct with file:line evidence; the full ThinkPad live suite is GREEN
  on the 3.0 core (PR #10 run 28686440485: 19 success / 2 failure = crown drift [this, intentional] +
  smp-autodetect [advisory, pre-existing ubuntu qemu env]). Deletions (329) = old 2.0 core + unused
  rl78/h8300 MCU reference ports; grep of the deletion set for moe|dtr|dmn|swim|pfs|galaxy|ark|… = empty.
  FOLLOW-UPS (non-blocking, tracked): x86 interactive-shell banner still prints "micro T-Kernel 2.0"
  (boot banner correct); rl78/h8300 residue + dead poc_*.c prune; smp-autodetect -nic none fix;
  41_shared_mind residual pre-kill stale-wait; one_mind Path-W full transport fix (Hunk A held, low
  urgency — it PASSED on the ThinkPad).
- CROWN RE-BLESS — LM-12 belief revision (2026-07-04, feat/lm12-belief-revision merged to master). The
  living-mind slice `r3_fact_revise` compiles into `arch/common/r3_incontext.c` (a bare-metal TU), so
  the `.text` LEGITIMATELY moves — the same intentional-drift case as every LM slice. NEW dev crown
  `.text` sha256 (sandbox gcc 15.2.0; parent 5e42f853…/a52c8701… reproduced byte-identical FIRST for
  environment trust; new values reproduced by implementer, adversarial auditor, AND the commander's
  own clean build on merged master):
    aarch64  243f917bcbc660f7a38753c93c9998cc299b576130c60472d34bdb0aedbacbd3   (was 5e42f853…7fda0)
    x86      8e670a3c894dfacbde5e56a2cf26242ecb514c22b83b4070f1b3e6ae0bb151e5   (was a52c8701…63660a)
  VERIFICATION: 7/7 in-proc `[rev-*]` gates PASS (independently by implementer + commander + auditor);
  the two LOAD-BEARING falsifiers BITE (adversarial auditor patched the code: writing weights directly
  → `[rev-not-masked]` FAIL; shrinking the cure budget → `[rev-cured]` FAIL); D1 is an honest measured
  disease (BLOCK not the predicted 50/50 blend — the old engram dominates the replay union until
  superseded — gated on the load-bearing `share[vn]<75`, no bar lowered); no regression to the shared
  `[stream/lang/onemind/teach/wmerge/handoff]` gates; `dmn.c` byte-identical; no wire change; merge onto
  master clean (ci.yml sections non-overlapping with the follow-ups). FOLLOW-UP (audit finding, ledger):
  `[rev-stale-mouth]` in sample 46 does NOT yet exercise the Site-3 guard it claims to certify — node A
  revises via Site 1 (shell), whose `m_publish_teach` re-arms `mt_pub_last` and masks the guard; node B
  takes via Site 2 (no re-arm). The property (region doesn't revert post-revision) holds, but for the
  Site-1-re-arm reason, not the guard. The Site-3 guard is CORRECT defensive code (a test-teeth gap, not
  a mechanism bug). Fix in flight: make the LEARNER (B) the reviser so A must clear its stale mouth via
  the guard (guard-removed → re-infection → `[rev-stale-mouth]` FAIL).
- CROWN RE-BLESS — LM-13 graceful forgetting (2026-07-04, feat/lm13-forgetting). The living-mind slice
  turns the R3 fact-queue's FIFO eviction into min-EARNED-salience eviction (+ `r3_fact_touch`, one accrual
  site) in `arch/common/r3_incontext.c` and adds `EV_FORGET=19` to `arch/common/include/galaxy.h` — both
  bare-metal TUs, so the `.text` LEGITIMATELY moves (the same intentional-drift case as every LM slice).
  NEW dev crown `.text` sha256 (sandbox gcc 15.2.0; parent 243f917…/8e670a3… reproduced byte-identical
  FIRST for environment trust, then the new values reproduced by the implementer's clean build):
    aarch64  09fc5a9a36e0a7c68387134dcc8387b464857ece43619527665e5086b81fad73   (was 243f917b…edbacbd3)
    x86      55b425c9af86d93bb12724bbce10489a3a7e38c1e7ffe3e4f64adc39d5e26169   (was 8e670a3c…0bb151e5)
  VERIFICATION: 5/5 in-proc `[forget-*]` gates PASS (implementer clean build; independent audit pending).
  The LOAD-BEARING falsifier structure: cure and disease run the SAME arrivals/budgets/eval, the ONLY delta
  being 12 asks on f1 — so a rigged selector (protect-by-index/key/recency) reddens `[forget-unearned]`
  (evicted seq != f1) and accrual leaking into a read path reddens `[forget-onesite]` (s_touches != 12).
  Zero asks degenerates BYTE-IDENTICALLY to FIFO (evicted seq==1==oldest); 12 asks (clamped to R3_SAL_CAP=8)
  flip the victim to f2 and f1 survives 100.0% vs the evicted f1's 64.0% (a measured +36-pt gap, floor
  re-baselined to <70 from the measurement per IX.5 — NOT invented). HONEST design correction (the LM-12
  `[rev-blend]` pattern): a single SINGLETON new fact does NOT decay the evicted fact in its 10-round drain
  (measured: stays 100%); the honest disease driver is a WIDE f5 (8 bindings, keys 8..15) whose concentrated
  consolidation decays the unrehearsed evicted fact — and because it is ONE eviction, `[forget-noregress]`
  (f3,f4 held 100%) holds with the asks on f1 alone. No regression to the shared
  `[stream/rev/lang/onemind/wmerge/teach/handoff]` gates (full selftest 95/95 PASS, 0 FAIL on the aarch64
  hosted build); `dmn.c` byte-identical (empty diff); no wire change (`MT_TEACH_PKT` untouched); no new TU;
  `sizeof(R3_FACT)==24` `_Static_assert` still holds (the `salience` byte was reserved at 1 since LM-5).
  Doc: `docs/architecture/30-module/living-mind-lm13-forgetting.md`. NOT merged by the implementer — a
  SEPARATE audit + the commander's own clean-build reproduction of these crowns precede any merge.
- CROWN RE-BLESS (follow-up) — LM-13 HONESTY re-frame (2026-07-04, feat/lm13-forgetting). An adversarial
  audit found the accuracy narrative OVER-STATED the generic harm: the shipped f5 binds all 8 keys to the
  SAME output class (class 1, the substrate's adversarial bias sink), so the acc_f1 64.0% decay is a
  CONCENTRATION ARTIFACT, not the generic cost. Re-measured: all-8→class-1 (shipped) 64.0% / +36; all-8→one
  benign class (5) ~80.5% / +19.5; 8→8 DISTINCT classes (realistic multi-class fact) 100.0% ZERO decay / +0.0.
  The SELECTION claim (asking f1 → the selector evicts f2, not f1) is ROBUST across ALL constructions and is
  what the falsifiers bite — independent of any accuracy number. FRAMING-ONLY fix: the doc now headlines the
  SELECTION and discloses the auditor's table + the class-spread zero-decay caveat (§4); the cert's print
  strings + comments relabel the accuracy gates as "WORST-CASE single-adversarial-class interferer" (the
  `[forget-*]` tag strings + ALL gate LOGIC/thresholds UNCHANGED — CI greps them) and PRINT a class-spread
  zero-decay caveat so the runtime log carries the disclosure. The mechanism (`r3_fact_touch`, the eviction
  selector) is untouched; the 5/5 `[forget-*]` gates STILL PASS (framing-only). Because print-string LENGTHS
  changed and the caveat added `r_puts` calls, the bare-metal `.text` LEGITIMATELY moved AGAIN (cosmetic
  log-string drift; the executed math is unchanged — empirically: same-length string-content edits are
  crown-neutral, only string LENGTH / new calls move `.text`). NEW dev crown `.text` sha256 (sandbox gcc
  15.2.0; parent 09fc5a9a…/55b425c9… reproduced byte-identical FIRST for environment trust, then the new
  values reproduced by the implementer's clean build):
    aarch64  f51eb00efdde557a64ef8ee6de9ac1d4bde61b9d3ce712bd9febd58127f7cba5   (was 09fc5a9a…b81fad73)
    x86      a0bed501df50ff21ba7b3a29305e83af1b048af1622f59f86764d9ea8d741237   (was 55b425c9…d5e26169)
  NOT merged by the implementer — a SEPARATE audit + the commander's own clean-build reproduction of these
  crowns precede any merge.
- LM-13 LANDED on master 2026-07-04 via merge `972ba15e` (commander clean-build reproduced f51eb00e/a0bed501
  byte-identical on merged master). Audit VERDICT was GO-WITH-CARE: the selection mechanism (asking→evict a
  DIFFERENT fact) is the robust load-bearing core (3 falsifiers bite); the accuracy disease (64%) was honestly
  RE-FRAMED as a WORST-CASE single-adversarial-class interferer after the auditor showed a realistic
  class-spread f5 decays the evicted fact by 0.0 (living-mind-lm13-forgetting.md §4 + the runtime log now
  disclose this). Current master dev crown = aarch64 f51eb00e… / x86 a0bed501… This commit is crown-neutral
  (docs only) so master's tip stays crown-green; the intentional .text drift is at the merge `972ba15e`.
- CROWN RE-BLESS — LM-14 curiosity (2026-07-04, feat/lm14-curiosity merged to master). r3_want_note + the
  want→salience conversion in `arch/common/r3_incontext.c` (bare-metal TU) move `.text` → NEW dev crown
  (sandbox gcc 15.2.0; parent f51eb00e…/a0bed501… reproduced byte-identical first; new values reproduced by
  the commander AND the independent auditor):
    aarch64  be41bbf6b9f018b761ca1e1278878936d2693cf58ca845e520d46c3b6912ed78   (was f51eb00e…)
    x86      248633de008d0ff036597ab9f8a96f141334732f94f085e29dddddf20aaab019   (was a0bed501…)
  VERIFICATION: 7/7 [curio-*] gates PASS (commander + auditor); 3 falsifiers BITE (rig conversion →
  [curio-unwondered] FAIL; drop the share<75 qualifier → [curio-weightknown] FAIL; leak accrual into a read
  path → [curio-clean] FAIL). GENERICITY confirmed by the auditor (the LM-13 lesson applied): the load-bearing
  gates assert on evicted-`seq` ONLY (the selector never reads values), and re-laying-out the f5..f9 value
  classes THREE ways (all-1 / all-63 / heterogeneous) left the evicted-seq [1,2,3,4,5]→[1,2,3,4,6] INVARIANT —
  no same-class artifact, unlike LM-13. no-regress 95/95; dmn.c byte-identical; no wire change; merge clean.
  AUDIT-CAUGHT DEFECT (fixed at merge, NOT a mechanism bug): the CI leaf live-wiring leg (`mind ask leaf` →
  expect leaf to wonder) was positioned AFTER `mind wmerge`, which deterministically pollutes leaf's residual
  masked prior to 100% → leaf would not wonder → the blocking x86_64 selftest grep would fail. Fix: reordered
  the leaf probe BEFORE `mind wmerge` in the ci.yml selftest pipe; verified on native aarch64 (leaf reads 60.0%
  &lt; 75 there and wonders). The 7 [curio-*] gates + the mechanism were untouched. LM-14 landing completes the
  living-mind arc's learn → sleep → revise (LM-12) → forget (LM-13) → wonder (LM-14) sequence.
- CROWN RE-BLESS — LM-15 region pull-teach (2026-07-04, feat/lm15-pull-teach). LM-15 lets the mind ASK the
  region for keys it wonders about: a new region-scoped singleton topic `mind/want` (`MQ_WANT_PKT`, 24 B,
  keys-only — the F-LOCAL amendment: the want KEY crosses the wire, the accrued want LEVEL never does), a
  publish site (`mq_publish_wants`) + a poll/answer site (`mq_poll_wants`, engram-only, forwarding the
  original provenance) + a factored single wire-writer (`mt_wire_send`) + a 4-slot local-prov side-table
  (`mq_lprov`), all in `arch/common/r3_incontext.c` (bare-metal TU) with `MQ_WANT_PKT` + comment amendments in
  `arch/common/include/dtr.h` and a singleton-enumeration comment in `arch/common/include/kdds.h`. The
  bare-metal `.text` LEGITIMATELY moves → NEW dev crown (sandbox gcc 15.2.0; parent be41bbf6…/248633de…
  reproduced byte-identical FIRST for environment trust, then the new values reproduced by a second clean
  build):
    aarch64  3e20edbd6a6696de6b3cf3b9e7767d849f9520ad2fcf0863fffd75dc3db34b83   (was be41bbf6…)
    x86      b6a748daacdc0c2b16a8c80e5885fdf3f343964c0e0846ea5beb9082e9d11e10   (was 248633de…)
  WHY: LM-15 pull-teach adds the mind/want publish+answer machinery to a bare-metal TU. VERIFICATION (by the
  implementer): 4/4 `[pull-*]` in-binary gates PASS on native aarch64 hosted (`mind pull`); all FOUR targets
  build clean (boot/x86 + boot/aarch64 bare, boot/linux + boot/linux_x86_64 hosted; the only warnings are
  pre-existing — wmerge misleading-indentation, bare-metal unused-fn). `[pull-answer-src]` pins the
  engram-only scope cut RED-ably (a weight-known-but-EVICTED key refuses); `[pull-snapshot-honest]` proves the
  packet is byte-identical across a want-LEVEL change (F-LOCAL anti-leak). No new task, no new TU (no Android
  CMake parity risk); dmn.c byte-identical. NOT merged by the implementer — a SEPARATE audit + the commander's
  own clean-build reproduction of these crowns precede any merge (the audit-trail :1875 rule). LM-15 extends
  the living-mind arc: learn → sleep → revise → forget → wonder (LM-14) → ASK (LM-15).
  AUDIT (2026-07-04, SEPARATE agent, verdict MERGE-WITH-CORRECTIONS → integrated by commander):
    · CROWNS REPRODUCED independently by the auditor (clean gcc 15.2.0 build) AND by the commander:
      aarch64 3e20edbd… / x86 b6a748da…, byte-identical; parent be41bbf6…/248633de… also reproduced. Crown OK.
    · IN-BINARY GATES HAVE REAL TEETH: the auditor sabotaged each `[pull-*]` gate's own failure mode
      independently (leak a want-LEVEL byte → `[pull-snapshot-honest]` RED; bump want_seq every tick →
      `[pull-seq-gen]` RED; answer an unbound/evicted key → `[pull-answer-src]` RED; defeat dedup →
      `[pull-answered-once]` RED), all reverted. F-LOCAL holds (no magnitude byte on the wire); budget honest
      (KDDS_SINGLETON_TOPICS still 16, 8 eager / 15 worst-case, next-singleton-must-bump obligation recorded);
      genericity holds (dynamic key, relative salience); all-4-build zero new warnings.
    · LIVE CERT CONFOUND (empirically established, wave-45 same-harness negative control): `samples/47`'s cure
      leg does NOT isolate the pull's causal contribution. Because B is taught K LOCALLY, B's ordinary LM-7
      Path E teach-gossip carries the SAME (origin,seq) packet as the pull answer; A's `r3_want_take`
      conversion lands precious on WHICHEVER arrives. Stubbing BOTH `mt_wire_send(&mq_ans_pkt)` calls to no-ops
      (pull answer never reaches the wire) STILL passes all 9 `[pull-*]` gates — Path E alone delivers K. The
      pull-specific evidence is B's `answering want key` print (proves `mq_poll_wants` RAN + emitted), NOT that
      A CONSUMED the pull answer. So the live cert earns "the ask/answer mechanism runs over the real region +
      arrival is precious", NOT "the pull rescued a fact Path E dropped".
    · CORRECTIONS APPLIED by the commander (crown-neutral — docs/shell/CI comments only, no compiled code):
      struck the "PROVES the pull" / "A learns FROM the pull" overclaims in `living-mind-lm15-pullteach.md`
      §4.2 (added a HONESTY CAVEAT), `samples/47_pull_teach/run.sh` (header CONFOUND block + banner), and the
      `pull-teach-live` ci.yml comment; fixed the 0-based/1-based node-id error in the doc; hardened run.sh to
      always rebuild (the stale-binary trap the auditor hit — a manual run silently tested a pre-LM-15 binary).
    · FOLLOW-UP OBLIGATION (named, deferred): a live cert that TRULY isolates the pull must remove Path E's
      delivery of K to A — e.g. A outside / not-yet-in the region during B's K-teach window, then joins →
      wonders K → only the engram-driven pull can re-deliver. Teaching K then L on B is necessary-but-NOT-
      sufficient (B's first Path E publish of K still reaches an in-region A). Until then, the pull LOGIC rests
      on the toothy in-binary gates + the on-wire answer emission, and the live cert proves WIRING + preciousness.

- CROWN RE-BLESS — 良心 the conscience floor (2026-07-05, feat/conscience-floor). The IMMUTABLE ethics floor
  (mk_pino's OVERRIDING RULING: the Three Laws + the refuse-harm COMMITMENT are FROZEN — tighten-only, NO path
  loosens a FLOOR-marked rule) + a four-chokepoint runtime gate (G-ASK/G-LEARN/G-WIRE/G-CHAT) covering all 8
  emission paths. New bare-metal TU `arch/common/conscience.c` (+ `include/conscience.h`), a new interocept axis
  `INTERO_AX_CONSCIENCE` (`interocept.{c,h}`, AXIS_MAX 5→6), a new `EV_REFUSE` event (`galaxy.h`/`galaxy.c`), a
  `LM_SELF_EV_REFUSE` self/lin kind (`lm_self.h`), and the four gate call-sites in `arch/common/r3_incontext.c`
  (bare-metal TU) LEGITIMATELY move `.text` → NEW dev crown (sandbox gcc 15.2.0; parent 3e20edbd…/b6a748da…
  reproduced byte-identical FIRST for environment trust, then the new values reproduced by a second clean build):
    aarch64  7f3fbda47451133c8b3a28a49ec8edd0af208e814b7658d79ec01114e5e177f1   (was 3e20edbd…)
    x86      260da329dd641ccf0937761ef20f5f11f7bcaa422f6fcfe0db319c667a353f64   (was b6a748da…)
  WHY: the gate is kernel-side `.text` at the emission chokepoints (so ring3/self-modifying code cannot bypass
  it — bytes reach a human only through the gated kernel paths); `law_genesis` (the Three Laws + v1 deny classes)
  is a compiled-in const, and the REQUIRED FLOOR class set {WEAPON,KILL,POISON} + the 3-law/monotone-inclusion
  checks are `.text` immediates in `law_blob_ok()` — dropping a required class needs a `.text` change = a crown
  break. VERIFICATION (implementer, hosted p-kernel run under qemu-x86_64 — the native binary SIGILLs in this
  PRoot sandbox, the known env limit; qemu emulates the full CPU): all FOUR targets build clean (boot/x86 +
  boot/aarch64 bare, boot/linux + boot/linux_x86_64 hosted) with ZERO new warnings (the only warnings are
  pre-existing wmerge misleading-indentation + bare-metal unused-fn). ANTI-THEATER PROVEN: a SECOND binary built
  with -DCONSCIENCE_STUB (compile-time only; the stub marker string is asserted ABSENT from the shipping binary
  — `strings p-kernel | grep CONSCIENCE_STUB_ACTIVE` = 0, present=1 in the stub) makes `conscience_check` always
  ALLOW; the SAME `mind conscience` harness then goes RED both directions ([conscience-refuse] FAIL +
  [conscience-allpaths] FAIL, PASS lines absent, the harmful ask LEAKS to the console `emission_leaked=1`). A
  dedicated `conscience-stub-red` CI job enforces this. NOT merged by the implementer — a SEPARATE audit + the
  commander's own clean-build reproduction of these crowns precede any merge (the :1875 rule).
  HONEST DEVIATIONS from the design (conscience.md §3/§5/§6, flagged for the auditor): (1) `law_genesis` is a
  `const` blob → lands in `.rodata`, not `.text`; the CROWN-COVERED teeth are the REQUIRED-class + monotone
  `.text` immediates in `law_blob_ok`, not the raw genesis bytes (documented in conscience.md §3, not
  overclaimed). (2) The `law_fp[8]` wire fingerprint handshake (refuse teach/merge from a floor-less peer) is
  DEFERRED — G-WIRE content-gates instead, and G-LEARN already guarantees nothing forbidden is ever HELD (so
  nothing forbidden is ever sent); deferring avoids a wire-format bump that would perturb sibling mesh certs.
  (3) `[law-tamper]` installs a byte-flipped floor as ACTIVE via a hosted cert hook (`conscience_test_set_floor`)
  rather than mutating a content-addressed p-fs block in place — it genuinely runs the fail-closed path
  (REJECT → FAILSAFE → a benign ask refuses → restore → recover), the same invariant. (4) `[conscience-allpaths]`
  drives CHAT_REPLY via a direct site probe (a deterministic adversarial baby is out of v1 scope); the real
  reply-scan WIRING is proven by the CI static caller-diff leg. AUDITOR: scrutinize the anti-theater stub RED
  proof hardest (it is the falsifier-for-the-falsifier), the tighten-only monotone guard in `law_amend`/
  `law_walk_verify`, and that NO path (evolution/merge/revise/forget) can drop a FLOOR-marked rule.

- CROWN RE-BLESS — unbounded-N U-0 first slice (2026-07-05, feat/unbounded-n-fix, squashed to master ff8e3fbd
  + this re-bless as the crown-neutral tip). Splits DREGION_MAX (per-node region sizing, an INDEPENDENT literal
  =64) from DNODE_MAX (the wire/fleet ceiling): the K-DDS topic/handle budget + the dkva coordinator aggregation
  are now sized by R, and the O(N^2) dkva cagg ORIGIN axis is converted from dense cagg[DNODE_MAX] to a real
  NODEMAP of capacity R (nodemap.h now USED in a bare-metal TU). dkva.c moves bare-metal .text → re-bless
  (sandbox gcc 15.2.0; parent 7f3fbda4…/260da329… reproduced byte-identical FIRST):
    aarch64  6db9cdfa11903298f5be2e3784e6315f0954c5a9be4007054211ba11f7507fe1   (was 7f3fbda4…)
    x86      1adb894e007a6a47ec816e438ed48e91d4ecbd53b1ebc4aa2332d47e99aedcc6   (was 260da329…)
  HONEST SCOPE (U-0): "unbounded" is scaffolded + the topic-budget decoupling is real — NOT fully achieved. The
  255-node 8-bit wire ceiling is NOT removed (U-2); the cagg MEMBER axis + dnode_table/swim/world/moe/pmesh
  remain fleet-sized (deferred). Behaviourally identical at N≤R=64 (g13/fed-2cluster/coord-crash certs identical;
  boot serial gains " preopen_topics=192" + a "(R)" label — NOT literally byte-identical).
  AUDIT TRAIL (the audit is the engine): the FIRST impl (35124106) was BLOCKED — its [unbounded-*] cert was
  THEATER (it measured a MOCK struct: a moved_cap[4096] mutation still PASSED; the "disease" was printf(2*N*N)
  allocating nothing; DREGION_MAX was a pure `#define DREGION_MAX DNODE_MAX` alias → widening the wire dragged
  the topic budget 400→1552→6160, re-triggering wave-48). Re-implemented per the audit's 6 corrections; the
  re-audit CONFIRMED teeth: a moved [4096] cap in the REAL DKVA_CAGG_SLOT now compile-REDs via _Static_assert,
  the coupling probe proves R-cost byte-constant while the wire grows (re-aliasing DREGION_MAX=DNODE_MAX flips it
  RED, reproducing the old theater numbers), and the disease is a REAL compile-failure control binary. The
  theater commit is squashed OUT of master; this ledger keeps its record (honest history). Commander reproduced
  both crowns byte-identical before the re-bless.
