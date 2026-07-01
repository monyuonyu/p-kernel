# survival-network §7 / G38 — distributed-gating wiring: implementation plan (cert-first)

**Status:** DESIGN PLAN produced by an automated design-harden workflow on trunk `1f656c3c`. NOT YET IMPLEMENTED. Awaiting commander review, then a SEPARATE impl → audit cycle (implementer ≠ auditor ≠ commander, per the constitution). The acceptance cert in §1 is authored here as a starting point; the auditor OWNS the final pass/fail formula and runs it — the implementer must not also define success (trap A1).

---

## 0. Decision: which design won, and why

Three designs were hardened and critiqued. All three scored low and **none survived its own killer objection** — that is the honest starting state. The winner is chosen by which killer objection is *cheapest to discharge while keeping the design's core*, with the other two designs' best ideas grafted in.

| angle | true_local_gradient | oscillation_safe | cert_falsifiable | nocentral_held | avoids_learner_trap | score |
|---|---|---|---|---|---|---|
| **minimal-diff** (`gacc`) | ✅ | ❌ | ❌ | ✅ | ✅ | **38** |
| regions-r3-native (`gate_bias`) | ❌ | ❌ | ❌ | ✅ | ❌ | 38 |
| cert-first (`cap`) | ❌ | ❌ | ❌ | ✅ | ❌ | 34 |

**Winner: the minimal-diff `gacc` design (per-observer, self-observed remote-merit estimator), as the SKELETON.** Why it wins on the five axes:

- **true_local_gradient — only minimal-diff earns it.** Its error signal is the action→outcome residual of the route THIS node actually took (`remote-class == learned_class ? 100 : 0`), a private per-observer estimate. The other two were marked `false` because their "regret"/`q` signals collapse on the production code (see below).
- **avoids_learner_trap — only minimal-diff earns it.** The regions-r3 and cert-first designs both feed a learner whose objective is **degenerate against the real `moe_infer`**: I verified at moe.c:563 and moe.c:577 that `result_class = learned_class` is assigned on BOTH the local-route branch AND the remote-timeout/fallback branch. So `good += (result_class == learned_class)` is automatically TRUE for every local route and every *timed-out* remote — the learner descends toward "route to myself or to whoever times out." That is the all-green-while-wrong trap (A4) baked into the objective. Minimal-diff's signal is the only one that distinguishes a *successful* remote (line 572, the only branch producing an independent class) from a fallback, so it can rank a chronically-timing-out peer BELOW its broadcast accuracy — a thing the `correct/total` table at moe.c:587 structurally cannot express.
- **nocentral_held — all three hold it**; minimal-diff keeps it *and* demotes the broadcast table to a cold-start seed, strengthening it.
- **Its killer objection is a CERT-SEAM bug, not a CONCEPT bug.** The objection (correct, I confirmed it) is that `select_expert` has exactly ONE caller — moe.c:443, inside `moe_infer` — needing `dtr_decide` + `drpc_my_node` + `dnode_table` + live SWIM. The existing tests (`st_herd` at moe.c:811, `st_test_nocentral` at moe.c:683) are *reconstructions* that call only leaf helpers (`expert_utility`/`ewma_step`/`st_reflex_step`) and rebuild the candidate loop. So a cert "mirroring st_herd" would certify a parallel reconstruction's decimation flag, not the production write-gate — trap A2. **This is fixable by refactoring a testable seam out of the production hot path (Wave 0 below), without changing the routing concept.** The regions-r3 and cert-first killer objections are *objective inversions* — fixable only by abandoning their error signal. A seam refactor is strictly cheaper than re-deriving the learning objective.

**The weakest-killer-objection tiebreak therefore goes to minimal-diff:** "certify the wrong system" (A2) is closed by extracting a real function the cert and production both call; "learn the wrong objective" (A4) is closed only by replacing the signal — which is exactly what we graft the *minimal-diff signal* in to do.

### Grafts from the runners-up (kept verbatim where noted)

1. **From regions-r3-native — DELETE the broadcast table, don't just stop reading it.** Demoting `peer_scores[]`/`broadcast_score()` to "vestigial" still leaves a global rank in the binary that a future edit can re-privilege. We keep the broadcast table alive ONLY as an explicit cold-start *seed* read behind a `gacc_valid` flag (so NOCENTRAL is structural at the decision), but the plan flags full deletion of the `MOE_SCORE` K-DDS topic as a fast-follow once `gacc` is proven (deferred, §6). Graft the regions-r3 **cert discipline**: an explicit unfixed-control NAIVE arm in the SAME harness (C5) + a `[live]` 3-node N≥5 relay kill-test kept OPEN in the gap-ledger.
2. **From cert-first — the `[in-proc]` vs `[live]` live-first gating (kept verbatim as a mandate).** The cap-reseed-on-recycled-TCB row stays OPEN as a `[live]` N≥5 kill-test (never silently promoted), and we **confirm the outcome signal has real dynamic range under flood on mk_pino's phone BEFORE crediting the cure** (the salty-bug lesson: in-proc green, dead live path). Also grafted: the **byte-equal honest-degrade assertion** for never-observed nodes (a clean falsifiable sub-check that doubles as proof `gacc` is not just a re-skinned accuracy table — its valid-flag path is byte-identical to the old seed when no sample exists).
3. **From cert-first's killer objection (the hardest graft) — the COUPLED-SUM proof.** The minimal-diff objection's second half is load-bearing and is carried forward as a first-class cert requirement: the fast load axis already writes `recent_pick[pick] += MOE_PICK_LOAD` (moe.c:374, confirmed) into the SAME `expert_utility` sum that `gacc` feeds. A slow bias term does **not** low-pass a fast oscillation in a shared sum. Therefore the §8 separation cert (C3) MUST prove the time-constant ratio on the **coupled pressure+gacc utility**, not on the gacc axis in isolation. This is the single most important correction this plan makes to the winning design.

---

## 1. THE ACCEPTANCE CERT (cert-first — the implementer must pass exactly this)

Everything below in §2–§5 exists only to make this cert pass. The implementer implements TO this cert; the auditor owns and runs it.

### 1.0 The testable seam (Wave 0 prerequisite — without this the cert is a rubber stamp)

The production hot path is refactored so the cert drives the **real** selection arithmetic, closing trap A2. Extract from `select_expert` (moe.c:279–375) a pure, no-I/O function:

```c
/* Pure: one decision over a candidate set. NO dtr/drpc/SWIM/printf.
 * select_expert() calls this after enumerating candidates; the cert
 * calls this VERBATIM with a scripted candidate set. Same code, two callers. */
UB moe_select_step(const MOE_CAND *cand, UB ncand, UB gate_class,
                   UB *incumbent_io);   /* returns picked candidate index */
```

`MOE_CAND` carries the per-candidate `{node_id, acc, rtt, eff_pressure, threat, same_region}` already computed at moe.c:308–319. `moe_select_step` runs the *real* `expert_utility` + `ewma_step` + `deadband_pick` + `recent_pick` write. **`select_expert` becomes: enumerate candidates → fill `MOE_CAND[]` → call `moe_select_step`.** The cert calls `moe_select_step` with a scripted `MOE_CAND[]` and the real `gacc` step. Removing the §8 write-gate then provably reddens CI because the cert exercises the shipped function, not a clone.

> Acceptance gate for Wave 0 alone: `st_test_oscillation` ([moe-osc]) and `[moe-twolayer]`/`[moe-nocentral]` produce **byte-identical output** before and after the refactor (the refactor is behaviour-preserving). No new numeric claim in Wave 0.

### 1.1 `[moe-gacc-osc]` — disease-first, production-self-test, `[in-proc]`

New `st_test_gacc()` registered in `moe_self_test()` at moe.c:1330-block, modeled on `st_test_oscillation` (moe.c:842) but driving the **coupled** axis.

**The disease (C1 — reproduce in numbers FIRST).** 3 candidates, identical seed merit, 30 decisions, via `moe_select_step`. The plant couples BOTH axes the production sum couples: when candidate `k` is over-picked, (a) its `recent_pick` load rises (existing fast axis) AND (b) its *next-round observed outcome* degrades to `target=0` while an unchosen candidate recovers to `100` (the new `gacc` axis). Count `gacc`-argmax inversions over the steady tail (decisions 10–30) AND total route switches.

**Two runs in ONE harness, the decimation flag the ONLY toggled variable (C5 unfixed-control):**

| arm | gacc write cadence | LR / dead-zone |
|---|---|---|
| **NAIVE** | every decision | `GACC_LR_DIV=1`, dead-zone OFF |
| **STABILIZED** | once per 10 decisions (slow tick) | `GACC_LR_DIV=4`, `GACC_DEADBAND=8` |

**PASS/FAIL NUMERIC GATE (auditor-authored final; this is the starting formula, mirrors [moe-osc] moe.c:852-868):**

```
G1 (disease real, C1):     naive_switches   >= 18
G2 (cure, C2):             stable_switches  <= 6   AND  stable_switches*2 <= naive_switches
G3 (gacc-axis settles):    naive_inversions >= 12  AND  stable_inversions <= 4
                                                   AND  stable_inversions*2 <= naive_inversions
G4 (separation on the COUPLED sum, C3 — the cert-first graft):
        Measure 2-tick peak-to-peak of the FULL util_ewma feeding the pick
        (pressure + gacc together, NOT gacc alone):
           util_pp_slow * 3 <= util_pp_naive
        AND the gacc write is invoked exactly floor(decisions/10) times
        vs `decisions` reflex selections  ->  ratio == 10  (τ-ratio witness)
G5 (converges, C4):        over the last 10 decisions, reexc == 0 (no re-excursion)
```

If `naive_switches < 18` OR `naive_inversions < 12`, the disease was NOT reproduced → **FAIL** (guards against a toy plant that can't ring, exactly like [moe-osc] line 852). Setting `GACC_LR_DIV=1` or removing the slow-tick gate flips the stabilized arm into the naive slot and reddens G2/G4 — so the §8 stabilizer is load-bearing, not decorative.

> **G4 is the heart of the plan.** It is the carried-forward minimal-diff killer objection: it forbids certifying the gacc axis in isolation. The peak-to-peak MUST be measured on the summed utility the pick actually uses.

### 1.2 `[moe-nocentral]` extension — structural NOCENTRAL survives the §7 swap (D5)

Extend `st_test_nocentral` (moe.c:683–732, which already FAILs on global-argmax collapse at moe.c:717-721). After feeding 3 observer nodes DIFFERENT local outcome streams into their private `gacc`, assert `pick[0..2]` are not all equal. Global-argmax regression → all same → FAIL. The byte-equal honest-degrade sub-check (cert-first graft): with zero samples, `gacc_valid==0` and the candidate's effective `acc` is **byte-identical** to the old seed read — proving `gacc` is an override only on observed peers, and a never-observed peer still routes.

### 1.3 `[moe-protect]` re-verification (A3 sign-flip guard)

`expert_utility` still ADDS threat / SUBTRACTS pressure (moe.c:197-207, confirmed unchanged). The `gacc` value now feeds the `acc` slot. Re-run `st_test_protect` (moe.c:983) and assert the learner cannot drive a threatened node's effective merit so low it gets abandoned — the rally axis must dominate `gacc` drift. Add an unfixed-control arm proving the protect property holds with the `gacc` learner disabled.

### 1.4 `[live]` row — kept OPEN in the gap-ledger (B1/B2/B3 — the strong form)

The `[in-proc]` green above is **necessary, not sufficient**. The strong gate, tagged `[live]` and kept OPEN until its CI job is green N≥5:

> ≥2 OS processes over the relay, N≥5, kill `-9` a peer mid-flood; assert each survivor's `gacc` re-homes within τ_slow without re-excursion, the survivors' picks diverge (no stampede onto one node), AND — the cert-first mandate — **confirm on mk_pino's phone that the outcome signal `(remote-class==learned_class)` has real dynamic range under flood** before the cure is credited (in-proc green / dead live path is the salty-bug failure mode). Ledger row: `[in-proc] PASS / [live] OPEN`. Never silently promote.

---

## 2. MINIMAL CODE SPEC

### 2.1 The local-gradient gating update rule (`gacc`)

State, beside `peer_scores`/`my_accuracy` (moe.c:84 region) — fixed dims, **no-VLA**:

```c
static W  gacc[DNODE_MAX][MOE_NUM_CLASSES];        /* learned per-observer merit, [0,100] */
static UB gacc_valid[DNODE_MAX][MOE_NUM_CLASSES];  /* 0 until first local sample */
static UB pend_node = 0xFF, pend_cls;              /* one pending route outcome */
static W  pend_target;
```

**Hot-path read** — the ONLY edit to the selection arithmetic, at the `acc` source (moe.c:308–309). After Wave 0 this is inside the `MOE_CAND[]` fill:

```c
W accw = gacc_valid[n][gate_class] ? gacc[n][gate_class]
                                   : (is_self ? (W)my_accuracy[gate_class]
                                              : (W)peer_scores[n].accuracy[gate_class]); /* SEED ONLY */
UB acc = (UB)(accw < 0 ? 0 : accw > 100 ? 100 : accw);
```

`expert_utility` (moe.c:197-207), EWMA (moe.c:321-324), `deadband_pick` (moe.c:259-270), `recent_pick` (moe.c:374) **all unchanged**.

**Local error signal** — set the pending slot from the locally-observed route outcome in `moe_infer`. Grafting the minimal-diff signal that survives the regions-r3 aliasing trap: the signal is taken **only on the successful-remote branch** (moe.c:572, the single branch producing an independent `result_class`), never on the fallback (moe.c:577) which aliases to `learned_class`:

- successful remote (moe.c:570-574, `er==E_OK`) AND `cls == learned_class` → `pend_target = 100` (meritorious helper)
- successful remote AND `cls != learned_class` → `pend_target = 0` (disagreed — honest demerit)
- remote **timeout/fallback** (moe.c:575-579, `er != E_OK`) → `pend_target = 0` (the helper FAILED us — *not* "agreed")
- self-pick answered locally (moe.c:563) → `pend_target = my_accuracy[gate_class]` (own measured ratio)
  Set `pend_node`/`pend_cls`/`pend_target` once, near moe.c:580.

> This is the explicit fix for the regions-r3 killer objection: a fallback is scored 0 (failure), NOT 100 (agreement-with-self). A dead/slow peer is demerited, never rewarded.

**The descent step** — lives ONLY in the slow tick, immediately after `update_my_accuracy()` (moe.c:587). This is the **only writer** of `gacc`:

```c
if (pend_node < DNODE_MAX) {
    W *g = &gacc[pend_node][pend_cls];
    if (!gacc_valid[pend_node][pend_cls]) { *g = pend_target; gacc_valid[pend_node][pend_cls] = 1; }
    else if ((pend_target - *g > GACC_DEADBAND) || (*g - pend_target > GACC_DEADBAND))
        *g += (pend_target - *g) / GACC_LR_DIV;   /* integer single-pole IIR — the gradient step */
    pend_node = 0xFF;
}
```

### 2.2 The §8 two-timescale / hysteresis stabilizer

The whole anti-oscillation argument is **WHERE the two operations attach** — and (the cert-first graft) the claim is only valid because the cert proves separation on the *coupled* sum, not the gacc axis alone.

- **τ_fast = MOE_REFLEX_TICK_MS = 200ms** (moe.h:55, confirmed). The reflex layer (`select_expert`/`moe_select_step`) **READS `gacc` but is FORBIDDEN to write it.** Its existing fast damping — `recent_pick` decay ×2/3 (moe.h:123-124), util EWMA α=1/4 (moe.h:142), `MOE_SWITCH_MARGIN=12` dead-zone (moe.h:143) — is 100% intact. The fast loop closes the immediate response on local virtual-load and never rides the slow signal.
- **τ_slow = MOE_DELIB_TICK_MS = 2000ms** (moe.h:56, confirmed) — a clean 10× decimation. The `gacc` step lives ONLY here, with its own hysteresis: IIR divisor `GACC_LR_DIV=4` (one bad outcome moves the estimate by at most `(100-g)/4`) + `GACC_DEADBAND=8` dead-zone (skip the step near equilibrium → no chatter).

New `#define`s in the §8 block (moe.h:143, beside `MOE_SWITCH_MARGIN`):

```c
#define GACC_LR_DIV    4   /* slow-layer learning rate 1/4 (IIR hysteresis) */
#define GACC_DEADBAND  8   /* skip the step if |target-g| <= 8 (dead-zone)   */
```

**Why this damps the COUPLED ring (the corrected argument):** the merit signal the swarm routes on changes 10× slower than the routing decisions it drives, AND it sits behind a dead-zone. The fast `応援殺到↔引き上げ` scramble on the *pressure* axis is the loop that `recent_pick`+EWMA+deadband already damps (the shipped [moe-osc] cure). The `gacc` axis adds a *second slow* input to the same sum; because it is decimated 10× + IIR + dead-zoned, it cannot inject a fast oscillation of its own, and G4 of the cert proves the **summed** peak-to-peak is low-passed, not just the gacc term. If a future edit lets the fast loop write `gacc` (collapsing the two writers to one rate), G4 reddens.

---

## 3. EXACT FILE:LINE TOUCHPOINTS

| # | file:line | change |
|---|---|---|
| 0 | `arch/common/moe.c:279-375` | **Wave 0 refactor:** extract pure `moe_select_step(MOE_CAND*, ncand, gate_class, incumbent_io)` from `select_expert`'s candidate loop; `select_expert` enumerates → fills `MOE_CAND[]` → calls it. Behaviour-preserving (cert §1.0). Declare `MOE_CAND` in `moe.h`. |
| 1 | `arch/common/moe.c:84` (region) | declare `static W gacc[DNODE_MAX][MOE_NUM_CLASSES]; static UB gacc_valid[...][...]; static UB pend_node=0xFF, pend_cls; static W pend_target;` — fixed dims, **no-VLA** |
| 2 | `arch/common/moe.c:308-309` | hot-path read: `gacc` with seed-fallback to old tables only when `!gacc_valid` (inside the `MOE_CAND` fill after Wave 0). `expert_utility`/EWMA/`deadband_pick` untouched |
| 3 | `arch/common/moe.c:563` | self-pick branch: `pend_* = (..., my_accuracy[gate_class])` |
| 4 | `arch/common/moe.c:570-574` | successful-remote branch (`er==E_OK`): `pend_target = (cls==learned_class) ? 100 : 0` |
| 5 | `arch/common/moe.c:575-579` | fallback/timeout branch: `pend_target = 0` (failure, NOT agreement — the regions-r3 fix) |
| 6 | `arch/common/moe.c:587` | the §8 SLOW-TICK gradient step, right after `update_my_accuracy()` — the ONLY writer of `gacc`, with `GACC_DEADBAND` guard + `GACC_LR_DIV` IIR |
| 7 | `arch/common/moe.c:614-622` | `moe_init`: zero `gacc[][]`, `gacc_valid[][]`, `pend_node=0xFF` (cold-start determinism) |
| 8 | `arch/common/include/moe.h:143` | add `#define GACC_LR_DIV 4` / `#define GACC_DEADBAND 8` in the §8 hysteresis block; declare `MOE_CAND` struct |
| 9 | `arch/common/moe.c:683-732` | extend `st_test_nocentral`: per-node-divergent `gacc` still yields divergent picks (§1.2) |
| 10 | `arch/common/moe.c:842` region + `:1330` | new `st_test_gacc()` (naive-oscillates vs stabilized-settles; switches + inversions + COUPLED-sum peak-to-peak + ×10 witness); register in `moe_self_test` |
| 11 | `arch/common/moe.c:983` | re-run/extend `st_test_protect` with a gacc-disabled unfixed control (A3) |

**Not touched in this plan (deliberately):** `gossip_learn.c` — `gacc` is purely local, never gossiped or merged (preserves `gl_check_no_central` at gossip_learn.c:389-420 by construction). `r3_incontext.c` `rw[]` core — intact. `eff_pressure`/`eff_threat` (moe.c:224-242) — already local-gradient, kept verbatim.

> Note on the regions-r3 graft: that design proposed calling a routing learner from `gossip_learn.c:1055-1060` (the GL_SLOW_BAND_MS=2000 tick, confirmed) to couple routing-learning with weight-merge. We **defer** that coupling (§6) — it is a second, heavier claim that needs its own cert and its own NOCENTRAL re-proof. The §7 本丸 ships first as local-only `gacc`.

---

## 4. INVARIANTS HONORED

- **NOCENTRAL:** the decision reads only `gacc[n][c]`, a table THIS node owns and writes solely from outcomes IT personally experienced. No peer's `gacc` is read; `broadcast_score` (moe.c:396-408) is demoted to cold-start seed + observability. Two nodes leaning on the same peer hold DIFFERENT `gacc` (different RTT/timeout experience) — per-observer pheromone, never a vote. `st_test_nocentral`'s global-argmax detector (moe.c:717-721) is the structural guard. There is no merit oracle to kill (survival-network.md:177-178 has no target).
- **Determinism / one-math (-ffp-contract=off):** `gacc` arithmetic is INTEGER `W` (IIR via integer divide, same idiom as `ewma_step` moe.c:247). No transcendental in the hot path; no cross-node float reduction → no salty-bug FMA surface (D1/D2). Build with `-ffp-contract=off` on every target; the cert is a precondition, not a nicety.
- **No-VLA:** `gacc`/`gacc_valid` are fixed `[DNODE_MAX][MOE_NUM_CLASSES]` `.bss`; `MOE_CAND[]` scratch in `select_expert` is bounded by `DNODE_MAX` (a compile-time constant), never a runtime dim. `[no-vla]` tripwire stays green (D4).
- **R3 `rw[]` core intact:** `r3_incontext.c` untouched; `gacc` is not routed through `gl_merge`.
- **Honest degrade:** a never-routed peer keeps `gacc_valid==0` and falls back byte-identically to the old seed (cert §1.2). Sparse-signal peers are honestly un-tracked, not silently zeroed.

---

## 5. WAVE SEQUENCE (small, falsifiable, SS/G-style)

**Wave G38.0 — the seam (smallest thing that makes the cert *possible*). ✅ DONE (`9f7a9bc4`, separate audit PASS / CLOSE).** Refactored pure `moe_select_step(MOE_CAND*, ncand, gate_class, incumbent_io)` out of `select_expert` (touchpoint 0); `select_expert` enumerates → fills `MOE_CAND[DNODE_MAX]` → calls it (one production call site). Behaviour byte-preserved (existing `[moe-osc]` naive=28/stabilized=4, `[moe-twolayer]` ratio=10, `[moe-nocentral]`, `[moe-protect]` all byte-identical pre/post, cross-ABI). New `[moe-seam]` cert (`st_test_seam`, 5 scenarios + sabotage→RED) drives THE SAME function production calls → trap A2 closed (no clone left in `select_expert` to route around). Scope held to Wave 0: **NO gacc / NO learning / NO §8 stabilizer** — those are G38.1+, on hold pending mk_pino's review of his core §7 philosophy. *This wave exists solely to close trap A2 — without it every later cert is a rubber stamp.*

**Wave G38.1 — the smallest thing that makes the cert PASS (`[in-proc]`).** Add `gacc` state + read + slow-tick step + the two `#define`s (touchpoints 1–8). Add `st_test_gacc()` (touchpoint 10) and the `[moe-nocentral]` extension (touchpoint 9). Acceptance: gates **G1–G5** green; `[moe-nocentral]`, `[moe-twolayer]`, `[moe-osc]` still green. This is the first wave that closes the §7 本丸 `[in-proc]`.

**Wave G38.2 — sign-flip / protect re-verification (A3).** Touchpoint 11: `st_test_protect` with the gacc-disabled unfixed control. Acceptance: rally axis dominates gacc drift; a threatened node is never abandoned.

**Wave G38.3 — the `[live]` kill-test (the strong form, B1).** ≥2 processes over the relay, N≥5, kill `-9` mid-flood; survivor `gacc` re-homes within τ_slow, picks diverge, no re-stampede; outcome-signal dynamic-range confirmed on-device. Until green N≥5, the gap-ledger row reads `[in-proc] PASS / [live] OPEN`.

---

## 6. DEFERRED / OUT OF SCOPE

- **Full deletion of the `MOE_SCORE` K-DDS broadcast topic + `peer_scores[]`** (the regions-r3 graft's strong form). Kept alive here as cold-start seed only. Deleting the topic, the `broadcast_score()` publisher (moe.c:396-408), and the subscribe loop (moe.c:578-585) is a separate wave AFTER `gacc` is `[live]`-proven — it changes the wire protocol and needs its own migration-chain cert.
- **Coupling routing-learning to the G22 weight-merge round** (regions-r3's `gossip_learn.c:1055-1060` call site). The "routing learns on the same heartbeat the weights average" mutual-aid coupling is a second claim with its own NOCENTRAL re-proof; deferred.
- **Exploration for sparse/cold peers.** A rarely-routed peer keeps its seed merit. Occasional exploration to gradient-track cold peers is a separate wave and must NOT be added to the hot path.
- **Graded (non-binary) outcome reward.** `gacc` targets are `{0,100}` for determinism. A graded reward (slow-but-correct peers) is deferred; `expert_utility`'s RTT penalty (moe.c:201) still covers slowness in the interim.
- **`gacc` as float.** If anyone later makes `gacc` a float, the salty-bug FMA class (D1) reopens — explicitly out of scope; integer-only is a hard requirement.
- **Guard→learn Arrow 2 activation** (`reflex_threat_experience`, reflex.c:382-386). Orthogonal to §7 routing; separate wave.

---

## 7. OPEN RISKS / UNKNOWNS THE IMPLEMENTER MUST WATCH

Every unresolved killer objection, carried forward honestly:

1. **(minimal-diff killer, half 1 — A2) The cert seam is the whole ballgame.** If Wave G38.0 is skipped or `moe_select_step` is not the EXACT function `select_expert` calls, `st_test_gacc` certifies a reconstruction and "removing the gate reddens CI" becomes false. The auditor must confirm by inspection that production and cert share the function, e.g. by deleting the slow-tick gate and watching CI go red.
2. **(minimal-diff killer, half 2 — the COUPLED sum, carried into G4) A slow bias does NOT low-pass a fast oscillation in a shared sum.** `recent_pick` (fast) and `gacc` (slow) are summed in one `expert_utility` call. G4 MUST measure peak-to-peak on the **summed** util, not the gacc axis alone. If the auditor measures gacc in isolation, the green is a rubber stamp and the coupled ring may still resonate. This is the single highest-risk item.
3. **(regions-r3 killer — A4, the objective inversion) The outcome signal must distinguish fallback from agreement.** Confirm at moe.c:563/577 that `result_class==learned_class` is set on the local AND fallback branches; the `pend_target` for fallback MUST be `0`, derived from `er != E_OK` (the timeout), NOT from a `result_class==learned_class` comparison. A regression here silently inverts the learner toward "reward dead peers." Add an explicit cert sub-check: feed a candidate that always times out and assert its `gacc` descends toward 0, not 100.
4. **(B1/B3 — in-proc is not live) An `[in-proc]` green is necessary-not-sufficient.** The distributed anti-oscillation and re-homing claims close ONLY at the `[live]` N≥5 kill-test. Keep the row OPEN; do not promote silently (G27/G32's whole lesson).
5. **(salty-bug — D1, dead live path) Confirm the outcome signal has real dynamic range under flood on-device** BEFORE crediting the cure. In-proc the plant guarantees the signal swings; on mk_pino's phone it may not, exactly the wave-49 trap (gradients right, convergence dead).
6. **(A3 sign-flip) `gacc` reads the `acc` slot that feeds the same sum as threat/pressure.** Any future wave that flips the threat/load sign must re-aim the `gacc` objective too. Grep every reader of `eff_pressure`/`eff_threat`/the route outcome before crediting a sign change. `[moe-protect]` with a gacc-disabled control is the guard.
7. **(tuning, not robustness) `GACC_LR_DIV=4` / `GACC_DEADBAND=8` are hand-fit to the herd harness.** The 10× decimation is the robust part; the magnitudes are not fleet-certified. If the live flood has different gain, re-fit — but never re-fit by weakening the cert gate.
8. **(A1 discipline) The architect wrote this gate; a SEPARATE auditor must own the final PASS/FAIL formula and run it.** Do not let the implementer also define success. The commander reads the gate formula (G1–G5) line-by-line before any code is credited.