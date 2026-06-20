# p-kernel independent audit trail

This file is the project's independent-auditor ledger: each row records that a
SEPARATE actor (an independent adversarial subagent, NOT the implementer)
reproduced the evidence for a shipped wave, in git. It exists because the
2026-06-20 harsh review found that "implementer != auditor" left NO independent
trace in the repository's own history. A row here is that trace.

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
