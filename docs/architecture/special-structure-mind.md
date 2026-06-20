# special-structure-mind — the unified, fleet-sized, sparse, cross-node mind

**Status: ROADMAP — PARTIALLY SHIPPED (SS-1 + SS-2 + SS-3 live; SS-4..7 still design). 2026-06-19.
Grounds every claim in code on `wave-i18n-galaxy`.**
What actually shipped: **SS-1** (adaptive-K firing — heavy tokens fire a WIDER expert set;
the router + runtime `topk_n` + `st_last_fire_width` observability in `arch/common/llm/student.c`),
**SS-2** (the S/M/L tier scaffolding — runtime dims + the `ST_TIERS` table, M-tier
byte-identical to the legacy baby), and **SS-3** (same-tier merge cohorts — the student's OWN
isolated weight average `st_merge_cohort` + chunked p-fs transport `gl_student_publish`/`_fetch`;
cohort = tier, cross-tier REFUSED; never calls `gl_merge` / touches R3 `rw[]`; cert `run_ss3.sh`).
SS-5 (deterministic placement) + SS-6 (cross-node expert firing with local fallback — the F4
capstone) have since SHIPPED (in-process certs; the true multi-process forward over the relay
is a deferred `[live]` row). SS-4 (function-preserving expert growth), SS-7 (bigger baby) remain
genuine design. This is the multi-wave plan to turn mk_pino's four-feature
vision (脳サイズがノード数で伸縮 / 真の疎 MoE / 重い仕事ほど広い領域が発火 / 分散合意 /
複数ノードをまたぐ一回の forward) from today's hariboté into a real unified mind.

---
## ⚠ CRITIQUE GATES (adversarial review 2026-06-19 — verdict: needs-work). Honor these; they OVERRIDE the body where they conflict.
1. **KV cache is a PREREQUISITE for any cross-node / remote-expert wave (SS-6).** `st_generate`
   recomputes the whole `nctx` window every byte (no cache) — per-token relay fan-out is
   ~nctx RTTs/byte (10s/byte). Build a KV cache (its own correctness wave) FIRST, or drop
   per-window remote fan-out.
2. **Resolve the ONE-MIND contradiction in the doc, don't say both.** Different tiers can't
   co-merge (`gl_merge` = flat fold to one length). Either the fleet-wide collective mind stays
   R3's `rw[]` (and the student is the per-node mouth/body that grows locally), OR cross-tier
   distillation is built+PROVEN before any heterogeneity headline.
3. **Canonical reduction order for remote-expert sums** (ascending expert-id, byte-identical vs
   single-node, both arches, -ffp-contract=off) — else SS-6 reopens the salty-bug class fleet-wide.
4. **[no-vla] is a STANDING tripwire pulled into SS-0/SS-1** (not SS-2): adaptive-K makes K runtime,
   so all student.c scratch must be bound to ST_*_MAX (fixed) immediately.
5. **Growth must quantize to the same S/M/L tiers** so N-driven expert growth never produces an
   off-cohort shape that splinters the merge cohort.
6. **DMN-budget cert across BOTH tracks** before SS-4/SS-7; re-certify wave-26/35 teach→answer→
   survive-death against the new student-mouth-with-R3-retrieval answer path.

**CHEAPEST FIRST SLICE (start here, safe + real): SS-0+SS-1 fused** — [baby-merge-isolation]
tripwire + adaptive top-K (router-margin widening, K bounded to K_MAX=E, deterministic, single
node). Makes "heavy task → wider firing" literally true; cannot fragment the collective; installs
the VLA discipline. The amoeba organ-ring's undulation intensity visualizes exactly this firing width.
---
It is honest about what is decorative today and which waves need a bigger model first.

---

## 0. The north star (mk_pino's 考え方)

The **student IS the special mind**: neuron/expert count grows and shrinks with node
count, it routes through real sparse MoE (heavy task → wider region fires), distributed
consensus governs it, and one forward pass spans nodes. You **cannot** copy a pretrained
model in (the architecture is incompatible); you **educate** the baby by a high-spec
teacher conversing with the child a lot. The baby is already byte-level MoE and is
already what the chat speaks from.

## 1. Current-vs-target (verified on trunk)

| Vision feature | Reality on `wave-i18n-galaxy` | Verdict |
|---|---|---|
| F1 brain scales with N | Every model is fixed compile-time. R3: `_Static_assert(R_NP==21568)` (`r3_incontext.c:130`), `static float rw[R_NP]` (`:140`). Student: `#define ST_DMODEL 128 / ST_NEXPERT 4 / ST_TOPK 2 / ST_NLAYER 4` (`student.h:44-48`). Only a *number* scales: `cap_experts_of(N)=clamp(N,1,16)` (`degrade.c:148`), consumed by `degrade_stat` print + `capacity_self_test` only — it sizes NO model. | **DECORATIVE** |
| F2a real sparse MoE | LIVE but ONLY in the byte student: router scores E=4, top-K=2 experts' SwiGLU computed (`student.c:484-518`). Chat speaks from this (`student_chat_generate` → `st_generate_stream`, `student_shell.c:439`). R3 conversational mind is DENSE. `moe.c` routes over network NODES, not neurons. | **PARTIAL** |
| F2b heavy task → wider region | `ST_TOPK=2` is a fixed const; same 2 experts every token. | **ABSENT** |
| F3 distributed consensus | SWIM membership (live, `swim.c`), min-id region coordinator (deterministic fn of SWIM RTT, `region.c:45-71`, NOT voted), FedAvg weight-merge (live: `gl_merge` `gossip_learn.c:69`, `mind_merge_task` `r3_incontext.c:3163`, no aggregator). Raft deliberately unwired ("NOCENTRAL"). | **PARTIAL by design** |
| F4 cross-node single forward | **SS-6 SHIPPED + `[live]` CASHED (wave-ss6-live):** the byte STUDENT fans out its WIDE experts across nodes — when the SS-1 router widens K on a hard token, the extra experts SS-5 places on a peer are computed remotely and summed into `moe[]` in a canonical order, BYTE-IDENTICAL to the single-node forward (`student.c` `st_set_remote_expert` hook + `st_expert_forward_ref`; in-proc cert `run_ss6.sh` 7/7, M=`63e8de333e995913`, L=`67f2434f50e791b6`). The `[live]` row is now REAL: a UDP request/reply transport (`arch/common/ss6_live.c`, port `SS6L_PORT`=7378, mirrors drpc's pending+sem+timeout) ships the `[D]` `f_in` to the SS-5 owner over the mesh; the owner runs the EXACT `st_expert_forward_ref` and replies the `[D]` output. Cert `samples/11_distributed/run_ss6_live.sh`: **4 p-kernel PROCESSES over `./relay`, degrade FULL; node 1's LIVE cross-node forward hash == its single-node oracle hash (`00d6136e6d4676a3`), 266 experts computed on PEERS over the wire** (266 REQ + 266 REP `[D]`-vector frames on the relay). Killed-peer fallback (`run_ss6_live_fallback.sh`): a peer killed but still ALIVE in placement → 4 experts time out → recomputed LOCALLY → still byte-identical, `fallback=4`, no stall. Opt-in `PKERNEL_REMOTE_EXPERTS=1` + FULL + region≥2; single-node byte-unchanged when off; training fail-closes (`st_backward` guard). HONEST SCOPE: the hook is on `st_forward` only; the LIVE CHAT / KV `kv_step` generation path is NOT wired (still a follow-up — "live cross-node forward" ≠ "live chat is distributed"). R3's `ask` answer is still LOCAL (`m_ask` → `r_forward`). | **STUDENT: `[live]` SHIPPED (4-process, byte-identical)** |

The crown result we must NOT break: "one mind" (wave-41/42/44) is FedAvg of `rw[]` and
**depends on byte-identical weight shape** across the fleet (`gl_merge` is a flat
element-wise fold to a single length `n`, `GL_MERGE_MAXFLOATS==21568`,
`gossip_learn.c:97`; `R3_WP_HDR.r_np` fail-closes on shape mismatch).

---

## 2. unified_mind — the target architecture

**Decision: the byte STUDENT becomes the one special-structure mind. R3 is NOT
retired; it is demoted to a SUBSYSTEM the student reuses (memory + Self), not a
second conversational brain.** Justification:

- The student already satisfies three of the four features in embryo (sparse MoE; is
  what the chat generates from; has a malloc'd runtime arena `st_model` with runtime
  `n_params`/`o_*` offsets, `student.h:62-79`). R3 is the opposite: dense, with a
  hard `_Static_assert` shape that is *load-bearing* for the merge.
- The device-capacity verdict (`device-capacity-verdict.md`) already concluded the
  student is "the ONLY place variability may live" and R3 `rw[]` "STAYS FIXED
  fleet-wide." Making the student the growth substrate is forced by that constraint.
- Two conversational brains is the "three brains" anti-pattern the project already
  fought (`moe.c` ONE-BRAIN). We will not re-introduce a dual conversational mind.

**End-state name: the Cradle mind.** One per node: a tiered, MoE byte transformer
(`st_model`) whose firing width adapts to task hardness, whose experts can live on peer
nodes, and which is taught — not loaded — by teacher conversation. It keeps two
borrowed organs from R3:
- **R3 as episodic/working memory** (the in-context key→value binding, `r_forward`)
  becomes a *retrieval signal* fed into the student's context, not a separate answerer.
- **The Self-lineage + DMN consolidation engine** (hash-chained `self/lin`, the sleep
  tick) drive the student's distillation, exactly as `student_dmn_consolidate`
  (`student_shell.c:315`) already does today.

So: **compose at the subsystem level, unify at the conversational level.** One mouth
(the student), R3's memory + Self + sleep kept as machinery beneath it.

---

## 3. dynamic_size — how the brain grows with N at runtime

**Pick the SIMPLEST viable path: discrete tiers (S/M/L) + experts that physically
live on more nodes. NOT arbitrary runtime dims.**

### 3.1 What "more nodes → bigger brain" concretely means
The first growth axis is **expert COUNT and expert PLACEMENT**, per `native-student.md
§A.4` ("expert を足す"), because expert⇔node is the most natural map to the fleet's
dynamic-N and because function-preserving expert duplication (Net2Net / MoE upcycling)
has a clean weight-translation. Concretely:
- `experts_active(N) = clamp(N, E_min, E_max)` reusing the EXISTING `cap_experts_of`
  curve (`degrade.c:148`, today clamped to `CAP_E_MAX=16`). Today that number sizes
  nothing; this wave makes it size the router's expert table.
- New experts are **function-preserving copies** of existing experts + a router
  expansion (`native-student.md §A.4`), so a fixed prompt's output is ε-unchanged at
  the instant of growth (`[grow-preserves]` cert). Subsequent DMN distill differentiates
  the twin experts.
- A shrink folds an evicted expert's mass into its replica before the node leaves
  (the L→S thermal-demotion data-loss hole called out in the verdict).
- `d_model` and `n_layers` are LATER axes (§A.4 "第二/第三成長軸"); we do NOT vary them
  first — they touch attention/embedding/layout and break weight translation.

### 3.2 The VLA / stack-overflow trap — the SIMPLEST fix
`student.c` has ~11 stack-local arrays sized by the compile-time dims (`tmp[D]`
`:465`, `moe[D]` `:494`, `g_logit[V]` `:753`, `g_oin[D]` `:764`, `g_fin[D]` `:811`,
`g_gate[E]` `:813`, `gw_chosen[K]`, `g_eo[D]`, `g_eh[DFF]`, `g_ain[D]`, `chosen[E]`
`:368`). If the dims became runtime variables these turn into **VLAs** = the hosted-relay
stack-overflow class (`feedback_hosted_relay_stack_overflow`).

**The fix is two-pronged and both are cheap:**
1. **Discrete tiers, not arbitrary sizes.** Define `ST_TIER_S / _M / _L` as fixed
   `{E,topK,D,DFF,nLayer}` triples. The dims become a small `const` struct selected at
   `st_init` time. Each scratch array is bounded by `ST_*_MAX` (the L-tier value), so
   they stay FIXED-size on the stack — never VLA. Same `-O1 -ffp-contract=off` recipe.
2. **The big cache already lives in the malloc'd arena** (`cache_get`,
   `student.c:250`), so growth there is free. Only the small per-token scratch needs
   the `_MAX` bound. (If even the L-tier `g_logit[V=256]` is uncomfortable on a 4KB
   kernel stack, move those into the arena too — the verdict's "no-VLA" gate.)

**Mergeability constraint (the cost of tiers):** `gl_merge` assumes identical dims.
So the fleet is partitioned into **same-tier merge-cohorts**: S nodes average with S
nodes, L with L. There is NO single fleet-wide student average across tiers — a weak
node cannot receive a strong node's expert learning by merge, by construction. The
ONLY cross-tier bridge is distillation (the teacher path), which is honest but must be
proven before the heterogeneity headline is claimed (verdict gate). The shared
fleet-wide "one mind" stays the R3 `rw[]` core (unchanged), so heterogeneous students
never threaten the crown result.

**Why not arbitrary `n_params`:** arbitrary sizes (a) re-open VLAs, (b) make every pair
of nodes a merge-island, (c) buy nothing the watch-class M baby needs today. Tiers are
the 80/20.

---

## 4. adaptive_moe — heavy task fires a wider neuron region

**Make `top-K` adaptive per token, deterministically.** Today `router_pick`
(`student.c:365`) always picks `K=ST_TOPK=2`. Replace the fixed K with a **confidence-
gated widening**:

- **Hardness signal = router margin** (NOT entropy, NOT logit values — margin is the
  cheapest order-stable signal under `-ffp-contract=off`). After scoring the E gate
  logits, sort descending; let `margin = gate[top1] - gate[topk]`. While
  `margin < THETA` and `k < K_MAX`, admit the next expert. So an EASY token (one expert
  dominates, large margin) fires K_min=1–2; a HARD/ambiguous token (flat gate, small
  margin) widens toward K_max (up to E). This is mk_pino's "重い仕事ほど広い領域が
  発火" made literal at neuron granularity.
- **Determinism / one-math:** THETA is a fixed compile-time threshold; the comparison
  is `gap < THETA` on already-summed float logits (no new transcendental), so
  `(weights, bytes) → identical K` on every target (the wave-49 discipline). The
  softmax over the K chosen is unchanged. Grad-check still freezes routing
  (`st_freeze_routing`, `student.c:361`) so the adaptive selection is differentiable at
  fixed routing exactly as top-K already is.
- **Bounded compute:** K_max ≤ E, so the per-token cost is still ≤ the dense FFN cost;
  the scratch (`e_g/e_u/e_h` indexed by `[..][K][DFF]`, `cache_get`) is sized to K_max.
  No VLA, bounded latency.

This is the NS-3 "variable top-k (2→8)" lever, brought forward to be the cheapest
visible win (see sequencing). It needs only the byte student, no network.

---

## 5. cross_node_firing — one forward across nodes

**Honest stance: per-TOKEN cross-node generation over the relay is NOT feasible for the
student** — generation is already ~1s/byte with NO KV cache (`student_shell.c` /
`st_forward` recomputes the whole window each step), the relay adds 100s of ms RTT, and
DKVA itself gates remote fan-out behind FULL-degrade + ≥3 region members + a 600ms
timeout with LOCAL fallback (`dkva.c:65`, `degrade.c:78`). Naively adding a relay hop
inside the per-token loop multiplies an already-slow path by the fan-out.

**What IS feasible — fan out the EXPERTS, not the attention, and only the heavy ones:**

- The student is MoE. **Remote-expert execution** generalizes the existing
  `moe.c` pattern (`dtk_infer` remote RPC + local fallback, `moe.c:470`) from the sensor
  classifier to the student's FFN: when the adaptive router (§4) widens K on a HARD
  token, the *extra* experts beyond the local K_min can be computed on peer nodes that
  host them, in parallel, and their weighted outputs summed into `moe[]`
  (`student.c:516`). The expert output is a single `[D]` vector — tiny on the wire
  (D=128 floats ≈ 512 B) vs the DKVA KV tensors. The sum is associative (same
  `Σ wⱼ·eoⱼ` structure DKVA already proves order-independent, `dkva_self_test`), so it
  matches a single-node forward exactly.
- **Latency rule (mandatory):** the local K_min experts always run locally; remote
  experts are fired **only** when (a) the router widened (hard token) AND (b) a peer in
  the region hosts that expert AND (c) we are FULL-degrade with ≥2 region members.
  Each remote expert call has a hard timeout with **local fallback** (recompute that
  expert locally) — the same survival contract as DKVA/`dtk_infer`. So a slow/absent
  peer never stalls a token; it only loses the *width*, degrading gracefully to a
  narrower local forward (honest `degraded(k/n)` print, like `dkva.c:617`).
- **NOT attention-KV split for the student** (unlike DKVA): the student's attention is
  single-head over a ≤64 window and recomputed per step; splitting it across the relay
  buys nothing and costs an RTT inside the inner loop. KV-split stays the sensor path's
  trick where the cache is the expensive part.

So: **the student's cross-node forward = remote MoE experts on hard, wide tokens, with
strict local fallback.** This is the one place "複数ノードで一回の forward" becomes
literal for the conversational mind, and it composes with §4 (only hard tokens widen,
only wide tokens fan out) so the common case stays fast and local.

---

## 6. consensus_stance — reconcile with NOCENTRAL

**Honest stance: the current decentralized coordination (SWIM + region + FedAvg) is
SUFFICIENT for the vision's "distributed consensus" for the WEIGHTS, but the new
expert-placement layer needs a small amount of NEW agreement — and it can stay
NOCENTRAL.**

- **Weights:** already solved without a leader. `gl_merge` is peer-symmetric, order-
  independent, no aggregator index (`gl_check_no_central`, `gossip_learn.c:389`); every
  node folds locally. Raft stays unwired; a Raft leader = the forbidden central
  coordinator. Nothing to add.
- **The genuinely new question: "which node holds which expert?"** Remote-expert firing
  (§5) and expert placement on growth (§3) require nodes to AGREE on a placement map
  (expert e → node n). The NOCENTRAL-faithful answer is **NOT to vote and NOT to elect a
  registrar**, but to use the SAME deterministic-from-membership trick `region.c` and
  HRW lookup (`lookup.h`, `LOOKUP_MAX_MEMBERS`) already use: place expert e on the node
  that wins a **rendezvous hash** of (expert-id, alive-member-set). Every node computes
  the identical map locally from its local SWIM view — no broadcast, no quorum, no
  leader, and it re-derives automatically when membership changes (a dead node's
  experts re-home deterministically, replica-style). This is the same min-id /
  deterministic-coordinator philosophy already shipped (`region_coordinator`,
  `region.c:83`), extended from "who summarizes" to "who hosts expert e."
- **Eventual consistency, not strong consensus:** during membership churn two nodes may
  briefly disagree on the map. That is fine — a misrouted remote-expert call simply
  times out and falls back local (§5). We do NOT need strong consensus; we need a
  deterministic map + a safe fallback. This honors NOCENTRAL: no node is privileged, no
  vote, the truth is a local function of local membership.

So the vision's "distributed consensus" is satisfied by **deterministic-from-membership
placement + peer-symmetric merge + safe fallback**, never by a Raft/quorum leader.

---

## 7. dual_mind_resolution — what happens to R3

**R3 is preserved, not broken; it stops being a second conversational brain and becomes
the Cradle mind's memory + Self + sleep machinery.**

- **DMN / sleep consolidation (wave-21..26):** UNCHANGED engine. `student_dmn_consolidate`
  (`student_shell.c:315`) already runs as a SECOND track after R3's own consolidation in
  the same `dmn.c` idle hook. The roadmap keeps both: R3's consolidation maintains the
  shared `rw[]` core + working memory; the student track grows the conversational mind.
  Sleep stays the single consolidation heartbeat.
- **Facts / in-context binding (R3, LM-6..):** R3's `r_forward` key→value recall becomes
  a **retrieval subsystem** — its answer is injected into the student's prompt context
  (a fact the student conditions on), NOT a competing final answer. The student's mouth
  is the only mouth.
- **Self-lineage (wave-22):** UNTOUCHED. The hash-chained `self/lin` continuity, tamper-
  evidence, ownerless reconstruction all stay; they now also record the student's tier +
  growth events (the verdict's "Self-layer growth 記帳").
- **`rw[]` stays fixed fleet-wide** (the merge crown). The student's tiered weights NEVER
  enter `gl_merge`/`gl_merge_w` of `rw[]` — this is a mandated CI tripwire
  (`[baby-merge-isolation]`, verdict gate): R3 merge inputs are only `R_NP`-sized, only
  from `r3_weights_get`.

Net: no living-mind capability regresses; R3's three DMN functions + Self + facts all
keep running. The only change is that R3 no longer *answers chat* — the student does,
using R3's memory.

---

## 8. sequencing — ordered, small, falsifiable waves

Cheapest/highest-value first. Honest about what needs a bigger model first.

1. **SS-0 — `[baby-merge-isolation]` CI tripwire (NO model change).** Prove the student's
   weights can never reach R3's `gl_merge`. Pure guard; unblocks everything else safely.
   Falsifiable: a test that feeds a student blob to the R3 merge inputs must fail-closed.
2. **SS-1 — adaptive top-K in the byte student (§4).** Router-margin widening, K_min..K_max,
   deterministic. NO network, NO sizing. Highest visible value: "heavy task → wider firing"
   becomes real and measurable on ONE node. Cert: `[adaptive-k-margin]` (hard tokens fire
   wider) + `[adaptive-k-determinism]` (same bytes → same K across targets) + no
   loss regression vs fixed K=2.
3. **SS-2 — tier scaffolding (§3.2), still single-node.** Move the 5 dims into a `const`
   tier struct; bound all scratch to `ST_*_MAX` (kill the VLA risk); add a tier byte to
   `st_save`/`st_load` header. Cert: `[no-vla]` (build + stack-bound check), `[tier-load]`
   (S/M/L round-trip), M-tier numerically identical to today.
4. **SS-3 — same-tier merge-cohorts (§3.2). SHIPPED 2026-06-19 (wave-ss3-merge-cohorts).**
   The student's OWN, isolated weight average `st_merge_cohort` (`arch/common/llm/student.c`):
   canonical reduction (self first, then peers ascending; plain sum then one divide — no new
   transcendental, no libc math, `-O1 -ffp-contract=off`). **2-way is byte-identical** —
   `merge(A,{B}) == merge(B,{A})` (the cert + contract case). For N≥3 cohorts the result is
   deterministic per the SAME accepted-set but NOT byte-identical across a different *aggregator*
   (FP add is non-associative; audit measured ~0.4M/1.9M weights differ) — **convergence, not
   bit-identity, is the goal** (each node merges with itself as `into`; standard FedAvg). Cohort
   = TIER: the SAME fail-closed `st_blob_tier_ok` guard
   `st_load` uses gates every peer, so a cross-tier blob is REFUSED (islands by construction);
   it NEVER calls `gl_merge`/`gl_merge_w` and NEVER touches R3 `rw[]` ([baby-merge-isolation]
   re-confirmed). Variable-length p-fs transport `gl_student_publish`/`gl_student_fetch`
   (`arch/common/gossip_learn.c`): the multi-MB student blob is CHUNKED across ≤`PFS_BLOCK_MAX`
   (4096-byte) objects + a header object (`gl_blob` is fixed R3-class and too small) — a blob
   needing > `GL_ST_MAXCHUNK` chunks is REFUSED, never truncated.
   - **Cert (`tests/llm/run_ss3.sh`, in-process, no network):** `[ss3-cohort-converge]` (two
     M-tier minds, shared objective, different orderings → merged held-out loss ≤ the worse
     parent AND merge(A,{B}) bytes == merge(B,{A}) bytes); `[ss3-cohort-island]` (an S blob
     into an M merge is REFUSED, M weights byte-unchanged); `[ss3-merge-falsifiable]` (a
     1e-3-perturbed peer breaks the symmetry — the cert can FAIL); `[baby-merge-isolation]` +
     `[no-vla]` (`-Werror=vla`) re-confirmed. Measured: merged loss 2.65 ≤ worse 3.56 (6/6 PASS).
   - **HONEST (Path-W, memory `moment_2026_06_12_wave41_one_mind`):** averaging is the MECHANISM
     and converges minds toward a SHARED/compatible objective; it is LOSSY for two minds that
     learned DIFFERENT facts (one survives, one decays toward chance). This wave proves the
     mechanism, NOT divergent-fact preservation — union-replay / Fisher recovery is Path-W² and
     OUT OF SCOPE here.
   - **HONEST (blob size):** even the S tier is ~1.9 MB ≈ 480 chunks; M ~22 MB ≈ 5577 chunks;
     L ~247 MB ≈ 60330 chunks (> `GL_ST_MAXCHUNK`, so L-over-relay is a documented bound). The
     LIVE chunked publish/fetch over the relay is a DEFERRED `[live]` row; the converge claim is
     in-process and needs no network. Adam moments are RESET (not averaged) post-merge (FedAvg).
5. **SS-4 — function-preserving expert growth (§3.1).** `cap_experts_of(N)` now SIZES the
   router. Cert: `[grow-preserves]` (ε-unchanged output at growth) + `[grow-then-learn]`
   (distill lowers loss after growth). **Needs SS-2/3 first; benefit is small until the
   baby is larger** (verdict point 5 — watch-class M already runs everywhere).
6. **SS-5 — deterministic expert placement map (§6). SHIPPED 2026-06-20 (wave-ss5-placement).**
   `st_expert_owner(expert_id)` places expert e on the node that WINS a rendezvous hash (HRW)
   of (expert-id, alive-member-set), computed LOCALLY from the SWIM `dnode_table[]` view —
   no broadcast, no vote, no leader, no new gossip. New module `arch/common/placement.c` /
   `arch/common/include/placement.h` (kernel tier, COMMON_C_SRCS — builds on all four boots).
   It is a THIN shim over the EXISTING HRW primitive `lookup_responsible()` (`arch/common/
   lookup.c`): the per-expert key is a stable 32-byte vector derived ONLY from the id
   (`st_expert_key`), so `weight(n,k)=sha256(key||node)` is byte-identical across aarch64 /
   x86_64 / i686 (the lookup.h cross-ABI contract — NO float, integer + sha256 only). PURE
   cores `st_expert_owner_in`/`st_expert_owners_in` take an explicit member set (testable, no
   globals); LIVE wrappers (`st_expert_owner`, `st_expert_is_local`, `st_expert_owners`) read
   the DNODE_ALIVE set; a top-`ST_PLACE_RMAX` replica list is provided for SS-6 fallback.
   - **Cert (`tests/llm/run_ss5.sh`, in-process, no network; shell verb `place`):**
     `[place-deterministic]` (one synthetic alive-member set → each expert's owner is identical
     no matter which node computes it; the fn reads NO self-id; cross-arch identity BY
     CONSTRUCTION), `[place-rehome]` (kill the owner of expert e → it re-homes to the next HRW
     winner AND every OTHER expert's owner is UNCHANGED — minimal disruption; FALSIFIABLE: a
     modulo-N control reshuffled 8/8 experts vs HRW's 4 on the same set-shrink), `[place-balance]`
     (16/32 experts spread across 5/5 nodes — reported, not over-claimed). `[nocentral]` +
     `[no-float]` + `[no-vla]` (`-Werror=vla`) structural greps clean. 8/8 PASS.
   - **HONEST (scope):** this is the placement MAP + cert ONLY. Remote-expert EXECUTION
     (firing an expert on its owner node over the mesh) is **SS-6, DEFERRED**. On a single node
     the alive set is `{self}`, so every expert maps to the one node — the map is SS-6's
     FOUNDATION, not cross-node firing. A true multi-node LIVE placement-convergence run over
     the relay is a DEFERRED `[live]` row; the in-proc cert drives the REAL function with
     synthetic member sets.
7. **SS-6 — remote-expert firing with local fallback (§5). SHIPPED 2026-06-20 (wave-ss6-cross-node).**
   The F4 capstone — "複数ノードをまたぐ一回の forward" for the byte student. When the SS-1
   adaptive router WIDENS K beyond K_min on a hard token, the EXTRA experts (chosen-slot j ≥ K_min)
   that SS-5 placement puts on a PEER are computed REMOTELY (the peer runs that expert's SwiGLU on
   the `[D]` f_in vector — ~D floats on the wire, tiny vs DKVA's KV tensors) and their `[D]` outputs
   are summed into `moe[]` in a FIXED canonical reduction order (ascending chosen-slot j == the
   single-node accumulation order — CRITIQUE GATE #3) ⇒ a remote forward equals the single-node
   forward **BYTE-FOR-BYTE** (`-O1 -ffp-contract=off`, both arches). Both branches call the SAME
   `st_expert_forward_ref` (one math); the MoE loop materializes each chosen expert's `[D]` output
   into a file-static `eo_all[KMAX][DMAX]` scratch (no-VLA, off the small kernel stack) then folds.
   The local K_min experts ALWAYS run locally; a remote timeout / absent peer RECOMPUTES that expert
   LOCALLY (lose the WIDTH, not correctness — honest degraded). Transport is the caller-installed
   `st_set_remote_expert(fn, gate, ctx)` hook (the kernel wires DRPC; the cert wires a stub) — NO new
   transport invented, mirroring `dtk_infer` / `dkva.c`. Gating: j ≥ K_min AND `!st_expert_is_local`
   (SS-5) AND FULL-degrade with ≥ 2 region members.
   - **Cert (`tests/llm/run_ss6.sh`, in-process, no network):** `[remote-expert-equiv]` (a forward
     that fires the wide experts on a STUB peer is byte-identical to the pure single-node forward —
     same logit FNV hash — M tier 506 experts remote, L tier 940; some experts MUST have gone
     remote, counted); `[remote-expert-fallback]` (the peer ALWAYS times out → st_forward recomputes
     locally and the result is STILL byte-identical, no stall, honest degraded count); `[remote-
     falsifiable]` (a 1e-6 remote perturbation FAILS the equiv — the cert has teeth);
     `[canonical-order]` + `[no-vla]` greps clean. 7/7 PASS. The S tier is proven INERT (E=2 == K_min,
     no room to widen, so no wide expert to fan out — byte-identity trivially the single-node path).
   - **Crown (re-confirmed):** NEVER touches R3 `rw[]` / `gl_merge`; KV byte-identity intact
     (`kv_step` UNTOUCHED — `run_kv.sh` still byte-identical); single-node path byte-UNCHANGED when
     the hook is clear; LP64 / no-VLA.
   - **HONEST (scope / `[live]` deferral):** the in-process cert drives the REAL canonical-sum +
     fallback code path with a STUB peer; a true multi-process cross-node forward over the relay /
     mesh is a DEFERRED `[live]` row. The hook is wired into `st_forward` (the canonical forward);
     wiring it into the KV incremental `kv_step` generation path (the chat-speed loop) is a documented
     follow-up — `run_kv.sh`'s byte-identity is UNCHANGED by SS-6. Needs SS-1 (adaptive K) +
     SS-5 (placement), both shipped.
8. **SS-7 — bigger baby (D/layers up a tier) THEN re-run S/L tiers.** Sequenced LAST per the
   verdict: tiers are inert until a device actually can't run the max baby. This is where
   F1 ("strong device, bigger brain") stops being decorative in practice.

Waves needing a bigger model first: **SS-4, SS-7** (and the heterogeneity headline of
SS-3 only pays off once tiers genuinely differ in capability). SS-1 and SS-5/6 deliver
visible vision-features on the current small baby.

---

## 9. hard_problems — the genuine unknowns / risks

- **Cross-tier learning is fundamentally lossy by merge.** A weak node can never receive a
  strong node's expert learning except by distillation, and the distill loop in
  `student.c` is still vapor (verdict point 3). If distillation doesn't transfer, "one
  mind" silently fractures into per-tier sub-collectives. This is the single biggest risk.
- **Per-token relay latency is a hard wall.** The student is ~1s/byte with no KV cache;
  even fanning out only wide tokens, a region with high RTT may make remote-expert firing
  net-negative. KV-caching the student (currently absent) may be a prerequisite, and that
  is its own wave with its own correctness surface.
- **FMA / one-math under adaptive K and remote sum.** `-ffp-contract=off` killed the salty
  bug (wave-49). Adaptive K adds an order-stable comparison (safe), but remote-expert
  summation introduces a NEW cross-node float-order question: the partial sums must be
  combined in a fixed order or the answer drifts per-fleet. DKVA proves single-integer
  partials are order-independent; the student's real-valued expert outputs are NOT exactly
  associative — needs a fixed reduction order + a `[remote-expert-equiv]` tolerance.
- **Expert-placement churn vs. determinism.** Rendezvous hashing re-homes experts on every
  membership change; under flapping SWIM this could thrash placement and cause repeated
  fallback. Needs hysteresis (the `moe.c` deadband lesson) without re-introducing a central
  registrar.
- **Growth correctness.** Function-preserving expert duplication is a borrowed idea
  (Net2Net / MoE upcycling); whether the twin experts actually DIFFERENTIATE under the
  tiny DMN distill budget (not just stay redundant copies) is unproven on this codebase.
- **VLA regression is permanent vigilance.** Even after SS-2 bounds the scratch, any future
  edit that sizes a stack array by a runtime dim re-opens the stack-overflow class. The
  `[no-vla]` gate must be a standing tripwire, not a one-time check.
- **Two consolidation tracks competing for the DMN budget.** R3 and the student both
  consolidate on the same sleep tick; as the student grows, the fixed engram/time budget
  may starve one. Salience-weighted budgeting (wave-23) may need extending across both.
- **The honest-growth discipline (非交渉).** No fake progress in the 地層. Every accuracy in
  the lineage must be a cert number, and growth events must be Self-recorded truthfully —
  a social/process risk as much as a technical one.
