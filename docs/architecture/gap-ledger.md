# Gap Ledger — the single living list of OPEN gaps (canonical)

> One table. The gaps that are **open RIGHT NOW on master**. Proof of progress is
> **rows DECREASING**, not documents increasing.
>
> **Rule of this file (the antidote to audit-sprawl):**
> 1. This table only **shrinks**. New findings **replace** closed rows; they do not lengthen it.
> 2. When a gap ships AND is CI-enforced, **delete its row** (move its epitaph to "Closed" below, one line).
> 3. **Do NOT spawn `philosophy-gap-audit-9`.** v1..v8 are the frozen historical trail
>    (see `README.md` and the banner atop each). The canonical open list is *here*.
> 4. Keep it short. If this file ever rivals the learning code again, it has become the disease it tracks.
>
> Reconciled across `philosophy-gap-audit-{,-2..-8}.md` + `../review-2026-06-three-brains.md`.
> Verified on master `803a465`. Severity: 🔴 contradicts the core · 🟡 weakens it.

---

## OPEN (this is the whole list)

| id | one-line | sev | evidence it is still open (file:line on master) | "closed" means |
|---|---|---|---|---|
| **3BRAIN** (review #1·#3; G38-incomplete; G34; gate-ladder) | `moe_infer` runs three different brains: it **routes** with a temp if-ladder that discards 3 channels, **returns** the handwritten-constant MLP, and **guards** with the learned dtr. Returned ≠ guarded. "Collective learning makes us smarter" lands in a brain the output never reads. | 🔴 | `moe.c:108-114` (gate if-ladder, `(void)hum/press/light`), `moe.c:394/442` (returns `mlp_forward`), `ai_job.c:42-49` (handwritten MLP constants on the return path), `moe.c:430/440` (guards with dtr `learned_class`) | Returned == guarded == routed, all from **one** dtr forward; gate if-ladder and `ai_job.c` constants deleted; all 4 channels affect the output. **(wave-18-A; acceptance test below.)** |
| **R3** | The "thinking" is a 635-param, 3-class, 4-channel toy whose routing answer is analytically a 3-line temp threshold. Even with the neurons wired, the *content* of thought is trivial. | 🔴 | `dtr.h:168` (635 params), `dtr.h:34` (3 classes), `dtr.h:37` (4ch), `moe.c:107-114` (analytic temp threshold) | A non-trivial task/model where learned beats frozen by a margin that does **not** collapse to a hand-written if. |
| **G23** | `DNODE_MAX=32` caps the collective brain at 32 nodes — contradicts UMP "every install = a node". | 🔴 | `drpc.h:35` (=32), `drpc.h:103` (table), `drpc.h:23` (8-bit id), `gossip_learn.c:243/493` (`GL_MAXNODES` assumed) | Node ceiling raised past 32 in code, with a live test exercising >32 participants. |
| **G33** | Reflex threat *level* is released by a 5 s wall-clock timer + a clamped scalar nudge, not by a controlled quantity. | 🟡 | `reflex.c:304-311`, `reflex.h:60` (`HOLD=5000`), `reflex.c:321-352` (scalar nudge) | Threat level rises/falls with the controlled variable, not a timer. |
| **G13** | Cross-region inference is re-serialized by the coordinator's fixed 200 ms blocking window. (Partly probed by `parallel-infer-live`; residual remains.) | 🟡 | `dkva.c:252-269/640`, `dkva.h:66` (`WIN=200`) | Cross-region inference not gated by a fixed blocking window. |
| **AUDIT-SPRAWL** (review #5) | The self-audit became a second product (v1..v8 ≈ 2431 lines, rivalling the learning code); gaps were *versioned*, not *closed*. | 🟡 | `philosophy-gap-audit{,-2..-8}.md` (8 files) | This ledger is the sole open list; rows only shrink; no v9 is ever spawned. Closes by sustained discipline. |

**Open rows: 6.** Every other G-number is closed (below).

---

## Closed (epitaphs only — one line each; do not re-litigate)

CI-enforced and shipped, therefore removed from the open table:

- **G20** rally/two-axis gate (flee→approach) — CI `[moe-protect] PASS` `ci.yml:68`, `27_protect`.
- **G28** grounded protect (guarded object is first-class; actuator drives threat→0; survives owner kill) — CI `[protect-ground]/[protect-loop]` `ci.yml:75-76`, live `protect-loop-live` `ci.yml:202`.
- **G35** plural protect (many points defended in parallel; survives owner kill) — CI `[plural-protect]` `ci.yml:79`, live `plural-protect-live` `ci.yml:232`, `28_plural_protect`.
- **G22** collective > individual (disjoint shards, no-central gossip avg, survives kill+rejoin) — CI `[g22-gossip-learn]` `ci.yml:86`, live `collective-learn-live` `ci.yml:263`, `32_collective_learn`.
- **G24** durable memory / ARK FS (content-addressed, versioned, crash-safe, power-cut survival on real HW) — `23_durable`, `ark-crash-fuzzer` `ci.yml:358`, `arkfs-audit.md`.
- **G27 / G32** live-path CI gates (self-test green is not promoted; live N≥3 + kill is the gate) — the live jobs above are the institution.
- **G29** + the earlier gaps **G1–G19, G21, G25, G26, G30, G31, G36, G37** — closed across waves 1–16; full provenance in `philosophy-gap-audit-{,-2..-8}.md`.

> Note on **G38**: it *landed as code* (the `0xFF` dead gate is gone — `moe.c:440` now passes real
> dtr confidence) but it landed **fake-coupled** — the review's #1. It is therefore NOT closed; it
> lives in the open table as the **3BRAIN** row. Real confidence on the *wrong brain* is not coupling.

---

## Acceptance test — wave-18-A (three-brain unification)

Hand this to the commander to verify wave-18-A before calling 3BRAIN closed. Tokens are
proposals (`grep -aF`-able); naming is the implementer's. REAL convergence is **all** of:

- **One brain, one answer.** `moe_infer`'s **returned** class == the **guarded** class ==
  the **routing/gate** class, all derived from a **single** dtr forward pass.
  → `[w18a-unified] returned==guarded==routed PASS` (in-kernel deterministic self-test).
- **All four channels matter.** Perturb each of temp/hum/press/light independently; the output
  changes for each. The `(void)hum/press/light` discard is **gone**.
  → `[w18a-channels] all-4-affect-output PASS`.
- **Learning lifts the RETURNED accuracy.** G22 collective gossip improves the class `moe_infer`
  *returns to the caller*, not only the guard signal. learned > frozen on the **return path**,
  live N≥3 through `./relay`, kill injected mid-flight, ≥5× non-flaky.
  → `[w18a-learn-lifts-return] collective>solo on RETURNED class PASS` + `RESULT: PASS` ×5.

**FAKE tells (any one present ⇒ NOT converged):**

- `gate_predict`'s temp if-ladder still live → `grep -nE 'if \(temp < 20\)' arch/common/moe.c` hits.
- Handwritten MLP constants still on the return path → `grep -n 'temp HIGH' arch/common/ai_job.c`
  hits **and** `mlp_forward` still feeds `result_class` (`moe.c:394/407/442`).
- Returned ≠ guarded → `result_class` and `learned_class` are two variables from two forwards.
- A channel still discarded → `grep -n '(void)hum' arch/common/moe.c` hits.

> Until every REAL token is green AND every FAKE tell is absent, wave-18-A is "three brains
> standing next to each other," not one. Do not delete the 3BRAIN row before then.
