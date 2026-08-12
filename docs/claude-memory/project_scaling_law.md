---
name: project-scaling-law
description: "mk_pino's thesis-test request (2026-07-04): the project's CENTRAL promise is 'more nodes → smarter mind'. Every design hedges 'it's toy-scale'. Is the 'smarter with N' law actually DESIGNED and PROVEN, or only asserted? fable5 to design THE collective-intelligence scaling law: define 'smarter', enumerate the mechanisms, separate real intelligence-gain from cosmetic memory/breadth, design the falsifiable scaling-curve cert, and name the ceiling honestly."
metadata: 
  node_type: memory
  type: project
  originSessionId: 2563c3bf-6291-45c2-a7f8-68ef86d738f8
---

**Stated by mk_pino 2026-07-04.** Observation: every design so far ends with the honest hedge「モデルが小さい /
ネットワークが小さい」. But the project's CENTRAL premise ([[project_education_via_conversation]],
[[project_survival_network]]) is「端末数が増えていく＝ノード数が増えていくと、だんだん賢くなってくる」. His
question:「そこってもうできているのか」— is the "smarter with N" law actually built/proven, or just asserted?
He wants fable5 to design THAT.

**Honest state (verified in code) — real in FRAGMENTS, not as a proven LAW:**
- ✅ **g22 `gossip_learn.c`** — `[g22-gossip-learn]` PROVES "collective EXCEEDS solo ceiling — the swarm learned
  what no node could" + `[g38-learning-improves-guarding]`. N nodes, disjoint shards, gossip-merge. BUT dtr
  (survival-net small model) scale, in-process N-sim + p-fs live, and it measures BREADTH of learning, not raw
  per-answer reasoning quality. Closest existing proof.
- ⚠️ **dmoe** (scratchpad/distributed_moe_design.md) — capacity grows with N, but §10.1 hosted≠routed = may be
  cosmetic. Memory ≠ smarter.
- ⚠️ **capacity(N)** degrade.c — `depth=1+floor(log2(rs))` = LOG scaling, explicitly "display only, not growth."
- ❌ **No society-of-minds** — dkva quorum is attention-aggregation only; NO "N minds cross-check/vote/debate →
  better answer than any single." Likely the MISSING piece for genuine per-N quality gain.

**SIBLING (infra-scale, mk_pino 2026-07-04「64上限も無制限にしましょう」).** DNODE_MAX=64 (drpc.h:35) is NOT a
tunable — it is a HARD architectural bound. 432 uses; many `[DNODE_MAX]` static arrays hold a GLOBAL view of all
N, and some are O(N²) per node (`cagg_exp[DNODE_MAX][DNODE_MAX]`, `cagg_got[...][...]` dkva.c:330-331); dkva
PRE-OPENS 3×DNODE_MAX topics/node at boot (`KDDS_TOPIC_MAX=6*DNODE_MAX+16` kdds.h:50) = the wave-48 overflow
mechanism. Bumping the constant blows static RAM (N² arrays) + re-triggers wave-48 (phones won't boot). TRUE
unbounded N = ARCHITECTURE change: global O(N) views → LOCAL/REGION views (a node tracks only its region/
neighbors, O(region_size)/O(log N), not all N) so per-node state is bounded by REGION size, not fleet size.
Regions ([[project_regions_architecture]], region.h) were built for exactly this; SWIM is partial-view gossip;
dkva already region-scopes aggregation (arrays still global-sized). This is the INFRA counterpart of the
scaling-law thesis (intelligence-vs-N is meaningless if infra caps at 64). fable5 designing (unbounded-N-arch);
mates with dmoe (HRW over unbounded node/expert space), g22 (gossip_learn.c:569 already notes >32-node swarms).
Cert must assert per-node memory/topics do NOT grow with N (anti-theater: simulate 256/1024 nodes in-process,
bounded per-node state). Implementation deferred with the rest.

**The design mandate (thesis test).** Define "smarter" operationally on separate axes that scale DIFFERENTLY:
(a) breadth/coverage (g22 — maybe unbounded), (b) capacity (dmoe — log/linear-ish, maybe cosmetic), (c) raw
reasoning/quality PER answer (teacher-ceiling-bound — does N help this AT ALL?), (d) resilience (proven). The
central question: is there genuine per-N RAW-INTELLIGENCE gain, or does N only buy breadth+capacity+resilience+
data-throughput while raw reasoning stays capped by the toy substrate + teacher ceiling? Design the possibly-
missing ENSEMBLE/society-of-minds mechanism (ownerless, over the lossy mesh, deterministic) if it can give a
real per-N quality gain without raising the per-node ceiling. Design the falsifiable scaling-CURVE cert
(fleet-of-N provably beats fleet-of-1 on an INTELLIGENCE metric, not memory; anti-theater per
[[feedback_cert_isolation_shared_path]] — gain from the collective mechanism, not "one node did all the work"
or "just more data" or cosmetic capacity). Name the ceiling shape honestly. Be willing to conclude the honest
truth — incl. "N buys breadth+resilience+capacity+data, raw smarts is teacher-ceiling-bound, frontier-mouth
([[project_frontier_mouth]]) is the escape for raw quality" if that is the answer. Design doc: scratchpad/
scaling_law_design.md. Implementation deferred (all impl waits until the design set is complete, per mk_pino).
