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
