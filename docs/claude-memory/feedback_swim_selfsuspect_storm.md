---
name: feedback_swim_selfsuspect_storm
description: "p-kernel: the SWIM self-suspicion SCATTER STORM — a node rumored SUSPECT calls the heavy unpaced replica_scatter_all() EVERY time, which under load self-amplifies and deafens the node; ONE bug behind both one_mind/Path W AND shared_mind CI reds. Plus: the CI crown gate compares .text to the PARENT commit (hard freeze), so intentional bare-metal changes must be hosted-gated or re-blessed in a dedicated wave."
metadata:
  node_type: memory
  type: feedback
  originSessionId: 2563c3bf-6291-45c2-a7f8-68ef86d738f8
---

2026-07-01, root-causing the last 2 self-hosted CI reds (one_mind/Path W
`[onemind-survive]` + shared_mind `[shared-consolidated/grounded/live]`).

**They are ONE bug, not two, and NOT a flake.** `arch/common/swim.c` (~line 290):
when a node hears a gossip rumor that IT ITSELF is SUSPECT/DEAD, it calls
`replica_scatter_all()` (arch/common/replica.c:387) — which sends the node's FULL
memory snapshot, **UNPACED**, to ALL DNODE_MAX peer slots ("待っている暇はない").
Under self-hosted CPU load a node routinely falls behind on SWIM PING/ACK and gets
**falsely** rumored SUSPECT → full scatter → falls further behind → more suspicion
→ more scatter: a self-feeding **STORM**. CI artifacts (run 28340693132): each node
emits `[replica] *** DEATH THROES ***` 7-8× tracking `[swim] *** SELF-SUSPICION ***`
**1:1**. The node goes DEAF → can't fold 84KB weight chunks (one_mind) and can't
consolidate/re-ask/name-the-teacher (shared_mind). SUSPECT is a TRANSIENT state;
scattering the whole mind on every suspicion rumor is the defect.

**Diagnostic method that worked:** per-node counts from artifacts — `sent block`,
`heard announce`, `gave up`, plus `grep -c SELF-SUSPICION` vs `grep -c "DEATH THROES"`.
The 1:1 correlation + "B never served ANY block (sent=0)" + "B published rw 0×" is
what pinned it to swim, not pfs delivery. (I initially suspected cold-ARP pfs
want/serve per [[feedback_live_forward_cold_arp]] — WRONG; the node was too busy
scattering to serve. Read the whole node's behavior, not just the stuck-block trace.)

**Fix shipped (hosted-gated):** throttle the scatter to ≤1 per interval under
`_TK_HOSTED_LIBC_`; the REFUTE (incarnation bump + ALIVE re-assert) still fires every
time (liveness preserved); survival semantic kept (a suspected node still scatters
once). Bare-metal `#else` keeps the unconditional call → crown byte-identical.

**CROWN ENFORCEMENT MECHANISM (was not in memory):** the CI job
`crown-text-identity` (.github/workflows/ci.yml ~1050-1116) builds the bare-metal
.text of HEAD **and of its PARENT** and BLOCKS if they differ — a HARD per-commit
freeze, drift-vs-parent, not a golden-hash check. So ANY intentional bare-metal
.text change trips it; you must either (a) keep the change hosted-gated
(`#ifdef _TK_HOSTED_LIBC_`, bare `#else` byte-identical — the default discipline),
or (b) deliberately re-bless in a dedicated wave (document + re-baseline the dev
crown in docs/audit-trail.md). Canonical dev crown: aarch64
`755a20fae2d9b7415045193ea8287623dbeb906963609a63dce8a19c8a130513`, x86
`4064d8a95e68950eee263a1bd6f131518f655f002bf2eccc1e824b4d87ee0413`.

**TEST-RUNNER TRAP (auditor caught):** the host certs (run_survival_l0/l1,
run_swim_selfsuspect) each `make clean && make` the SHARED `boot/linux` aarch64
tree, so they are NOT safe to run CONCURRENTLY — parallel runs mutually corrupt
build artifacts → spurious CURE-FAIL / falsifier-not-RED. CI runs them SERIAL (fine);
never fan them out in parallel in a future wave without per-run build dirs.

**HONEST BOUND:** hosted-gating fixes the deployed (Linux/Android) fleet but leaves
bare-metal still storm-prone under load → a real bare-metal fix + crown re-bless is
a deferred backlog follow-up. Related: [[feedback_ci_operations_flake_runid]] (the
de-flake correctly did NOT mask this — it's a real bug, not a transient).
