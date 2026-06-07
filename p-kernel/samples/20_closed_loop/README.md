# 20 — Closed control loop (鎖を環にする)

Wave 12. The audit (`docs/architecture/philosophy-gap-audit-2.md`) found the
survival pieces were all green individually but wired as **separate green
chains, not one closed control loop**. This sample proves the ring is now
closed — *with numbers*.

```
        ┌────────────────────── learning (slow, §9 G18) ──────────────────────┐
        │                                                                      │
   thought (§7 MoE gate) ──G17──▶ action (§8 reflex) ──▶ perception (world      │
        ▲                          CONSERVE/SHIELD/BEACON   pressure) ──┐       │
        │                                                               ▼       │
        └──────────── gate re-routes load (negative feedback) ◀── §7 gate ──────┘
```

## Run

```sh
./loop.sh        # exit 0 = loop closed; non-0 = a link is broken
```

Single node, no relay/net needed: `moe_infer` takes the local path so the G17
hook fires from the gate's own inference completion, and `reflex test` is
pure-local deterministic integer math (bit-identical every run, every ABI).

## What it asserts

**(a) G17 — the action half-loop.** Until now `reflex_on_inference` was called
only from the `dtr` path; the expert chosen by the §7 MoE gate produced a class
label that went nowhere. Now `moe_infer`'s completion point also drives the
reflex layer. The demo shows a `critical` MoE inference producing
`[reflex] FIRE class=2 -> CONSERVE BEACON`, while a `normal` inference stays
silent (reflex is default-enabled but does nothing on class 0).

**(b) Negative feedback that converges (not feedforward).** The reflex action
CONSERVE adds a bias to this node's gossiped *pressure* (exactly as
`world.c::compute_pressure` does: `p += reflex_pressure_bias()`), which the §7
gate reads (`moe_expert_utility`) and uses to route load to a less-pressed
neighbour. Shedding load clears the overload that was producing the threat — one
full turn of the ring. `reflex test` injects a disturbance burst and reports:

| run                         | dwell | settle | iae | reexc | tailvar |
|-----------------------------|------:|-------:|----:|------:|--------:|
| loop ON (action→perception) |     1 |      7 |  78 |     0 |       0 |
| loop OFF (feedforward)      |    13 |     19 | 600 |     0 |      12 |

With the loop closed the threat decays to a steady state (short dwell, fast
settle, ~0 steady-state residual, zero re-excursions = no oscillation). With the
loop open (the action never reaches perception) the *same* disturbance lingers.

**(c) G18 — deliberation→learning→reflex.** The slow deliberation tick
(`reflex_deliberate`, run from `reflex_task` on a slow sub-tick) reads the
accumulated experience — how long threats *dwell* — and nudges the learned
CONSERVE gain (`learned_conserve`) up or down. No external `dtr train`: the
reflex rewrites itself from live observation. Starting from a deliberately weak
gain, the indicator improves over episodes (`dwell 13 -> 3`) and converges,
while a frozen gain stays stuck at `13 -> 13`.

## What is still feedforward (honest)

- The closed loop measured here couples **reflex → pressure → gate** on a single
  node. The cross-node beacon path (BEACON → neighbour's attenuated CONSERVE) is
  wired and demonstrated by `15_reflex/reflex_demo.sh`, but its *swarm-level*
  convergence is not yet measured here.
- The G18 learner adapts one scalar (the CONSERVE gain). Per-expert weights are
  still only updated by the explicit `dtr train` path; the deliberation layer
  does not yet retrain the experts from live error.

See `docs/architecture/closed-loop.md` for the full design and measurements.
