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
