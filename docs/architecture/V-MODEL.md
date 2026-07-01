# V-MODEL — the design↔verification spine of p-kernel

> **Phase 0 (placement mapping).** This file is being built in phases. This first
> section is the **canonical placement mapping**: every hot architecture doc → its
> V-level. The V spine, level definitions, and the design↔verification
> correspondence table are added in a later phase.

The V-model's two arms — **left = design** (why → requirements → architecture →
module) and **right = verification** (module test → integration → system →
acceptance) — are joined rung by rung: *every design claim on the left has a
falsifiable cert on the right at the same level.* That horizontal correspondence
is not a foreign template bolted onto p-kernel — it is **exactly the ethos this
project already runs on**: the [gap-ledger](gap-ledger.md) + the `[live]` /
`[in-proc]` verification tiers + CI enforcement. A design doc with **no
corresponding cert is a broken V-rung = a gap.**

## Placement mapping (doc → V-level)

Numeric prefixes give the natural left-arm-down / right-arm-up order.

| level (dir) | meaning | docs |
|---|---|---|
| `00-concept/` | **L0** なぜ / CONOPS — mission & why | `survival-network.md` |
| `10-requirements/` | **L1** システム要件 — strategy/invariants as requirements | `compatibility.md`, `compat-migration-chain-plan.md` |
| `20-architecture/` | **L2** アーキテクチャ — high-level structure | `regions.md`, `reflex-deliberation.md`, `reflex-action.md`, `p-fs.md`, `decentralized-lookup.md`, `inference-engine.md`, `memory-thought.md`, `survival-loop.md`, `federation.md`, `connect-anywhere.md`, `p2p-overlay.md`, `closed-loop.md`, `r3-nontrivial-thought.md`, `r3-model-widening.md`, `base-model-survey.md`, `moe-distillation-survey.md`, `native-student.md`, `full-smp-plan.md` |
| `30-module/` | **L3** 詳細設計 — per-subsystem mechanism | `genome.md`, `ark-profile.md`, `galaxy.md`, `persistence.md`, `dynamic-id.md`, `supernode-autopromote.md`, `relay-ha.md`, `survival-fs.md`, `self-access.md`, `device-capacity.md`, `gpu-compute.md`, `gpu-3-wiring.md`, `webd-user-space.md`, `conversation.md`, `conversational-teaching.md`, `death-piercing.md`, `fault-recovery.md`, `interoception.md`, `living-body-inspector.md`, `r3b-breathing-params.md`, `living-mind.md`, `special-structure-mind.md`, `n1-lan-direct-plan.md`, `student-blob-transport.md`, `survival-g38-impl-plan.md` |
| `40-verify/` | **right arm** — integration / system / acceptance scenarios | `composite-scenarios.md` |
| `50-evolution/` | **outside the classic V** — runtime self-modification (the V that runs on itself) | `ring3-core.md`, `selfc-ring3.md`, `self-compile.md` |
| _(root, meta)_ | index / ledger / spine — not a rung | `README.md`, `INDEX.md`, `V-MODEL.md`, `gap-ledger.md`, `BACKLOG.md`, `review-2026-06-20-harsh.md` |
| `archive/` | past V-iterations (年輪) — untouched | (frozen) |

50 hot docs placed into 6 levels; 6 meta docs stay at the root; `archive/` is the
spiral's earlier turns and is not re-filed.
