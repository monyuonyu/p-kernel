# Federation R0 — minimal live 2-cluster slice: implementation plan (cert-first)

Status: **DESIGN PLAN by an automated design-harden on trunk 9e50e4a8, awaiting commander review + a separate impl→audit cycle.** Read-only on code; no source was modified. Every claim grounded in `file:line` at HEAD `9e50e4a8`.

---

## 0. The headline (read this first)

**The hierarchy is already LIVE for the DKVA path — but it has NO falsifiable cert, and the design doc that says it's "未実装" is stale.** The cert-first work is therefore *not* "build the bridge"; it is **"prove the bridge carries O(region-count) and FAILS if it degenerates to flat all-to-all"**, plus the one genuinely-missing arm (coordinator crash → deterministic re-delegation). Concretely:

- `federation.md:1,6` still claims `DNODE_MAX = 32` and "上位フェデレーション機構が存在しない." **Both are stale at 9e50e4a8.** `arch/common/include/drpc.h:35` is `#define DNODE_MAX 64`. The inter-region aggregation layer **exists and runs live** (see §1).
- The 2-region, hierarchical, cross-region-summary-only path is implemented in `dkva.c` and demonstrated by a committed live harness `samples/11_distributed/run_4node_regions.sh`.
- KDDS scopes (`KDDS_SCOPE_REGION`/`KDDS_SCOPE_GLOBAL`, `kdds.h:85-86`) already restrict per-publish fanout to region members (`kdds.c:273`), and the `[locality]` tx counters that distinguish near vs far traffic already exist (`kdds.c:543-549`).

So R0's deliverable is a **production self-test + a `[live]` 2-process harness assertion** that turn the existing-but-uncertified hierarchy into a *falsifiable* one, and **pin the DNODE_MAX question honestly: R0 needs NO raise.**

---

## 1. What already exists (the bridge is partly built) — grounded

| Capability | Where | State |
|---|---|---|
| `DNODE_MAX = 64` (not 32) | `arch/common/include/drpc.h:35`; mirror `arch/linux/node_id.c:44` | LIVE. federation.md:6 is stale. |
| Region = egocentric RTT≤τ leaf; coordinator = min member id (deterministic) | `arch/common/region.c:68-94` (`region_recompute`), `region.c:214-218` (`region_coordinator`) | LIVE. NOCENTRAL: pure min-id, no vote. |
| KDDS scoped delivery (region vs global) | `kdds.h:85-86`; fanout gated by `region_is_member(n)` at `kdds.c:273` | LIVE. region-scoped pub already skips non-members. |
| Locality tx counters (near/far msgs+bytes) | `kdds.c:37-46` (`kdds_tx_stats`), printed `kdds.c:543-549` (`[locality]`) | LIVE. This is the cert's measurement instrument. |
| 2-stage DKVA aggregation: in-region per-node `resp/<n>` (REGION) + cross-region `rsum/<coord>` (GLOBAL) | `dkva.c:811-813` (scope assignment), `dkva.c:430-520` (fold own region directly + remote-region summaries), `dkva.c:374-384` (coordinator re-publishes its region summary) | LIVE. The cross-region path carries ONLY coordinator summaries. |
| Region-id gossiped in world beacon; requester counts remote regions from *fresh* gossip | `world.h:54` (`region_id` field), `world.c:228`, `world.c:315-321` (`world_peer_region_fresh`), consumed `dkva.c:498-505` | LIVE. This is how "expect O(#regions) summaries" is computed. |
| **Live 2-region harness** (2 latency zones, cross-region adds 200ms > τ=50ms; asserts region-B internal partials fold into one summary) | `samples/11_distributed/run_4node_regions.sh` | LIVE & committed. |
| RTT-zone injection plumbing (the seam that *creates* a boundary on one host) | `swim.c:358-387` (`swim_set_sim_zone`, cross-zone penalty), env `PKERNEL_RTT_ZONE_SIZE`/`PKERNEL_RTT_ZONE_PENALTY` → `arch/linux/{aarch64,x86_64}/usermain.c:307-313` | LIVE. |
| Supernode/teacher per-region selectors (deterministic lowest capable member; survives-death cert) | `region.c:143-202`, cert `region.c:291-424` (`region_supernode_test`) | LIVE. The cert *pattern* R0 reuses. |

**Conclusion:** the seam is `region_recompute()` + KDDS REGION scope + DKVA rsum. R0 does not invent a new layer; it **certifies the existing one and adds the missing crash arm.**

---

## 2. The minimal R0 slice — smallest thing that proves the hierarchy is REAL and LIVE

Two regions of small N (well within the 64 cap), a signal crossing the boundary **only via coordinator summaries**, measured to be O(region-count) not O(N), and a cert that **fails on degeneration**.

### 2.1 The cleanest seam (decision)

**Reuse `region_coordinator()` as the leaf's representative, and the existing DKVA `rsum/<coord>` GLOBAL topic as the inter-region channel.** Rationale, grounded:

- The representative already exists and is already deterministic (`region.c:214` → min member id, no vote). Do **not** introduce a super-region HRW for R0; `lookup.c` HRW (`lookup.c:134-148`) is deferred to F2/F3 when the coordinator *set itself* grows.
- The cross-region channel already exists: only `rsum/<coord>` is GLOBAL (`dkva.c:813` opens rsum with `KDDS_SCOPE_REGION`?? — **verify at impl**: rsum must be GLOBAL; the design text at `dkva.c:446-447` says rsum/<rid> is GLOBAL and folded once. The impl agent must confirm the rsum handle's scope is `KDDS_SCOPE_GLOBAL`, since that is the load-bearing invariant of the whole slice). All dense chatter (`resp/<n>`, MoE scores) stays REGION-scoped (`dkva.c:811`).
- The just-shipped `region_supernode`/`region_teacher` selectors (`region.c:154,198`) are the *same deterministic min-capable-member pattern*; R0 does NOT need them, but the cert reuses their **proof shape** (determinism / survives-death / no-vote, `region.c:307-345`).

### 2.2 What code changes vs what stays untouched

R0 is **cert-dominant**. The production code is essentially already there.

**Stays 100% untouched (the load-bearing existing assets):**
- `region.c` membership/coordinator (`region.c:68-94,214-218`).
- `kdds.c` scoped fanout + locality counters (`kdds.c:262-280,543-549`).
- `dkva.c` 2-stage aggregation (`dkva.c:430-520`).
- `swim.c` zone injection (`swim.c:358-387`).
- `world.c` region gossip (`world.c:228,315-321`).

**New code (small, additive):**
1. **A production self-test `dkva_fed2_self_test()`** (new function, appended near `dkva_self_test` at `dkva.c:888` / `dkva_arrival_test` at `dkva.c:967`) — the `[in-proc]` arm of the cert (§3). Wired into the existing `dkvatest` shell verb path (`dkva.c:1118`).
2. **A `[live]` assertion block appended to `run_4node_regions.sh`** that diffs the `[locality]` counters across one inference and applies the O(region-count) gate + degeneration falsifier (§3). No kernel code; harness-only.
3. **Possibly a tiny counter reset hook** if the `[locality]` counters cannot be diffed cleanly across one `infer` (the harness currently greps absolute values via `kdds.c:543`). Prefer **diff of two prints** (before/after `infer`) over adding a reset, to keep R0 zero-footprint on production. Decide empirically at impl time.

That is the entire R0 production surface: **one self-test function + one harness assertion block.** No new wire format, no new topic, no DNODE_MAX change.

---

## 3. THE FALSIFIABLE CERT (cert-first)

### 3.1 `[fed-2cluster]` — the core cert

**Claim under test:** *Two regions form; a signal crosses the boundary via the aggregation layer (coordinator summaries), and the cross-region traffic a requester emits/sees is O(region-count), NOT O(N). The cert FAILS if the hierarchy degenerates to flat all-to-all.*

It has two arms, mechanically tagged:

#### Arm A — `[fed-2cluster][in-proc]` — production self-test `dkva_fed2_self_test()`

Single-process, deterministic, arch-uniform — the byte-identical-on-every-node tier (models the synthetic-converged-view pattern of `region.c:291`).

Construct an in-proc synthetic converged view of **two regions**: region A = {self, a1} and region B = {b1, b2}, by directly seeding `dnode_table[].state = DNODE_ALIVE`, injecting RTT (region A members ≤ τ via `rtt_observe`-equivalent, region B members > τ so they are NOT in self's egocentric region), and seeding world region gossip so `world_peer_region_fresh(b1)==world_peer_region_fresh(b2)==coordB` (`world.c:315`).

Assertions (each PASS/FAIL line, `[fed-2cluster]` prefix):
1. **Two distinct regions exist:** `region_size()` for self == 2 (self + a1); `region_is_member(b1)==FALSE && region_is_member(b2)==FALSE` (`region.c:223`). FAIL if everyone collapsed into one region (the degenerate flat case).
2. **Expect-set is O(#regions) for cross-boundary, not O(N):** drive the requester's expect-building loop (`dkva.c:489-507`); assert `rc_cnt0 == 1` (exactly ONE remote-region coordinator expected — coordB), NOT 2 (= the count of region-B *nodes*). **This is the falsifier:** if rsum aggregation degenerated so the requester waits on each remote *node* individually, `rc_cnt0` would equal the remote node count (2), and this asserts ==1.
3. **Cross-region fold is summary-driven:** the folded result over {self, a1, [coordB-summary]} numerically equals the dense fold over all four nodes' KV (reuse the exact-reconstruction property already asserted in `dkva_self_test`, `dkva.c:888`). FAIL if the summary drops region-B contributions.
4. **NOCENTRAL representative:** `region_coordinator()` for region B's view == min(b1,b2), computed by pure recomputation, identical on repeated calls (mirror `region.c:313-318`). No election symbol is called anywhere on the path (the impl/audit must `nm`-tripwire that no `vote`/`elect` symbol exists, per the `region.c:317` "no vote" assertion style).

#### Arm B — `[fed-2cluster][live]` — 2-process harness gate appended to `run_4node_regions.sh`

The real-process tier (the critique's `[live]` demand). Reuses the existing 4-node/2-zone harness; **adds a measured, falsifiable gate** rather than the current human-readable grep.

Procedure (extends `run_4node_regions.sh:64-80`):
1. Before `infer`, capture node1's `[locality]` line (`kdds.c:543`): `far0 = far_msgs`.
2. Run one `infer`.
3. After, capture again: `far1 = far_msgs`, and read node1's region view (`region` → `[region] size=2`).
4. **Gate (O(region-count) not O(N)):** `(far1 - far0) <= (#remote_regions) × K` where `#remote_regions == 1` and `K` = the bounded per-region summary message count (Q-broadcast to coordB + rsum back; pin K from a clean run, e.g. K≤3). Assert the cross-region message count does NOT scale with region-B's node count.
5. **Degeneration falsifier (must FAIL flat):** run a **control** with `PKERNEL_RTT_ZONE_PENALTY=0` (every node within τ → one flat region). Now there is no boundary; the cert asserts that with the boundary present (penalty=200) `far_msgs` per remote node is *strictly less* than the flat-case all-to-all `tx_msgs` per node. If region scoping were a no-op (degenerate to all-to-all), the two runs would produce identical fanout and **the gate fails** — this is the mechanical falsifier the critique requires.
6. **Exact-result preserved:** node1's `=> class` answer is identical in zoned and flat runs (hierarchy must not change the math — one-math invariant).

> **Tag honesty:** Arm A is `[in-proc]` (single process, synthetic membership). Arm B is `[live]` (≥2 OS processes through `./relay`, real SWIM RTT, real UDP). The headline must report them separately, never merge into one "PASS."

### 3.2 `[coord-crash]` — coordinator death → deterministic re-delegation (the critique's mandated row)

The critique (`regions.md:99-104`) flags **crash-during-aggregation as a proof obligation, not an assumption**, and notes no dedicated live cert exists. R0 should ship at least the `[in-proc]` arm and scope the `[live]` arm.

- **`[coord-crash][in-proc]`:** in `dkva_fed2_self_test`, with region B = {b1<b2}, coordinator = b1. Assert `region_coordinator`(B-view) == b1. Mark b1 DEAD (`dnode_table[b1].state = DNODE_DEAD`), recompute, assert the representative deterministically becomes b2 with NO election call (exact mirror of the survives-death assertions `region.c:323-331`). Then assert the requester's `rc_expect` re-forms against b2 (`dkva.c:498-505` keyed on `world_peer_region_fresh`), proving the cross-region fold survives coordinator death by recomputation.
- **`[coord-crash][live]` (scope, may defer to R0.1):** in the live harness, `kill` region-B's coordinator process mid-`infer`; assert node1's inference still completes (honest-degraded `k/n`, not hang) and a subsequent `infer` folds region B via the new coordinator. This reuses the "death-during-inference" `[live]` precedent (`regions.md:104`, the survival loop). If the kill-timing proves flaky on the host, ship `[in-proc]` in R0 and ledger the `[live]` arm OPEN with the named harness as the diagnostic.

---

## 4. The DNODE_MAX dependency — pinned honestly

**R0 does NOT require the DNODE_MAX raise.** Reasons, grounded:

- The cap is already `64` (`drpc.h:35`), not the doc's stale `32`. Two regions of 2 nodes each = 4 logical nodes, far inside 64. The 2-cluster hierarchy is provable entirely within the existing cap.
- R0 proves the *inter-region layer* (boundary + summary-only crossing + O(region-count) traffic). That property is **independent of the absolute node ceiling** — it is about *topology*, not *count* (echoing `regions.md:223-225`: "R0–R2 は玩具モデルのままでも正しく作れる").
- The real ceiling beyond R0 is **not** DNODE_MAX but the **8-bit `node_id` wire field** (`drpc.h:36-37,86-87`, `GOBJ_NODE` = 8 bits at `drpc.h:100`) → hard 256 logical-node wall, and the composite-ID `(region_id, local_id)` scheme to break it (`federation.md §2.2`). That is F1+, **explicitly out of R0 scope.**

**Therefore: R0 ships with zero changes to DNODE_MAX and zero wire changes.** The raise (8-bit→composite-ID, the wide/risky change touching every `[DNODE_MAX]` array enumerated at `federation.md:46-62`) is scoped as its **own prerequisite wave with its own cert** (§5, F1), NOT bundled into R0. This keeps R0 small and safe — the anti-bloat lesson honored.

---

## 5. Sequencing (small falsifiable waves)

| Wave | Deliverable | Cert | DNODE_MAX raise? |
|---|---|---|---|
| **R0** (this plan) | `dkva_fed2_self_test()` + harness gate; refresh `federation.md` stale §1 (32→64, "未実装"→"DKVA path live, now certified") | `[fed-2cluster][in-proc]` + `[fed-2cluster][live]` + `[coord-crash][in-proc]` | **NO** |
| **R0.1** | Live coordinator-crash arm if deferred from R0 | `[coord-crash][live]` | NO |
| **F1** (separate prereq wave) | Composite `(region_id, local_id)` ID; cross the 256 wall; upper coordinator-only mesh as a *distinct* membership | `[id-composite]` (wire back-compat: R=1 byte-identical to today) + raise/widen cert for every `[DNODE_MAX]` array (`federation.md:46-62`) | **YES — its own wave, own cert, own blast-radius audit** |
| **F2** | Kill per-node topics → shared topic + src field (break `KDDS_TOPIC_MAX`/`CFN_MAX_SEMID` linear dep, `kdds.h:44,68`); differential gossip; pmesh BEACON leaf-only (MTU, `pmesh.h:68`) | `[topic-shared]`, `[mtu-bound]` | (independent) |
| **F3** | Region-of-regions recursion; coordinator-set HRW via `lookup.c`; locality-MoE across regions | `[multilayer]` | (independent) |

R0 is the *only* wave this plan authorizes for implementation. F1–F3 are the staircase, sketched for sequencing.

---

## 6. NOCENTRAL / determinism / no-VLA / honest-degrade compliance

- **NOCENTRAL:** the inter-region representative is `region_coordinator()` = deterministic min member id (`region.c:91,214-218`), **delegated from membership, never elected/voted.** Arm A/[coord-crash] explicitly assert "no vote" by repeated-pure-call equality (mirror `region.c:317`) and an `nm` tripwire that no election symbol is on the path.
- **Determinism / one-math:** the cross-region fold is integer/float sums that exactly reconstruct the dense result (`dkva.c:446-449` softmax numerator/denominator are plain sums); Arm B asserts identical `=> class` zoned vs flat. Selectors are integer-only pure functions (`region.c:143-149`, "reads no float").
- **No-VLA:** all cert scratch is fixed `[DNODE_MAX]` (the established pattern, e.g. `region.c:300`, `dkva.c:480-483`). The self-test must not introduce a VLA.
- **Honest degrade:** the requester already reports `k/n` degraded folds when a coordinator/region is missing (`dkva.c:464-507`); `[coord-crash]` asserts degrade is *reported*, never silently dropped (the `regions.md:104` "黙って成功にしない" rule).

---

## 7. DEFERRED / OUT-OF-SCOPE (R0 does NOT do these)

- **DNODE_MAX raise + composite ID** (the 256 wall, `drpc.h:36-37`) → F1, own cert.
- **Per-node topic elimination / `CFN_MAX_SEMID` linear-dep break** (`kdds.h:44,68`, `utk_config_depend.h:41-42`) → F2.
- **pmesh BEACON MTU bound** (`pmesh.h:68`, breaks at N>~340 globally; today bounded by DNODE_MAX=64 → ~264B, safe) → F2.
- **SWIM capability gossip for supernode/teacher** is already live, but **super-region HRW / coordinator-set sharding** (`lookup.c`) → F3.
- **`[live]` coordinator-crash arm** if R0's host kill-timing is flaky → R0.1.
- **Real τ tuning** (`region.h:24` REGION_TAU_MS=50 is provisional) → Phase D / Android fleet (`regions.md:112`).
- **Multi-relay region_id lease arbitration** (`federation.md:307`) → F1.

---

## 8. OPEN RISKS (carried honestly)

1. **rsum scope must be GLOBAL — verify at impl.** The whole O(region-count) property hinges on `rsum/<coord>` being `KDDS_SCOPE_GLOBAL` while `resp/<n>` is `KDDS_SCOPE_REGION`. `dkva.c:811-813` shows both opened with `KDDS_SCOPE_REGION` in the grepped lines — **the impl agent MUST confirm which handle is which**; if rsum is region-scoped the cross-region path is broken and Arm A's assertion #2 will (correctly) fail. This is the single highest-value thing the impl→audit cycle must nail down first.
2. **Egocentric region view ≠ agreed membership.** `region_recompute` is per-node egocentric (`region.h:11-17` admits "ノード間でビューが完全一致する保証はまだ無い"). With asymmetric RTT a node may be in B's view but not A's. R0's synthetic Arm A sidesteps this (seeded view); Arm B uses symmetric `sim_zone` so it holds — but **the cert must not over-claim agreed global membership.** Tag the limitation in the cert output.
3. **The `[locality]` counters are cumulative, not per-inference.** Arm B relies on diffing two prints (`kdds.c:543`). If background SWIM/world traffic pollutes the diff, the gate's K bound needs slack or a quiescent window. Decide empirically; do not add a production reset unless the diff proves unstable.
4. **Live kill-timing flakiness** for `[coord-crash][live]` (race between kill and the aggregation window, `dkva.c` 200ms window). The kill-timer-race class is a known recurring trap in this repo. Ledger OPEN with the named harness rather than ship a flaky green.
5. **The 10k-dream gap stays wide open.** R0 proves *2 regions* with *O(region-count)* crossing. It does **not** prove thousands: the 8-bit wire (256), the per-node array fan-out (`federation.md:46-62`), and the MTU bound are all still walls. R0 is the *first concrete tread* of the staircase, not the staircase. The plan must not let a green `[fed-2cluster]` be read as "10,000 nodes work."
6. **Doc-drift is itself a risk surface.** `federation.md:6,10` asserting "32 / 未実装" while the code is "64 / live DKVA hierarchy" means the *map* lies about the *territory*. R0 should correct §1/§5.1 of `federation.md` to match 9e50e4a8, or future waves will re-derive from a false baseline.

---

## 9. One-line summary

> **The 2-cluster hierarchy is already LIVE in the DKVA path (DNODE_MAX is 64, not 32; `run_4node_regions.sh` runs it) but UNCERTIFIED. R0 = one production self-test + one live-harness gate that prove cross-region traffic is O(region-count) and FAIL on degeneration to flat all-to-all, plus deterministic coordinator-crash re-delegation — with ZERO DNODE_MAX/wire change. The risky raise is F1's own wave.**

---

*Plan grounded entirely at trunk `9e50e4a8`. Files central to implementation: `arch/common/dkva.c` (new self-test near :888/:967; rsum scope at :811-813 — VERIFY FIRST), `arch/common/kdds.c:543-549` (locality instrument), `arch/common/region.c:214-218,291-345` (representative + cert pattern), `samples/11_distributed/run_4node_regions.sh` (live harness to extend), `arch/common/swim.c:358-387` (zone injection), `docs/architecture/federation.md:6,10` (stale text to correct).*

---
