# Closed control loop — 鎖を環にする

Wave 12. Status of the survival "考える器官" after this wave: the individually
green pieces (reflex §8, MoE gate §7, deliberation/two-layer §8, world map §6)
are joined into **one closed control loop**. This doc records the wiring, the
measured negative-feedback convergence, the deliberation→learning loop, and —
honestly — what is still feedforward.

Source of truth for the audit: `archive/philosophy-gap-audit-2.md` (G17, G18). Those
lines are owned by the auditor; this doc only describes the implementation.

## 1. The chain that was, the ring that is

The second audit's finding was *"閉じた制御ループではなく、個別に緑の鎖"* —
three cuts:

- **(a) action was half-wired (G17).** `moe_infer` (the §7 gate that picks the
  most-effective expert) never called `reflex_on_inference`. Reflex fired only
  from the `dtr` inference path. The conclusion of the gate went nowhere.
- **(b) action did not change perception** — the system was feedforward, not a
  negative-feedback loop. The substrate existed (CONSERVE → pressure → gate) but
  was never closed and measured as a loop.
- **(c) the deliberation layer learned nothing (G18).** The slow layer existed
  (`moe_task`'s delib tick) but produced no learning that fed back to reflex.

```
        ┌──────────────────── learning (slow §9, G18) ───────────────────────┐
        │   reflex_deliberate(): dwell experience -> learned_conserve nudge   │
        ▼                                                                      │
  thought ──G17──▶ action ───────▶ perception ───────▶ decision ──────────────┘
  §7 MoE gate      §8 reflex       §6 world pressure    §7 gate re-routes load
  moe_infer()      CONSERVE        compute_pressure()   moe_expert_utility()
                   +bias on threat  p += bias            sheds to neighbour
                                                          │
                                          load drops ◀────┘  (negative feedback)
                                          threat clears
```

## 2. (a) G17 — closing the action half-loop

`moe_infer()` now calls `reflex_on_inference(result_class, 0xFF, drpc_my_node)`
at its completion point (`arch/common/moe.c`). The class produced by the chosen
expert is reinterpreted as a threat level and translated by the reflex action
table into real local defence (SHIELD / CONSERVE / BEACON).

No double-fire: `moe_infer` (mlp_forward / DRPC path) and `dtr_infer`
(`dtr_log_push` / TP completion) are separate shell commands; on a single node
only one fires per inference. Confidence is unknown on the MoE path, so `0xFF`
is passed (the reflex confidence gate treats `0xFF` as "unknown → allowed").

Demonstrated by `samples/20_closed_loop/loop.sh`: a `critical` MoE inference
emits `[reflex] FIRE class=2 -> CONSERVE BEACON`; a `normal` inference stays
silent.

## 3. (b) Negative feedback, measured

The loop body, all real code:

1. **action → perception.** `reflex_pressure_bias()` returns the (learned)
   CONSERVE gain while engaged; `world.c::compute_pressure()` adds it to this
   node's gossiped pressure (`p += reflex_pressure_bias()`). The action
   literally changes what the network perceives about this node.
2. **perception → decision.** `select_expert()` reads peer pressure via
   `world_peer_pressure()` and scores candidates with `expert_utility()`
   (`utility = accuracy − rtt/k − pressure/2 + region_bonus`).
3. **decision → plant.** A more-pressed node loses the utility comparison by
   more than `MOE_SWITCH_MARGIN`, so load is routed to a less-pressed neighbour.
   Shedding load clears the overload that produced the threat → threat subsides
   → CONSERVE releases (hysteresis) → pressure returns to baseline.

### Measurement (`reflex test`, `[reflex-fb]`)

`reflex_self_test()` (`arch/common/reflex.c`) drives a minimal but faithful
plant: the disturbance is a threat burst; the **perception→decision** half uses
the *real* `moe_expert_utility` and `MOE_SWITCH_MARGIN`; the **action→perception**
half uses the *real* additive CONSERVE injection. The only thing the test models
is the plant dynamics (load in/out). The single difference between the two runs
is whether the action reaches perception (`bias>0`) or not (`bias=0`,
feedforward).

| run                         | dwell | settle | iae | reexc | tailvar |
|-----------------------------|------:|-------:|----:|------:|--------:|
| loop ON (action→perception) |     1 |      7 |  78 |     0 |       0 |
| loop OFF (feedforward)      |    13 |     19 | 600 |     0 |      12 |

- **dwell** — ticks the node stays in the threat class. Closed loop: 1.
  Feedforward: 13.
- **settle** — first tick after which no further threat (settling time).
  Closed: 7. Feedforward: 19.
- **iae** — integral of the excursion (Σ|deviation|). Closed: 78. Feedforward:
  600 (~8×).
- **reexc** — re-excursions after first recovery (oscillation indicator). Closed
  loop = 0 → **converges without oscillation**. The §8 hysteresis (CONSERVE hold)
  is what prevents the "shed → relieved → re-overload → shed" chatter; the same
  hysteresis-vs-oscillation result is independently proven by `moe test`'s
  `[moe-osc]` (naive 28 switches → stabilized 4).
- **tailvar** — steady-state residual variance. Closed: 0 (fully settled).
  Feedforward: 12 (still decaying / never settled within the horizon).

This is the quantitative form of *"行動が知覚を変える負帰還であって、フィード
フォワードではない"*.

## 4. (c) G18 — deliberation → learning → reflex

The fast reflex layer reacts; the slow **deliberation** layer learns. Wiring:

- `reflex_on_inference` records each threat **episode's dwell** (length of the
  consecutive threat run) into a window accumulator — this is the *experience*.
- `reflex_deliberate()` runs on a slow time constant (from `reflex_task`, every
  `REFLEX_DELIB_EVERY` polls = 5 s, slower than the MoE delib tick). It reads the
  windowed average dwell and **nudges `learned_conserve`**: dwell longer than
  `REFLEX_DWELL_TARGET` ⇒ response too weak ⇒ raise the gain; shorter ⇒
  over-defending ⇒ lower it. Bounded to `[REFLEX_CONSERVE_MIN, MAX]` with small
  steps (a stable outer loop that tunes the *gain* of the inner negative-feedback
  loop).
- `reflex_pressure_bias()` returns `learned_conserve` (not the fixed constant),
  so the learned value actually changes the action, which changes perception,
  which changes routing — the experience rewrites the reflex behaviour. No
  external `dtr train` is involved; this is adaptation from live observation.

### Measurement (`reflex test`, `[reflex-learn]`)

Starting from a deliberately weak gain (`REFLEX_CONSERVE_MIN = 8`, below the
gate's offload threshold), with learning **on** the indicator improves and
converges over episodes; with learning **off** (gain frozen) it stays stuck:

```
learning ON : dwell 13 -> 3   (learned_conserve 8 -> 14, homeostasis at target)
learning OFF: dwell 13 -> 13
```

This is *"考える器官が経験から自分を書き換える環"* (§9), minimal but real.

## 5. What is still feedforward (honest)

- **Swarm-level convergence is not yet measured.** The single-node
  reflex→pressure→gate loop is measured here. The cross-node path (BEACON →
  neighbour's attenuated CONSERVE, `15_reflex/reflex_demo.sh`) is wired but its
  multi-node negative-feedback convergence is not yet quantified.
- **The learner adapts one scalar.** G18 here tunes the CONSERVE gain. The
  expert weights themselves are still updated only by the explicit `dtr train`
  path — the deliberation layer does not yet retrain experts from live error.
  Closing *that* (online error → expert weight update → reflex) is the next ring.
- **The plant in `reflex test` is a model.** The decision math
  (`moe_expert_utility`, `MOE_SWITCH_MARGIN`) and the action injection are real;
  the load in/out dynamics are a deterministic model so the proof is bit-stable
  and net-free. The live loop runs the same primitives over real
  world/gossip/gate state.

## 6. Files

- `arch/common/moe.c` — G17 hook in `moe_infer`; `moe_expert_utility` public
  wrapper (shared by the self-test, no duplicate gate math).
- `arch/common/reflex.c` — `learned_conserve` + dwell experience tracking;
  `reflex_deliberate()`; slow delib sub-tick in `reflex_task`;
  `reflex_self_test()` (`reflex test`).
- `arch/common/include/reflex.h`, `moe.h` — constants + public API.
- `samples/20_closed_loop/loop.sh` — the soul demo.
